"""Run a Unix CI build/test command with a deadline and process diagnostics."""
import argparse
import os
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=1500)
    parser.add_argument("--interval", type=float, default=60)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command or args.timeout <= 0 or args.interval <= 0:
        parser.error("a command and positive timeout/interval are required")
    process = subprocess.Popen(args.command, start_new_session=True)
    started = time.monotonic()

    def stop():
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        finally:
            # The command can exit while one of its children ignores TERM.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()

    def interrupted(number, _frame):
        stop()
        raise SystemExit(128 + number)

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        while True:
            remaining = args.timeout - (time.monotonic() - started)
            if remaining <= 0:
                print("CI test command exceeded its deadline", flush=True)
                stop()
                return 124
            try:
                result = process.wait(timeout=min(args.interval, remaining))
                print(f"CI test command finished: exit={result}, "
                      f"elapsed={time.monotonic() - started:.0f}s", flush=True)
                return result if result >= 0 else 128 - result
            except subprocess.TimeoutExpired:
                elapsed = time.monotonic() - started
                print(f"CI test command still running after {elapsed:.0f}s", flush=True)
                # comm excludes command arguments, which can contain secrets.
                snapshot = subprocess.run(
                    ["ps", "-axo", "pid,ppid,state,etime,comm"],
                    text=True, capture_output=True, timeout=10,
                )
                for line in snapshot.stdout.splitlines():
                    if any(name in line for name in (
                        "test_", "ctest", "cmake", "clang", "cc1", "g++",
                        "python", "ninja", "make", "panel_session_acceptance",
                    )):
                        print(line, flush=True)
    finally:
        stop()


if __name__ == "__main__":
    raise SystemExit(main())
