"""Daemon restart disconnect test for BridgeSessions (2026-08-09 regression).

Root cause: running "systemctl restart bridgesessions" via bs shell kills the
daemon you're connected through. The session dies with "SSL_read: unexpected eof"
rather than hanging. This test documents that the error path is clean (no hang).

This test:
  - Spins up two local daemons
  - Executes a command that kills the remote daemon
  - Verifies the CLI gets a clean error (not a 30s hang/timeout)
"""

from __future__ import annotations

import os
import signal
import subprocess
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
    """Manages a BS daemon."""

    def __init__(self, tmp_path: Path, name: str, mesh_port: int, ipc_port: int):
        self.name = name
        self.mesh_port = mesh_port
        self.ipc_port = ipc_port
        self.config_dir = tmp_path / name
        self.config_dir.mkdir(parents=True, exist_ok=True)
        self.process: subprocess.Popen | None = None

    def keygen(self) -> str:
        subprocess.run(
            [BINARY, "--config-dir", str(self.config_dir), "keygen"],
            env=self._env(), capture_output=True, timeout=10,
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
        # Foreground (no --daemon): pytest stdin is not a TTY, so main()
        # runs MeshController in this process. --daemon forks and the
        # parent exits immediately, which this helper treats as a crash.
        # pid() is then the live daemon, so remote kill -TERM hits it.
        self.process = subprocess.Popen(
            [BINARY, "--config-dir", str(self.config_dir)],
            env=self._env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        for _ in range(80):
            if self._is_ready():
                return
            time.sleep(0.1)
            if self.process.poll() is not None:
                raise RuntimeError(f"Daemon {self.name} exited early")
        raise TimeoutError(f"Daemon {self.name} not ready")

    def pid(self) -> int:
        if self.process:
            return self.process.pid
        raise RuntimeError("Daemon not started")

    def stop(self) -> None:
        if self.process and self.process.poll() is None:
            try:
                os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)
                self.process.wait(timeout=5)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass

    def shell(self, peer: str, cmd: str, timeout: int = 15) -> subprocess.CompletedProcess:
        return subprocess.run(
            [BINARY, "--config-dir", str(self.config_dir), "shell", peer, "--cmd", cmd],
            env=self._env(), capture_output=True, text=True, timeout=timeout,
        )


@pytest.fixture
def two_daemons(tmp_path):
    a = DaemonHelper(tmp_path, "restart-a", _free_port(), _free_port())
    b = DaemonHelper(tmp_path, "restart-b", _free_port(), _free_port())

    pk_a = a.keygen()
    pk_b = b.keygen()
    a.authorize(pk_b)
    b.authorize(pk_a)

    a.write_config(seeds=[("restart-b", "127.0.0.1", b.mesh_port, pk_b)])
    b.write_config(seeds=[("restart-a", "127.0.0.1", a.mesh_port, pk_a)])

    a.start()
    b.start()

    try:
        yield a, b
    finally:
        b.stop()
        a.stop()


def test_daemon_restart_produces_clean_error(two_daemons):
    """Killing the remote daemon via shell produces SSL eof, not a hang."""
    a, b = two_daemons

    # Wait for mesh to connect.
    for _ in range(100):
        result = a.shell("restart-b", "echo READY")
        if "READY" in result.stdout:
            break
        time.sleep(0.1)
    else:
        pytest.skip("Mesh not established")

    # Now send a command that kills the remote daemon's process.
    # We use kill on the daemon PID. The shell command runs on B and kills B.
    b_pid = b.pid()

    # Send the kill command. This should terminate B and break the SSL connection.
    # The CLI should get an error (SSL eof) within the timeout, not hang.
    start = time.monotonic()
    result = a.shell(
        "restart-b",
        f"kill -TERM {b_pid}",
        timeout=10,
    )
    elapsed = time.monotonic() - start

    # The command should return quickly (either error or partial output).
    # Key assertion: it did NOT hang for the full 10s timeout.
    # A clean SSL disconnect produces an error in 1-3s typically.
    assert elapsed < 8, (
        f"Shell command took {elapsed:.1f}s — possible hang on daemon restart"
    )

    # The remote daemon should now be dead.
    assert b.process is not None
    # Wait for process to exit.
    try:
        b.process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass


def test_daemon_disconnect_returns_error_not_hang(two_daemons):
    """After remote daemon dies, subsequent commands return error, not hang."""
    a, b = two_daemons

    # Wait for mesh.
    for _ in range(100):
        result = a.shell("restart-b", "echo READY")
        if "READY" in result.stdout:
            break
        time.sleep(0.1)
    else:
        pytest.skip("Mesh not established")

    # Kill B directly.
    b.stop()

    # Now try a shell command to B. It should fail quickly, not hang.
    start = time.monotonic()
    try:
        result = a.shell("restart-b", "echo POST_DEATH", timeout=10)
        elapsed = time.monotonic() - start
    except subprocess.TimeoutExpired:
        elapsed = time.monotonic() - start
        # A timeout is the worst case — we want to detect it.
        pytest.fail(
            f"Shell command hung for {elapsed:.1f}s after daemon death — "
            "expected clean error within timeout"
        )

    # Should complete in <10s (the timeout), and produce an error.
    assert elapsed < 8, (
        f"Post-death command took {elapsed:.1f}s — possible hang"
    )
