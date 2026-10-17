"""Provision a temporary local ranked service and run the native two-client test."""
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ranked_service import RankedStore, make_server


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_ranked_e2e.py <ttt2_test_ranked_e2e.exe>")
    with tempfile.TemporaryDirectory(prefix="ttt2-ranked-e2e-") as temp:
        store = RankedStore(Path(temp) / "test.sqlite3")
        alice = store.provision("alice")
        bob = store.provision("bob")
        server = make_server(store, 0, "127.0.0.1")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{server.server_port}"
            executable = os.path.abspath(sys.argv[1])
            result = subprocess.run([executable, base, alice, bob], check=False)
            if result.returncode:
                return result.returncode

            # Exercise the launcher's already-confirmed handoff independently of queueing:
            # provision a match in the test service, then start native clients with only
            # the pre-confirmed environment block consumed by ranked_adapter.cpp.
            charlie = store.provision("confirmed-charlie")
            delta = store.provision("confirmed-delta")
            store.dispatch(charlie, "queue", {"build": "confirmed-e2e", "region": "local",
                                                "listen_endpoint": "127.0.0.1:9011"})
            store.dispatch(delta, "queue", {"build": "confirmed-e2e", "region": "local",
                                              "listen_endpoint": "127.0.0.1:9012"})
            pending = store.dispatch(charlie, "status", {})
            match_id = pending["match_id"]
            store.dispatch(charlie, "accept", {"match_id": match_id})
            store.dispatch(delta, "accept", {"match_id": match_id})
            reservations = {"confirmed-charlie": store.dispatch(charlie, "status", {}),
                            "confirmed-delta": store.dispatch(delta, "status", {})}

            children = []
            try:
                for player, token, port in (("confirmed-charlie", charlie, 9011),
                                            ("confirmed-delta", delta, 9012)):
                    reservation = reservations[player]
                    peer_id = reservation["player_b"] if player == reservation["player_a"] else reservation["player_a"]
                    env = dict(os.environ, TTT2_NET="ipv4", TTT2_RANKED_RUNTIME="1",
                               TTT2_RANKED_CONFIRMED="1", TTT2_RANKED_MATCH_ID=reservation["match_id"],
                               TTT2_RANKED_SESSION_KEY=reservation["session_key_bytes"],
                               TTT2_RANKED_ROLE=reservation["role"],
                               TTT2_RANKED_PEER_ENDPOINT=reservation["opponent_endpoint"],
                               TTT2_RANKED_MATCH_TOKEN=reservation["match_token"],
                               TTT2_RANKED_PLAYER=player, TTT2_RANKED_PEER_ID=peer_id)
                    children.append(subprocess.Popen([executable, "--confirmed-client", str(port)], env=env))
                codes = [child.wait(timeout=35) for child in children]
                if any(codes):
                    return next(code for code in codes if code)
            finally:
                for child in children:
                    if child.poll() is None:
                        child.kill()
            print("confirmed reservation env -> runtime adapter -> ENet peer exchange passed")
            return 0
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
