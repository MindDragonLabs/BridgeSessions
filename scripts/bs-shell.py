#!/usr/bin/env python3
"""BridgeSessions shell wrapper — works around macOS CLI IPC timeout bug."""
import socket, base64, sys

def shell(peer, cmd, host="127.0.0.1", port=19980, timeout=60):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((host, port))
    session = base64.b64encode(b"default").decode()
    cmd_b64 = base64.b64encode(cmd.encode()).decode()
    request = f"SHELL {peer} {session} {cmd_b64}\n"
    s.send(request.encode())
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
        if b"\n" in chunk:
            break
    s.close()
    if not data:
        return 1, ""
    decoded = base64.b64decode(data.strip()).decode()
    colon = decoded.find(":")
    if colon == -1:
        return 0, decoded
    try:
        exit_code = int(decoded[:colon])
    except ValueError:
        return 0, decoded
    return exit_code, decoded[colon+1:]

def health(peer, host="127.0.0.1", port=19980):
    s = socket.socket()
    s.settimeout(3)
    s.connect((host, port))
    s.send(f"HEALTH {peer}\n".encode())
    data = s.recv(1024).decode().strip()
    s.close()
    return data

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: bs-shell.py <peer> <command>")
        sys.exit(1)
    peer = sys.argv[1]
    cmd = " ".join(sys.argv[2:])
    if cmd == "health":
        print(health(peer))
    else:
        ec, output = shell(peer, cmd)
        if output:
            print(output, end="")
        sys.exit(ec)
