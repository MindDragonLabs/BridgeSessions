#!/usr/bin/env python3
"""ralph-loop.py — autonomous perf-optimization loop for BridgeSessions.

Driver: local model served by llama.cpp (OpenAI-compatible /v1/chat/completions).
Each iteration: propose a unified diff against the current best tree, build,
benchmark, accept/reject, log.

Hard rules (enforced by the harness, not the model):
  - Model may only touch files in EDITABLE (transfer/IO/constants). UI/render
    code is out of scope, so "rendering stays good" is structural.
  - A candidate must build clean and pass bench with the acceptance rule:
    >=3% improvement in score AND no metric worse than 5% regression.
  - Every ACCEPT_EVERY-th accepted candidate runs full ctest; failure reverts.
  - Everything is logged to loop-log.jsonl; best tree lives on branch
    ralph-best.

Score: file throughput (higher better) and latency (lower better) folded into
one number; see score().
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = Path(os.environ.get("RALPH_REPO", "/root/bridgesessions"))
MODEL_URL = os.environ.get("RALPH_MODEL_URL", "http://127.0.0.1:8080/v1/chat/completions")
MODEL_NAME = os.environ.get("RALPH_MODEL", "local")
LOG = Path(os.environ.get("RALPH_LOG", "/root/loop-log.jsonl"))
BENCH_OUT = Path("/tmp/ralph-bench.json")
ACCEPT_EVERY = int(os.environ.get("RALPH_CTEST_EVERY", "3"))
MAX_ITERS = int(os.environ.get("RALPH_ITERS", "100000"))

EDITABLE = [
    "bs-mesh-transfer.h",
    "bs-mesh-conn.h",
    "bs-codec.h",
    "bs-protocol.h",
    "bs-config.h",
    "bs-mesh-cli.h",
    "bs-mesh-controller.h",
    "bs-mesh-support.h",
    "main.cpp",
]

QUICK_BENCH = ["--sizes", "64", "--reps", "3", "--keystrokes", "30", "--cmd-reps", "6"]


def run(cmd, cwd=REPO, timeout=900, env=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)


def log(entry: dict):
    entry["ts"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    with LOG.open("a") as fh:
        fh.write(json.dumps(entry) + "\n")


def score(bench: dict) -> float | None:
    """Higher is better. Throughput in MiB/s minus latency penalties."""
    try:
        f = bench["file_mib_s"]
        thr = sum(v["mib_s"] for v in f.values()) / len(f)
        cmd = bench["cmd_ms"]["p50_ms"]
        key = bench["key_ms"]["p50_ms"]
        return thr - 0.5 * cmd - 0.2 * key
    except Exception:
        return None


def ask_model(messages: list[dict], max_tokens: int = 4096) -> str:
    body = json.dumps({
        "model": MODEL_NAME,
        "messages": messages,
        "temperature": 0.2,
        "max_tokens": max_tokens,
    }).encode()
    req = urllib.request.Request(MODEL_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"]


def extract_diff(text: str) -> str | None:
    m = re.search(r"```(?:diff|patch)?\n(.*?)```", text, re.S)
    candidate = m.group(1) if m else text
    if "--- " in candidate and "+++ " in candidate and "@@" in candidate:
        return candidate
    return None


def touched_files(diff: str) -> list[str]:
    files = []
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            files.append(line[6:].strip())
    return files


def build() -> tuple[bool, str]:
    r = run(["cmake", "--build", "build", "-j", str(os.cpu_count() or 8),
             "--target", "bridgesessions"], timeout=1200)
    if r.returncode != 0:
        return False, (r.stdout + r.stderr)[-3000:]
    return True, ""


def bench() -> dict | None:
    r = run(["python3", "scripts/bench-perf.py", "--bin", "./build/bridgesessions",
             "--out", str(BENCH_OUT), *QUICK_BENCH], timeout=900)
    if r.returncode != 0 or not BENCH_OUT.exists():
        return None
    return json.loads(BENCH_OUT.read_text())


def ctest() -> tuple[bool, str]:
    r = run(["ctest", "--test-dir", "build", "-j", str(os.cpu_count() or 8),
             "--output-on-failure"], timeout=2400)
    return r.returncode == 0, (r.stdout + r.stderr)[-2000:]


SYSTEM = """You are optimizing the BridgeSessions mesh daemon (C++20) for speed.
Goals, in order:
1. Higher file-transfer throughput (file_mib_s, MiB/s — higher is better).
2. Lower warm command latency (cmd_ms p50 — lower is better).
3. Lower keystroke echo latency (key_ms p50 — lower is better).

Hard constraints:
- Output ONE unified diff (```diff fenced) against the current tree. Nothing else.
- Only touch these files: {editable}
- Do not touch UI/rendering/TUI code paths; rendering quality must not change.
- Do not weaken security: no removing TLS, hash checks, auth, or pinning.
- Small, surgical changes. One idea per diff.
- The benchmark runs two daemons on 127.0.0.1 loopback. Throughput on raw
  loopback should reach multiple GiB/s; current bottleneck is inside our
  framing/crypto/chunking pipeline.
""".format(editable=", ".join(EDITABLE))


def main() -> int:
    os.chdir(REPO)
    run(["git", "checkout", "-B", "ralph-best"])
    log({"event": "loop_start"})

    ok, err = build()
    if not ok:
        print("baseline build failed:", err[:500])
        return 1
    base = bench()
    if not base or score(base) is None:
        print("baseline bench failed")
        return 1
    best_score = score(base)
    assert best_score is not None
    log({"event": "baseline", "score": best_score, "bench": base})
    print(f"baseline score {best_score:.1f}")

    history: list[str] = []
    accepted = 0
    it = 0
    while it < MAX_ITERS:
        it += 1
        target_files = []
        for f in EDITABLE:
            p = REPO / f
            if p.exists():
                target_files.append(f"===== {f} =====\n" + p.read_text()[:60000])
        context = "\n\n".join(target_files)
        user_msg = (
            f"Current best score: {best_score:.1f}\n"
            f"Latest bench: {json.dumps(base)[:1500]}\n"
            f"Recent attempts (avoid repeating failures):\n" + "\n---\n".join(history[-6:])
            + "\n\nCurrent tree (only files you may edit):\n" + context
            + "\n\nPropose the next single optimization as a unified diff."
        )
        try:
            reply = ask_model([
                {"role": "system", "content": SYSTEM},
                {"role": "user", "content": user_msg},
            ])
        except Exception as e:  # noqa: BLE001
            log({"event": "model_error", "iter": it, "error": str(e)[:300]})
            time.sleep(30)
            continue

        diff = extract_diff(reply)
        if not diff:
            log({"event": "no_diff", "iter": it})
            history.append(f"iter {it}: model produced no parseable diff")
            continue

        bad = [f for f in touched_files(diff) if f not in EDITABLE]
        if bad:
            log({"event": "scope_violation", "iter": it, "files": bad})
            history.append(f"iter {it}: tried to touch {bad} — rejected")
            continue

        run(["git", "checkout", "--", "."])
        ap = subprocess.run(["git", "apply", "--check", "-"], input=diff,
                            cwd=REPO, capture_output=True, text=True)
        if ap.returncode != 0:
            log({"event": "apply_fail", "iter": it, "err": ap.stderr[-300:]})
            history.append(f"iter {it}: diff did not apply: {ap.stderr[-120:]}")
            continue
        subprocess.run(["git", "apply", "-"], input=diff, cwd=REPO,
                       capture_output=True, text=True)

        ok, build_err = build()
        if not ok:
            log({"event": "build_fail", "iter": it, "err": build_err[-400:]})
            history.append(f"iter {it}: build failed: {build_err[-120:]}")
            run(["git", "checkout", "--", "."])
            continue

        cand = bench()
        cs = score(cand) if cand else None
        if cand is None or cs is None:
            log({"event": "bench_fail", "iter": it})
            history.append(f"iter {it}: benchmark failed")
            run(["git", "checkout", "--", "."])
            continue

        improved = cs >= best_score * 1.03
        no_regress = True
        for k in cand["file_mib_s"]:
            base_v = base["file_mib_s"].get(k, {}).get("mib_s")
            if base_v and cand["file_mib_s"][k]["mib_s"] < base_v * 0.95:
                no_regress = False
        if cand["cmd_ms"]["p50_ms"] > base["cmd_ms"]["p50_ms"] * 1.05:
            no_regress = False
        if cand["key_ms"]["p50_ms"] > base["key_ms"]["p50_ms"] * 1.05 + 1.0:
            no_regress = False

        if improved and no_regress:
            # periodic full-test gate
            accepted += 1
            if accepted % ACCEPT_EVERY == 0:
                ok, terr = ctest()
                if not ok:
                    log({"event": "ctest_fail", "iter": it, "err": terr[-300:]})
                    history.append(f"iter {it}: ctest failed after accept — reverted")
                    run(["git", "checkout", "--", "."])
                    continue
            run(["git", "add", "-A"])
            run(["git", "commit", "-q", "-m",
                 f"ralph iter {it}: score {best_score:.1f} -> {cs:.1f}"])
            log({"event": "accept", "iter": it, "old": best_score, "new": cs,
                 "bench": cand, "diff_head": diff[:400]})
            best_score = cs
            base = cand
            history.append(f"iter {it}: ACCEPTED score {cs:.1f}")
            print(f"iter {it}: ACCEPT {cs:.1f}")
        else:
            log({"event": "reject", "iter": it, "score": cs, "best": best_score})
            history.append(f"iter {it}: rejected (score {cs:.1f} vs best {best_score:.1f})")
            run(["git", "checkout", "--", "."])

    return 0


if __name__ == "__main__":
    sys.exit(main())
