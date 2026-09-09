// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon
//
// Upgrade watchdog (operator mandate 2026-09-07: upgrades must be seamless,
// quick, painless, NON-DESTRUCTIVE).
//
// The upgrade CLI already swaps atomically (rename-only) and rolls back if
// `--version` verification fails. The remaining stranding scenario is: the
// new binary cannot start at all (arch mismatch, broken loader, corrupted
// .app copy, service-manager start failure) and the upgrade process itself
// dies with the daemon it just stopped. Then `.old` sits on disk unstarted
// and the peer is offline until an operator rescues it.
//
// arm_upgrade_watchdog() spawns a DETACHED helper (never a child of the
// soon-to-restart daemon or the upgrade process) that:
//   1. waits a grace period for the new daemon to start,
//   2. probes a TCP connect to the mesh port,
//   3. on persistent failure: renames old_path back over bin_path, starts
//      the daemon, and logs to <home>/upgrade-watchdog.log.
// On success the watchdog exits silently; the upgrade CLI's normal
// verification deletes old_path, so the watchdog's later restore attempt is
// a no-op (old_path missing -> nothing to do).
//
// This header does NOT open its own namespace — its caller (bs-protocol.h's
// facade, included as `namespace bs::mesh { #include ... }`) provides it.
// Defining inline functions inside an extra namespace block here would nest
// them as bs::mesh::bs::mesh::name and break lookup from main.cpp (same
// contract as bs-upgrade-safety.h).
//
// Launchers:
//   Linux:  setsid sh -c <script>      (bash /dev/tcp probe)
//   Windows (2026-09-08 RCA — Windows was the stranding surface): detached
//           cmd.exe running a generated .cmd script; probe uses
//           `powershell -Command Test-NetConnection`-free approach — curl
//           is guaranteed present (the upgrade downloads with it), so the
//           probe is `curl -s -m 2 -o NUL http://127.0.0.1:<port>/`.
//           The script is written to <home>\upgrade-watchdog.cmd and
//           launched via _spawnl(_P_DETACH) so it survives this process
//           and the daemon restart. Windows rollback is what the fleet
//           was missing: peers stranded exactly this way before.

#include <string>
#include <cstdlib>
#include <cstdio>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

// NOTE: no `namespace bs::mesh { ... }` here — the facade provides it.

// Arm the detached rollback watchdog. bin_path/old_path are absolute;
// start_cmd is the platform command that starts the daemon (e.g.
// `systemctl --user start bridgesessions.service`); port is the mesh listen
// port used for the liveness probe.
inline void arm_upgrade_watchdog(const std::string& bin_path,
                                 const std::string& old_path,
                                 const std::string& start_cmd,
                                 const std::string& port,
                                 const std::string& home_dir) {
    if (bin_path.empty() || old_path.empty() || start_cmd.empty()) return;
#ifdef __linux__
    // Helper script (sh): wait grace, then probe. Probe selection at arm
    // time: /dev/tcp is a bash-ism — dash's /bin/sh cannot run it, and arming
    // a prober the host can't execute would roll back HEALTHY daemons (the
    // probe fails every round regardless of the real port state). Prefer
    // bash (present on every server distro); fall back to curl (exit 7 is
    // connect-refused, any other outcome means the port answered). With
    // neither, do not arm: an unprobed rollback is worse than none.
    std::string log = home_dir + "/upgrade-watchdog.log";
    const bool have_bash = std::system("command -v bash >/dev/null 2>&1") == 0;
    const bool have_curl = std::system("command -v curl >/dev/null 2>&1") == 0;
    if (!have_bash && !have_curl) {
        std::ofstream lg(log, std::ios::app);
        lg << "watchdog: no bash or curl for port probe; not armed\n";
        return;
    }
    auto q = [](const std::string& s) {
        std::string out{"'"};
        for (const char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };

    std::string script;
    script += "sleep 20; ";                                   // grace for new daemon
    script += "for i in 1 2 3 4 5 6; do ";                    // ~60s of probes
    if (have_bash)
        script += "  if (exec 3<>/dev/tcp/127.0.0.1/" + port + ") 2>/dev/null; then exit 0; fi; ";
    else
        script += "  curl -m 2 -sk https://127.0.0.1:" + port +
                  "/ >/dev/null 2>&1; if [ $? -ne 7 ]; then exit 0; fi; ";
    script += "  sleep 10; ";
    script += "done; ";
    script += "echo \"$(date -Is) watchdog: new daemon did not bind; rolling back\" >> " + q(log) + "; ";
    // Daemon still down → restore previous binary and start it.
    script += "if [ -f " + q(old_path) + " ]; then ";
    script += "  mv -f " + q(old_path) + " " + q(bin_path) + "; ";
    script += "  echo \"$(date -Is) watchdog: restored old binary\" >> " + q(log) + "; ";
    script += "fi; ";
    script += "eval " + q(start_cmd) + " >> " + q(log) + " 2>&1; ";

    // Detached launcher: survives the upgrade process and the daemon restart.
    std::string cmd = "setsid sh -c " + q(script) + " </dev/null >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#elif defined(_WIN32)
    // Windows watchdog (2026-09-08): generate a .cmd helper and launch it
    // detached via _spawnl(_P_DETACH, cmd.exe). The script:
    //   - sleeps ~20s grace,
    //   - probes the mesh port 6x10s with curl (present: the upgrade itself
    //     downloaded via curl; -m 2 caps each probe),
    //   - on persistent failure: copies the staged exe path aside, moves
    //     .old back over bin, and runs the start command.
    // All paths are wrapped in double quotes for cmd.exe.
    (void)port; // used below via string
    std::string script_path = home_dir + "\\upgrade-watchdog.cmd";
    std::string log = home_dir + "\\upgrade-watchdog.log";
    auto qw = [](const std::string& s) { return "\"" + s + "\""; };

    std::string s;
    s += "@echo off\r\n";
    s += "timeout /t 20 /nobreak >nul\r\n";
    s += "for %%i in (1 2 3 4 5 6) do (\r\n";
    s += "  curl -s -m 2 -o nul http://127.0.0.1:" + port + "/ >nul 2>&1\r\n";
    s += "  if not errorlevel 1 exit /b 0\r\n";
    s += "  timeout /t 10 /nobreak >nul\r\n";
    s += ")\r\n";
    s += "echo %date% %time% watchdog: new daemon did not bind; rolling back >> " + qw(log) + "\r\n";
    s += "if exist " + qw(old_path) + " (\r\n";
    s += "  copy /y " + qw(old_path) + " " + qw(bin_path) + " >nul\r\n";
    s += "  echo %date% %time% watchdog: restored old binary >> " + qw(log) + "\r\n";
    s += ")\r\n";
    s += start_cmd + " >> " + qw(log) + " 2>&1\r\n";

    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    // Detached: cmd /c "script" — _P_DETACH means we do not wait, and the
    // child is not a child of the daemon (we are the upgrade CLI).
    const std::string args = "/c " + qw(script_path);
    const intptr_t rc = _spawnl(_P_DETACH, "cmd.exe", "cmd.exe",
                                "/c", qw(script_path).c_str(), nullptr);
    (void)args; (void)rc;
#else
    (void)bin_path; (void)old_path; (void)start_cmd;
    (void)port; (void)home_dir;
#endif
}
