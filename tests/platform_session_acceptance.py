"""Native session acceptance against two isolated daemon processes."""
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
expected = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip()
windows_shell = sys.argv[2] if len(sys.argv) > 2 else "powershell.exe -NoLogo -NoExit -NoProfile"
windows_cmd = windows_shell.lower().startswith("cmd.exe")
assert subprocess.check_output([binary, "--version"], text=True).strip() == expected
processes = []
logs = []

def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]

def call(node, command):
    token = (node["root"] / "ipc-token").read_text().strip()
    with socket.create_connection(("127.0.0.1", node["ipc"]), timeout=3) as sock:
        sock.settimeout(3)
        sock.sendall((token + " " + command + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        return data.decode().strip()

def wait(predicate, detail, seconds=45):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            if predicate():
                return
        except (OSError, ValueError, AssertionError) as exc:
            last = exc
        time.sleep(0.25)
    raise AssertionError(detail + ": " + str(last))

def read(node, session):
    reply = call(node, "SESSION_SCROLLBACK release-b " + session + " 0 65536")
    fields = reply.split()
    assert len(fields) >= 3 and fields[0] == "OK", reply
    assert fields[1].isdigit(), reply
    try:
        base64.b64decode(fields[2] + "=" * (-len(fields[2]) % 4), validate=True) if fields[2] != "-" else None
    except ValueError as exc:
        raise AssertionError("bad scrollback reply: " + reply) from exc
    return "" if fields[2] == "-" else base64.b64decode(fields[2] + "=" * (-len(fields[2]) % 4), validate=True).decode("utf-8", "replace")

with tempfile.TemporaryDirectory(prefix="bs-release-sessions-") as temp:
    root = Path(temp)
    nodes = [{"name": "release-" + letter, "root": root / letter,
              "mesh": free_port(), "ipc": free_port()} for letter in ("a", "b")]
    try:
        for node in nodes:
            node["root"].mkdir()
            subprocess.run([binary, "--config-dir", str(node["root"]), "keygen"],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            node["key"] = (node["root"] / "id_ed25519.pub").read_text().strip()
        for index, node in enumerate(nodes):
            peer = nodes[1-index]
            config = ("node.name " + node["name"] + "\nnode.listen 127.0.0.1:" + str(node["mesh"]) +
                      "\nmesh.auto_upgrade false\nmesh.mdns_enabled false\nmesh.require_seed_pins true\n" +
                      "seed " + peer["name"] + " 127.0.0.1:" + str(peer["mesh"]) + " pubkey=" + peer["key"] + "\n")
            if os.name == "nt":
                config += "sessions.default_shell " + windows_shell + "\n"
            (node["root"] / "config").write_text(config)
            env = dict(os.environ, BRIDGESESSIONS_IPC_PORT=str(node["ipc"]), BS_SESSION_WORKER="0")
            env.pop("BS_SESSION", None)
            env.pop("BS_SESSION_ID", None)
            output = open(node["root"] / "daemon.log", "wb")
            logs.append(output)
            process = subprocess.Popen([binary, "--config-dir", str(node["root"]),
                                        "--config", str(node["root"] / "config")]
                                       + (["--daemon"] if os.name == "nt" else []),
                                       env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=output)
            processes.append(process)
        for node, process in zip(nodes, processes):
            wait(lambda: process.poll() is None and call(node, "DAEMON_PROBE") == "OK bridgesessions",
                 "native daemon readiness")
        a, b = nodes
        env = dict(os.environ, BRIDGESESSIONS_IPC_PORT=str(a["ipc"]))
        env.pop("BS_SESSION", None)
        env.pop("BS_SESSION_ID", None)
        def cli(*args):
            return subprocess.run([binary, "--config-dir", str(a["root"]),
                                   "--config", str(a["root"] / "config"), *args],
                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=45)
        wait(lambda: cli("health", "release-b").returncode == 0, "pinned native mesh health", 70)
        for session in ("proof-a", "proof-b"):
            launch = [] if os.name == "nt" else ["-x", "/bin/sh -i"]
            result = cli("shell", "release-b", "-n", session, *launch, "--detach")
            assert result.returncode == 0, result.stderr
            wait(lambda: bool(read(a, session)), "session scrollback route readiness")
        for session, letter in (("proof-a", "A"), ("proof-b", "B")):
            marker = "RELEASE_" + letter + "_EXECUTED"
            command = (("set BS_PROOF=" + letter + "_EXECUTED\r\necho RELEASE_%BS_PROOF%\r\n"
                        if windows_cmd else "Write-Output ('RELEASE_' + '" + letter + "_EXECUTED')\r\n")
                       if os.name == "nt" else "printf '%s%s\\n' RELEASE_ " + letter + "_EXECUTED\n")
            encoded = base64.b64encode(command.encode()).decode().rstrip("=")
            reply = call(a, "SESSION_INPUT release-b " + session + " " + encoded)
            assert reply == "OK", reply
            wait(lambda: marker in read(a, session), "executed marker " + session)
        output_a, output_b = read(a, "proof-a"), read(a, "proof-b")
        assert "RELEASE_A_EXECUTED" in output_a and "RELEASE_B_EXECUTED" not in output_a
        assert "RELEASE_B_EXECUTED" in output_b and "RELEASE_A_EXECUTED" not in output_b
        time.sleep(10)
        command = (("set BS_IDLE=READBACK\r\necho IDLE_%BS_IDLE%\r\n"
                    if windows_cmd else "Write-Output ('IDLE_' + 'READBACK')\r\n")
                   if os.name == "nt" else "printf '%s%s\\n' IDLE_ READBACK\n")
        assert call(a, "SESSION_INPUT release-b proof-a " + base64.b64encode(command.encode()).decode().rstrip("=")) == "OK"
        wait(lambda: "IDLE_READBACK" in read(a, "proof-a"), "input/readback after full idle interval")
        print(json.dumps({"platform": sys.platform, "shell": windows_shell if os.name == "nt" else "/bin/sh -i", "version": expected, "native_daemons": 2,
                          "pinned_health": True, "session_input_readback": True,
                          "session_isolation": True, "idle_seconds": 10, "idle_readback": True}))
    except Exception:
        for session in ("proof-a", "proof-b"):
            try:
                print("Session " + session + ": " + repr(read(nodes[0], session)), file=sys.stderr)
            except Exception:
                pass
        for log in logs:
            log.flush()
        for node in nodes:
            path = node["root"] / "daemon.log"
            if path.exists():
                print(path.read_text(errors="replace")[-2500:], file=sys.stderr)
            events = node["root"] / "bs-mesh.log"
            if events.exists():
                print(events.read_text(errors="replace")[-7500:], file=sys.stderr)
        raise
    finally:
        for node in nodes:
            for session in ("proof-a", "proof-b"):
                try:
                    call(node, "SESSION_KILL . " + session)
                except OSError:
                    pass
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in logs:
            log.close()
