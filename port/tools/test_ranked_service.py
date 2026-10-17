import concurrent.futures
import json
from pathlib import Path
import sqlite3
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
        pending = self.call('bob', 'queue', build='tu3-v1', region='asia')
        mid = pending['match_id']
        self.call('alice', 'accept', match_id=mid)
        self.call('bob', 'accept', match_id=mid)
        return mid

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

    def test_relay_node_selection_and_reservation_fields(self):
        self.store.relay_nodes = [
            {'id': 'eu-west', 'endpoint': '198.51.100.1:9100'},
            {'id': 'asia-east', 'endpoint': '198.51.100.2:9100'},
        ]
        self.call('alice', 'queue', build='tu3-v1', region='global', client_rtt_map={'eu-west': 25, 'asia-east': 90})
        pending = self.call('bob', 'queue', build='tu3-v1', region='global', client_rtt_map={'eu-west': 70, 'asia-east': 15})
        self.assertEqual(pending['state'], 'pending_acceptance')
        mid = pending['match_id']
        self.call('alice', 'accept', match_id=mid)
        self.call('bob', 'accept', match_id=mid)
        confirmed = self.call('alice', 'status')
        self.assertEqual(confirmed['relay_node_id'], 'eu-west')
        self.assertEqual(confirmed['relay_endpoint'], '198.51.100.1:9100')
        self.assertEqual(len(confirmed['relay_token']), 32)
        self.assertEqual(self.call('bob', 'status')['relay_token'], confirmed['relay_token'])

    def test_invalid_relay_rtt_map_is_rejected(self):
        with self.assertRaises(Rejected):
            self.call('alice', 'queue', build='v1', region='global', client_rtt_map={'x': -1})

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
        self.assertEqual(self.call('bob', 'profile')['disconnect_count'], 1)
        self.assertEqual(self.call('bob', 'profile')['disconnect_rate'], 1.0)
        history = self.store.player_history('bob')['matches'][0]
        self.assertEqual(history['status'], 'disconnected')
        self.assertEqual(history['winner_id'], 'alice')

    def test_conflicting_reports_still_dispute(self):
        mid = self.active()
        self.call('alice', 'report', match_id=mid, winner='alice')
        result = self.call('bob', 'report', match_id=mid, winner='bob')
        self.assertEqual(result['state'], 'disputed')
        self.assertEqual(self.call('alice', 'profile')['rating'], 1000)
        self.assertEqual(self.call('bob', 'profile')['rating'], 1000)

    def test_match_summary_consensus_writes_full_ledger(self):
        mid = self.active()
        report = dict(match_id=mid, p1_chars=['Kazuya', 'Jun'], p2_chars=['Armor King'],
                      p1_score=3, p2_score=1, winner_id='alice')
        self.assertEqual(self.call('alice', 'match_report', **report)['state'], 'active')
        result = self.call('bob', 'match_report', **report)
        self.assertEqual(result['status'], 'completed')
        self.assertEqual((result['p1_score'], result['p2_score']), (3, 1))
        profile = self.call('alice', 'profile')
        self.assertEqual((profile['games'], profile['wins']), (1, 1))
        history = self.store.player_history('alice')['matches'][0]
        self.assertEqual(history['match_id'], mid)
        self.assertEqual(history['p1_chars'], ['Kazuya', 'Jun'])
        self.assertEqual(history['p2_chars'], ['Armor King'])
        self.assertEqual((history['rating_change_p1'], history['rating_change_p2']), (16, -16))
        self.assertIsNotNone(history['started_at'])
        self.assertIsNotNone(history['ended_at'])
        self.assertEqual(self.store.leaderboard()['players'][0]['id'], 'alice')
        self.call('alice', 'match_report', **report)
        self.assertEqual(self.call('alice', 'profile')['games'], 1)

    def test_conflicting_match_summaries_are_disputed_without_rating(self):
        mid = self.active()
        self.call('alice', 'match_report', match_id=mid, p1_chars=['Kazuya'], p2_chars=['Jin'],
                  p1_score=3, p2_score=1, winner_id='alice')
        result = self.call('bob', 'match_report', match_id=mid, p1_chars=['Kazuya'], p2_chars=['Jin'],
                           p1_score=2, p2_score=1, winner_id='alice')
        self.assertEqual(result['status'], 'disputed')
        self.assertIsNone(result['winner_id'])
        self.assertEqual((result['rating_change_p1'], result['rating_change_p2']), (0, 0))
        self.assertEqual(self.call('alice', 'profile')['rating'], 1000)
        self.assertEqual(self.call('bob', 'profile')['rating'], 1000)

    def test_match_report_rejects_inconsistent_scores(self):
        mid = self.active()
        with self.assertRaises(Rejected):
            self.call('alice', 'match_report', match_id=mid, p1_chars=['Kazuya'], p2_chars=['Jin'],
                      p1_score=1, p2_score=2, winner_id='alice')

    def test_existing_database_migrates_match_and_disconnect_columns(self):
        path = Path(self.temp.name) / 'legacy.db'
        db = sqlite3.connect(path)
        try:
            db.executescript("""
                CREATE TABLE players(id TEXT PRIMARY KEY,token_hash TEXT UNIQUE NOT NULL,
                                     rating INTEGER NOT NULL DEFAULT 1000,games INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE queue(player TEXT PRIMARY KEY,build TEXT NOT NULL,region TEXT NOT NULL,
                                   expires REAL NOT NULL,joined REAL NOT NULL);
                CREATE TABLE matches(id TEXT PRIMARY KEY,a TEXT NOT NULL,b TEXT NOT NULL,
                    build TEXT NOT NULL,region TEXT NOT NULL,expires REAL NOT NULL,state TEXT NOT NULL,
                    ready_a INTEGER DEFAULT 0,ready_b INTEGER DEFAULT 0,report_a TEXT,report_b TEXT);
                INSERT INTO players(id,token_hash) VALUES ('alice','a'),('bob','b');
                INSERT INTO matches(id,a,b,build,region,expires,state)
                    VALUES ('m1','alice','bob','v1','asia',2000,'active');
            """)
        finally:
            db.close()
        migrated = RankedStore(path, lambda: self.now)
        db = sqlite3.connect(path)
        try:
            player_columns = {row[1] for row in db.execute('PRAGMA table_info(players)')}
            match_columns = {row[1] for row in db.execute('PRAGMA table_info(matches)')}
            row = db.execute('SELECT match_id,p1_id,p2_id,status FROM matches WHERE id=?', ('m1',)).fetchone()
        finally:
            db.close()
        self.assertTrue({'disconnect_count', 'disconnect_rate', 'dan'} <= player_columns)
        self.assertTrue({'p1_chars', 'p2_chars', 'p1_score', 'p2_score', 'winner_id',
                         'rating_change_p1', 'rating_change_p2', 'started_at', 'ended_at'} <= match_columns)
        self.assertEqual(row, ('m1', 'alice', 'bob', 'active'))

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
        pending = self.call('bob', 'queue', build='v1', region='asia')
        mid = pending['match_id']
        self.assertNotIn('match_token', pending)
        self.call('alice', 'accept', match_id=mid)
        self.call('bob', 'accept', match_id=mid)
        alice = self.call('alice', 'status')
        guest = self.call('bob', 'status')
        self.assertEqual(alice['match_token'], guest['match_token'])
        self.assertGreaterEqual(len(alice['match_token']), 32)

    def test_reservation_exchanges_endpoints_roles_and_session_key(self):
        self.call('alice', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28100')
        pending = self.call('bob', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28101')
        mid = pending['match_id']
        self.assertNotIn('opponent_endpoint', pending)
        self.assertNotIn('session_key_bytes', pending)
        self.call('alice', 'accept', match_id=mid)
        self.call('bob', 'accept', match_id=mid)
        guest = self.call('bob', 'status')
        host = self.call('alice', 'status')
        self.assertEqual(host['role'], 'host')
        self.assertEqual(guest['role'], 'guest')
        self.assertEqual(host['opponent_endpoint'], '127.0.0.1:28101')
        self.assertEqual(guest['opponent_endpoint'], '127.0.0.1:28100')
        self.assertEqual(host['id'], guest['id'])
        self.assertEqual(host['match_token'], guest['match_token'])
        self.assertEqual(host['session_key_bytes'], guest['session_key_bytes'])
        self.assertEqual(len(bytes.fromhex(host['session_key_bytes'])), 16)

    def test_acceptance_hides_credentials_until_both_accept(self):
        self.call('alice', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28100')
        pending = self.call('bob', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28101')
        mid = pending['match_id']
        for player in ('alice', 'bob'):
            status = self.call(player, 'status')
            self.assertEqual(status['state'], 'pending_acceptance')
            self.assertGreater(status['time_remaining'], 0)
            self.assertIn('rating', status['opponent'])
            for secret in ('opponent_endpoint', 'session_key', 'session_key_bytes', 'match_token', 'role'):
                self.assertNotIn(secret, status)
        self.assertEqual(self.call('alice', 'accept', match_id=mid)['status'], 'waiting_for_peer')
        self.assertEqual(self.call('alice', 'status')['state'], 'pending_acceptance')
        accepted = self.call('bob', 'accept', match_id=mid)
        self.assertEqual(accepted['status'], 'confirmed')
        host, guest = self.call('alice', 'status'), self.call('bob', 'status')
        self.assertEqual(host['state'], 'confirmed')
        self.assertEqual(host['opponent_endpoint'], '127.0.0.1:28101')
        self.assertEqual(guest['opponent_endpoint'], '127.0.0.1:28100')
        self.assertEqual(host['session_key'], guest['session_key'])

    def test_decline_requeues_other_player_and_blocks_decliner(self):
        self.call('alice', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28100')
        pending = self.call('bob', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:28101')
        mid = pending['match_id']
        self.assertEqual(self.call('alice', 'decline', match_id=mid)['status'], 'declined')
        status = self.call('bob', 'status')
        self.assertEqual(status['state'], 'queued')
        self.assertEqual(status['expires'], self.now + 30)
        with self.assertRaises(Rejected) as error:
            self.call('alice', 'queue', build='v1', region='asia')
        self.assertEqual(error.exception.status, 429)
        self.now += 30
        self.assertEqual(self.call('alice', 'status')['state'], 'idle')

    def test_acceptance_timeout_expires_and_requeues_only_accepting_peer(self):
        self.call('alice', 'queue', build='v1', region='asia')
        pending = self.call('bob', 'queue', build='v1', region='asia')
        mid = pending['match_id']
        self.call('alice', 'accept', match_id=mid)
        self.now += 12.1
        self.assertEqual(self.call('alice', 'status')['state'], 'queued')
        self.assertEqual(self.call('bob', 'status')['state'], 'cooldown')
        expired = self.call('bob', 'accept', match_id=mid)
        self.assertEqual(expired['state'], 'expired')

    def test_acceptance_timeout_with_no_accepts_cools_down_both_players(self):
        self.call('alice', 'queue', build='v1', region='asia')
        pending = self.call('bob', 'queue', build='v1', region='asia')
        self.now += 12.1
        self.assertEqual(self.call('alice', 'status')['state'], 'cooldown')
        self.assertEqual(self.call('bob', 'status')['state'], 'cooldown')
        for player in ('alice', 'bob'):
            with self.assertRaises(Rejected) as error:
                self.call(player, 'queue', build='v1', region='asia')
            self.assertEqual(error.exception.status, 429)

    def test_decline_requires_participant_and_pending_state(self):
        self.call('alice', 'queue', build='v1', region='asia')
        mid = self.call('bob', 'queue', build='v1', region='asia')['match_id']
        with self.assertRaises(Rejected) as error:
            self.call('charlie', 'decline', match_id=mid)
        self.assertEqual(error.exception.status, 404)
        self.call('alice', 'accept', match_id=mid)
        self.call('bob', 'accept', match_id=mid)
        with self.assertRaises(Rejected) as error:
            self.call('alice', 'decline', match_id=mid)
        self.assertEqual(error.exception.status, 409)

    def test_invalid_listen_endpoint_rejected(self):
        for endpoint in ('localhost:28100', '127.0.0.1:0', '127.0.0.1:65536', '224.0.0.1:9000', '[::1]:9000'):
            with self.subTest(endpoint=endpoint), self.assertRaises(Rejected):
                self.call('alice', 'queue', build='v1', region='asia', listen_endpoint=endpoint)

    def test_concurrent_queue_reserves_once(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda p: self.call(p, 'queue', build='v1', region='asia'), self.tokens))
        states = [self.call(p, 'status')['state'] for p in self.tokens]
        self.assertEqual(states.count('pending_acceptance'), 2)
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
            match = request('bob', 'queue', q)
            mid = match['match_id']
            request('alice', 'matches/' + mid + '/accept', b'{}')
            request('bob', 'matches/' + mid + '/accept', b'{}')
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

    def test_health_endpoint(self):
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{server.server_port}/healthz', timeout=3) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(json.load(response), {'status': 'ok'})
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_public_leaderboard_and_player_history_routes(self):
        mid = self.active()
        for player in ('alice', 'bob'):
            self.call(player, 'report', match_id=mid, winner='alice')
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{server.server_port}/v1/leaderboard?limit=2') as response:
                leaderboard = json.load(response)
            self.assertEqual(leaderboard['players'][0]['id'], 'alice')
            with urllib.request.urlopen(f'http://127.0.0.1:{server.server_port}/v1/players/alice/history') as response:
                history = json.load(response)
            self.assertEqual(history['matches'][0]['match_id'], mid)
            self.assertEqual(history['matches'][0]['p1_score'], 1)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_http_match_report_route(self):
        mid = self.active()
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            def report(player):
                body = json.dumps(dict(p1_chars=['Kazuya'], p2_chars=['Jin'], p1_score=3,
                                       p2_score=2, winner_id='alice')).encode()
                request = urllib.request.Request(
                    f'http://127.0.0.1:{server.server_port}/v1/matches/{mid}/report', data=body,
                    headers={'Authorization': 'Bearer ' + self.tokens[player], 'Content-Type': 'application/json'})
                with urllib.request.urlopen(request, timeout=3) as response:
                    return json.load(response)
            self.assertEqual(report('alice')['state'], 'active')
            self.assertEqual(report('bob')['status'], 'completed')
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_http_acceptance_routes_and_get_status(self):
        self.call('alice', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:29001')
        pending = self.call('bob', 'queue', build='v1', region='asia', listen_endpoint='127.0.0.1:29002')
        mid = pending['match_id']
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            def request(player, method, route, payload=None):
                headers = {'Authorization': 'Bearer ' + self.tokens[player]}
                data = None
                if payload is not None:
                    data = json.dumps(payload).encode()
                    headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(f'http://127.0.0.1:{server.server_port}{route}',
                                             data=data, headers=headers, method=method)
                with urllib.request.urlopen(req, timeout=3) as response:
                    return json.load(response)
            before = request('alice', 'GET', '/v1/status')
            self.assertEqual(before['state'], 'pending_acceptance')
            for secret in ('opponent_endpoint', 'session_key', 'session_key_bytes', 'match_token'):
                self.assertNotIn(secret, before)
            self.assertEqual(request('alice', 'POST', f'/v1/matches/{mid}/accept', {})['status'], 'waiting_for_peer')
            self.assertEqual(request('bob', 'POST', f'/v1/matches/{mid}/accept', {})['status'], 'confirmed')
            after = request('alice', 'GET', '/v1/status')
            self.assertEqual(after['state'], 'confirmed')
            self.assertEqual(after['opponent_endpoint'], '127.0.0.1:29002')
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_http_accept_after_deadline_returns_408_and_expires_match(self):
        self.call('alice', 'queue', build='v1', region='asia')
        pending = self.call('bob', 'queue', build='v1', region='asia')
        mid = pending['match_id']
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            self.now += 12.1
            request = urllib.request.Request(
                f'http://127.0.0.1:{server.server_port}/v1/matches/{mid}/accept', data=b'{}',
                headers={'Authorization': 'Bearer ' + self.tokens['alice'], 'Content-Type': 'application/json'})
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(request, timeout=3)
            self.assertEqual(error.exception.code, 408)
            self.assertEqual(json.load(error.exception)['state'], 'expired')
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_http_self_registration_and_token_authentication(self):
        server = make_server(self.store, 0)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            url = f'http://127.0.0.1:{server.server_port}/v1/register'
            request = urllib.request.Request(url, data=json.dumps({'player': 'new_player'}).encode(),
                                             headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(request, timeout=3) as response:
                self.assertEqual(response.status, 201)
                account = json.load(response)
            self.assertEqual(account['id'], 'new_player')
            self.assertGreaterEqual(len(account['token']), 32)
            profile = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/v1/profile',
                data=b'{}', headers={'Authorization': 'Bearer ' + account['token']})
            with urllib.request.urlopen(profile, timeout=3) as response:
                self.assertEqual(json.load(response)['rating'], 1000)
            duplicate = urllib.request.Request(url, data=json.dumps({'player': 'new_player'}).encode(),
                                               headers={'Content-Type': 'application/json'})
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(duplicate, timeout=3)
            self.assertEqual(error.exception.code, 409)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_http_self_registration_can_be_disabled(self):
        server = make_server(self.store, 0, allow_registration=False)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            request = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/v1/register',
                data=b'{"player":"blocked"}', headers={'Content-Type': 'application/json'})
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(request, timeout=3)
            self.assertEqual(error.exception.code, 403)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == '__main__':
    unittest.main()
