#include "session_manager.hpp"
#include <algorithm>
#include <iostream>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace bs::server {

std::string SessionManager::resolve_command(const std::string& from_client) {
    // ADR-007: Command resolution order:
    // 1. Client --cmd (already embedded in name:command format)
    // 2. Hardcoded default
    if (!from_client.empty()) return from_client;
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/bash -l";
#endif
}

std::string SessionManager::session_key(const std::string& owner_id, const std::string& name) {
    if (owner_id.empty()) return name;  // legacy/local tests
    return owner_id + "\x1f" + name;
}

// ── attach ──────────────────────────────────────────────────────

std::expected<Session*, SessionMgrError> SessionManager::attach(
    const std::string& name,
    const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    return attach_for("", name, command, cols, rows, term);
}

std::expected<Session*, SessionMgrError> SessionManager::attach_for(
    const std::string& owner_id,
    const std::string& name,
    const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    std::unique_lock lock(mutex_);

    // Check if session already exists in this owner's namespace
    const auto key = session_key(owner_id, name);
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        auto* s = it->second.get();

        if (s->state == SessionState::Running || s->state == SessionState::Detached) {
            // Reattach — send scrollback (handled by caller)
            s->state = SessionState::Attached;
            s->last_attach_at = std::chrono::steady_clock::now();
            // Resize PTY to match new client
#ifdef _WIN32
            (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#else
            (void)resize_pty(reinterpret_cast<intptr_t>(s->master_fd), cols, rows);
#endif
            return s;
        }

        if (s->state == SessionState::Attached) {
            // v1: single-attach — this replaces existing attachment
            s->state = SessionState::Attached;
            s->last_attach_at = std::chrono::steady_clock::now();
#ifdef _WIN32
            (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#else
            (void)resize_pty(reinterpret_cast<intptr_t>(s->master_fd), cols, rows);
#endif
            return s;
        }

        // Session is Died/Exited/Killed
        if (s->state == SessionState::Died && s->auto_restart) {
            return std::unexpected(SessionMgrError{
                "session '" + name + "' died but auto-restart is in progress"});
        }
        return std::unexpected(SessionMgrError{
            "session '" + name + "' is " + session_state_str(s->state)});
    }

    // Create new session
    auto resolved_cmd = resolve_command(command);
    auto session_result = create_session(name, resolved_cmd, cols, rows, term);
    if (!session_result) {
        return std::unexpected(SessionMgrError{
            "create_session failed: " + session_result.error().message});
    }

    auto s = std::make_unique<Session>(std::move(*session_result));
    s->owner_id = owner_id;
    s->command = resolved_cmd;
    s->state = SessionState::Attached;

    auto* ptr = s.get();
    sessions_[key] = std::move(s);
    return ptr;
}

// ── detach ──────────────────────────────────────────────────────

void SessionManager::detach(const std::string& name) {
    detach_for("", name);
}

void SessionManager::detach_for(const std::string& owner_id, const std::string& name) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(session_key(owner_id, name));
    if (it != sessions_.end() && it->second->state == SessionState::Attached) {
        it->second->state = SessionState::Detached;
    }
}

// ── list ────────────────────────────────────────────────────────

std::vector<bs::protocol::SessionInfo> SessionManager::list() const {
    std::shared_lock lock(mutex_);
    std::vector<bs::protocol::SessionInfo> result;
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

std::vector<bs::protocol::SessionInfo> SessionManager::list_for(const std::string& owner_id) const {
    std::shared_lock lock(mutex_);
    std::vector<bs::protocol::SessionInfo> result;
    for (auto& [key, s] : sessions_) {
        if (s->owner_id != owner_id) continue;
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

// ── get ─────────────────────────────────────────────────────────

Session* SessionManager::get(const std::string& name) {
    return get_for("", name);
}

const Session* SessionManager::get(const std::string& name) const {
    return get_for("", name);
}

Session* SessionManager::get_for(const std::string& owner_id, const std::string& name) {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(session_key(owner_id, name));
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

const Session* SessionManager::get_for(const std::string& owner_id, const std::string& name) const {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(session_key(owner_id, name));
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

// ── kill ────────────────────────────────────────────────────────

void SessionManager::kill(const std::string& name) {
    kill_for("", name);
}

void SessionManager::kill_for(const std::string& owner_id, const std::string& name) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(session_key(owner_id, name));
    if (it != sessions_.end()) {
        it->second->state = SessionState::Killed;
        sessions_.erase(it);  // destructor cleans up PTY
    }
}

// ── reap_dead ───────────────────────────────────────────────────

void SessionManager::reap_dead() {
    std::unique_lock lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto* s = it->second.get();
        if (s->state == SessionState::Running || s->state == SessionState::Attached) {
#ifdef _WIN32
            if (s->child_pid && WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                s->state = SessionState::Died;
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
#else
            int status = 0;
            pid_t result = waitpid(s->child_pid, &status, WNOHANG);
            if (result == s->child_pid) {
                s->state = SessionState::Died;
                s->child_pid = -1;
#endif

                // Auto-restart logic
                if (s->auto_restart) {
                    auto now = std::chrono::steady_clock::now();
                    auto window = std::chrono::seconds(60);
                    if (now - s->restart_window_start > window) {
                        s->reset_restart_failures();
                    }
                    if (s->restart_failures < 3) {
                        ++s->restart_failures;
                        // Respawn
                        auto new_session = create_session(
                            s->name, s->command, 80, 24, "xterm-256color");
                        if (new_session) {
                            auto fresh = std::make_unique<Session>(std::move(*new_session));
                            fresh->owner_id = s->owner_id;
                            fresh->command = s->command;
                            fresh->auto_restart = true;
                            fresh->state = SessionState::Detached;
                            fresh->restart_failures = s->restart_failures;
                            fresh->restart_window_start = s->restart_window_start;
                            it->second = std::move(fresh);
                            ++it;
                            continue;
                        }
                    }
                    // Too many failures — mark as Exited
                    s->state = SessionState::Exited;
                }
            }
        }
        ++it;
    }
}

// ── prune_idle ──────────────────────────────────────────────────

void SessionManager::prune_idle(std::chrono::seconds max_idle) {
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto* s = it->second.get();
        if (s->state == SessionState::Detached) {
            auto idle = now - s->last_output_at;
            if (idle > max_idle) {
                it = sessions_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// ── count ───────────────────────────────────────────────────────

size_t SessionManager::count() const {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

// ── Persistence (Phase 10) ──────────────────────────────────────

void SessionManager::load_persisted_sessions() {
    if (persistence_path_.empty()) return;
    auto metas = load_sessions(persistence_path_);
    if (metas.empty()) return;

    std::unique_lock lock(mutex_);
    for (auto& m : metas) {
        const auto key = session_key(m.owner_id, m.name);
        if (sessions_.find(key) != sessions_.end()) continue;
        auto s = std::make_unique<Session>();
        s->name = m.name;
        s->owner_id = m.owner_id;
        s->command = m.command;
        s->state = SessionState::Recoverable;
        s->created_at = std::chrono::steady_clock::now();
        s->last_output_at = s->created_at;
        s->last_attach_at = s->created_at;
        sessions_[key] = std::move(s);
        std::cout << "loaded session '" << m.name << "' (recoverable)" << std::endl;
    }
}

bool SessionManager::save_persisted_sessions() const {
    if (persistence_path_.empty()) return true;
    std::vector<SessionMeta> metas;
    {
        std::shared_lock lock(mutex_);
        for (auto& [key, s] : sessions_) {
            SessionMeta m;
            m.name = s->name;
            m.owner_id = s->owner_id;
            m.command = s->command;
            m.state = session_state_str(s->state);
            m.created_at = std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(
                    s->created_at.time_since_epoch()).count());
            metas.push_back(m);
        }
    }
    return save_sessions(persistence_path_, metas);
}

std::expected<Session*, SessionMgrError> SessionManager::resurrect(
    const std::string& name,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    return resurrect_for("", name, cols, rows, term);
}

std::expected<Session*, SessionMgrError> SessionManager::resurrect_for(
    const std::string& owner_id,
    const std::string& name,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(session_key(owner_id, name));
    if (it == sessions_.end()) {
        return std::unexpected(SessionMgrError{"session '" + name + "' not found"});
    }
    auto* s = it->second.get();
    if (s->state != SessionState::Recoverable) {
        return std::unexpected(SessionMgrError{
            "session '" + name + "' is " + session_state_str(s->state) + ", not recoverable"});
    }

    auto session_result = create_session(name, s->command, cols, rows, term);
    if (!session_result) {
        return std::unexpected(SessionMgrError{
            "resurrect failed: " + session_result.error().message});
    }

    auto fresh = std::make_unique<Session>(std::move(*session_result));
    fresh->owner_id = owner_id;
    fresh->command = s->command;
    fresh->state = SessionState::Attached;
    auto* ptr = fresh.get();
    sessions_[session_key(owner_id, name)] = std::move(fresh);
    return ptr;
}

} // namespace bs::server
