#!/usr/bin/env python3
"""Local integration check for the relay's registration API and UDP forwarding."""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, token, method="POST", body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Authorization": "Bearer " + token,
                                          "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as response:
        return response.status, response.read()


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_relay_daemon.py PATH_TO_DAEMON")
    binary = os.path.abspath(sys.argv[1])
    udp_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    secret = "local-test-admin-token"
    env = dict(os.environ, TTT2_RELAY_ADMIN_TOKEN=secret)
    process = subprocess.Popen([binary, "--udp-port", str(udp_port), "--http-port", str(http_port), "--workers", "1"],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    match_id, relay_token = "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210"
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("relay exited during startup: " + process.stderr.read().decode(errors="replace"))
            try:
                request(f"http://127.0.0.1:{http_port}/v1/relay/sessions", secret,
                        body={"match_id": match_id, "relay_token": relay_token,
                              "p1_id": "player_a", "p2_id": "player_b", "ttl": 120})
                break
            except (OSError, urllib.error.URLError):
                time.sleep(.05)
        else:
            raise TimeoutError("relay HTTP API did not start")
        relay = ("127.0.0.1", udp_port)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as p1, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as p2:
            p1.bind(("127.0.0.1", 0)); p2.bind(("127.0.0.1", 0))
            p1.settimeout(2); p2.settimeout(2)
            p1.sendto(bytes.fromhex(relay_token) + b"\x01" + b"hello-from-one", relay)
            p2.sendto(bytes.fromhex(relay_token) + b"\x02" + b"hello-from-two", relay)
            from_one, source_one = p1.recvfrom(2048)
            assert from_one == bytes.fromhex(relay_token) + b"\x02" + b"hello-from-two", from_one
            p1.sendto(bytes.fromhex(relay_token) + b"\x01" + b"second-from-one", relay)
            from_two, source_two = p2.recvfrom(2048)
            assert from_two == bytes.fromhex(relay_token) + b"\x01" + b"second-from-one", from_two
            assert source_one == relay and source_two == relay
        status, _ = request(f"http://127.0.0.1:{http_port}/v1/relay/sessions/{match_id}", secret, method="DELETE")
        assert status == 200
        print("relay registration, bidirectional routing, source translation, and teardown passed")
    finally:
        process.terminate()
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
