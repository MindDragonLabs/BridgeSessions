// bs-session.h — RingBuffer + Session types for BridgeSessions
// Extracted from bs-protocol.h (R5 structural refactor, 2026-07-23)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// 0. RING BUFFER (thread-safe circular buffer for PTY scrollback)
// ────────────────────────────────────────────────────────────────────

template <size_t Capacity>
class RingBuffer {
    // Power-of-2 capacity enables mask-based indexing
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of 2");

    static constexpr size_t kMask = Capacity - 1;

    std::unique_ptr<std::array<char, Capacity>> buf_{
        std::make_unique<std::array<char, Capacity>>()};
    std::atomic<size_t> write_pos_{0};
    mutable std::shared_mutex mutex_;

public:
    RingBuffer() = default;

    // Move-only (atomics are non-copyable)
    RingBuffer(RingBuffer&& other) noexcept
        : write_pos_(other.write_pos_.load(std::memory_order_relaxed))
    {
        std::unique_lock lock(other.mutex_);
        std::memcpy(buf_->data(), other.buf_->data(), Capacity);
    }

    RingBuffer& operator=(RingBuffer&& other) noexcept {
        if (this != &other) {
            write_pos_.store(other.write_pos_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            std::scoped_lock lock(mutex_, other.mutex_);
            std::memcpy(buf_->data(), other.buf_->data(), Capacity);
        }
        return *this;
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ── Write ──────────────────────────────────────────────────
    // Append data to the buffer. Thread-safe: exclusive lock.
    void write(std::span<const char> data) {
        if (data.empty()) return;
        std::unique_lock lock(mutex_);

        size_t pos = write_pos_.load(std::memory_order_relaxed);
        size_t len = data.size();

        if (len >= Capacity) {
            // Data larger than buffer — keep only the tail. Advance the
            // absolute stream position monotonically (read_since/SCROLLBACK
            // and total_written() depend on it being total bytes ever written).
            // The retained tail MUST land at absolute-aligned slots: byte with
            // absolute position q lives at buf[q & kMask]. A flat memcpy to
            // buf[0] is only aligned when (pos+len) % Capacity == 0 — rotate
            // instead (2.0.8 MoA finding: misaligned read_since after big writes).
            auto tail = data.subspan(data.size() - Capacity);
            const size_t new_pos = pos + len;
            const size_t idx = new_pos & kMask; // slot of oldest retained byte
            const size_t first = Capacity - idx;
            std::memcpy(buf_->data() + idx, tail.data(), first);
            std::memcpy(buf_->data(), tail.data() + first, idx);
            write_pos_.store(new_pos, std::memory_order_release);
            return;
        }

        size_t idx = pos & kMask;
        size_t space_to_end = Capacity - idx;

        if (len <= space_to_end) {
            std::memcpy(buf_->data() + idx, data.data(), len);
        } else {
            std::memcpy(buf_->data() + idx, data.data(), space_to_end);
            std::memcpy(buf_->data(), data.data() + space_to_end, len - space_to_end);
        }

        write_pos_.store(pos + len, std::memory_order_release);
    }

    void write(std::string_view data) {
        write(std::span<const char>(data.data(), data.size()));
    }

    // ── Read helpers ───────────────────────────────────────────

    // Total bytes written since creation (for uptime/statistics)
    size_t total_written() const noexcept {
        return write_pos_.load(std::memory_order_acquire);
    }

    // Current content size (up to Capacity)
    size_t size() const noexcept {
        size_t wp = write_pos_.load(std::memory_order_acquire);
        return std::min(wp, Capacity);
    }

    // ── Snapshot read — full buffer copy (shared lock) ─────────
    std::vector<char> snapshot() const {
        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);

        std::vector<char> result(n);
        if (n == 0) return result;

        if (wp <= Capacity) {
            // Buffer hasn't wrapped yet
            std::memcpy(result.data(), buf_->data(), n);
        } else {
            // Wrapped — read in order from oldest to newest
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(result.data(), buf_->data() + idx, tail);
            std::memcpy(result.data() + tail, buf_->data(), idx);
        }
        return result;
    }

    // ── read_since — incremental read for IPC SCROLLBACK verb
    // Returns bytes newer than since_byte (absolute offset). If since_byte is
    // older than what the ring still holds, returns oldest available + RESET.
    // Returns at most 64 KiB. b64enc (no padding) is used by caller.
    std::pair<std::string, bool> read_since(size_t since_byte) const {
        constexpr size_t kMax = 64 * 1024;
        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        if (since_byte >= wp) return {"", false};
        size_t start = since_byte;
        bool reset = false;
        if (wp > Capacity && start < wp - Capacity) {
            start = wp - Capacity;
            reset = true;
        }
        // 2.0.8 MoA fix: when the retained window still exceeds the read cap,
        // deliver the NEWEST kMax bytes (not the oldest) so the client's
        // fast-forward to total_written() skips nothing it was shown.
        if (wp - start > kMax) {
            start = wp - kMax;
            reset = true;
        }
        size_t n = std::min(kMax, wp - start);
        std::vector<char> out(n);
        if (n == 0) return {"", reset};
        size_t idx = start & kMask;
        size_t tail = Capacity - idx;
        if (n <= tail) {
            std::memcpy(out.data(), buf_->data() + idx, n);
        } else {
            std::memcpy(out.data(), buf_->data() + idx, tail);
            std::memcpy(out.data() + tail, buf_->data(), n - tail);
        }
        return {std::string(out.data(), n), reset};
    }

    // ── read_last_lines — backward scan for N most recent lines ─
    std::string read_last_lines(size_t num_lines) const {
        if (num_lines == 0) return {};

        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);
        if (n == 0) return {};

        // Build linear view of buffer
        std::vector<char> linear(n);
        if (wp <= Capacity) {
            std::memcpy(linear.data(), buf_->data(), n);
        } else {
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(linear.data(), buf_->data() + idx, tail);
            std::memcpy(linear.data() + tail, buf_->data(), idx);
        }

        // Scan backward for newlines
        // If the last char is '\n', we need num_lines+1 transitions to skip the empty trailing line.
        // Otherwise we need exactly num_lines transitions.
        size_t target_newlines = (linear.back() == '\n') ? num_lines + 1 : num_lines;
        size_t lines_found = 0;
        ptrdiff_t cut = static_cast<ptrdiff_t>(n);
        for (ptrdiff_t i = static_cast<ptrdiff_t>(n) - 1; i >= 0; --i) {
            if (linear[i] == '\n') {
                ++lines_found;
                if (lines_found >= target_newlines) {
                    cut = i + 1;
                    break;
                }
            }
        }
        if (lines_found < target_newlines) cut = 0;

        return std::string(linear.data() + cut, n - static_cast<size_t>(cut));
    }

    // ── read_range — offset + length for chunked replay ─────────
    std::string read_range(size_t offset, size_t length) const {
        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);
        if (offset >= n) return {};

        size_t actual_len = std::min(length, n - offset);

        std::vector<char> linear(n);
        if (wp <= Capacity) {
            std::memcpy(linear.data(), buf_->data(), n);
        } else {
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(linear.data(), buf_->data() + idx, tail);
            std::memcpy(linear.data() + tail, buf_->data(), idx);
        }

        return std::string(linear.data() + offset, actual_len);
    }

    // ── Clear ─────────────────────────────────────────────────
    void clear() {
        std::unique_lock lock(mutex_);
        write_pos_.store(0, std::memory_order_release);
    }

    // ── Capacity (compile-time constant, but accessible) ─────
    static constexpr size_t capacity() noexcept { return Capacity; }
};

// ── SessionState ──────────────────────────────────────────────────

enum class SessionState : uint8_t {
    Created,
    Running,
    Detached,
    Attached,
    Died,
    Exited,
    Killed,
    Recoverable,
};

inline const char* session_state_str(SessionState s) {
    switch (s) {
        case SessionState::Created:     return "created";
        case SessionState::Running:     return "running";
        case SessionState::Detached:    return "detached";
        case SessionState::Attached:    return "attached";
        case SessionState::Died:        return "died";
        case SessionState::Exited:      return "exited";
        case SessionState::Killed:      return "killed";
        case SessionState::Recoverable: return "recoverable";
    }
    return "unknown";
}

// ── Session struct ────────────────────────────────────────────────

// Default ring buffer: 1 MB (2^20)
constexpr size_t kDefaultRingBufferSize = 1'048'576;

struct Session {
    std::string name;
    std::vector<std::string> peer_ids; // pubkey hex of all peers currently attached (empty if detached)
    // 2.0.8 multi-attach: per-connection attachments, keyed by server-assigned
    // attach_id. Distinct from peer_ids (pubkey set) so N connections from one
    // key are tracked separately (scrollback cursor, detach bookkeeping, geometry).
    struct Attachment {
        uint32_t attach_id = 0;
        uint16_t cols = 80;
        uint16_t rows = 24;
        bool spectator = false;
        std::string pubkey;
    };
    std::unordered_map<uint32_t, Attachment> attachments;
    std::string command;
    std::string detach_signal; // optional HUP/TERM/INT/KILL requested by the attaching peer
    std::string parent_id; // empty for primary sessions
    // A5: pubkey of the peer that first spawned (created) this session. Empty
    // until first attach. Used to detect foreign-owner kill attempts (logged
    // as session_kill_foreign; hard enforcement deferred pending fleet rollout).
    std::string owner_pubkey;
    // Session origin class. Set at spawn time so the panel can distinguish
    // operator interactive shells from agent/harness-spawned shells from
    // internal one-shot probes (health checks, --cmd relays). See
    // session_class() in bs-protocol.h for the classification helpers.
    enum class Kind : uint8_t { User, Harness, Probe };
    Kind kind = Kind::User;
#ifdef _WIN32
    HANDLE master_fd = nullptr;     // ConPTY output read handle (child stdout -> server)
    HANDLE child_pid = nullptr;     // process handle
    HANDLE write_handle = nullptr;  // ConPTY input write handle (server -> child stdin)
    HPCON hpcon = nullptr;          // for ResizePseudoConsole
    // A3: Job Object wrapping the child + its descendants. Closing this handle
    // (KILL_ON_JOB_CLOSE) terminates the entire tree — the ConPTY root, the
    // shell, and any grandchildren it spawned — instead of leaking orphans the
    // way TerminateProcess on the direct child does.
    HANDLE job_handle = nullptr;
#else
    int master_fd = -1;
    int child_pid = -1;
    // v2.0.6: bounded pending input queue for nonblocking PTY master writes.
    // Keystrokes/clipboard pasted faster than the child consumes are queued
    // here and drained by the event loop. High/low water marks apply
    // backpressure by temporarily skipping reads from the attached peer.
    std::string pending_input;
    bool input_backpressured = false;
    static constexpr size_t kPtyInputHighWater = 64 * 1024;
    static constexpr size_t kPtyInputLowWater  = 16 * 1024;
    static constexpr size_t kPtyInputMax       = 256 * 1024;
#endif
    SessionState state = SessionState::Created;

    RingBuffer<kDefaultRingBufferSize> scrollback;

    std::chrono::steady_clock::time_point created_at;
    std::chrono::system_clock::time_point created_at_sys; // wall-clock for persistence
    std::chrono::steady_clock::time_point last_output_at;
    std::chrono::steady_clock::time_point last_attach_at;

    bool auto_restart = false;
    int restart_failures = 0;
    std::chrono::steady_clock::time_point restart_window_start;
    bool history_recorded = false;

    // Monotonic spawn generation. Bumped every time a child process is spawned
    // (initial create + each auto-restart/resurrect). Unlike child_pid/HANDLE,
    // this never repeats, so callers/tests can reliably detect a respawn even
    // when the OS recycles the freed PID or HANDLE-table slot.
    uint64_t generation = 0;

    Session();
    ~Session();

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void touch_output();
    void reset_restart_failures();
    void release_exited_runtime();
#ifdef _WIN32
    bool is_valid() const { return master_fd != nullptr; }
    bool is_pollable() const { return master_fd != nullptr && child_pid != nullptr; }
    // A3: kill the whole child process tree by closing the Job Object
    // (KILL_ON_JOB_CLOSE). The direct child handle is left live so the
    // reaper observes its death through the normal reap_dead path.
    void kill_tree();
#else
    bool is_valid() const { return master_fd >= 0; }
    bool is_pollable() const { return master_fd >= 0 && child_pid > 0; }
#endif
};

// ── Session implementation ────────────────────────────────────────

// Process-wide monotonic spawn counter. Every successful child spawn gets a
// unique value, so a respawn is detectable even when the OS recycles the freed
// PID/HANDLE. Starts at 1 so 0 reliably means "never spawned".
static std::atomic<uint64_t> g_session_generation{0};

Session::Session()
    : created_at(std::chrono::steady_clock::now())
    , created_at_sys(std::chrono::system_clock::now())
    , last_output_at(created_at)
    , last_attach_at(created_at)
{}

Session::~Session() {
#ifdef _WIN32
    if (master_fd) {
        CloseHandle(master_fd);
        master_fd = nullptr;
    }
    if (write_handle) {
        CloseHandle(write_handle);
        write_handle = nullptr;
    }
    // A3: kill the whole process tree (job-close + taskkill /T fallback) before
    // terminating the direct child, so grandchildren never outlive the session.
    kill_tree();
    if (child_pid) {
        TerminateProcess(child_pid, 1);
        WaitForSingleObject(child_pid, 500);
        CloseHandle(child_pid);
        child_pid = nullptr;
    }
    if (hpcon) {
        ClosePseudoConsole(hpcon);
        hpcon = nullptr;
    }
#else
    if (master_fd >= 0) {
        close(master_fd);
        master_fd = -1;
    }
    if (child_pid > 0) {
        // Kill the whole process group (process group id == the forkpty
        // session-leader pid), so background jobs spawned by the shell die
        // with it instead of outliving the session as orphans.
        kill(-child_pid, SIGTERM);
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 10; ++i) {
            const pid_t waited = waitpid(child_pid, &status, WNOHANG);
            if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            usleep(50000);
        }
        if (!reaped) {
            kill(-child_pid, SIGKILL);
            while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
        }
        child_pid = -1;
    }
#endif
}

Session::Session(Session&& other) noexcept
    : name(std::move(other.name))
    , peer_ids(std::move(other.peer_ids))
    , command(std::move(other.command))
    , detach_signal(std::move(other.detach_signal))
    , parent_id(std::move(other.parent_id))
    , owner_pubkey(std::move(other.owner_pubkey))
    , kind(other.kind)
    , master_fd(other.master_fd)
    , child_pid(other.child_pid)
#ifndef _WIN32
    , pending_input(std::move(other.pending_input))
    , input_backpressured(other.input_backpressured)
#endif
    , state(other.state)
    , scrollback(std::move(other.scrollback))
    , created_at(other.created_at)
    , created_at_sys(other.created_at_sys)
    , last_output_at(other.last_output_at)
    , last_attach_at(other.last_attach_at)
    , auto_restart(other.auto_restart)
    , restart_failures(other.restart_failures)
    , restart_window_start(other.restart_window_start)
    , generation(other.generation)
{
#ifdef _WIN32
    write_handle = other.write_handle;
    hpcon = other.hpcon;
    job_handle = other.job_handle;
    other.write_handle = nullptr;
    other.hpcon = nullptr;
    other.job_handle = nullptr;
    other.master_fd = nullptr;
    other.child_pid = nullptr;
#else
    other.master_fd = -1;
    other.child_pid = -1;
#endif
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        // Reconstruct the object after releasing its current runtime. Assigning
        // into std::string/vector members after an explicit destructor call is
        // use-after-lifetime undefined behavior.
        this->~Session();
        new (this) Session(std::move(other));
    }
    return *this;
}

void Session::touch_output() {
    last_output_at = std::chrono::steady_clock::now();
}

void Session::reset_restart_failures() {
    restart_failures = 0;
    restart_window_start = std::chrono::steady_clock::now();
}

void Session::release_exited_runtime() {
#ifdef _WIN32
    if (master_fd) {
        CloseHandle(master_fd);
        master_fd = nullptr;
    }
    if (write_handle) {
        CloseHandle(write_handle);
        write_handle = nullptr;
    }
    if (hpcon) {
        ClosePseudoConsole(hpcon);
        hpcon = nullptr;
    }
    if (job_handle) {   // A3: release the job handle (tree already dead at this point)
        CloseHandle(job_handle);
        job_handle = nullptr;
    }
#else
    if (master_fd >= 0) {
        close(master_fd);
        master_fd = -1;
    }
    pending_input.clear();
    input_backpressured = false;
#endif
}

#ifdef _WIN32
// A3: kill the whole child process tree by PID, independent of Job Object
// topology.
//
// Primary: close the Job Object (KILL_ON_JOB_CLOSE) — terminates the ConPTY
// root + shell + every descendant on interactive-session hosts where the child
// is NOT pre-wrapped in a Task Scheduler job.
//
// Fallback: `taskkill /F /T /PID <child>` — kills the tree by PID, which works
// even on schtasks-SYSTEM hosts (Shadow PC, avir servers) where every spawned
// child lands in a Task Scheduler job BEFORE our AssignProcessToJobObject, so
// KILL_ON_JOB_CLOSE silently no-ops (verified 2026-08-20 on a Windows host:
// IsProcessInJob=True pre-assign, closing our job handle left ping/cmd alive).
//
// The direct child handle stays live so reap_dead observes the death and
// records exit history normally.
void Session::kill_tree() {
    // 1. Best-effort Job Object close (fast, no subprocess spawn).
    if (job_handle) {
        CloseHandle(job_handle);
        job_handle = nullptr;
    }
    // 2. Deterministic taskkill /T fallback — reaches grandchildren regardless
    //    of job ownership. taskkill is always on PATH on Windows and blocks
    //    until the tree is gone. /PID is an integer, no shell-injection surface.
    if (child_pid) {
        const DWORD pid = GetProcessId(child_pid);
        if (pid != 0) {
            std::string cmd = "taskkill /F /T /PID " + std::to_string(pid) +
                              " >nul 2>&1";
            (void)std::system(cmd.c_str());
        }
    }
}
#endif
