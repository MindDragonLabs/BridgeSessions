#pragma once

#include "session.hpp"
#include "pty.hpp"
#include "persistence.hpp"
#include <bsprotocol/message.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs::server {

constexpr const char* kSessionStatePath = "~/.bridgesessions/sessions.json";

// ── SessionManager ──────────────────────────────────────────────
//
// Thread-safe registry of named sessions. Uses std::shared_mutex for
// concurrent read access (SessionList, status queries) and exclusive
// write access (create, destroy, attach, detach).

struct SessionMgrError {
    std::string message;
};

class SessionManager {
public:
    SessionManager() = default;

    // ── Persistence path ────────────────────────────────────────
    void set_persistence_path(const std::string& path) { persistence_path_ = path; }

    // ── Attach / Create ─────────────────────────────────────────
    // Returns a pointer to the session. If session exists and is RUNNING or
    // DETACHED, reattaches. If it doesn't exist, creates a new one.
    // If it exists but is ATTACHED (single-attach mode), detaches the existing
    // attachment first.
    [[nodiscard]] std::expected<Session*, SessionMgrError> attach(
        const std::string& name,
        const std::string& command,
        uint16_t cols, uint16_t rows, const std::string& term);

    // Same logical session names are isolated per authorized client identity.
    // The owner_id is normally the client's authorized ed25519 public-key hex.
    [[nodiscard]] std::expected<Session*, SessionMgrError> attach_for(
        const std::string& owner_id,
        const std::string& name,
        const std::string& command,
        uint16_t cols, uint16_t rows, const std::string& term);

    // ── Detach ──────────────────────────────────────────────────
    // Moves session to DETACHED state. Session survives.
    void detach(const std::string& name);
    void detach_for(const std::string& owner_id, const std::string& name);

    // ── List ────────────────────────────────────────────────────
    // Returns session info for all sessions.
    [[nodiscard]] std::vector<bs::protocol::SessionInfo> list() const;
    [[nodiscard]] std::vector<bs::protocol::SessionInfo> list_for(const std::string& owner_id) const;

    // ── Get Session ─────────────────────────────────────────────
    [[nodiscard]] Session* get(const std::string& name);
    [[nodiscard]] const Session* get(const std::string& name) const;
    [[nodiscard]] Session* get_for(const std::string& owner_id, const std::string& name);
    [[nodiscard]] const Session* get_for(const std::string& owner_id, const std::string& name) const;

    // ── Kill session ────────────────────────────────────────────
    void kill(const std::string& name);
    void kill_for(const std::string& owner_id, const std::string& name);

    // ── Reap dead children ──────────────────────────────────────
    // Checks all sessions for exited children, updates state.
    void reap_dead();

    // ── Idle timeout cleanup ────────────────────────────────────
    // Kills sessions idle longer than timeout (7 days default).
    void prune_idle(std::chrono::seconds max_idle = std::chrono::hours(7 * 24));

    // ── Size ────────────────────────────────────────────────────
    [[nodiscard]] size_t count() const;

    // ── Persistence (Phase 10) ──────────────────────────────────
    // Load sessions from JSON and mark as RECOVERABLE
    void load_persisted_sessions();

    // Save all sessions to JSON (including RECOVERABLE sessions loaded from disk)
    [[nodiscard]] bool save_persisted_sessions() const;

    // Resurrect a RECOVERABLE session by spawning its PTY
    [[nodiscard]] std::expected<Session*, SessionMgrError> resurrect(
        const std::string& name,
        uint16_t cols = 80, uint16_t rows = 24, const std::string& term = "xterm-256color");
    [[nodiscard]] std::expected<Session*, SessionMgrError> resurrect_for(
        const std::string& owner_id,
        const std::string& name,
        uint16_t cols = 80, uint16_t rows = 24, const std::string& term = "xterm-256color");

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::string persistence_path_;

    static std::string session_key(const std::string& owner_id, const std::string& name);

    // Command resolution (ADR-007)
    static std::string resolve_command(const std::string& from_client);
};

} // namespace bs::server
