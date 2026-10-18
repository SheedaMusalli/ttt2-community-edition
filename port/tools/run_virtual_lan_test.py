import os, socket, subprocess, sys

def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def main():
    executable=os.path.abspath(sys.argv[1]); host_port=free_port(); client_port=free_port()
    while client_port==host_port: client_port=free_port()
    common=dict(os.environ,TTT2_NET="hamachi",TTT2_LOCAL_IP="127.0.0.1",TTT2_NET_PORT="0",TTT2_NET_ROLLBACK="1")
    host_env=dict(common,TTT2_DISCOVERY_PORT=str(host_port),TTT2_PEER_IP=f"127.0.0.1:{client_port}")
    client_env=dict(common,TTT2_DISCOVERY_PORT=str(client_port),TTT2_PEER_IP=f"127.0.0.1:{host_port}")
    host=subprocess.Popen([executable,"host"],env=host_env,stdout=subprocess.PIPE,text=True)
    try:
        assert host.stdout.readline().strip()=="READY", "host initialization failed"
        client=subprocess.run([executable,"client"],env=client_env,timeout=30)
        assert client.returncode==0,f"client exit {client.returncode}"
        assert host.wait(timeout=10)==0,"host failed"
    finally:
        if host.poll() is None: host.kill();host.wait()
    print("Two-process test used distinct local discovery endpoints and unique ENet peer IDs.")
if __name__=="__main__":main()
