#!/usr/bin/env python3
import socket, sys
def health(host, peer, port=19980, timeout=6):
    try:
        s=socket.create_connection((host,port),timeout=timeout)
        s.sendall(f"HEALTH {peer}\n".encode())
        s.settimeout(timeout)
        data=b''
        while b'\n' not in data and len(data)<256:
            chunk=s.recv(256)
            if not chunk: break
            data+=chunk
        s.close()
        return data.decode(errors='replace').strip() or "(empty)"
    except Exception as e:
        return f"(err {e})"

# Run locally: queries THIS node's daemon IPC for each peer
node=sys.argv[1]
peers=sys.argv[2:]
for p in peers:
    print(f"{node}->{p}: {health('127.0.0.1', p)}")
