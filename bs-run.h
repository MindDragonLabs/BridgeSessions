// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-run.h — `bs run` persistent background services (lane 1, 26.09.10)
//
// `bs run --self --name <n> -- <cmd...>` registers a run-service with the
// daemon. The daemon spawns the command DETACHED from any session (its own
// process group, double-fork / systemd-run scope like spawn_session_worker),
// supervises it, restarts it with exponential backoff capped at 60s, and
// appends its stdout/stderr to a per-service log under the BridgeSessions
// home. State lives in <app_home>/state/run-services.json so supervision
// resumes after a daemon restart (same quarantine pattern as sessions.json,
// 26.09.09).
//
// Windows: the daemon-side supervisor is platform-aware, but the detached
// spawn uses POSIX process groups. On Windows `bs run` refuses with a clear
// error until the scheduled-task worker pattern is ported (documented in
// docs/usage.md).
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#if defined(__linux__) && defined(SYS_close_range)
#include <sys/syscall.h>
#endif
#endif

namespace runsrv {

// ── State model ─────────────────────────────────────────────────────

inline constexpr int kRunRestartMaxBackoffSecs = 60;
inline constexpr int kRunRestartBaseBackoffSecs = 2;
// After this many consecutive failed restarts the service is left in
// "backoff" (supervision paused, state kept) instead of spawning forever.
inline constexpr int kRunRestartGiveUp = 10;

struct RunServiceState {
    std::string name;            // unique service name
    std::string command;         // full shell command line (joined from argv)
    std::string desired = "running";  // "running" | "stopped"
    int64_t pid = -1;            // last spawned process-group leader pid
    int restart_count = 0;       // consecutive failures (reset on clean start)
    int64_t last_exit_code = 0;
    int64_t next_start_after = 0;  // unix secs; backoff gate
    int64_t created_at = 0;        // unix secs
};

[[nodiscard]] inline std::string run_services_state_path(const std::string& app_home) {
    return (std::filesystem::path(make_app_paths(app_home).state) /
            "run-services.json").string();
}

[[nodiscard]] inline std::string run_service_log_path(const std::string& app_home,
                                                      const std::string& name) {
    return (std::filesystem::path(make_app_paths(app_home).logs) /
            ("run-" + sanitize_systemd_unit_name(name) + ".log")).string();
}

[[nodiscard]] inline int64_t run_now_unix() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Service names become part of a log filename and JSON state keys — restrict
// to the same charset systemd unit names use so no path/JSON surprises.
[[nodiscard]] inline bool run_service_name_valid(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == ':' || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// ── Persistence (atomic write; corrupt files quarantined like sessions) ──

inline bool save_run_services(const std::string& path,
                              const std::vector<RunServiceState>& services) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& s : services) {
        nlohmann::json e;
        e["name"] = s.name;
        e["command"] = redact_secrets(s.command);
        e["desired"] = s.desired;
        e["pid"] = s.pid;
        e["restart_count"] = s.restart_count;
        e["last_exit_code"] = s.last_exit_code;
        e["next_start_after"] = s.next_start_after;
        e["created_at"] = s.created_at;
        j.push_back(e);
    }
    std::string plain = j.dump(2);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << "v1:plain\n" << plain << '\n';
        f.flush();
        if (!f) { std::filesystem::remove(tmp); return false; }
    }
    if (!restrict_private_file_permissions(tmp)) {
        std::filesystem::remove(tmp);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp); return false; }
    return true;
}

inline std::vector<RunServiceState> load_run_services(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string header;
    if (!std::getline(f, header)) return {};
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    std::string plain = (header == "v1:plain" || header == "v2:plain")
        ? data : header + "\n" + data;  // legacy raw JSON
    try {
        auto j = nlohmann::json::parse(plain);
        std::vector<RunServiceState> out;
        for (auto& e : j) {
            RunServiceState s;
            s.name = e.value("name", "");
            s.command = e.value("command", "");
            s.desired = e.value("desired", "running");
            s.pid = e.value("pid", static_cast<int64_t>(-1));
            s.restart_count = e.value("restart_count", 0);
            s.last_exit_code = e.value("last_exit_code", static_cast<int64_t>(0));
            s.next_start_after = e.value("next_start_after", static_cast<int64_t>(0));
            s.created_at = e.value("created_at", static_cast<int64_t>(0));
            if (run_service_name_valid(s.name)) out.push_back(std::move(s));
        }
        return out;
    } catch (...) {
        // Quarantine a half-written/corrupt state file (26.09.09 pattern).
        std::error_code ec;
        std::filesystem::rename(path, path + ".corrupt", ec);
        if (!ec) log_event("run_state_quarantined",
                           path + " (unparseable; renamed .corrupt)");
        return {};
    }
}

// ── Process liveness / kill (POSIX) ─────────────────────────────────

#ifndef _WIN32
// Best-effort "is this pid alive". A zombie we have not reaped counts as
// alive here; supervision reaps explicitly.
[[nodiscard]] inline bool run_pid_alive(int64_t pid) {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;  // exists but not ours
}

// Kill the whole process group: TERM, grace window, then KILL. The service
// was spawned as a session/process-group leader, so -pid reaches children.
inline void run_kill_process_group(int64_t pid) {
    if (pid <= 0) return;
    const pid_t pgid = static_cast<pid_t>(pid);
    (void)::kill(-pgid, SIGTERM);
    for (int i = 0; i < 30; ++i) {   // ~3s grace
        if (!run_pid_alive(pid)) break;
        ::usleep(100 * 1000);
    }
    (void)::kill(-pgid, SIGKILL);
    (void)::kill(pgid, SIGKILL);     // leader may have escaped its own group
    // Reap if it is still our direct child; ignore errors (not our child).
    int status = 0;
    (void)::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
}
#endif

// ── Detached spawn (process-group leader, logs to per-service file) ──
// Returns the child pid (>0) or -1 on failure. The child is a session
// leader (setsid) so it survives daemon restarts; the daemon re-adopts it
// by pid after a restart, exactly like session workers are re-adopted.
#ifndef _WIN32
inline int64_t run_spawn_detached(const std::string& command,
                                  const std::string& log_path) {
    std::string dir;
    {
        const auto slash = log_path.rfind('/');
        if (slash != std::string::npos) dir = log_path.substr(0, slash);
    }
    if (!dir.empty()) (void)ensure_private_directory(dir);

    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid > 0) {
        // Parent: do NOT waitpid — the daemon is not the permanent supervisor
        // across restarts, so the child must be able to outlive us. It is
        // reparented to PID 1 the moment we exit or lose interest; while we
        // live it stays a child and supervision reaps it explicitly.
        return static_cast<int64_t>(pid);
    }

    // Child: detached session leader.
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
    {
        long max_fd = ::sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;
        for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
    }
    ::signal(SIGHUP, SIG_IGN);
    ::setsid();   // new session + process group: kill(-pgid) reaches the tree

    int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) log_fd = ::open("/dev/null", O_WRONLY);
    if (log_fd >= 0) {
        ::dup2(log_fd, STDIN_FILENO);
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        if (log_fd > STDERR_FILENO) ::close(log_fd);
    }
    int null_fd = ::open("/dev/null", O_RDONLY);
    if (null_fd >= 0) { ::dup2(null_fd, STDIN_FILENO); ::close(null_fd); }

    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}
#endif

// ── Backoff policy (pure, unit-tested) ──────────────────────────────

// Exponential backoff with a 60s cap: 2, 4, 8, 16, 32, 60, 60, ...
[[nodiscard]] inline int run_backoff_secs(int consecutive_failures) {
    if (consecutive_failures < 1) return kRunRestartBaseBackoffSecs;
    int shift = consecutive_failures - 1;
    if (shift > 5) shift = 5;
    const int secs = kRunRestartBaseBackoffSecs << shift;
    return secs > kRunRestartMaxBackoffSecs ? kRunRestartMaxBackoffSecs : secs;
}

// Supervision decision for one observed child exit. Pure function of the
// recorded state so tests can exercise it without spawning processes.
struct RunSupervision {
    bool restart = false;        // spawn again now (subject to backoff gate)
    bool give_up = false;        // too many consecutive failures — stop trying
    int next_restart_count = 0;  // new consecutive-failure count
    int backoff_secs = 0;        // when restart/give_up: delay before next try
};

[[nodiscard]] inline RunSupervision run_supervise_after_exit(
        const RunServiceState& s, int64_t exit_code, bool clean_exit,
        int64_t now_unix) {
    (void)exit_code;
    RunSupervision r;
    if (s.desired != "running") return r;   // operator stopped it — leave dead
    if (clean_exit) return r;               // exited 0 on purpose — leave dead
    r.next_restart_count = s.restart_count + 1;
    r.backoff_secs = run_backoff_secs(r.next_restart_count);
    if (r.next_restart_count > kRunRestartGiveUp) {
        r.give_up = true;
        return r;
    }
    r.restart = true;
    (void)now_unix;
    return r;
}

// Join argv into one shell line (each word single-quote escaped POSIX-style).
[[nodiscard]] inline std::string run_join_command(
        const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        if (!out.empty()) out += ' ';
        out += shell_arg_quote(a);
    }
    return out;
}

} // namespace runsrv
