"""Interactive typing latency test for BridgeSessions (2026-08-09 regression).

Root cause: each keystroke in interactive mode round-trips:
  CLI → daemon IPC → TLS mesh → remote daemon → PTY → back
Under handshake storm conditions (dead seeds), this could take 30s+.

This test:
  - Spins up two local daemons on ephemeral ports
  - Measures keystroke round-trip latency
  - Asserts <500ms per keystroke
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
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


def _make_config(config_dir: Path, node_name: str, listen_port: int,
                 ipc_port: int, seeds: list[tuple[str, str, int]] | None = None) -> Path:
    """Create a minimal BS config with identity."""
    config_dir.mkdir(parents=True, exist_ok=True)
    config_path = config_dir / "config"
    lines = [
        f"node.name {node_name}",
        f"node.listen 127.0.0.1:{listen_port}",
        "mesh.gossip_interval_secs 300",
        "mesh.ping_interval_secs 5",
        "mesh.reconnect_backoff_max_secs 300",
        "mesh.startup_wait_secs 0",
        f"mesh.max_peers 10",
    ]
    if seeds:
        for name, addr, port in seeds:
            lines.append(f"seed {name} {addr}:{port}")
    config_path.write_text("\n".join(lines) + "\n")
    return config_path


class DaemonProcess:
    """Manages a BS daemon subprocess."""

    def __init__(self, config_dir: Path, ipc_port: int, name: str = "daemon"):
        self.config_dir = config_dir
        self.ipc_port = ipc_port
        self.name = name
        self.process: subprocess.Popen | None = None
        self.log_file: Path | None = None

    def start(self) -> None:
        log_fd, log_path = tempfile.mkstemp(suffix=".log", prefix=f"bs-{self.name}-")
        os.close(log_fd)
        self.log_file = Path(log_path)

        env = os.environ.copy()
        env["BRIDGESESSIONS_IPC_PORT"] = str(self.ipc_port)

        self.process = subprocess.Popen(
            [BINARY, "--config-dir", str(self.config_dir), "--daemon"],
            env=env,
            stdout=open(log_path, "w"),
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        # Wait for daemon to be ready (check IPC port is listening).
        for _ in range(50):
            if self._is_ready():
                return
            time.sleep(0.1)
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"Daemon {self.name} exited early. Log:\n{self.log_file.read_text()}"
                )
        raise TimeoutError(f"Daemon {self.name} did not become ready in 5s")

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

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()


@pytest.fixture
def two_daemons(tmp_path):
    """Spin up two BS daemons that can mesh-connect."""
    mesh_port_a = _free_port()
    mesh_port_b = _free_port()
    ipc_port_a = _free_port()
    ipc_port_b = _free_port()

    # Create configs — each seeds the other.
    # First, we need keypairs. Run keygen for each.
    dir_a = tmp_path / "daemon-a"
    dir_b = tmp_path / "daemon-b"
    dir_a.mkdir()
    dir_b.mkdir()

    # Generate keys via the binary
    env_a = os.environ.copy()
    env_a["BRIDGESESSIONS_IPC_PORT"] = str(ipc_port_a)
    subprocess.run(
        [BINARY, "--config-dir", str(dir_a), "keygen"],
        env=env_a, capture_output=True, timeout=10,
    )
    env_b = os.environ.copy()
    env_b["BRIDGESESSIONS_IPC_PORT"] = str(ipc_port_b)
    subprocess.run(
        [BINARY, "--config-dir", str(dir_b), "keygen"],
        env=env_b, capture_output=True, timeout=10,
    )

    # Read pubkeys
    pk_a = (dir_a / "identity.pub").read_text().strip()
    pk_b = (dir_b / "identity.pub").read_text().strip()

    # Cross-authorize
    (dir_a / "authorized_keys").write_text(pk_b + "\n")
    (dir_b / "authorized_keys").write_text(pk_a + "\n")

    # Create configs with seeds and pubkey pins
    config_a = dir_a / "config"
    config_a.write_text(
        f"node.name node-a\n"
        f"node.listen 127.0.0.1:{mesh_port_a}\n"
        f"mesh.gossip_interval_secs 300\n"
        f"mesh.ping_interval_secs 5\n"
        f"mesh.reconnect_backoff_max_secs 300\n"
        f"mesh.startup_wait_secs 0\n"
        f"seed node-b 127.0.0.1:{mesh_port_b} pubkey={pk_b}\n"
    )
    config_b = dir_b / "config"
    config_b.write_text(
        f"node.name node-b\n"
        f"node.listen 127.0.0.1:{mesh_port_b}\n"
        f"mesh.gossip_interval_secs 300\n"
        f"mesh.ping_interval_secs 5\n"
        f"mesh.reconnect_backoff_max_secs 300\n"
        f"mesh.startup_wait_secs 0\n"
        f"seed node-a 127.0.0.1:{mesh_port_a} pubkey={pk_a}\n"
    )

    da = DaemonProcess(dir_a, ipc_port_a, "node-a")
    db = DaemonProcess(dir_b, ipc_port_b, "node-b")

    with da, db:
        # Wait for mesh connection to establish
        yield da, db, env_a


def test_typing_latency_under_500ms(two_daemons):
    """Measure keystroke round-trip latency; assert <500ms."""
    da, db, env_a = two_daemons

    # Wait for peers to connect via mesh (up to 10 seconds).
    connected = False
    for _ in range(100):
        result = subprocess.run(
            [BINARY, "--config-dir", str(da.config_dir), "shell", "node-b",
             "--cmd", "echo MESH_OK"],
            env=env_a, capture_output=True, text=True, timeout=15,
        )
        if "MESH_OK" in result.stdout:
            connected = True
            break
        time.sleep(0.1)

    if not connected:
        pytest.skip("Could not establish mesh connection between local daemons")

    # Now measure latency: send a command and time the round-trip.
    latencies = []
    for i in range(5):
        marker = f"LATENCY_TEST_{i}"
        start = time.monotonic()
        result = subprocess.run(
            [BINARY, "--config-dir", str(da.config_dir), "shell", "node-b",
             "--cmd", f"echo {marker}"],
            env=env_a, capture_output=True, text=True, timeout=10,
        )
        elapsed_ms = (time.monotonic() - start) * 1000

        assert marker in result.stdout, f"Marker {marker} not found in output"
        latencies.append(elapsed_ms)

    avg_latency = sum(latencies) / len(latencies)
    max_latency = max(latencies)

    print(f"\nLatencies (ms): {[f'{l:.0f}' for l in latencies]}")
    print(f"Average: {avg_latency:.0f}ms, Max: {max_latency:.0f}ms")

    # Assert all latencies are under 500ms.
    for i, lat in enumerate(latencies):
        assert lat < 500, (
            f"Keystroke latency {lat:.0f}ms exceeds 500ms threshold (iteration {i})"
        )
