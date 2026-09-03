// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-conn.h — MeshController types, members, hello/handshake, WebRTC/DHT handlers
// Extracted from bs-mesh-controller.h (R6b structural refactor, 2026-09-03)
// Designed for inclusion inside `class MeshController { ... }`
// Does NOT open its own namespace or class — parent file provides it.
#pragma once

public:
    enum class ConnectionPurpose : uint8_t {
        Unknown,
        Mesh,
        DirectSession,
    };

    struct FileReceiveState {
        std::string filename;
        std::string path;          // full output path
        std::string checksum;      // expected SHA-256
        uint64_t expected_size = 0;
        uint64_t received_bytes = 0;
        uint32_t total_chunks = 0;
        uint32_t received_chunks = 0;
        size_t chunk_raw_size = kTransferChunkRawSizeDefault;
        std::ofstream file;
        // Fresh transfers hash incrementally as chunks arrive, avoiding an
        // O(file-size) verification pass on the single-threaded event loop.
        // Resumed transfers fall back to a final streaming re-hash because an
        // EVP digest state cannot be reconstructed from the sidecar.
        std::unique_ptr<Sha256Stream> hasher;
        bool active = false;
    };

    struct Conn {
        std::string peer_name;
        std::string peer_pubkey;
        std::string peer_addr;
        SslPtr ssl;
        SOCKET sock_fd = INVALID_SOCKET;
        bool is_outbound = false;
        ConnectionPurpose purpose = ConnectionPurpose::Mesh;
        std::chrono::steady_clock::time_point last_pong;
        // B1: RTT-aware pong deadline. ping_sent_at is stamped when we enqueue
        // a PingMsg; on Pong receipt we derive pong_rtt_ms. check_pong_timeouts
        // then uses max(base, 4×rtt) so healthy WAN peers (144ms RTT) are not
        // spuriously dropped when the pong lands just past a tight static window.
        std::chrono::steady_clock::time_point ping_sent_at{};
        std::chrono::milliseconds pong_rtt_ms{0};
        std::chrono::steady_clock::time_point connected_at = std::chrono::steady_clock::now();
        uint64_t bytes_in = 0;
        uint64_t bytes_out = 0;
        std::vector<uint8_t> rx_buffer;
        Session* attached_session = nullptr;
        std::string remote_session;
        // 2.0.8 multi-attach: per-connection attach id assigned by the server,
        // plus the spectator flag. detach() is keyed by attach_id so N connections
        // from one pubkey detach independently.
        uint32_t attach_id = 0;
        bool spectator = false;
        // Delta gossip: generation this connection last received. 0 = needs
        // full snapshot on next broadcast.
        uint32_t last_gossip_generation = 0;
        // v1.7 fix (Known Issue #2): set while a background thread owns this
        // conn's socket/SSL object for a one-shot `daemon_shell_exec` relay.
        // The main event loop must not select()/read/write this fd while
        // busy — the exec thread has exclusive access — otherwise two
        // threads touch the same SSL* concurrently and corrupt the TLS
        // record stream. Also used to skip ping/pong bookkeeping so a
        // long-running exec doesn't get treated as a stalled peer.
        std::shared_ptr<std::atomic<bool>> exec_busy = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> exec_completed = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> exec_cancelled = std::make_shared<std::atomic<bool>>(false);
        bool heartbeat_suspended_for_busy = false;
        // A detached exec worker may own ssl/sock_fd. Close paths mark this
        // and defer destruction until exec_busy is released.
        // v2.0.1: timestamp when an exec/transfer began,
        // so check_stale_exec() can force-release a stuck exec_busy flag
        // if the CLI timed out and the worker thread outlived its caller.
        std::chrono::steady_clock::time_point exec_started_at = {};
        // Shared "last progress" timestamp used by the exec watchdog. Refreshed
        // on each transfer progress tick so a healthy long transfer is not killed
        // at the 90s deadline; a stalled transfer (no progress for 90s) still trips.
        std::shared_ptr<std::atomic<std::chrono::steady_clock::time_point::rep>>
            exec_last_progress_at =
                std::make_shared<std::atomic<std::chrono::steady_clock::time_point::rep>>(0);
        bool close_requested = false;
        FileReceiveState file_receive;
        std::string pending_recv_dir;
        // Initial handshake Hello. Later Hello frames are ignored only if
        // identical; any mismatch closes the connection.
        std::optional<HelloMsg> initial_hello;
        // Remote peer version, populated from Hello or ServerInfo gossip.
        std::string remote_version;
        // Last host metrics from ServerInfo (cpu/mem/disk JSON); empty if peer
        // has not advertised them yet (older builds).
        std::string remote_host_stats_json;
        double remote_load = -1.0;
        // ── 2.0.8 P3 per-connection output queue ──────────────────────
        // When a fanout write fails (slow client, full socket buffer), the
        // OutputMsg is enqueued here instead of silently dropped. The event
        // loop drains queues after PTY polling. If the queue exceeds the
        // high-water mark, oldest messages are dropped and an OutputGap is
        // emitted so the client knows data was lost.
        static constexpr size_t kOutputQueueHighWater = 256;
        struct QueuedOutput { std::string data; bool render_markdown = false; };
        std::deque<QueuedOutput> output_queue;
        uint64_t output_dropped_bytes = 0;
        bool output_gap_pending = false;

        // ── Non-blocking control-plane TX queue ──────────────────────
        // Encoded frames (ping/gossip/acks/output) wait here instead of
        // blocking the event loop in write_frame() up to 30s. Drained when
        // select() reports the socket writable (or immediately on enqueue).
        static constexpr size_t kTxQueueHighWaterBytes = 8u * 1024u * 1024u;
        struct QueuedTxFrame {
            std::vector<uint8_t> data;
            size_t offset = 0;
        };
        std::deque<QueuedTxFrame> tx_queue;
        size_t tx_queue_bytes = 0;
        bool want_write = false;
    };

    // Return 0 to drop the older connection (i), 1 to drop the newer one (j).
    // Same-direction reconnects replace stale clients immediately.
    static size_t duplicate_index_to_drop(bool i_matches, bool j_matches) {
        if (i_matches && !j_matches) return 1;
        return 0;
    }

    static bool connections_are_mesh_duplicates(const Conn& a, const Conn& b) {
        return a.purpose == ConnectionPurpose::Mesh &&
               b.purpose == ConnectionPurpose::Mesh &&
               !a.peer_pubkey.empty() &&
               a.peer_pubkey == b.peer_pubkey;
    }

    static bool is_live_mesh_transport_for(const Conn& conn,
                                           const std::string& peer_name,
                                           bool require_idle = true) {
        const bool transfer_reserved = !conn.pending_recv_dir.empty() ||
                                       conn.file_receive.active;
        return conn.purpose == ConnectionPurpose::Mesh &&
               conn.sock_fd != INVALID_SOCKET &&
               (!require_idle || ((!conn.exec_busy || !conn.exec_busy->load()) &&
                                  !transfer_reserved)) &&
               peer_name_eq(conn.peer_name, peer_name);
    }

    static bool refresh_heartbeat_after_busy(
            Conn& conn, std::chrono::steady_clock::time_point now) {
        if (conn.exec_busy && conn.exec_busy->load()) {
            conn.heartbeat_suspended_for_busy = true;
            return true;
        }
        const bool completed = conn.exec_completed && conn.exec_completed->exchange(false);
        if (completed || conn.heartbeat_suspended_for_busy) {
            conn.heartbeat_suspended_for_busy = false;
            conn.last_pong = now;
            return true;
        }
        return false;
    }

    static std::string shell_ipc_relay_policy_response() {
        // One-shot shell commands use their own direct TLS transport. Sharing a
        // mesh Conn with a detached IPC worker lets the event loop and worker
        // race the same SSL object during reconnect/duplicate cleanup.
        return "ERROR direct TLS required\n";
    }

    static bool should_fallback_to_direct_shell(
            int ipc_result, const std::string& output) {
        return ipc_result == -1 &&
               (output.empty() || output == "direct TLS required");
    }

#ifdef BS_TESTING
    void close_conn_for_test(Conn& conn) { (void)close_conn(conn); }

    // Test accessors for invite/join regression tests
    size_t test_pending_invite_count() const {
        std::lock_guard lock(invite_mutex_);
        return pending_invites_.size();
    }
    bool test_has_unclaimed_invite(const std::string& token) const {
        std::lock_guard lock(invite_mutex_);
        return std::any_of(pending_invites_.begin(), pending_invites_.end(),
            [&](const auto& p) { return p.token == token && p.claimed_by.empty(); });
    }
    void test_add_invite(
            const std::string& token,
            std::chrono::seconds age = std::chrono::seconds(0)) {
        std::lock_guard lock(invite_mutex_);
        const auto now = std::chrono::steady_clock::now();
        PendingInvite pi;
        pi.token = token;
        pi.created_at = now - age;
        pending_invites_.push_back(std::move(pi));
        open_join_window_locked(now);
    }
    JoinReplyMsg test_process_join(const JoinRequestMsg& jr, const std::string& peer_pk) {
        return process_join_request(jr, peer_pk);
    }
    void test_close_join_window() { maybe_close_join_window(); }
    bool test_join_window_open() const {
        return allow_join_connections_.load(std::memory_order_relaxed);
    }
    void test_age_join_window(std::chrono::seconds age) {
        std::lock_guard lock(invite_mutex_);
        join_window_opened_at_ = std::chrono::steady_clock::now() - age;
    }
    [[nodiscard]] uint64_t test_invite_expired_event_count() const {
        std::lock_guard lock(invite_mutex_);
        return invite_expired_event_count_;
    }
    // Bootstrap enrollment test hooks.
    DirectoryEnrollMsg test_make_enroll(const std::string& name,
                                        const std::string& pk, const std::string& addr) {
        return make_directory_enroll(name, pk, addr);
    }
    bool test_apply_enroll(const DirectoryEnrollMsg& e) {
        return apply_directory_enroll(e);
    }
    // True if pubkey is present in the authorized_keys file on disk.
    bool test_authorized_on_disk(const std::string& pubkey_hex) const {
        std::ifstream f(config_.authorized_keys_path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "pubkey " + pubkey_hex || line == pubkey_hex) return true;
        }
        return false;
    }
#endif

private:
    MeshConfig config_;
    SessionRegistry sessions_;
    // ── 2.0.8 P4 conversation store ──────────────────────────────
    std::unordered_map<std::string, std::vector<ConversationAppendMsg>> conversations_;
    std::mutex conversations_mutex_;
    uint64_t next_conv_seq_ = 1;
    // ── 2.0.9 join/invite ────────────────────────────────────────
    struct PendingInvite {
        std::string token;
        std::chrono::steady_clock::time_point created_at;
        std::string claimed_by;
    };
    std::vector<PendingInvite> pending_invites_;
    mutable std::mutex invite_mutex_;
    std::chrono::steady_clock::time_point join_window_opened_at_{};
    std::atomic<bool> allow_join_connections_{false};
    uint64_t invite_expired_event_count_ = 0;
    // ── Bootstrap enrollment dedupe (flood bound) ─────────────────
    std::unordered_set<std::string> enroll_seen_;
    std::mutex enroll_seen_mutex_;
    // ── BridgePanel v3 mesh plane ────────────────────────────────
    // Pre-rendered JSON arrays of session summaries per peer, populated by
    // session gossip (ServerInfoMsg trailing field). Empty until gossip lands.
    std::unordered_map<std::string, std::string> gossip_sessions_json_;
    std::shared_mutex gossip_sessions_mutex_;
    // R8.3: own the TLS cert-verify callback contexts so they are freed with the
    // controller instead of leaking via `new`. MUST be declared BEFORE the
    // SSL_CTX pointers below — members destruct in reverse declaration order, so
    // declaring these first means they are destroyed AFTER tls_listen_/tls_connect_,
    // guaranteeing the SSL_CTX never references freed callback storage.
    AuthorizedKeys authorized_keys_;
    std::function<bool(const std::string&)> tofu_cb_;
    SslCtxPtr tls_listen_;
    SslCtxPtr tls_connect_;
    std::string our_pubkey_;
    std::string home_dir_;
    std::string receive_dir_ = "~/.bridgesessions/received";

    // R8.4: `conns_` is touched only from MeshController::run()'s single-threaded
    // event loop and from CLI methods that run before/after the loop — never
    // concurrently from multiple threads. Long-operation workers (v2.0.6) capture
    // SSL* / SOCKET while exec_busy is set; the event loop skips busy conns.
    std::vector<Conn> conns_;
    static constexpr size_t kMaxConnections = 64;

    // Backoff state per seed
    struct Backoff {
        int delay_ms = 100;
        int max_ms = 30000;
        int attempt = 0;
        std::chrono::steady_clock::time_point next_attempt{};
    };
    std::unordered_map<std::string, Backoff> backoffs_;

    // B2: dead-seed cooldown. Independent of the exponential retry Backoff
    // above — that struct is erased optimistically the moment a dial is
    // *started* (see try_connect_to_seeds), so it can't carry a reliable
    // consecutive-failure streak. This tracks handshake_deadline failures
    // (real timeouts, not immediate connect() refusals) per addr and, once
    // a seed has strung together kDeadSeedCooldownThreshold of them in a
    // row, stops scheduling dials to it for a jittered cooldown window.
    struct DeadSeedCooldown {
        int consecutive_deadline_failures = 0;
        bool in_cooldown = false;
        std::chrono::steady_clock::time_point cooldown_until{};
    };
    std::unordered_map<std::string, DeadSeedCooldown> seed_cooldowns_;
    static constexpr int kDeadSeedCooldownThreshold = 3;
    static constexpr int kDeadSeedCooldownBaseSecs = 600; // 10 min

    // B3: per-source-IP hourly counter for non-BS port-scan noise. Key is the
    // source IP (no port); value is {last_full_log_hour, suppressed_count}.
    struct NonBsClientBucket {
        int64_t hour = 0;      // unix hour (wall clock) of last full log
        uint64_t suppressed = 0;
    };
    std::unordered_map<std::string, NonBsClientBucket> nonbs_clients_;
    static constexpr size_t kMaxNonBsClientBuckets = 4096;

    // B3: classify inbound TLS probe noise. ssl_err=1 without a BS Hello on an
    // inbound handshake is a non-BS scanner; log first occurrence per IP/hour
    // in full, suppress the rest to a counter.
    void note_nonbs_client(const std::string& contact_addr) {
        std::string ip = contact_addr;
        auto colon = ip.rfind(':');
        if (colon != std::string::npos) ip.resize(colon);
        const int64_t hour = static_cast<int64_t>(std::time(nullptr)) / 3600;
        auto& b = nonbs_clients_[ip];
        if (nonbs_clients_.size() > kMaxNonBsClientBuckets && b.hour == 0) {
            nonbs_clients_.erase(nonbs_clients_.begin());  // bound growth
        }
        if (b.hour != hour) {
            if (b.suppressed > 0) {
                log_event("tls_nonbs_client", ip + " suppressed=" +
                          std::to_string(b.suppressed));
            }
            b.hour = hour;
            b.suppressed = 0;
            log_event("tls_nonbs_client", ip);
        } else {
            ++b.suppressed;
        }
    }

    // Seeded RNG for reconnect jitter (P2 audit fix — replaces global rand()).
    std::mt19937 rng_{std::random_device{}()};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> accept_only_until_;
    // Tie-break defer extension budget per addr (collision-avoidance fix): how many
    // times the 12s accept-only window may be exponentially extended before one
    // straight probe dial is allowed. 5 extends → longest defer ≈ 12s·2⁵ = 384s.
    static constexpr int kTieBreakMaxExtends = 5;
    std::unordered_map<std::string, int> tiebreak_probe_extends_;
    static constexpr int kTieBreakAcceptWindowMs = 12000;
    static constexpr int kForcedReconnectDeadlineMs = 20000;

    // ── Non-blocking TLS + Hello handshake state ────────────────────
    struct PendingHandshake {
        enum class State {
            TcpConnect,
            TlsHandshake,
            ReadHello,
            WriteHello,   // outbound: sent our Hello, waiting for reply
            ReadJoinRequest, // server-side join window: read JoinRequest after Hello reply
            Done,
            Failed
        };
        SOCKET sock_fd = INVALID_SOCKET;
        SslPtr ssl;
        bool server_side = false;
        State state = State::TlsHandshake;
        std::chrono::steady_clock::time_point deadline;
        std::string expected_addr;        // for outbound: seed addr
        std::string expected_pubkey;      // for outbound: pinned pubkey
        std::string expected_name;        // for outbound: pinned name
        std::vector<uint8_t> rx_buffer;
        std::vector<uint8_t> tx_buffer;   // buffered outbound Hello
        HelloMsg outbound_hello;          // client side: our Hello already built
        HelloMsg peer_hello;              // authenticated peer Hello retained across partial writes
        std::string peer_pk;              // cert pubkey once TLS completes
        std::string contact_addr;         // inbound: peer ip:port (for JoinReply host_addr)
        bool hello_written = false;       // join path: Hello reply written to peer
        bool want_read = true;
        bool want_write = false;
    };
    std::vector<PendingHandshake> pending_handshakes_;
    static constexpr size_t kMaxPendingHandshakes = 16;


    // Shutdown flag for event loop
    std::atomic<bool> running_{false};

    // Last gossip/ping/mdns broadcast times
    std::chrono::steady_clock::time_point last_ping_time_;
    std::chrono::steady_clock::time_point last_gossip_time_;
    std::chrono::steady_clock::time_point last_mdns_time_;
    std::chrono::steady_clock::time_point last_session_prune_time_{};
    std::chrono::steady_clock::time_point last_discovered_prune_time_{};
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
    // mDNS LAN discovery
    SOCKET mdns_fd_ = INVALID_SOCKET;
    static constexpr const char* kMdnsGroup = "224.0.0.252";
    static constexpr uint16_t kMdnsPort = 19949;

    // Listen socket
    SOCKET listen_fd_ = INVALID_SOCKET;
    std::atomic<uint16_t> actual_listen_port_{0};

    std::string config_file_path_;
    // Delta gossip: bumped whenever the peer set changes so build_gossip
    // can send only what changed since a connection's last snapshot.
    std::atomic<uint32_t> gossip_generation_{1};
    std::chrono::steady_clock::time_point last_config_reload_check_{};
    std::chrono::steady_clock::time_point last_authorization_check_{};
    std::filesystem::file_time_type config_mtime_{};
    bool config_mtime_set_ = false;

    int outbound_connect_timeout_ms_ = kConnectTimeoutMs;
    SOCKET cli_listen_fd_ = INVALID_SOCKET;
    std::string ipc_token_;
    std::string ipc_token_path_;

    // D15: WebRTC transport
#ifndef BS_NO_WEBRTC
    std::unordered_map<std::string, WebRtcChannel> webrtc_channels_;
    mutable std::mutex webrtc_mutex_;
#endif

    // D16: DHT node
#ifndef BS_NO_DHT
    DhtNode dht_;
    bool dht_inited_ = false;
#endif

    // D17: NAT traversal
#ifndef BS_NO_NAT
    UpnpNat upnp_;
    std::string external_addr_;
#endif

    // v2.0.6: bounded worker pool for long file/edit/vfolder operations.
    std::optional<LongOperationWorkerPool> worker_pool_;
    static constexpr size_t kLongOperationWorkers = 2;

    // Auto-upgrade: last attempt time per peer (cooldown).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> auto_upgrade_last_;

    // ── Transfer telemetry ────────────────────────────────────────────
    TransferTelemetryRing transfer_telemetry_;

#ifdef _WIN32
    struct WindowsPtyWriteTask {
        HANDLE handle = nullptr;
        std::string data;
    };
    std::mutex windows_pty_mutex_;
    std::condition_variable windows_pty_cv_;
    std::queue<WindowsPtyWriteTask> windows_pty_queue_;
    std::thread windows_pty_writer_;
    bool windows_pty_stop_ = false;
    std::atomic<size_t> windows_pty_pending_bytes_{0};
    static constexpr size_t kWindowsPtyInputHighWater = 64 * 1024;
    static constexpr size_t kWindowsPtyInputMax = 256 * 1024;

    void windows_pty_writer_loop() {
        for (;;) {
            WindowsPtyWriteTask task;
            {
                std::unique_lock lock(windows_pty_mutex_);
                windows_pty_cv_.wait(lock, [this] {
                    return windows_pty_stop_ || !windows_pty_queue_.empty();
                });
                if (windows_pty_stop_) return;
                task = std::move(windows_pty_queue_.front());
                windows_pty_queue_.pop();
            }
            size_t offset = 0;
            while (offset < task.data.size()) {
                DWORD wrote = 0;
                if (!WriteFile(task.handle, task.data.data() + offset,
                               static_cast<DWORD>(task.data.size() - offset),
                               &wrote, nullptr) || wrote == 0) {
                    break;
                }
                offset += wrote;
            }
            windows_pty_pending_bytes_.fetch_sub(task.data.size());
            CloseHandle(task.handle);
        }
    }

    bool enqueue_windows_pty_input(Session& session, std::string_view data) {
        if (!session.write_handle || data.empty()) return data.empty();
        const size_t pending = windows_pty_pending_bytes_.load();
        if (data.size() > kWindowsPtyInputMax ||
            pending > kWindowsPtyInputMax - data.size()) {
            log_event("pty_input_overflow", session.name);
            return false;
        }
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), session.write_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            log_event("pty_input_duplicate_failed", session.name);
            return false;
        }
        {
            std::lock_guard lock(windows_pty_mutex_);
            if (windows_pty_stop_) {
                CloseHandle(duplicate);
                return false;
            }
            windows_pty_pending_bytes_.fetch_add(data.size());
            windows_pty_queue_.push(WindowsPtyWriteTask{
                duplicate, std::string(data)});
            if (!windows_pty_writer_.joinable()) {
                windows_pty_writer_ = std::thread([this] {
                    windows_pty_writer_loop();
                });
            }
        }
        windows_pty_cv_.notify_one();
        return true;
    }

    void shutdown_windows_pty_writer() {
        {
            std::lock_guard lock(windows_pty_mutex_);
            windows_pty_stop_ = true;
        }
        windows_pty_cv_.notify_all();
        if (windows_pty_writer_.joinable()) {
            CancelSynchronousIo(reinterpret_cast<HANDLE>(
                windows_pty_writer_.native_handle()));
            windows_pty_writer_.join();
        }
        while (!windows_pty_queue_.empty()) {
            CloseHandle(windows_pty_queue_.front().handle);
            windows_pty_queue_.pop();
        }
        windows_pty_pending_bytes_.store(0);
    }
#endif

    // ── Internal helpers ───────────────────────────────────────

    // Resolve "host:port" → sockaddr_in
    static sockaddr_in resolve_addr(const std::string& addr) {
        auto colon = addr.rfind(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("invalid addr (no port): " + addr);
        }
        std::string host = addr.substr(0, colon);
        std::string port_str = addr.substr(colon + 1);
        int port = std::stoi(port_str);

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));

        if (host == "localhost") host = "127.0.0.1";
        if (host.empty()) host = "127.0.0.1";

        // P1 fix: use getaddrinfo instead of gethostbyname (thread-safe, non-blocking)
        if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) <= 0) {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int gai_rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
            if (gai_rc != 0 || !res) {
#ifdef _WIN32
                // MinGW with -municode maps gai_strerror to the wide-char version.
                // Use gai_strerrorA explicitly for ANSI output.
                std::string err = gai_rc != 0 ? gai_strerrorA(gai_rc) : "no results";
#else
                std::string err = gai_rc != 0 ? gai_strerror(gai_rc) : "no results";
#endif
                throw std::runtime_error("DNS resolution failed for '" + host + "': " + err);
            }
            sa.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        return sa;
    }

    // Check if we already have a conn to a peer (by pubkey)
    bool has_conn_for_pubkey(const std::string& pubkey_hex) const {
        for (auto& c : conns_) {
            if (c.purpose == ConnectionPurpose::Mesh &&
                c.peer_pubkey == pubkey_hex && c.sock_fd != INVALID_SOCKET)
                return true;
        }
        return false;
    }

    // Check if we already have a conn to a peer (by addr)
    bool has_conn_for_addr(const std::string& addr) const {
        for (auto& c : conns_) {
            if (c.peer_addr == addr && c.sock_fd != INVALID_SOCKET)
                return true;
        }
        return false;
    }

    static std::string ascii_lower(std::string s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
        }
        return s;
    }

    bool has_conn_for_peer(const std::string& peer_name,
                           const std::string& addr,
                           const std::string& pubkey_hex) const {
        for (const auto& c : conns_) {
            if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
            if (!pubkey_hex.empty() && c.peer_pubkey == pubkey_hex) return true;
            if (!peer_name.empty() && peer_name_eq(c.peer_name, peer_name)) return true;
            if (!addr.empty() && c.peer_addr == addr) return true;
        }
        return false;
    }

    std::string peer_listen_addr_for(const std::string& peer_name,
                                     const std::string& pubkey_hex) const {
        for (const auto& s : config_.seeds) {
            if ((!pubkey_hex.empty() && s.pubkey_hex == pubkey_hex) ||
                (!peer_name.empty() && peer_name_eq(s.name, peer_name))) {
                return s.addr;
            }
        }
        for (const auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            if ((!pubkey_hex.empty() && d.pubkey_hex == pubkey_hex) ||
                (!peer_name.empty() && peer_name_eq(d.name, peer_name))) {
                return d.addr;
            }
        }
        return "";
    }

    void clear_accept_only_for(const std::string& peer_name,
                               const std::string& addr,
                               const std::string& pubkey_hex) {
        if (!addr.empty()) { accept_only_until_.erase(addr); tiebreak_probe_extends_.erase(addr); }
        std::string listen_addr = peer_listen_addr_for(peer_name, pubkey_hex);
        if (!listen_addr.empty()) {
            accept_only_until_.erase(listen_addr);
            tiebreak_probe_extends_.erase(listen_addr);
        }
    }

    bool should_accept_only_for(const PeerEntry& peer) const {
        if (!our_pubkey_.empty() && !peer.pubkey_hex.empty() && our_pubkey_ != peer.pubkey_hex) {
            return our_pubkey_ > peer.pubkey_hex;
        }
        if (!config_.node_name.empty() && !peer.name.empty() &&
            !peer_name_eq(config_.node_name, peer.name)) {
            return ascii_lower(config_.node_name) > ascii_lower(peer.name);
        }
        return false;
    }

    bool should_defer_outbound_for(const PeerEntry& peer,
                                   std::chrono::steady_clock::time_point now) {
        if (peer.addr.empty() || !should_accept_only_for(peer)) return false;
        auto it = accept_only_until_.find(peer.addr);
        if (it == accept_only_until_.end()) {
            accept_only_until_[peer.addr] = now + std::chrono::milliseconds(kTieBreakAcceptWindowMs);
            log_event("peer_dial_deferred",
                      peer.name + " addr=" + peer.addr + " accept_only_ms=" +
                      std::to_string(kTieBreakAcceptWindowMs));
            return true;
        }
        if (now >= it->second) {
            // Tie-break defer expired without an inbound from this peer. Extend the
            // defer exponentially (12s → 24s → 48s … capped) instead of dialing into
            // a probable simultaneous collision. The smaller-pubkey side (which never
            // defers) still originates the connection; if the peer is genuinely gone,
            // the operator-facing `bs reconnect <peer>` path or the cap keeps this
            // bounded, and any inbound handshake clears the defer immediately
            // (clear_accept_only_for on promote). One straight dial is allowed per
            // extension so a stuck smaller side still gets probed eventually.
            auto& ext = tiebreak_probe_extends_[peer.addr];
            if (ext < kTieBreakMaxExtends) {
                ++ext;
                const int window_ms = static_cast<int>(kTieBreakAcceptWindowMs) << ext;
                accept_only_until_[peer.addr] =
                    now + std::chrono::milliseconds(window_ms);
                log_event("peer_dial_defer_extended",
                          peer.name + " addr=" + peer.addr + " accept_only_ms=" +
                          std::to_string(window_ms) + " extend=" + std::to_string(ext));
                return true;
            }
            // Budget exhausted: allow exactly one probe now, then re-defer fresh.
            tiebreak_probe_extends_.erase(peer.addr);
            accept_only_until_[peer.addr] =
                now + std::chrono::milliseconds(kTieBreakAcceptWindowMs);
            log_event("peer_dial_defer_probe",
                      peer.name + " addr=" + peer.addr);
            return false;
        }
        return true;
    }

    // Find conn index by sock_fd
    int find_conn_index(SOCKET fd) const {
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == fd) return static_cast<int>(i);
        }
        return -1;
    }

    // Remove a connection and clean up. A detached worker can temporarily own
    // the TLS transport; in that case keep the Conn object alive until the
    // worker releases exec_busy.
    bool remove_conn(size_t index) {
        if (index >= conns_.size()) return false;
        auto& c = conns_[index];
        if (!close_conn(c)) return false;
        // Remove backoff for this peer so it can be reconnected
        if (!c.peer_addr.empty()) {
            backoffs_.erase(c.peer_addr);
        }
        conns_.erase(conns_.begin() + static_cast<ptrdiff_t>(index));
        return true;
    }

    bool has_replacement_transport(const Conn& c) const {
        if (!c.attached_session) return false;
        for (const auto& other : conns_) {
            if (&other == &c || other.sock_fd == INVALID_SOCKET) continue;
            if (other.attached_session == c.attached_session &&
                other.peer_pubkey == c.peer_pubkey) {
                return true;
            }
        }
        return false;
    }

    // TLS shutdown + socket close; idempotent (safe if already INVALID_SOCKET).
    // A background exec worker has exclusive SSL ownership while exec_busy is
    // true. Never free or shutdown that transport from the event-loop thread.
    bool close_conn(Conn& c) {
        if (c.exec_busy && c.exec_busy->load()) {
            c.close_requested = true;
            return false;
        }
        c.close_requested = false;
        c.pending_recv_dir.clear();
        if (c.file_receive.active) {
            // Preserve .part + .bsmeta across a transport drop so the sender can
            // reconnect and resume. Validation/write failures remove them at the
            // failure site; a normal connection close is not data corruption.
            c.file_receive.file.close();
            c.file_receive.hasher.reset();
            c.file_receive.active = false;
        }
        if (c.attached_session) {
            detach_connection_session(c, has_replacement_transport(c));
        }
        if (!c.peer_name.empty()) {
            std::unique_lock lock(gossip_sessions_mutex_);
            gossip_sessions_json_.erase(c.peer_name);
        }
        if (c.sock_fd == INVALID_SOCKET) return true;
        ssl_close(c.ssl.get(), c.sock_fd);
        c.sock_fd = INVALID_SOCKET;
        return true;
    }

    // ── Hello exchange ─────────────────────────────────────────

    // Build a HelloMsg with our info + all known peers
    HelloMsg build_hello() const {
        HelloMsg h;
        h.node_name = config_.node_name;
        // Advertise wire capabilities (+frm2 = u32 frame length / large chunks).
        h.version = version_string_with_local_caps();
        h.pubkey_hex = our_pubkey_;

        // Add seeds as known peers. Only gossip peers with pubkeys; empty pubkey
        // seed entries make Hello frames incompatible with 8-bit field lengths and
        // are not useful for auth/routing.
        for (auto& s : config_.seeds) {
            if (s.pubkey_hex.empty()) continue;
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            h.known_peers.push_back(std::move(pi));
        }

        // Add discovered peers only while their key remains explicitly trusted.
        for (auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            PeerInfo pi;
            pi.name = d.name;
            pi.addr = d.addr;
            pi.pubkey_hex = d.pubkey_hex;
            pi.last_seen = d.last_seen;
            h.known_peers.push_back(std::move(pi));
        }

        // Add already connected peers
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            PeerInfo pi;
            pi.name = c.peer_name;
            pi.addr = c.peer_addr;
            pi.pubkey_hex = c.peer_pubkey;
            pi.last_seen = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            h.known_peers.push_back(std::move(pi));
        }

        return h;
    }

    // Cached trust check for const/read-only paths. Runtime discovery paths call
    // is_trusted_pubkey(), which reloads authorized_keys first so revocations
    // take effect before accepting an address update.
    bool is_trusted_pubkey_cached(const std::string& pubkey_hex) const {
        if (pubkey_hex.empty()) return false;
        for (const auto& s : config_.seeds) {
            if (!s.pubkey_hex.empty() && s.pubkey_hex == pubkey_hex) return true;
        }
        std::vector<uint8_t> raw = hex_decode(pubkey_hex);
        if (raw.size() == 32 && authorized_keys_.contains(raw)) return true;
        return false;
    }

    bool is_trusted_pubkey(const std::string& pubkey_hex) {
        authorized_keys_.reload();
        return is_trusted_pubkey_cached(pubkey_hex);
    }

    void prune_revoked_connections() {
        // Skip pruning when join window is open — joining peers are not yet
        // in authorized_keys. The JoinRequest handler adds them on success.
        // Window transitions are logged by open/close_join_window_locked(), so
        // this once-per-second path remains silent while onboarding is active.
        if (allow_join_connections_.load(std::memory_order_relaxed)) {
            return;
        }
        authorized_keys_.reload();
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET || c.peer_pubkey.empty() ||
                is_trusted_pubkey_cached(c.peer_pubkey)) {
                continue;
            }
            if (c.exec_cancelled) c.exec_cancelled->store(true);
            log_event("mesh_peer_revoked", c.peer_name);
            close_conn(c);
        }
    }

    void maybe_prune_revoked_connections() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_authorization_check_ < std::chrono::seconds(1)) return;
        last_authorization_check_ = now;
        prune_revoked_connections();
    }

    static uint64_t now_unix_seconds() {
        return static_cast<uint64_t>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    // Merge peers from Hello or Gossip into discovered. New peers are added only
    // when their announced pubkey is explicitly trusted (pinned seed or
    // authorized_keys). Existing discovered entries are updated only while the
    // pubkey remains trusted. Untrusted announcements are dropped and never
    // persisted.
    void merge_peers(const std::vector<PeerInfo>& peers) {
        bool changed = false;
        for (auto& p : peers) {
            if (p.pubkey_hex.empty()) continue;          // require identity
            if (p.pubkey_hex == our_pubkey_) continue;   // skip self

            if (!is_trusted_pubkey(p.pubkey_hex)) continue;

            // A seed is trusted only when its configured pin exactly matches.
            // Never learn a missing seed pin from gossip/Hello.
            // Do NOT overwrite seed.addr from gossip/Hello: inbound peers often
            // present an ephemeral source port, which used to corrupt the
            // configured listen address (broke fleet display + redials).
            bool is_seed = false;
            for (auto& s : config_.seeds) {
                if (!s.pubkey_hex.empty() && s.pubkey_hex == p.pubkey_hex) {
                    is_seed = true;
                    s.last_seen = now_unix_seconds();
                    break;
                }
                if (peer_name_eq(s.name, p.name) && s.pubkey_hex != p.pubkey_hex)
                    is_seed = true;  // name collision: reject the announcement
            }
            if (is_seed) continue;

            // Existing discovered entry: identity is the key, not the name.
            bool found = false;
            for (auto& d : config_.discovered) {
                if (d.pubkey_hex == p.pubkey_hex) {
                    found = true;
                    if (!p.addr.empty() && d.addr != p.addr) { d.addr = p.addr; changed = true; }
                    if (!p.name.empty() && d.name != p.name) { d.name = p.name; changed = true; }
                    d.last_seen = now_unix_seconds();
                    break;
                }
                if (peer_name_eq(d.name, p.name) && d.pubkey_hex != p.pubkey_hex)
                    found = true;  // name collision: reject the announcement
            }
            if (found) continue;

            PeerEntry pe;
            pe.name = p.name;
            pe.addr = p.addr;
            pe.pubkey_hex = p.pubkey_hex;
            pe.last_seen = now_unix_seconds();
            config_.discovered.push_back(std::move(pe));
            changed = true;  // new peer added
        }
        if (changed) gossip_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    // Prune runtime-learned (discovered) peers that have been silent past
    // mesh.discovered_ttl_secs. Durable `seed` peers are exempt — they stay in
    // the directory offline and reconnect when they return. This is for
    // ephemeral rentals (4090/5090) that appear, do work, then are destroyed.
    void prune_stale_discovered_peers() {
        if (config_.discovered_ttl_secs <= 0) return;  // 0 = keep forever
        const uint64_t now = now_unix_seconds();
        const uint64_t cutoff = static_cast<uint64_t>(config_.discovered_ttl_secs);
        auto has_live_conn = [&](const std::string& pubkey) {
            for (auto& c : conns_) {
                if (c.sock_fd == INVALID_SOCKET) continue;
                if (c.peer_pubkey == pubkey) return true;
            }
            return false;
        };
        auto& d = config_.discovered;
        size_t before = d.size();
        d.erase(std::remove_if(d.begin(), d.end(),
            [&](const PeerEntry& p) {
                // Never prune a peer with an active connection, even if its
                // last_seen is stale (last_seen updates on Hello/gossip only).
                if (has_live_conn(p.pubkey_hex)) return false;
                if (p.last_seen == 0) return false;  // never merged properly
                return (now - p.last_seen) > cutoff;
            }),
            d.end());
        if (d.size() != before) {
            log_event("prune_stale_discovered",
                      "removed=" + std::to_string(before - d.size()));
            gossip_generation_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // R4.2/R4.3: reload SEED list when config file changes on disk.
    // NOTE: discovered peers are runtime state — we intentionally do NOT reload
    // them. Reloading discovered from disk creates a churn loop: reload clobbers
    // them, the next Hello re-merges them, merge_peers saves the config, the mtime
    // change triggers another reload, ad infinitum — starving the event loop.
    void reload_seeds_from_disk() {
        if (config_file_path_.empty()) return;
        MeshConfig fresh = load_config(config_file_path_);
        config_.seeds = std::move(fresh.seeds);
        // r3 fix (P1): the 2026-08-31 D-002 incident proved mesh.auto_upgrade was
        // frozen at daemon start — edits on disk (pin=false) were ignored by the
        // running daemon because hot-reload carried only seeds. The dispatch path
        // then shot at peers mid-upgrade and DOWNGRADED them (GitHub latest=r1).
        // Reload the policy flags alongside the seeds so the pin is live.
        config_.auto_upgrade = fresh.auto_upgrade;
        config_.auto_upgrade_cooldown_secs = fresh.auto_upgrade_cooldown_secs;
        log_event("config_reload", config_file_path_);
    }

    void maybe_reload_config_seeds() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_config_reload_check_ < std::chrono::seconds(30)) return;
        last_config_reload_check_ = now;
        if (config_file_path_.empty()) return;
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(config_file_path_, ec)) return;
        auto mtime = fs::last_write_time(config_file_path_, ec);
        if (ec) return;
        if (!config_mtime_set_) {
            config_mtime_ = mtime;
            config_mtime_set_ = true;
            return;
        }
        if (mtime != config_mtime_) {
            config_mtime_ = mtime;
            reload_seeds_from_disk();
        }
    }

    // Duplicate resolution: when both A and B dial each other simultaneously,
    // two TCP connections form for the same peer pair. We must converge on
    // exactly ONE surviving connection, and BOTH endpoints must independently
    // agree on which physical connection survives.
    //
    // Deterministic rule: keep the connection INITIATED BY the endpoint with
    // the lexicographically smaller pubkey. Concretely, for a duplicate pair
    // with peer_pubkey P:
    //   - if our_pubkey_ < P  → we are the smaller endpoint → keep OUTBOUND (we initiated it)
    //   - if our_pubkey_ > P  → we are the larger endpoint  → keep INBOUND  (they initiated it)
    // Both sides apply the same rule to the same pair, so both keep the single
    // connection that the smaller-pubkey node opened. No mid-handshake teardown,
    // no split — the loser is closed gracefully after the winner is established.
    void resolve_duplicates() {
        // Iterative scan (P2 audit fix): originally recursed after each drop,
        // which could stack-overflow under reconnect storms with many duplicates.
        bool restarted = true;
        while (restarted) {
            restarted = false;
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == INVALID_SOCKET) continue;
            for (size_t j = i + 1; j < conns_.size(); ++j) {
                if (conns_[j].sock_fd == INVALID_SOCKET) continue;
                if (connections_are_mesh_duplicates(conns_[i], conns_[j])) {
                    const std::string& pk = conns_[i].peer_pubkey;
                    bool we_are_smaller = our_pubkey_ < pk;
                    // Desired surviving direction on THIS endpoint.
                    bool want_outbound = we_are_smaller;
                    // Pick the candidate whose direction matches the desired one.
                    bool i_matches = (conns_[i].is_outbound == want_outbound);
                    bool j_matches = (conns_[j].is_outbound == want_outbound);
                    const size_t relative_drop = duplicate_index_to_drop(i_matches, j_matches);
                    const size_t drop = relative_drop == 0 ? i : j;
                    if (!remove_conn(drop)) return;
                    // Restart scan from the top — the conns_ vector changed.
                    restarted = true;
                    break;
                }
            }
            if (restarted) break;
        }
        }
    }

    // ── Accept new inbound connection ──────────────────────────
    // v2.0.6: accept is now non-blocking. The TLS handshake and Hello exchange
    // happen incrementally in advance_handshakes() driven by select() readiness.

    void accept_inbound() {
        sockaddr_in peer_addr{};
        socklen_t addr_len = sizeof(peer_addr);
        SOCKET cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len);
        if (cfd == INVALID_SOCKET) return;

        const std::string source_ip = inet_ntoa(peer_addr.sin_addr);
        size_t pending_from_source = 0;
        for (const auto& ph : pending_handshakes_) {
            if (ph.server_side &&
                ph.expected_addr.rfind(source_ip + ":", 0) == 0) {
                ++pending_from_source;
            }
        }
        if (pending_from_source >= 2) {
            log_event("handshake_source_limit", source_ip);
            ssl_close(nullptr, cfd);
            return;
        }

        if (conns_.size() + pending_handshakes_.size() >= kMaxConnections) {
            ssl_close(nullptr, cfd);
            return;
        }
        if (pending_handshakes_.size() >= kMaxPendingHandshakes) {
            log_event("handshake_pending_limit", "dropped inbound, pending=" +
                      std::to_string(pending_handshakes_.size()));
            ssl_close(nullptr, cfd);
            return;
        }

        // Make socket non-blocking so the handshake state machine never blocks.
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(cfd, FIONBIO, &nb);
#else
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
#endif

        auto ssl = SslPtr(SSL_new(tls_listen_.get()));
        if (!ssl) { ssl_close(nullptr, cfd); return; }
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        SSL_set_accept_state(ssl.get());

        PendingHandshake ph;
        ph.sock_fd = cfd;
        ph.ssl = std::move(ssl);
        ph.server_side = true;
        ph.state = PendingHandshake::State::TlsHandshake;
        ph.expected_addr = std::string(inet_ntoa(peer_addr.sin_addr)) + ":" +
                           std::to_string(ntohs(peer_addr.sin_port));
        ph.want_read = true;
        ph.want_write = false;
        ph.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAcceptHandshakeTimeoutMs);
        set_tcp_nodelay(cfd);  // interactive shell performance (server side)
        pending_handshakes_.push_back(std::move(ph));
        log_event("inbound_accepted", std::string(inet_ntoa(peer_addr.sin_addr)) + ":" +
                  std::to_string(ntohs(peer_addr.sin_port)));
    }

    // ── Incremental TLS + Hello handshake ─────────────────────

    // Called once when TLS completes to verify the peer certificate pubkey.
    bool handshake_verify_cert_pubkey(PendingHandshake& ph) {
        ph.peer_pk = peer_public_key_hex(ph.ssl.get());
        if (ph.peer_pk.empty()) {
            log_event("handshake_no_cert_pubkey", ph.server_side ? "inbound" : "outbound");
            return false;
        }
        return true;
    }

    // Promote a completed handshake to a live Conn.
    void promote_handshake_to_conn(PendingHandshake& ph, const HelloMsg& hello) {
        if (!ph.server_side) record_dead_seed_success(ph.expected_addr); // B2
        Conn c;
        c.peer_name = hello.node_name;
        c.peer_pubkey = ph.peer_pk;
        c.peer_addr = ph.expected_addr;
        c.initial_hello = hello;
        c.remote_version = hello.version;
        c.ssl = std::move(ph.ssl);
        c.sock_fd = ph.sock_fd;
        c.rx_buffer = std::move(ph.rx_buffer);
        c.is_outbound = !ph.server_side;
        c.purpose = ConnectionPurpose::Unknown;
        c.last_pong = std::chrono::steady_clock::now();
        // Steady-state recv timeout for established links.
        set_socket_timeouts(ph.sock_fd, kPeerRecvTimeoutMs);
        set_tcp_nodelay(ph.sock_fd);  // interactive shell performance (mesh connections)
        // v2.0.12c: increase socket buffers for large file transfers
        { int sz = 262144; setsockopt(ph.sock_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
          setsockopt(ph.sock_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz)); }

        merge_peers(hello.known_peers);
        conns_.push_back(std::move(c));
        resolve_duplicates();
        clear_accept_only_for(hello.node_name, ph.expected_addr, ph.peer_pk);

        log_event(ph.server_side ? "mesh_peer_connected" : "mesh_peer_connected_outbound",
                  hello.node_name + " addr=" + (ph.server_side ? "inbound" : ph.expected_addr) +
                  " pubkey=" + ph.peer_pk.substr(0, 16) + "...");

        maybe_schedule_auto_upgrade(hello.node_name, hello.version);

        // Handshake object will be erased; mark fd moved so ssl_close isn't called twice.
        ph.sock_fd = INVALID_SOCKET;
        ph.state = PendingHandshake::State::Done;
    }

    // If peer is behind us, fire-and-forget a remote `bridgesessions upgrade`
    // so hosts that were offline during a fleet cut catch up when they return.
    void maybe_schedule_auto_upgrade(const std::string& peer, const std::string& remote_ver) {
        if (!config_.auto_upgrade) return;
        if (peer.empty() || peer == config_.node_name) return;
        // The peer name comes from a remote Hello. Never interpolate an
        // unvalidated remote string into a shell command.
        if (!bs_peer_name_shell_safe(peer)) {
            log_event("auto_upgrade_rejected", "unsafe peer name");
            return;
        }
        if (!version_is_older(remote_ver, kBridgeSessionsVersion)) return;
        const auto now = std::chrono::steady_clock::now();
        const auto cooldown = std::chrono::seconds(
            std::max(60, config_.auto_upgrade_cooldown_secs));
        auto it = auto_upgrade_last_.find(peer);
        if (it != auto_upgrade_last_.end() && now - it->second < cooldown) return;
        auto_upgrade_last_[peer] = now;
        log_event("auto_upgrade_dispatch",
                  peer + " remote=" + remote_ver +
                  " local=" + std::string(kBridgeSessionsVersion));
        // Use the bounded, joinable long-operation pool instead of spawning an
        // unbounded detached thread for every returning peer.
        LongOperationTask task;
        task.type = LongOperationTask::Type::AutoUpgrade;
        task.peer_name = peer;
        worker_pool_->enqueue(std::move(task));
    }

    void advance_handshakes() {
        auto now = std::chrono::steady_clock::now();
        std::vector<size_t> to_erase;

        for (size_t i = 0; i < pending_handshakes_.size(); ++i) {
            auto& ph = pending_handshakes_[i];
            if (now > ph.deadline) {
                log_event("handshake_deadline", ph.server_side ? "inbound" : ph.expected_addr);
                // Collision-aware dead-seed accounting (fix): a handshake timeout
                // while this endpoint is in its tie-break accept-only window is a
                // probable simultaneous-dial collision, not a dead seed — both sides
                // dialed, the loser's TCP is torn down after duplicate resolution
                // converges. Feeding these into the B2 dead-seed streak parked
                // healthy peers in mutual cooldown (observed 2026-08-21). Only
                // count failures when we were NOT deferring an inbound.
                bool tiebreak_collision = false;
                if (!ph.server_side) {
                    auto it = accept_only_until_.find(ph.expected_addr);
                    tiebreak_collision = it != accept_only_until_.end() &&
                                         now < it->second;
                }
                if (!ph.server_side && !tiebreak_collision) {
                    record_dead_seed_failure(ph.expected_addr); // B2
                }
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
                continue;
            }
            if (!socket_pollable(ph.sock_fd)) {
                log_event("handshake_fd_not_selectable", std::to_string(ph.sock_fd));
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
                continue;
            }

            short events = 0;
            if (ph.want_read) events |= POLLIN;
            if (ph.want_write) events |= POLLOUT;
            bs_pollfd handshake_fd{ph.sock_fd, events, 0};
            const int ready = bs_poll(&handshake_fd, 1, 0);
            if (ready <= 0 && SSL_pending(ph.ssl.get()) <= 0) continue;

            try {
                switch (ph.state) {
                case PendingHandshake::State::TcpConnect: {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    if (getsockopt(ph.sock_fd, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char*>(&so_error), &len) != 0 || so_error != 0) {
                        ph.state = PendingHandshake::State::Failed;
                        to_erase.push_back(i);
                        break;
                    }
                    ph.state = PendingHandshake::State::TlsHandshake;
                    ph.want_read = true;
                    ph.want_write = true;
                    break;
                }
                case PendingHandshake::State::TlsHandshake: {
                    int ret = ph.server_side ? SSL_accept(ph.ssl.get()) : SSL_connect(ph.ssl.get());
                    if (ret > 0) {
                        if (!handshake_verify_cert_pubkey(ph)) {
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                            break;
                        }
                        if (ph.server_side) {
                            ph.state = PendingHandshake::State::ReadHello;
                            ph.want_read = true;
                            ph.want_write = false;
                        } else {
                            ph.outbound_hello = build_hello();
                            ph.state = PendingHandshake::State::WriteHello;
                            ph.want_read = false;
                            ph.want_write = true;
                        }
                    } else {
                        int err = SSL_get_error(ph.ssl.get(), ret);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                            ph.want_read = err == SSL_ERROR_WANT_READ;
                            ph.want_write = err == SSL_ERROR_WANT_WRITE;
                        } else {
                            // B3: inbound ssl_err=1 with no BS Hello is almost
                            // always a non-BS port scanner probing 19949. Log
                            // the first hit per source-IP per hour in full and
                            // suppress the rest to a counter so genuine peer
                            // rejections (ssl_err=5, post-Hello) stay visible.
                            if (ph.server_side && err == 1) {
                                note_nonbs_client(ph.contact_addr);
                            } else {
                                log_event("tls_handshake_failed",
                                          (ph.server_side ? "inbound ssl_err=" : "outbound ssl_err=") +
                                          std::to_string(err));
                            }
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                        }
                    }
                    break;
                }
                case PendingHandshake::State::WriteHello: {
                    int want = SSL_ERROR_WANT_WRITE;
                    if (write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                CONTROL_STREAM_ID, ph.tx_buffer, &want)) {
                        ph.tx_buffer.clear();
                        ph.state = ph.server_side
                            ? PendingHandshake::State::Done
                            : PendingHandshake::State::ReadHello;
                        ph.want_read = !ph.server_side;
                        ph.want_write = false;
                        if (ph.state == PendingHandshake::State::Done) {
                            promote_handshake_to_conn(ph, ph.peer_hello);
                            to_erase.push_back(i);
                        }
                    } else {
                        ph.want_read = want == SSL_ERROR_WANT_READ;
                        ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    }
                    break;
                }
                case PendingHandshake::State::ReadHello: {
                    int want = SSL_ERROR_WANT_READ;
                    auto msg_opt = read_frame_nonblocking(ph.ssl.get(), ph.rx_buffer, &want);
                    ph.want_read = want == SSL_ERROR_WANT_READ;
                    ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    if (!msg_opt) break;
                    if (!std::holds_alternative<HelloMsg>(*msg_opt)) {
                        log_event("handshake_expected_hello",
                                  ph.server_side ? "inbound" : ph.expected_addr);
                        ph.state = PendingHandshake::State::Failed;
                        to_erase.push_back(i);
                        break;
                    }
                    auto& hello = std::get<HelloMsg>(*msg_opt);
                    ph.peer_hello = hello;

                    if (ph.server_side) {
                        auto identity = verify_inbound_peer_identity(
                            config_, ph.peer_pk, hello.pubkey_hex, hello.node_name);
                        if (!identity.ok) {
                            // Join window: allow unknown peers through Hello
                            // verification so they can send a JoinRequest with
                            // a valid invite token. The TLS cert verify
                            // listener callback already accepted them for this join window.
                            // The JoinRequest handler validates the token and
                            // adds them to authorized_keys; without a valid
                            // token the connection is dropped after join.
                            if (!allow_join_connections_.load(std::memory_order_relaxed)) {
                                log_event("hello_identity_rejected",
                                          hello.node_name + " reason=" + identity.reason);
                                ph.state = PendingHandshake::State::Failed;
                                to_erase.push_back(i);
                                break;
                            }
                            log_event("hello_identity_accept_join_window",
                                      hello.node_name + " pubkey=" + hello.pubkey_hex.substr(0, 16) + "...");
                        }
                        // Send Hello reply. For join-window connections, write
                        // the Hello here (so the client gets it immediately) but
                        // transition to ReadJoinRequest (not promote) so the
                        // connection stays in pending_handshakes_ (immune to prune).
                        ph.outbound_hello = build_hello();
                        bool join_window = allow_join_connections_.load(std::memory_order_relaxed) &&
                            !is_trusted_pubkey(ph.peer_pk);
                        int write_want = SSL_ERROR_WANT_WRITE;
                        bool hello_written = write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                    CONTROL_STREAM_ID, ph.tx_buffer, &write_want);
                        if (hello_written) {
                            ph.tx_buffer.clear();
                            if (join_window) {
                                // Hello sent; wait for JoinRequest in next state.
                                ph.state = PendingHandshake::State::ReadJoinRequest;
                                ph.hello_written = true;
                                ph.want_read = true;
                                ph.want_write = false;
                            } else {
                                promote_handshake_to_conn(ph, ph.peer_hello);
                                to_erase.push_back(i);
                            }
                        } else {
                            // Partial write — stay in WriteHello for non-join,
                            // or transition to ReadJoinRequest for join (which
                            // will finish the write on the next iteration).
                            if (join_window) {
                                ph.state = PendingHandshake::State::ReadJoinRequest;
                                ph.want_read = write_want == SSL_ERROR_WANT_READ;
                                ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            } else {
                                ph.state = PendingHandshake::State::WriteHello;
                                ph.want_read = write_want == SSL_ERROR_WANT_READ;
                                ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            }
                        }
                    } else {
                        auto v = verify_outbound_peer_identity(
                            ph.expected_pubkey, ph.peer_pk, hello.pubkey_hex,
                            ph.expected_name, hello.node_name, config_.require_seed_pins);
                        if (!v.ok) {
                            log_event("mesh_peer_identity_rejected",
                                      ph.expected_addr + " name=" + hello.node_name +
                                      " reason=" + v.reason);
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                            break;
                        }
                        promote_handshake_to_conn(ph, hello);
                        to_erase.push_back(i);
                    }
                    break;
                }
                case PendingHandshake::State::ReadJoinRequest: {
                    // Server-side join window: write Hello reply, then read
                    // JoinRequest inline so prune can't kill the connection.
                    // Step 1: write Hello reply if not yet written.
                    if (!ph.outbound_hello.pubkey_hex.empty() && ph.tx_buffer.empty() && !ph.hello_written) {
                        ph.tx_buffer = encode(ph.outbound_hello, CONTROL_STREAM_ID);
                        ph.hello_written = true;
                    }
                    if (!ph.tx_buffer.empty()) {
                        int write_want = SSL_ERROR_WANT_WRITE;
                        if (write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                    CONTROL_STREAM_ID, ph.tx_buffer, &write_want)) {
                            ph.tx_buffer.clear();
                            ph.want_read = true;
                            ph.want_write = false;
                        } else {
                            ph.want_read = write_want == SSL_ERROR_WANT_READ;
                            ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            break;
                        }
                    }
                    // Read JoinRequest
                    int want = SSL_ERROR_WANT_READ;
                    auto msg_opt = read_frame_nonblocking(ph.ssl.get(), ph.rx_buffer, &want);
                    ph.want_read = want == SSL_ERROR_WANT_READ;
                    ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    if (!msg_opt) break;
                    if (std::holds_alternative<JoinRequestMsg>(*msg_opt)) {
                        auto& jr = std::get<JoinRequestMsg>(*msg_opt);
                        log_event("join_request_received", ph.peer_pk.substr(0, 16) + "...");
                        JoinReplyMsg reply = process_join_request(jr, ph.peer_pk);
                        bool reply_sent = false;
                        try {
                            write_frame(ph.ssl.get(), reply, CONTROL_STREAM_ID);
                            reply_sent = true;
                        } catch (const std::exception& e) {
                            log_event("join_reply_write_failed", e.what());
                        }
                        if (reply.ok && reply_sent) {
                            log_event("join_success", ph.peer_pk.substr(0, 16) + "...");
                            // Flush JoinReply to the wire before promoting —
                            // otherwise the event loop floods gossip frames
                            // that the client reads before JoinReply.
                            BIO_flush(SSL_get_wbio(ph.ssl.get()));
                            promote_handshake_to_conn(ph, ph.peer_hello);
                        } else if (reply.ok && !reply_sent) {
                            log_event("join_promote_without_reply", ph.peer_pk.substr(0, 16) + "...");
                        }
                    } else {
                        log_event("join_expected_join_request",
                                  "got message type index=" + std::to_string(msg_opt->index()));
                    }
                    to_erase.push_back(i);
                    break;
                }
                case PendingHandshake::State::Done:
                case PendingHandshake::State::Failed:
                    to_erase.push_back(i);
                    break;
                }
            } catch (const std::exception& e) {
                log_event("handshake_exception",
                          (ph.server_side ? "inbound " : ph.expected_addr + " ") + e.what());
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
            } catch (...) {
                log_event("handshake_exception",
                          ph.server_side ? "inbound unknown" : ph.expected_addr + " unknown");
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
            }
        }

        // Erase failed/done handshakes from back to front to keep indices stable.
        for (auto it = to_erase.rbegin(); it != to_erase.rend(); ++it) {
            auto& ph = pending_handshakes_[*it];
            if (ph.sock_fd != INVALID_SOCKET) {
                if (ph.ssl) SSL_set_quiet_shutdown(ph.ssl.get(), 1);
                CLOSESOCK(ph.sock_fd);
                ph.sock_fd = INVALID_SOCKET;
                ph.ssl.reset();
            }
            pending_handshakes_.erase(pending_handshakes_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
    }

    // ── Start non-blocking outbound handshake to a seed/discovered peer ────
    // Returns true if a handshake was started, false on immediate failure.
    bool start_outbound_handshake(const PeerEntry& peer) {
        for (const auto& ph : pending_handshakes_) {
            if (!ph.server_side &&
                (ph.expected_addr == peer.addr ||
                 (!peer.pubkey_hex.empty() && ph.expected_pubkey == peer.pubkey_hex))) {
                return false;
            }
        }
        if (pending_handshakes_.size() >= kMaxPendingHandshakes) return false;
        try {
            auto sa = resolve_addr(peer.addr);
            SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sfd == INVALID_SOCKET) return false;
            { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }

            // Non-blocking connect.
#ifdef _WIN32
            u_long nb = 1;
            ioctlsocket(sfd, FIONBIO, &nb);
#else
            int fl = fcntl(sfd, F_GETFL, 0);
            if (fl >= 0) fcntl(sfd, F_SETFL, fl | O_NONBLOCK);
#endif
            int rc = connect(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            bool connected_immediately = rc == 0;
            if (rc != 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                    CLOSESOCK(sfd); return false;
                }
#else
                if (errno != EINPROGRESS) {
                    CLOSESOCK(sfd); return false;
                }
#endif
            }

            auto ssl = SslPtr(SSL_new(tls_connect_.get()));
            if (!ssl) { CLOSESOCK(sfd); return false; }
            if (!set_expected_peer_pubkey(ssl.get(), peer.pubkey_hex)) {
                CLOSESOCK(sfd); return false;
            }
            SSL_set_fd(ssl.get(), static_cast<int>(sfd));
            SSL_set_connect_state(ssl.get());

            PendingHandshake ph;
            ph.sock_fd = sfd;
            ph.ssl = std::move(ssl);
            ph.server_side = false;
            ph.state = connected_immediately
                ? PendingHandshake::State::TlsHandshake
                : PendingHandshake::State::TcpConnect;
            ph.want_read = connected_immediately;
            ph.want_write = true;
            ph.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(outbound_connect_timeout_ms_);
            ph.expected_addr = peer.addr;
            ph.expected_pubkey = peer.pubkey_hex;
            ph.expected_name = peer.name;
            pending_handshakes_.push_back(std::move(ph));
            return true;
        } catch (...) {
            return false;
        }
    }

    // Helper used by advance_handshakes to check if a non-blocking connect finished.
    static bool socket_connect_finished(SOCKET fd) {
        if (fd == INVALID_SOCKET) return false;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) != 0)
            return false;
        return so_error == 0;
    }

    // ── Public API ──────────────────────────────────────────

    bool connect_to_peer_impl(const std::string& addr) {
        try {
            // Check if already connected to this addr
            if (has_conn_for_addr(addr)) return true;

            // Resolve and connect
            auto sa = resolve_addr(addr);
            SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sfd == INVALID_SOCKET) return false;
            set_socket_timeouts(sfd, outbound_connect_timeout_ms_);
            set_tcp_nodelay(sfd);  // interactive shell performance
            { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }  // R3.6

            const auto connect_result = connect_socket_with_timeout(
                sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
                outbound_connect_timeout_ms_);
            if (!connect_result.connected) {
                ssl_close(nullptr, sfd);
                return false;
            }

            const PeerEntry* pe = find_peer_entry_by_addr(config_, addr);
            const std::string expected_pk = pe ? pe->pubkey_hex : std::string{};
            const std::string expected_name = pe ? pe->name : std::string{};

            // TLS handshake (client side)
            auto ssl = SslPtr(SSL_new(tls_connect_.get()));
            if (!ssl) { ssl_close(nullptr, sfd); return false; }
            if (!set_expected_peer_pubkey(ssl.get(), expected_pk)) {
                ssl_close(nullptr, sfd); return false;
            }
            SSL_set_fd(ssl.get(), static_cast<int>(sfd));

            int ret = ssl_connect_blocking(ssl.get(), sfd, outbound_connect_timeout_ms_);
            if (ret <= 0) {
                // R1: capture error before ssl_close drains the queue
                int ssl_err = SSL_get_error(ssl.get(), ret);
                char errbuf[256] = {};
                unsigned long e = ERR_get_error();
                if (e) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                std::string detail = "ssl_err=" + std::to_string(ssl_err) +
                                     (errbuf[0] ? std::string(" ") + errbuf : "");
                append_ssl_connect_error_detail(detail, ssl_err);
                log_event("tls_connect_failed", detail);
                ssl_close(ssl.get(), sfd);
                return false;
            }

            // Get peer's Ed25519 public key from the TLS certificate.
            std::string peer_pk = peer_public_key_hex(ssl.get());
            if (peer_pk.empty()) { ssl_close(ssl.get(), sfd); return false; }

            // Seeds always require pins when require_seed_pins; discovered same.
            const bool require_pin = config_.require_seed_pins;

            // Send our Hello
            write_frame(ssl.get(), build_hello(), CONTROL_STREAM_ID);

            // Read Hello from peer
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                ssl_close(ssl.get(), sfd);
                return false;
            }
            auto& hello = std::get<HelloMsg>(msg);

            // P0-1: pin ↔ cert ↔ Hello before merge_peers or trusting the link.
            auto v = verify_outbound_peer_identity(
                expected_pk, peer_pk, hello.pubkey_hex,
                expected_name, hello.node_name, require_pin);
            if (!v.ok) {
                log_event("mesh_peer_identity_rejected",
                          addr + " name=" + hello.node_name + " reason=" + v.reason);
                ssl_close(ssl.get(), sfd);
                return false;
            }

            Conn c;
            c.peer_name = hello.node_name;
            c.peer_pubkey = peer_pk;
            c.peer_addr = addr;
            c.initial_hello = hello;
            c.remote_version = hello.version;
            std::string subj_out = peer_cert_subject_oneline(ssl.get());  // R1.4 before move
            c.ssl = std::move(ssl);
            c.sock_fd = sfd;
            c.is_outbound = true;
            c.last_pong = std::chrono::steady_clock::now();
            // Steady-state recv timeout (see kPeerRecvTimeoutMs): bound mid-frame
            // stalls to drop+reconnect instead of a single-threaded loop freeze.
            set_socket_timeouts(sfd, kPeerRecvTimeoutMs);
            // v2.0.12c: increase socket buffers for large file transfers
            { int sz = 262144; setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
              setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz)); }

            // Merge known peers from Hello only after identity is verified.
            merge_peers(hello.known_peers);

            conns_.push_back(std::move(c));
            resolve_duplicates();

            // Reset backoff on success
            backoffs_.erase(addr);
            record_dead_seed_success(addr); // B2
            clear_accept_only_for(hello.node_name, addr, peer_pk);

            log_event("mesh_peer_connected_outbound", hello.node_name + " addr=" + addr
                      + " pubkey=" + peer_pk.substr(0, 16) + "..."
                      + " subject=" + subj_out);  // R1.4

#ifndef BS_NO_WEBRTC
            // D15: After TCP connection, try WebRTC upgrade
            if (config_.webrtc_enabled) {
                try_webrtc_upgrade(c);
            }
#endif

            return true;
        } catch (...) {
            return false;
        }
    }

    // ── D15: WebRTC upgrade attempt ──────────────────────────

#ifndef BS_NO_WEBRTC
    void try_webrtc_upgrade(Conn& c) {
        // Send SDP offer over existing TCP gossip channel
        try {
            // NOTE: In a full implementation, we'd create a real SDP offer here
            // using libdatachannel's PeerConnection API.
            // For now, we send a placeholder to signal WebRTC capability.
            SdpOfferMsg offer;
            offer.peer_name = config_.node_name;
            offer.sdp = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=bridgesessions\r\nt=0 0\r\n";
            (void)enqueue_frame(c, offer, CONTROL_STREAM_ID);
            log_event("webrtc_offer_sent", c.peer_name);
        } catch (...) {
            log_event("webrtc_offer_send_failed", c.peer_name);
        }
    }
#endif

    // ── Build Gossip message ───────────────────────────────────
    // min_generation > 0: only meaningful when peers changed (delta). When
    // gossip_generation_ <= min_generation the caller already has current
    // state, so we return an empty peers list (nothing to send).
    // min_generation == 0: full snapshot (backward compat / first contact).
    GossipMsg build_gossip(uint32_t min_generation = 0) const {
        GossipMsg g;
        if (min_generation > 0 && gossip_generation_.load() <= min_generation)
            return g;  // caller is current — nothing changed

        for (auto& s : config_.seeds) {
            if (s.pubkey_hex.empty()) continue;
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = s.last_seen;
            g.peers.push_back(std::move(pi));
        }
        for (auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            PeerInfo pi;
            pi.name = d.name;
            pi.addr = d.addr;
            pi.pubkey_hex = d.pubkey_hex;
            pi.last_seen = d.last_seen;
            g.peers.push_back(std::move(pi));
        }
        return g;
    }

    // ── D15: WebRTC SDP handlers ──────────────────────────────

    void handle_sdp_offer(Conn& c, const SdpOfferMsg& offer) {
#ifndef BS_NO_WEBRTC
        if (!config_.webrtc_enabled) return;
        try {
            std::string answer_sdp;
            auto pc = WebRtcChannel::create_offerer(offer.sdp, answer_sdp);

            SdpAnswerMsg answer;
            answer.peer_name = config_.node_name;
            answer.sdp = answer_sdp;
            (void)enqueue_frame(c, answer, CONTROL_STREAM_ID);

            log_event("webrtc_offer_accepted", "from " + offer.peer_name);
        } catch (...) {
            log_event("webrtc_offer_failed", "from " + offer.peer_name);
        }
#endif
    }

    void handle_sdp_answer(Conn& c, const SdpAnswerMsg& answer) {
#ifndef BS_NO_WEBRTC
        if (!config_.webrtc_enabled) return;
        try {
            log_event("webrtc_answer_received", "from " + answer.peer_name);
        } catch (...) {}
#endif
    }

    // ── D16: DHT message handlers ─────────────────────────────

    void handle_dht_find_node(Conn& c, const DhtFindNodeMsg& query) {
#ifndef BS_NO_DHT
        if (!config_.dht_enabled || !dht_inited_) return;
        // Reply with GossipMsg containing closest peers
        auto closest = dht_.find_closest(query.target_id, 20);
        GossipMsg g;
        for (auto& dp : closest) {
            PeerInfo pi;
            pi.name = dp.name;
            pi.addr = dp.addr;
            pi.pubkey_hex = ""; // DHT peers may not have pubkeys
            pi.last_seen = dp.last_seen;
            g.peers.push_back(std::move(pi));
        }
        if (!g.peers.empty()) {
            (void)enqueue_frame(c, g, CONTROL_STREAM_ID);
        }
#endif
    }

