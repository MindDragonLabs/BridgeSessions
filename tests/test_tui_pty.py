#!/usr/bin/env python3
"""Exercise actual CLI menus in a PTY using only Python's standard library.

Usage: python3 tests/test_tui_pty.py /absolute/path/to/bridgesessions
--menus-only runs the socket-free geometry/input suite. The default also starts
one isolated foreground daemon, creates three bounded test sessions, and proves
repeated delete/selection. A socket denial is a failure, never a skipped pass.
"""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata


def environment(home, ipc_port=1):
    # Do not inherit operator session/jail/mesh overrides or profile settings.
    return {"HOME": str(home), "PATH": "/usr/bin:/bin", "TERM": "xterm-256color",
            "LC_ALL": "C.UTF-8", "BRIDGESESSIONS_IPC_PORT": str(ipc_port),
            "XDG_CONFIG_HOME": str(home / "xdg"), "XDG_RUNTIME_DIR": str(home)}


def set_size(fd, columns, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))


class Screen:
    """Small independent VT oracle for the CSI vocabulary emitted by menus.

    Cell widths use Python's Unicode database, not the production scanner.
    The geometry corpus deliberately uses ASCII, accents, combining and CJK;
    C++ fixtures separately cover the terminal emoji cluster policy.
    """
    def __init__(self, columns, rows):
        self.columns, self.rows = columns, rows
        self.grid = [[(" ", False)] * columns for _ in range(rows)]
        self.x = self.y = 0
        self.reverse = False
        self.wraps = self.scrolls = self.clamps = 0

    def feed(self, raw):
        text = raw.decode("utf-8", errors="strict")
        pos = 0
        while pos < len(text):
            char = text[pos]
            if char == "\x1b":
                match = re.match(r"\x1b\[([0-?]*)([ -/]*)([@-~])", text[pos:])
                assert match, ("unexpected terminal sequence", text[pos:pos + 30])
                params, intermediate, final = match.groups()
                assert not intermediate
                numbers = [int(n or "0") for n in params.lstrip("?").split(";")]
                amount = numbers[0] or 1
                if final == "m":
                    for n in numbers:
                        if n == 0: self.reverse = False
                        if n == 7: self.reverse = True
                elif final == "J":
                    assert numbers == [2]
                    self.grid = [[(" ", False)] * self.columns for _ in range(self.rows)]
                elif final in ("H", "f"):
                    self.y = (numbers[0] or 1) - 1
                    self.x = (numbers[1] if len(numbers) > 1 else 1) - 1
                elif final == "A":
                    if amount > self.y: self.clamps += 1
                    self.y = max(0, self.y - amount)
                elif final == "K":
                    assert numbers == [2]
                    self.grid[self.y] = [(" ", False)] * self.columns
                elif final in ("h", "l"):
                    assert params == "?25"
                else:
                    raise AssertionError(("unexpected CSI", match.group()))
                pos += len(match.group())
                continue
            if char == "\r": self.x = 0
            elif char == "\n":
                self.y += 1
                if self.y >= self.rows:
                    self.scrolls += 1
                    self.grid.pop(0)
                    self.grid.append([(" ", False)] * self.columns)
                    self.y = self.rows - 1
            else:
                assert ord(char) >= 32 and ord(char) != 127, ("control injection", repr(char))
                if unicodedata.combining(char) or unicodedata.category(char) in ("Mn", "Me"):
                    if self.x: self.grid[self.y][self.x - 1] = (self.grid[self.y][self.x - 1][0] + char, self.reverse)
                    pos += 1
                    continue
                width = 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
                if self.x + width > self.columns:
                    self.wraps += 1
                    self.x = 0
                    self.y += 1
                    assert self.y < self.rows, "menu overflowed the terminal height"
                self.grid[self.y][self.x] = (char, self.reverse)
                if width == 2: self.grid[self.y][self.x + 1] = ("", self.reverse)
                self.x += width
            pos += 1
        return self

    def selected(self):
        return ["".join(char for char, _ in row).strip()
                for row in self.grid if any(reverse for _, reverse in row)]

    def assert_geometry(self):
        assert (self.wraps, self.scrolls, self.clamps) == (0, 0, 0), (
            "menu wrapped, scrolled or clamped", self.wraps, self.scrolls, self.clamps)
        selected = self.selected()
        assert len(selected) == 1, ("expected one selected row", selected)
        borders = [i for i, row in enumerate(self.grid) if row[0][0] in ("╭", "│", "├", "╰")]
        if borders:
            ends = [max(i for i, (char, _) in enumerate(self.grid[y]) if char.strip()) for y in borders]
            assert len(set(ends)) == 1, ("misaligned borders", ends)


class Menu:
    def __init__(self, binary, home, args, columns, rows, env=None):
        self.columns, self.rows = columns, rows
        self.master, self.slave = pty.openpty()
        set_size(self.slave, columns, rows)
        self.saved = termios.tcgetattr(self.slave)
        self.output = bytearray()
        self.screen_start = 0
        def child_terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        self.process = subprocess.Popen(
            [binary, "--config-dir", str(home), *args], stdin=self.slave,
            stdout=self.slave, stderr=self.slave, env=env or environment(home),
            preexec_fn=child_terminal)

    def read(self, timeout=0.05):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                self.output.extend(os.read(self.master, 65536))
            except OSError as exc:
                if exc.errno != errno.EIO: raise

    def until(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read()
            if predicate(bytes(self.output)): return
            if self.process.poll() is not None:
                raise AssertionError(("binary exited before expected output", self.process.returncode, bytes(self.output)[-1000:]))
        raise AssertionError(("timed out waiting for menu", bytes(self.output)[-1000:]))

    def ready(self, compact=False):
        self.until(lambda raw: b"\x1b[?25l" in raw and (b"\xe2\x95\xb0" in raw if not compact else b"\x1b[H" in raw and len(raw.split(b"\x1b[H")[-1]) > 0))
        self.read(0.05)

    def screen(self):
        return Screen(self.columns, self.rows).feed(bytes(self.output[self.screen_start:]))

    def key(self, data, repaint=True):
        before = len(self.output)
        os.write(self.master, data)
        if repaint:
            self.until(lambda raw: len(raw) > before and (raw[before:].endswith(b"\x1b[0m") or b"\x1b[H" in raw[before:]))
            self.read(0.05)

    def resize(self, columns, rows):
        self.screen_start = len(self.output)
        self.columns, self.rows = columns, rows
        set_size(self.slave, columns, rows)
        # No key is sent: idle resize must repaint on its own.
        self.until(lambda raw: b"\x1b[2J\x1b[H" in raw[self.screen_start:])
        self.read(0.05)

    def finish(self, key=b"q", expected=2):
        self.key(key, repaint=False)
        self.until(lambda _: self.process.poll() is not None)
        self.read(0)
        assert self.process.returncode == expected, (self.process.returncode, bytes(self.output)[-1000:])
        assert b"\x1b[?25h" in self.output, "cursor was not restored"
        # 26.10.04: termios restoration is asserted on the SLAVE fd, and macOS
        # revokes that fd the moment the child exits - tcgetattr then raises
        # instead of returning the attributes, so the check failed on a restore
        # that had actually succeeded. The comparison is only meaningful while
        # the fd is still a tty; on a platform that has already torn it down,
        # fall back to the observable proxy (the cursor-show sequence above).
        #
        # The errno must be read from .args[0], NOT from .errno: on macOS
        # termios.error is a distinct OSError subclass that does not populate
        # the errno attribute, so getattr(exc, "errno", None) is always None and
        # a guard written against .errno re-raises. Both ENOTTY ("Inappropriate
        # ioctl for device") and EBADF ("Bad file descriptor") are observed here
        # depending on how far the teardown has got; both mean the fd is gone
        # and there is nothing left to inspect.
        restored = None
        try:
            restored = termios.tcgetattr(self.slave) == self.saved
        except termios.error as exc:
            code = exc.args[0] if exc.args else None
            if code not in (errno.ENOTTY, errno.EBADF):
                raise
        if restored is not None:
            assert restored, "termios was not restored"

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
            try: self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=2)
        os.close(self.master)
        os.close(self.slave)

    def __enter__(self): return self
    def __exit__(self, *_): self.close()


def menu_cases(binary, root):
    home = root / "menus"
    home.mkdir()
    labels = ["a-é", "b-e\u0301", "c-名称", *[f"peer-{i:02}" for i in range(25)]]
    (home / "config").write_text("node.name tui-pty-probe\n" + "".join(
        f"seed {name} 127.0.0.1:1\n" for name in labels), encoding="utf-8")
    count = 0
    for cols, rows in [(60, 10), (20, 6), (6, 6), (5, 8), (1, 1), (40, 5)]:
        compact = cols < 6 or rows < 6
        with Menu(binary, home, ["connect"], cols, rows) as menu:
            menu.ready(compact)
            menu.screen().assert_geometry()
            menu.key(b"\x1b[F")
            menu.screen().assert_geometry()
            if not compact:
                screen = menu.screen()
                screen.assert_geometry()
                if cols >= 14: assert "peer-24" in screen.selected()[0], screen.selected()
                menu.key(b"\x1b[H")
                menu.key(b"\x1b[B")
                menu.key(b"\x1b[A")
                menu.key(b"j")
                menu.key(b"k")
                menu.screen().assert_geometry()
            menu.finish()
        count += 1
    for cancel in (b"\x1b", b"\x03"):
        with Menu(binary, home, ["connect"], 60, 10) as menu:
            menu.ready()
            menu.finish(cancel)
        count += 1
    with Menu(binary, home, ["connect"], 80, 12) as menu:
        menu.ready()
        menu.key(b"\x1b[F")
        for cols, rows in [(10, 4), (20, 6), (80, 12)]:
            menu.resize(cols, rows)
            if rows >= 6:
                screen = menu.screen()
                screen.assert_geometry()
                assert "peer-24" in screen.selected()[0], screen.selected()
        menu.finish()
    count += 1
    # Enter on the CJK peer proves arrows select the expected original index.
    with Menu(binary, home, ["connect"], 60, 10) as menu:
        menu.ready()
        menu.key(b"jj")
        menu.key(b"\r", repaint=False)
        menu.until(lambda raw: "c-名称".encode() in raw and b"hermes" in raw)
        menu.screen_start = bytes(menu.output).rfind(b"\x1b[2J\x1b[H")
        menu.screen().assert_geometry()
        menu.finish()
    return count + 1


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def command(binary, home, env, args, check=True):
    result = subprocess.run([binary, "--config-dir", str(home), *args], env=env,
                            capture_output=True, timeout=15)
    if check: assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    return result


def daemon_case(binary, root):
    # Every IPC and mesh port belongs to this throwaway fixture. No connection
    # to the production default IPC port, config, HOME or session names.
    ports = set()
    while len(ports) < 4: ports.add(free_port())
    server_mesh, client_mesh, server_ipc, client_ipc = ports
    server, client = root / "server", root / "client"
    server.mkdir(); client.mkdir()
    env_server, env_client = environment(server, server_ipc), environment(client, client_ipc)
    keys = {}
    for name, home, env in [("pty-server", server, env_server), ("pty-client", client, env_client)]:
        result = command(binary, home, env, ["keygen"])
        keys[name] = re.search(rb"Pubkey hex:\s+([0-9a-f]{64})", result.stdout).group(1).decode()
    for name, home, mesh, other, other_mesh in [
            ("pty-server", server, server_mesh, "pty-client", client_mesh),
            ("pty-client", client, client_mesh, "pty-server", server_mesh)]:
        (home / "config").write_text(
            f"node.name {name}\nnode.listen 127.0.0.1:{mesh}\n"
            f"seed {other} 127.0.0.1:{other_mesh} pubkey={keys[other]}\n"
            "mesh.mdns_enabled false\nmesh.startup_wait_secs 0\nmesh.gossip_interval_secs 300\n")
        (home / "authorized_keys").write_text(keys[other] + "\n")
    log = open(root / "daemon.log", "w+b")
    daemon = subprocess.Popen([binary, "--config-dir", str(server)], env=env_server,
                              stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    try:
        deadline = time.monotonic() + 10
        while True:
            if daemon.poll() is not None:
                log.seek(0); raise AssertionError(("isolated daemon exited", log.read()))
            try:
                with socket.create_connection(("127.0.0.1", server_ipc), timeout=.2): break
            except OSError:
                assert time.monotonic() < deadline, "isolated daemon did not bind IPC"
                time.sleep(.05)
        for suffix in ("a", "b", "c"):
            # Bounded lifetime is an additional cleanup backstop for workers.
            launch = f"i=0; while [ $i -lt 45 ]; do printf 'PTY_SESSION_{suffix}\\n'; sleep 1; i=$((i+1)); done"
            command(binary, client, env_client, ["shell", "pty-server", "-n", f"tui-{suffix}", "-x", launch, "--detach"])
        with Menu(binary, client, ["pty-server", "-s"], 60, 9, env_client) as menu:
            menu.ready()
            screen = menu.screen()
            screen.assert_geometry()
            # The registry's iteration order is deliberately unspecified.
            order = []
            for row in screen.grid:
                match = re.search(r"tui-[abc]", "".join(char for char, _ in row))
                if match: order.append(match.group())
            assert len(order) == 3 and set(order) == {"tui-a", "tui-b", "tui-c"}, order
            menu.key(b"j") # New -> first original session row
            menu.key(b"d")
            menu.until(lambda raw: ("killed " + order[0]).encode() in raw)
            menu.key(b"d") # Delete the next row, using its original index.
            menu.until(lambda raw: ("killed " + order[1]).encode() in raw)
            latest = bytes(menu.output).rfind(b"\x1b[2J\x1b[H")
            menu.screen_start = latest
            menu.screen().assert_geometry()
            assert order[2] in menu.screen().selected()[0]
            menu.key(b"\r", repaint=False)
            menu.until(lambda raw: ("PTY_SESSION_" + order[2][-1]).encode() in raw)
            menu.finish(b"\x04", expected=0)
        listed = command(binary, client, env_client, ["sessions", "pty-server"]).stdout
        assert order[2].encode() in listed and all(name.encode() not in listed for name in order[:2]), listed
    finally:
        if daemon.poll() is None:
            # Only the fixture daemon and its fixture sessions are addressed.
            try:
                command(binary, server, env_server, ["sessions", "--kill-all"], check=False)
            finally:
                if daemon.poll() is None: os.killpg(daemon.pid, signal.SIGTERM)
                try: daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(daemon.pid, signal.SIGKILL); daemon.wait(timeout=2)
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default=os.environ.get("BRIDGESESSIONS_BINARY"))
    parser.add_argument("--menus-only", action="store_true", help="run only the 10 socket-free menu cases")
    args = parser.parse_args()
    assert args.binary and Path(args.binary).is_file(), "pass an actual built binary"
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="bs-tui-", dir="/tmp") as tmp:
        root = Path(tmp)
        count = menu_cases(binary, root)
        print(f"PASS: {count} real-binary menu PTY cases", flush=True)
        if not args.menus_only:
            try: daemon_case(binary, root)
            except PermissionError as exc:
                raise SystemExit(f"BLOCKED: isolated daemon PTY proof requires socket permission: {exc}") from exc
            print("PASS: isolated daemon repeated-delete/selection PTY case")


if __name__ == "__main__":
    main()
