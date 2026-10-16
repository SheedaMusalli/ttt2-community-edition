"""Standalone provisional ranked matchmaking service.

Python standard library only. Ratings are provisional, based on matching peer reports.
For Internet deployment, place behind a TLS reverse proxy (see deploy/).
"""
import argparse
import hashlib
import json
import re
import secrets
import sqlite3
import time
import hmac
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os

HEARTBEAT_INTERVAL = 25
HEARTBEAT_TIMEOUT = 75
FORFEIT_GRACE = 30
INACTIVITY_FORFEIT_TIMEOUT = HEARTBEAT_TIMEOUT + FORFEIT_GRACE
ACTIVE_MATCH_TIMEOUT = 900


class Rejected(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def label(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
        raise Rejected(400, "expected a 1-64 character identifier")
    return value


class RankedStore:
    def __init__(self, path, clock=time.time):
        self.path, self.clock = str(path), clock
        with closing(self.connect()) as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS players(
                    id TEXT PRIMARY KEY, token_hash TEXT UNIQUE NOT NULL,
                    rating INTEGER NOT NULL DEFAULT 1000, games INTEGER NOT NULL DEFAULT 0);
                CREATE TABLE IF NOT EXISTS queue(
                    player TEXT PRIMARY KEY REFERENCES players(id), build TEXT NOT NULL,
                    region TEXT NOT NULL, expires REAL NOT NULL, joined REAL NOT NULL);
                CREATE TABLE IF NOT EXISTS matches(
                    id TEXT PRIMARY KEY, a TEXT NOT NULL, b TEXT NOT NULL,
                    build TEXT NOT NULL, region TEXT NOT NULL, expires REAL NOT NULL,
                    state TEXT NOT NULL, ready_a INTEGER DEFAULT 0, ready_b INTEGER DEFAULT 0,
                    report_a TEXT, report_b TEXT, connect_token TEXT,
                    heartbeat_a REAL, heartbeat_b REAL, forfeit_claim TEXT, forfeit_at REAL);
            """)
            # Upgrade databases created by the earlier prototype.
            columns = {r[1] for r in db.execute("PRAGMA table_info(matches)")}
            for name, kind in (("connect_token", "TEXT"), ("heartbeat_a", "REAL"), ("heartbeat_b", "REAL"),
                               ("forfeit_claim", "TEXT"), ("forfeit_at", "REAL")):
                if name not in columns:
                    db.execute(f"ALTER TABLE matches ADD COLUMN {name} {kind}")

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
        with self.transaction() as db:
            db.execute("INSERT INTO players(id,token_hash) VALUES (?,?)",
                       (player, hashlib.sha256(token.encode()).hexdigest()))
        return token

    def expire(self, db):
        now = self.clock()
        db.execute("DELETE FROM queue WHERE expires<=?", (now,))
        db.execute("UPDATE matches SET state='expired' WHERE expires<=? AND state IN ('reserved','active')", (now,))
        # A forfeit claim is only accepted after the accused player's heartbeat is stale.
        claims = db.execute("SELECT * FROM matches WHERE state='active' AND forfeit_claim IS NOT NULL AND forfeit_at<=?", (now - FORFEIT_GRACE,)).fetchall()
        for match in claims:
            side = match['forfeit_claim']
            accused_side = 'b' if side == 'a' else 'a'
            last = match['heartbeat_' + accused_side]
            if last is None or last <= match['forfeit_at']:
                self.finish(db, match, match[side], 'forfeit')
            else:
                db.execute("UPDATE matches SET forfeit_claim=NULL,forfeit_at=NULL WHERE id=?", (match['id'],))
        # Heartbeat expiry resolves an unresponsive active match instead of parking it for 15 minutes.
        active = db.execute("SELECT * FROM matches WHERE state='active'", ()).fetchall()
        for match in active:
            a, b = match['heartbeat_a'], match['heartbeat_b']
            if a is not None and b is not None and now - a > INACTIVITY_FORFEIT_TIMEOUT and now - b <= HEARTBEAT_TIMEOUT:
                self.finish(db, match, match['b'], 'forfeit')
            elif b is not None and now - b > INACTIVITY_FORFEIT_TIMEOUT and now - a <= HEARTBEAT_TIMEOUT:
                self.finish(db, match, match['a'], 'forfeit')
            elif (a is None or b is None or now - a > INACTIVITY_FORFEIT_TIMEOUT and now - b > INACTIVITY_FORFEIT_TIMEOUT) or now >= match['expires']:
                db.execute("UPDATE matches SET state='expired' WHERE id=?", (match['id'],))

    @staticmethod
    def finish(db, match, winner, state):
        db.execute("UPDATE matches SET state=? WHERE id=?", (state, match['id']))
        ra = db.execute("SELECT rating FROM players WHERE id=?", (match['a'],)).fetchone()[0]
        rb = db.execute("SELECT rating FROM players WHERE id=?", (match['b'],)).fetchone()[0]
        score = 0.5 if winner == 'draw' else float(winner == match['a'])
        delta = round(32 * (score - 1 / (1 + 10 ** (max(-4000, min(4000, rb - ra)) / 400))))
        db.execute("UPDATE players SET rating=rating+?,games=games+1 WHERE id=?", (delta, match['a']))
        db.execute("UPDATE players SET rating=rating-?,games=games+1 WHERE id=?", (delta, match['b']))

    @staticmethod
    def match_response(match, player):
        result = {"id": match['id'], "state": match['state'], "build": match['build'],
                  "region": match['region'], "expires": match['expires']}
        if player in (match['a'], match['b']):
            result.update(player_a=match['a'], player_b=match['b'], match_token=match['connect_token'])
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
                return dict(id=player, rating=row['rating'], games=row['games'], provisional=True)
            if operation == 'cancel':
                db.execute("DELETE FROM queue WHERE player=?", (player,))
                return dict(state='cancelled')
            if operation in ('queue', 'status'):
                match = db.execute("SELECT * FROM matches WHERE (a=? OR b=?) AND state IN ('reserved','active')",
                                   (player, player)).fetchone()
                if match:
                    return self.match_response(match, player)
                if operation == 'status':
                    q = db.execute("SELECT expires FROM queue WHERE player=?", (player,)).fetchone()
                    return dict(state='queued', expires=q[0]) if q else dict(state='idle')
                build, region = label(data.get('build')), label(data.get('region'))
                # One atomic transaction reserves both players; clients cannot choose ratings.
                db.execute("""INSERT INTO queue VALUES (?,?,?,?,?) ON CONFLICT(player) DO UPDATE SET
                           build=excluded.build,region=excluded.region,expires=excluded.expires""",
                           (player, build, region, now + 30, now))
                other = db.execute("""SELECT q.player FROM queue q JOIN players p ON p.id=q.player
                           WHERE q.player<>? AND q.build=? AND q.region=? AND ABS(p.rating-?)<=200
                           ORDER BY q.joined,q.player LIMIT 1""", (player, build, region, row['rating'])).fetchone()
                if not other:
                    return dict(state='queued', expires=now + 30)
                mid = secrets.token_hex(16)
                token = secrets.token_urlsafe(32)
                db.execute("INSERT INTO matches(id,a,b,build,region,expires,state,connect_token) VALUES (?,?,?,?,?,?,'reserved',?)",
                           (mid, other[0], player, build, region, now + 30, token))
                db.execute("DELETE FROM queue WHERE player IN (?,?)", (player, other[0]))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)
            if operation not in ('match', 'ready', 'report', 'abort', 'heartbeat', 'forfeit'):
                raise Rejected(404, "unknown operation")
            mid = label(data.get('match_id'))
            match = db.execute("SELECT * FROM matches WHERE id=? AND (a=? OR b=?)",
                               (mid, player, player)).fetchone()
            if match is None:
                raise Rejected(404, "match not found")
            side = 'a' if player == match['a'] else 'b'
            if operation == 'match':
                return self.match_response(match, player)
            if operation == 'heartbeat':
                if match['state'] != 'active':
                    raise Rejected(409, 'match is not active')
                db.execute(f"UPDATE matches SET heartbeat_{side}=?,expires=? WHERE id=?", (now, now + ACTIVE_MATCH_TIMEOUT, mid))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)
            if operation == 'forfeit':
                if match['state'] != 'active':
                    raise Rejected(409, 'match is not active')
                target_side = 'b' if side == 'a' else 'a'
                last = match['heartbeat_' + target_side]
                if last is not None and now - last < HEARTBEAT_TIMEOUT:
                    raise Rejected(409, 'opponent heartbeat is still current')
                if match['forfeit_claim'] and match['forfeit_claim'] != side:
                    db.execute("UPDATE matches SET state='disputed',forfeit_claim=NULL,forfeit_at=NULL WHERE id=?", (mid,))
                else:
                    db.execute("UPDATE matches SET forfeit_claim=?,forfeit_at=? WHERE id=?", (side, now, mid))
                return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)
            if operation == 'abort':
                if match['state'] in ('reserved', 'active'):
                    db.execute("UPDATE matches SET state='aborted' WHERE id=?", (mid,))
            elif operation == 'ready':
                if match['state'] not in ('reserved', 'active'):
                    raise Rejected(409, "match is not joinable")
                db.execute(f"UPDATE matches SET ready_{side}=1,heartbeat_{side}=? WHERE id=?", (now, mid))
                db.execute("""UPDATE matches SET state='active',expires=? WHERE id=? AND
                           state='reserved' AND ready_a=1 AND ready_b=1""", (now + ACTIVE_MATCH_TIMEOUT, mid))
            else:
                winner = data.get('winner')
                if winner not in (match['a'], match['b'], 'draw'):
                    raise Rejected(400, "winner must be a participant or draw")
                prior = match['report_' + side]
                if prior is not None and prior != winner:
                    raise Rejected(409, "report is immutable")
                if match['state'] not in ('active', 'completed', 'disputed'):
                    raise Rejected(409, "match is not active")
                if match['state'] == 'active':
                    db.execute(f"UPDATE matches SET report_{side}=? WHERE id=?", (winner, mid))
                    other_report = match['report_b' if side == 'a' else 'report_a']
                    if other_report is not None:
                        state = 'completed' if other_report == winner else 'disputed'
                        db.execute("UPDATE matches SET state=?,forfeit_claim=NULL,forfeit_at=NULL WHERE id=?", (state, mid))
                        if state == 'completed':
                            self.finish(db, match, winner, 'completed')
            return self.match_response(db.execute("SELECT * FROM matches WHERE id=?", (mid,)).fetchone(), player)


def make_server(store, port=8765, host='127.0.0.1'):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *_):
            pass  # Do not log bearer tokens or payloads.

        def do_GET(self):
            if self.path != '/healthz':
                self.send_error(404)
                return
            try:
                with closing(store.connect()) as db:
                    db.execute('SELECT 1').fetchone()
                body = b'{"status":"ok"}'
                self.send_response(200)
            except sqlite3.Error:
                body = b'{"status":"unavailable"}'
                self.send_response(503)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
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
                auth = self.headers.get('Authorization', '')
                if not auth.startswith('Bearer ') or len(auth) > 256:
                    raise Rejected(401, 'bearer token required')
                if not self.path.startswith('/v1/'):
                    raise Rejected(404, 'unknown endpoint')
                data = json.loads(self.rfile.read(size))
                result = store.dispatch(auth[7:], self.path[4:], data)
                status = 200
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
