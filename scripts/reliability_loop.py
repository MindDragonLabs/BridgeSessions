#!/usr/bin/env python3
"""Run the BridgeSessions reliability lanes with reproducible evidence.

The loop intentionally coordinates verification; it does not edit source or
auto-merge changes. Each lane gets an isolated log and JSON result so local,
CI, and disposable cloud workers can use the same contract.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def run_lane(name: str, command: list[str], cwd: Path, env: dict[str, str],
             output_dir: Path, timeout: int | None) -> dict[str, object]:
    started = time.monotonic()
    log_path = output_dir / f"{name}.log"
    output_dir.mkdir(parents=True, exist_ok=True)
    record: dict[str, object] = {
        "lane": name,
        "command": command,
        "started_at": utc_now(),
        "log": str(log_path),
    }
    try:
        with log_path.open("w", encoding="utf-8") as log:
            log.write("$ " + " ".join(command) + "\n\n")
            completed = subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                check=False,
            )
        record["returncode"] = completed.returncode
        record["status"] = "PASS" if completed.returncode == 0 else "FAIL"
    except subprocess.TimeoutExpired:
        with log_path.open("a", encoding="utf-8") as log:
            log.write(f"\nTIMEOUT after {timeout}s\n")
        record["returncode"] = None
        record["status"] = "TIMEOUT"
    except OSError as exc:
        with log_path.open("a", encoding="utf-8") as log:
            log.write(f"\nEXECUTION ERROR: {exc}\n")
        record["returncode"] = None
        record["status"] = "ERROR"
    record["duration_seconds"] = round(time.monotonic() - started, 3)
    record["finished_at"] = utc_now()
    (output_dir / f"{name}.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8"
    )
    return record


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=1,
                        help="number of verification iterations (default: 1)")
    parser.add_argument("--sleep-seconds", type=float, default=0,
                        help="pause between iterations")
    parser.add_argument("--build-dir", type=Path, default=Path("build-reliability"))
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts/reliability"))
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--timeout", type=int, default=900,
                        help="per-lane timeout in seconds")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-python", action="store_true")
    parser.add_argument("--no-panel", action="store_true",
                        help="skip the BridgePanel Python lane")
    parser.add_argument("--keep-going", action="store_true",
                        help="run all iterations after a failure")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.iterations < 1 or args.jobs < 1:
        raise SystemExit("--iterations and --jobs must be positive")

    root = Path(__file__).resolve().parents[1]
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    artifact_root = args.artifacts if args.artifacts.is_absolute() else root / args.artifacts
    build_dir.mkdir(parents=True, exist_ok=True)
    artifact_root.mkdir(parents=True, exist_ok=True)

    base_env = os.environ.copy()
    base_env["PYTHONPATH"] = str(root) + os.pathsep + base_env.get("PYTHONPATH", "")
    base_env["BRIDGESESSIONS_BINARY"] = str(build_dir / "bridgesessions")
    all_records: list[dict[str, object]] = []

    if not args.no_build:
        build_output_dir = artifact_root / "build"
        configure = [
            "cmake", "-S", str(root), "-B", str(build_dir),
            "-DBUILD_TESTING=ON", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        ]
        build = ["cmake", "--build", str(build_dir), "--parallel", str(args.jobs)]
        for name, command in (("configure", configure), ("build", build)):
            record = run_lane(name, command, root, base_env, build_output_dir, args.timeout)
            all_records.append(record)
            print(f"{record['status']:7} {name} ({record['duration_seconds']}s)")
            if record["status"] != "PASS":
                summary = {
                    "started_at": all_records[0].get("started_at"),
                    "finished_at": utc_now(),
                    "root": str(root),
                    "build_dir": str(build_dir),
                    "results": all_records,
                }
                (artifact_root / "summary.json").write_text(
                    json.dumps(summary, indent=2) + "\n", encoding="utf-8"
                )
                return 1

    for iteration in range(1, args.iterations + 1):
        iteration_dir = artifact_root / f"iteration-{iteration:03d}"
        iteration_dir.mkdir(parents=True, exist_ok=True)
        print(f"== reliability iteration {iteration}/{args.iterations} ==")

        parallel_lanes: list[tuple[str, list[str]]] = [
            ("ctest-core", [
                "ctest", "--test-dir", str(build_dir), "--output-on-failure",
                "-E", "^(panel_|release_python_tests$)",
            ]),
        ]
        serialized_lanes: list[tuple[str, list[str]]] = []
        if not args.no_python:
            if shutil.which(sys.executable) is None:
                raise SystemExit(f"python executable not found: {sys.executable}")
            parallel_lanes.append(("python-release", [
                sys.executable, "-m", "pytest", "-q",
                "tests/test_release.py", "tests/test_install_script.py",
                "tests/test_regression_install.py",
            ]))
            if not args.no_panel:
                # These tests launch real daemons and use shared local IPC and
                # identity state. Keep them out of the core wave so the
                # coordinator does not manufacture false readiness failures.
                serialized_lanes.append(("python-panel", [
                    sys.executable, "-m", "pytest", "-q",
                    "tools/bridgepanel", "tests/test_typing_latency.py",
                ]))

        def run_wave(lanes: list[tuple[str, list[str]]]) -> None:
            if not lanes:
                return
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(lanes)) as pool:
                futures = [
                    pool.submit(run_lane, name, command, root, base_env,
                                iteration_dir, args.timeout)
                    for name, command in lanes
                ]
                for future in futures:
                    record = future.result()
                    all_records.append(record)
                    print(f"{record['status']:7} {record['lane']} ({record['duration_seconds']}s)")

        run_wave(parallel_lanes)
        # Run daemon-backed tests after the parallel wave for deterministic
        # local resources. This is still parallel across independent remote
        # workers when each worker owns its own machine/worktree.
        run_wave(serialized_lanes)

        failed = [r for r in all_records if r.get("status") != "PASS"
                  and str(r.get("lane", "")).startswith(("configure", "build", "ctest", "python"))]
        if failed and not args.keep_going:
            break
        if iteration != args.iterations and args.sleep_seconds:
            time.sleep(args.sleep_seconds)

    summary = {
        "started_at": all_records[0].get("started_at") if all_records else utc_now(),
        "finished_at": utc_now(),
        "root": str(root),
        "build_dir": str(build_dir),
        "results": all_records,
    }
    (artifact_root / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    failures = [r for r in all_records if r.get("status") != "PASS"]
    print(f"summary: {len(all_records) - len(failures)} passed, {len(failures)} failed")
    print(f"evidence: {artifact_root / 'summary.json'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
