#!/usr/bin/env python3
"""bench-perf.py — local loopback perf harness for BridgeSessions.

Spins up two daemons on 127.0.0.1 (isolated --config-dir each) and measures:

  file_mib_s   file transfer throughput (median over reps, per size)
  cmd_ms       warm one-shot `bs shell --cmd` latency (p50/p95)
  key_ms       interactive keystroke→echo latency inside an attached
               session (p50/p95) — the "typing feels fast" metric

Output: JSON to --out and a one-line summary on stdout.
Designed for the ralph loop: deterministic sizes, median scoring, machine
output only. Rendering sanity is enforced by the caller (ctest must stay
green); this harness only measures speed.
"""
from __future__ import annotations

import argparse
import json
import os
import pty as pty_mod
import select
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SIZES_MIB_DEFAULT = [1, 64, 256]
REPS_DEFAULT = 3
KEYSTROKES_DEFAULT = 40
CMD_REPS_DEFAULT = 8


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=kw.pop("timeout", 120), **kw)


def pubkey_hex(config_dir: Path) -> str:
    # keygen writes id_ed25519.pub with the raw 32-byte key in hex on line 1
    pub = config_dir / "id_ed25519.pub"
    if pub.exists():
        for line in pub.read_text().splitlines():
            hexpart = line.strip().split()[-1] if line.strip() else ""
            if len(hexpart) == 64 and all(c in "0123456789abcdef" for c in hexpart.lower()):
                return hexpart.lower()
    out = subprocess.run(
        ["openssl", "pkey", "-in", str(config_dir / "id_ed25519.pem"), "-pubout", "-outform", "DER"],
        capture_output=True, timeout=15,
    )
    return out.stdout[-32:].hex()


def wait_port(port: int, timeout: float = 15.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


class Node:
    def __init__(self, name: str, port: int, ipc_port: int, root: Path, binary: str):
        self.name = name
        self.port = port
        self.ipc_port = ipc_port
        self.dir = root / name
        self.dir.mkdir(parents=True, exist_ok=True)
        self.binary = binary
        (self.dir / "config").write_text(
            f"node.name {name}\n"
            f"node.listen 127.0.0.1:{port}\n"
            f"mesh.mdns_enabled false\n"
            f"mesh.ping_interval_secs 1\n"
            f"mesh.gossip_interval_secs 5\n"
        )
        self.proc: subprocess.Popen | None = None

    def env(self) -> dict:
        e = dict(os.environ)
        e["BRIDGESESSIONS_IPC_PORT"] = str(self.ipc_port)
        return e

    def cli(self, *args, timeout=120):
        return sh([self.binary, "--config-dir", str(self.dir), *args],
                  timeout=timeout, env=self.env())

    def start_daemon(self):
        self.cli("keygen")
        self.cli("--daemon", timeout=15)
        if not wait_port(self.port):
            raise RuntimeError(f"{self.name}: daemon did not open port {self.port}")

    def stop(self):
        subprocess.run(
            ["pkill", "-f", f"config-dir {self.dir}"],
            capture_output=True, timeout=10,
        )


def bench_file(a: Node, b: Node, sizes: list[int], reps: int, compressible: bool = False) -> dict:
    results = {}
    tmp = Path(tempfile.mkdtemp(prefix="bs-bench-file-"))
    try:
        for size in sizes:
            f = tmp / f"{size}m.bin"
            if compressible:
                # Realistic compressible payload: a 4KB random block repeated.
                # Random-only benches hide the effect of compression entirely.
                block = os.urandom(4096)
                f.write_bytes(block * (size * 1024 * 1024 // 4096))
            else:
                f.write_bytes(os.urandom(size * 1024 * 1024))
            times = []
            for _ in range(reps):
                t0 = time.monotonic()
                r = a.cli("file", "send", b.name, str(f), "--wait", timeout=600)
                dt = time.monotonic() - t0
                if r.returncode != 0:
                    raise RuntimeError(f"file send failed: {r.stderr[-300:]}")
                times.append(dt)
            med = statistics.median(times)
            results[f"{size}MiB"] = {
                "median_s": round(med, 3),
                "mib_s": round(size / med, 2),
                "reps_s": [round(t, 3) for t in times],
            }
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return results


def bench_cmd_latency(a: Node, b: Node, reps: int) -> dict:
    times = []
    for _ in range(reps):
        t0 = time.monotonic()
        r = a.cli("shell", b.name, "--cmd", "echo BS_BENCH_PING", timeout=30)
        dt = (time.monotonic() - t0) * 1000
        if "BS_BENCH_PING" not in r.stdout:
            raise RuntimeError(f"cmd probe failed: {r.stdout[-200:]} {r.stderr[-200:]}")
        times.append(dt)
    times.sort()
    return {
        "p50_ms": round(statistics.median(times), 1),
        "p95_ms": round(times[max(0, int(len(times) * 0.95) - 1)], 1),
        "reps_ms": [round(t, 1) for t in times],
    }


def bench_keystroke(a: Node, b: Node, strokes: int) -> dict:
    """Attach to a session on b, run `cat`, measure char→echo latency."""
    session = "benchkeys"
    # ensure session exists (run detached cat via shell)
    a.cli("shell", b.name, "-n", session, "--cmd", "true", timeout=30)

    pid, fd = pty_mod.fork()
    if pid == 0:
        os.environ["BRIDGESESSIONS_IPC_PORT"] = str(a.ipc_port)
        os.execv(a.binary, [a.binary, "--config-dir", str(a.dir), b.name, session])
    lat = []
    try:
        os.set_blocking(fd, False)

        def drain(until: bytes | None = None, timeout: float = 20.0) -> bytes:
            buf = b""
            t0 = time.monotonic()
            while time.monotonic() - t0 < timeout:
                rl, _, _ = select.select([fd], [], [], 0.2)
                if rl:
                    try:
                        chunk = os.read(fd, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    if until and until in buf:
                        return buf
            return buf

        drain(until=b"$", timeout=20)  # shell prompt
        marker = b"BSKEY_READY"
        os.write(fd, b"stty -echo; printf %s " + marker + b"\n")
        drain(until=marker, timeout=10)
        drain(timeout=0.5)
        # raw char echo test: send char, time until it appears back
        for i in range(strokes):
            ch = b"x" if i % 2 == 0 else b"y"
            t0 = time.monotonic()
            os.write(fd, ch)
            got = drain(until=ch, timeout=5)
            if not got:
                continue
            lat.append((time.monotonic() - t0) * 1000)
        os.write(fd, b"\x03exit\n")  # ctrl-c then exit
        drain(timeout=1)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except (ChildProcessError, ProcessLookupError):
            pass
    lat.sort()
    if not lat:
        raise RuntimeError("keystroke bench: no echoes received")
    return {
        "p50_ms": round(statistics.median(lat), 1),
        "p95_ms": round(lat[max(0, int(len(lat) * 0.95) - 1)], 1),
        "samples": len(lat),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="bridgesessions binary under test")
    ap.add_argument("--out", required=True, help="JSON output path")
    ap.add_argument("--sizes", default=",".join(map(str, SIZES_MIB_DEFAULT)))
    ap.add_argument("--reps", type=int, default=REPS_DEFAULT)
    ap.add_argument("--keystrokes", type=int, default=KEYSTROKES_DEFAULT)
    ap.add_argument("--cmd-reps", type=int, default=CMD_REPS_DEFAULT)
    ap.add_argument("--compressible", action="store_true",
                    help="file payloads are compressible (repeated 4KB block)")
    ap.add_argument("--keep", action="store_true", help="keep bench dirs (debug)")
    args = ap.parse_args()

    sizes = [int(s) for s in args.sizes.split(",") if s]
    root = Path(tempfile.mkdtemp(prefix="bs-bench-"))
    a = Node("benchA", 29491, 29493, root, args.bin)
    b = Node("benchB", 29492, 29494, root, args.bin)
    report: dict = {"bin": args.bin, "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    try:
        a.start_daemon()
        b.start_daemon()
        pub_a, pub_b = pubkey_hex(a.dir), pubkey_hex(b.dir)
        # authorize each other + mutual seeds
        for node, peer, pub in ((a, b, pub_b), (b, a, pub_a)):
            with open(node.dir / "authorized_keys", "a") as fh:
                fh.write(f"pubkey {pub}\n")
            node.cli("peers", "add", peer.name, f"127.0.0.1:{peer.port}", "--pubkey", pub)
        # wait for the mesh to come up
        up = False
        for _ in range(30):
            r = a.cli("health", b.name, timeout=10)
            if r.returncode == 0:
                up = True
                break
            time.sleep(1)
        if not up:
            raise RuntimeError("mesh did not come up between benchA and benchB")

        report["file_mib_s"] = bench_file(a, b, sizes, args.reps, args.compressible)
        report["cmd_ms"] = bench_cmd_latency(a, b, args.cmd_reps)
        report["key_ms"] = bench_keystroke(a, b, args.keystrokes)
        report["status"] = "ok"
    except Exception as e:  # noqa: BLE001
        report["status"] = "error"
        report["error"] = str(e)[:500]
    finally:
        a.stop()
        b.stop()
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)

    Path(args.out).write_text(json.dumps(report, indent=2))
    if report["status"] == "ok":
        f = report["file_mib_s"]
        print(
            "BENCH file[{}]MiB/s cmd p50={}ms key p50={}ms".format(
                "/".join(str(f[k]["mib_s"]) for k in f),
                report["cmd_ms"]["p50_ms"],
                report["key_ms"]["p50_ms"],
            )
        )
        return 0
    print(f"BENCH ERROR: {report['error']}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
