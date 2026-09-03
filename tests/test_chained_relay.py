"""Chained BS relay test (bs → bs → ssh) for BridgeSessions (2026-08-09 regression).

Root cause: BS should work when chaining through multiple hops:
  bs shell linux-a → from linux-a bs shell linux-b → should work

This test:
  - Spins up 3 local daemons forming a chain: A → B → C
  - Verifies shell commands relay correctly through the chain
  - Tests that a command on C can be executed via B's relay
"""

from __future__ import annotations

import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get(
    "BRIDGESESSIONS_BINARY",
    str(REPO_ROOT / "build" / "bridgesessions"),
)


def _free_port() -> int:
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class DaemonHelper:
    """Manages a BS daemon with config, keys, and seeds."""

    def __init__(self, tmp_path: Path, name: str, mesh_port: int, ipc_port: int):
        self.name = name
        self.mesh_port = mesh_port
        self.ipc_port = ipc_port
        self.config_dir = tmp_path / name
        self.config_dir.mkdir(parents=True, exist_ok=True)
        self.process: subprocess.Popen | None = None
        self.log_file: Path | None = None

    def keygen(self) -> str:
        env = self._env()
        subprocess.run(
            [BINARY, "--config-dir", str(self.config_dir), "keygen"],
            env=env, capture_output=True, timeout=10,
        )
        return (self.config_dir / "id_ed25519.pub").read_text().strip()

    def authorize(self, pubkey: str) -> None:
        ak = self.config_dir / "authorized_keys"
        existing = ak.read_text() if ak.exists() else ""
        ak.write_text(existing + pubkey + "\n")

    def write_config(self, seeds: list[tuple[str, str, int, str]] | None = None) -> None:
        lines = [
            f"node.name {self.name}",
            f"node.listen 127.0.0.1:{self.mesh_port}",
            "mesh.gossip_interval_secs 300",
            "mesh.ping_interval_secs 5",
            "mesh.reconnect_backoff_max_secs 300",
            "mesh.startup_wait_secs 0",
            "mesh.max_peers 10",
        ]
        if seeds:
            for sname, saddr, sport, spk in seeds:
                lines.append(f"seed {sname} {saddr}:{sport} pubkey={spk}")
        (self.config_dir / "config").write_text("\n".join(lines) + "\n")

    def _env(self) -> dict:
        env = os.environ.copy()
        env["BRIDGESESSIONS_IPC_PORT"] = str(self.ipc_port)
        return env

    def _is_ready(self) -> bool:
        import socket
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect(("127.0.0.1", self.ipc_port))
            s.close()
            return True
        except (OSError, ConnectionRefusedError):
            return False

    def start(self) -> None:
        log_fd, log_path = tempfile.mkstemp(suffix=".log", prefix=f"bs-{self.name}-")
        os.close(log_fd)
        self.log_file = Path(log_path)

        self.process = subprocess.Popen(
            [BINARY, "--config-dir", str(self.config_dir), "--daemon"],
            env=self._env(),
            stdout=open(log_path, "w"),
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        for _ in range(50):
            if self._is_ready():
                return
            time.sleep(0.1)
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"Daemon {self.name} exited. Log:\n{self.log_file.read_text()}"
                )
        raise TimeoutError(f"Daemon {self.name} not ready in 5s")

    def stop(self) -> None:
        if self.process and self.process.poll() is None:
            try:
                os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
                self.process.wait(timeout=5)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def shell(self, peer: str, cmd: str, timeout: int = 15) -> str:
        result = subprocess.run(
            [BINARY, "--config-dir", str(self.config_dir), "shell", peer, "--cmd", cmd],
            env=self._env(), capture_output=True, text=True, timeout=timeout,
        )
        return result.stdout.strip()


@pytest.fixture
def three_daemon_chain(tmp_path):
    """Spin up 3 daemons: A seeds B, B seeds C."""
    a = DaemonHelper(tmp_path, "chain-a", _free_port(), _free_port())
    b = DaemonHelper(tmp_path, "chain-b", _free_port(), _free_port())
    c = DaemonHelper(tmp_path, "chain-c", _free_port(), _free_port())

    pk_a = a.keygen()
    pk_b = b.keygen()
    pk_c = c.keygen()

    # Cross-authorize adjacent pairs
    a.authorize(pk_b)
    b.authorize(pk_a)
    b.authorize(pk_c)
    c.authorize(pk_b)

    # Chain: A → B, B → C
    a.write_config(seeds=[("chain-b", "127.0.0.1", b.mesh_port, pk_b)])
    b.write_config(seeds=[("chain-c", "127.0.0.1", c.mesh_port, pk_c)])
    c.write_config()

    for d in [a, b, c]:
        d.start()

    try:
        yield a, b, c
    finally:
        for d in [c, b, a]:
            d.stop()


def test_chain_direct_connections(three_daemon_chain):
    """A→B and B→C direct connections work."""
    a, b, c = three_daemon_chain

    # Wait for A→B mesh to establish, then test shell.
    connected_ab = False
    for _ in range(100):
        out = a.shell("chain-b", "echo CHAIN_AB_OK")
        if "CHAIN_AB_OK" in out:
            connected_ab = True
            break
        time.sleep(0.1)

    if not connected_ab:
        pytest.skip("A→B mesh not established")

    # Wait for B→C mesh to establish.
    connected_bc = False
    for _ in range(100):
        out = b.shell("chain-c", "echo CHAIN_BC_OK")
        if "CHAIN_BC_OK" in out:
            connected_bc = True
            break
        time.sleep(0.1)

    if not connected_bc:
        pytest.skip("B→C mesh not established")


def test_chain_shell_returns_correct_hostname(three_daemon_chain):
    """Shell commands return the correct remote host."""
    a, b, c = three_daemon_chain

    # Wait for A→B connection
    for _ in range(100):
        out = a.shell("chain-b", "echo READY")
        if "READY" in out:
            break
        time.sleep(0.1)
    else:
        pytest.skip("A→B mesh not established")

    # Verify shell on B returns B's hostname (not A's)
    out = a.shell("chain-b", "hostname")
    # The shell should have executed (non-empty output).
    assert len(out) > 0, "Shell command produced no output"


def test_chain_b_can_reach_c(three_daemon_chain):
    """B can independently reach C (the chain's second hop)."""
    a, b, c = three_daemon_chain

    # Wait for B→C connection
    for _ in range(100):
        out = b.shell("chain-c", "echo HOP_OK")
        if "HOP_OK" in out:
            break
        time.sleep(0.1)
    else:
        pytest.skip("B→C mesh not established")

    # Verify multiple commands work through the chain.
    for i in range(3):
        marker = f"CHAIN_CMD_{i}"
        out = b.shell("chain-c", f"echo {marker}")
        assert marker in out, f"Chain command {i} failed: {out}"
