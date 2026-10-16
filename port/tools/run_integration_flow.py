"""Provision two disposable local players, run the C++ HTTP/ENet integration test, clean up."""
import json
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from ranked_service import RankedStore

tools = Path(__file__).resolve().parent
exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ttt2-integration-') as temp:
    db = Path(temp) / 'ranked.sqlite3'
    store = RankedStore(db)
    alice, bob = store.provision('alice'), store.provision('bob')
    port = 18765
    server = subprocess.Popen([sys.executable, str(tools / 'ranked_service.py'), '--db', str(db), 'serve', '--port', str(port)],
                             cwd=tools, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = f'http://127.0.0.1:{port}'
    try:
        for _ in range(50):
            try:
                req = urllib.request.Request(base + '/v1/profile', data=b'{}', headers={'Authorization': 'Bearer ' + alice})
                urllib.request.urlopen(req, timeout=0.2).close()
                break
            except OSError:
                if server.poll() is not None:
                    raise RuntimeError('ranked service exited during startup')
                time.sleep(0.1)
        else:
            raise RuntimeError('ranked service did not start')
        result = subprocess.run([str(exe), base, alice, bob], cwd=temp, check=False)
        raise SystemExit(result.returncode)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
