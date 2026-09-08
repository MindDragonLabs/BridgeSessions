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
// arm_upgrade_watchdog() spawns a DETACHED helper (setsid — never a child of
// the soon-to-restart daemon or the upgrade process) that:
//   1. waits a grace period for the new daemon to start,
//   2. probes a TCP connect to the mesh port (bash /dev/tcp),
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
// POSIX-only launcher (setsid sh -c); on other platforms the armer is a
// no-op for now — the Win32 pause path already falls back to a detached
// _spawnl start, and schtasks-based recovery stays operator-side until the
// service-managed path lands.

#include <string>
#include <cstdlib>

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
#ifdef __linux__
    if (bin_path.empty() || old_path.empty() || start_cmd.empty()) return;

    // Helper script (sh): wait grace, then probe. Probe = TCP connect to the
    // mesh port via bash /dev/tcp; success exits 0 silently. On final failure
    // restore the previous binary and start the daemon.
    std::string log = home_dir + "/upgrade-watchdog.log";
    auto q = [](const std::string& s) {
        std::string out{"'"};
        for (const char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
        return out;
    };

    std::string script;
    script += "sleep 20; ";                                   // grace for new daemon
    script += "for i in 1 2 3 4 5 6; do ";                    // ~60s of probes
    script += "  if (exec 3<>/dev/tcp/127.0.0.1/" + port + ") 2>/dev/null; then exit 0; fi; ";
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
#else
    (void)bin_path; (void)old_path; (void)start_cmd;
    (void)port; (void)home_dir;
#endif
}
