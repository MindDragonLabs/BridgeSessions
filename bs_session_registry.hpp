// bs_session_registry.hpp — Thread-safe session lifecycle (extracted from bs-protocol.h)
#pragma once
#include "bs_logging.hpp"
namespace bs::mesh {
class SessionRegistry {
    struct SessionHistoryEntry {
        std::string name;
        std::string peer;
        std::string pid;
        std::string state;
        int32_t exit_code = 0;
        uint64_t runtime_seconds = 0;
        uint64_t bytes = 0;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::atomic<uint32_t> next_attach_id_{1};
    std::vector<SessionHistoryEntry> recent_;
    std::string persistence_path_;
    static constexpr size_t kMaxRecentSessions = 50;

    // P0 UAF fix: fires before a session is erased from the map so callers
    // (MeshController) can null raw attached_session pointers that would dangle.
    std::function<void(const std::string&)> on_session_erased_;

    static ResolvedSessionCommand complete_command(
        const std::string& command) {
        if (command.empty()) {
            return {platform_default_shell(), SessionCommandSource::ConfigDefault};
        }
        return {command, SessionCommandSource::NamedProfile};
    }

    // Replace only the spawned runtime while keeping the Session object's
    // address stable for Conn::attached_session pointers. User-visible
    // scrollback, attached peers, and restart policy survive the respawn.
    static void install_spawned_runtime(Session& target, Session&& spawned,
                                        SessionState state) {
        auto peer_ids = std::move(target.peer_ids);
        auto scrollback = std::move(target.scrollback);
#ifndef _WIN32
        auto pending_input = std::move(target.pending_input);
        const bool input_backpressured = target.input_backpressured;
#endif
        const auto last_attach_at = target.last_attach_at;
        const bool auto_restart = target.auto_restart;
        const int restart_failures = target.restart_failures;
        const auto restart_window_start = target.restart_window_start;

        target.~Session();
        new (&target) Session(std::move(spawned));
        target.peer_ids = std::move(peer_ids);
        target.scrollback = std::move(scrollback);
#ifndef _WIN32
        target.pending_input = std::move(pending_input);
        target.input_backpressured = input_backpressured;
#endif
        target.last_attach_at = last_attach_at;
        target.auto_restart = auto_restart;
        target.restart_failures = restart_failures;
        target.restart_window_start = restart_window_start;
        target.history_recorded = false;
        target.state = state;
    }

    static std::string session_pid_string(const Session& s) {
#ifdef _WIN32
        return s.child_pid ? std::to_string(GetProcessId(s.child_pid)) : "-";
#else
        return s.child_pid > 0 ? std::to_string(s.child_pid) : "-";
#endif
    }

    void record_history_locked(Session& s, int32_t exit_code, const std::string& state) {
        if (s.history_recorded) return;
        auto now = std::chrono::steady_clock::now();
        SessionHistoryEntry h;
        h.name = s.name;
        h.peer = s.peer_ids.empty() ? "-" : s.peer_ids.front().substr(0, 16);
        h.pid = session_pid_string(s);
        h.state = state;
        h.exit_code = exit_code;
        h.runtime_seconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - s.created_at).count());
        h.bytes = static_cast<uint64_t>(s.scrollback.total_written());
        recent_.push_back(std::move(h));
        if (recent_.size() > kMaxRecentSessions)
            recent_.erase(recent_.begin(), recent_.begin() + static_cast<ptrdiff_t>(recent_.size() - kMaxRecentSessions));
        s.history_recorded = true;
    }

public:
    SessionRegistry() = default;

    // P0 UAF fix: allow MeshController to register a callback that fires
    // before any session is erased, so it can null dangling attached_session pointers.
    void set_on_session_erased(std::function<void(const std::string&)> cb) {
        on_session_erased_ = std::move(cb);
    }

    void set_persistence_path(const std::string& path) {
        persistence_path_ = path;
    }

    // ── Attach / Create ─────────────────────────────────────────
    // Connection-path attach (2.0.8): registers a per-connection Attachment,
    // returns the server-assigned attach_id, and reports the effective
    // (min-wins) geometry across all current attachments. Returns 0 on error.
    uint32_t attach_connection(const std::string& name,
                               const ResolvedSessionCommand& resolved,
                               uint16_t cols, uint16_t rows, const std::string& term,
                               const std::string& peer_pubkey,
                               uint32_t client_instance_id, bool spectator,
                               uint16_t& out_eff_cols, uint16_t& out_eff_rows) {
        std::unique_lock lock(mutex_);
        Session* s = nullptr;

        // 2.0.8 MoA fix: session names cross the line-oriented IPC protocol
        // (SCROLLBACK <name> <offset>) and log lines — whitespace/control
        // chars make a session unaddressable and forge protocol fields.
        if (name.empty()) return 0;
        for (unsigned char ch : name)
            if (ch <= 0x20 || ch == 0x7f) return 0;

        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            s = it->second.get();

            if (s->state == SessionState::Running || s->state == SessionState::Detached
                || s->state == SessionState::Attached) {
                s->state = SessionState::Attached;
                s->last_attach_at = std::chrono::steady_clock::now();
                if (!peer_pubkey.empty()
                    && std::find(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey)
                           == s->peer_ids.end())
                    s->peer_ids.push_back(peer_pubkey);
            } else {
                // Died/Exited/Killed/Recoverable — recreate PTY in place.
                log_event("session_resurrect_replace", name);
                record_history_locked(*s, -1, session_state_str(s->state));
                const std::string spawn_command = prepare_session_command(resolved);
                auto session_result = create_session(name, spawn_command, cols, rows, term);
                if (!session_result) return 0;
                install_spawned_runtime(*s, std::move(*session_result), SessionState::Attached);
                if (!peer_pubkey.empty()
                    && std::find(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey)
                           == s->peer_ids.end())
                    s->peer_ids.push_back(peer_pubkey);
            }
        } else {
            // Create new session
            const std::string spawn_command = prepare_session_command(resolved);
            auto session_result = create_session(name, spawn_command, cols, rows, term);
            if (!session_result) return 0;
            auto news = std::make_unique<Session>(std::move(*session_result));
            news->state = SessionState::Attached;
            if (!peer_pubkey.empty()) news->peer_ids.push_back(peer_pubkey);
            s = news.get();
            sessions_[name] = std::move(news);
        }

        // Register this connection's Attachment and compute effective geometry.
        // Reserve 0 as the error sentinel: skip it if the counter ever wraps.
        uint32_t aid = next_attach_id_.fetch_add(1, std::memory_order_relaxed);
        if (aid == 0) aid = next_attach_id_.fetch_add(1, std::memory_order_relaxed);
        Session::Attachment att;
        att.attach_id = aid;
        att.cols = cols; att.rows = rows;
        att.spectator = spectator;
        att.pubkey = peer_pubkey;
        s->attachments[aid] = att;

        // MIN-wins geometry across all attachments (narrowest pane drives the PTY).
        uint16_t min_c = cols, min_r = rows;
        for (auto& kv : s->attachments) {
            min_c = std::min(min_c, kv.second.cols);
            min_r = std::min(min_r, kv.second.rows);
        }
        out_eff_cols = min_c; out_eff_rows = min_r;
        apply_min_geometry_locked(*s);
        log_event("session_attach", name + " attach_id=" + std::to_string(aid)
                  + " spectator=" + (spectator ? "1" : "0")
                  + " eff=" + std::to_string(min_c) + "x" + std::to_string(min_r));
        return aid;
    }

    // Programmatic / test-facing attach: returns the Session* (registers a
    // default interactive attachment). Preserves the pre-2.0.8 call shape.
    Session* attach(const std::string& name,
                    const ResolvedSessionCommand& resolved,
                    uint16_t cols, uint16_t rows, const std::string& term,
                    const std::string& peer_pubkey = "") {
        uint16_t ec = 0, er = 0;
        uint32_t aid = attach_connection(name, resolved, cols, rows, term,
                                          peer_pubkey, 0, false, ec, er);
        if (aid == 0) return nullptr;
        return get(name);
    }

    // Look up a Session by its attach_id (for detach-by-id bookkeeping).
    Session* session_by_attach_id(uint32_t attach_id) {
        std::shared_lock lock(mutex_);
        for (auto& kv : sessions_) {
            if (kv.second->attachments.count(attach_id)) return kv.second.get();
        }
        return nullptr;
    }

    // Recompute the effective (MIN-wins) geometry across all attachments and
    // resize the PTY accordingly. Caller must hold mutex_. Used by
    // attach_connection, detach(uint32_t), and set_attachment_geometry.
    void apply_min_geometry_locked(Session& s) {
        if (s.attachments.empty()) return;
        uint16_t min_c = UINT16_MAX, min_r = UINT16_MAX;
        for (auto& kv : s.attachments) {
            min_c = std::min(min_c, kv.second.cols);
            min_r = std::min(min_r, kv.second.rows);
        }
#ifndef _WIN32
        if (s.master_fd >= 0) (void)resize_pty(static_cast<intptr_t>(s.master_fd), min_c, min_r);
#else
        if (s.hpcon) (void)resize_pty(reinterpret_cast<intptr_t>(s.hpcon), min_c, min_r);
#endif
    }

    // Update one attachment's geometry and re-apply MIN-wins (called from the
    // ResizeMsg path). No-op if the attach_id is unknown.
    void set_attachment_geometry(uint32_t attach_id, uint16_t cols, uint16_t rows) {
        std::unique_lock lock(mutex_);
        // 2.0.8 MoA fix: floor the geometry. Spectators may legitimately
        // resize (min-wins policy), but a 0x0/1x1 resize from ANY attachment
        // collapses the shared PTY for every viewer — a read-only role must
        // not be able to DoS the session display.
        cols = std::max<uint16_t>(cols, 20);
        rows = std::max<uint16_t>(rows, 5);
        // Inline lookup — session_by_attach_id() acquires its own shared_lock,
        // which would deadlock against our unique_lock on a non-recursive mutex.
        Session* s = nullptr;
        for (auto& kv : sessions_) {
            if (kv.second->attachments.count(attach_id)) { s = kv.second.get(); break; }
        }
        if (!s) return;
        auto it = s->attachments.find(attach_id);
        if (it == s->attachments.end()) return;
        it->second.cols = cols;
        it->second.rows = rows;
        apply_min_geometry_locked(*s);
        log_event("session_geometry", s->name
                  + " attach_id=" + std::to_string(attach_id)
                  + " eff=" + std::to_string(cols) + "x" + std::to_string(rows));
    }

    // Programmatic/internal callers pass complete commands. They are never
    // interpreted as remote one-shot overrides, so Windows launches them
    // directly instead of adding a cmd.exe layer.
    Session* attach(const std::string& name, const std::string& command,
                    uint16_t cols, uint16_t rows, const std::string& term,
                    const std::string& peer_pubkey = "") {
        return attach(name, complete_command(command), cols, rows, term,
                      peer_pubkey);
    }

    // ── Detach ──────────────────────────────────────────────────
    // Resolve a textual signal name (HUP/TERM/INT/KILL/QUIT) to a platform
    // signal token. On POSIX this is the signal number; on Windows it is a
    // small code (0 = Ctrl-C/INT-style, 1 = terminate, -1 = unknown) the
    // detach path maps to GenerateConsoleCtrlEvent / TerminateProcess.
    static int resolve_detach_signal(const std::string& name) {
        if (name.empty()) return -1;
#ifdef _WIN32
        if (name == "INT" || name == "HUP" || name == "QUIT")
            return 0;            // console Ctrl-C event
        if (name == "TERM" || name == "KILL") return 1;  // terminate / hard kill
        return -1;
#else
        if (name == "HUP")  return SIGHUP;
        if (name == "TERM") return SIGTERM;
        if (name == "INT")  return SIGINT;
        if (name == "QUIT") return SIGQUIT;
        if (name == "KILL") return SIGKILL;
        return -1;
#endif
    }

    // Detach a single connection by its server-assigned attach_id. Removes
    // only that attachment; the session survives until the last attachment is
    // gone. Returns true if the session still has attachments, false if empty.
    bool detach(uint32_t attach_id) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.begin();
        for (; it != sessions_.end(); ++it) {
            if (it->second->attachments.count(attach_id)) break;
        }
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        const std::string name = s->name;
        auto ait = s->attachments.find(attach_id);
        const std::string pubkey = (ait != s->attachments.end()) ? ait->second.pubkey : "";
        s->attachments.erase(attach_id);
        // Re-apply MIN-wins geometry: if the detached pane was the narrowest,
        // the PTY must grow back to the next-smallest remaining pane.
        if (!s->attachments.empty()) apply_min_geometry_locked(*s);
        if (!pubkey.empty()) {
            s->peer_ids.erase(
                std::remove(s->peer_ids.begin(), s->peer_ids.end(), pubkey),
                s->peer_ids.end());
        }
        if (s->attachments.empty() &&
            (s->state == SessionState::Attached || s->state == SessionState::Running)) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) {
                        GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    } else {
                        TerminateProcess(s->child_pid, 1);
                    }
                    log_event("session_detach_signal", name + " -> " + s->detach_signal);
#else
                    ::kill(s->child_pid, sig);
                    log_event("session_detach_signal", name + " -> " + s->detach_signal);
#endif
                } else if (s->child_pid) {
                    log_event("session_detach_signal_unknown", name + " signal=" + s->detach_signal);
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return !s->attachments.empty();
    }

    // Backward-compat (used by existing tests + any caller that tracked only
    // pubkey, not attach_id): detach every attachment belonging to a pubkey.
    bool detach(const std::string& name, const std::string& peer_pubkey) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        std::vector<uint32_t> to_erase;
        for (auto& kv : s->attachments)
            if (kv.second.pubkey == peer_pubkey) to_erase.push_back(kv.first);
        for (uint32_t id : to_erase) s->attachments.erase(id);
        if (!peer_pubkey.empty()) {
            s->peer_ids.erase(
                std::remove(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey),
                s->peer_ids.end());
        }
        if (s->attachments.empty() &&
            (s->state == SessionState::Attached || s->state == SessionState::Running)) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifndef _WIN32
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return !s->attachments.empty();
    }

    // Detach ALL attachments from a session (legacy fallback for old callers
    // that wired Conn.attached_session directly without the AttachMsg path).
    // Returns false if the session was not found.
    bool detach_all(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        s->attachments.clear();
        s->peer_ids.clear();
        if (s->state == SessionState::Attached || s->state == SessionState::Running) {
            if (!s->detach_signal.empty() && s->child_pid) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0) {
#ifdef _WIN32
                    if (sig == 0) GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    else TerminateProcess(s->child_pid, 1);
#else
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return false; // no attachments remain
    }

    // Backward-compat: detach every attachment for a session name (used by
    // legacy callers/tests that don't track attach_id). Returns true if
    // attachments remain (should be false after a full detach).
    bool detach(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        s->attachments.clear();
        s->peer_ids.clear();
        if (s->state == SessionState::Attached || s->state == SessionState::Running) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) {
                        GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    } else {
                        TerminateProcess(s->child_pid, 1);
                    }
#else
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return false;
    }

    // ── List ────────────────────────────────────────────────────
    std::vector<SessionInfo> list() const {
        std::shared_lock lock(mutex_);
        std::vector<SessionInfo> result;
        for (auto& [key, s] : sessions_) {
            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - s->created_at).count();
            result.push_back({
                s->name,
                session_state_str(s->state),
                static_cast<uint64_t>(uptime)
            });
        }
        return result;
    }

    std::string summary() const {
        std::shared_lock lock(mutex_);
        std::ostringstream out;
        bool wrote = false;
        auto now = std::chrono::steady_clock::now();
        for (auto& [key, s] : sessions_) {
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - s->created_at).count();
            if (wrote) out << " | ";
            out << "live " << s->name
                << " state=" << session_state_str(s->state)
                << " pid=" << session_pid_string(*s)
                << " peer=" << (s->peer_ids.empty() ? "-" : s->peer_ids.front().substr(0, 16))
                << " uptime=" << uptime << "s"
                << " bytes=" << s->scrollback.total_written();
            wrote = true;
        }
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
            if (wrote) out << " | ";
            out << "recent " << it->name
                << " state=" << it->state
                << " pid=" << it->pid
                << " peer=" << it->peer
                << " runtime=" << it->runtime_seconds << "s"
                << " exit=" << it->exit_code
                << " bytes=" << it->bytes;
            wrote = true;
        }
        return wrote ? out.str() : "No sessions.";
    }

    void record_finished(Session& s, int32_t exit_code, const std::string& state) {
        std::unique_lock lock(mutex_);
        record_history_locked(s, exit_code, state);
    }

    // ── Get ─────────────────────────────────────────────────────
    Session* get(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(name);
        return (it != sessions_.end()) ? it->second.get() : nullptr;
    }

    const Session* get(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(name);
        return (it != sessions_.end()) ? it->second.get() : nullptr;
    }

    // ── Kill ────────────────────────────────────────────────────
    void kill(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            std::string killed_name = it->second->name;
            it->second->state = SessionState::Killed;
            record_history_locked(*it->second, -1, "killed");
            // P0 UAF fix: fire callback before erasing so Conn::attached_session can be nulled
            if (on_session_erased_) on_session_erased_(killed_name);
            sessions_.erase(it);
            log_event("session_kill", killed_name);
        }
    }

    // ── Reap dead children ──────────────────────────────────────
    void reap_dead(bool include_attached = true) {
        std::unique_lock lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (!include_attached && s->state == SessionState::Attached) {
                ++it;
                continue;
            }
            if (s->state != SessionState::Running && s->state != SessionState::Attached &&
                s->state != SessionState::Detached) { ++it; continue; }
            bool died = false;
#ifdef _WIN32
            int32_t exit_code = 0;
            if (s->child_pid && WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                died = true;
                s->state = SessionState::Died;
                DWORD code = 0;
                GetExitCodeProcess(s->child_pid, &code);
                exit_code = static_cast<int32_t>(code);
                record_history_locked(*s, exit_code, "died");
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
            }
#else
            int32_t exit_code = 0;
            if (s->child_pid > 0) {
                int status = 0;
                pid_t result = waitpid(s->child_pid, &status, WNOHANG);
                if (result == s->child_pid) {
                    died = true;
                    s->state = SessionState::Died;
                    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
                    record_history_locked(*s, exit_code, "died");
                    s->child_pid = -1;
                }
            }
#endif
            if (died) {
                s->release_exited_runtime();
                // Auto-restart logic
                if (s->auto_restart) {
                    auto now = std::chrono::steady_clock::now();
                    auto window = std::chrono::seconds(60);
                    if (now - s->restart_window_start > window) {
                        s->reset_restart_failures();
                    }
                    if (s->restart_failures < 3) {
                        ++s->restart_failures;
                        std::string restart_name = s->name;
                        int restart_failures = s->restart_failures;
                        auto new_session = create_session(
                            restart_name, s->command, 80, 24, "xterm-256color");
                        if (new_session) {
                            const SessionState resumed_state = s->peer_ids.empty()
                                ? SessionState::Detached
                                : SessionState::Attached;
                            install_spawned_runtime(*s, std::move(*new_session),
                                                    resumed_state);
                            log_event("session_auto_restart", restart_name + " attempt=" + std::to_string(restart_failures));
                            ++it;
                            continue;
                        }
                    }
                    s->state = SessionState::Exited;
                    log_event("session_circuit_breaker", s->name + " failures=" + std::to_string(s->restart_failures));
                }
            }
            ++it;
        }
    }

    // ── Idle timeout cleanup ────────────────────────────────────
    void prune_idle(std::chrono::seconds max_idle) {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (s->state == SessionState::Detached) {
                auto idle = now - s->last_output_at;
                if (idle > max_idle) {
                    log_event("session_prune_idle", s->name);
                    record_history_locked(*s, -1, "pruned");
                    if (on_session_erased_) on_session_erased_(s->name);
                    it = sessions_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // ── Size ────────────────────────────────────────────────────
    size_t count() const {
        std::shared_lock lock(mutex_);
        return sessions_.size();
    }

    // ── Persistence ────────────────────────────────────────────
    void load_persisted_sessions() {
        if (persistence_path_.empty()) return;
        auto metas = load_sessions(persistence_path_);
        if (metas.empty()) return;

        std::unique_lock lock(mutex_);
        for (auto& m : metas) {
            if (sessions_.find(m.name) != sessions_.end()) continue;
            auto s = std::make_unique<Session>();
            s->name = m.name;
            s->command = m.command;
            s->state = SessionState::Recoverable;
            s->created_at = std::chrono::steady_clock::now();
            s->last_output_at = s->created_at;
            s->last_attach_at = s->created_at;
            sessions_[m.name] = std::move(s);
            log_event("session_loaded", m.name);
        }
    }

    bool save_persisted_sessions() const {
        if (persistence_path_.empty()) return true;
        std::vector<SessionMeta> metas;
        {
            std::shared_lock lock(mutex_);
            for (auto& [key, s] : sessions_) {
                SessionMeta m;
                m.name = s->name;
                m.owner_id = "";
                m.command = s->command;
                m.state = session_state_str(s->state);
                m.created_at = std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        s->created_at.time_since_epoch()).count());
                metas.push_back(m);
            }
        }
        bool ok = save_sessions(persistence_path_, metas);
        if (ok) log_event("session_persist_saved");
        return ok;
    }

    // ── Resurrect a RECOVERABLE session ────────────────────────
    Session* resurrect(const std::string& name,
                       uint16_t cols, uint16_t rows, const std::string& term) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return nullptr;
        auto* s = it->second.get();
        if (s->state != SessionState::Recoverable) return nullptr;

        const std::string spawn_command = prepare_session_command(
            {s->command, SessionCommandSource::NamedProfile});
        auto session_result = create_session(name, spawn_command, cols, rows, term);
        if (!session_result) return nullptr;

        install_spawned_runtime(*s, std::move(*session_result),
                                SessionState::Attached);
        log_event("session_resurrect", name);
        return s;
    }
};
} // namespace bs::mesh
