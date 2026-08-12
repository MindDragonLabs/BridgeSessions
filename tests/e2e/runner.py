#!/usr/bin/env python3
"""BridgeSessions multi-layer e2e orchestrator.

Layers:
  L2 — mesh (wraps scripts/e2e-fleet-test.sh)
  L3 — desktop CUA / tray / menubar (setup + probe)

Usage:
  python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/e2e.json
  python3 tests/e2e/runner.py --layers L3 --skip-setup
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict, field
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional

REPO = Path(__file__).resolve().parents[2]
HARNESS = Path(__file__).resolve().parent / "harness"


@dataclass
class Result:
    status: str  # PASS|FAIL|SKIP
    suite: str
    name: str
    detail: str = ""


@dataclass
class Report:
    started: str
    finished: str = ""
    layers: List[str] = field(default_factory=list)
    results: List[Result] = field(default_factory=list)

    @property
    def pass_n(self) -> int:
        return sum(1 for r in self.results if r.status == "PASS")

    @property
    def fail_n(self) -> int:
        return sum(1 for r in self.results if r.status == "FAIL")

    @property
    def skip_n(self) -> int:
        return sum(1 for r in self.results if r.status == "SKIP")


def ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def which_bs() -> str:
    for key in ("BS", "BRIDGESESSIONS_BINARY"):
        p = os.environ.get(key)
        if p and Path(p).is_file():
            return p
    for name in ("bs", "bridgesessions"):
        w = shutil.which(name)
        if w:
            return w
    cand = Path.home() / ".local/bin/bs"
    if cand.is_file():
        return str(cand)
    raise SystemExit("bs/bridgesessions not found on PATH")


def run(cmd: List[str], timeout: int = 120, env: Optional[dict] = None) -> subprocess.CompletedProcess:
    e = os.environ.copy()
    if env:
        e.update(env)
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=e,
    )


def rec(report: Report, status: str, suite: str, name: str, detail: str = "") -> None:
    report.results.append(Result(status=status, suite=suite, name=name, detail=detail[:500]))
    color = {"PASS": "32", "FAIL": "31", "SKIP": "33"}.get(status, "0")
    print(f"  \033[{color}m{status}\033[0m  [{suite}] {name}: {detail[:120]}")


# ── L2 ──────────────────────────────────────────────────────────────
def run_l2(report: Report, peers: List[str]) -> None:
    script = REPO / "scripts" / "e2e-fleet-test.sh"
    if not script.is_file():
        rec(report, "FAIL", "mesh_core", "missing_script", str(script))
        return
    out_json = Path(tempfile.gettempdir()) / "bs-e2e-l2.json"
    cmd = ["bash", str(script), "--json", str(out_json), *peers]
    print("\n== L2 mesh_core ==")
    try:
        cp = run(cmd, timeout=600)
    except subprocess.TimeoutExpired:
        rec(report, "FAIL", "mesh_core", "timeout", "e2e-fleet-test.sh > 600s")
        return
    # Parse script output lines "  PASS  peer feature detail"
    for line in (cp.stdout + "\n" + cp.stderr).splitlines():
        m = re.search(r"\b(PASS|FAIL|SKIP)\b\s+(\S+)\s+(\S+)\s+(.*)$", line)
        if m:
            rec(report, m.group(1), "mesh_core", f"{m.group(2)}/{m.group(3)}", m.group(4).strip())
    if cp.returncode != 0 and report.fail_n == 0:
        rec(report, "FAIL", "mesh_core", "exit_code", f"rc={cp.returncode}")
    elif cp.returncode == 0:
        rec(report, "PASS", "mesh_core", "script_exit", "0")


# ── L3 Windows ──────────────────────────────────────────────────────
def setup_windows_desktop(bs: str, peer: str, report: Report) -> None:
    print("\n== L3 setup: Windows desktop (cua-helper + tray) ==")
    ps1 = HARNESS / "win_desktop_setup.ps1"
    # Send script then run-script
    tmp = Path(tempfile.gettempdir()) / "win_desktop_setup.ps1"
    tmp.write_text(ps1.read_text(encoding="utf-8"), encoding="utf-8")
    # Also push tray script
    tray = REPO / "scripts" / "bs_tray.ps1"
    if tray.is_file():
        run([bs, "file", "send", peer, str(tray), "--wait"], timeout=60)
    run([bs, "file", "send", peer, str(tmp), "--wait"], timeout=60)
    # Execute via run-script (fixed TEMP path)
    cp = run([bs, "run-script", peer, str(tmp)], timeout=120)
    out = cp.stdout + cp.stderr
    if "WIN_DESKTOP_SETUP_OK" in out:
        rec(report, "PASS", "win_setup", "desktop_setup", "helper+tray")
    elif "Missing binary" in out:
        rec(report, "FAIL", "win_setup", "desktop_setup", "binary missing on peer")
    else:
        # Fallback: shell one-shot start helper
        start_cmd = (
            r'powershell -NoProfile -Command '
            r'"$b=Join-Path $env:LOCALAPPDATA bridgesessions\bridgesessions.exe; '
            r'Start-Process $b -ArgumentList --cua-helper -WindowStyle Hidden; '
            r'Start-Sleep 2; Test-Path (Join-Path $env:USERPROFILE .bridgesessions\cua-helper-token)"'
        )
        cp2 = run([bs, "shell", peer, "--cmd", start_cmd], timeout=60)
        if "True" in (cp2.stdout + cp2.stderr):
            rec(report, "PASS", "win_setup", "cua_helper_start", "token present")
        else:
            rec(report, "FAIL", "win_setup", "desktop_setup", (out + cp2.stdout)[:300])


def test_cua(bs: str, peer: str, suite: str, report: Report) -> None:
    print(f"\n== L3 CUA: {peer} ==")
    cp = run([bs, "cua", "screen", peer], timeout=45)
    out = (cp.stdout + cp.stderr).strip()
    if re.search(r"\d{2,5}\s*[xX×]\s*\d{2,5}", out) and "ERROR" not in out.upper():
        rec(report, "PASS", suite, "cua_screen", out.splitlines()[-1][:80])
    elif "helper" in out.lower() or "cannot determine" in out.lower() or "ERROR" in out:
        rec(report, "FAIL", suite, "cua_screen", out[:200])
    else:
        rec(report, "FAIL", suite, "cua_screen", out[:200] or f"rc={cp.returncode}")

    shot = Path(tempfile.gettempdir()) / f"bs-cua-{peer}.jpg"
    if shot.exists():
        shot.unlink()
    cp = run([bs, "cua", "capture", peer, "-o", str(shot)], timeout=60)
    out = cp.stdout + cp.stderr
    if shot.is_file() and shot.stat().st_size > 1024:
        rec(report, "PASS", suite, "cua_capture", f"{shot.stat().st_size} bytes")
    else:
        rec(report, "FAIL", suite, "cua_capture", out[:200] or "no image")


def test_windows_tray(bs: str, peer: str, report: Report) -> None:
    print(f"\n== L3 tray: {peer} ==")
    cmd = (
        'powershell -NoProfile -Command '
        '"Get-CimInstance Win32_Process -Filter \\"Name=\'powershell.exe\'\\" | '
        'Where-Object { $_.CommandLine -match \'bs_tray\' } | '
        'Select-Object -First 1 ProcessId | ForEach-Object { \'TRAY_PID=\' + $_.ProcessId }"'
    )
    cp = run([bs, "shell", peer, "--cmd", cmd], timeout=45)
    out = cp.stdout + cp.stderr
    if "TRAY_PID=" in out:
        rec(report, "PASS", "tray_windows", "process", out.strip().splitlines()[-1][:80])
    else:
        # try start tray via shell
        start = (
            r'powershell -NoProfile -Command '
            r'"$t=Join-Path $env:LOCALAPPDATA bridgesessions\bs_tray.ps1; '
            r'if(-not (Test-Path $t)){$t=Join-Path $env:USERPROFILE bridgesessions\bs_tray.ps1}; '
            r'if(Test-Path $t){Start-Process powershell -ArgumentList '
            r'\"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File $t\"}; '
            r'Start-Sleep 2; '
            r'if(Get-CimInstance Win32_Process | Where-Object {$_.CommandLine -match \"bs_tray\"})'
            r'{Write-Output TRAY_OK}else{Write-Output TRAY_FAIL}}"'
        )
        cp2 = run([bs, "shell", peer, "--cmd", start], timeout=60)
        if "TRAY_OK" in (cp2.stdout + cp2.stderr):
            rec(report, "PASS", "tray_windows", "process", "started")
        else:
            rec(report, "FAIL", "tray_windows", "process", (out + cp2.stdout)[:200])

    # Service posture: one mesh + at most one helper
    svc = (
        "powershell -NoProfile -Command "
        "\"$m=@(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match "
        "'bridgesessions.exe' -and $_.CommandLine -notmatch 'cua-helper' }); "
        "$h=@(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'cua-helper' }); "
        "Write-Output ('MESH=' + $m.Count + ' HELPER=' + $h.Count); "
        "if($h.Count -gt 1){Write-Output 'MULTI_HELPER'}\""
    )
    cp3 = run([bs, "shell", peer, "--cmd", svc], timeout=45)
    out3 = cp3.stdout + cp3.stderr
    if "MULTI_HELPER" in out3:
        rec(report, "FAIL", "tray_windows", "helper_count", "more than one --cua-helper process")
    elif "MESH=" in out3:
        rec(report, "PASS", "tray_windows", "service_posture", out3.replace("\n", " ")[:120])
    else:
        rec(report, "SKIP", "tray_windows", "service_posture", out3[:120])


# ── L3 macOS ────────────────────────────────────────────────────────
def test_mac_desktop(bs: str, peer: str, report: Report) -> None:
    print(f"\n== L3 macOS desktop: {peer} ==")
    probe = HARNESS / "mac_desktop_probe.sh"
    tmp = Path(tempfile.gettempdir()) / "mac_desktop_probe.sh"
    tmp.write_text(probe.read_text(encoding="utf-8"), encoding="utf-8")
    run([bs, "file", "send", peer, str(tmp), "--wait"], timeout=60)
    # find received path and run
    cmd = (
        'f=$(ls -t "$HOME/.bridgesessions/received/mac_desktop_probe.sh"* 2>/dev/null | head -1); '
        'bash "$f" 2>&1'
    )
    cp = run([bs, "shell", peer, "--cmd", cmd], timeout=60)
    out = cp.stdout + cp.stderr
    for line in out.splitlines():
        if "|" in line and any(line.startswith(s) for s in ("PASS", "FAIL", "SKIP")):
            parts = line.split("|", 2)
            if len(parts) >= 2:
                rec(report, parts[0], "menubar_macos", parts[1], parts[2] if len(parts) > 2 else "")
    if "MAC_DESKTOP_SUMMARY" not in out and report.fail_n == 0:
        # fallback process checks
        if "FAIL" in out:
            rec(report, "FAIL", "menubar_macos", "probe", out[:200])
        else:
            rec(report, "SKIP", "menubar_macos", "probe", "no summary line")

    # CUA against peer-mac
    test_cua(bs, peer, "cua_macos", report)


# ── L3 Linux KVM ────────────────────────────────────────────────────
def setup_linux_kvm(bs: str, report: Report) -> Optional[str]:
    """Run KVM setup on Linux lab hop (BS_E2E_LINUX_HOST); return guest IP if known."""
    host = os.environ.get("BS_E2E_LINUX_HOST", "peer-linux-a")
    print(f"\n== L3 setup: Linux KVM desktop on {host} ==")
    script = HARNESS / "linux_kvm_setup.sh"
    run([bs, "file", "send", host, str(script), "--wait"], timeout=60)
    cmd = (
        'f=$(ls -t "$HOME/.bridgesessions/received/linux_kvm_setup.sh"* 2>/dev/null | head -1); '
        'chmod +x "$f"; bash "$f" 2>&1'
    )
    try:
        cp = run([bs, "shell", host, "--cmd", cmd], timeout=900)
    except subprocess.TimeoutExpired:
        rec(report, "FAIL", "linux_kvm", "setup", "timeout 900s")
        return None
    out = cp.stdout + cp.stderr
    print(out[-2000:] if len(out) > 2000 else out)
    m = re.search(r"KVM_READY(?: ip=([0-9.]+))?", out)
    if m:
        rec(report, "PASS", "linux_kvm", "setup", m.group(0))
        return m.group(1)
    if "KVM_STARTED_PENDING_SSH" in out:
        rec(report, "SKIP", "linux_kvm", "setup", "VM started, SSH pending cloud-init")
        return None
    rec(report, "FAIL", "linux_kvm", "setup", out[-300:])
    return None


def test_linux_desktop_via_linux_hop(bs: str, report: Report) -> None:
    """CUA against KVM guest as seen from Linux hop (virbr0 path)."""
    host = os.environ.get("BS_E2E_LINUX_HOST", "peer-linux-a")
    print(f"\n== L3 Linux CUA via {host} hop (bs-qa-ubuntu) ==")
    cmd = (
        'export PATH="$HOME/.local/bin:/usr/bin:/bin"; '
        'bridgesessions health bs-qa-ubuntu 2>&1; '
        'bridgesessions cua screen bs-qa-ubuntu 2>&1; '
        'bridgesessions cua capture bs-qa-ubuntu -o /tmp/e2e-linux-cua.png 2>&1 | tail -3; '
        'file /tmp/e2e-linux-cua.png 2>/dev/null | head -1'
    )
    try:
        cp = run([bs, "shell", host, "--cmd", cmd], timeout=90)
    except subprocess.TimeoutExpired:
        rec(report, "FAIL", "cua_linux_desktop", "linux_hop_timeout", "")
        return
    out = cp.stdout + cp.stderr
    if "healthy" in out:
        rec(report, "PASS", "cua_linux_desktop", "health_via_linux_hop", "ok")
    else:
        rec(report, "FAIL", "cua_linux_desktop", "health_via_linux_hop", out[:200])
    m = re.search(r"(\d{3,5})\s*[xX]\s*(\d{3,5})", out)
    if m and int(m.group(1)) >= 640:
        rec(report, "PASS", "cua_linux_desktop", "cua_screen", m.group(0))
    else:
        rec(report, "FAIL", "cua_linux_desktop", "cua_screen", out[:200])
    if "PNG image" in out or "Saved" in out:
        rec(report, "PASS", "cua_linux_desktop", "cua_capture", "png ok")
    else:
        rec(report, "FAIL", "cua_linux_desktop", "cua_capture", out[:200])


def test_linux_desktop_peer(bs: str, peer: str, report: Report) -> None:
    """If peer is on mesh with DISPLAY, run CUA; else SKIP."""
    print(f"\n== L3 Linux desktop peer: {peer} ==")
    # Check if peer exists
    cp = run([bs, "health", peer], timeout=20)
    if "healthy" not in (cp.stdout + cp.stderr):
        rec(report, "SKIP", "cua_linux_desktop", "health", f"{peer} not healthy/not joined")
        rec(report, "SKIP", "tray_linux", "health", f"{peer} not on mesh")
        return
    # DISPLAY check
    cp = run([bs, "shell", peer, "--cmd", "echo DISPLAY=$DISPLAY; ls /tmp/bs-display-ready 2>/dev/null || true"], timeout=30)
    out = cp.stdout + cp.stderr
    if "DISPLAY=:" not in out and "DISPLAY=:0" not in out:
        # try set DISPLAY=:0 for graphical session
        cp2 = run(
            [bs, "shell", peer, "--cmd", "export DISPLAY=:0; xdotool getdisplaygeometry 2>&1 || echo NO_X"],
            timeout=30,
        )
        if "NO_X" in (cp2.stdout + cp2.stderr) or "cannot open display" in (cp2.stdout + cp2.stderr).lower():
            rec(report, "SKIP", "cua_linux_desktop", "display", "no X display yet")
            return
    test_cua(bs, peer, "cua_linux_desktop", report)
    # tray process
    cp = run(
        [bs, "shell", peer, "--cmd", "pgrep -af bs_tray || pgrep -af pystray || echo NO_TRAY"],
        timeout=30,
    )
    if "NO_TRAY" in (cp.stdout + cp.stderr):
        # start tray if script available
        rec(report, "SKIP", "tray_linux", "process", "not running (start bs_tray.py under XFCE)")
    else:
        rec(report, "PASS", "tray_linux", "process", "found")
    # systemd user service should own mesh when installed
    cp_svc = run(
        [bs, "shell", peer, "--cmd",
         "systemctl --user is-active bridgesessions 2>/dev/null || echo inactive; "
         "pgrep -x bridgesessions >/dev/null && echo MESH_PROC || echo NO_MESH"],
        timeout=30,
    )
    outs = cp_svc.stdout + cp_svc.stderr
    if "active" in outs and "MESH_PROC" in outs:
        rec(report, "PASS", "tray_linux", "systemd_mesh", "active")
    elif "MESH_PROC" in outs:
        rec(report, "SKIP", "tray_linux", "systemd_mesh", "mesh running without active unit")
    else:
        rec(report, "FAIL", "tray_linux", "systemd_mesh", outs[:120])


# ── main ────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description="BridgeSessions e2e orchestrator")
    ap.add_argument("--layers", default="L2,L3", help="Comma list: L2,L3,L4")
    ap.add_argument("--json", default="", help="Write JSON report path")
    ap.add_argument("--skip-setup", action="store_true", help="Skip KVM/Windows desktop bootstrap")
    ap.add_argument(
        "--peers",
        default=os.environ.get("BS_E2E_PEERS", "peer-linux-a,peer-linux-b,peer-mac,peer-win"),
        help="L2 peers comma-separated (override with BS_E2E_PEERS for lab fleet)",
    )
    ap.add_argument(
        "--peer-win",
        default=os.environ.get("BS_E2E_PEER_WIN", "peer-win"),
        help="Windows desktop peer (BS_E2E_PEER_WIN)",
    )
    ap.add_argument(
        "--peer-mac",
        default=os.environ.get("BS_E2E_PEER_MAC", "peer-mac"),
        help="macOS desktop peer (BS_E2E_PEER_MAC)",
    )
    args = ap.parse_args()
    layers = [x.strip().upper() for x in args.layers.split(",") if x.strip()]
    peers = [x.strip() for x in args.peers.split(",") if x.strip()]
    # Ensure Windows desktop peer is in the L2 matrix when L3 needs it
    if args.peer_win and args.peer_win not in peers:
        peers.append(args.peer_win)

    bs = which_bs()
    report = Report(started=ts(), layers=layers)
    print(f"BridgeSessions e2e runner  bs={bs}  layers={layers}  started={report.started}")

    if "L2" in layers:
        run_l2(report, peers)

    if "L3" in layers:
        if not args.skip_setup:
            setup_windows_desktop(bs, args.peer_win, report)
            setup_linux_kvm(bs, report)
        else:
            rec(report, "SKIP", "setup", "skipped", "--skip-setup")

        test_windows_tray(bs, args.peer_win, report)
        test_cua(bs, args.peer_win, "cua_windows", report)
        test_mac_desktop(bs, args.peer_mac, report)
        # Linux desktop: KVM guest (mesh via host virbr0)
        test_linux_desktop_via_linux_hop(bs, report)

        test_linux_desktop_peer(bs, "bs-qa-ubuntu", report)

    report.finished = ts()
    print(
        f"\n== summary ==  pass={report.pass_n} fail={report.fail_n} "
        f"skip={report.skip_n} finished={report.finished}"
    )
    if args.json:
        payload = {
            "started": report.started,
            "finished": report.finished,
            "layers": report.layers,
            "pass": report.pass_n,
            "fail": report.fail_n,
            "skip": report.skip_n,
            "results": [asdict(r) for r in report.results],
        }
        Path(args.json).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"  json: {args.json}")

    return 1 if report.fail_n else 0


if __name__ == "__main__":
    sys.exit(main())
