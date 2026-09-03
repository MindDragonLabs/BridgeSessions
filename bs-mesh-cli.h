// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-cli.h — MeshController ctor, event loop, and CLI entry points
// Extracted from bs-mesh-controller.h (R6b structural refactor, 2026-09-03)
// Designed for inclusion inside `class MeshController { ... }`
// Does NOT open its own namespace or class — parent file provides it.
#pragma once

public:
    // ── Constructor ───────────────────────────────────────────

    MeshController(const MeshConfig& cfg, std::string app_home = {},
                   std::string config_path = {})
        : config_(cfg)
    {
        configure_sigpipe_handling();
        // app_home is the BridgeSessions root directory (default ~/.bridgesessions).
        // --config-dir sets this so identity/keys/receive never touch $HOME/.bridgesessions.
        if (app_home.empty()) {
            app_home = expand_home("~/.bridgesessions");
        }
        while (app_home.size() > 1 && (app_home.back() == '/' || app_home.back() == '\\')) {
            app_home.pop_back();
        }
        home_dir_ = app_home;
        ipc_token_path_ = ipc_token_path(app_home);
        configure_logger_home(app_home);
        AppPaths paths = make_app_paths(app_home);
        apply_app_home_defaults(config_, app_home);

        receive_dir_ = paths.received;

        // Config override: if config specifies a receive_dir, use that instead.
        // Needed when daemon runs as SYSTEM but needs files in user's home.
        if (!config_.receive_dir_override.empty()) {
            receive_dir_ = expand_home(config_.receive_dir_override);
        }
        bootstrap_identity(paths.root);

        std::string pub_path = paths.pub;
        std::ifstream pf(pub_path);
        if (pf.is_open()) {
            std::getline(pf, our_pubkey_);
            pf.close();
        }

        std::string key_path = paths.key_pem;
        std::string cert_path = paths.cert_pem;

        NodeTlsConfig listen_cfg;
        listen_cfg.cert_file = cert_path;
        listen_cfg.key_file = key_path;
        listen_cfg.authorized_keys_file = resolve_under_app_home(config_.authorized_keys_path, app_home);
        tls_listen_ = create_node_tls(listen_cfg, TlsMode::Listen, &authorized_keys_, nullptr,
                                      &allow_join_connections_);

        NodeTlsConfig connect_cfg;
        connect_cfg.cert_file = cert_path;
        connect_cfg.key_file = key_path;
        // Outbound peer authentication is enforced at the application layer in
        // connect_and_hello() / verify_outbound_peer_identity(), which compare
        // the certificate + Hello public key against the pinned `expected_pubkey`
        // immediately after the TLS handshake and reject on any mismatch. The
        // TLS-level verify callback therefore accepts the peer cert (TOFU at the
        // transport layer) so that pin enforcement can happen one layer up with
        // the full per-peer pin context, which a daemon-wide shared SSL_CTX
        // cannot carry. Do NOT add a permissive callback here that shadows the
        // app-layer check — see connect_and_hello() for the authoritative gate.
        connect_cfg.tofu_cb = [](const std::string& /*fingerprint*/) {
            return true;  // transport TOFU; pin enforced post-handshake (see above)
        };
        tls_connect_ = create_node_tls(connect_cfg, TlsMode::Connect, nullptr, &tofu_cb_);

        sessions_.set_persistence_path(resolve_under_app_home(config_.persistence_path, app_home));
        config_file_path_ = !config_path.empty()
            ? expand_home(config_path)
            : (!config_.source_path.empty() ? config_.source_path : paths.config);

        // D16: Initialize DHT if enabled
#ifndef BS_NO_DHT
        if (config_.dht_enabled) {
            std::string our_addr = config_.listen_addr + ":" + std::to_string(config_.listen_port);
            dht_.init(our_pubkey_, config_.node_name, our_addr);
            dht_inited_ = true;
        }
#endif

        // D17: Initialize UPnP if enabled
#ifndef BS_NO_NAT
        if (config_.upnp_enabled) {
            if (upnp_.init()) {
                upnp_.setup_port_mapping(config_.listen_port);
                external_addr_ = upnp_.external_ip();
                if (!external_addr_.empty()) {
                    log_event("upnp_ready", "external ip: " + external_addr_);
                }
            }
        }
#endif

        // v2.0.6: start long-operation worker pool. Handlers run on worker threads
        // and borrow mesh transports while exec_busy is set.
        worker_pool_.emplace(kLongOperationWorkers,
            [this](const LongOperationTask& task) { execute_long_operation_task(task); });
    }

#ifdef BS_TESTING
    [[nodiscard]] const std::string& config_file_path_for_test() const {
        return config_file_path_;
    }
    [[nodiscard]] std::string hello_version_for_test() const {
        return build_hello().version;
    }
    [[nodiscard]] std::string sessions_summary_json_for_test() const {
        return build_sessions_summary_json();
    }
    [[nodiscard]] std::string mesh_tree_json_for_test() {
        return build_mesh_tree_json();
    }
    [[nodiscard]] bool direct_connect_rejects_missing_pin_for_test() {
        auto result = connect_and_hello("127.0.0.1:1", {});
        return result.fail == ConnectFailReason::TlsRejected &&
               result.fail_detail == "peer key not pinned";
    }
    // Actual listen port when config asked for 0 (ephemeral). 0 if not listening.
    [[nodiscard]] uint16_t actual_listen_port_for_test() const {
        return actual_listen_port_.load();
    }
    [[nodiscard]] size_t pending_handshake_count_for_test() const {
        return pending_handshakes_.size();
    }
    static constexpr size_t kMaxPendingHandshakes_for_test() {
        return kMaxPendingHandshakes;
    }
    bool start_outbound_handshake_for_test(const PeerEntry& peer) {
        return start_outbound_handshake(peer);
    }
    [[nodiscard]] long next_backoff_ms_for_test(int attempt) const {
        return next_backoff_ms(attempt);
    }
    void advance_handshakes_for_test() { advance_handshakes(); }
    // B2: dead-seed cooldown test hooks.
    void try_connect_to_seeds_for_test() { try_connect_to_seeds(); }
    void record_dead_seed_failure_for_test(const std::string& addr) {
        record_dead_seed_failure(addr);
    }
    void record_dead_seed_success_for_test(const std::string& addr) {
        record_dead_seed_success(addr);
    }
    [[nodiscard]] bool dead_seed_in_cooldown_for_test(const std::string& addr) {
        return dead_seed_cooldown_active(addr, std::chrono::steady_clock::now());
    }
    [[nodiscard]] int dead_seed_failure_streak_for_test(const std::string& addr) const {
        auto it = seed_cooldowns_.find(addr);
        return it == seed_cooldowns_.end() ? 0 : it->second.consecutive_deadline_failures;
    }
    // Force an active cooldown to expire immediately (simulates the 10min
    // window elapsing) without sleeping in the test.
    void expire_dead_seed_cooldown_for_test(const std::string& addr) {
        auto it = seed_cooldowns_.find(addr);
        if (it != seed_cooldowns_.end()) it->second.cooldown_until = std::chrono::steady_clock::now();
    }
    [[nodiscard]] std::string seed_dial_health_for_test(const std::string& addr) {
        return seed_dial_health(addr, std::chrono::steady_clock::now());
    }
    // True if a pending handshake exists whose outbound target is addr —
    // deterministic proof that try_connect_to_seeds() attempted (or didn't
    // attempt) a dial, independent of real-world connect() timing.
    [[nodiscard]] bool has_pending_handshake_for_addr_for_test(const std::string& addr) const {
        for (const auto& ph : pending_handshakes_) {
            if (!ph.server_side && ph.expected_addr == addr) return true;
        }
        return false;
    }
    // Collision-aware dead-seed accounting (2026-08-21 fix) test hooks.
    void set_our_pubkey_for_test(const std::string& pk) { our_pubkey_ = pk; }
    bool should_defer_outbound_for_test(const PeerEntry& peer) {
        return should_defer_outbound_for(peer, std::chrono::steady_clock::now());
    }
    void clear_accept_only_for_test(const std::string& peer_name,
                                    const std::string& addr,
                                    const std::string& pubkey_hex) {
        clear_accept_only_for(peer_name, addr, pubkey_hex);
    }
    // Force the tie-break accept-only window for addr into the past so
    // should_defer_outbound_for takes the extension path deterministically.
    void expire_tiebreak_window_for_test(const std::string& addr) {
        auto it = accept_only_until_.find(addr);
        if (it != accept_only_until_.end()) it->second =
            std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }
    [[nodiscard]] bool tiebreak_extension_active_for_test(const std::string& addr) const {
        return tiebreak_probe_extends_.count(addr) > 0;
    }
    [[nodiscard]] int tiebreak_extension_count_for_test(const std::string& addr) const {
        auto it = tiebreak_probe_extends_.find(addr);
        return it == tiebreak_probe_extends_.end() ? 0 : it->second;
    }
    // Simulate the collision-aware accounting branch of advance_handshakes():
    // returns whether a handshake_deadline on this outbound addr WOULD count
    // as a dead-seed failure (true) or be excused as a tie-break collision (false).
    [[nodiscard]] bool handshake_deadline_counts_as_dead_seed_for_test(
            const std::string& addr) const {
        auto it = accept_only_until_.find(addr);
        bool tiebreak_collision = it != accept_only_until_.end() &&
                                  std::chrono::steady_clock::now() < it->second;
        return !tiebreak_collision;
    }
    [[nodiscard]] size_t worker_queue_depth_for_test() const {
        return worker_pool_ ? worker_pool_->pending_count() : 0;
    }
    size_t add_connection_for_test(Conn&& conn) {
        conns_.push_back(std::move(conn));
        return conns_.size() - 1;
    }
    void prune_revoked_connections_for_test() { prune_revoked_connections(); }
    bool connection_open_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].sock_fd != INVALID_SOCKET;
    }
    bool connection_closed_for_test(size_t index) const {
        return !connection_open_for_test(index);
    }
    void expire_exec_watchdog_for_test(size_t index) {
        auto& c = conns_.at(index);
        c.exec_busy->store(true);
        c.exec_cancelled->store(false);
        c.exec_started_at = std::chrono::steady_clock::now() -
                            std::chrono::seconds(7501);
        check_stale_exec();
    }
    // Run the watchdog against the current exec_started_at (no back-dating),
    // for testing the sub-90s "do not fire" path.
    void check_stale_exec_for_test() { check_stale_exec(); }
    bool exec_busy_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].exec_busy->load();
    }
    bool exec_cancelled_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].exec_cancelled->load();
    }
    [[nodiscard]] bool conn_busy_for_test(const std::string& peer_name) const {
        for (const auto& c : conns_) {
            if (peer_name_eq(c.peer_name, peer_name) && c.exec_busy)
                return c.exec_busy->load();
        }
        return false;
    }
    [[nodiscard]] bool conn_close_requested_for_test(const std::string& peer_name) const {
        for (const auto& c : conns_) {
            if (peer_name_eq(c.peer_name, peer_name) && c.close_requested)
                return c.close_requested;
        }
        return false;
    }
    [[nodiscard]] std::vector<std::string> conn_peer_names_for_test() const {
        std::vector<std::string> names;
        for (const auto& c : conns_) names.push_back(c.peer_name);
        return names;
    }
#endif

    // ── Destructor ────────────────────────────────────────────

    ~MeshController() {
        running_ = false;
#ifdef _WIN32
        shutdown_windows_pty_writer();
#endif
        // v2.0.6: join long-operation workers before tearing down SSL transports
        // so no worker touches a Conn after the destructor begins. Cancel active
        // workers and shut down their sockets so blocked selects/reads return.
        if (worker_pool_) {
            for (auto& c : conns_) {
                if (c.exec_cancelled) c.exec_cancelled->store(true);
                if (c.sock_fd != INVALID_SOCKET) {
#ifdef _WIN32
                    ::shutdown(c.sock_fd, SD_BOTH);
#else
                    ::shutdown(c.sock_fd, SHUT_RDWR);
#endif
                }
            }
            worker_pool_->shutdown();
        }
#ifndef BS_NO_NAT
        if (config_.upnp_enabled) {
            upnp_.cleanup();
        }
#endif
        for (auto& ph : pending_handshakes_) {
            if (ph.ssl) SSL_set_quiet_shutdown(ph.ssl.get(), 1);
            if (ph.sock_fd != INVALID_SOCKET) {
                CLOSESOCK(ph.sock_fd);
                ph.sock_fd = INVALID_SOCKET;
            }
            ph.ssl.reset();
        }
        pending_handshakes_.clear();
        for (auto& c : conns_) {
            (void)close_conn(c);
        }
        conns_.clear();
        if (listen_fd_ != INVALID_SOCKET) {
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
        cli_ipc_shutdown();
    }

    bool cli_ipc_init() {
        cli_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (cli_listen_fd_ == INVALID_SOCKET) return false;
        int opt = 1;
        setsockopt(cli_listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        if (bind(cli_listen_fd_, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
        if (listen(cli_listen_fd_, 8) == SOCKET_ERROR) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#ifndef _WIN32
        if (cli_listen_fd_ >= FD_SETSIZE) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#endif
        // Generate a fresh CSPRNG IPC token only after successfully binding the
        // socket. Write it owner-only under the app home.
        try {
            ipc_token_ = generate_ipc_token();
        } catch (...) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
        if (!write_private_text_file(ipc_token_path_, ipc_token_)) {
            ipc_token_.clear();
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(cli_listen_fd_, FIONBIO, &nb);
#else
        int fl = fcntl(cli_listen_fd_, F_GETFL, 0);
        fcntl(cli_listen_fd_, F_SETFL, fl | O_NONBLOCK);
#endif
        log_event("mesh_cli_ipc_listen", std::to_string(mesh_cli_port()));
        return true;
    }

    void cli_ipc_shutdown() {
        if (cli_listen_fd_ != INVALID_SOCKET) {
            CLOSESOCK(cli_listen_fd_);
            cli_listen_fd_ = INVALID_SOCKET;
        }
        // Best-effort remove the daemon's IPC token so a stale token cannot be
        // replayed after the daemon exits. A new daemon always generates a fresh
        // token and overwrites any existing file on bind.
        // 2.0.8: only remove when the file still contains OUR token — CLI mesh
        // clients (e.g. `bs shell`) share this path with the long-running daemon;
        // without the check, a client exit would strip the daemon's live token.
        if (!ipc_token_path_.empty() && !ipc_token_.empty()) {
            std::error_code ec;
            std::string on_disk;
            {
                std::ifstream f(ipc_token_path_);
                if (f) on_disk.assign(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>());
            }
            // Exact-match (mod trailing whitespace): a substring check would
            // delete a file that merely CONTAINS our token among other data.
            while (!on_disk.empty() &&
                   (on_disk.back() == '\n' || on_disk.back() == '\r' ||
                    on_disk.back() == ' ' || on_disk.back() == '\t'))
                on_disk.pop_back();
            if (!on_disk.empty() && on_disk == ipc_token_) {
                std::filesystem::remove(ipc_token_path_, ec);
            }
        }
        ipc_token_.clear();
    }

    void cli_ipc_accept_one() {
        if (cli_listen_fd_ == INVALID_SOCKET) return;
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        SOCKET cfd = accept(cli_listen_fd_, (sockaddr*)&peer, &plen);
        if (cfd == INVALID_SOCKET) return;
        // Normalize accepted IPC sockets to blocking mode. macOS can inherit
        // O_NONBLOCK from the listener; without this, a fragmented request may
        // return EAGAIN between the token and command and be parsed as empty.
#ifdef _WIN32
        { u_long blocking = 0; ioctlsocket(cfd, FIONBIO, &blocking); }
#else
        {
            int flags = fcntl(cfd, F_GETFL, 0);
            if (flags >= 0) fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);
        }
#endif
        // CRITICAL: bound the recv. The accepted socket does not reliably inherit
        // the listen socket's non-blocking flag (esp. on macOS), so a client that
        // connects but sends no data would block recv() here and stall the ENTIRE
        // daemon event loop — no peer reads, missed pongs, false pong_timeouts, and
        // the whole mesh collapses. Use a short per-read timeout plus an
        // absolute two-second framing deadline so slow trickle clients cannot
        // hold the loop beyond the deadline regardless of request size.
        set_socket_timeouts(cfd, 250);
        // 128 KiB request buffer: CONV_APPEND carries b64 bodies up to the
        // 65535-byte wire limit (≈87.4 KiB encoded) plus token + verb
        // overhead. The absolute 2s deadline is the slow-trickle DoS control,
        // NOT the buffer size — enlarging the buffer does not widen that
        // window, it only raises the max single-request length.
        std::vector<char> req_buf(128 * 1024);
        char* buf = req_buf.data();
        int n = 0;
        bool newline_seen = false;
        const auto request_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
        while (n < static_cast<int>(req_buf.size() - 1) &&
               std::chrono::steady_clock::now() < request_deadline) {
            int got = recv(cfd, buf + n,
                           static_cast<int>(req_buf.size() - 1) - n, 0);
            if (got <= 0) break;
            n += got;
            if (std::memchr(buf, '\n', static_cast<size_t>(n)) != nullptr) {
                newline_seen = true;
                break;
            }
        }
        std::string response = "ERROR bad request\n";
        bool response_sent = false;
        if (n > 0) {
            buf[n] = '\0';
            std::string line(buf);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            const bool authorized = const_time_token_match(line, ipc_token_);
            if (!authorized) {
                log_event("ipc_auth_rejected", "unauthorized local IPC request");
                response = "ERROR unauthorized\n";
                send(cfd, response.data(), static_cast<int>(response.size()), 0);
                CLOSESOCK(cfd);
                return;
            }
            line.erase(0, ipc_token_.size());
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.erase(line.begin());
            if (line == "DAEMON_PROBE") {
                response = "OK bridgesessions\n";
            }
            else if (line.rfind("HEALTH ", 0) == 0) {
                std::string peer_name = line.substr(7);
                std::string want_addr = find_peer_addr(peer_name);
                bool found = false, ok = false;
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    bool name_match = peer_name_eq(c.peer_name, peer_name);
                    bool addr_match = !want_addr.empty() && c.peer_addr == want_addr;
                    if (!name_match && !addr_match) continue;
                    found = true;
                    // The daemon's own event loop pings every ping_interval_secs and
                    // updates last_pong. A live conn whose last_pong is within the
                    // pong-timeout window is healthy. Do NOT issue a synchronous ping
                    // here: the main loop owns reads on this socket and would consume
                    // the pong, producing false "no pong" results.
                    ok = (now - c.last_pong) <= fresh;
                    break;
                }
                response = found ? (peer_name + (ok ? " healthy\n" : " no pong\n"))
                                   : (peer_name + " not connected\n");
            }
            else if (line.rfind("RECONNECT ", 0) == 0) {
                std::string peer_name = line.substr(10);
                response = daemon_reconnect_peer(peer_name) + "\n";
            }
            else if (line == "STATS") {
                response = daemon_stats_summary() + "\n";
            }
            else if (line == "INVITE") {
                // Generate an invite token. Redeemable only while the TLS join
                // window is open (mesh.join_window_max_secs, default 300s).
                // A 2-hour record TTL is the backstop if the event loop never
                // ticks the hard cap.
                std::lock_guard lock(invite_mutex_);
                unsigned char raw_bytes[16];
                if (RAND_bytes(raw_bytes, sizeof(raw_bytes)) != 1) {
                    response = "ERROR could not generate invite token\n";
                } else {
                    std::ostringstream tok;
                    for (size_t i = 0; i < sizeof(raw_bytes); ++i)
                        tok << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(raw_bytes[i]);
                    auto now = std::chrono::steady_clock::now();
                    expire_pending_invites_locked(now);
                    PendingInvite pi;
                    pi.token = tok.str();
                    pi.created_at = now;
                    response = pi.token + "\n";
                    pending_invites_.push_back(std::move(pi));
                    // Open the join window so unknown peers can TLS-connect to
                    // present their invite token. Closed after successful join
                    // or naturally expires when invites time out.
                    open_join_window_locked(now);
                }
            }
            else if (line.rfind("ENROLL ", 0) == 0) {
                // "ENROLL <name> <pubkey_hex> <addr>" — issue a signed directory
                // enrollment for a new member, then gossip it to every peer.
                std::istringstream es(line.substr(7));
                std::string name, pubkey_hex, addr;
                es >> name >> pubkey_hex >> addr;
                if (name.empty() || pubkey_hex.size() != 64 || addr.empty()) {
                    response = "ERROR usage: ENROLL <name> <pubkey64hex> <addr>\n";
                } else {
                    DirectoryEnrollMsg e = make_directory_enroll(name, pubkey_hex, addr);
                    if (e.signature.empty()) {
                        response = "ERROR could not sign enrollment\n";
                    } else {
                        broadcast_enroll(e);
                        response = "OK enrolled " + name + " " + addr + "\n";
                    }
                }
            }
            else if (line == "SESSIONS") {
                response = sessions_.summary() + "\n";
            }
            else if (line == "PEERS") {
                // One line per live mesh conn: name addr state=... last_pong_s=N
                std::ostringstream out;
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    bool ok = (now - c.last_pong) <= fresh;
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - c.last_pong).count();
                    out << c.peer_name << " " << c.peer_addr
                        << " state=" << (ok ? "healthy" : "no-pong")
                        << " last_pong_s=" << age << "\n";
                }
                // B2: configured seeds/discovered peers with no live conn — surface
                // dial health so operators can see backoff/cooldown without logs.
                for (const auto& s : config_.seeds) {
                    if (s.name.empty() || has_conn_for_addr(s.addr)) continue;
                    out << s.name << " " << s.addr
                        << " state=offline dial_health=" << seed_dial_health(s.addr, now) << "\n";
                }
                for (const auto& d : config_.discovered) {
                    if (d.name.empty() || has_conn_for_addr(d.addr)) continue;
                    out << d.name << " " << d.addr
                        << " state=offline dial_health=" << seed_dial_health(d.addr, now) << "\n";
                }
                out << "END\n";
                response = out.str();
            }
            else if (line == "FLEET") {
                // JSON fleet directory: self + live mesh peers + configured seeds
                // that are not connected (status=offline). Host metrics only when
                // known — never invent load1=0.0 from legacy ServerInfo defaults.
                auto emit_host_fields = [](std::ostringstream& out, const HostStats& hs) {
                    auto num_or_null = [](double v) -> std::string {
                        if (v < 0) return "null";
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.1f", v);
                        return buf;
                    };
                    out << ",\"cpu_pct\":" << num_or_null(hs.cpu_pct)
                        << ",\"mem_pct\":" << num_or_null(hs.mem_pct)
                        << ",\"disk_pct\":" << num_or_null(hs.disk_pct)
                        << ",\"load1\":" << num_or_null(hs.load1)
                        << ",\"os\":\"" << gossip_json_escape(hs.os) << "\""
                        << ",\"arch\":\"" << gossip_json_escape(hs.arch) << "\""
                        << ",\"ncpu\":" << hs.ncpu
                        << ",\"mem_used_mb\":" << hs.mem_used_mb
                        << ",\"mem_total_mb\":" << hs.mem_total_mb
                        << ",\"disk_used_gb\":" << hs.disk_used_gb
                        << ",\"disk_total_gb\":" << hs.disk_total_gb
                        << ",\"cua\":" << (hs.cua_helper ? "true" : "false")
                        << ",\"metrics\":true";
                };
                // Only emit metrics when peer advertised host_stats_json.
                // Do NOT fall back to ServerInfo.load — legacy peers leave it 0.
                auto emit_host_from_json = [&](std::ostringstream& out,
                                               const std::string& js) {
                    if (js.empty()) {
                        out << ",\"metrics\":false";
                        return;
                    }
                    try {
                        auto j = nlohmann::json::parse(js);
                        auto getd = [&](const char* k) -> std::string {
                            if (!j.contains(k) || j[k].is_null()) return "null";
                            if (j[k].is_number()) {
                                char buf[32];
                                std::snprintf(buf, sizeof(buf), "%.1f", j[k].get<double>());
                                return buf;
                            }
                            return "null";
                        };
                        out << ",\"cpu_pct\":" << getd("cpu")
                            << ",\"mem_pct\":" << getd("mem")
                            << ",\"disk_pct\":" << getd("disk")
                            << ",\"load1\":" << getd("load")
                            << ",\"os\":\"" << gossip_json_escape(j.value("os", "")) << "\""
                            << ",\"arch\":\"" << gossip_json_escape(j.value("arch", "")) << "\""
                            << ",\"ncpu\":" << j.value("ncpu", 0)
                            << ",\"mem_total_mb\":" << j.value("mem_mb", 0)
                            << ",\"disk_total_gb\":" << j.value("disk_gb", 0)
                            << ",\"cua\":" << (j.value("cua", false) ? "true" : "false")
                            << ",\"metrics\":true";
                    } catch (...) {
                        out << ",\"metrics\":false";
                    }
                };
                auto seed_addr_for = [&](const std::string& name) -> std::string {
                    for (const auto& s : config_.seeds)
                        if (peer_name_eq(s.name, name) && !s.addr.empty()) return s.addr;
                    return {};
                };

                std::ostringstream out;
                out << "{";
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                HostStats self_hs = collect_host_stats(home_dir_);
                if (self_hs.cpu_pct < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    self_hs = collect_host_stats(home_dir_);
                }
                std::unordered_set<std::string> listed; // lowercased names
                auto mark = [&](const std::string& n) {
                    std::string k = n;
                    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    listed.insert(k);
                };
                auto listed_has = [&](const std::string& n) {
                    std::string k = n;
                    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return listed.count(k) > 0;
                };

                // Self: prefer real advertise IP over 0.0.0.0 listen bind.
                std::string self_host = self_hs.primary_addr;
                if (self_host.empty() || self_host == "0.0.0.0" || self_host == "::")
                    self_host = config_.listen_addr;
                if (self_host.empty() || self_host == "0.0.0.0" || self_host == "::")
                    self_host = "127.0.0.1";
                std::string self_addr = self_host + ":" + std::to_string(config_.listen_port);
                out << "\"" << gossip_json_escape(config_.node_name) << "\":{\"name\":\""
                    << gossip_json_escape(config_.node_name)
                    << "\",\"addr\":\""
                    << gossip_json_escape(self_addr)
                    << "\",\"version\":\"" << gossip_json_escape(std::string(kBridgeSessionsVersion))
                    << "\",\"status\":\"self\"";
                emit_host_fields(out, self_hs);
                out << "}";
                mark(config_.node_name);

                // Live mesh peers
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    if (c.peer_name.empty()) continue;
                    bool ok = (now - c.last_pong) <= fresh;
                    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                        now - c.connected_at).count();
                    // Prefer configured seed listen addr (stable) over conn
                    // peer_addr (often an ephemeral inbound source port).
                    std::string addr = peer_listen_addr_for(c.peer_name, c.peer_pubkey);
                    if (addr.empty()) addr = seed_addr_for(c.peer_name);
                    if (addr.empty()) addr = c.peer_addr;
                    out << ",\"" << gossip_json_escape(c.peer_name) << "\":{";
                    out << "\"addr\":\"" << gossip_json_escape(addr) << "\",";
                    out << "\"version\":\"" << gossip_json_escape(c.remote_version) << "\",";
                    out << "\"status\":\"" << (ok ? "healthy" : "no-pong") << "\",";
                    out << "\"uptime_s\":" << uptime;
                    emit_host_from_json(out, c.remote_host_stats_json);
                    out << "}";
                    mark(c.peer_name);
                }

                // Configured seeds with no live conn → offline (full fleet directory)
                for (const auto& s : config_.seeds) {
                    if (s.name.empty()) continue;
                    if (peer_name_eq(s.name, config_.node_name)) continue;
                    if (listed_has(s.name)) continue;
                    out << ",\"" << gossip_json_escape(s.name) << "\":{";
                    out << "\"name\":\"" << gossip_json_escape(s.name) << "\",";
                    out << "\"addr\":\"" << gossip_json_escape(s.addr) << "\",";
                    out << "\"version\":\"\",";
                    out << "\"status\":\"offline\",";
                    out << "\"dial_health\":\"" << gossip_json_escape(seed_dial_health(s.addr, now)) << "\",";
                    out << "\"source\":\"seed\",";
                    out << "\"metrics\":false";
                    out << "}";
                    mark(s.name);
                }

                // Runtime-learned (discovered) peers with no live conn → stale
                // (ephemeral rentals that have not been seen for a while but are
                // still within mesh.discovered_ttl_secs before auto-prune).
                for (const auto& d : config_.discovered) {
                    if (d.name.empty()) continue;
                    if (peer_name_eq(d.name, config_.node_name)) continue;
                    if (listed_has(d.name)) continue;
                    out << ",\"" << gossip_json_escape(d.name) << "\":{";
                    out << "\"name\":\"" << gossip_json_escape(d.name) << "\",";
                    out << "\"addr\":\"" << gossip_json_escape(d.addr) << "\",";
                    out << "\"version\":\"\",";
                    out << "\"status\":\"stale\",";
                    out << "\"dial_health\":\"" << gossip_json_escape(seed_dial_health(d.addr, now)) << "\",";
                    out << "\"source\":\"discovered\",";
                    out << "\"last_seen\":" << d.last_seen << ",";
                    out << "\"metrics\":false";
                    out << "}";
                    mark(d.name);
                }
                out << "}\n";
                response = out.str();
            }
            else if (line.rfind("SCROLLBACK ", 0) == 0) {
                // SCROLLBACK <session> <since_byte> → OK <new_offset> <b64>[ RESET]
                auto rest = line.substr(11);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: SCROLLBACK <session> <since_byte>\n";
                } else {
                    std::string sname = rest.substr(0, sp);
                    size_t since = 0;
                    bool bad_offset = false;
                    try { since = static_cast<size_t>(std::stoull(rest.substr(sp + 1))); }
                    catch (...) { bad_offset = true; }
                    if (bad_offset) {
                        response = "ERROR bad offset\n";
                    } else {
                        auto* s = sessions_.get(sname);
                        if (!s) {
                            response = "ERROR no such session\n";
                        } else {
                            auto [chunk, reset] = s->scrollback.read_since(since);
                            // On RESET the client fast-forwards to the live edge.
                            size_t new_off = reset ? s->scrollback.total_written()
                                                   : since + chunk.size();
                            response = "OK " + std::to_string(new_off) + " "
                                     + b64enc(chunk) + (reset ? " RESET" : "") + "\n";
                        }
                    }
                }
            }
            else if (line == "MESH_TREE") {
                response = build_mesh_tree_json() + "\n";
            }
            else if (line == "TELEMETRY") {
                response = transfer_telemetry_.to_json() + "\n";
            }
            else if (line.rfind("CONV_APPEND ", 0) == 0) {
                // CONV_APPEND <conv_id> <role> <b64_body> → OK <seq>
                auto rest = line.substr(12);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? sp1 : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: CONV_APPEND <conv> <role> <b64>\n";
                } else {
                    std::string conv = rest.substr(0, sp1);
                    std::string role = rest.substr(sp1 + 1, sp2 - sp1 - 1);
                    std::string body_b64 = rest.substr(sp2 + 1);
                    uint8_t role_u8;
                    if (role == "system") role_u8 = 0;
                    else if (role == "user") role_u8 = 1;
                    else if (role == "agent") role_u8 = 2;
                    else if (role == "tool") role_u8 = 3;
                    else {
                        response = "ERROR bad role\n";
                        role_u8 = 255;
                    }
                    if (role_u8 != 255) {
                        ConversationAppendMsg ca;
                        ca.conv_id = conv;
                        ca.role = role_u8;
                        ca.agent_id = "ipc";
                        ca.body = b64dec(body_b64);
                        if (ca.body.size() > 65535) {
                            // Wire limit (u16-prefixed) — reject before store
                            // so a later ConversationQuery serialize can never throw.
                            response = "ERROR body too large (max 65535 bytes)\n";
                        } else {
                        using namespace std::chrono;
                        ca.ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                        {
                            std::lock_guard lock(conversations_mutex_);
                            ca.seq = next_conv_seq_++;
                            conversations_[ca.conv_id].push_back(ca);
                            // Same bounds as the mesh path (2.0.8 MoA).
                            auto& vec = conversations_[ca.conv_id];
                            static constexpr size_t kMaxMsgsPerConv = 10000;
                            if (vec.size() > kMaxMsgsPerConv)
                                vec.erase(vec.begin(), vec.begin() + (vec.size() - kMaxMsgsPerConv));
                            static constexpr size_t kMaxConvs = 1024;
                            if (conversations_.size() > kMaxConvs)
                                conversations_.erase(conversations_.begin());
                        }
                        log_event("conversation_append", ca.conv_id + " seq=" + std::to_string(ca.seq) + " via=ipc");
                        response = "OK " + std::to_string(ca.seq) + "\n";
                        }
                    }
                }
            }
            else if (line.rfind("FILE_SEND ", 0) == 0) {
                // FILE_SEND <peer> <local-path>
                auto rest = line.substr(10);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_SEND <peer> <path>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string path = rest.substr(sp + 1);
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileSendWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        worker_pool_->enqueue(std::move(task));
                        response = "OK queued send to " + peer_name + "\n";
                    }
                }
            }
            else if (line.rfind("FILE_SEND_WAIT_B64 ", 0) == 0) {
                // FILE_SEND_WAIT_B64 <peer> <b64-local-path> [b64-dest-path]
                auto rest = line.substr(19);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_SEND_WAIT_B64 <peer> <b64-local-path> [b64-dest]\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string rest2 = rest.substr(sp + 1);
                    auto sp2 = rest2.find(' ');
                    std::string path, dest;
                    if (sp2 == std::string::npos) {
                        path = b64dec(rest2);
                    } else {
                        path = b64dec(rest2.substr(0, sp2));
                        dest = b64dec(rest2.substr(sp2 + 1));
                    }
                    // v2.0.6: offload the long transfer to a worker thread. The
                    // worker owns the IPC socket and streams PROGRESS + final response.
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileSendWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.path2 = dest;  // scp-style remote dest (optional)
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        task.ipc_fd = cfd;
                        worker_pool_->enqueue(std::move(task));
                        // IPC socket ownership transferred; do not send/close here.
                        response.clear();
                        response_sent = true;
                    }
                }
            }
            else if (line.rfind("FILE_RECV ", 0) == 0) {
                auto rest = line.substr(10);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_RECV <peer> <remote-path>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string path = rest.substr(sp + 1);
                    std::string result = daemon_file_recv(peer_name, path);
                    response = result + "\n";
                }
            }
            else if (line.rfind("FILE_RECV_B64 ", 0) == 0) {
                auto rest = line.substr(14);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? std::string::npos : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: FILE_RECV_B64 <peer> <b64-remote-path> <b64-local-dir>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp1);
                    std::string path = b64dec(rest.substr(sp1 + 1, sp2 - sp1 - 1));
                    std::string local_dir = b64dec(rest.substr(sp2 + 1));
                    response = daemon_file_recv(peer_name, path, local_dir) + "\n";
                }
            }
            else if (line.rfind("FILE_RECV_WAIT_B64 ", 0) == 0) {
                auto rest = line.substr(19);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? std::string::npos : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: FILE_RECV_WAIT_B64 <peer> <b64-remote-path> <b64-local-dest>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp1);
                    std::string path = b64dec(rest.substr(sp1 + 1, sp2 - sp1 - 1));
                    std::string local_dest = b64dec(rest.substr(sp2 + 1));
                    // v2.0.6: offload to worker thread; worker owns IPC socket.
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileRecvWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.path2 = local_dest;
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        task.ipc_fd = cfd;
                        worker_pool_->enqueue(std::move(task));
                        response.clear();
                        response_sent = true;
                    }
                }
            }
            else if (line.rfind("SHELL ", 0) == 0) {
                // Interactive and one-shot shell commands use a dedicated direct
                // TLS connection. A detached worker must never borrow a mesh
                // connection's SSL object from the event loop.
                response = shell_ipc_relay_policy_response();
            }
            else if (line.rfind("CANCEL ", 0) == 0) {
                std::string peer_name = line.substr(7);
                Conn* target = nullptr;
                // Allow cancelling a busy transport; the worker exclusively owns it
                // while exec_busy is set, so require_idle must be false.
                for (auto& c : conns_) {
                    if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                }
                if (!target) {
                    response = "ERROR no conn to " + peer_name + "\n";
                } else if (!target->exec_busy || !target->exec_busy->load()) {
                    response = "OK no active operation on " + peer_name + "\n";
                } else {
                    if (target->exec_cancelled) target->exec_cancelled->store(true);
                    if (target->sock_fd != INVALID_SOCKET) {
#ifdef _WIN32
                        ::shutdown(target->sock_fd, SD_BOTH);
#else
                        ::shutdown(target->sock_fd, SHUT_RDWR);
#endif
                    }
                    response = "OK cancelling operation on " + peer_name + "\n";
                }
            }
            else if (line.rfind("EDIT_DL ", 0) == 0) {
                response = "ERROR edit uses a dedicated direct TLS connection; upgrade the CLI\n";
            }
            else if (line.rfind("VFOLDER_SYNC ", 0) == 0) {
                response = "ERROR vfolder sync runs in the CLI over direct TLS\n";
            }
            else if (line.rfind("VFOLDER_LIST", 0) == 0) {
                std::string result = "[";
                for (auto& v : config_.vfolders) {
                    if (result.size() > 1) result += ",";
                    result += "{\"name\":\"" + v.name + "\",\"local\":\"" + v.local_path + "\",\"peer\":\"" + v.remote_peer + "\",\"remote\":\"" + v.remote_path + "\",\"interval\":" + std::to_string(v.sync_interval_secs) + ",\"direction\":\"" + v.direction + "\"}";
                }
                result += "]";
                response = result + "\n";
            }
            else if (line.rfind("CUA_VIDEO_CAPTURE_B64 ", 0) == 0) {
                auto rest = line.substr(22);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: CUA_VIDEO_CAPTURE_B64 <peer> <b64-fps-dur-qual-maxw>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string params_b64 = rest.substr(sp + 1);
                    std::string params = b64dec(params_b64);
                    // params format: "fps:duration:quality:maxw"
                    std::stringstream ss(params);
                    std::string token;
                    std::vector<int> vals;
                    while (std::getline(ss, token, ':')) {
                        // P3 audit fix: std::stoi throws on non-numeric input —
                        // malformed params crashed the daemon. Validate instead.
                        if (token.empty() ||
                            token.find_first_not_of("0123456789") != std::string::npos) {
                            vals.clear();
                            break;
                        }
                        try {
                            vals.push_back(std::stoi(token));
                        } catch (...) { vals.clear(); break; }
                    }
                    if (vals.size() < 4) {
                        response = "ERROR video capture: expected fps:duration:quality:maxw\n";
                    } else {
                        CuaVideoCaptureMsg req;
                        req.fps = static_cast<uint8_t>(vals[0]);
                        req.duration_sec = static_cast<uint16_t>(vals[1]);
                        req.quality = static_cast<uint8_t>(vals[2]);
                        req.max_width = static_cast<uint16_t>(vals[3]);
                        Conn* target = nullptr;
                        for (auto& c : conns_) {
                            if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                        }
                        if (!target) {
                            response = "ERROR no conn to " + peer_name + "\n";
                        } else {
                            (void)req;
                            response =
                                "ERROR CUA_VIDEO_CAPTURE requires direct TLS; "
                                "use: bs capture-video " + peer_name + "\n";
                        }
                    }
                }
            }
            else if (line.rfind("EDIT_UP ", 0) == 0) {
                response = "ERROR edit uses a dedicated direct TLS connection; upgrade the CLI\n";
            }
            if (response == "ERROR bad request\n") {
                auto separator = line.find(' ');
                log_event("ipc_bad_request",
                          "verb=" + line.substr(0, separator) +
                          " bytes=" + std::to_string(line.size()));
            }
        }
        if (!response_sent) {
            // 2.0.8 MoA fix: send() may short-write (large SCROLLBACK/MESH_TREE
            // replies). Loop until the full reply is out or the socket fails —
            // a truncated reply silently corrupts the client's incremental sync.
            size_t sent_total = 0;
            while (sent_total < response.size()) {
                int snt = send(cfd, response.data() + sent_total,
                               static_cast<int>(response.size() - sent_total), 0);
                if (snt <= 0) break; // timeout/closed — client retries from last offset
                sent_total += static_cast<size_t>(snt);
            }
            if (!newline_seen) {
                // Overlong/truncated request: the client may still be sending.
                // Closing a socket with unread receive data triggers RST,
                // which can destroy our own in-flight response (observed as
                // ECONNRESET client-side). Half-close the write side, then
                // drain briefly so the peer's remaining bytes land harmlessly.
#ifdef _WIN32
                ::shutdown(cfd, SD_SEND);
#else
                ::shutdown(cfd, SHUT_WR);
#endif
                char sink[4096];
                const auto drain_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(300);
                while (std::chrono::steady_clock::now() < drain_deadline) {
                    int got = recv(cfd, sink, static_cast<int>(sizeof(sink)), 0);
                    if (got <= 0) break; // EOF (client done) or 250ms timeout
                }
            }
            CLOSESOCK(cfd);
        }
        // else: worker owns cfd and will close it after streaming the response.
    }

    std::string daemon_health_via_ipc(const std::string& peer_name, int wait_ms) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        // Bound connect + recv so a stalled/dead daemon can never hang the CLI.
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 8000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string req = token + " HEALTH " + peer_name + "\n";
        send(sfd, req.data(), (int)req.size(), 0);
        char buf[256] = {};
        int total = 0;
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (std::chrono::steady_clock::now() < dl && total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) {
                total += n; buf[total] = '\0';
                if (strchr(buf, '\n')) break;
            } else if (n == 0) {
                break;  // peer closed
            } else {
                // recv timed out (SO_RCVTIMEO) or transient error; stop, don't spin.
                break;
            }
        }
        CLOSESOCK(sfd);
        if (total <= 0) return "";
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    }

    // CLI-side: send a file via daemon IPC (reads local file, sends FILE_SEND command,
    // blocks for response with longer wait_ms to cover transfer time).
    std::string daemon_send_via_ipc(const std::string& peer_name, const std::string& path,
                                    int wait_ms, bool wait_for_completion = false,
                                    const std::string& dest_path = {}) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        int to = wait_for_completion ? std::max(wait_ms, 7200000) : (wait_ms > 0 ? wait_ms : 120000);
        set_socket_timeouts(sfd, to);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string cmd = token + " FILE_SEND_WAIT_B64 " + peer_name + " " + b64enc(path);
        if (!dest_path.empty()) cmd += " " + b64enc(dest_path);
        cmd += "\n";
        // Token-authenticated 127.0.0.1 IPC. HOME-derived paths in the
        // command are not an unauthorized disclosure.
        send(sfd, cmd.data(), (int)cmd.size(), 0); // codeql[cpp/system-data-exposure]
        std::string pending;
        char buf[8192];
        while (true) {
            int n = recv(sfd, buf, (int)sizeof(buf) - 1, 0);
            if (n <= 0) break;
            auto terminal = consume_transfer_ipc_chunk(
                pending, std::string_view(buf, static_cast<size_t>(n)),
                [](const std::string& line) { std::cerr << line << "\n"; });
            if (terminal) {
                CLOSESOCK(sfd);
                return *terminal;
            }
        }
        CLOSESOCK(sfd);
        while (!pending.empty() && (pending.back() == '\r' || pending.back() == '\n'))
            pending.pop_back();
        return pending;
    }

    // CLI-side: shell command relay through daemon IPC.
    // Returns: exit_code on success (>=0), -1 = daemon-unreachable, -2 = timeout.
    // Writes stdout to 'output' param.
    int daemon_shell_via_ipc(const std::string& peer_name, const std::string& session_name,
                             const std::string& cmd, std::string* output,
                             int wait_ms = 60000) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return -1;
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return -1;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 60000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return -1;
        }
        std::string req = token + " SHELL " + peer_name + " "
                        + b64enc(session_name) + " "
                        + b64enc(cmd) + "\n";
        // Token-authenticated 127.0.0.1 IPC. HOME-derived paths in the
        // command are not an unauthorized disclosure.
        send(sfd, req.data(), (int)req.size(), 0); // codeql[cpp/system-data-exposure]
        char buf[65536] = {}; int total = 0;
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (std::chrono::steady_clock::now() < dl && total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
            else break;
        }
        CLOSESOCK(sfd);
        if (total <= 0) return (std::chrono::steady_clock::now() > dl) ? -2 : -1;
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        // v1.6 daemons without SHELL handler return "ERROR bad request" (plaintext),
        // not base64. Route through daemon_shell_via_ipc caller's fallback.
        if (line.rfind("ERROR ", 0) == 0) {
            *output = line.substr(6);
            return -1;
        }
        std::string decoded = b64dec(line);
        auto colon = decoded.find(':');
        if (colon == std::string::npos) { *output = decoded; return 0; }
        int exit_code = 0;
        try { exit_code = std::stoi(decoded.substr(0, colon)); }
        catch (...) { /* non-numeric prefix (e.g. C:\... paths) → exit 0 */ }
        *output = decoded.substr(colon + 1);
        return exit_code;
    }

    // CLI-side: request a file from a peer via daemon IPC.
    std::string daemon_recv_via_ipc(const std::string& peer_name, const std::string& path,
                                    const std::string& local_dest, int wait_ms,
                                    bool wait_for_completion = false) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        // Large transfers stream PROGRESS lines then final OK/ERROR; allow hours.
        int to = wait_for_completion ? std::max(wait_ms, 7200000) : (wait_ms > 0 ? wait_ms : 120000);
        set_socket_timeouts(sfd, to);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string cmd = token + " FILE_RECV_WAIT_B64 " + peer_name + " " + b64enc(path) + " " + b64enc(local_dest) + "\n";
        send(sfd, cmd.data(), (int)cmd.size(), 0);
        std::string pending;
        char buf[8192];
        while (true) {
            const int n = recv(sfd, buf, static_cast<int>(sizeof(buf)), 0);
            if (n <= 0) break;
            auto terminal = consume_transfer_ipc_chunk(
                pending, std::string_view(buf, static_cast<size_t>(n)),
                [](const std::string& line) { std::cerr << line << "\n"; });
            if (terminal) {
                CLOSESOCK(sfd);
                return *terminal;
            }
        }
        CLOSESOCK(sfd);
        while (!pending.empty() && (pending.back() == '\r' || pending.back() == '\n'))
            pending.pop_back();
        return pending;
    }

    // Returns true if another bridgesessions daemon is already running locally
    // and proves possession of the owner-only IPC token. Used as a single-instance guard so a
    // double-click / second `bsmesh` launch cannot squat ports and split the mesh.
    bool another_daemon_running() {
        const std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return false;
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return false;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, 1500);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd);
            return false;
        }
        const std::string probe = token + " DAEMON_PROBE\n";
        if (send(sfd, probe.data(), static_cast<int>(probe.size()), 0) <= 0) {
            CLOSESOCK(sfd);
            return false;
        }
        char reply[64] = {};
        int n = recv(sfd, reply, static_cast<int>(sizeof(reply) - 1), 0);
        CLOSESOCK(sfd);
        return n > 0 && std::string_view(reply, static_cast<size_t>(n)).find(
                            "OK bridgesessions") != std::string_view::npos;
    }

    // ── Startup network readiness gate ────────────────────────
    // Probes each seed's TCP port with a 2s connect timeout. Returns when any
    // seed is reachable or after max_secs. Services inbound connections while
    // waiting so the daemon isn't deaf during boot.
    void startup_wait_for_network(int max_secs) {
        if (max_secs <= 0 || config_.seeds.empty()) return;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(max_secs);
        log_event("startup_network_wait_begin",
                  "max_secs=" + std::to_string(max_secs) +
                  " seeds=" + std::to_string(config_.seeds.size()));

        while (running_ && std::chrono::steady_clock::now() < deadline) {
            for (const auto& seed : config_.seeds) {
                if (seed.addr.empty()) continue;
                sockaddr_in sa = resolve_addr(seed.addr);
                SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sfd == INVALID_SOCKET) continue;
                set_socket_timeouts(sfd, 2000);
                auto cr = connect_socket_with_timeout(
                    sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa), 2000);
                if (cr.connected) {
                    CLOSESOCK(sfd);
                    log_event("startup_network_wait_ok", seed.addr);
                    return;
                }
                CLOSESOCK(sfd);
            }
            // Service inbound connections while waiting (non-blocking 1s tick).
            if (listen_fd_ != INVALID_SOCKET)
                service_reconnect_wait_once(1000);
            else
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        log_event("startup_network_wait_timeout",
                  "no seed reachable in " + std::to_string(max_secs) + "s");
    }

    // ── Main event loop ───────────────────────────────────────

    void run() {
        running_ = true;

        // P0 UAF fix: wire session-erased callback to null dangling attached_session pointers
        sessions_.set_on_session_erased([this](const std::string& name) {
            for (auto& c : conns_)
                if (c.attached_session && c.attached_session->name == name)
                    c.attached_session = nullptr;
        });

        // The event loop also accepts inbound CLI sessions. Keep seed dials short so
        // several offline/discovered peers cannot starve accept() for 3s each.
        outbound_connect_timeout_ms_ = 1000;

        // Single-instance guard: if a daemon already owns the CLI IPC port,
        // refuse to start a second one. SO_REUSEADDR otherwise lets a second
        // process silently co-bind the mesh port and split-brain the mesh.
        if (another_daemon_running()) {
            log_event("mesh_already_running",
                      "another daemon is listening on CLI IPC port "
                      + std::to_string(mesh_cli_port()) + "; refusing to start");
            std::cerr << "bridgesessions: another daemon already running (IPC port "
                      << mesh_cli_port() << "). Refusing to start a second instance.\n";
            running_ = false;
            return;
        }
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_handler = sighup_reload_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGHUP, &sa, nullptr);  // R4.2
#endif

        // Create listen socket
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == INVALID_SOCKET) {
            log_event("mesh_listen_socket_failed");
            return;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in listen_sa{};
        listen_sa.sin_family = AF_INET;
        listen_sa.sin_addr.s_addr = inet_addr(config_.listen_addr.c_str());
        if (listen_sa.sin_addr.s_addr == INADDR_NONE) {
            listen_sa.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        listen_sa.sin_port = htons(config_.listen_port);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&listen_sa), sizeof(listen_sa)) == SOCKET_ERROR) {
            int err =
#ifdef _WIN32
                WSAGetLastError();
#else
                errno;
#endif
            log_event("mesh_listen_bind_failed", "errno=" + std::to_string(err));  // R3.5
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR) {
            log_event("mesh_listen_failed");
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        sockaddr_in actual_addr{};
        socklen_t actual_len = sizeof(actual_addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual_addr), &actual_len) == 0)
            actual_listen_port_.store(ntohs(actual_addr.sin_port));

        // Make listen socket non-blocking so accept() never blocks the loop.
#ifdef _WIN32
        { u_long nb = 1; ioctlsocket(listen_fd_, FIONBIO, &nb); }
#else
        { int fl = fcntl(listen_fd_, F_GETFL, 0); if (fl >= 0) fcntl(listen_fd_, F_SETFL, fl | O_NONBLOCK); }
        // poll() has no FD_SETSIZE limit — high FDs are fine.
#endif

        log_event("mesh_listening", config_.listen_addr + ":" + std::to_string(config_.listen_port));

        if (!cli_ipc_init()) {
            log_event("mesh_cli_ipc_failed", "daemon startup aborted");
            running_ = false;
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        if (config_.mdns_enabled) mdns_init();
        last_ping_time_ = std::chrono::steady_clock::now();
        last_gossip_time_ = std::chrono::steady_clock::now();
        last_mdns_time_ = std::chrono::steady_clock::now();

        // Restore persisted session metadata, then re-adopt live session
        // workers whose shells survived our restart/upgrade. Sessions with no
        // live worker stay Recoverable (reattach = fresh shell, as before).
        try { sessions_.load_persisted_sessions(); }
        catch (const std::exception& e) { log_event("session_load_failed", e.what()); }
        catch (...) { log_event("session_load_failed", "unknown error"); }
#ifndef _WIN32
        try { sessions_.adopt_workers(); }
        catch (const std::exception& e) { log_event("session_adopt_failed", e.what()); }
        catch (...) { log_event("session_adopt_failed", "unknown error"); }
#endif

        // Boot-time network readiness gate: wait for at least one seed before
        // entering the event loop (config: mesh.startup_wait_secs, default 30).
        startup_wait_for_network(config_.startup_wait_secs);

        while (running_) {
            // 1. Build pollfd set (poll/WSAPoll — no FD_SETSIZE ceiling).
            std::vector<bs_pollfd> pfds;
            pfds.reserve(8 + conns_.size() + pending_handshakes_.size());

            auto add_poll = [&](SOCKET fd, short events) {
                if (fd == INVALID_SOCKET) return;
                bs_pollfd p{};
                p.fd = static_cast<decltype(p.fd)>(fd);
                p.events = events;
                p.revents = 0;
                pfds.push_back(p);
            };

            add_poll(listen_fd_, POLLIN);
            // CLI IPC listen — event-driven so health is not starved mid-handshake.
            if (cli_listen_fd_ != INVALID_SOCKET)
                add_poll(cli_listen_fd_, POLLIN);

            for (auto& c : conns_) {
                // Skip conns whose socket/SSL is owned by a worker (exec_busy).
                if (c.exec_busy && c.exec_busy->load()) continue;
#ifdef _WIN32
                if (c.attached_session &&
                    windows_pty_pending_bytes_.load() >= kWindowsPtyInputHighWater)
                    continue;
#else
                if (c.attached_session && c.attached_session->input_backpressured)
                    continue;
#endif
                if (c.sock_fd != INVALID_SOCKET) {
                    short ev = POLLIN;
                    if (c.want_write || !c.tx_queue.empty() || !c.output_queue.empty()
                        || c.output_gap_pending)
                        ev = static_cast<short>(ev | POLLOUT);
                    add_poll(c.sock_fd, ev);
                }
            }
#ifndef _WIN32
            // PTY masters wake the loop immediately (avoids torn TUI frames).
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || !session->is_pollable() || session->master_fd < 0)
                    continue;
                short ev = POLLIN;
                if (!session->pending_input.empty() ||
                    (session->hosted && !session->worker_tx.empty()))
                    ev = static_cast<short>(ev | POLLOUT);
                add_poll(session->master_fd, ev);
            }
#endif
            if (mdns_fd_ != INVALID_SOCKET)
                add_poll(mdns_fd_, POLLIN);

            for (auto& ph : pending_handshakes_) {
                if (ph.sock_fd == INVALID_SOCKET) continue;
                short ev = 0;
                if (ph.want_read) ev = static_cast<short>(ev | POLLIN);
                if (ph.want_write) ev = static_cast<short>(ev | POLLOUT);
                if (ev == 0) ev = POLLIN; // always watch something
                add_poll(ph.sock_fd, ev);
            }

            // Adaptive timeout: Windows needs faster ConPTY polling when busy.
#ifdef _WIN32
            int poll_timeout_ms = sessions_.empty() ? 500 : 50;
#else
            int poll_timeout_ms = 100;
#endif
            int nfds = pfds.empty()
                ? 0
                : bs_poll(pfds.data(),
#ifdef _WIN32
                          static_cast<unsigned long>(pfds.size()),
#else
                          static_cast<nfds_t>(pfds.size()),
#endif
                          poll_timeout_ms);

            if (nfds < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                log_event("mesh_poll_error", "errno=" + std::to_string(
#ifdef _WIN32
                    WSAGetLastError()
#else
                    errno
#endif
                ));
                continue;
            }
            // Snapshot readiness for the rest of the tick (pfds holds revents).
            const std::vector<bs_pollfd>& ready = pfds;

            auto now = std::chrono::steady_clock::now();
            maybe_reload_config_seeds();
            maybe_prune_revoked_connections();
            maybe_close_join_window();
            if (now - last_session_prune_time_ >= std::chrono::minutes(1)) {
                if (config_.idle_timeout_hours > 0)
                    sessions_.prune_idle(std::chrono::hours(config_.idle_timeout_hours));
                // Always prune agent/health one-shots — they accumulate PTYs otherwise.
                sessions_.prune_ephemeral_sessions(std::chrono::seconds(90));
                last_session_prune_time_ = now;
            }
            if (now - last_discovered_prune_time_ >= std::chrono::minutes(1)) {
                prune_stale_discovered_peers();
                last_discovered_prune_time_ = now;
            }
#ifndef _WIN32
            if (g_config_reload_requested.exchange(false))
                reload_seeds_from_disk();
#endif

            // Service CLI IPC the moment a request arrives (event-driven).
            if (cli_listen_fd_ != INVALID_SOCKET && poll_fd_readable(ready, cli_listen_fd_)) {
                cli_ipc_accept_one();
            }

            // 3. Accept new connections
            if (poll_fd_readable(ready, listen_fd_)) {
                accept_inbound();
            }
            // 3.5. mDNS read
            if (mdns_fd_ != INVALID_SOCKET && poll_fd_readable(ready, mdns_fd_)) {
                mdns_check();
            }

            // 4. Read from established connections
            for (int i = 0; i < static_cast<int>(conns_.size()); ++i) {
                auto& conn = conns_[static_cast<size_t>(i)];
                if (conn.exec_busy && conn.exec_busy->load()) continue;
                if (conn.sock_fd != INVALID_SOCKET &&
                    poll_fd_readable(ready, conn.sock_fd)) {
                    check_conn_read(i);
                }
            }

            // 4.2. Writable sockets: flush non-blocking TX queues.
            for (auto& conn : conns_) {
                if (conn.exec_busy && conn.exec_busy->load()) continue;
                if (conn.sock_fd == INVALID_SOCKET) continue;
                if (conn.want_write || !conn.tx_queue.empty()) {
                    if (poll_fd_writable(ready, conn.sock_fd) || SSL_pending(conn.ssl.get()) > 0
                        || !conn.tx_queue.empty()) {
                        flush_tx_queue(conn);
                    }
                }
            }

            // 4.5. Advance non-blocking TLS + Hello handshakes.
            // Run unconditionally: progress may be driven by SSL buffered data too.
            advance_handshakes();

            // 5. Connect to seeds / discovered peers
            try_connect_to_seeds();

            // 6. Ping broadcast
            auto ping_interval = std::chrono::seconds(config_.ping_interval_secs);
            if (now - last_ping_time_ >= ping_interval) {
                broadcast_ping();
                last_ping_time_ = now;
            }

            // 7. Pong timeout check
            check_pong_timeouts();

            // 7b. Stale exec watchdog (BUG-1)
            check_stale_exec();

            // 8. Gossip broadcast
            auto gossip_interval = std::chrono::seconds(config_.gossip_interval_secs);
            if (now - last_gossip_time_ >= gossip_interval) {
                broadcast_gossip();
                last_gossip_time_ = now;
            }

            // 8.5. mDNS announce (every 30s)
            if (mdns_fd_ != INVALID_SOCKET && now - last_mdns_time_ >= std::chrono::seconds(30)) {
                mdns_announce();
                last_mdns_time_ = now;
            }
            // 9. Clean dead connections
            clean_dead_conns();

            // 9.5. Poll PTY output for all attached sessions
            pty_output_poller();

            // 9.6. Drain per-connection output queues (2.0.8 P3 streaming)
            drain_output_queues();

#ifndef _WIN32
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || session->master_fd < 0)
                    continue;
                if (session->pending_input.empty() &&
                    !(session->hosted && !session->worker_tx.empty()))
                    continue;
                if (poll_fd_writable(ready, session->master_fd))
                    (void)drain_pending_pty_input(*session);
            }
#endif

            // 10. Reap dead sessions
            // Attached-session death is owned by pty_output_poller(), which
            // drains final output and emits SessionDied. Reaping it here first
            // steals waitpid() and leaves the client waiting forever.
            sessions_.reap_dead(false);
            (void)nfds; // readiness is driven by poll revents, not the count
        }

        // P1 fix: persist sessions on graceful shutdown
        try { sessions_.save_persisted_sessions(); }
        catch (const std::exception& e) { log_event("session_save_failed", e.what()); }
        catch (...) { log_event("session_save_failed", "unknown error"); }
    }

    // ── mDNS LAN discovery ─────────────────────────────────────

    void mdns_init() {
        mdns_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mdns_fd_ == INVALID_SOCKET) return;
        int yes = 1;
        setsockopt(mdns_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(kMdnsPort);
        if (bind(mdns_fd_, (sockaddr*)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
            CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET; return;
        }
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(mdns_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
            CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET; return;
        }
#ifdef _WIN32
        u_long mode = 1; ioctlsocket(mdns_fd_, FIONBIO, &mode);
#else
        int flags = fcntl(mdns_fd_, F_GETFL, 0); fcntl(mdns_fd_, F_SETFL, flags | O_NONBLOCK);
#endif
        log_event("mdns_init", std::string("listening on ") + kMdnsGroup + ":" + std::to_string(kMdnsPort));
    }

    void mdns_announce() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        nlohmann::json j;
        j["name"] = config_.node_name; j["port"] = config_.listen_port; j["pubkey"] = our_pubkey_;
#ifndef BS_NO_NAT
        if (!external_addr_.empty()) {
            j["wan"] = external_addr_;
        }
#endif
        std::string payload = j.dump();
        sockaddr_in dest{};
        dest.sin_family = AF_INET; dest.sin_addr.s_addr = inet_addr(kMdnsGroup); dest.sin_port = htons(kMdnsPort);
        sendto(mdns_fd_, payload.data(), (int)payload.size(), 0, (sockaddr*)&dest, sizeof(dest));
    }

    void process_mdns_announcement(const std::string& name,
                                   const std::string& addr,
                                   const std::string& pubkey) {
        if (pubkey.empty() || pubkey == our_pubkey_) return;
        if (!is_trusted_pubkey(pubkey)) return;
        for (auto& s : config_.seeds) {
            if (!s.pubkey_hex.empty() && s.pubkey_hex == pubkey) {
                if (!addr.empty()) s.addr = addr;
                s.last_seen = now_unix_seconds();
                log_event("mdns_address_update", s.name + " " + addr);
                return;
            }
            if (peer_name_eq(s.name, name) && s.pubkey_hex != pubkey) return;
        }
        for (auto& d : config_.discovered) {
            if (d.pubkey_hex == pubkey) {
                if (!addr.empty()) d.addr = addr;
                if (!name.empty()) d.name = name;
                d.last_seen = now_unix_seconds();
                log_event("mdns_address_update", d.name + " " + addr);
                return;
            }
            if (peer_name_eq(d.name, name) && d.pubkey_hex != pubkey) return;
        }
        PeerEntry pe{name, addr, pubkey, now_unix_seconds()};
        config_.discovered.push_back(std::move(pe));
        log_event("mdns_discovered", name + " " + addr);
    }

    void mdns_check() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        char buf[2048]; sockaddr_in from{}; socklen_t from_len = sizeof(from);
        int n = recvfrom(mdns_fd_, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &from_len);
        if (n <= 0) return; buf[n] = '\0';
        try {
            auto j = nlohmann::json::parse(buf);
            if (!j.contains("name") || !j.contains("port") || !j.contains("pubkey")) return;
            std::string name = j["name"], pubkey = j["pubkey"];
            if (pubkey == our_pubkey_) return;
            char ip_str[64]; inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
            std::string addr = std::string(ip_str) + ":" + std::to_string(j["port"].get<int>());
            process_mdns_announcement(name, addr, pubkey);
        } catch (...) {}
    }

    void mdns_shutdown() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        ip_mreq mreq{}; mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup); mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(mdns_fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
        CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET;
    }

    // ── Common: resolve peer → addr ─────────────────────────────
    static bool peer_name_eq(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    }

    std::string find_peer_addr(const std::string& peer_name) const {
        for (auto& s : config_.seeds) if (peer_name_eq(s.name, peer_name)) return s.addr;
        for (auto& d : config_.discovered) if (peer_name_eq(d.name, peer_name)) return d.addr;
        return "";
    }

    // ── Peer name resolution: 4-tier fuzzy matching ────────────
    // Tier 1: exact case-insensitive (backward-compatible)
    // Tier 2: (reserved for config aliases — not yet in config format)
    // Tier 3: hyphen-segment suffix/prefix (shadow → lab-shadow)
    // Tier 4: Levenshtein ≤ 2 (shadwo → shadow)
    struct PeerResolveResult {
        std::string name;       // resolved canonical name, empty if not found
        std::string addr;       // "host:port"
        std::string pubkey_hex;
        enum Tier { None_, Exact, Suffix, Levenshtein } tier = None_;
        std::vector<std::string> suggestions; // when ambiguous, the candidates
    };

    // Collect all configured peer names (seeds + discovered).
    std::vector<std::string> all_peer_names() const {
        std::vector<std::string> names;
        names.reserve(config_.seeds.size() + config_.discovered.size());
        for (const auto& s : config_.seeds) names.push_back(s.name);
        for (const auto& d : config_.discovered) names.push_back(d.name);
        return names;
    }

    // Look up addr+pubkey for a canonical name.
    bool peer_lookup(const std::string& canonical,
                     std::string& addr, std::string& pubkey) const {
        for (const auto& s : config_.seeds)
            if (peer_name_eq(s.name, canonical)) { addr = s.addr; pubkey = s.pubkey_hex; return true; }
        for (const auto& d : config_.discovered)
            if (peer_name_eq(d.name, canonical)) { addr = d.addr; pubkey = d.pubkey_hex; return true; }
        return false;
    }

    [[nodiscard]] PeerResolveResult resolve_peer(const std::string& query) const {
        PeerResolveResult r;
        // Never treat this node as a remote peer (would fuzzy-remap node-3→node-4).
        if (is_local_node_name(query, config_.node_name)) return r;
        // Tier 1: exact match (case-insensitive)
        std::string addr, pubkey;
        if (peer_lookup(query, addr, pubkey)) {
            r.name = query; r.addr = addr; r.pubkey_hex = pubkey;
            r.tier = PeerResolveResult::Exact;
            return r;
        }
        // Tier 3: hyphen-segment suffix/prefix
        std::vector<std::string> segment_matches;
        auto check_segments = [&](const std::vector<PeerEntry>& peers) {
            for (const auto& p : peers) {
                if (is_local_node_name(p.name, config_.node_name)) continue;
                if (name_has_segment(p.name, query))
                    segment_matches.push_back(p.name);
            }
        };
        check_segments(config_.seeds);
        check_segments(config_.discovered);
        // P2 fix: actually deduplicate (same peer in seeds + discovered)
        std::sort(segment_matches.begin(), segment_matches.end());
        segment_matches.erase(std::unique(segment_matches.begin(), segment_matches.end()), segment_matches.end());
        if (segment_matches.size() == 1) {
            if (peer_lookup(segment_matches[0], addr, pubkey)) {
                r.name = segment_matches[0]; r.addr = addr; r.pubkey_hex = pubkey;
                r.tier = PeerResolveResult::Suffix;
                return r;
            }
        }
        if (segment_matches.size() > 1) {
            r.suggestions = segment_matches;
            return r; // ambiguous
        }
        // Tier 4: Levenshtein ≤ 2 (typos). Digit-only siblings (node-3/node-4) are not typos.
        std::vector<std::string> fuzzy_matches;
        auto check_fuzzy = [&](const std::vector<PeerEntry>& peers) {
            for (const auto& p : peers) {
                if (query.empty() || query.size() < 3 || p.name.size() < 3) continue;
                if (is_local_node_name(p.name, config_.node_name)) continue;
                if (names_are_numeric_siblings(query, p.name)) continue;
                if (levenshtein(query, p.name) <= 2)
                    fuzzy_matches.push_back(p.name);
            }
        };
        check_fuzzy(config_.seeds);
        check_fuzzy(config_.discovered);
        if (fuzzy_matches.size() == 1) {
            if (peer_lookup(fuzzy_matches[0], addr, pubkey)) {
                r.name = fuzzy_matches[0]; r.addr = addr; r.pubkey_hex = pubkey;
                r.tier = PeerResolveResult::Levenshtein;
                return r;
            }
        }
        if (fuzzy_matches.size() > 1) {
            r.suggestions = fuzzy_matches;
            return r; // ambiguous
        }
        // No match at any tier
        r.suggestions = segment_matches.empty() ? fuzzy_matches : segment_matches;
        return r;
    }

    // Print "Peer not found" with suggestions and available peers list.
    void print_peer_not_found(const std::string& query) const {
        if (is_local_node_name(query, config_.node_name)) {
            std::cerr << "Cannot shell to this node (\"" << config_.node_name
                      << "\"). You are already here.\n";
            return;
        }
        auto result = resolve_peer(query);
        bool showed_suggestions = false;
        if (!result.suggestions.empty()) {
            showed_suggestions = true;
            std::cerr << "Ambiguous peer name: \"" << query << "\"\n";
            std::cerr << "Did you mean one of these?\n";
            for (const auto& s : result.suggestions)
                std::cerr << "  " << s << "\n";
        } else {
            std::cerr << "Peer not found: \"" << query << "\"\n";
            // Offer fuzzy suggestions if any are close
            std::vector<std::string> close;
            auto check_close = [&](const std::vector<PeerEntry>& peers) {
                for (const auto& p : peers) {
                    if (query.empty() || query.size() < 3 || p.name.size() < 3) continue;
                    if (is_local_node_name(p.name, config_.node_name)) continue;
                    if (names_are_numeric_siblings(query, p.name)) continue;
                    if (levenshtein(query, p.name) <= 2)
                        close.push_back(p.name);
                }
            };
            check_close(config_.seeds);
            check_close(config_.discovered);
            if (!close.empty()) {
                showed_suggestions = true;
                std::cerr << "Did you mean one of these?\n";
                for (const auto& s : close)
                    std::cerr << "  " << s << "\n";
            }
        }
        // P3: only dump the full peer list when no suggestions were shown
        if (!showed_suggestions) {
            auto names = all_peer_names();
            if (!names.empty()) {
                std::cerr << "\nAvailable peers:\n";
                for (const auto& n : names)
                    std::cerr << "  " << n << "\n";
            } else {
                std::cerr << "\nNo peers configured. Use 'bs seed <host:port>' to add one.\n";
            }
        }
    }

    // print_connect_failure + connect_with_retry defined after SslConn/connect_and_hello below

    // Read frames until Pong or deadline (handles Gossip/Hello interleaved on mesh link).
    bool wait_for_pong(SSL* ssl, SOCKET sfd, std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remain.count() <= 0) break;
            int ms = static_cast<int>(std::min<int64_t>(remain.count(), 2000));
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sfd, &read_fds);
            timeval tv{};
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
#ifdef _WIN32
            int sel = select(0, &read_fds, nullptr, nullptr, &tv);
#else
            int sel = select(static_cast<int>(sfd) + 1, &read_fds, nullptr, nullptr, &tv);
#endif
            if (sel > 0 || SSL_pending(ssl) > 0) {
                try {
                    Message resp = read_frame(ssl);
                    if (std::holds_alternative<PongMsg>(resp)) return true;
                } catch (...) {
                    return false;
                }
            }
        }
        return false;
    }

    // ── Common: TCP + TLS + Hello ────────────────────────────────
    struct SslConn {
        SslPtr ssl;
        SOCKET sfd = INVALID_SOCKET;
        HelloMsg hello{};
        ConnectFailReason fail = ConnectFailReason::None;
        std::string fail_detail;
    };
    static std::string connect_fail_string(ConnectFailReason r) {
        switch (r) {
        case ConnectFailReason::Refused: return "refused";
        case ConnectFailReason::Timeout: return "timeout";
        case ConnectFailReason::TlsRejected: return "tls_rejected";
        case ConnectFailReason::HelloRejected: return "hello_rejected";
        default: return "unknown";
        }
    }
    SslConn connect_and_hello(const std::string& addr,
                              const std::string& expected_pubkey = {},
                              bool trust_on_first_use = false) {
        SslConn out;
        if (expected_pubkey.empty() && !trust_on_first_use) {
            out.fail = ConnectFailReason::TlsRejected;
            out.fail_detail = "peer key not pinned";
            return out;
        }
        sockaddr_in sa = resolve_addr(addr);
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) {
            out.fail = ConnectFailReason::Refused;
            out.fail_detail = "socket() failed";
            return out;
        }
        set_socket_timeouts(sfd, outbound_connect_timeout_ms_);
        set_tcp_nodelay(sfd);  // interactive shell performance
        { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }  // R3.6
        const auto connect_result = connect_socket_with_timeout(
            sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
            outbound_connect_timeout_ms_);
        if (!connect_result.connected) {
            out.fail = connect_result.timed_out
                ? ConnectFailReason::Timeout
                : ConnectFailReason::Refused;
            out.fail_detail = "connect errno=" + std::to_string(connect_result.error);
            ssl_close(nullptr, sfd);
            return out;
        }
        auto ssl = SslPtr(SSL_new(tls_connect_.get()));
        if (!ssl) { ssl_close(nullptr, sfd); out.fail = ConnectFailReason::TlsRejected; return out; }
        if (!set_expected_peer_pubkey(ssl.get(), expected_pubkey)) {
            ssl_close(nullptr, sfd); out.fail = ConnectFailReason::TlsRejected; return out;
        }
        SSL_set_fd(ssl.get(), (int)sfd);
        {
            int rc = ssl_connect_blocking(ssl.get(), sfd, outbound_connect_timeout_ms_);
            if (rc <= 0) {
                int ssl_err = SSL_get_error(ssl.get(), rc);
                char errbuf[256] = {};
                unsigned long e = ERR_get_error();
                if (e) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                out.fail = classify_ssl_connect_fail(ssl_err);
                out.fail_detail = "ssl_err=" + std::to_string(ssl_err) +
                                  (errbuf[0] ? std::string(" ") + errbuf : "");
                append_ssl_connect_error_detail(out.fail_detail, ssl_err);
                log_event("tls_connect_and_hello_failed", out.fail_detail);
                ssl_close(ssl.get(), sfd);
                return out;
            }
        }
        const std::string certificate_pubkey = peer_public_key_hex(ssl.get());
        if (!expected_pubkey.empty() &&
            !peer_identity_matches(expected_pubkey, certificate_pubkey)) {
            out.fail = ConnectFailReason::TlsRejected;
            out.fail_detail = "peer certificate fingerprint mismatch";
            log_event("tls_peer_identity_mismatch", addr);
            ssl_close(ssl.get(), sfd);
            return out;
        }
        try {
            write_frame(ssl.get(), build_hello(), CONTROL_STREAM_ID);
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                ssl_close(ssl.get(), sfd);
                out.fail = ConnectFailReason::HelloRejected;
                out.fail_detail = "expected HelloMsg";
                return out;
            }
            out.hello = std::get<HelloMsg>(msg);
            if (!certificate_pubkey.empty() &&
                out.hello.pubkey_hex != certificate_pubkey) {
                ssl_close(ssl.get(), sfd);
                out.fail = ConnectFailReason::HelloRejected;
                out.fail_detail = "Hello pubkey does not match TLS certificate";
                return out;
            }
        } catch (const std::exception& e) {
            ssl_close(ssl.get(), sfd);
            out.fail = ConnectFailReason::HelloRejected;
            out.fail_detail = e.what();
            log_event("tls_connect_and_hello_failed", out.fail_detail);
            return out;
        } catch (...) {
            ssl_close(ssl.get(), sfd);
            out.fail = ConnectFailReason::HelloRejected;
            out.fail_detail = "hello exchange failed";
            log_event("tls_connect_and_hello_failed", out.fail_detail);
            return out;
        }
        out.ssl = std::move(ssl);
        out.sfd = sfd;
        return out;
    }

    // ── Rich connect failure diagnostics ───────────────────────
    // Per-reason actionable error messages.
    void print_connect_failure(const std::string& peer_name, const SslConn& sc) const {
        std::cerr << "\nFailed to connect to " << peer_name << ": "
                  << connect_fail_string(sc.fail);
        if (!sc.fail_detail.empty())
            std::cerr << " (" << sc.fail_detail << ")";
        std::cerr << "\n\n";
        switch (sc.fail) {
        case ConnectFailReason::Refused:
            std::cerr << "The peer refused the TCP connection.\n"
                      << "  → Is the bridgesessions daemon running on " << peer_name << "?\n"
                      << "    Try: bs activate  (on the remote host)\n"
                      << "  → Check firewall rules allow port 19949.\n";
            break;
        case ConnectFailReason::Timeout:
            std::cerr << "Connection timed out — no response from " << peer_name << ".\n"
                      << "  → Check network/VPN/Tailscale connectivity.\n"
                      << "    Try: ping <peer-ip>  or  tailscale status\n"
                      << "  → The peer may be offline or overloaded.\n";
            break;
        case ConnectFailReason::TlsRejected:
            std::cerr << "TLS handshake failed — key mismatch or rejection.\n"
                      << "  → The peer's public key doesn't match the pinned key.\n"
                      << "    Re-authorize: bs seed <host:port> pubkey=<new-key>\n"
                      << "  → First connect uses trust-on-first-use (TOFU).\n"
                      << "    Verify the key out-of-band before trusting.\n";
            break;
        case ConnectFailReason::HelloRejected:
            std::cerr << "Hello/auth handshake failed.\n"
                      << "  → Possible version incompatibility or authorization issue.\n"
                      << "    Check: bs ctl logs  (on the remote peer)\n"
                      << "  → Ensure both peers run compatible protocol versions.\n";
            break;
        default:
            std::cerr << "Unknown connection failure.\n"
                      << "  → Check: bs ctl logs\n";
            break;
        }
        std::cerr << "\n";
    }

    // ── Connect with retry for transient failures ──────────────
    // Retries only timeout/refused (transient). Permanent failures
    // (tls_rejected/hello_rejected) fail immediately. NEVER falls back
    // to ssh/winrm/telnet — see NO-FALLBACK CONTRACT at top of file.
    SslConn connect_with_retry(const std::string& addr,
                               const std::string& expected_pubkey,
                               int max_attempts = 3,
                               bool trust_on_first_use = false) {
        SslConn result;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            result = connect_and_hello(addr, expected_pubkey, trust_on_first_use);
            if (result.ssl && result.sfd != INVALID_SOCKET) break;
            const bool retryable = result.fail == ConnectFailReason::Timeout ||
                                   result.fail == ConnectFailReason::Refused;
            if (!retryable || attempt == max_attempts - 1) break;
            // Linear backoff with jitter: 250ms, 500ms, ...
            int base_ms = 250 * (attempt + 1);
            int jitter = std::rand() % (base_ms / 4 + 1);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(base_ms + jitter));
        }
        return result;
    }

    // ── Shutdown ───────────────────────────────────────────────

    void shutdown() { mdns_shutdown(); running_ = false; }

    // ── CLI: shell_peer_detach (--detach) ──────────────────────
    // Returns: 0 on success, 255 on failure (fire-and-forget).
    int shell_peer_detach(const std::string& peer_name, const std::string& session_name,
                          const std::string& cmd, uint16_t cols, uint16_t rows,
                          const std::string& term) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { std::cerr << "Peer not found: " << peer_name << "\n"; return 255; }
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            print_connect_failure(peer_name, sc);
            return 255;
        }
        try {
            AttachMsg am;
            am.session_name = session_name;
            am.cols = cols; am.rows = rows; am.term = term;
            am.command = cmd;
            write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
            // Read AttachAck or error response to confirm session was created
            Message resp = read_frame(sc.ssl.get());
            if (std::holds_alternative<AttachAckMsg>(resp)) {
                auto& ack = std::get<AttachAckMsg>(resp);
                std::cout << "Session " << ack.session_name
                          << " started on " << peer_name
                          << " (attach_id=" << ack.attach_id << ")\n";
                std::cout << "Reattach: bs shell " << peer_name
                          << " -n " << ack.session_name << "\n";
                return 0;
            }
            if (std::holds_alternative<SessionDiedMsg>(resp)) {
                auto& sd = std::get<SessionDiedMsg>(resp);
                std::cerr << "Session died (exit=" << sd.exit_code
                          << " signal=" << sd.signal_num << ")\n";
                return sd.exit_code != 0 ? sd.exit_code : 1;
            }
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cerr << "Session output (detached): "
                          << std::get<OutputMsg>(resp).data << "\n";
                return 255;
            }
        } catch (...) {}
        // If we didn't get AttachAck, the session might still have been created
        std::cout << "Session sent to " << peer_name
                  << ". Use 'bs sessions' to verify.\n";
        return 0;
    }

    // ── CLI: shell_peer ────────────────────────────────────────
    // Returns: 0 on success (interactive), session exit_code on non-interactive,
    //          255 on connection/peer failure.
    int shell_peer(const std::string& peer_name, const std::string& session_name,
                   const std::string& cmd, uint16_t cols, uint16_t rows, const std::string& term,
                   bool signal_forward = true, const std::string& signal_on_detach = "",
                   bool force_interactive = false) {
        if (is_self_target(config_, peer_name)) {
            std::cerr << "Cannot connect to yourself. This is "
                      << self_display_name(config_) << ".\n";
            return 255;
        }
        // 4-tier peer name resolution (exact → suffix → levenshtein)
        auto resolved = resolve_peer(peer_name);
        if (resolved.name.empty()) {
            print_peer_not_found(peer_name);
            return 255;
        }
        // Warn on non-exact match
        if (resolved.tier == PeerResolveResult::Suffix)
            std::cerr << "Resolved \"" << peer_name << "\" → \"" << resolved.name << "\" (suffix match)\n";
        else if (resolved.tier == PeerResolveResult::Levenshtein)
            std::cerr << "Resolved \"" << peer_name << "\" → \"" << resolved.name << "\" (fuzzy match)\n";
        std::string addr = resolved.addr;  // non-const: interactive loop re-resolves on reconnect
        std::string expected_pubkey = resolved.pubkey_hex.empty()
            ? trusted_peer_pubkey(config_, resolved.name) : resolved.pubkey_hex;
        // force_interactive (bs connect selector, `bs shell -i`): run the
        // command on the peer's PTY with the full raw-terminal passthrough —
        // the plain `-x` path strips ANSI and would scramble TUIs.
        const bool non_interactive = !force_interactive &&
            !shell_command_uses_interactive_mode(cmd, stdin_is_terminal());

        // Ephemeral session name for one-shots on the CLI default "default" so
        // concurrent agents do not fight one shared PTY.
        std::string effective_session = session_name;
        if (non_interactive && !cmd.empty()
            && (effective_session.empty() || effective_session == "default")) {
            effective_session = make_ephemeral_cmd_session_name();
        }

        if (non_interactive) {
            auto sc = connect_with_retry(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                print_connect_failure(resolved.name, sc);
                return 255;
            }
            int32_t exit_code = 0;
            bool running = true;
            bool transport_error = false;
            bool saw_session_end = false;
            const int shell_timeout_sec = noninteractive_shell_timeout_sec();
            const auto shell_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(shell_timeout_sec);
            try {
                AttachMsg am;
                am.session_name = effective_session;
                am.cols = cols;
                am.rows = rows;
                am.term = term;
                am.command = cmd;
                am.signal_on_detach = signal_on_detach;
                write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
                while (running) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= shell_deadline) {
                        std::cerr << "Shell timed out after " << shell_timeout_sec
                                  << "s waiting for session end"
                                  << " (session=" << effective_session
                                  << ", peer=" << resolved.name << ")\n"
                                  << "  → One-shots use an ephemeral session; if this persists,"
                                  << " check peer version and PTY reaper health.\n";
                        CLOSESOCK(sc.sfd);
                        sc.sfd = INVALID_SOCKET;
                        return 124;
                    }
                    int remain_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            shell_deadline - now).count());
                    if (remain_ms < 1) remain_ms = 1;
                    if (remain_ms > 5000) remain_ms = 5000;
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    timeval tv{remain_ms / 1000, (remain_ms % 1000) * 1000};
#ifdef _WIN32
                    int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                    int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                    if (ready < 0) {
#ifndef _WIN32
                        if (errno == EINTR) continue;
#endif
                        throw std::runtime_error("select failed while waiting for noninteractive session");
                    }
                    if ((ready > 0 && FD_ISSET((int)sc.sfd, &read_fds)) ||
                        SSL_pending(sc.ssl.get()) > 0) {
                        running = process_noninteractive_response(
                            sc.ssl.get(), exit_code, &transport_error, &saw_session_end);
                    }
                }
                // v2.0.1: after SessionDied, drain late OutputMsg frames briefly.
                // Server may still push conhost-flushed text after death notice.
                if (saw_session_end && !transport_error && sc.ssl && sc.sfd != INVALID_SOCKET) {
                    const auto drain_deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < drain_deadline) {
                        if (SSL_pending(sc.ssl.get()) <= 0) {
                            fd_set rfds; FD_ZERO(&rfds); FD_SET((int)sc.sfd, &rfds);
                            timeval tv{0, 50'000};
#ifdef _WIN32
                            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#else
                            if (select((int)sc.sfd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#endif
                        }
                        try {
                            Message resp = read_frame(sc.ssl.get());
                            if (std::holds_alternative<OutputMsg>(resp)) {
                                std::cout << strip_ansi_escapes(std::get<OutputMsg>(resp).data)
                                          << std::flush;
                            } else if (std::holds_alternative<PingMsg>(resp)) {
                                write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                            }
                        } catch (...) {
                            break;
                        }
                    }
                }
                if (transport_error) {
                    CLOSESOCK(sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    return 255;
                }
                ssl_close(sc.ssl.get(), sc.sfd);
                sc.sfd = INVALID_SOCKET;
                return static_cast<int>(exit_code);
            } catch (...) {
                ssl_close(sc.ssl.get(), sc.sfd);
                sc.sfd = INVALID_SOCKET;
                return 255;
            }
        }

        std::optional<InteractiveTerminalGuard> terminal_guard;
        try {
            terminal_guard.emplace(signal_forward);
        } catch (...) {
            return 255;
        }
#ifndef _WIN32
        struct sigaction shell_sa{};
        struct sigaction shell_old_sa{};
        bool sigint_installed = false;
        if (signal_forward) {
            g_shell_sigint_forward = 0;
            shell_sa.sa_handler = shell_sigint_forward_handler;
            sigemptyset(&shell_sa.sa_mask);
            shell_sa.sa_flags = 0;
            if (sigaction(SIGINT, &shell_sa, &shell_old_sa) == 0)
                sigint_installed = true;
        }
        // SIGHUP/SIGTERM while raw mode is held must not leave the local
        // terminal jammed (raw + mouse tracking → escape garbage on mouse
        // movement, BEL noise). Best-effort restore, then die by the signal.
        struct sigaction cleanup_sa{};
        struct sigaction sighup_old_sa{};
        struct sigaction sigterm_old_sa{};
        bool cleanup_signals_installed = false;
        {
            g_sig_saved_termios = terminal_guard->saved().saved_termios;
            cleanup_sa.sa_handler = shell_signal_cleanup_handler;
            sigemptyset(&cleanup_sa.sa_mask);
            cleanup_sa.sa_flags = 0;
            if (sigaction(SIGHUP, &cleanup_sa, &sighup_old_sa) == 0 &&
                sigaction(SIGTERM, &cleanup_sa, &sigterm_old_sa) == 0) {
                g_sig_have_termios = 1;
                cleanup_signals_installed = true;
            }
        }
#endif
        auto restore_local_terminal = [&]() {
            terminal_guard->restore();
#ifndef _WIN32
            g_sig_have_termios = 0;
            if (cleanup_signals_installed) {
                sigaction(SIGHUP, &sighup_old_sa, nullptr);
                sigaction(SIGTERM, &sigterm_old_sa, nullptr);
                cleanup_signals_installed = false;
            }
            if (sigint_installed) {
                sigaction(SIGINT, &shell_old_sa, nullptr);
                sigint_installed = false;
            }
#endif
        };

        std::string pending_input;
        auto wait_for_local_stop = [&](int wait_ms) {
            std::array<char, 256> input{};
#ifdef _WIN32
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            if (WaitForSingleObject(hIn, static_cast<DWORD>(wait_ms)) != WAIT_OBJECT_0)
                return false;
            DWORD nread = 0;
            if (!ReadFile(hIn, input.data(), static_cast<DWORD>(input.size()), &nread, nullptr))
                return false;
            if (nread == 0) return false;
            return queue_disconnected_input(
                pending_input, std::string_view(input.data(), nread));
#else
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(STDIN_FILENO, &read_fds);
            timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
            int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) return false;
            ssize_t n = ::read(STDIN_FILENO, input.data(), input.size());
            if (n <= 0) return true;
            return queue_disconnected_input(
                pending_input, std::string_view(input.data(), static_cast<size_t>(n)));
#endif
        };

        bool local_stop = false;
        bool announced_reconnect = false;
        int reconnect_delay_ms = 100;
        try {
        while (!local_stop) {
            addr = find_peer_addr(peer_name);
            if (addr.empty()) {
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
                continue;
            }

            auto sc = connect_and_hello(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                // Authentication/certificate rejection is permanent until the
                // operator changes trust configuration. Retrying it silently
                // would look like a hung terminal and weaken failure visibility.
                if (sc.fail == ConnectFailReason::TlsRejected) {
                    restore_local_terminal();
                    print_connect_failure(peer_name, sc);
                    return 255;
                }
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
                continue;
            }

            reconnect_delay_ms = 100;
            bool transport_alive = true;
            auto [last_cols, last_rows] = get_winsize();
            if (last_cols == 0 || last_rows == 0) {
                last_cols = cols;
                last_rows = rows;
            }
            try {
                AttachMsg am;
                am.session_name = session_name;
                am.cols = last_cols;
                am.rows = last_rows;
                am.term = term;
                am.command = cmd;
                am.signal_on_detach = signal_on_detach;
                write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
                if (announced_reconnect) {
                    std::cerr << "[reconnected]\r\n" << std::flush;
                    announced_reconnect = false;
                }
                if (!pending_input.empty()) {
                    KeystrokeMsg queued;
                    queued.data = pending_input;
                    write_frame(sc.ssl.get(), queued, CONTROL_STREAM_ID);
                    pending_input.clear();
                }

                auto forward_local_input = [&](std::string_view input) {
                    // Ctrl-D (0x04) is the local detach key — forward the bytes
                    // before it, then leave. The session stays alive; DetachMsg
                    // is sent on the way out below.
                    if (const size_t d = input.find('\x04');
                        d != std::string_view::npos) {
                        if (d > 0) {
                            KeystrokeMsg pk;
                            pk.data.assign(input.data(), d);
                            try { write_frame(sc.ssl.get(), pk, CONTROL_STREAM_ID); }
                            catch (...) {}
                        }
                        local_stop = true;
                        return;
                    }
                    // Every keystroke, including every Ctrl-C (0x03), goes to
                    // the remote PTY. Nested apps handle SIGINT themselves;
                    // the session stays up until the remote shell exits.
                    // Reconnect-wait still uses local_input_requests_disconnect().
                    if (session_ctrl_c_disconnects() &&
                        local_input_requests_disconnect(input)) {
                        local_stop = true;
                        transport_alive = false;
                        return;
                    }
                    KeystrokeMsg km;
                    km.data.assign(input.data(), input.size());
                    try {
                        write_frame(sc.ssl.get(), km, CONTROL_STREAM_ID);
                    } catch (...) {
                        (void)queue_disconnected_input(pending_input, input);
                        transport_alive = false;
                    }
                };

                std::array<char, 4096> stdin_buf{};
                while (!local_stop && transport_alive) {
#ifdef _WIN32
                    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
                    if (WaitForSingleObject(hIn, 0) == WAIT_OBJECT_0) {
                        DWORD nread = 0;
                        if (ReadFile(hIn, stdin_buf.data(), static_cast<DWORD>(stdin_buf.size()), &nread, nullptr) && nread > 0) {
                            forward_local_input(std::string_view(stdin_buf.data(), nread));
                        }
                    }
                    fd_set sock_fds;
                    FD_ZERO(&sock_fds);
                    FD_SET(sc.sfd, &sock_fds);
                    timeval sock_tv{0, 1000};  // 1ms poll — was 50ms (caused typing lag)
                    if (!local_stop && transport_alive &&
                        (select(0, &sock_fds, nullptr, nullptr, &sock_tv) > 0 ||
                         SSL_pending(sc.ssl.get()) > 0)) {
                        bool session_ended = false;
                        transport_alive = process_shell_response(sc.ssl.get(), &session_ended);
                        if (session_ended) local_stop = true;  // remote exit / Detach
                    }
#else
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET(STDIN_FILENO, &read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    int maxfd = std::max(STDIN_FILENO, (int)sc.sfd);
                    timeval tv{0, 1000};  // 1ms poll — was 50ms (caused typing lag)
                    int ready = select(maxfd + 1, &read_fds, nullptr, nullptr, &tv);
                    if (ready < 0 && errno != EINTR) {
                        transport_alive = false;
                    } else if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
                        ssize_t n = ::read(STDIN_FILENO, stdin_buf.data(), stdin_buf.size());
                        if (n <= 0) {
                            local_stop = true;
                        } else {
                            forward_local_input(std::string_view(
                                stdin_buf.data(), static_cast<size_t>(n)));
                        }
                    }
                    if (g_shell_sigint_forward) {
                        g_shell_sigint_forward = 0;
                        if (!local_stop && transport_alive)
                            forward_local_input(std::string_view("\x03", 1));
                    }
                    if (!local_stop && transport_alive &&
                        ((ready > 0 && FD_ISSET((int)sc.sfd, &read_fds)) || SSL_pending(sc.ssl.get()) > 0)) {
                        bool session_ended = false;
                        transport_alive = process_shell_response(sc.ssl.get(), &session_ended);
                        if (session_ended) local_stop = true;  // remote exit / Detach
                    }
#endif
                    if (!local_stop && transport_alive) {
                        auto [new_cols, new_rows] = get_winsize();
                        if (new_cols > 0 && new_rows > 0 &&
                            (new_cols != last_cols || new_rows != last_rows)) {
                            ResizeMsg resize;
                            resize.cols = new_cols;
                            resize.rows = new_rows;
                            write_frame(sc.ssl.get(), resize, CONTROL_STREAM_ID);
                            last_cols = new_cols;
                            last_rows = new_rows;
                        }
                    }
                }

                if (local_stop && sc.ssl && sc.sfd != INVALID_SOCKET) {
                    try { write_frame(sc.ssl.get(), DetachMsg{}, CONTROL_STREAM_ID); } catch (...) {}
                }
            } catch (...) {
                transport_alive = false;
            }

            if (local_stop) {
                ssl_close(sc.ssl.get(), sc.sfd);
            } else if (sc.sfd != INVALID_SOCKET) {
                // The transport already failed. Avoid SSL_shutdown writing to a
                // dead socket (SIGPIPE on POSIX); reconnect using a fresh TLS link.
                CLOSESOCK(sc.sfd);
            }
            sc.sfd = INVALID_SOCKET;
            if (!local_stop) {
                // Unexpected transport loss (daemon died, network drop). The
                // remote TUI may have left mouse/alt-screen/bracketed-paste
                // modes active in OUR terminal — reset them now so the
                // reconnect wait is not a jammed terminal, and say what is
                // happening (silent retry reads as a hang; users force-kill
                // the client and lose the terminal entirely).
                try { cleanup_terminal_modes(); } catch (...) {}
                std::cerr << "\r\n[transport lost — reconnecting to " << peer_name
                          << "… Ctrl-D to quit]\r\n" << std::flush;
                announced_reconnect = true;
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
            }
        }
        } catch (...) {
            restore_local_terminal();
            return 255;
        }

        restore_local_terminal();
        return 0;
    }

    // Non-interactive response handler: writes OutputMsg data to stdout
    // (ANSI-stripped — ConPTY mode CSI is not useful for --cmd capture),
    // returns false on SessionDiedMsg (capturing exit_code).
    bool process_noninteractive_response(SSL* ssl, int32_t& exit_code,
                                         bool* transport_error = nullptr,
                                         bool* session_ended = nullptr) {
        if (transport_error) *transport_error = false;
        if (session_ended) *session_ended = false;
        try {
            Message resp = read_frame(ssl);
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cout << strip_ansi_escapes(std::get<OutputMsg>(resp).data) << std::flush;
            } else if (std::holds_alternative<ScrollbackMsg>(resp)) {
                // Reattach may push prior scrollback; strip and surface it so
                // agents see context, but do not treat as session end.
                std::cout << strip_ansi_escapes(std::get<ScrollbackMsg>(resp).data) << std::flush;
            } else if (std::holds_alternative<AttachAckMsg>(resp)) {
                // Handshake ack — keep waiting for Output/SessionDied.
            } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                exit_code = std::get<SessionDiedMsg>(resp).exit_code;
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<DetachMsg>(resp)) {
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                exit_code = std::get<ExitCodeMsg>(resp).code;
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<PingMsg>(resp)) {
                write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
            }
        } catch (const std::exception& e) {
            std::cerr << "Shell transport failed: " << e.what() << "\n";
            if (transport_error) *transport_error = true;
            return false;
        } catch (...) {
            std::cerr << "Shell transport failed: unknown transport error\n";
            if (transport_error) *transport_error = true;
            return false;
        }
        return true;
    }

    // Interactive shell response pump.
    // Returns true while the transport should keep reading.
    // Returns false on transport loss or session end.
    // If session_ended is non-null and the remote PTY exited cleanly (SessionDied /
    // ExitCode / server Detach after shell exit), *session_ended is set true so the
    // client leaves the attach loop instead of auto-reconnecting. Typing `exit`
    // in the remote shell produces SessionDied.
    bool process_shell_response(SSL* ssl, bool* session_ended = nullptr) {
        if (session_ended) *session_ended = false;
        try {
            Message resp = read_frame(ssl);
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cout << std::get<OutputMsg>(resp).data << std::flush;
            } else if (std::holds_alternative<ScrollbackMsg>(resp)) {
                std::cout << std::get<ScrollbackMsg>(resp).data << std::flush;
            } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                // Remote shell/PTY exited (e.g. user typed `exit`). Do not
                // resurrect — that would ignore the user's intent to leave.
                auto& sd = std::get<SessionDiedMsg>(resp);
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[session ended"
                          << " exit=" << sd.exit_code;
                if (sd.signal_num != 0)
                    std::cerr << " signal=" << sd.signal_num;
                std::cerr << "] — disconnecting...\r\n";
                return false;
            } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[session ended exit="
                          << std::get<ExitCodeMsg>(resp).code
                          << "] — disconnecting...\r\n";
                return false;
            } else if (std::holds_alternative<DetachMsg>(resp)) {
                // Server-initiated detach without death — treat as leave (not
                // silent reconnect) so the CLI returns to the prompt.
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[detached] — disconnecting...\r\n";
                return false;
            } else if (std::holds_alternative<PingMsg>(resp)) {
                write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
            }
        } catch (...) { return false; }
        return true;
    }

    // ── CLI: list_sessions ────────────────────────────────────
    void list_sessions(const std::string& peer_name, bool all) {
        (void)all;
        if (peer_name.empty()) {
            std::cout << sessions_.summary() << "\n";
            return;
        }
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { std::cerr << "Peer not found: " << peer_name << "\n"; return; }
        auto sc = connect_and_hello(addr, trusted_peer_pubkey(config_, peer_name));
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) { print_connect_failure(peer_name, sc); return; }
        try {
            SessionListMsg req; write_frame(sc.ssl.get(), req, 0);
            fd_set read_fds; FD_ZERO(&read_fds); FD_SET(sc.sfd, &read_fds);
            timeval tv{5, 0};
#ifdef _WIN32
            if (select(0, &read_fds, nullptr, nullptr, &tv) > 0) {
#else
            if (select((int)sc.sfd+1, &read_fds, nullptr, nullptr, &tv) > 0) {
#endif
                Message resp = read_frame(sc.ssl.get());
                if (std::holds_alternative<SessionListMsg>(resp))
                    for (auto& si : std::get<SessionListMsg>(resp).sessions)
                        std::cout << si.name << "  " << si.state << "  uptime=" << si.uptime_seconds << "s\n";
            } else std::cerr << "Timeout\n";
            CLOSESOCK(sc.sfd);
        } catch (...) { if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd); }
    }

    // ── CLI: health_check ─────────────────────────────────────
    bool health_check(const std::string& peer_name, std::string* status_out = nullptr) {
        std::string nonce = "bs-health-" + sha256_hex(
            peer_name + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
            .substr(0, 16);
        std::string output;
        int ec = daemon_shell_via_ipc(peer_name, "health-" + nonce, "echo " + nonce, &output, 15000);
        if (ec >= 0) {
            output = strip_ansi_escapes(output);
            bool ok = (ec == 0 && output.find(nonce) != std::string::npos);
            if (status_out) {
                *status_out = ok ? "healthy (data-plane ok)"
                                 : "unhealthy (data-plane probe mismatch)";
            }
            return ok;
        }
        if (!output.empty() &&
            !should_fallback_to_direct_shell(ec, output)) {
            if (status_out) *status_out = "unhealthy (data-plane probe failed: " + output + ")";
            return false;
        }
        // No safe daemon relay: fall back to a direct data-plane probe rather
        // than a bare Ping/Pong, so CLI-only health does not report control-plane
        // liveness as peer health.
        int prev = outbound_connect_timeout_ms_;
        outbound_connect_timeout_ms_ = kHealthConnectTimeoutMs;
        struct TimeoutRestore { int& ref; int val; ~TimeoutRestore() { ref = val; } } restore{outbound_connect_timeout_ms_, prev};

        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) {
            if (status_out) *status_out = "unknown peer";
            return false;
        }
        auto sc = connect_and_hello(addr, trusted_peer_pubkey(config_, peer_name));
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            if (status_out) {
                if (sc.fail != ConnectFailReason::None)
                    *status_out = connect_fail_string(sc.fail);
                else
                    *status_out = "unreachable";
            }
            return false;
        }
        try {
            AttachMsg am;
            am.session_name = "health-" + nonce;
            am.command = "echo " + nonce;
            am.cols = 80;
            am.rows = 24;
            am.term = "xterm-256color";
            write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
            std::string stdout_buf;
            int32_t exit_code = -1;
            auto drain_late_output = [&](int32_t code) {
                bool transport_eof = false;
                const auto drain_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                while (!health_probe_drain_complete(
                           code, stdout_buf, nonce, transport_eof) &&
                       std::chrono::steady_clock::now() < drain_deadline) {
                    if (SSL_pending(sc.ssl.get()) <= 0) {
                        fd_set drain_fds;
                        FD_ZERO(&drain_fds);
                        FD_SET(sc.sfd, &drain_fds);
                        timeval drain_tv{0, 50'000};
#ifdef _WIN32
                        if (select(0, &drain_fds, nullptr, nullptr, &drain_tv) <= 0) continue;
#else
                        if (select(static_cast<int>(sc.sfd) + 1,
                                   &drain_fds, nullptr, nullptr, &drain_tv) <= 0) continue;
#endif
                    }
                    try {
                        Message late = read_frame(sc.ssl.get());
                        if (std::holds_alternative<OutputMsg>(late)) {
                            stdout_buf += strip_ansi_escapes(
                                std::get<OutputMsg>(late).data);
                        } else if (std::holds_alternative<PingMsg>(late)) {
                            write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                        }
                    } catch (...) {
                        transport_eof = true;
                    }
                }
            };
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline) {
                if (SSL_pending(sc.ssl.get()) <= 0) {
                    fd_set rfds; FD_ZERO(&rfds); FD_SET(sc.sfd, &rfds);
                    timeval tv{2, 0};
#ifdef _WIN32
                    if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#else
                    if (select(static_cast<int>(sc.sfd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#endif
                }
                Message resp = read_frame(sc.ssl.get());
                if (std::holds_alternative<OutputMsg>(resp)) {
                    stdout_buf += strip_ansi_escapes(std::get<OutputMsg>(resp).data);
                } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                    exit_code = std::get<ExitCodeMsg>(resp).code;
                    // Exit can race ahead of the final OutputMsg on Windows.
                    drain_late_output(exit_code);
                    break;
                } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                    exit_code = std::get<SessionDiedMsg>(resp).exit_code;
                    drain_late_output(exit_code);
                    break;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            bool ok = (exit_code == 0 && stdout_buf.find(nonce) != std::string::npos);
            if (status_out) {
                *status_out = ok ? "healthy (data-plane ok)"
                                 : "unhealthy (data-plane probe failed)";
            }
            CLOSESOCK(sc.sfd);
            return ok;
        } catch (const std::exception& e) {
            if (status_out) *status_out = std::string("error: ") + e.what();
            if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd);
            return false;
        } catch (...) {
            if (status_out) *status_out = "error";
            if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd);
            return false;
        }
    }

    // ── CLI: health_latency_report (C3) ────────────────────────
    // Measures TCP connect RTT + data-plane probe RTT to a peer and formats a
    // human-readable latency line. Best-effort: returns empty on any failure so
    // `bs health --latency` degrades gracefully to the plain status.
    std::string health_latency_report(const std::string& peer_name, std::string* out = nullptr) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return {};
        auto sa = resolve_addr(addr);
        auto t0 = std::chrono::steady_clock::now();
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return {};
        int saved = outbound_connect_timeout_ms_;
        outbound_connect_timeout_ms_ = kHealthConnectTimeoutMs;
        auto cr = connect_socket_with_timeout(
            sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
            outbound_connect_timeout_ms_);
        outbound_connect_timeout_ms_ = saved;
        auto t1 = std::chrono::steady_clock::now();
        double connect_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        CLOSESOCK(sfd);
        if (!cr.connected) return {};

        // Data-plane probe RTT: time the full health check (echo round-trip).
        auto p0 = std::chrono::steady_clock::now();
        std::string status;
        bool ok = health_check(peer_name, &status);
        auto p1 = std::chrono::steady_clock::now();
        double probe_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();

        std::string line = "latency: connect=" + std::to_string(static_cast<int>(connect_ms)) +
                           "ms probe=" + std::to_string(static_cast<int>(probe_ms)) +
                           "ms (data-plane " + (ok ? "ok" : "degraded") + ")";
        if (out) *out = line;
        return line;
    }

    // ── CLI: file_send ──────────────────────────────────────────
    // dest_path: optional scp-style remote destination (absolute, ~/…, or
    // relative under peer receive_dir). Empty → classic receive_dir/basename.
    std::string file_send(const std::string& peer_name, const std::string& local_path,
                          bool wait_for_completion,
                          const std::string& dest_path = {}) {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path) || fs::is_directory(local_path)) {
            return "ERROR file not found or is a directory: " + local_path;
        }
        // Try daemon IPC first (reuses existing mesh conns)
        std::string ipc = daemon_send_via_ipc(peer_name, local_path, 120000,
                                              wait_for_completion, dest_path);
        if (!ipc.empty()) {
            // If daemon IPC failed with "no conn", fall back to direct TLS
            if (ipc.rfind("ERROR no conn", 0) == 0 || ipc.rfind("ERROR no daemon", 0) == 0) {
                std::string direct = direct_connect_file_send(
                    peer_name, local_path, wait_for_completion, dest_path);
                if (!direct.empty()) return direct;
            }
            return ipc;
        }
        // No daemon at all — try direct TLS
        std::string direct = direct_connect_file_send(
            peer_name, local_path, wait_for_completion, dest_path);
        if (!direct.empty()) return direct;
        return "ERROR no daemon running and direct TLS file send failed — cannot send";
    }

    // ── CLI: file_recv ──────────────────────────────────────────
    // ── CLI: file_recv ──────────────────────────────────────────
    std::string file_recv(const std::string& peer_name, const std::string& remote_path,
                          const std::string& local_dest, bool wait_for_completion) {
        std::string dest = local_dest.empty() ? "." : local_dest;
        std::string ipc = daemon_recv_via_ipc(peer_name, remote_path, dest, 120000, wait_for_completion);
        if (!ipc.empty()) {
            // If daemon IPC failed with "no conn", fall back to direct TLS
            if (ipc.rfind("ERROR no conn", 0) == 0 || ipc.rfind("ERROR no daemon", 0) == 0) {
                std::string direct = direct_connect_file_recv(peer_name, remote_path, dest);
                if (!direct.empty()) return direct;
            }
            return ipc;
        }
        // No daemon at all — try direct TLS
        std::string direct = direct_connect_file_recv(peer_name, remote_path, dest);
        if (!direct.empty()) return direct;
        return "ERROR no daemon running and direct TLS file recv failed";
    }

    // ── CLI: capture_video ────────────────────────────────────────
    // Use a dedicated direct TLS connection. The old daemon-IPC path only
    // checked for a peer connection and then captured on the local machine.
    std::string capture_video(const std::string& peer_name, const CuaVideoCaptureMsg& request) {
        const std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            const std::string detail = sc.fail_detail.empty()
                ? connect_fail_string(sc.fail)
                : sc.fail_detail;
            return "ERROR failed to connect to " + peer_name + ": " + detail;
        }

        CuaVideoCaptureMsg req = request;
        req.request_id = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds((std::max)(30, static_cast<int>(req.duration_sec) + 30));
        try {
            write_frame(sc.ssl.get(), req, CONTROL_STREAM_ID);
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET((int)sc.sfd, &read_fds);
                timeval tv{1, 0};
#ifdef _WIN32
                const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                const int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    throw std::runtime_error("select failed while waiting for video capture");
                }
                if (ready == 0 && SSL_pending(sc.ssl.get()) <= 0) continue;

                Message msg = read_frame(sc.ssl.get());
                if (std::holds_alternative<CuaVideoCaptureResultMsg>(msg)) {
                    const auto& result = std::get<CuaVideoCaptureResultMsg>(msg);
                    if (result.request_id != req.request_id) continue;
                    ssl_close(sc.ssl.get(), sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    if (result.status != 0) return "ERROR " + result.error;
                    return "video captured at " + result.file_path +
                           " — use 'file recv " + peer_name + " " +
                           result.file_path + " .' to retrieve";
                }
                if (std::holds_alternative<PingMsg>(msg)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            ssl_close(sc.ssl.get(), sc.sfd);
            sc.sfd = INVALID_SOCKET;
            return "ERROR timed out waiting for remote video capture";
        } catch (const std::exception& e) {
            if (sc.sfd != INVALID_SOCKET) ssl_close(sc.ssl.get(), sc.sfd);
            return "ERROR remote video capture transport failed: " + std::string(e.what());
        }
    }

    // ── CLI: send_cua_request ──────────────────────────────────
    // Sends CuaRequestMsg to a peer via direct TLS, returns CuaResponseMsg.
    CuaResponseMsg send_cua_request(const std::string& peer_name, uint8_t action,
                                    int16_t x, int16_t y, uint8_t button,
                                    uint32_t hid_key, uint8_t modifiers,
                                    const std::string& text) {
        CuaResponseMsg err;
        err.status = 1;
        const std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { err.error = "peer not found: " + peer_name; return err; }
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            err.error = "failed to connect to " + peer_name + ": " +
                (sc.fail_detail.empty() ? connect_fail_string(sc.fail) : sc.fail_detail);
            return err;
        }
        CuaRequestMsg req;
        req.request_id = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        req.action = action;
        req.x = x; req.y = y;
        req.button = button;
        req.hid_key = hid_key;
        req.modifiers = modifiers;
        req.text = text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        try {
            write_frame(sc.ssl.get(), req, CONTROL_STREAM_ID);
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET((int)sc.sfd, &read_fds);
                timeval tv{1, 0};
#ifdef _WIN32
                const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                const int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    throw std::runtime_error("select failed while waiting for cua response");
                }
                if (ready == 0 && SSL_pending(sc.ssl.get()) <= 0) continue;
                Message msg = read_frame(sc.ssl.get());
                if (std::holds_alternative<CuaResponseMsg>(msg)) {
                    auto resp = std::get<CuaResponseMsg>(msg);
                    if (resp.request_id != req.request_id) continue;
                    ssl_close(sc.ssl.get(), sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    return resp;
                }
                if (std::holds_alternative<PingMsg>(msg)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            ssl_close(sc.ssl.get(), sc.sfd);
            sc.sfd = INVALID_SOCKET;
            err.error = "timed out waiting for remote cua response";
            return err;
        } catch (const std::exception& e) {
            if (sc.sfd != INVALID_SOCKET) ssl_close(sc.ssl.get(), sc.sfd);
            err.error = std::string("cua transport failed: ") + e.what();
            return err;
        }
    }

    // ── CLI: edit_peer ──────────────────────────────────────────
    void edit_peer(const std::string& target) {
        auto colon = target.find(':');
        if (colon == std::string::npos || colon == 0 || colon == target.size() - 1) {
            std::cerr << "usage: bridgesessions edit <peer>:<path>\n";
            return;
        }
        const std::string peer_name = target.substr(0, colon);
        const std::string remote_path = target.substr(colon + 1);
        const std::string suffix = std::filesystem::path(remote_path).extension().string();
        const std::string local_path = create_private_temp_file("edit", suffix);
        if (local_path.empty()) { std::cerr << "cannot create edit temp file\n"; return; }
        struct TempGuard {
            std::string path;
            ~TempGuard() { std::error_code ec; std::filesystem::remove(path, ec); }
        } cleanup{local_path};

        const std::string received = direct_connect_file_recv(peer_name, remote_path, local_path);
        if (received.rfind("ERROR", 0) == 0) { std::cerr << received << "\n"; return; }
        const std::string original_checksum = sha256_file_stream(local_path);
        if (original_checksum.empty()) { std::cerr << "cannot hash downloaded file\n"; return; }

#ifdef _WIN32
        std::string editor = "notepad";
#else
        std::string editor = "vim";
#endif
        if (const char* env_editor = std::getenv("EDITOR"); env_editor && *env_editor)
            editor = env_editor;
        const int editor_rc = run_editor_process(editor, local_path);
        if (editor_rc != 0) { std::cerr << "editor exited with code " << editor_rc << "\n"; return; }

        const std::string new_checksum = sha256_file_stream(local_path);
        if (new_checksum == original_checksum) { std::cout << "no changes\n"; return; }
        const std::string uploaded = direct_connect_file_send(
            peer_name, local_path, true, remote_path);
        std::cout << uploaded << "\n";
    }

    // ── CLI: run_script ─────────────────────────────────────────
    // Sends a script file to a peer, then executes it via shell_peer.
    // Composes file_send + shell execution — zero new wire protocol.
    int run_script(const std::string& peer_name, const std::string& local_path,
                   const std::string& interpreter = "auto") {
        namespace fs = std::filesystem;

        // 1. Resolve local script file (slurp stdin if "-")
        std::string script_path = local_path;
        std::string temp_local; // cleaned up at end
        if (local_path == "-") {
            // Read all of stdin into a temp file
            std::string content((std::istreambuf_iterator<char>(std::cin)),
                                std::istreambuf_iterator<char>());
            if (content.empty()) {
                std::cerr << "ERROR stdin is empty — nothing to run\n";
                return 1;
            }
            // Detect extension from content shebang or default to .sh
            std::string ext = ".sh";
            if (content.size() >= 2 && content[0] == '#' && content[1] == '!') {
                auto nl = content.find('\n');
                std::string shebang = (nl != std::string::npos)
                    ? content.substr(0, nl) : content;
                if (shebang.find("python") != std::string::npos) ext = ".py";
                else if (shebang.find("powershell") != std::string::npos
                         || shebang.find("pwsh") != std::string::npos) ext = ".ps1";
            }
            temp_local = create_private_temp_file("stdin", ext);
            if (temp_local.empty()) {
                std::cerr << "ERROR cannot create temp file\n"; return 1;
            }
            std::ofstream tf(temp_local, std::ios::binary | std::ios::trunc);
            if (!tf) { std::cerr << "ERROR cannot write temp file\n"; return 1; }
            tf.write(content.data(), static_cast<std::streamsize>(content.size()));
            tf.close();
            script_path = temp_local;
        }

        // 2. Read script content locally — we'll pipe it to the remote via shell
        std::ifstream sf(script_path, std::ios::binary);
        if (!sf) {
            std::cerr << "ERROR cannot read script: " << script_path << "\n";
            if (!temp_local.empty()) fs::remove(temp_local);
            return 1;
        }
        std::string script_content((std::istreambuf_iterator<char>(sf)),
                                   std::istreambuf_iterator<char>());
        sf.close();

        // 3. Resolve interpreter
        std::string interp = interpreter;
        if (interp == "auto" || interp.empty()) {
            interp = detect_interpreter(script_path);
        }

        // 4. Remote temp is a random stem — never the operator filename.
        std::string b64 = base64_encode(script_content);
        for (unsigned char c : b64) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok) {
                std::cerr << "ERROR run-script: invalid base64 encoding\n";
                if (!temp_local.empty()) fs::remove(temp_local);
                return 1;
            }
        }
        std::string exec_cmd;
        std::string session_id = "run-script";
        if (auto tok = generate_ipc_token(); tok.size() >= 16)
            session_id += "-" + tok.substr(0, 16);

        if (interp == "powershell" || interp == "pwsh") {
            std::string exe = (interp == "pwsh") ? "pwsh" : "powershell";
            exec_cmd = exe + " -NoProfile -Command \""
                "$d = Join-Path $env:USERPROFILE '.bridgesessions\\tmp'; "
                "New-Item -ItemType Directory -Force -Path $d | Out-Null; "
                "$p = Join-Path $d ([guid]::NewGuid().ToString('N') + '.ps1'); "
                "[IO.File]::WriteAllBytes($p, [Convert]::FromBase64String('" + b64 + "')); "
                + exe + " -ExecutionPolicy Bypass -NoProfile -File $p; "
                "$ec = $LASTEXITCODE; Remove-Item $p -ErrorAction SilentlyContinue; "
                "exit $ec\"";
        } else if (interp == "cmd" || interp == "bat") {
            exec_cmd = "powershell -NoProfile -Command \""
                "$d = Join-Path $env:USERPROFILE '.bridgesessions\\tmp'; "
                "New-Item -ItemType Directory -Force -Path $d | Out-Null; "
                "$p = Join-Path $d ([guid]::NewGuid().ToString('N') + '.cmd'); "
                "[IO.File]::WriteAllBytes($p, [Convert]::FromBase64String('" + b64 + "')); "
                "cmd /c $p; $ec = $LASTEXITCODE; "
                "Remove-Item $p -ErrorAction SilentlyContinue; exit $ec\"";
        } else {
            std::string runner = (interp == "python" || interp == "python3") ? "python3" : "sh";
            exec_cmd =
                "d=\"${HOME}/.bridgesessions/tmp\"; mkdir -p \"$d\" && chmod 700 \"$d\" && "
                "f=$(mktemp \"$d/bs-rs.XXXXXX\") && "
                "printf '%s\\n' '" + b64 + "' | base64 -d > \"$f\" && "
                + runner + " \"$f\"; rc=$?; rm -f \"$f\"; exit $rc";
        }

        std::cerr << "executing run-script on " << peer_name
                  << ": interpreter=" << interp
                  << " bytes=" << script_content.size() << "\n";
        if (!temp_local.empty()) fs::remove(temp_local);
        auto [cols, rows] = get_winsize();
        return shell_peer(peer_name, session_id, exec_cmd,
                          cols, rows, "xterm-256color");
    }

    // Base64 encode for run-script (no external deps)
    static std::string base64_encode(const std::string& in) {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i+1] << 8 | (uint8_t)in[i+2];
            out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
        }
        if (i < in.size()) {
            uint32_t n = (uint8_t)in[i] << 16;
            if (i + 1 < in.size()) n |= (uint8_t)in[i+1] << 8;
            out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
            out += (i + 1 < in.size()) ? tbl[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    // P3: detect_interpreter returns "sh" for .sh to match the runner which uses "sh"
    static std::string detect_interpreter(const std::string& path) {
        namespace fs = std::filesystem;
        std::string ext = fs::path(path).extension().string();
        // lowercase
        for (auto& c : ext) { if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); }
        if (ext == ".sh") return "sh";
        if (ext == ".ps1") return "powershell";
        if (ext == ".py") return "python";
        if (ext == ".bat" || ext == ".cmd") return "cmd";
        // No extension or unknown: assume sh
        return "sh";
    }

    // ── Content-addressed script cache (bs script) ───────────────────
    // Scripts stored in ~/.bridgesessions/scripts/<sha256>.sh, named via symlink.

    static bool script_hash_valid(std::string_view value) {
        if (value.size() != 64) return false;
        for (const unsigned char c : value) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    static bool script_alias_valid(std::string_view value) {
        if (value.empty() || value.size() > 64 || value == "." || value == "..") return false;
        for (const unsigned char c : value) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
        }
        return true;
    }

    static std::string posix_shell_quote(std::string_view value) {
        std::string out{"'"};
        for (const char c : value) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
        return out;
    }

    [[nodiscard]] std::string script_cache_dir() const {
        return home_dir_ + "/scripts";
    }

    [[nodiscard]] std::string script_cache_path(const std::string& sha256) const {
        if (!script_hash_valid(sha256)) return {};
        return script_cache_dir() + "/" + sha256 + ".sh";
    }

    [[nodiscard]] std::string script_resolve(const std::string& name_or_hash) const {
        namespace fs = std::filesystem;
        const std::string dir = script_cache_dir();
        std::error_code ec;
        if (script_hash_valid(name_or_hash)) {
            const std::string hash_path = script_cache_path(name_or_hash);
            return fs::is_regular_file(hash_path, ec) && path_is_inside_directory(hash_path, dir)
                ? hash_path : std::string{};
        }
        if (!script_alias_valid(name_or_hash)) return {};
        const fs::path alias_path = fs::path(dir) / name_or_hash;
        if (!path_is_inside_directory(alias_path, dir)) return {};
        if (fs::is_symlink(alias_path, ec)) {
            const fs::path resolved = fs::weakly_canonical(alias_path, ec);
            if (ec || !path_is_inside_directory(resolved, dir)) return {};
            const std::string filename = resolved.filename().string();
            if (filename.size() != 67 || filename.substr(64) != ".sh" ||
                !script_hash_valid(filename.substr(0, 64))) return {};
            return resolved.string();
        }
        if (fs::is_regular_file(alias_path, ec)) {
            const std::string hash = sha256_file_stream(alias_path.string());
            const std::string canonical = script_cache_path(hash);
            if (!canonical.empty() && fs::is_regular_file(canonical, ec)) return canonical;
        }
        return {};
    }

    // bs script add <file> [--name alias]
    // Hashes content, stores as <sha256>.sh, optionally creates named symlink.
    // Returns "OK <sha256> <path>" or "ERROR ...".
    std::string script_add(const std::string& file_path, const std::string& alias) {
        namespace fs = std::filesystem;
        if (!alias.empty() && !script_alias_valid(alias))
            return "ERROR invalid script alias (use 1-64 letters, digits, ., _, -)";
        std::ifstream f(file_path, std::ios::binary);
        if (!f) return "ERROR cannot read file: " + file_path;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        f.close();
        const std::string hash = sha256_hex(content);
        const std::string dir = script_cache_dir();
        if (!ensure_private_directory(dir)) return "ERROR cannot create private script cache";
        std::error_code ec;
        const std::string dest = script_cache_path(hash);
        if (!fs::exists(dest)) {
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out) return "ERROR cannot write to cache: " + dest;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.close();
            if (!out || !restrict_private_file_permissions(dest))
                return "ERROR cannot secure script cache file";
        }
        std::string result = "OK " + hash + " " + dest;
        if (!alias.empty()) {
            std::string link = dir + "/" + alias;
            std::error_code lec;
            fs::remove(link, lec);
            std::string target = hash + ".sh";
            fs::create_symlink(target, link, lec);
            if (lec) {
                std::ofstream cp(link, std::ios::binary | std::ios::trunc);
                if (!cp) return "ERROR cannot create script alias";
                cp.write(content.data(), static_cast<std::streamsize>(content.size()));
                cp.close();
                if (!cp || !restrict_private_file_permissions(link))
                    return "ERROR cannot secure script alias";
            }
            result += " alias=" + alias;
        }
        return result;
    }

    // bs script list — prints all cached scripts
    void script_list() const {
        namespace fs = std::filesystem;
        std::string dir = script_cache_dir();
        if (!fs::exists(dir)) { std::cout << "No scripts cached.\n"; return; }
        std::cout << "=== Cached scripts (" << dir << ") ===\n";
        // Collect symlinks (names) and hash files
        std::vector<std::string> hashes, names;
        for (auto& e : fs::directory_iterator(dir)) {
            auto name = e.path().filename().string();
            if (name.size() == 67 && name.substr(64) == ".sh") {
                hashes.push_back(name.substr(0, 64));
            } else if (e.is_symlink() || e.is_regular_file()) {
                // Skip .sh hash files (already handled)
                if (!(name.size() == 67 && name.substr(64) == ".sh"))
                    names.push_back(name);
            }
        }
        std::sort(hashes.begin(), hashes.end());
        for (auto& h : hashes) {
            std::cout << "  " << h.substr(0, 16) << "...";
            // Find aliases pointing to this hash
            std::string target_hash = h + ".sh";
            std::vector<std::string> aliases;
            for (auto& e : fs::directory_iterator(dir)) {
                if (e.is_symlink()) {
                    auto tgt = fs::read_symlink(e.path());
                    if (tgt.filename().string() == target_hash)
                        aliases.push_back(e.path().filename().string());
                }
            }
            if (!aliases.empty()) {
                std::cout << "  [";
                for (size_t i = 0; i < aliases.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << aliases[i];
                }
                std::cout << "]";
            }
            std::cout << "\n";
        }
        if (hashes.empty()) std::cout << "  (none)\n";
    }

    // bs script remove <name> — remove symlink or hash file
    std::string script_remove(const std::string& name) {
        namespace fs = std::filesystem;
        if (!script_hash_valid(name) && !script_alias_valid(name)) return "ERROR invalid script name";
        std::string dir = script_cache_dir();
        std::string link = dir + "/" + name;
        std::error_code ec;
        if (fs::exists(link) || fs::is_symlink(link)) {
            fs::remove(link, ec);
            if (ec) return "ERROR failed to remove: " + link;
            return "OK removed " + name;
        }
        // Try as hash
        std::string hash_path = script_cache_path(name);
        if (fs::exists(hash_path)) {
            fs::remove(hash_path, ec);
            if (ec) return "ERROR failed to remove: " + hash_path;
            // Also remove any symlinks pointing to it
            for (auto& e : fs::directory_iterator(dir)) {
                if (e.is_symlink()) {
                    auto tgt = fs::read_symlink(e.path());
                    if (tgt.filename().string() == name + ".sh")
                        fs::remove(e.path(), ec);
                }
            }
            return "OK removed " + name;
        }
        return "ERROR script not found: " + name;
    }

    // bs script push <name> --peer <peer>
    // Sends script to peer's ~/.bridgesessions/scripts/ cache if not already present.
    // Composes file_send + shell_peer — no new wire protocol.
    std::string script_push(const std::string& name, const std::string& peer) {
        std::string path = script_resolve(name);
        if (path.empty()) return "ERROR script not found: " + name;
        namespace fs = std::filesystem;
        // Extract hash from filename (<hash>.sh)
        const std::string fname = fs::path(path).filename().string();
        if (fname.size() != 67 || fname.substr(64) != ".sh" ||
            !script_hash_valid(fname.substr(0, 64)))
            return "ERROR unexpected cache filename: " + fname;
        const std::string hash = fname.substr(0, 64);

        // Check if peer already has it
        std::string check_cmd = "test -f ~/.bridgesessions/scripts/" + hash + ".sh && echo PRESENT || echo ABSENT";
        std::string check_output;
        // Use daemon_shell_via_ipc for quick check (non-interactive)
        int ec = daemon_shell_via_ipc(peer, "script-check-" + hash.substr(0, 8),
                                      check_cmd, &check_output);
        if (ec >= 0 && check_output.find("PRESENT") != std::string::npos) {
            return "OK " + hash + " already on " + peer;
        }
        // Also try direct shell for peer check if IPC failed
        if (ec < 0) {
            // Use shell_peer in non-interactive mode to check
            // (if daemon not available, shell_peer does direct TLS)
            std::string cmd_out;
            // Can't easily capture from shell_peer here — just push if check failed
        }

        // Ensure peer has the scripts directory
        std::string mkdir_cmd = "mkdir -p ~/.bridgesessions/scripts";
        daemon_shell_via_ipc(peer, "script-mkdir", mkdir_cmd, nullptr);

        // Send the script file via existing file_send
        std::string result = file_send(peer, path, true);
        if (result.rfind("ERROR", 0) == 0) return result;

        // Move received file to scripts cache on peer
        // file_send delivers to peer's received/ dir — move it
        std::string received_name = fname;
        std::string move_cmd = "mv ~/.bridgesessions/received/" + received_name
                             + " ~/.bridgesessions/scripts/" + hash + ".sh 2>/dev/null"
                             + " || true";
        daemon_shell_via_ipc(peer, "script-move-" + hash.substr(0, 8), move_cmd, nullptr);

        return "OK pushed " + hash + " to " + peer;
    }

    // bs script run <name> --peer <peer> [-- args...]
    // Ensures script is on peer (push if needed), then executes it.
    int script_run(const std::string& name, const std::string& peer,
                   const std::vector<std::string>& args) {
        std::string path = script_resolve(name);
        if (path.empty()) {
            std::cerr << "ERROR script not found: " + name + "\n";
            return 1;
        }
        namespace fs = std::filesystem;
        const std::string fname = fs::path(path).filename().string();
        if (fname.size() != 67 || fname.substr(64) != ".sh" ||
            !script_hash_valid(fname.substr(0, 64))) {
            std::cerr << "ERROR unexpected cache filename: " + fname + "\n";
            return 1;
        }
        const std::string hash = fname.substr(0, 64);

        // Check if peer has it; push if not
        std::string check_cmd = "test -f ~/.bridgesessions/scripts/" + hash + ".sh && echo PRESENT || echo ABSENT";
        std::string check_output;
        int ec = daemon_shell_via_ipc(peer, "script-runcheck-" + hash.substr(0, 8),
                                      check_cmd, &check_output);
        bool need_push = (ec < 0) || (check_output.find("PRESENT") == std::string::npos);
        if (need_push) {
            std::cerr << "Script not on peer, pushing...\n";
            std::string push_result = script_push(name, peer);
            std::cerr << push_result << "\n";
            if (push_result.rfind("ERROR", 0) == 0) return 1;
        }

        // Quote each argv element independently; raw joins become shell code.
        std::string exec_cmd =
            "bash \"$HOME/.bridgesessions/scripts/" + hash + ".sh\"";
        for (const auto& arg : args) exec_cmd += " " + posix_shell_quote(arg);
        auto [cols, rows] = get_winsize();
        return shell_peer(peer, "script-" + hash.substr(0, 8), exec_cmd,
                          cols, rows, "xterm-256color");
    }

    // ── CLI: show_stats ───────────────────────────────────────
    void show_stats() const {
        // Same table layout as daemon STATS IPC (local / no-daemon fallback).
        std::cout << daemon_stats_summary();
        if (!our_pubkey_.empty())
            std::cout << "pubkey   " << our_pubkey_.substr(0, 16) << "...\n";
    }

    // ── CLI: show_peers_detail — live connection status ──────────
    void show_peers_detail(const std::string& peer_name = "") {
        auto now = std::chrono::steady_clock::now();
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (!peer_name.empty() && c.peer_name != peer_name) continue;
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last_pong).count();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - c.connected_at).count();
            std::cout << c.peer_name << " " << c.peer_addr << " "
                      << (c.is_outbound ? "outbound" : "inbound")  << " "
                      << "latency=" << latency << "ms "
                      << "uptime=" << uptime << "s" << std::endl;
        }
    }

    std::string sync_vfolder(const std::string& name) {
        return vfolder_sync_direct(name);
    }

    // ── Accessors (for tests) ──────────────────────────────────

    SessionRegistry& sessions() { return sessions_; }

#ifndef _WIN32
    // Daemon-only opt-in: host sessions in detached worker processes so they
    // survive daemon restart/upgrade. Tests and one-shot controllers keep the
    // direct forkpty path. Call before run().
    void enable_session_hosting() {
        sessions_.set_app_home(home_dir_);
#ifndef _WIN32
        // r3 fix (P3b): warm the worker exe once at daemon start. On macOS every
        // exec of an ad-hoc-signed binary pays an AMFI/page-in cost that can exceed
        // the old 3s worker-socket wait (all mac-peer spawns failed on
        // 2026-08-31 right after the r2 swap — a cold binary). Paying it once
        // here makes the first real spawn fast; on Linux this is a cheap
        // no-op --version run.
        const std::string& wexe = sessions_.worker_exe_for_warm();
        if (!wexe.empty()) {
            std::string warm = posix_shell_quote(wexe) + " --version >/dev/null 2>&1";
            std::system(warm.c_str());
        }
#endif
    }
#endif
    const std::vector<Conn>& conns() const { return conns_; }
    size_t conn_count() const { return conns_.size(); }

    // ── Public connect (for tests/CLI) ──────────────────────────
    bool connect_to_peer(const std::string& addr) {
        return connect_to_peer_impl(addr);
    }

#ifdef BS_TESTING
    // ── Test helpers ─────────────────────────────────────────────

    // True if the given conn has not received a pong within pong_timeout_secs.
    bool is_pong_timed_out(const Conn& c) const {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(config_.pong_timeout_secs);
        return (now - c.last_pong > timeout);
    }

    // Inject a GossipMsg directly into the discovered peers list.
    void inject_gossip(const GossipMsg& g) { merge_peers(g.peers); }

    // Return a snapshot of all discovered (non-seed) peers.
    std::vector<PeerEntry> discovered_peers() const { return config_.discovered; }

    // This node's own ed25519 public key hex.
    std::string own_pubkey_hex() const { return our_pubkey_; }

    // Duplicate-connection resolution: true if this node should keep its
    // outbound connection when both sides raced to connect each other.
    // Rule: keep outbound when our pubkey is lexicographically less than peer's.
    static bool should_keep_outbound(const std::string& own_hex, const std::string& peer_hex) {
        return own_hex < peer_hex;
    }

    // Compute the next reconnect delay in milliseconds for the given attempt number.
    // Doubles from 100ms, capped at reconnect_backoff_max_secs * 1000.
    long next_backoff_ms(int attempt) const {
        long ms = 100;
        long cap = static_cast<long>(config_.reconnect_backoff_max_secs) * 1000;
        for (int i = 0; i < attempt; ++i) {
            ms = std::min(ms * 2, cap);
        }
        return ms;
    }

    // Trust-filter tests.
    bool is_trusted_pubkey_for_test(const std::string& pk) {
        return is_trusted_pubkey(pk);
    }
    std::string configured_peer_addr_for_test(const std::string& name) const {
        for (const auto& s : config_.seeds)
            if (peer_name_eq(s.name, name)) return s.addr;
        for (const auto& d : config_.discovered)
            if (peer_name_eq(d.name, name)) return d.addr;
        return {};
    }
    void process_mdns_announcement_for_test(const std::string& name,
                                            const std::string& addr,
                                            const std::string& pubkey) {
        process_mdns_announcement(name, addr, pubkey);
    }

    // Hello duplicate-policy tests.
    void test_set_initial_hello_for_test(Conn& c, const HelloMsg& h) {
        c.initial_hello = h;
    }
    bool test_handle_hello_for_test(Conn& c, const HelloMsg& h) {
        if (!c.initial_hello.has_value()) {
            c.initial_hello = h;
            c.peer_name = h.node_name;
            merge_peers(h.known_peers);
            return true;
        }
        if (*c.initial_hello == h) return true;
        c.close_requested = true;
        return false;
    }

    // IPC token tests.
    void set_ipc_token_for_test(const std::string& token) { ipc_token_ = token; }
    std::string ipc_token_for_test() const { return ipc_token_; }
    std::string ipc_token_path_for_test() const { return ipc_token_path_; }
    bool ipc_request_is_authorized_for_test(const std::string& line) const {
        return const_time_token_match(line, ipc_token_);
    }
    bool another_daemon_running_for_test() { return another_daemon_running(); }
    void inject_file_meta_for_test(Conn& c, const FileMetaMsg& m) { handle_file_meta(c, m); }
    void inject_file_chunk_for_test(Conn& c, const FileChunkMsg& m) { handle_file_chunk(c, m); }
    const std::string& pending_recv_dir_for_test(const Conn& c) const { return c.pending_recv_dir; }
    bool begin_async_receive_for_test(Conn& c, const std::string& dir) {
        return begin_async_receive(c, dir);
    }
    const FileReceiveState& file_receive_for_test(const Conn& c) const { return c.file_receive; }
    bool write_pty_input_for_test(Session& s, const void* data, size_t len) {
        return write_pty_input(s, data, len);
    }
#ifndef _WIN32
    bool drain_pending_pty_input_for_test(Session& s) {
        return drain_pending_pty_input(s);
    }
    const std::string& pending_input_for_test(const Session& s) const {
        return s.pending_input;
    }
#endif
#endif
