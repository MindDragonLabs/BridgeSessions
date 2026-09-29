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
        # ollama defaults num_ctx to 2048 — silently truncating the 30k-token
        # prompt to nothing, which is how the model ended up hallucinating
        # file contents. 64k covers both focus files + history.
        "options": {"num_ctx": 65536},
    }).encode()
    req = urllib.request.Request(MODEL_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"]


def extract_replacements(text: str) -> list[tuple[str, str, str]]:
    """Parse `replace` blocks: FILE / <<<<<<< old / ======= / >>>>>>> new.

    Tolerant: fence may be replace/diff/patch/empty, marker runs of >=4,
    FILE: spacing loose, markers may appear without a fence at all.
    """
    out: list[tuple[str, str, str]] = []
    for m in re.finditer(
        r"FILE:\s*(\S+)\s*\n<{4,}\s*\n(.*?)\n={4,}\s*\n(.*?)\n>{4,}",
        text, re.S,
    ):
        out.append((m.group(1), m.group(2), m.group(3)))
    return out


def extract_diff(text: str) -> str | None:
    m = re.search(r"```(?:diff|patch)?\n(.*?)```", text, re.S)
    candidate = m.group(1) if m else text
    if "--- " in candidate and "+++ " in candidate and "@@" in candidate:
        return candidate
    return None


# Models hallucinate familiar-but-wrong paths (llama.cpp's src/config.cpp is
# the classic). Redirect known aliases to the real editable file. Shared by
# the replace-block path and the diff path.
REDIRECT = {
    "src/config.cpp": "bs-config.h",
    "config.cpp": "bs-config.h",
    "config.h": "bs-config.h",
    "mesh_config.cpp": "bs-config.h",
    "mesh-config.cpp": "bs-config.h",
    "bridgesessions.cpp": "main.cpp",
    "src/main.cpp": "main.cpp",
    "transfer.cpp": "bs-mesh-transfer.h",
    "transfer.h": "bs-mesh-transfer.h",
    "mesh-transfer.h": "bs-mesh-transfer.h",
}


def redirect_path(path: str) -> str:
    return REDIRECT.get(path, path.lstrip("./"))


def rewrite_diff_paths(diff: str) -> str:
    out = []
    for line in diff.splitlines(keepends=True):
        m = re.match(r"^(\+\+\+ b/|--- a/)(.+)$", line.rstrip("\n"))
        if m:
            line = m.group(1) + redirect_path(m.group(2).strip()) + "\n"
        out.append(line)
    return "".join(out)


def apply_replacements(repls: list[tuple[str, str, str]]) -> tuple[bool, str]:
    for path, old, new in repls:
        path = redirect_path(path)
        if path not in EDITABLE:
            return False, f"scope violation: {path}"
        p = REPO / path
        if not p.exists():
            return False, f"missing file: {path}"
        content = p.read_text()
        n = content.count(old)
        if n != 1:
            return False, f"old block occurs {n}x in {path} (need exactly 1)"
        p.write_text(content.replace(old, new))
    return True, ""


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
- Use ONLY `replace` blocks. NEVER emit unified diffs — they get rejected.
  Format:
  ```replace
  FILE: bs-mesh-transfer.h
  <<<<<<<
  <exact text copied verbatim from the current tree>
  =======
  <replacement text>
  >>>>>>>
  ```
  The old text must appear EXACTLY ONCE in the file. You may emit several
  blocks. The FILE path must be one of exactly these names (no src/ prefix,
  no .cpp renames): {editable}
- Do not touch UI/rendering/TUI code paths; rendering quality must not change.
- Do not weaken security: no removing TLS, hash checks, auth, or pinning.
- Small, surgical changes. One idea per iteration.
- The benchmark runs two daemons on 127.0.0.1 loopback. Throughput on raw
  loopback should reach multiple GiB/s; current bottleneck is inside our
  framing/crypto/chunking pipeline.
""".format(editable=", ".join(EDITABLE))

# Files the model sees in full each iteration. Kept tight on purpose: a 30B
# model with a huge context is slow and loses focus. It can read anything in
# EDITABLE on the next turn by asking — but in practice these two carry the
# transfer hot path and the tunables.
FOCUS_FILES = ["bs-mesh-transfer.h", "bs-config.h"]


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
        for f in FOCUS_FILES:
            p = REPO / f
            if p.exists():
                target_files.append(f"===== {f} =====\n" + p.read_text()[:80000])
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

        repls = extract_replacements(reply)
        if not repls:
            # Replace-blocks only. Diffs from this model class fail at a ~95%
            # rate (context mismatch) even with --recount; force the reliable
            # format instead of accepting a diff fallback.
            log({"event": "no_diff", "iter": it, "reply_head": reply[:300]})
            history.append(
                f"iter {it}: no replace block found. You MUST use the "
                "```replace FILE/<<<<<<< /======= />>>>>>> format — no diffs.")
            continue

        run(["git", "checkout", "--", "."])
        ok, err = apply_replacements(repls)
        if not ok:
            log({"event": "apply_fail", "iter": it, "err": err[:300]})
            history.append(f"iter {it}: replace failed: {err[:150]}")
            continue

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
            cr = run(["git", "commit", "-q", "-m",
                      f"ralph iter {it}: score {best_score:.1f} -> {cs:.1f}"])
            if cr.returncode != 0:
                # Commit failed (e.g. no git identity). Revert and do NOT
                # adopt the phantom baseline.
                log({"event": "commit_fail", "iter": it,
                     "err": (cr.stdout + cr.stderr)[-300:]})
                history.append(f"iter {it}: git commit failed — reverted")
                run(["git", "checkout", "--", "."])
                continue
            log({"event": "accept", "iter": it, "old": best_score, "new": cs,
                 "bench": cand})
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
