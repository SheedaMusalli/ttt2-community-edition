import concurrent.futures
import json
from pathlib import Path
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

from ranked_service import RankedStore, Rejected, make_server


class RankedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.now = 1000
        self.store = RankedStore(Path(self.temp.name) / 'test.db', lambda: self.now)
        self.tokens = {p: self.store.provision(p) for p in ('alice', 'bob', 'charlie')}

    def call(self, p, op, **data):
        return self.store.dispatch(self.tokens[p], op, data)

    def pair(self):
        self.call('alice', 'queue', build='tu3-v1', region='asia')
        return self.call('bob', 'queue', build='tu3-v1', region='asia')['id']

    def active(self):
        mid = self.pair()
        self.call('alice', 'ready', match_id=mid)
        self.assertEqual(self.call('bob', 'ready', match_id=mid)['state'], 'active')
        return mid

    def test_consensus_persistence_and_idempotency(self):
        mid = self.active()
        self.call('alice', 'report', match_id=mid, winner='alice')
        self.assertEqual(self.call('alice', 'profile')['games'], 0)
        self.assertEqual(self.call('bob', 'report', match_id=mid, winner='alice')['state'], 'completed')
        for _ in range(3):
            self.call('bob', 'report', match_id=mid, winner='alice')
        self.store = RankedStore(self.store.path, lambda: self.now)
        self.assertEqual(self.call('alice', 'profile')['rating'], 1016)
        self.assertEqual(self.call('bob', 'profile')['rating'], 984)
        self.assertEqual(self.call('alice', 'profile')['games'], 1)

    def test_disputed_results_no_rating(self):
        mid = self.active()
        self.call('alice', 'report', match_id=mid, winner='alice')
        self.assertEqual(self.call('bob', 'report', match_id=mid, winner='bob')['state'], 'disputed')
        self.assertEqual(self.call('alice', 'profile')['games'], 0)
        with self.assertRaises(Rejected):
            self.call('bob', 'report', match_id=mid, winner='alice')

    def test_auth_and_participant_isolation(self):
        mid = self.pair()
        with self.assertRaises(Rejected) as error:
            self.store.dispatch('bad-token', 'profile', {})
        self.assertEqual(error.exception.status, 401)
        for op in ('match', 'ready', 'report', 'abort'):
            with self.assertRaises(Rejected):
                self.call('charlie', op, match_id=mid, winner='charlie')
        with self.assertRaises(Rejected):
            self.call('alice', 'report', match_id=mid, winner='alice')

    def test_build_region_cancel_and_queue_expiry(self):
        self.call('alice', 'queue', build='v1', region='asia')
        self.assertEqual(self.call('bob', 'queue', build='v2', region='asia')['state'], 'queued')
        self.assertEqual(self.call('charlie', 'queue', build='v1', region='eu')['state'], 'queued')
        self.call('alice', 'cancel')
        self.assertEqual(self.call('alice', 'status')['state'], 'idle')
        self.now += 31
        self.assertEqual(self.call('bob', 'status')['state'], 'idle')

    def test_reservation_and_active_expiry(self):
        mid = self.pair()
        self.now += 31
        self.assertEqual(self.call('alice', 'match', match_id=mid)['state'], 'expired')
        mid = self.active()
        self.now += 901
        self.assertEqual(self.call('bob', 'match', match_id=mid)['state'], 'expired')
        self.assertEqual(self.call('alice', 'profile')['games'], 0)

    def test_abort_and_draw(self):
        mid = self.active()
        self.call('alice', 'abort', match_id=mid)
        with self.assertRaises(Rejected):
            self.call('bob', 'report', match_id=mid, winner='bob')
        mid = self.active()
        for p in ('alice', 'bob'):
            self.call(p, 'report', match_id=mid, winner='draw')
        self.assertEqual(self.call('alice', 'profile')['rating'], 1000)
        self.assertEqual(self.call('alice', 'profile')['games'], 1)

    def test_unilateral_disconnect_forfeit(self):
        mid = self.active()
        self.now += 76
        claim = self.call('alice', 'forfeit', match_id=mid)
        self.assertEqual(claim['state'], 'active')
        self.now += 31
        result = self.call('alice', 'match', match_id=mid)
        self.assertEqual(result['state'], 'forfeit')
        self.assertEqual(self.call('alice', 'profile')['rating'], 1016)
        self.assertEqual(self.call('bob', 'profile')['rating'], 984)

    def test_conflicting_reports_still_dispute(self):
        mid = self.active()
        self.call('alice', 'report', match_id=mid, winner='alice')
        result = self.call('bob', 'report', match_id=mid, winner='bob')
        self.assertEqual(result['state'], 'disputed')
        self.assertEqual(self.call('alice', 'profile')['rating'], 1000)
        self.assertEqual(self.call('bob', 'profile')['rating'], 1000)

    def test_active_match_abandonment_expiry(self):
        mid = self.active()
        self.now += 106
        result = self.call('bob', 'match', match_id=mid)
        self.assertEqual(result['state'], 'expired')
        self.assertEqual(self.call('alice', 'profile')['games'], 0)

    def test_heartbeat_cancels_stale_forfeit_claim(self):
        mid = self.active()
        self.now += 76
        self.call('alice', 'forfeit', match_id=mid)
        self.now += 1
        self.call('bob', 'heartbeat', match_id=mid)
        self.call('alice', 'heartbeat', match_id=mid)
        self.now += 31
        result = self.call('alice', 'match', match_id=mid)
        self.assertEqual(result['state'], 'active')

    def test_match_reservation_token_shared_only_with_participants(self):
        self.call('alice', 'queue', build='v1', region='asia')
        reserved = self.call('bob', 'queue', build='v1', region='asia')
        alice = self.call('alice', 'status')
        self.assertEqual(alice['id'], reserved['id'])
        self.assertEqual(alice['match_token'], reserved['match_token'])
        self.assertGreaterEqual(len(alice['match_token']), 32)

    def test_concurrent_queue_reserves_once(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda p: self.call(p, 'queue', build='v1', region='asia'), self.tokens))
        states = [self.call(p, 'status')['state'] for p in self.tokens]
        self.assertEqual(states.count('reserved'), 2)
        self.assertEqual(states.count('queued'), 1)
        self.assertEqual(len({r['id'] for r in results if 'id' in r}), 1)

    def test_http_two_clients_and_bad_requests(self):
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            def request(p, op, payload):
                req = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/v1/{op}',
                    data=payload, headers={'Authorization': 'Bearer ' + self.tokens[p]})
                with urllib.request.urlopen(req, timeout=3) as response:
                    return json.load(response)
            q = json.dumps(dict(build='v1', region='asia')).encode()
            self.assertEqual(request('alice', 'queue', q)['state'], 'queued')
            mid = request('bob', 'queue', q)['id']
            for p in ('alice', 'bob'):
                request(p, 'ready', json.dumps(dict(match_id=mid)).encode())
            for p in ('alice', 'bob'):
                result = request(p, 'report', json.dumps(dict(match_id=mid, winner='bob')).encode())
            self.assertEqual(result['state'], 'completed')
            for payload, code in ((b'[]', 400), (b'{', 400), (b' ' * 4097, 413)):
                with self.assertRaises(urllib.error.HTTPError) as error:
                    request('alice', 'queue', payload)
                self.assertEqual(error.exception.code, code)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
