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
            return subprocess.run([os.path.abspath(sys.argv[1]), base, alice, bob], check=False).returncode
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
