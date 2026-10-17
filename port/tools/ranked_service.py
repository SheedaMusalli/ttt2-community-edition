"""Standalone provisional ranked matchmaking service.

Python standard library only. Ratings are provisional, based on matching peer reports.
For Internet deployment, place behind a TLS reverse proxy (see deploy/).
"""
import argparse
import hashlib
import ipaddress
import json
import re
import secrets
import sqlite3
import time
import hmac
import threading
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, urlopen
import os

HEARTBEAT_INTERVAL = 25
HEARTBEAT_TIMEOUT = 75
FORFEIT_GRACE = 30
INACTIVITY_FORFEIT_TIMEOUT = HEARTBEAT_TIMEOUT + FORFEIT_GRACE
ACTIVE_MATCH_TIMEOUT = 900
ACCEPTANCE_WINDOW = 12.0
DECLINE_COOLDOWN = 30.0


class Rejected(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def label(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
        raise Rejected(400, "expected a 1-64 character identifier")
    return value


def listen_endpoint(value):
    """Validate a client-advertised IPv4 UDP endpoint; this does not prove reachability."""
    if value is None:
        return None
    if not isinstance(value, str) or len(value) > 64 or value.count(':') != 1:
        raise Rejected(400, 'listen_endpoint must be an IPv4 address and UDP port')
    address, raw_port = value.rsplit(':', 1)
    if not re.fullmatch(r'[0-9]{1,5}', raw_port):
        raise Rejected(400, 'listen_endpoint must be an IPv4 address and UDP port')
    try:
        parsed = ipaddress.ip_address(address)
        port = int(raw_port, 10)
    except (ValueError, TypeError):
        raise Rejected(400, 'listen_endpoint must be an IPv4 address and UDP port')
    if parsed.version != 4 or parsed.is_unspecified or parsed.is_multicast or parsed.is_reserved or not 1 <= port <= 65535:
        raise Rejected(400, 'listen_endpoint must be a usable IPv4 UDP endpoint')
    return f'{parsed.compressed}:{port}'


class RankedStore:
    def __init__(self, path, clock=time.time, relay_nodes=None, relay_register_url=None, relay_admin_token=None):
        self.path, self.clock = str(path), clock
        self.relay_nodes = relay_nodes if relay_nodes is not None else self._load_relay_nodes()
        self.relay_register_url = relay_register_url if relay_register_url is not None else os.environ.get('TTT2_RELAY_REGISTER_URL', '')
        self.relay_admin_token = relay_admin_token if relay_admin_token is not None else os.environ.get('TTT2_RELAY_ADMIN_TOKEN', '')
        self._relay_lock = threading.Lock()
        self._registered_relays = set()
        with closing(self.connect()) as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS players(
                    id TEXT PRIMARY KEY, token_hash TEXT UNIQUE NOT NULL,
                    rating INTEGER NOT NULL DEFAULT 1000, games INTEGER NOT NULL DEFAULT 0,
                    disconnect_count INTEGER NOT NULL DEFAULT 0, disconnect_rate REAL NOT NULL DEFAULT 0,
                    dan INTEGER NOT NULL DEFAULT 1, wins INTEGER NOT NULL DEFAULT 0,
                    losses INTEGER NOT NULL DEFAULT 0, queue_blocked_until REAL NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS queue(
                    player TEXT PRIMARY KEY REFERENCES players(id), build TEXT NOT NULL,
                    region TEXT NOT NULL, expires REAL NOT NULL, joined REAL NOT NULL,
                    listen_endpoint TEXT, client_rtt_map TEXT);
                CREATE TABLE IF NOT EXISTS matches(
                    id TEXT PRIMARY KEY, a TEXT NOT NULL, b TEXT NOT NULL,
                    build TEXT NOT NULL, region TEXT NOT NULL, expires REAL NOT NULL,
                    state TEXT NOT NULL, ready_a INTEGER DEFAULT 0, ready_b INTEGER DEFAULT 0,
                    report_a TEXT, report_b TEXT, connect_token TEXT,
                    heartbeat_a REAL, heartbeat_b REAL, forfeit_claim TEXT, forfeit_at REAL,
                    accept_deadline REAL, p1_accepted INTEGER NOT NULL DEFAULT 0,
                    p2_accepted INTEGER NOT NULL DEFAULT 0, queued_at_a REAL, queued_at_b REAL,
                    relay_node_id TEXT, relay_endpoint TEXT, relay_token TEXT);
            """)
            # Upgrade databases created by earlier prototype builds.
            player_columns = {r[1] for r in db.execute("PRAGMA table_info(players)")}
            for name, kind in (("disconnect_count", "INTEGER NOT NULL DEFAULT 0"),
                               ("disconnect_rate", "REAL NOT NULL DEFAULT 0"),
                               ("dan", "INTEGER NOT NULL DEFAULT 1"),
                               ("wins", "INTEGER NOT NULL DEFAULT 0"),
                               ("losses", "INTEGER NOT NULL DEFAULT 0"),
                               ("queue_blocked_until", "REAL NOT NULL DEFAULT 0")):
                if name not in player_columns:
                    db.execute(f"ALTER TABLE players ADD COLUMN {name} {kind}")
            columns = {r[1] for r in db.execute("PRAGMA table_info(matches)")}
            for name, kind in (("connect_token", "TEXT"), ("heartbeat_a", "REAL"), ("heartbeat_b", "REAL"),
                               ("forfeit_claim", "TEXT"), ("forfeit_at", "REAL"),
                               ("accept_deadline", "REAL"),
                               ("p1_accepted", "INTEGER NOT NULL DEFAULT 0"),
                               ("p2_accepted", "INTEGER NOT NULL DEFAULT 0"),
                               ("queued_at_a", "REAL"), ("queued_at_b", "REAL")):
                if name not in columns:
                    db.execute(f"ALTER TABLE matches ADD COLUMN {name} {kind}")
            queue_columns = {r[1] for r in db.execute("PRAGMA table_info(queue)")}
            if 'listen_endpoint' not in queue_columns:
                db.execute("ALTER TABLE queue ADD COLUMN listen_endpoint TEXT")
            if 'client_rtt_map' not in queue_columns:
                db.execute("ALTER TABLE queue ADD COLUMN client_rtt_map TEXT")
            match_columns = {r[1] for r in db.execute("PRAGMA table_info(matches)")}
            for name, kind in (
                    ('endpoint_a', 'TEXT'), ('endpoint_b', 'TEXT'), ('session_key', 'TEXT'),
                    ('match_id', 'TEXT'), ('p1_id', 'TEXT'), ('p2_id', 'TEXT'),
                    ('p1_chars', 'TEXT'), ('p2_chars', 'TEXT'),
                    ('p1_score', 'INTEGER'), ('p2_score', 'INTEGER'), ('winner_id', 'TEXT'),
                    ('rating_change_p1', 'INTEGER NOT NULL DEFAULT 0'),
                    ('rating_change_p2', 'INTEGER NOT NULL DEFAULT 0'), ('status', 'TEXT'),
                    ('started_at', 'REAL'), ('ended_at', 'REAL'), ('disconnected_player', 'TEXT'),
                    ('relay_node_id', 'TEXT'), ('relay_endpoint', 'TEXT'), ('relay_token', 'TEXT')):
                if name not in match_columns:
                    db.execute(f"ALTER TABLE matches ADD COLUMN {name} {kind}")
            db.execute("UPDATE matches SET match_id=id WHERE match_id IS NULL")
            db.execute("UPDATE matches SET p1_id=a WHERE p1_id IS NULL")
            db.execute("UPDATE matches SET p2_id=b WHERE p2_id IS NULL")
            db.execute("UPDATE matches SET status=state WHERE status IS NULL")
            db.execute("CREATE INDEX IF NOT EXISTS matches_p1_history ON matches(p1_id,ended_at DESC)")
            db.execute("CREATE INDEX IF NOT EXISTS matches_p2_history ON matches(p2_id,ended_at DESC)")
            db.commit()

    @staticmethod
    def _load_relay_nodes():
        raw = os.environ.get('TTT2_RELAY_NODES', '')
        if not raw:
            endpoint = os.environ.get('TTT2_RELAY_ENDPOINT', '')
            register_url = os.environ.get('TTT2_RELAY_REGISTER_URL', '')
            return [{'id': 'default', 'endpoint': endpoint, 'register_url': register_url}] if endpoint else []
        try:
            nodes = json.loads(raw)
            if not isinstance(nodes, list):
                return []
            valid = []
            for node in nodes:
                if not isinstance(node, dict) or not isinstance(node.get('id'), str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', node['id']):
                    continue
                if not isinstance(node.get('endpoint'), str) or not re.fullmatch(r'[A-Za-z0-9_.-]+:[0-9]{1,5}', node['endpoint']):
                    continue
                valid.append({'id': node['id'], 'endpoint': node['endpoint'],
                              'register_url': node.get('register_url', '') if isinstance(node.get('register_url', ''), str) else ''})
            return valid
        except (ValueError, TypeError):
            return []

    def _choose_relay(self, first, second):
        if not self.relay_nodes or not first or not second:
            return None
        try:
            a, b = json.loads(first), json.loads(second)
        except (ValueError, TypeError):
            return None
        choices = []
        for node in self.relay_nodes:
            ra, rb = a.get(node['id']), b.get(node['id'])
            if (isinstance(ra, (int, float)) and not isinstance(ra, bool) and 0 <= ra <= 2000 and
                    isinstance(rb, (int, float)) and not isinstance(rb, bool) and 0 <= rb <= 2000):
                choices.append((max(ra, rb), ra + rb, node['id'], node['endpoint']))
        if not choices:
            return None
        _, _, node_id, endpoint = min(choices)
        return node_id, endpoint

    def provision_relay(self, result):
        node = next((item for item in self.relay_nodes if item['id'] == result.get('relay_node_id')), {})
        register_url = node.get('register_url') or self.relay_register_url
        if not result.get('relay_endpoint') or not result.get('relay_token') or not register_url or not self.relay_admin_token:
            return result
        match_id = result.get('match_id')
        with self._relay_lock:
            if match_id in self._registered_relays:
                return result
            payload = json.dumps({'match_id': match_id, 'relay_token': result['relay_token'],
                                  'p1_id': result['player_a'], 'p2_id': result['player_b'], 'ttl': 1200}).encode()
            request = Request(register_url.rstrip('/') + '/v1/relay/sessions', data=payload, method='POST',
                              headers={'Authorization': 'Bearer ' + self.relay_admin_token,
                                       'Content-Type': 'application/json'})
            try:
                with urlopen(request, timeout=2) as response:
                    if response.status == 200:
                        self._registered_relays.add(match_id)
                        return result
            except (OSError, ValueError):
                pass
        result.pop('relay_endpoint', None)
        result.pop('relay_token', None)
        return result

    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        return db

    @contextmanager
    def transaction(self):
        db = self.connect()
        try:
            db.execute("BEGIN IMMEDIATE")
            yield db
            db.commit()
        except Exception:
            db.rollback()
            raise
        finally:
            db.close()

    def provision(self, player):
        label(player)
        if player == 'draw':
            raise Rejected(400, 'draw is reserved for result reporting')
        token = secrets.token_urlsafe(32)
        try:
            with self.transaction() as db:
                db.execute("INSERT INTO players(id,token_hash) VALUES (?,?)",
                           (player, hashlib.sha256(token.encode()).hexdigest()))
        except sqlite3.IntegrityError:
            raise Rejected(409, 'player identifier is already registered')
        return token

    def expire(self, db):
        now = self.clock()
        db.execute("DELETE FROM queue WHERE expires<=?", (now,))
        pending = db.execute("SELECT * FROM matches WHERE state='pending_acceptance' AND accept_deadline<=?", (now,)).fetchall()
        for match in pending:
            db.execute("UPDATE matches SET state='expired',status='expired',ended_at=? WHERE id=?", (now, match['id']))
            accepted_a, accepted_b = bool(match['p1_accepted']), bool(match['p2_accepted'])
            if accepted_a != accepted_b:
                accepted_side = 'a' if accepted_a else 'b'
                idle_side = 'b' if accepted_a else 'a'
                self._restore_queue(db, match, accepted_side, now)
                db.execute("UPDATE players SET queue_blocked_until=? WHERE id=?",
                           (now + DECLINE_COOLDOWN, match[idle_side]))
            elif not accepted_a and not accepted_b:
                db.execute("UPDATE players SET queue_blocked_until=? WHERE id IN (?,?)",
                           (now + DECLINE_COOLDOWN, match['a'], match['b']))
        db.execute("UPDATE matches SET state='expired',status='expired',ended_at=? WHERE expires<=? AND state IN ('confirmed','reserved','active')", (now, now))
        # A forfeit claim is only accepted after the accused player's heartbeat is stale.
        claims = db.execute("SELECT * FROM matches WHERE state='active' AND forfeit_claim IS NOT NULL AND forfeit_at<=?", (now - FORFEIT_GRACE,)).fetchall()
        for match in claims:
            side = match['forfeit_claim']
            accused_side = 'b' if side == 'a' else 'a'
            last = match['heartbeat_' + accused_side]
            if last is None or last <= match['forfeit_at']:
                self.finish(db, match, match[side], 'forfeit', now)
            else:
                db.execute("UPDATE matches SET forfeit_claim=NULL,forfeit_at=NULL WHERE id=?", (match['id'],))
        # Heartbeat expiry resolves an unresponsive active match instead of parking it for 15 minutes.
        active = db.execute("SELECT * FROM matches WHERE state='active'", ()).fetchall()
        for match in active:
            a, b = match['heartbeat_a'], match['heartbeat_b']
            if a is not None and b is not None and now - a > INACTIVITY_FORFEIT_TIMEOUT and now - b <= HEARTBEAT_TIMEOUT:
                self.finish(db, match, match['b'], 'forfeit', now)
            elif b is not None and now - b > INACTIVITY_FORFEIT_TIMEOUT and now - a <= HEARTBEAT_TIMEOUT:
                self.finish(db, match, match['a'], 'forfeit', now)
            elif (a is None or b is None or now - a > INACTIVITY_FORFEIT_TIMEOUT and now - b > INACTIVITY_FORFEIT_TIMEOUT) or now >= match['expires']:
                db.execute("UPDATE matches SET state='expired',status='expired',ended_at=? WHERE id=?", (now, match['id']))

    @staticmethod
    def finish(db, match, winner, state, ended_at=None):
        if match['state'] not in ('reserved', 'active'):
            return
        ra = db.execute("SELECT rating FROM players WHERE id=?", (match['a'],)).fetchone()[0]
        rb = db.execute("SELECT rating FROM players WHERE id=?", (match['b'],)).fetchone()[0]
        score = 0.5 if winner == 'draw' else float(winner == match['a'])
        delta = round(32 * (score - 1 / (1 + 10 ** (max(-4000, min(4000, rb - ra)) / 400))))
        disconnected = (match['b'] if state == 'forfeit' and winner == match['a'] else
                        match['a'] if state == 'forfeit' and winner == match['b'] else None)
        p1_score, p2_score = match['p1_score'], match['p2_score']
        if p1_score is None or p2_score is None:
            p1_score, p2_score = ((0, 0) if winner == 'draw' else
                                  (1, 0) if winner == match['a'] else (0, 1))
        status = 'disconnected' if state == 'forfeit' else state
        ended_at = time.time() if ended_at is None else ended_at
        db.execute("""UPDATE matches SET state=?,status=?,winner_id=?,p1_score=?,p2_score=?,
                   rating_change_p1=?,rating_change_p2=?,ended_at=?,disconnected_player=? WHERE id=?""",
                   (state, status, winner, p1_score, p2_score, delta, -delta, ended_at, disconnected, match['id']))
        for player_id, rating_delta, won, lost in (
                (match['a'], delta, int(winner == match['a']), int(winner == match['b'])),
                (match['b'], -delta, int(winner == match['b']), int(winner == match['a']))):
            db.execute("""UPDATE players SET rating=rating+?,games=games+1,wins=wins+?,losses=losses+?,
                       disconnect_count=disconnect_count+?,
                       disconnect_rate=CAST(disconnect_count+? AS REAL)/(games+1) WHERE id=?""",
                       (rating_delta, won, lost, int(disconnected == player_id), int(disconnected == player_id), player_id))
            rating = db.execute("SELECT rating FROM players WHERE id=?", (player_id,)).fetchone()[0]
            dan = max(1, min(30, 1 + (rating - 1000) // 100))
            db.execute("UPDATE players SET dan=? WHERE id=?", (dan, player_id))

    @staticmethod
    def _characters(value, field):
        if isinstance(value, str):
            value = [value]
        if not isinstance(value, list) or not 1 <= len(value) <= 2:
            raise Rejected(400, f'{field} must contain one or two character identifiers')
        normalized = []
        for character in value:
            if (not isinstance(character, str) or character != character.strip() or
                    not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_. -]{0,63}', character)):
                raise Rejected(400, f'{field} contains an invalid character identifier')
            normalized.append(character)
        return normalized

    @staticmethod
    def _score(value, field):
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 99:
            raise Rejected(400, f'{field} must be an integer from 0 to 99')
        return value

    @staticmethod
    def _decode_report(value, match):
        try:
            report = json.loads(value)
        except (TypeError, json.JSONDecodeError):
            # Rows from the first service version stored only the literal winner ID.
            winner = value
            if winner not in (match['a'], match['b'], 'draw'):
                return None
            p1_score, p2_score = ((0, 0) if winner == 'draw' else
                                  (1, 0) if winner == match['a'] else (0, 1))
            return dict(p1_chars=None, p2_chars=None, p1_score=p1_score,
                        p2_score=p2_score, winner_id=winner)
        return report if isinstance(report, dict) else None

    def _profile(self, db, player):
        row = db.execute("SELECT * FROM players WHERE id=?", (player,)).fetchone()
        return dict(id=player, rating=row['rating'], dan=row['dan'], games=row['games'],
                    wins=row['wins'], losses=row['losses'], disconnect_count=row['disconnect_count'],
                    disconnect_rate=row['disconnect_rate'], provisional=True)

    def leaderboard(self, limit=50):
        try:
            limit = int(limit)
        except (TypeError, ValueError):
            raise Rejected(400, 'limit must be an integer')
        if not 1 <= limit <= 100:
            raise Rejected(400, 'limit must be between 1 and 100')
        with closing(self.connect()) as db:
            rows = db.execute("""SELECT id,rating,dan,games,wins,losses,disconnect_count,disconnect_rate
                               FROM players ORDER BY rating DESC,wins DESC,games ASC,id ASC LIMIT ?""",
                              (limit,)).fetchall()
        return dict(players=[dict(row) for row in rows])

    def player_history(self, player, limit=20):
        label(player)
        try:
            limit = int(limit)
        except (TypeError, ValueError):
            raise Rejected(400, 'limit must be an integer')
        if not 1 <= limit <= 100:
            raise Rejected(400, 'limit must be between 1 and 100')
        with closing(self.connect()) as db:
            if db.execute("SELECT 1 FROM players WHERE id=?", (player,)).fetchone() is None:
                raise Rejected(404, 'player not found')
            rows = db.execute("""SELECT match_id,p1_id,p2_id,p1_chars,p2_chars,
                               p1_score,p2_score,winner_id,rating_change_p1,rating_change_p2,
                               status,started_at,ended_at FROM matches WHERE p1_id=? OR p2_id=?
                               ORDER BY COALESCE(ended_at,started_at,0) DESC LIMIT ?""",
                              (player, player, limit)).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            for field in ('p1_chars', 'p2_chars'):
                item[field] = json.loads(item[field]) if item[field] else None
            result.append(item)
        return dict(player=player, matches=result)

    def _restore_queue(self, db, match, side, now):
        player = match[side]
        endpoint = match['endpoint_a' if side == 'a' else 'endpoint_b']
        joined = match['queued_at_a' if side == 'a' else 'queued_at_b']
        db.execute("""INSERT INTO queue(player,build,region,expires,joined,listen_endpoint)
                   VALUES (?,?,?,?,?,?) ON CONFLICT(player) DO UPDATE SET
                   build=excluded.build,region=excluded.region,expires=excluded.expires,
                   joined=MIN(queue.joined,excluded.joined),listen_endpoint=excluded.listen_endpoint""",
                   (player, match['build'], match['region'], now + 30,
                    now if joined is None else joined, endpoint))

    def match_response(self, match, player, db=None):
        result = {"id": match['id'], "state": match['state'], "build": match['build'],
                  "match_id": match['match_id'] or match['id'], "status": match['status'] or match['state'],
                  "region": match['region'], "expires": match['expires'],
                  "p1_chars": json.loads(match['p1_chars']) if match['p1_chars'] else None,
                  "p2_chars": json.loads(match['p2_chars']) if match['p2_chars'] else None,
                  "p1_score": match['p1_score'], "p2_score": match['p2_score'],
                  "winner_id": match['winner_id'], "rating_change_p1": match['rating_change_p1'],
                  "rating_change_p2": match['rating_change_p2'],
                  "started_at": match['started_at'], "ended_at": match['ended_at']}
        if player in (match['a'], match['b']):
            is_a = player == match['a']
            if match['state'] == 'pending_acceptance':
                opponent = match['b' if is_a else 'a']
                result['time_remaining'] = max(0.0, (match['accept_deadline'] or 0) - self.clock())
                result['opponent'] = dict(player_id=opponent)
                if db is not None:
                    profile = db.execute("SELECT rating,disconnect_rate FROM players WHERE id=?", (opponent,)).fetchone()
                    if profile:
                        result['opponent'].update(rating=profile['rating'],
                                                  disconnect_rate=profile['disconnect_rate'])
            elif match['state'] in ('confirmed', 'reserved', 'active'):
                result.update(player_a=match['a'], player_b=match['b'], match_token=match['connect_token'],
                              role='host' if is_a else 'guest',
                              opponent_endpoint=match['endpoint_b' if is_a else 'endpoint_a'],
                              session_key=match['session_key'],
                              session_key_bytes=match['session_key'])
                if match['relay_endpoint'] and match['relay_token']:
                    result.update(relay_node_id=match['relay_node_id'], relay_endpoint=match['relay_endpoint'],
                                  relay_token=match['relay_token'])
        return result

    def dispatch(self, token, operation, data):
        if not isinstance(data, dict):
            raise Rejected(400, "expected JSON object")
        with self.transaction() as db:
            row = db.execute("SELECT * FROM players WHERE token_hash=?",
                             (hashlib.sha256(token.encode()).hexdigest(),)).fetchone()
            if row is None:
                raise Rejected(401, "invalid bearer token")
            player = row['id']
            self.expire(db)
            now = self.clock()
            if operation == 'profile':
                return self._profile(db, player)
            if operation == 'cancel':
                db.execute("DELETE FROM queue WHERE player=?", (player,))
                return dict(state='cancelled')
            if operation in ('queue', 'status'):
                match = db.execute("SELECT * FROM matches WHERE (a=? OR b=?) AND state IN ('pending_acceptance','confirmed','reserved','active')",
                                   (player, player)).fetchone()
                if match:
                    return self.match_response(match, player, db)
                if operation == 'status':
                    q = db.execute("SELECT expires FROM queue WHERE player=?", (player,)).fetchone()
                    if q: return dict(state='queued', expires=q[0])
                    blocked = db.execute("SELECT queue_blocked_until FROM players WHERE id=?", (player,)).fetchone()[0]
                    return dict(state='cooldown', time_remaining=max(0, blocked-now)) if blocked > now else dict(state='idle')
                blocked = db.execute("SELECT queue_blocked_until FROM players WHERE id=?", (player,)).fetchone()[0]
                if blocked > now:
                    raise Rejected(429, f'queue cooldown active for {blocked-now:.1f} more seconds')
                build, region = label(data.get('build')), label(data.get('region'))
                endpoint = listen_endpoint(data.get('listen_endpoint'))
                # One atomic transaction reserves both players; clients cannot choose ratings.
                rtt_map = data.get('client_rtt_map')
                if rtt_map is not None:
                    if not isinstance(rtt_map, dict) or len(rtt_map) > 32:
                        raise Rejected(400, 'client_rtt_map must be a map of relay IDs to RTT milliseconds')
                    clean_rtt = {}
                    for node_id, rtt in rtt_map.items():
                        label(node_id)
                        if not isinstance(rtt, (int, float)) or isinstance(rtt, bool) or not 0 <= rtt <= 2000:
                            raise Rejected(400, 'relay RTT values must be between 0 and 2000 ms')
                        clean_rtt[node_id] = float(rtt)
                    rtt_map = json.dumps(clean_rtt, separators=(',', ':'))
                else:
                    rtt_map = None
                db.execute("""INSERT INTO queue(player,build,region,expires,joined,listen_endpoint,client_rtt_map)
                           VALUES (?,?,?,?,?,?,?) ON CONFLICT(player) DO UPDATE SET
                           build=excluded.build,region=excluded.region,expires=excluded.expires,
                           listen_endpoint=excluded.listen_endpoint,client_rtt_map=excluded.client_rtt_map""",
                           (player, build, region, now + 30, now, endpoint, rtt_map))
                other = db.execute("""SELECT q.player,q.listen_endpoint,q.client_rtt_map,q.joined FROM queue q JOIN players p ON p.id=q.player
                           WHERE q.player<>? AND q.build=? AND q.region=? AND ABS(p.rating-?)<=200
                           ORDER BY q.joined,q.player LIMIT 1""", (player, build, region, row['rating'])).fetchone()
                if not other:
                    return dict(state='queued', expires=now + 30)
                mid = secrets.token_hex(16)
                token = secrets.token_urlsafe(32)
                session_key = secrets.token_hex(16)
                relay = self._choose_relay(other['client_rtt_map'], rtt_map)
                relay_node_id, relay_endpoint = relay if relay else (None, None)
                relay_token = secrets.token_hex(16) if relay else None
                db.execute("""INSERT INTO matches(id,match_id,a,b,p1_id,p2_id,build,region,expires,state,status,connect_token,
                           endpoint_a,endpoint_b,session_key,accept_deadline,p1_accepted,p2_accepted,queued_at_a,queued_at_b,
                           relay_node_id,relay_endpoint,relay_token)
                           VALUES (?,?,?,?,?,?,?,?,?,'pending_acceptance','pending_acceptance',?,?,?,?,?,0,0,?,?,?,?,?)""",
                           (mid, mid, other[0], player, other[0], player, build, region, now + ACCEPTANCE_WINDOW,
                            token, other['listen_endpoint'], endpoint, session_key, now + ACCEPTANCE_WINDOW,
                            other['joined'], now, relay_node_id, relay_endpoint, relay_token))
                db.execute("DELETE FROM queue WHERE player IN (?,?)", (player, other[0]))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player, db)
            if operation not in ('match', 'ready', 'report', 'match_report', 'abort', 'heartbeat', 'forfeit', 'accept', 'decline'):
                raise Rejected(404, "unknown operation")
            mid = label(data.get('match_id'))
            match = db.execute("SELECT * FROM matches WHERE id=? AND (a=? OR b=?)",
                               (mid, player, player)).fetchone()
            if match is None:
                raise Rejected(404, "match not found")
            side = 'a' if player == match['a'] else 'b'
            if operation == 'accept' and match['state'] == 'expired' and match['status'] == 'expired':
                return dict(state='expired', status='expired', error='acceptance window expired')
            if operation == 'match':
                return self.match_response(match, player, db)
            if operation == 'accept':
                if match['state'] != 'pending_acceptance':
                    raise Rejected(409, 'no match is pending acceptance')
                if now > match['accept_deadline']:
                    self.expire(db)
                    return dict(state='expired', status='expired')
                accepted_col = 'p1_accepted' if side == 'a' else 'p2_accepted'
                db.execute(f"UPDATE matches SET {accepted_col}=1 WHERE id=?", (mid,))
                current = db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone()
                if current['p1_accepted'] and current['p2_accepted']:
                    db.execute("UPDATE matches SET state='confirmed',status='confirmed',expires=? WHERE id=?", (now + 30, mid))
                    return dict(status='confirmed', state='confirmed', message='Match starting')
                return dict(status='waiting_for_peer', state='pending_acceptance')
            if operation == 'decline':
                if match['state'] != 'pending_acceptance':
                    raise Rejected(409, 'no match is pending acceptance')
                db.execute("UPDATE matches SET state='declined',status='declined',ended_at=? WHERE id=?", (now, mid))
                other_side = 'b' if side == 'a' else 'a'
                self._restore_queue(db, match, other_side, now)
                db.execute("UPDATE players SET queue_blocked_until=? WHERE id=?",
                           (now + DECLINE_COOLDOWN, player))
                return dict(status='declined', state='declined', cooldown=DECLINE_COOLDOWN)
            if operation == 'heartbeat':
                if match['state'] != 'active':
                    raise Rejected(409, 'match is not active')
                db.execute(f"UPDATE matches SET heartbeat_{side}=?,expires=?,status='active' WHERE id=?", (now, now + ACTIVE_MATCH_TIMEOUT, mid))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)
            if operation == 'forfeit':
                if match['state'] != 'active':
                    raise Rejected(409, 'match is not active')
                target_side = 'b' if side == 'a' else 'a'
                last = match['heartbeat_' + target_side]
                if last is not None and now - last < HEARTBEAT_TIMEOUT:
                    raise Rejected(409, 'opponent heartbeat is still current')
                if match['forfeit_claim'] and match['forfeit_claim'] != side:
                    db.execute("UPDATE matches SET state='disputed',status='disputed',ended_at=?,forfeit_claim=NULL,forfeit_at=NULL WHERE id=?", (now, mid))
                else:
                    db.execute("UPDATE matches SET forfeit_claim=?,forfeit_at=? WHERE id=?", (side, now, mid))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)
            if operation == 'abort':
                if match['state'] in ('reserved', 'active'):
                    db.execute("UPDATE matches SET state='aborted',status='aborted',ended_at=? WHERE id=?", (now, mid))
            elif operation == 'ready':
                if match['state'] not in ('confirmed', 'reserved', 'active'):
                    raise Rejected(409, "match is not joinable")
                db.execute(f"UPDATE matches SET ready_{side}=1,heartbeat_{side}=? WHERE id=?", (now, mid))
                db.execute("""UPDATE matches SET state='active',status='active',started_at=?,expires=? WHERE id=? AND
                           state IN ('confirmed','reserved') AND ready_a=1 AND ready_b=1""", (now, now + ACTIVE_MATCH_TIMEOUT, mid))
            else:
                winner = data.get('winner_id', data.get('winner'))
                if winner not in (match['a'], match['b'], 'draw'):
                    raise Rejected(400, "winner_id must be a participant or draw")
                if operation == 'match_report':
                    p1_chars = self._characters(data.get('p1_chars'), 'p1_chars')
                    p2_chars = self._characters(data.get('p2_chars'), 'p2_chars')
                    p1_score = self._score(data.get('p1_score'), 'p1_score')
                    p2_score = self._score(data.get('p2_score'), 'p2_score')
                    if ((winner == 'draw' and p1_score != p2_score) or
                            (winner == match['a'] and p1_score <= p2_score) or
                            (winner == match['b'] and p2_score <= p1_score)):
                        raise Rejected(400, 'winner_id must agree with the reported score')
                else:
                    # Keep the original /v1/report winner-only contract working.
                    p1_chars = p2_chars = None
                    p1_score, p2_score = ((0, 0) if winner == 'draw' else
                                          (1, 0) if winner == match['a'] else (0, 1))
                report = dict(p1_chars=p1_chars, p2_chars=p2_chars, p1_score=p1_score,
                              p2_score=p2_score, winner_id=winner)
                encoded_report = json.dumps(report, sort_keys=True, separators=(',', ':'))
                prior = match['report_' + side]
                prior_report = self._decode_report(prior, match) if prior is not None else None
                if prior is not None and prior_report != report:
                    raise Rejected(409, "report is immutable")
                if match['state'] not in ('active', 'completed', 'disputed'):
                    raise Rejected(409, "match is not active")
                if match['state'] == 'active':
                    db.execute(f"UPDATE matches SET report_{side}=? WHERE id=?", (encoded_report, mid))
                    other_report = match['report_b' if side == 'a' else 'report_a']
                    if other_report is not None:
                        agrees = self._decode_report(other_report, match) == report
                        state = 'completed' if agrees else 'disputed'
                        db.execute("""UPDATE matches SET state=?,status=?,forfeit_claim=NULL,forfeit_at=NULL,
                                   p1_chars=?,p2_chars=?,p1_score=?,p2_score=?,winner_id=?,ended_at=? WHERE id=?""",
                                   (state, state, json.dumps(p1_chars) if agrees and p1_chars else None,
                                    json.dumps(p2_chars) if agrees and p2_chars else None,
                                    p1_score if agrees else None, p2_score if agrees else None,
                                    winner if agrees else None, now, mid))
                        if state == 'completed':
                            completed_match = dict(match)
                            completed_match['p1_score'], completed_match['p2_score'] = p1_score, p2_score
                            self.finish(db, completed_match, winner, 'completed', now)
            return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)


def make_server(store, port=8765, host='127.0.0.1', allow_registration=None):
    if allow_registration is None:
        try:
            local_bind = host.lower() == 'localhost' or ipaddress.ip_address(host).is_loopback
        except ValueError:
            local_bind = False
        allow_registration = local_bind or os.environ.get('TTT2_RANKED_ALLOW_REGISTRATION') == '1'
    registration_attempts = {}
    registration_lock = threading.Lock()
    trust_proxy = os.environ.get('TTT2_RANKED_TRUST_PROXY') == '1'

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *_):
            pass  # Do not log bearer tokens or payloads.

        def do_GET(self):
            parsed = urlsplit(self.path)
            try:
                if parsed.path == '/healthz':
                    with closing(store.connect()) as db:
                        db.execute('SELECT 1').fetchone()
                    result, status = {'status': 'ok'}, 200
                elif parsed.path == '/v1/status':
                    auth = self.headers.get('Authorization', '')
                    if not auth.startswith('Bearer ') or len(auth) > 256:
                        raise Rejected(401, 'bearer token required')
                    result = store.dispatch(auth[7:], 'status', {})
                    result = store.provision_relay(result)
                    status = 200
                elif parsed.path == '/v1/leaderboard':
                    query = parse_qs(parsed.query)
                    result = store.leaderboard(query.get('limit', ['50'])[0])
                    status = 200
                else:
                    history = re.fullmatch(r'/v1/players/([A-Za-z0-9_.-]{1,64})/history', parsed.path)
                    if not history:
                        raise Rejected(404, 'unknown endpoint')
                    query = parse_qs(parsed.query)
                    result = store.player_history(history.group(1), query.get('limit', ['20'])[0])
                    status = 200
            except Rejected as error:
                result, status = {'error': error.message}, error.status
            except sqlite3.Error:
                result, status = {'error': 'service unavailable'}, 503
            body = json.dumps(result, separators=(',', ':')).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            self.connection.settimeout(5)
            try:
                if self.headers.get('Transfer-Encoding'):
                    raise Rejected(400, 'transfer encoding not supported')
                size = int(self.headers.get('Content-Length', '-1'))
                if not 0 <= size <= 4096:
                    raise Rejected(413, 'body must be at most 4096 bytes')
                route = urlsplit(self.path).path
                if route == '/v1/register':
                    if not allow_registration:
                        raise Rejected(403, 'self-registration is disabled on this server')
                    data = json.loads(self.rfile.read(size))
                    if not isinstance(data, dict) or set(data) != {'player'}:
                        raise Rejected(400, 'expected a player identifier')
                    now = time.monotonic()
                    client = self.client_address[0]
                    if trust_proxy:
                        forwarded = self.headers.get('X-Forwarded-For', '').split(',', 1)[0].strip()
                        try:
                            client = str(ipaddress.ip_address(forwarded))
                        except ValueError:
                            pass
                    with registration_lock:
                        attempts = [stamp for stamp in registration_attempts.get(client, []) if now - stamp < 3600]
                        if len(attempts) >= 5:
                            raise Rejected(429, 'registration limit reached; try again later')
                        attempts.append(now)
                        registration_attempts[client] = attempts
                    token = store.provision(data['player'])
                    result = dict(id=data['player'], token=token)
                    status = 201
                else:
                    auth = self.headers.get('Authorization', '')
                    if not auth.startswith('Bearer ') or len(auth) > 256:
                        raise Rejected(401, 'bearer token required')
                    if not route.startswith('/v1/'):
                        raise Rejected(404, 'unknown endpoint')
                    data = json.loads(self.rfile.read(size))
                    report_route = re.fullmatch(r'/v1/matches/([A-Za-z0-9_.-]{1,64})/report', route)
                    acceptance_route = re.fullmatch(r'/v1/matches/([A-Za-z0-9_.-]{1,64})/(accept|decline)', route)
                    if report_route:
                        if not isinstance(data, dict):
                            raise Rejected(400, 'expected JSON object')
                        data['match_id'] = report_route.group(1)
                        operation = 'match_report'
                    elif acceptance_route:
                        if not isinstance(data, dict) or data:
                            raise Rejected(400, 'accept and decline requests do not take a body')
                        data['match_id'] = acceptance_route.group(1)
                        operation = acceptance_route.group(2)
                    else:
                        operation = route[4:]
                    result = store.dispatch(auth[7:], operation, data)
                    result = store.provision_relay(result)
                    status = 408 if operation == 'accept' and result.get('state') == 'expired' else 200
            except Rejected as error:
                status, result = error.status, dict(error=error.message)
            except (ValueError, UnicodeError):
                status, result = 400, dict(error='invalid JSON or length')
            except (TimeoutError, OSError):
                return
            body = json.dumps(result).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(body)
    class Server(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True
    return Server((host, port), Handler)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--db', default=os.environ.get('TTT2_RANKED_DB', 'ranked.sqlite3'))
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('provision').add_argument('player')
    serve = sub.add_parser('serve')
    serve.add_argument('--host', default=os.environ.get('TTT2_RANKED_HOST', '127.0.0.1'))
    serve.add_argument('--port', type=int, default=int(os.environ.get('TTT2_RANKED_PORT', '8765')))
    args = parser.parse_args()
    store = RankedStore(args.db)
    if args.command == 'provision':
        print(store.provision(args.player))
    else:
        server = make_server(store, args.port, args.host)
        print(f'Ranked service listening on {args.host}:{server.server_port}', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
