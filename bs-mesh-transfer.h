// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-transfer.h — MeshController file transfer, dispatch, enroll, gossip, cleanup
// Extracted from bs-mesh-controller.h (R6b structural refactor, 2026-09-03)
// Designed for inclusion inside `class MeshController { ... }`
// Does NOT open its own namespace or class — parent file provides it.
#pragma once

    // ── P1: File transfer handlers ──────────────────────────────

    // File receive handlers run on the event loop. Their replies must use the
    // bounded non-blocking TX queue; a stalled peer must never freeze every
    // other peer for write_frame()'s 30-second deadline.
    bool enqueue_file_ack(Conn& c, FileAckMsg ack) {
#ifdef BS_TESTING
        // Unit tests inject transfer messages without constructing a TLS socket.
        if (c.sock_fd == INVALID_SOCKET && !c.ssl) return true;
#endif
        if (enqueue_frame(c, ack, CONTROL_STREAM_ID)) return true;
        log_event("file_ack_queue_failed", c.peer_name);
        close_conn(c);  // reconnect/resume is safer than silently losing an ACK
        return false;
    }

    void handle_file_meta(Conn& c, const FileMetaMsg& m) {
        // Prepare receive path — never trust raw remote filenames (P0-3).
        namespace fs = std::filesystem;

        // v2.0.6: the destination directory was bound to this Conn by the async
        // FILE_RECV request. Consume it now (one outstanding receive per Conn).
        // Fall back to the global default only when no per-request dir is set.
        std::string recv_dir = std::move(c.pending_recv_dir);
        if (recv_dir.empty()) recv_dir = receive_dir_;

        auto safe_name = sanitize_transfer_filename(m.filename);
        if (!safe_name) {
            std::string err = "rejected unsafe filename";
            log_event("file_recv_rejected", m.filename + " reason=unsafe_filename");
            (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
            return;
        }
        const size_t chunk_raw = effective_transfer_chunk_size(m.chunk_size);
        const auto metadata = validate_transfer_metadata(
            m.filesize, m.total_chunks, config_.transfer_max_bytes, chunk_raw);
        if (!metadata.ok) {
            const std::string& err = metadata.reason;
            log_event("file_recv_rejected",
                      *safe_name + " size=" + std::to_string(m.filesize) +
                      " chunks=" + std::to_string(m.total_chunks) +
                      " chunk_size=" + std::to_string(chunk_raw) +
                      " reason=" + err);
            (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
            return;
        }
        std::error_code ec;
        fs::create_directories(recv_dir, ec);
        if (ec) {
            std::string err = "cannot create receive directory";
            log_event("file_recv_failed", recv_dir + " reason=" + ec.message());
            (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
            return;
        }

        // Starting a new transfer on the same connection aborts its old partial
        // unless the new FileMeta resumes the same checksum (Wi‑Fi reconnect).
        auto& state = c.file_receive;
        if (state.active) {
            state.file.close();
            if (state.checksum != m.checksum) {
                fs::remove(state.path + ".part", ec);
            }
            state.active = false;
        }

        // scp-style dest: optional absolute/relative path from FileMeta.dest_path.
        // Empty → classic receive_dir/basename behavior.
        std::string out_path;
        if (!m.dest_path.empty()) {
            auto resolved = resolve_file_send_dest(
                m.dest_path, recv_dir, config_.dest_allow_home);
            if (!resolved) {
                std::string err = "rejected dest path (escape or invalid)";
                log_event("file_recv_rejected", m.dest_path + " reason=dest_escape");
                (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
                return;
            }
            out_path = *resolved;
            // If dest is a directory (or ends with separator), append basename.
            if (!out_path.empty() &&
                (out_path.back() == '/' || out_path.back() == '\\' ||
                 (fs::exists(out_path) && fs::is_directory(out_path)))) {
                out_path = (fs::path(out_path) / *safe_name).string();
            }
            std::error_code pec;
            auto parent = fs::path(out_path).parent_path();
            if (!parent.empty()) fs::create_directories(parent, pec);
            if (pec) {
                std::string err = "cannot create dest parent directory";
                log_event("file_recv_failed", out_path + " reason=" + pec.message());
                (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
                return;
            }
            if (is_sensitive_mesh_path(out_path) && !config_.allow_sensitive_paths) {
                std::string err = "refused sensitive dest path";
                log_event("file_recv_rejected", "reason=sensitive_dest");
                (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
                return;
            }
        } else {
            out_path = (fs::path(recv_dir) / *safe_name).string();
            if (!path_is_inside_directory(out_path, recv_dir)) {
                std::string err = "path escapes receive directory";
                log_event("file_recv_rejected", *safe_name + " reason=path_escape");
                (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
                return;
            }
        }
        // Resume candidate: existing .part for this basename with matching sidecar
        // checksum. Prefer that path over inventing suffix.N (avoids restart-from-0
        // after a Wi‑Fi drop mid-transfer).
        std::string part_path = out_path + ".part";
        std::string meta_path = out_path + ".part.bsmeta";
        bool resume = false;
        uint32_t resume_chunks = 0;
        uint64_t resume_bytes = 0;
        if (fs::exists(part_path) && fs::exists(meta_path)) {
            std::ifstream mf(meta_path);
            std::string line;
            std::string meta_sum;
            uint64_t meta_size = 0;
            uint32_t meta_total = 0;
            size_t meta_chunk = 0;
            while (std::getline(mf, line)) {
                if (line.rfind("checksum=", 0) == 0) meta_sum = line.substr(9);
                else if (line.rfind("size=", 0) == 0) meta_size = std::strtoull(line.c_str() + 5, nullptr, 10);
                else if (line.rfind("total_chunks=", 0) == 0) meta_total = static_cast<uint32_t>(std::strtoul(line.c_str() + 13, nullptr, 10));
                else if (line.rfind("chunk_raw=", 0) == 0) meta_chunk = static_cast<size_t>(std::strtoul(line.c_str() + 10, nullptr, 10));
            }
            std::error_code pec;
            auto psz = fs::file_size(part_path, pec);
            if (!pec && meta_sum == m.checksum && meta_size == m.filesize &&
                meta_total == m.total_chunks && meta_chunk == chunk_raw &&
                psz > 0 && psz < m.filesize && (psz % chunk_raw == 0 || psz == m.filesize)) {
                resume = true;
                resume_bytes = static_cast<uint64_t>(psz);
                resume_chunks = static_cast<uint32_t>(resume_bytes / chunk_raw);
                // last partial chunk: if not aligned, truncate to whole chunks
                if (resume_bytes % chunk_raw != 0) {
                    resume_bytes = static_cast<uint64_t>(resume_chunks) * chunk_raw;
                    fs::resize_file(part_path, resume_bytes, pec);
                }
            }
        }
        if (!resume) {
            // Only auto-suffix collisions under default receive_dir (inbox) mode.
            // scp-style dest overwrites/resumes the exact path requested.
            if (m.dest_path.empty()) {
                int suffix = 1;
                while (fs::exists(out_path) || fs::exists(out_path + ".part")) {
                    std::string alt = (fs::path(recv_dir) /
                                       (*safe_name + "." + std::to_string(suffix))).string();
                    if (!path_is_inside_directory(alt, recv_dir)) break;
                    out_path = alt;
                    ++suffix;
                }
            }
            part_path = out_path + ".part";
            meta_path = out_path + ".part.bsmeta";
        }

        state = FileReceiveState{};
        state.filename = *safe_name;
        state.path = out_path;
        state.checksum = m.checksum;
        state.expected_size = m.filesize;
        state.total_chunks = m.total_chunks;
        state.chunk_raw_size = chunk_raw;
        if (resume) {
            state.received_bytes = resume_bytes;
            state.received_chunks = resume_chunks;
            state.file.open(part_path, std::ios::binary | std::ios::app);
            log_event("file_recv_resume", *safe_name + " from_chunk=" + std::to_string(resume_chunks) +
                      " bytes=" + std::to_string(resume_bytes));
        } else {
            state.file.open(part_path, std::ios::binary | std::ios::trunc);
            state.hasher = std::make_unique<Sha256Stream>();
            // Sidecar for cross-connection resume after Wi‑Fi drops
            {
                std::ofstream mf(meta_path, std::ios::trunc);
                if (mf) {
                    mf << "checksum=" << m.checksum << "\n"
                       << "size=" << m.filesize << "\n"
                       << "total_chunks=" << m.total_chunks << "\n"
                       << "chunk_raw=" << chunk_raw << "\n";
                }
            }
            log_event("file_recv_start", *safe_name + " -> " + out_path);
        }
        state.active = state.file.is_open();
        if (!state.active || (!resume && (!state.hasher || !state.hasher->ok()))) {
            const std::string err = "cannot initialize receive file";
            log_event("file_recv_failed", *safe_name + " reason=init_failed");
            state.file.close();
            fs::remove(part_path, ec);
            state.active = false;
            (void)enqueue_file_ack(c, FileAckMsg{0, 0, true, err});
            return;
        }
        // next_requested = first chunk we still need (0 for fresh, N for resume).
        // Echo only the basename; returning an absolute local path leaks the
        // receiver's home/username to the sending peer.
        (void)enqueue_file_ack(
            c, FileAckMsg{resume ? (resume_chunks > 0 ? resume_chunks - 1 : 0) : 0,
                          resume_chunks, false,
                          "path=" + fs::path(out_path).filename().string()});
    }

    void handle_file_chunk(Conn& c, const FileChunkMsg& m) {
        auto& state = c.file_receive;
        if (!state.active) {
            log_event("file_chunk_orphan", "no active receive for chunk " + std::to_string(m.chunk_index));
            (void)enqueue_file_ack(
                c, FileAckMsg{m.chunk_index, 0, true, "no active receive"});
            return;
        }
        // Decompress chunk data (zstd magic sniff — v2.0.16: handles both
        // pre-2.0.14 double-compressed and v2.0.14+ raw senders)
        std::vector<uint8_t> decompressed;
        if (!m.data.empty()) {
            decompressed = decompress_chunk_payload(
                std::span<const uint8_t>(m.data.data(), m.data.size()));
        }
        const auto chunk_valid = validate_transfer_chunk(
            state.expected_size, state.received_bytes, state.received_chunks,
            state.total_chunks, m.chunk_index, m.total_chunks, decompressed.size(),
            state.chunk_raw_size);
        if (!chunk_valid.ok) {
            const std::string err = chunk_valid.reason;
            log_event("file_chunk_rejected", state.filename + " reason=" + err);
            state.file.close();
            std::error_code ec;
            std::filesystem::remove(state.path + ".part", ec);
            state.active = false;
            (void)enqueue_file_ack(
                c, FileAckMsg{m.chunk_index, state.received_chunks, true, err});
            return;
        }
        if (!decompressed.empty()) {
            state.file.write(reinterpret_cast<const char*>(decompressed.data()),
                             static_cast<std::streamsize>(decompressed.size()));
            if (!state.file) {
                const std::string err = "failed to write receive file";
                log_event("file_recv_failed", state.filename + " reason=write_error");
                state.file.close();
                std::error_code ec;
                std::filesystem::remove(state.path + ".part", ec);
                state.active = false;
                (void)enqueue_file_ack(
                    c, FileAckMsg{m.chunk_index, state.received_chunks, true, err});
                return;
            }
            if (state.hasher && !state.hasher->update(decompressed)) {
                const std::string err = "sha256 update failed";
                log_event("file_recv_failed", state.filename + " reason=hash_update");
                state.file.close();
                std::error_code ec;
                std::filesystem::remove(state.path + ".part", ec);
                state.active = false;
                (void)enqueue_file_ack(
                    c, FileAckMsg{m.chunk_index, state.received_chunks, true, err});
                return;
            }
        }
        state.received_chunks = m.chunk_index + 1;
        state.received_bytes += decompressed.size();
        if (state.received_chunks >= state.total_chunks) {
            state.file.close();
            namespace fs = std::filesystem;
            const std::string final_path = state.path;
            const std::string part_path = final_path + ".part";
            // Fresh transfers have already hashed each accepted chunk. Only a
            // resumed transfer needs the full-file fallback pass here.
            const std::string actual = state.hasher
                ? state.hasher->final_hex()
                : sha256_file_stream(part_path);
            const bool checksum_ok = !actual.empty() && actual == state.checksum;
            std::string final_error;
            if (!checksum_ok) {
                final_error = "checksum mismatch";
                std::error_code ec;
                fs::remove(part_path, ec);
            } else {
                std::error_code ec;
                fs::rename(part_path, final_path, ec);
                if (ec) {
                    final_error = "cannot publish received file";
                    log_event("file_recv_rename_failed",
                              fs::path(part_path).filename().string() + " reason=" + ec.message());
                    fs::remove(part_path, ec);
                }
            }
            const bool complete_ok = final_error.empty();
            log_event("file_recv_complete", state.filename
                       + " " + std::to_string(state.received_chunks) + " chunks"
                       + (complete_ok ? " checksum_ok" : " ERROR=" + final_error));
            // Drop resume sidecar either way (mismatch already removed .part)
            {
                std::error_code mec;
                fs::remove(final_path + ".part.bsmeta", mec);
            }
            state.active = false;
            state.hasher.reset();
            const std::string final_msg = complete_ok
                ? ("path=" + fs::path(final_path).filename().string())
                : final_error;
            (void)enqueue_file_ack(
                c, FileAckMsg{m.chunk_index, m.total_chunks, !complete_ok, final_msg});
        } else {
            (void)enqueue_file_ack(
                c, FileAckMsg{m.chunk_index, m.chunk_index + 1, false, ""});
        }
    }

    // ── Direct TLS file send (no daemon required) ──────────────
    // Opens a direct TLS connection via connect_and_hello, then sends the file
    // using file_send_wait_on_transport (same FileMetaMsg/FileChunkMsg/FileAckMsg
    // protocol as daemon_file_send). Falls back when daemon IPC is unavailable.
    std::string direct_connect_file_send(const std::string& peer_name,
                                         const std::string& local_path,
                                         bool wait_for_completion = true,
                                         const std::string& dest_path = {}) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);

        // Wi‑Fi-resilient: on transport errors, reconnect and resume from the
        // last FileAck.next_requested (receiver keeps .part + .bsmeta sidecar).
        uint32_t start_chunk = 0;
        std::string last_err = "ERROR transfer failed";
        for (int attempt = 0; attempt <= kTransferReconnectMax; ++attempt) {
            if (attempt > 0) {
                // Exponential backoff capped at 30s (survives multi-minute flaky Wi‑Fi
                // when combined with 300s idle budget across attempts).
                int backoff_ms = std::min(30000, 500 * (1 << std::min(attempt - 1, 6)));
                std::cerr << "RETRY phase=send peer=" << peer_name
                          << " attempt=" << attempt
                          << " from_chunk=" << start_chunk
                          << " backoff_ms=" << backoff_ms << "\n";
#ifdef _WIN32
                Sleep(static_cast<DWORD>(backoff_ms));
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
#endif
            }
            auto sc = connect_and_hello(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                std::string detail = sc.fail_detail.empty()
                    ? connect_fail_string(sc.fail) : sc.fail_detail;
                last_err = "ERROR failed to connect to " + peer_name + ": " + detail;
                continue;
            }
            struct SslCloseGuard {
                SslPtr* ssl; SOCKET* sfd;
                ~SslCloseGuard() {
                    if (*sfd != INVALID_SOCKET) {
                        ssl_close(ssl->get(), *sfd);
                        *sfd = INVALID_SOCKET;
                    }
                }
            } guard{&sc.ssl, &sc.sfd};

            uint32_t resume_hint = start_chunk;
            std::string result = file_send_wait_on_transport(
                sc.ssl.get(), sc.sfd, local_path, {}, {}, nullptr, peer_name,
                sc.hello.version, start_chunk, &resume_hint, dest_path);
            if (result.rfind("ERROR", 0) != 0) return result;  // success

            last_err = result;
            // Only retry transport/ack failures — not validation or cancel.
            const bool retryable =
                result.find("send chunk") != std::string::npos ||
                result.find("transfer ack") != std::string::npos ||
                result.find("idle timeout") != std::string::npos ||
                result.find("SSL") != std::string::npos ||
                result.find("connect") != std::string::npos;
            if (!retryable) return result;
            if (resume_hint > start_chunk) start_chunk = resume_hint;
        }
        return last_err + " (after " + std::to_string(kTransferReconnectMax) +
               " reconnects; last resume chunk=" + std::to_string(start_chunk) + ")";
    }

    // Direct TLS file recv (no daemon required) — sends FileRequestMsg, receives file
    std::string direct_connect_file_recv(const std::string& peer_name,
                                         const std::string& remote_path,
                                         const std::string& local_dest) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            std::string detail = sc.fail_detail.empty()
                ? connect_fail_string(sc.fail) : sc.fail_detail;
            return "ERROR failed to connect to " + peer_name + ": " + detail;
        }
        // P2: RAII guard — ensures ssl_close runs even if recv throws
        struct SslCloseGuard {
            SslPtr* ssl; SOCKET* sfd;
            ~SslCloseGuard() { if (*sfd != INVALID_SOCKET) { ssl_close(ssl->get(), *sfd); *sfd = INVALID_SOCKET; } }
        } guard{&sc.ssl, &sc.sfd};
        // Use file_recv_wait_on_transport: sends request, receives meta + chunks, writes file
        return file_recv_wait_on_transport(
            sc.ssl.get(), sc.sfd, remote_path, local_dest, receive_dir_, {}, {});
    }

    // v2.0.6: transport-agnostic file send-wait. Runs on the event loop or a
    // worker thread; caller must ensure exclusive SSL transport access.
    // start_chunk: skip [0, start_chunk) after FileMeta handshake (resume after Wi‑Fi drop).
    // On transport error returns "ERROR …" and *resume_out is set to the first unacked chunk
    // so the caller can reconnect and continue.
    std::string file_send_wait_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& local_path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {},
            TransferTelemetryRing* telemetry_ring = nullptr,
            const std::string& peer_name = {},
            std::string_view peer_version = {},
            uint32_t start_chunk = 0,
            uint32_t* resume_out = nullptr,
            const std::string& dest_path = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        // v2.0.12c: temporarily set blocking mode for the duration of the transfer.
        // Mesh sockets are non-blocking; SSL_write_ex on non-blocking sockets
        // returns SSL_ERROR_WANT_WRITE and fails on Windows/MinGW.
        struct BlockingGuard {
            SOCKET fd;
#ifdef _WIN32
            u_long orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(0) {
                ioctlsocket(f, FIONBIO, &orig);
            }
            ~BlockingGuard() {
                u_long restore = 1;
                ioctlsocket(fd, FIONBIO, &restore);
            }
#else
            int orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(fcntl(f, F_GETFL, 0)) {
                fcntl(f, F_SETFL, orig & ~O_NONBLOCK);
            }
            ~BlockingGuard() {
                fcntl(fd, F_SETFL, orig);
            }
#endif
        } guard{sock_fd};
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };
        if (!fs::exists(local_path) || fs::is_directory(local_path))
            return "ERROR file not found or is a directory: " + local_path;

        // Resolve peer capability (+frm2 → 256KB chunks + u32 frames).
        std::string ver{peer_version};
        if (ver.empty() && !peer_name.empty()) {
            for (const auto& c : conns_) {
                if (is_live_mesh_transport_for(c, peer_name, false)) {
                    ver = c.remote_version;
                    break;
                }
            }
        }
        const size_t chunk_raw = transfer_chunk_size_for_peer(ver);
        const bool allow_large = version_has_cap(ver, kCapFrm2);

        // E1: auto-tune pipeline depth by peer RTT. High-RTT WAN peers
        // (144ms+) need more chunks in flight to saturate the link; the fixed
        // 32-deep batch idles the pipe waiting on acks. If we have a live conn
        // for this peer with a measured pong RTT (B1), scale the window
        // 32 → 64. Conservative upper bound keeps memory bounded and receiver
        // validation unchanged.
        int pipeline_size = kTransferPipelineSize;
        if (!peer_name.empty()) {
            for (const auto& c : conns_) {
                if (is_live_mesh_transport_for(c, peer_name, false) &&
                    c.pong_rtt_ms.count() > 0) {
                    if (c.pong_rtt_ms >= std::chrono::milliseconds(100))
                        pipeline_size = 64;
                    else if (c.pong_rtt_ms >= std::chrono::milliseconds(30))
                        pipeline_size = 48;
                    break;
                }
            }
        }

        uint64_t filesize = static_cast<uint64_t>(fs::file_size(local_path));
        const auto shape = calculate_transfer_metadata(
            filesize, config_.transfer_max_bytes, chunk_raw);
        if (!shape.ok) return "ERROR " + shape.reason;
        std::string filename = fs::path(local_path).filename().string();
        std::string checksum = sha256_file_stream(local_path);
        if (checksum.empty()) return "ERROR cannot hash " + local_path;
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            meta.chunk_size = static_cast<uint32_t>(chunk_raw);
            meta.dest_path = dest_path;
            write_frame(ssl, meta, CONTROL_STREAM_ID, allow_large);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        uint32_t last_acked = start_chunk;  // first chunk not yet fully acked
        // Remote path confirmed by peer via FileAck.error_msg "path=…".
        // Empty after transfer + non-empty dest_path ⇒ peer likely ignored dest.
        std::string remote_path_confirmed;

        auto mark_resume = [&](uint32_t at) {
            if (resume_out) *resume_out = at;
        };

        auto note_ack_path = [&](const FileAckMsg& ack) {
            if (ack.error) return;
            if (ack.error_msg.rfind("path=", 0) == 0 && ack.error_msg.size() > 5)
                remote_path_confirmed = ack.error_msg.substr(5);
        };

        auto wait_ack = [&](uint32_t expected_next) -> std::string {
            while (std::chrono::steady_clock::now() < overall_deadline &&
                   std::chrono::steady_clock::now() < idle_deadline) {
                if (is_cancelled && is_cancelled()) return "ERROR cancelled";
                if (SSL_pending(ssl) <= 0) {
                    bs_pollfd pfd{sock_fd, POLLIN, 0};
                    if (bs_poll(&pfd, 1, 2000) <= 0)
                        continue;
                }
                try {
                    Message resp = read_frame(ssl);
                    if (std::holds_alternative<FileAckMsg>(resp)) {
                        auto& ack = std::get<FileAckMsg>(resp);
                        if (ack.error) return "ERROR remote: " + ack.error_msg;
                        note_ack_path(ack);
                        if (ack.next_requested >= expected_next) {
                            last_acked = ack.next_requested;
                            idle_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(kTransferIdleTimeoutSec);
                            return "OK";
                        }
                        // Peer ahead of us on resume: snap forward
                        if (ack.next_requested > last_acked)
                            last_acked = ack.next_requested;
                    } else if (std::holds_alternative<PingMsg>(resp)) {
                        write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID, allow_large);
                    }
                } catch (const std::exception& e) {
                    mark_resume(last_acked);
                    return "ERROR transfer ack: " + std::string(e.what());
                } catch (...) {
                    mark_resume(last_acked);
                    return "ERROR transfer ack failed";
                }
            }
            mark_resume(last_acked);
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout";
            return "ERROR transfer idle timeout waiting for ack";
        };

        // Initial handshake ack: next_requested may be >0 if peer resumes a .part
        std::string ack = wait_ack(0);
        if (ack.rfind("ERROR", 0) == 0) return ack;
        if (last_acked > start_chunk) start_chunk = last_acked;

        std::ifstream infile(local_path, std::ios::binary);
        if (!infile) return "ERROR cannot open " + local_path;
        if (start_chunk > 0) {
            infile.seekg(static_cast<std::streamoff>(
                static_cast<uint64_t>(start_chunk) * chunk_raw), std::ios::beg);
            if (!infile) return "ERROR seek for resume failed at chunk " + std::to_string(start_chunk);
            emit("RESUME phase=send file=" + filename + " from_chunk=" +
                 std::to_string(start_chunk) + "/" + std::to_string(total_chunks));
        }
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        uint64_t bytes_sent = static_cast<uint64_t>(start_chunk) * chunk_raw;
        if (bytes_sent > filesize) bytes_sent = filesize;
        std::vector<char> raw(chunk_raw);
        auto timing = make_transfer_timing();

        for (uint32_t ci = start_chunk; ci < total_chunks; /* incremented in batch */) {
            uint32_t batch_end = std::min(ci + static_cast<uint32_t>(pipeline_size), total_chunks);

            // ── Send batch: write all chunks without waiting for acks ──
            for (; ci < batch_end; ++ci) {
                if (is_cancelled && is_cancelled()) return "ERROR cancelled";
                infile.read(raw.data(), static_cast<std::streamsize>(chunk_raw));
                size_t chunk_sz = static_cast<size_t>(infile.gcount());
                // v2.0.12c: let encode() handle compression — manual zstd_compress here
                // causes double compression which breaks on Windows/MinGW.
                std::vector<uint8_t> raw_chunk;
                if (chunk_sz > 0) {
                    raw_chunk.assign(reinterpret_cast<const uint8_t*>(raw.data()),
                                     reinterpret_cast<const uint8_t*>(raw.data()) + chunk_sz);
                }
                FileChunkMsg chunk;
                chunk.chunk_index = ci;
                chunk.total_chunks = total_chunks;
                chunk.data = std::move(raw_chunk);

                auto chunk_t0 = std::chrono::steady_clock::now();
                try { write_frame(ssl, chunk, CONTROL_STREAM_ID, allow_large); }
                catch (const std::exception& e) {
                    mark_resume(last_acked > 0 ? last_acked : ci);
                    return "ERROR send chunk: " + std::string(e.what());
                }
                bytes_sent += chunk_sz;
                auto after_write = std::chrono::steady_clock::now();

                // Per-chunk write timing (ack wait measured at batch boundary)
                {
                    int64_t w_us = std::chrono::duration_cast<std::chrono::microseconds>(after_write - chunk_t0).count();
                    timing.record(0, w_us, 0, w_us);
                }
            }

            // ── Batch boundary: drain acks (no artificial sleep — acks pace us) ──
            // Pipeline of kTransferPipelineSize chunks reduces ack round-trips ~8x.
            if (batch_end < total_chunks) {
                ack = wait_ack(batch_end);
                if (ack.rfind("ERROR", 0) == 0) return ack;
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                batch_end == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, batch_end, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }
        // Final ack (may already be acked; harmless to ask again)
        ack = wait_ack(total_chunks);
        if (ack.rfind("ERROR", 0) == 0) return ack;
        auto tel = timing.format(filename, filesize);
        if (!tel.empty()) emit(tel);
        if (telemetry_ring && timing.count > 0)
            telemetry_ring->append(make_telemetry_entry(timing, filename, filesize,
                peer_name, "send"));
        // C1: structured per-transfer record (duration + rate).
        {
            double dur_s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            double mibs = dur_s > 0.001 ? (static_cast<double>(filesize) / dur_s) / 1048576.0 : 0.0;
            log_event("transfer_complete",
                      "peer=" + peer_name + " dir=send path=" + filename +
                      " bytes=" + std::to_string(filesize) +
                      " chunks=" + std::to_string(total_chunks) +
                      " rate_mibs=" + std::to_string(static_cast<int>(mibs * 10) / 10.0) +
                      " dur_s=" + std::to_string(static_cast<int>(dur_s * 10) / 10.0) +
                      " result=ok");
        }
        log_event("file_send_wait_complete", filename + " " + std::to_string(filesize) + " bytes");
        std::string ok = "OK sent " + filename + " " + std::to_string(filesize)
                       + " bytes sha256:" + checksum;
        if (!dest_path.empty()) {
            if (remote_path_confirmed.empty()) {
                // Peer never echoed path= — almost always an older build that
                // ignored FileMeta.dest_path and wrote to receive_dir/basename.
                ok += " WARNING dest not confirmed by peer"
                      " (likely landed in receive_dir; upgrade peer for scp-style dest)";
            } else {
                ok += " dest=" + remote_path_confirmed;
            }
        } else if (!remote_path_confirmed.empty()) {
            ok += " dest=" + remote_path_confirmed;
        }
        return ok;
    }

    // v2.0.6: transport-agnostic remote file request fulfillment. Peer asked us
    // to send <path>; this runs on a worker thread with exclusive SSL access.
    std::string file_request_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {},
            TransferTelemetryRing* telemetry_ring = nullptr,
            const std::string& peer_name = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        // v2.0.12c: temporarily set blocking mode for the duration of the transfer.
        // Mesh sockets are non-blocking; SSL_write_ex on non-blocking sockets
        // returns SSL_ERROR_WANT_WRITE and fails on Windows/MinGW.
        struct BlockingGuard {
            SOCKET fd;
#ifdef _WIN32
            u_long orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(0) {
                ioctlsocket(f, FIONBIO, &orig);
            }
            ~BlockingGuard() {
                u_long restore = 1;
                ioctlsocket(fd, FIONBIO, &restore);
            }
#else
            int orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(fcntl(f, F_GETFL, 0)) {
                fcntl(f, F_SETFL, orig & ~O_NONBLOCK);
            }
            ~BlockingGuard() {
                fcntl(fd, F_SETFL, orig);
            }
#endif
        } guard{sock_fd};
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };

        // Resolve relative paths against receive_dir_ without double-nesting
        // client-supplied `.bridgesessions/received/` / `received/` prefixes.
        std::string resolved_path = resolve_file_request_path(path, receive_dir_);
        // A remote peer may serve only files contained by receive_dir_. The
        // canonical component-wise helper handles prefix collisions
        // (`received-evil`) and symlinks that point outside the directory.
        if (!config_.allow_sensitive_paths && !resolved_path.empty() &&
            !path_is_inside_directory(resolved_path, receive_dir_)) {
            log_event("file_request_error", "refused path outside receive_dir");
            try {
                write_frame(ssl, FileAckMsg{0, 0, true, "refused path outside receive_dir"},
                            CONTROL_STREAM_ID);
            } catch (...) {}
            return "ERROR refused path outside receive_dir";
        }
        if (!resolved_path.empty() && is_sensitive_mesh_path(resolved_path) &&
            !config_.allow_sensitive_paths) {
            log_event("file_request_error", "refused sensitive path");
            try {
                write_frame(ssl, FileAckMsg{0, 0, true, "refused sensitive path"},
                            CONTROL_STREAM_ID);
            } catch (...) {}
            return "ERROR refused sensitive path";
        }
        if (resolved_path.empty() || !fs::exists(resolved_path) || fs::is_directory(resolved_path)) {
            // Do not return local absolute paths or receive_dir_ to the remote.
            log_event("file_request_error", "not found: " + fs::path(path).filename().string());
            try {
                write_frame(ssl, FileAckMsg{0, 0, true, "file not found"},
                            CONTROL_STREAM_ID);
            } catch (...) {}
            return "ERROR file not found";
        }
        uint64_t filesize = static_cast<uint64_t>(fs::file_size(resolved_path));
        // peer_name param identifies the requester; use their Hello caps.
        const size_t chunk_raw = transfer_chunk_size_for_peer(
            [&]() -> std::string {
                for (const auto& c : conns_) {
                    if (is_live_mesh_transport_for(c, peer_name, false))
                        return c.remote_version;
                }
                return {};
            }());
        const bool allow_large = chunk_raw > kTransferChunkRawSizeDefault;
        const auto shape = calculate_transfer_metadata(
            filesize, config_.transfer_max_bytes, chunk_raw);
        if (!shape.ok) {
            try { write_frame(ssl, FileAckMsg{0, 0, true, shape.reason}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR " + shape.reason;
        }
        std::string filename = fs::path(resolved_path).filename().string();
        std::string checksum = sha256_file_stream(resolved_path);
        if (checksum.empty()) {
            log_event("file_request_error", "cannot hash " + resolved_path);
            try { write_frame(ssl, FileAckMsg{0, 0, true, "cannot hash file"}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR cannot hash file";
        }
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            meta.chunk_size = static_cast<uint32_t>(chunk_raw);
            write_frame(ssl, meta, CONTROL_STREAM_ID, allow_large);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }
        log_event("file_request_sending", filename + " " + std::to_string(total_chunks) + " chunks");

        std::ifstream infile(resolved_path, std::ios::binary);
        if (!infile) { log_event("file_request_error", "cannot open " + resolved_path); return "ERROR cannot open " + path; }
        std::vector<char> raw(chunk_raw);

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        uint64_t bytes_sent = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        auto timing = make_transfer_timing();

        for (uint32_t ci = 0; ci < total_chunks; /* incremented in batch */) {
            uint32_t batch_end = std::min(ci + kTransferPipelineSize, total_chunks);
            if (is_cancelled && is_cancelled()) {
                try { write_frame(ssl, FileAckMsg{ci, ci, true, "cancelled"}, CONTROL_STREAM_ID); } catch (...) {}
                return "ERROR cancelled";
            }
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout at chunk " + std::to_string(ci);
            if (std::chrono::steady_clock::now() >= idle_deadline)
                return "ERROR transfer idle timeout at chunk " + std::to_string(ci);

            // ── Send batch: write all chunks, one select+breather between ──
            for (; ci < batch_end; ++ci) {
                infile.read(raw.data(), static_cast<std::streamsize>(chunk_raw));
                size_t chunk_sz = static_cast<size_t>(infile.gcount());
                // v2.0.12c: let encode() handle compression — manual zstd_compress here
                // causes double compression which breaks on Windows/MinGW.
                FileChunkMsg chunk;
                chunk.chunk_index = ci; chunk.total_chunks = total_chunks;
                if (chunk_sz > 0) {
                    chunk.data.assign(raw.data(), raw.data() + chunk_sz);
                }

                auto chunk_t0 = std::chrono::steady_clock::now();

                // Brief write+readiness check every 4 chunks (was every chunk)
                if ((ci % 4) == 0) {
                    bs_pollfd pfd{sock_fd, static_cast<short>(POLLIN | POLLOUT), 0};
                    int sel = bs_poll(&pfd, 1, 2000);
                    if (sel == 0) return "ERROR transfer idle timeout at chunk " + std::to_string(ci);
                }

                try { write_frame(ssl, chunk, CONTROL_STREAM_ID, allow_large); }
                catch (const std::exception& e) {
                    log_event("file_request_error", "send chunk failed " + std::to_string(ci));
                    return "ERROR send chunk " + std::to_string(ci) + ": " + e.what();
                }

                auto after_write = std::chrono::steady_clock::now();
                bytes_sent += chunk_sz;

                {
                    int64_t t_us = std::chrono::duration_cast<std::chrono::microseconds>(after_write - chunk_t0).count();
                    timing.record(0, t_us, 0, t_us);
                }
            }

            // ── Batch boundary: drain pending acks ──
            {
                auto drain_start = std::chrono::steady_clock::now();
                // Drain ALL pending frames before next batch
                while (SSL_pending(ssl) > 0) {
                    try {
                        Message resp = read_frame(ssl);
                        if (std::holds_alternative<PingMsg>(resp)) {
                            write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                        } else if (std::holds_alternative<FileAckMsg>(resp)) {
                            auto& ack = std::get<FileAckMsg>(resp);
                            if (ack.error) return "ERROR remote: " + ack.error_msg;
                        }
                    } catch (...) { break; }
                }
                // Also check socket-level readability for frames not yet in SSL buffer.
                {
                    bs_pollfd pfd{sock_fd, POLLIN, 0};
                    if (bs_poll(&pfd, 1, 0) > 0) {
                        try {
                            Message resp = read_frame(ssl);
                            if (std::holds_alternative<PingMsg>(resp)) {
                                write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                            } else if (std::holds_alternative<FileAckMsg>(resp)) {
                                auto& ack = std::get<FileAckMsg>(resp);
                                if (ack.error) return "ERROR remote: " + ack.error_msg;
                            }
                        } catch (...) {}
                    }
                }
            }

            idle_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(kTransferIdleTimeoutSec);

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                batch_end == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, batch_end, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }
        auto tel = timing.format(filename, filesize);
        if (!tel.empty()) emit(tel);
        if (telemetry_ring && timing.count > 0)
            telemetry_ring->append(make_telemetry_entry(timing, filename, filesize,
                peer_name, "send"));
        log_event("file_request_complete", filename + " " + std::to_string(filesize) + " bytes");
        return "OK sent " + filename + " " + std::to_string(filesize) + " bytes sha256:" + checksum;
    }

    std::string daemon_file_send_wait(const std::string& peer_name, const std::string& local_path,
                                      const std::function<void(const std::string&)>& on_progress = {}) {
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) {
                target = &c; break;
            }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_started_at = std::chrono::steady_clock::now();
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } guard{target->exec_busy, target->exec_completed};
        target->exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return file_send_wait_on_transport(target->ssl.get(), target->sock_fd, local_path, {}, on_progress,
                                           &transfer_telemetry_, peer_name, {}, 0, nullptr, {});
    }

    // v2.0.6: dispatch entry point for long-operation worker pool.
    void execute_long_operation_task(const LongOperationTask& task) {
        struct FrameCancellationGuard {
            const std::atomic<bool>* previous = g_frame_io_cancelled;
            explicit FrameCancellationGuard(const std::atomic<bool>* current) {
                g_frame_io_cancelled = current;
            }
            ~FrameCancellationGuard() { g_frame_io_cancelled = previous; }
        } frame_cancel_guard{task.cancelled.get()};
        auto progress_to_ipc = [&](const std::string& line) {
            // Refresh the shared "last progress" timestamp on every transfer
            // progress tick. The exec watchdog (check_stale_exec) measures the
            // 90s deadline from this, so a healthy, actively-streaming transfer
            // is never killed, while a STALLED transfer (no progress for 90s)
            // still trips it (BUG-1 guarantee).
            if (task.last_progress_at)
                task.last_progress_at->store(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            if (task.ipc_fd != INVALID_SOCKET) {
                std::string msg = line + "\n";
                send(task.ipc_fd, msg.data(), (int)msg.size(), 0);
            }
        };

        switch (task.type) {
        case LongOperationTask::Type::FileSendWait: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_send_wait_on_transport(
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc,
                &transfer_telemetry_, task.peer_name, {}, 0, nullptr, task.path2);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            } else {
                log_event("file_send_worker_complete", task.peer_name + " " + result);
            }
            break;
        }
        case LongOperationTask::Type::FileRecvWait: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_recv_wait_on_transport(
                task.ssl, task.sock_fd, task.path1, task.path2, receive_dir_, is_cancelled, progress_to_ipc);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            }
            break;
        }
        case LongOperationTask::Type::RemoteFileRequest: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_request_on_transport(
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc,
                &transfer_telemetry_, task.peer_name);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            } else {
                log_event("file_request_worker_complete", task.peer_name + " " + result);
            }
            break;
        }
        case LongOperationTask::Type::AutoUpgrade: {
            // Defense in depth: preserve the shell-safety contract even if a
            // future caller queues this task without the front-end check.
            if (!bs_peer_name_shell_safe(task.peer_name)) {
                log_event("auto_upgrade_rejected", "unsafe queued peer name");
                break;
            }
            // r3 fix (P1b): re-check the pin at EXECUTION time. The task may sit
            // in the pool queue long enough for an operator to pin=false on disk;
            // honoring the newest intent prevents the 2026-08-31 mid-wave
            // downgrade shot (D-002 incident, bs-mesh.log 19:39:34Z).
            // Greptile P1 (26.08.31-release): the worker thread must NOT touch
            // shared config state — neither maybe_reload_config_seeds() (mutates
            // config_mtime_ / seeds vector) nor even reading config_.auto_upgrade
            // (the event loop writes that field during hot-reload — data race).
            // Pure stateless snapshot read: parse the file into a local and
            // touch nothing shared. config_file_path_ is written only during
            // daemon construction, so reading it here is race-free. Absent
            // file → loader default (auto_upgrade = true), mirroring load_config.
            bool pin_live = true;
            if (!config_file_path_.empty()) {
                std::error_code pin_ec;
                if (std::filesystem::exists(config_file_path_, pin_ec) && !pin_ec) {
                    pin_live = load_config(config_file_path_).auto_upgrade;
                }
            }
            if (!pin_live) {
                log_event("auto_upgrade_rejected",
                          task.peer_name + " pin=false at execution time");
                break;
            }
            const std::string cmd =
                "bridgesessions shell " + task.peer_name +
                " --cmd 'bridgesessions upgrade' >/dev/null 2>&1";
            const int rc = std::system(cmd.c_str());
            log_event("auto_upgrade_complete",
                      task.peer_name + " rc=" + std::to_string(rc));
            break;
        }
        }

        // Clear busy/completed flags on the captured shared_ptrs.
        if (task.exec_completed) task.exec_completed->store(true);
        if (task.exec_busy) task.exec_busy->store(false);
    }

    // ── Daemon file request handler: peer asks us to send them a file ──
    // v2.0.6: offload the long send to the worker pool so the event loop stays
    // responsive. The worker exclusively owns this SSL transport while busy.
    void handle_file_request(Conn& c, const FileRequestMsg& m) {
        log_event("file_request_received", m.path + " from " + c.peer_name);
        if (c.exec_busy->exchange(true)) {
            log_event("file_request_busy", c.peer_name);
            // The existing worker owns SSL exclusively. Writing a busy reply
            // here would race its TLS record stream; drop the duplicate request.
            return;
        }
        c.exec_completed->store(false);
        c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
        c.exec_started_at = std::chrono::steady_clock::now();
        // Reset the shared last-progress timestamp to now so a healthy,
        // actively-streaming inbound file pull survives the 90s exec watchdog
        // (BUG-1 guarantee). Without this, check_stale_exec() falls back to
        // exec_started_at and force-cancels any pull that exceeds 90s.
        c.exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());

        LongOperationTask task;
        task.type = LongOperationTask::Type::RemoteFileRequest;
        task.peer_name = c.peer_name;
        task.path1 = m.path;
        task.ssl = c.ssl.get();
        task.sock_fd = c.sock_fd;
        task.exec_busy = c.exec_busy;
        task.exec_completed = c.exec_completed;
        task.cancelled = c.exec_cancelled;
        task.last_progress_at = c.exec_last_progress_at;
        task.ipc_fd = INVALID_SOCKET;
        worker_pool_->enqueue(std::move(task));
    }

    bool begin_async_receive(Conn& target, const std::string& dest_dir) {
        if (!target.pending_recv_dir.empty() || target.file_receive.active) return false;
        target.pending_recv_dir = dest_dir;
        return true;
    }

    // ── Daemon file recv: send FileRequest to peer (non-blocking) ──
    std::string daemon_file_recv(const std::string& peer_name, const std::string& remote_path,
                                 const std::string& local_dir = "") {
        log_event("file_recv_request",
                  std::filesystem::path(remote_path).filename().string()
                      + " from " + peer_name);
        std::string dest_dir = local_dir.empty() ? receive_dir_ : local_dir;
        if (!local_dir.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(local_dir, ec);
            if (ec) return "ERROR cannot create receive directory " + local_dir;
        }
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (!begin_async_receive(*target, dest_dir))
            return "ERROR receive already pending for " + peer_name;

        FileRequestMsg req;
        req.path = remote_path;
        if (!enqueue_frame(*target, req, CONTROL_STREAM_ID)) {
            target->pending_recv_dir.clear();
            return "ERROR could not queue file request";
        }

        log_event("file_recv_request_sent",
                  std::filesystem::path(remote_path).filename().string()
                      + " -> " + peer_name + " (async)");
        return "request sent to " + peer_name + " (arrives async)";
    }

    // v2.0.6: transport-agnostic file recv-wait. Caller must ensure exclusive SSL transport access.
    std::string file_recv_wait_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& remote_path,
            const std::string& local_dest, const std::string& receive_dir,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };

        try {
            FileRequestMsg req;
            req.path = remote_path;
            write_frame(ssl, req, CONTROL_STREAM_ID);
        } catch (const std::exception& e) {
            return "ERROR send request: " + std::string(e.what());
        }

        auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7200);
        auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
        std::optional<FileMetaMsg> meta;
        while (!meta && std::chrono::steady_clock::now() < overall_deadline &&
               std::chrono::steady_clock::now() < idle_deadline) {
            if (is_cancelled && is_cancelled()) return "ERROR cancelled";
            if (SSL_pending(ssl) <= 0) {
                bs_pollfd pfd{sock_fd, POLLIN, 0};
                if (bs_poll(&pfd, 1, 2000) <= 0)
                    continue;
            }
            try {
                Message resp = read_frame(ssl);
                if (std::holds_alternative<FileMetaMsg>(resp)) {
                    meta = std::get<FileMetaMsg>(resp);
                    idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
                } else if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                }
            } catch (const std::exception& e) {
                return "ERROR receive meta: " + std::string(e.what());
            }
        }
        if (!meta) return "ERROR transfer timeout waiting for file metadata";

        const size_t recv_chunk_raw = effective_transfer_chunk_size(meta->chunk_size);
        const auto metadata = validate_transfer_metadata(
            meta->filesize, meta->total_chunks, config_.transfer_max_bytes, recv_chunk_raw);
        if (!metadata.ok) {
            try { write_frame(ssl, FileAckMsg{0, 0, true, metadata.reason}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR " + metadata.reason;
        }
        overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(meta->filesize);

        auto safe_name = sanitize_transfer_filename(meta->filename);
        if (!safe_name) return "ERROR rejected unsafe remote filename";

        fs::path dest = local_dest.empty() ? fs::path(receive_dir) : fs::path(local_dest);
        std::string dest_str = dest.string();
        bool dest_is_dir = dest_str.empty() || dest_str.back() == '/' || dest_str.back() == '\\';
        std::error_code ec;
        if (!dest_is_dir && fs::exists(dest, ec) && fs::is_directory(dest, ec)) dest_is_dir = true;
        if (dest_is_dir) dest /= *safe_name;
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        std::string part_path = dest.string() + ".part";

        std::ofstream out(part_path, std::ios::binary | std::ios::trunc);
        if (!out) return "ERROR cannot open " + part_path;
        struct PartialFileGuard {
            std::string path;
            bool committed = false;
            ~PartialFileGuard() {
                if (!committed) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
        } partial_guard{part_path};

        Sha256Stream hasher;
        if (!hasher.ok()) return "ERROR sha256 init failed";

        uint32_t chunks_recv = 0;
        uint64_t bytes_recv = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        try { write_frame(ssl, FileAckMsg{0, 0, false, ""}, CONTROL_STREAM_ID); } catch (...) {}
        while (chunks_recv < meta->total_chunks &&
               std::chrono::steady_clock::now() < overall_deadline &&
               std::chrono::steady_clock::now() < idle_deadline) {
            if (is_cancelled && is_cancelled()) return "ERROR cancelled";
            if (SSL_pending(ssl) <= 0) {
                bs_pollfd pfd{sock_fd, POLLIN, 0};
                if (bs_poll(&pfd, 1, 2000) <= 0)
                    continue;
            }
            try {
                Message resp = read_frame(ssl);
                if (std::holds_alternative<FileChunkMsg>(resp)) {
                    auto& chunk = std::get<FileChunkMsg>(resp);
                    std::vector<uint8_t> data;
                    if (!chunk.data.empty())
                        data = decompress_chunk_payload(std::span<const uint8_t>(chunk.data.data(), chunk.data.size()));
                    const auto chunk_valid = validate_transfer_chunk(
                        meta->filesize, bytes_recv, chunks_recv, meta->total_chunks,
                        chunk.chunk_index, chunk.total_chunks, data.size(), recv_chunk_raw);
                    if (!chunk_valid.ok) {
                        try { write_frame(ssl, FileAckMsg{
                            chunk.chunk_index, chunks_recv, true, chunk_valid.reason},
                            CONTROL_STREAM_ID); } catch (...) {}
                        return "ERROR " + chunk_valid.reason;
                    }
                    if (!data.empty()) {
                        out.write(reinterpret_cast<const char*>(data.data()),
                                  static_cast<std::streamsize>(data.size()));
                        if (!out) return "ERROR failed to write receive file";
                        hasher.update(data);
                        bytes_recv += data.size();
                    }
                    ++chunks_recv;
                    idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
                    write_frame(ssl, FileAckMsg{chunk.chunk_index, chunks_recv, false, ""}, CONTROL_STREAM_ID);

                    auto now = std::chrono::steady_clock::now();
                    if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                        chunks_recv == meta->total_chunks) {
                        last_progress = now;
                        double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                        double rate = (static_cast<double>(bytes_recv) / elapsed) / (1024.0 * 1024.0);
                        int eta = 0;
                        if (rate > 0.001 && meta->filesize > bytes_recv)
                            eta = static_cast<int>((static_cast<double>(meta->filesize - bytes_recv) /
                                                   (rate * 1024.0 * 1024.0)));
                        emit(format_transfer_progress("recv", *safe_name, chunks_recv, meta->total_chunks,
                                                      bytes_recv, meta->filesize, rate, eta));
                    }
                } else if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                }
            } catch (const std::exception& e) {
                return "ERROR receive chunk: " + std::string(e.what());
            }
        }
        out.close();
        if (chunks_recv < meta->total_chunks) {
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout after " + std::to_string(chunks_recv) + "/" +
                       std::to_string(meta->total_chunks) + " chunks";
            return "ERROR transfer idle timeout after " + std::to_string(chunks_recv) + "/" +
                   std::to_string(meta->total_chunks) + " chunks";
        }

        std::string actual = hasher.final_hex();
        if (actual != meta->checksum) {
            return "ERROR checksum mismatch expected " + meta->checksum + " got " + actual;
        }
        if (bytes_recv != meta->filesize) {
            return "ERROR received byte count does not match metadata";
        }
        fs::rename(part_path, dest, ec);
        if (ec) return "ERROR rename failed: " + ec.message();
        partial_guard.committed = true;

        log_event("file_recv_wait_complete", meta->filename + " -> " + dest.string());
        return "OK received " + dest.string() + " " + std::to_string(bytes_recv) + " bytes sha256:" + actual;
    }

    std::string daemon_file_recv_wait(const std::string& peer_name, const std::string& remote_path,
                                      const std::string& local_dest,
                                      const std::function<void(const std::string&)>& on_progress = {}) {
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) {
                target = &c; break;
            }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_started_at = std::chrono::steady_clock::now();
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } guard{target->exec_busy, target->exec_completed};
        target->exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return file_recv_wait_on_transport(target->ssl.get(), target->sock_fd, remote_path, local_dest, receive_dir_, {}, on_progress);
    }

    std::string daemon_reconnect_peer(const std::string& peer_name) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR unknown peer: " + peer_name;

        std::string pubkey;
        for (const auto& s : config_.seeds) {
            if (peer_name_eq(s.name, peer_name)) { pubkey = s.pubkey_hex; break; }
        }
        if (pubkey.empty()) {
            for (const auto& d : config_.discovered) {
                if (peer_name_eq(d.name, peer_name)) { pubkey = d.pubkey_hex; break; }
            }
        }

        auto matches_peer = [&](const Conn& c) {
            return c.purpose == ConnectionPurpose::Mesh &&
                   (peer_name_eq(c.peer_name, peer_name) ||
                    (!addr.empty() && c.peer_addr == addr) ||
                    (!pubkey.empty() && c.peer_pubkey == pubkey));
        };

        // Preflight all matching transports before removing any of them. If a
        // worker owns even one duplicate, reject atomically instead of tearing
        // down idle siblings and then discovering the busy transport.
        for (const auto& c : conns_) {
            if (matches_peer(c) && c.exec_busy && c.exec_busy->load())
                return "ERROR peer busy with an active data operation";
        }

        int removed = 0;
        for (size_t i = 0; i < conns_.size();) {
            auto& c = conns_[i];
            if (!matches_peer(c)) { ++i; continue; }
            if (!remove_conn(i))
                return "ERROR peer busy with an active data operation";
            ++removed;
        }

        backoffs_.erase(addr);
        PeerEntry target_peer{peer_name, addr, pubkey};
        if (should_accept_only_for(target_peer)) {
            auto accept_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(kTieBreakAcceptWindowMs);
            accept_only_until_[addr] = accept_deadline;
            log_event("peer_reconnect_defer_dial",
                      peer_name + " addr=" + addr + " accept_only_ms=" +
                      std::to_string(kTieBreakAcceptWindowMs));
            return "OK reconnect waiting for inbound peer " + peer_name;
        }
        if (!start_outbound_handshake(target_peer))
            return "ERROR reconnect already pending or could not start for " + peer_name;
        log_event("peer_reconnect", peer_name + " removed=" + std::to_string(removed) +
                  " async_started");
        return "OK reconnecting " + peer_name;
    }

    void service_reconnect_wait_once(int timeout_ms) {
        if (listen_fd_ == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return;
        }

        std::vector<bs_pollfd> pfds;
        auto add = [&](SOCKET fd, short events) {
            if (fd == INVALID_SOCKET) return;
            bs_pollfd p{};
            p.fd = static_cast<decltype(p.fd)>(fd);
            p.events = events;
            pfds.push_back(p);
        };
        add(listen_fd_, POLLIN);
        for (const auto& c : conns_) {
            if (c.exec_busy && c.exec_busy->load()) continue;
            if (c.sock_fd == INVALID_SOCKET) continue;
            add(c.sock_fd, POLLIN);
        }
        for (auto& ph : pending_handshakes_) {
            if (ph.sock_fd == INVALID_SOCKET) continue;
            short ev = 0;
            if (ph.want_read) ev = static_cast<short>(ev | POLLIN);
            if (ph.want_write) ev = static_cast<short>(ev | POLLOUT);
            if (ev) add(ph.sock_fd, ev);
        }

        int n = pfds.empty() ? 0 : bs_poll(pfds.data(),
#ifdef _WIN32
            static_cast<unsigned long>(pfds.size()),
#else
            static_cast<nfds_t>(pfds.size()),
#endif
            timeout_ms);
        if (n <= 0) return;

        if (poll_fd_readable(pfds, listen_fd_))
            accept_inbound();
        for (int i = 0; i < static_cast<int>(conns_.size()); ++i) {
            if (conns_[static_cast<size_t>(i)].sock_fd != INVALID_SOCKET &&
                poll_fd_readable(pfds, conns_[static_cast<size_t>(i)].sock_fd)) {
                check_conn_read(i);
            }
        }

        // v2.0.6: advance outbound handshakes while waiting for reconnect.
        advance_handshakes();

        check_stale_exec();
        check_pong_timeouts();
        clean_dead_conns();
    }

    std::string daemon_stats_summary() const {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
        size_t live_conns = 0;
        uint64_t bytes_in = 0, bytes_out = 0;
        struct PRow {
            std::string name, addr, dir, up, pong, bin, bout, sess;
        };
        std::vector<PRow> prows;
        auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                          ? config_.pong_timeout_secs : 30);
        for (const auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            ++live_conns;
            bytes_in += c.bytes_in;
            bytes_out += c.bytes_out;
            PRow r;
            r.name = c.peer_name.empty() ? "-" : c.peer_name;
            r.addr = peer_listen_addr_for(c.peer_name, c.peer_pubkey);
            if (r.addr.empty()) r.addr = c.peer_addr;
            r.dir = c.is_outbound ? "out" : "in";
            auto up_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - c.connected_at).count();
            auto pong_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - c.last_pong).count();
            r.up = std::to_string(up_s) + "s";
            r.pong = (now - c.last_pong) <= fresh
                ? (std::to_string(pong_s) + "s") : ("stale " + std::to_string(pong_s) + "s");
            r.bin = std::to_string(c.bytes_in);
            r.bout = std::to_string(c.bytes_out);
            r.sess = c.attached_session ? c.attached_session->name : "-";
            prows.push_back(std::move(r));
        }
        auto live_sessions = sessions_.list();
        std::ostringstream out;
        out << "node     " << config_.node_name << "\n"
            << "uptime   " << uptime << "s\n"
            << "peers    " << live_conns << " / " << config_.max_peers << "\n"
            << "sessions " << live_sessions.size() << "\n"
            << "bytes_in " << bytes_in << "\n"
            << "bytes_out " << bytes_out << "\n";
        if (prows.empty()) {
            out << "(no live connections)\n";
            return out.str();
        }
        size_t w_n = 4, w_a = 7, w_d = 3, w_u = 2, w_p = 4, w_i = 2, w_o = 3, w_s = 4;
        for (const auto& r : prows) {
            w_n = std::max(w_n, r.name.size());
            w_a = std::max(w_a, r.addr.size());
            w_d = std::max(w_d, r.dir.size());
            w_u = std::max(w_u, r.up.size());
            w_p = std::max(w_p, r.pong.size());
            w_i = std::max(w_i, r.bin.size());
            w_o = std::max(w_o, r.bout.size());
            w_s = std::max(w_s, r.sess.size());
        }
        auto pad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
        };
        auto rpad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
        };
        out << "\n"
            << pad("PEER", w_n) << "  " << pad("ADDRESS", w_a) << "  "
            << pad("DIR", w_d) << "  " << rpad("UP", w_u) << "  "
            << pad("PONG", w_p) << "  " << rpad("IN", w_i) << "  "
            << rpad("OUT", w_o) << "  " << pad("SESS", w_s) << "\n";
        size_t total = w_n + w_a + w_d + w_u + w_p + w_i + w_o + w_s + 2 * 7;
        out << std::string(total, '-') << "\n";
        for (const auto& r : prows) {
            out << pad(r.name, w_n) << "  " << pad(r.addr, w_a) << "  "
                << pad(r.dir, w_d) << "  " << rpad(r.up, w_u) << "  "
                << pad(r.pong, w_p) << "  " << rpad(r.bin, w_i) << "  "
                << rpad(r.bout, w_o) << "  " << pad(r.sess, w_s) << "\n";
        }
        return out.str();
    }

    // One-shot shell IPC deliberately delegates to the client direct-TLS path;
    // no background worker may borrow a mesh connection's SSL transport.


    std::string vfolder_sync_direct(const std::string& name) {
        const MeshConfig::VFolderEntry* vf = nullptr;
        for (const auto& entry : config_.vfolders) {
            if (entry.name == name) { vf = &entry; break; }
        }
        if (!vf) return "ERROR no vfolder: " + name;
        if (vf->direction != "push")
            return "ERROR only push vfolder sync is implemented";
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = expand_home(vf->local_path);
        if (!fs::is_directory(root, ec)) return "ERROR local vfolder is not a directory";
        size_t sent = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
            if (ec) return "ERROR vfolder traversal failed: " + ec.message();
            if (!entry.is_regular_file(ec)) continue;
            const fs::path relative = fs::relative(entry.path(), root, ec);
            if (ec) return "ERROR vfolder relative path failed";
            const fs::path remote = fs::path(vf->remote_path) / relative;
            const std::string result = direct_connect_file_send(
                vf->remote_peer, entry.path().string(), true, remote.generic_string());
            if (result.rfind("ERROR", 0) == 0) return result;
            ++sent;
        }
        return "OK synced " + name + " (" + std::to_string(sent) + " files)";
    }

    void handle_file_ack(Conn& c, const FileAckMsg& m) {
        if (m.error) {
            log_event("file_send_failed", m.error_msg);
            return;
        }
        log_event("file_chunk_acked", "chunk " + std::to_string(m.chunk_index)
                   + " next=" + std::to_string(m.next_requested));
    }

    void handle_dht_find_value(Conn& c, const DhtFindValueMsg& query) {
#ifndef BS_NO_DHT
        if (!config_.dht_enabled || !dht_inited_) return;
        // Currently no value storage — reply with closest nodes
        auto closest = dht_.find_closest(query.key, 20);
        GossipMsg g;
        for (auto& dp : closest) {
            PeerInfo pi;
            pi.name = dp.name;
            pi.addr = dp.addr;
            pi.last_seen = dp.last_seen;
            g.peers.push_back(std::move(pi));
        }
        if (!g.peers.empty()) {
            (void)enqueue_frame(c, g, CONTROL_STREAM_ID);
        }
#endif
    }

    // ── Dispatch a received message ────────────────────────────

    void dispatch_message(int conn_idx, Message& msg) {
        auto& c = conns_[static_cast<size_t>(conn_idx)];
        if (c.purpose == ConnectionPurpose::Unknown) {
            c.purpose = std::holds_alternative<AttachMsg>(msg)
                ? ConnectionPurpose::DirectSession
                : ConnectionPurpose::Mesh;
        }

        if (std::holds_alternative<PingMsg>(msg)) {
            (void)enqueue_frame(c, PongMsg{}, CONTROL_STREAM_ID);
        }
        else if (std::holds_alternative<PongMsg>(msg)) {
            const auto now = std::chrono::steady_clock::now();
            c.last_pong = now;
            // B1: derive RTT from the last ping we sent on this conn.
            if (c.ping_sent_at != std::chrono::steady_clock::time_point{}) {
                auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - c.ping_sent_at);
                if (rtt > std::chrono::milliseconds(0))
                    c.pong_rtt_ms = rtt;
            }
        }
        else if (std::holds_alternative<HelloMsg>(msg)) {
            auto& h = std::get<HelloMsg>(msg);
            if (!c.initial_hello.has_value()) {
                // Should normally be set during handshake, but handle defensively.
                c.initial_hello = h;
                c.peer_name = h.node_name;
                merge_peers(h.known_peers);
            } else if (*c.initial_hello == h) {
                // Identical retransmission: ignore silently.
                log_event("hello_duplicate_ignored", c.peer_name);
            } else {
                // Mismatched follow-up Hello: close the connection.
                log_event("hello_mismatch_close", c.peer_name);
                c.close_requested = true;
            }
        }
        else if (std::holds_alternative<GossipMsg>(msg)) {
            auto& g = std::get<GossipMsg>(msg);
            merge_peers(g.peers);
        }
        else if (std::holds_alternative<DirectoryEnrollMsg>(msg)) {
            // Bootstrap: verify + apply a signed member enrollment, then
            // re-gossip it so it propagates transitively across the mesh.
            // Relay only once per entry to bound flood (dedupe below).
            auto& e = std::get<DirectoryEnrollMsg>(msg);
            if (apply_directory_enroll(e)) {
                // Re-broadcast to all OTHER mesh peers (exclude the sender) so
                // the enrollment reaches peers not directly connected to the
                // issuer. The signature chains trust transitively.
                std::lock_guard lock(enroll_seen_mutex_);
                const std::string key = e.issuer_pubkey + "|" + e.pubkey_hex +
                                        "|" + std::to_string(e.issued_at);
                if (enroll_seen_.insert(key).second) {
                    for (auto& oc : conns_) {
                        if (&oc == &c) continue;
                        if (oc.sock_fd == INVALID_SOCKET || oc.purpose != ConnectionPurpose::Mesh) continue;
                        if (oc.exec_busy && oc.exec_busy->load()) continue;
                        (void)enqueue_frame(oc, e, CONTROL_STREAM_ID);
                    }
                }
            }
        }
        else if (std::holds_alternative<ServerInfoMsg>(msg)) {
            auto& info = std::get<ServerInfoMsg>(msg);
            if (!info.version.empty()) c.remote_version = info.version;
            c.remote_load = info.load;
            if (!info.host_stats_json.empty()) {
                // Object shape; sessions use array shape. Both MoA-gated.
                if (host_stats_json_shape_ok(info.host_stats_json))
                    c.remote_host_stats_json = std::move(info.host_stats_json);
                else
                    log_event("gossip_host_stats_rejected_bad_json", c.peer_name);
            }
            if (!info.sessions_summary_json.empty() && !c.peer_name.empty()) {
                // 2.0.8 MoA fix: validate at the trust boundary. The payload is
                // re-interpolated VERBATIM into MESH_TREE output — a malformed
                // or envelope-breaking value from a peer would corrupt the
                // panel's JSON.parse. Authenticated ≠ safe (defense-in-depth).
                if (gossip_json_shape_ok(info.sessions_summary_json)) {
                    std::unique_lock lock(gossip_sessions_mutex_);
                    gossip_sessions_json_[c.peer_name] = std::move(info.sessions_summary_json);
                } else {
                    log_event("gossip_sessions_rejected_bad_json", c.peer_name);
                }
            }
        }
        else if (std::holds_alternative<SdpOfferMsg>(msg)) {
            handle_sdp_offer(c, std::get<SdpOfferMsg>(msg));
        }
        else if (std::holds_alternative<SdpAnswerMsg>(msg)) {
            handle_sdp_answer(c, std::get<SdpAnswerMsg>(msg));
        }
        else if (std::holds_alternative<DhtFindNodeMsg>(msg)) {
            handle_dht_find_node(c, std::get<DhtFindNodeMsg>(msg));
        }
        else if (std::holds_alternative<DhtFindValueMsg>(msg)) {
            handle_dht_find_value(c, std::get<DhtFindValueMsg>(msg));
        }
        else if (std::holds_alternative<FileMetaMsg>(msg)) {
            handle_file_meta(c, std::get<FileMetaMsg>(msg));
        }
        else if (std::holds_alternative<FileChunkMsg>(msg)) {
            handle_file_chunk(c, std::get<FileChunkMsg>(msg));
        }
        else if (std::holds_alternative<FileAckMsg>(msg)) {
            handle_file_ack(c, std::get<FileAckMsg>(msg));
        }
        else if (std::holds_alternative<FileRequestMsg>(msg)) {
            handle_file_request(c, std::get<FileRequestMsg>(msg));
        }
        else {
            // Route everything else through session / common handlers
            handle_inbound_session(c, msg);
            handle_outbound_session(c, msg);
            common_message_handler(c, msg);
        }
    }

    // ── Check for data on a connection ─────────────────────────

public:
    // 2.0.8 MoA: minimal JSON-array shape validator for peer-supplied
    // sessions_summary_json (re-interpolated verbatim into MESH_TREE).
    // Checks: non-empty, '[' ... ']', and bracket/brace balance outside
    // string literals with backslash-escape awareness. Not a full JSON parse —
    // sufficient to guarantee the value composes as one JSON value.
    // Public: pure static, exercised directly by the test suite.
    static bool gossip_json_shape_ok(const std::string& v) {
        if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
        int depth_square = 0, depth_curly = 0;
        bool in_str = false, esc = false;
        for (size_t i = 0; i < v.size(); ++i) {
            char ch = v[i];
            if (in_str) {
                if (esc) { esc = false; continue; }
                if (ch == '\\') { esc = true; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
                case '"': in_str = true; break;
                case '[': ++depth_square; break;
                case ']':
                    if (--depth_square < 0) return false;
                    if (depth_square == 0) {
                        // Outermost array closed: only trailing whitespace may
                        // follow. Anything else is a second value trying to
                        // break the MESH_TREE envelope (MoA gossip finding).
                        for (size_t j = i + 1; j < v.size(); ++j)
                            if (!std::isspace(static_cast<unsigned char>(v[j]))) return false;
                        return depth_curly == 0;
                    }
                    break;
                case '{': ++depth_curly; break;
                case '}': if (--depth_curly < 0) return false; break;
                default: break;
            }
        }
        return false; // outermost array never closed
    }

    // Object-shape validator for ServerInfo.host_stats_json ({…}). Same MoA
    // rules as gossip_json_shape_ok but for a single JSON object.
    static bool host_stats_json_shape_ok(const std::string& v) {
        if (v.size() < 2 || v.front() != '{' || v.back() != '}') return false;
        if (v.size() > 2048) return false;
        int depth_square = 0, depth_curly = 0;
        bool in_str = false, esc = false;
        for (size_t i = 0; i < v.size(); ++i) {
            char ch = v[i];
            if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t') return false;
            if (in_str) {
                if (esc) { esc = false; continue; }
                if (ch == '\\') { esc = true; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
                case '"': in_str = true; break;
                case '{': ++depth_curly; break;
                case '}':
                    if (--depth_curly < 0) return false;
                    if (depth_curly == 0) {
                        for (size_t j = i + 1; j < v.size(); ++j)
                            if (!std::isspace(static_cast<unsigned char>(v[j]))) return false;
                        return depth_square == 0;
                    }
                    break;
                case '[': ++depth_square; break;
                case ']': if (--depth_square < 0) return false; break;
                default: break;
            }
        }
        return false;
    }

    // Remove this transport's attachment while leaving the server-owned PTY alive.
    // A replacement connection with the same identity may already be attached
    // during a reconnect race; in that case keep the shared peer-id reference.
    void detach_connection_session(Conn& conn, bool replacement_attached) {
        auto* session = conn.attached_session;
        conn.attached_session = nullptr;
        uint32_t aid = conn.attach_id;
        conn.attach_id = 0;
        conn.spectator = false;
        if (!session || replacement_attached) return;
        // Prefer detach-by-attach_id (precise, multi-attach safe). Fall back to
        // detach-all when the attach_id is unknown (legacy/tests that set
        // attached_session directly without the AttachMsg path).
        if (aid != 0 && sessions_.session_by_attach_id(aid) != nullptr)
            sessions_.detach(aid);
        else
            sessions_.detach_all(session->name);
        log_event("session_transport_detached", session->name + " attach_id=" + std::to_string(aid) + " from " + conn.peer_name);
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 6: Session message handlers (public for tests)
    // ────────────────────────────────────────────────────────────────

    // write_pty_input — write terminal input to a session's PTY stdin.
    // Windows: duplicate the ConPTY input handle and enqueue to a bounded
    //          dedicated writer so blocking WriteFile never stalls the loop.
    // POSIX:  PTY master is nonblocking. Write as much as possible immediately,
    //         then queue the remainder in session.pending_input. The event loop
    //         drains the queue when the PTY becomes writable. Returns false only
    //         on a hard write error or if the pending queue would exceed its
    //         bounded maximum (no silent overflow).
    bool write_pty_input(Session& session, const void* data, size_t len) {
        if (!data || len == 0) return true;
        if (!session.is_valid()) return false;
#ifdef _WIN32
        return enqueue_windows_pty_input(
            session, std::string_view(static_cast<const char*>(data), len));
#else
        if (session.master_fd < 0) return false;

        // Hosted: frame bytes as WMSG_INPUT on the worker socket. The framed
        // queue (worker_tx) is partial-write safe; the same water marks apply.
        if (session.hosted) {
            if (session.worker_died) { session.state = SessionState::Died; return false; }
            if (session.worker_tx.size() + len + 5 > Session::kPtyInputMax) {
                log_event("pty_input_overflow", session.name);
                return false;
            }
            worker_queue_frame(session, worker::WMSG_INPUT, data, len);
            session.input_backpressured =
                session.worker_tx.size() >= Session::kPtyInputHighWater;
            return !session.worker_died;
        }

        // If the child is already backlogged above the high-water mark, do not
        // accept more input now. The event-loop backpressure path will resume
        // reading from the peer once the queue drains below low water.
        if (session.input_backpressured ||
            session.pending_input.size() >= Session::kPtyInputHighWater) {
            if (session.pending_input.size() + len > Session::kPtyInputMax) {
                log_event("pty_input_overflow", session.name);
                return false;
            }
            session.pending_input.append(static_cast<const char*>(data), len);
            session.input_backpressured = true;
            return true;
        }

        // Try to drain any previously queued bytes first, in order.
        if (!session.pending_input.empty()) {
            const ssize_t n = ::write(session.master_fd,
                                      session.pending_input.data(),
                                      session.pending_input.size());
            if (n > 0) {
                session.pending_input.erase(0, static_cast<size_t>(n));
            } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                session.state = SessionState::Died;
                return false;
            }
            // Preserve ordering: if old input remains queued, append new input
            // behind it instead of writing the new bytes ahead of the backlog.
            if (!session.pending_input.empty()) {
                if (session.pending_input.size() + len > Session::kPtyInputMax) {
                    log_event("pty_input_overflow", session.name);
                    return false;
                }
                session.pending_input.append(static_cast<const char*>(data), len);
                if (session.pending_input.size() >= Session::kPtyInputHighWater)
                    session.input_backpressured = true;
                return true;
            }
        }

        // Immediate write of the new bytes; queue whatever the kernel refuses.
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            const ssize_t n = ::write(session.master_fd, p, remaining);
            if (n > 0) {
                p += n;
                remaining -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (session.pending_input.size() + remaining > Session::kPtyInputMax) {
                    log_event("pty_input_overflow", session.name);
                    return false;
                }
                session.pending_input.append(p, remaining);
                if (session.pending_input.size() >= Session::kPtyInputHighWater)
                    session.input_backpressured = true;
                return true;
            }
            session.state = SessionState::Died;
            return false;
        }
        return true;
#endif
    }

#ifndef _WIN32
    // drain_pending_pty_input — called from the event loop when the PTY master
    // is writable. Writes queued input and returns true if the queue dropped
    // below the low-water mark (peer reads may resume).
    bool drain_pending_pty_input(Session& session) {
        if (session.hosted) {
            while (!session.worker_tx.empty()) {
                const ssize_t n = ::write(session.master_fd, session.worker_tx.data(),
                                          session.worker_tx.size());
                if (n > 0) { session.worker_tx.erase(0, static_cast<size_t>(n)); continue; }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                log_event("pty_input_drain_failed", session.name);
                session.worker_died = true;
                return false;
            }
            if (session.worker_tx.size() <= Session::kPtyInputLowWater)
                session.input_backpressured = false;
            return !session.input_backpressured;
        }
        if (session.master_fd < 0 || session.pending_input.empty()) return true;
        while (!session.pending_input.empty()) {
            const ssize_t n = ::write(session.master_fd,
                                      session.pending_input.data(),
                                      session.pending_input.size());
            if (n > 0) {
                session.pending_input.erase(0, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            log_event("pty_input_drain_failed", session.name);
            session.state = SessionState::Died;
            return false;
        }
        if (session.pending_input.size() <= Session::kPtyInputLowWater)
            session.input_backpressured = false;
        return !session.input_backpressured;
    }
#endif

    // Extract join request processing into a reusable method.
    // Used by both handle_inbound_session (promoted conn) and ReadJoinRequest (inline handshake).
    JoinReplyMsg process_join_request(const JoinRequestMsg& jr, const std::string& peer_pubkey) {
        JoinReplyMsg reply;
        {
            std::lock_guard lock(invite_mutex_);
            auto now = std::chrono::steady_clock::now();
            expire_pending_invites_locked(now);
            auto it = std::find_if(pending_invites_.begin(), pending_invites_.end(),
                [&](auto& p) { return p.token == jr.token && p.claimed_by.empty(); });
            if (it == pending_invites_.end()) {
                reply.ok = false;
                reply.error = "invalid or expired token";
            } else {
                it->claimed_by = peer_pubkey;
                reply.ok = true;
                reply.node_name = jr.node_name.empty()
                    ? ("node-" + jr.token.substr(0, 8))
                    : jr.node_name;
                std::ostringstream seeds;
                for (size_t si = 0; si < config_.seeds.size(); ++si) {
                    if (si) seeds << '|';
                    seeds << config_.seeds[si].name << ':' << config_.seeds[si].addr;
                }
                reply.seeds_csv = seeds.str();
                reply.host_pubkey = our_pubkey_;
                reply.host_addr = config_.listen_addr + ":" + std::to_string(config_.listen_port);
                std::ostringstream pkjson;
                pkjson << "[";
                for (size_t si = 0; si < config_.seeds.size(); ++si) {
                    if (si) pkjson << ",";
                    pkjson << "{\"name\":\"" << gossip_json_escape(config_.seeds[si].name)
                            << "\",\"addr\":\"" << gossip_json_escape(config_.seeds[si].addr)
                            << "\",\"pubkey_hex\":\"" << gossip_json_escape(config_.seeds[si].pubkey_hex)
                            << "\"}";
                }
                pkjson << "]";
                reply.peer_pubkeys_json = pkjson.str();
            }
        }
        if (reply.ok && !peer_pubkey.empty()) {
            std::string auth_path = config_.authorized_keys_path;
            std::string dir = auth_path;
            auto slash = dir.rfind('/');
            if (slash == std::string::npos) slash = dir.rfind('\\');
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            if (!bs::mesh::ensure_private_directory(dir)) {
                reply.ok = false;
                reply.error = "host could not prepare authorized_keys dir";
            } else {
                bool already_authorized = false;
                {
                    std::ifstream existing(auth_path);
                    std::string line;
                    while (std::getline(existing, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line == "pubkey " + peer_pubkey || line == peer_pubkey) {
                            already_authorized = true;
                            break;
                        }
                    }
                }
                if (!already_authorized) {
                    std::ofstream af(auth_path, std::ios::app);
                    if (af.is_open()) {
                        af << "pubkey " << peer_pubkey << "\n";
                    } else {
                        reply.ok = false;
                        reply.error = "host could not persist authorization";
                    }
                }
            }
        }
        // ── Bootstrap auto-enroll: the host vouches for the joiner mesh-wide ──
        // On a successful join, the host signs a DirectoryEnrollMsg for the new
        // member and gossips it, so every peer auto-trusts the joiner's key with
        // NO manual copying — the Tailscale auth-key model. Requires the joiner
        // to advertise its reachable listen_addr (the host only ever sees the
        // ephemeral source port, which is undialable); without it we still
        // complete the join but skip the directory propagation.
        if (reply.ok && !peer_pubkey.empty() && !jr.listen_addr.empty()) {
            DirectoryEnrollMsg e = make_directory_enroll(
                reply.node_name, peer_pubkey, jr.listen_addr);
            if (!e.signature.empty()) {
                // Apply locally (already authorized via the join path, but this
                // also seeds it as discovered) and gossip to all peers.
                apply_directory_enroll(e);
                broadcast_enroll(e);
                log_event("join_auto_enrolled", reply.node_name + " " + jr.listen_addr);
            }
        }
        return reply;
    }

    // ── Bootstrap: signed mesh-directory enrollment ──────────────
    // A trusted member vouches for a NEW member so every peer can auto-trust
    // it without manual key copying. Issued by `bs enroll <peer>` on a trusted
    // node, gossiped to all peers, verified + applied on receipt.

    // Sign {name, pubkey, addr} with OUR identity and return the enrollment.
    [[nodiscard]] DirectoryEnrollMsg make_directory_enroll(
        const std::string& name, const std::string& pubkey_hex,
        const std::string& addr) {
        DirectoryEnrollMsg e;
        e.name = name;
        e.pubkey_hex = pubkey_hex;
        e.addr = addr;
        e.issuer_pubkey = our_pubkey_;
        e.issued_at = now_unix_seconds();
        std::string key_path = make_app_paths(home_dir_).key_pem;
        std::ifstream kf(key_path, std::ios::binary);
        std::string key_pem;
        if (kf.is_open()) {
            key_pem.assign(std::istreambuf_iterator<char>(kf), {});
        }
        e.signature = ed25519_sign(key_pem, e.signed_payload());
        return e;
    }

    // Verify an inbound enrollment and, if valid, trust + seed the new member.
    // Returns false (and does NOT mutate state) on signature failure or issuer
    // that we do not already trust. Idempotent: re-applying an already-known
    // member is a no-op.
    bool apply_directory_enroll(const DirectoryEnrollMsg& e) {
        // 1. Issuer must be an explicitly-pinned SEED peer (config `seed ...
        //    pubkey=`), not merely any authorized_keys entry. This limits the
        //    enrollment trust root to operator-provisioned peers and prevents a
        //    single auto-enrolled member from transitively vouching for an
        //    arbitrary new key mesh-wide (Tailscale admin-key model: only the
        //    provisioned trust root can bootstrap members). An unknown signer
        //    cannot bootstrap a member either way.
        bool issuer_is_seed = false;
        for (const auto& s : config_.seeds) {
            if (!s.pubkey_hex.empty() && s.pubkey_hex == e.issuer_pubkey) {
                issuer_is_seed = true;
                break;
            }
        }
        if (!issuer_is_seed) {
            log_event("enroll_rejected_issuer_not_seed", e.name);
            return false;
        }
        // 2. Freshness guard — reject stale enrollments (clock-skew tolerant).
        const uint64_t now = now_unix_seconds();
        if (e.issued_at == 0 || e.issued_at + 86400 < now) {
            log_event("enroll_rejected_stale", e.name);
            return false;
        }
        // 3. Signature must verify over the canonical payload.
        if (!ed25519_verify(e.issuer_pubkey, e.signed_payload(), e.signature)) {
            log_event("enroll_rejected_bad_signature", e.name);
            return false;
        }
        // 4. Reject self-vouching (a member cannot enroll itself).
        if (e.pubkey_hex == e.issuer_pubkey) {
            log_event("enroll_rejected_self", e.name);
            return false;
        }
        // 5. Append new member pubkey to authorized_keys (idempotent).
        std::string auth_path = config_.authorized_keys_path;
        if (!auth_path.empty()) {
            bool already = false;
            {
                std::ifstream ex(auth_path);
                std::string line;
                while (std::getline(ex, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line == "pubkey " + e.pubkey_hex || line == e.pubkey_hex) {
                        already = true;
                        break;
                    }
                }
            }
            if (!already) {
                std::ofstream af(auth_path, std::ios::app);
                if (af.is_open()) {
                    af << "pubkey " << e.pubkey_hex << "\n";
                } else {
                    log_event("enroll_auth_write_failed", e.name);
                    return false;
                }
            }
            // Force the in-memory authorized_keys to reload immediately so the
            // newly-trusted key is honored without waiting for the mtime check.
            authorized_keys_.load_from_file(auth_path);
        }
        // 6. Add the new member as a discovered peer (runtime) so the next
        //    gossip/Hello advertises it; it becomes a durable seed once it
        //    actually connects. Skip if already present.
        bool have = false;
        for (const auto& s : config_.seeds)
            if (s.pubkey_hex == e.pubkey_hex) { have = true; break; }
        if (!have) {
            for (const auto& d : config_.discovered)
                if (d.pubkey_hex == e.pubkey_hex) { have = true; break; }
        }
        if (!have && !e.name.empty() && !e.addr.empty()) {
            PeerEntry pe;
            pe.name = e.name;
            pe.addr = e.addr;
            pe.pubkey_hex = e.pubkey_hex;
            pe.last_seen = now;
            config_.discovered.push_back(std::move(pe));
            gossip_generation_.fetch_add(1, std::memory_order_relaxed);
        }
        log_event("enroll_applied", e.name + " issuer=" + e.issuer_pubkey.substr(0, 12) + "...");
        return true;
    }

    // Broadcast an enrollment to every connected mesh peer so it propagates
    // transitively (each peer re-gossips the directory entry).
    void broadcast_enroll(const DirectoryEnrollMsg& e) {
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET || c.purpose != ConnectionPurpose::Mesh) continue;
            if (c.exec_busy && c.exec_busy->load()) continue;
            (void)enqueue_frame(c, e, CONTROL_STREAM_ID);
        }
    }

    static constexpr auto kInviteTtl = std::chrono::hours(2);

    void open_join_window_locked(std::chrono::steady_clock::time_point now) {
        if (!allow_join_connections_.exchange(true, std::memory_order_relaxed)) {
            join_window_opened_at_ = now;
            log_debug_event("prune_skip_join_window", "state=open");
        }
    }

    void close_join_window_locked(const char* reason) {
        if (allow_join_connections_.exchange(false, std::memory_order_relaxed)) {
            join_window_opened_at_ = {};
            log_debug_event("prune_skip_join_window",
                            std::string("state=closed reason=") + reason);
        }
    }

    void expire_pending_invites_locked(std::chrono::steady_clock::time_point now) {
        for (auto it = pending_invites_.begin(); it != pending_invites_.end();) {
            if (now - it->created_at <= kInviteTtl) {
                ++it;
                continue;
            }
            const auto age_secs = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->created_at).count();
            log_event("invite_expired", "age_secs=" + std::to_string(age_secs));
            ++invite_expired_event_count_;
            it = pending_invites_.erase(it);
        }
    }

    // Close the join window when all invites are claimed/expired or when its
    // hard wall-clock cap elapses, regardless of outstanding invite state.
    // Called from the event loop tick.
    void maybe_close_join_window() {
        std::lock_guard lock(invite_mutex_);
        auto now = std::chrono::steady_clock::now();
        expire_pending_invites_locked(now);
        if (!allow_join_connections_.load(std::memory_order_relaxed)) return;
        const auto max_window = std::chrono::seconds(config_.join_window_max_secs);
        if (join_window_opened_at_ != std::chrono::steady_clock::time_point{} &&
            now - join_window_opened_at_ >= max_window) {
            // Unknown certs cannot redeem a token after the TLS window closes.
            // Drop unclaimed invites so IPC state matches that fact.
            for (auto it = pending_invites_.begin(); it != pending_invites_.end();) {
                if (it->claimed_by.empty()) {
                    log_event("invite_expired", "reason=join_window_hard_cap");
                    ++invite_expired_event_count_;
                    it = pending_invites_.erase(it);
                } else {
                    ++it;
                }
            }
            close_join_window_locked("hard_cap");
            return;
        }
        // Close window if no unclaimed invites remain
        bool any_unclaimed = false;
        for (const auto& p : pending_invites_) {
            if (p.claimed_by.empty()) { any_unclaimed = true; break; }
        }
        if (!any_unclaimed) {
            close_join_window_locked("no_unclaimed_invites");
        }
    }
    void handle_inbound_session(Conn& conn, Message& msg) {
        // JoinRequest — new node onboarding
        if (std::holds_alternative<JoinRequestMsg>(msg)) {
            auto& jr = std::get<JoinRequestMsg>(msg);
            JoinReplyMsg reply = process_join_request(jr, conn.peer_pubkey);
            (void)enqueue_frame(conn, reply, CONTROL_STREAM_ID);
            return;
        }
        // AttachMsg — peer wants to attach to one of our sessions
        if (std::holds_alternative<AttachMsg>(msg)) {
            auto& a = std::get<AttachMsg>(msg);

            // Multi-hop routing (v2.1): routing="target:ttl" forwards the
            // attach through the mesh when this node is not the destination.
            if (!a.routing.empty()) {
                auto sep = a.routing.find(':');
                std::string hop_target = (sep != std::string::npos)
                    ? a.routing.substr(0, sep) : a.routing;
                int ttl = (sep != std::string::npos)
                    ? std::atoi(a.routing.substr(sep + 1).c_str()) : 0;

                // Forward if we're not the target and TTL allows one more hop
                if (!peer_name_eq(hop_target, config_.node_name) && ttl > 0) {
                    Conn* mesh = nullptr;
                    for (auto& mc : conns_) {
                        if (is_live_mesh_transport_for(mc, hop_target)) {
                            mesh = &mc; break;
                        }
                    }
                    if (mesh && mesh->ssl) {
                        AttachMsg forward = a;
                        forward.routing = hop_target + ":" + std::to_string(ttl - 1);
                        try {
                            write_frame(mesh->ssl.get(), forward, CONTROL_STREAM_ID);
                            log_event("session_attach_forwarded_to_hop",
                                a.session_name + " -> " + hop_target + " ttl=" + std::to_string(ttl));
                        } catch (...) {
                            log_event("session_attach_hop_forward_failed", hop_target);
                        }
                    } else {
                        log_event("session_attach_hop_unreachable", hop_target);
                    }
                    return;
                }
                // If TTL is 0 or we are the target, fall through to local attach
            }

            log_event("session_attach_request",
                      a.session_name + " from " + conn.peer_name);

            const ResolvedSessionCommand shell_cmd = resolve_session_command(
                config_, a.session_name, a.command);
            uint16_t eff_c = 0, eff_r = 0;
            // 2.0.8 MoA fix: a second AttachMsg on an already-attached conn
            // would overwrite conn.attach_id and ORPHAN the previous
            // Session::Attachment (leaks geometry into min-wins, blocks the
            // last-detach signal). Detach the old attachment first.
            if (conn.attach_id != 0 || conn.attached_session != nullptr) {
                log_event("session_reattach_detaching_previous",
                          a.session_name + " old_attach_id=" + std::to_string(conn.attach_id));
                detach_connection_session(conn, false);
            }
            uint32_t aid = sessions_.attach_connection(a.session_name,
                                       shell_cmd,
                                       a.cols, a.rows, a.term,
                                       conn.peer_pubkey,
                                       a.client_instance_id, a.spectator,
                                       eff_c, eff_r);
            auto* s = (aid != 0) ? sessions_.get(a.session_name) : nullptr;
            if (s) {
                // Record the detach-signal request (v2.1) so the server can
                // signal the child when the last peer detaches.
                if (!a.signal_on_detach.empty()) s->detach_signal = a.signal_on_detach;
                conn.attached_session = s;
                conn.attach_id = aid;
                conn.spectator = a.spectator;

                // 2.0.8: AttachAck reports the effective (min-wins) geometry so
                // the client can align its PTY view to the narrowest pane.
                AttachAckMsg ack;
                ack.attach_id = aid;
                ack.session_name = a.session_name;
                ack.cols = eff_c; ack.rows = eff_r;
                (void)enqueue_frame(conn, ack, CONTROL_STREAM_ID);

                // Send scrollback to reattaching peer
                auto lines = s->scrollback.read_last_lines(
                    static_cast<size_t>(config_.scrollback_lines));
                if (!lines.empty()) {
                    ScrollbackMsg sb;
                    sb.data = std::move(lines);
                    sb.total_lines = 0; // best-effort
                    sb.chunk_index = 0;
                    (void)enqueue_frame(conn, sb, 0);
                }
                log_event("session_attached",
                          a.session_name + " from " + conn.peer_name
                          + " attach_id=" + std::to_string(aid));
            } else {
                log_event("session_attach_failed",
                          a.session_name + " from " + conn.peer_name);
            }
            return;
        }

        // KeystrokeMsg — peer typed something; forward to PTY
        if (std::holds_alternative<KeystrokeMsg>(msg)) {
            auto& ks = std::get<KeystrokeMsg>(msg);
            // 2.0.8: spectators are read-only — reject input injection.
            if (conn.spectator) {
                log_event("pty_input_rejected_spectator", conn.attached_session
                          ? conn.attached_session->name : "?");
                return;
            }
            if (conn.attached_session && conn.attached_session->is_valid()) {
                if (!write_pty_input(*conn.attached_session,
                                     ks.data.data(), ks.data.size())) {
                    log_event("pty_input_rejected", conn.attached_session->name);
                    conn.close_requested = true;
                }
            }
            return;
        }

        // CuaRequestMsg — computer-use action (full dispatch lands in P5).
        // P1 invariant: spectators may never drive CUA.
        if (std::holds_alternative<CuaRequestMsg>(msg)) {
            auto& req = std::get<CuaRequestMsg>(msg);
            if (conn.spectator) {
                log_event("cua_rejected_spectator", conn.attached_session
                          ? conn.attached_session->name : "?");
                CuaResponseMsg resp;
                resp.request_id = req.request_id;
                resp.status = 1;
                resp.error = "spectator cannot drive computer use";
                (void)enqueue_frame(conn, resp, CONTROL_STREAM_ID);
                return;
            }
            // Non-spectator: dispatch to platform CUA backend (2.0.8 P5).
            CuaResponseMsg resp = cua_execute(req, home_dir_);
            resp.request_id = req.request_id;
            // Screenshots can use the u32 frame format. enqueue_frame selects
            // it from the peer capability and never blocks the event loop.
            if (!enqueue_frame(conn, resp, CONTROL_STREAM_ID))
                log_event("cua_response_queue_failed", conn.peer_name);
            return;
        }

        // CuaVideoCaptureMsg — remote video capture (2.0.12)
        // Same P1 invariant as CuaRequestMsg: spectators may not capture.
        if (std::holds_alternative<CuaVideoCaptureMsg>(msg)) {
            auto& req = std::get<CuaVideoCaptureMsg>(msg);
            if (conn.spectator) {
                log_event("cua_video_rejected_spectator", conn.attached_session
                          ? conn.attached_session->name : "?");
                CuaVideoCaptureResultMsg resp;
                resp.request_id = req.request_id;
                resp.status = 1;
                resp.error = "spectator cannot drive computer use";
                (void)enqueue_frame(conn, resp, CONTROL_STREAM_ID);
                return;
            }
            CuaVideoCaptureResultMsg resp = video_capture_execute(req);
            resp.request_id = req.request_id;
            (void)enqueue_frame(conn, resp, CONTROL_STREAM_ID);
            return;
        }

        // ResizeMsg — peer resized their terminal
        if (std::holds_alternative<ResizeMsg>(msg)) {
            auto& r = std::get<ResizeMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
                // 2.0.8: update this attachment's stored geometry and re-apply
                // MIN-wins so the PTY tracks the narrowest pane (not the last
                // resizer). Spectators may resize too — it only shrinks/grows
                // the shared effective geometry, never injects input.
                // Legacy fallback: when attach_id is 0 (old test path, conn
                // wired directly without the AttachMsg handler), resize the
                // PTY directly.
                if (conn.attach_id != 0)
                    sessions_.set_attachment_geometry(conn.attach_id, r.cols, r.rows);
                else {
                    // Legacy fallback: resize PTY directly (old test path without attach_id).
#ifndef _WIN32
                    if (conn.attached_session->master_fd >= 0) {
                        if (conn.attached_session->hosted) {
                            uint8_t p[4];
                            worker::write_u16be(p, r.cols);
                            worker::write_u16be(p + 2, r.rows);
                            worker_queue_frame(*conn.attached_session,
                                               worker::WMSG_RESIZE, p, 4);
                        } else {
                            (void)resize_pty(static_cast<intptr_t>(conn.attached_session->master_fd), r.cols, r.rows);
                        }
                    }
#else
                    if (conn.attached_session->hpcon)
                        (void)resize_pty(reinterpret_cast<intptr_t>(conn.attached_session->hpcon), r.cols, r.rows);
#endif
                }
            }
            return;
        }

        // DetachMsg — peer wants to detach from session
        if (std::holds_alternative<DetachMsg>(msg)) {
            if (conn.attached_session) {
                detach_connection_session(conn, has_replacement_transport(conn));
                log_event("session_detached", "from " + conn.peer_name);
            }
            return;
        }

        // SignalMsg — send signal to child process
        if (std::holds_alternative<SignalMsg>(msg)) {
            auto& sig = std::get<SignalMsg>(msg);
            // 2.0.8: spectators are read-only — process control (SIGINT/kill/
            // Restart with client-supplied command) is a write capability.
            if (conn.spectator) {
                log_event("signal_rejected_spectator", conn.attached_session
                    ? conn.attached_session->name : std::string{});
                return;
            }
            // A5: record foreign-owner kill attempts. The kill itself is
            // allowed (fleet multi-peer workflows may legitimately share a
            // session), but we surface ownership so watchdogs that reap
            // sessions they did not spawn are visible in the structured log.
            if (sig.signal == SignalMsg::SignalType::Kill && conn.attached_session) {
                const auto& owner = conn.attached_session->owner_pubkey;
                if (!owner.empty() && owner != conn.peer_pubkey) {
                    log_event("session_kill_foreign",
                              conn.attached_session->name +
                              " by " + conn.peer_name +
                              " (owner " + owner.substr(0, 12) + "...)");
                }
            }
            if (conn.attached_session && conn.attached_session->is_valid()) {
#ifdef _WIN32
                if (conn.attached_session->child_pid) {
                    DWORD ctrl_event = 0;
                    switch (sig.signal) {
                        case SignalMsg::SignalType::CtrlC:
                            ctrl_event = CTRL_C_EVENT; break;
                        case SignalMsg::SignalType::CtrlZ:
                            ctrl_event = CTRL_BREAK_EVENT; break;
                        case SignalMsg::SignalType::Kill:
                            conn.attached_session->kill_tree();  // A3: whole tree
                            TerminateProcess(conn.attached_session->child_pid, 1); break;
                        default: break;
                    }
                    if (ctrl_event)
                        GenerateConsoleCtrlEvent(ctrl_event,
                            GetProcessId(conn.attached_session->child_pid));
                }
#else
                if (conn.attached_session->child_pid > 0) {
                    auto& sm = std::get<SignalMsg>(msg);
                    if (sm.signal == SignalMsg::SignalType::Kill) {
                        // Terminate the shell AND every process in its group
                        // (the forkpty session-leader is the process-group id),
                        // so background jobs cannot outlive the session.
                        kill(-conn.attached_session->child_pid, SIGHUP);
                        kill(-conn.attached_session->child_pid, SIGTERM);
                    } else {
                        int s = (sm.signal == SignalMsg::SignalType::CtrlC) ? SIGINT :
                                (sm.signal == SignalMsg::SignalType::CtrlZ) ? SIGTSTP :
                                SIGQUIT;
                        kill(conn.attached_session->child_pid, s);
                    }
                }
#endif
            }
            // Restart signal: kill + respawn process in same session
            if (sig.signal == SignalMsg::SignalType::Restart) {
                if (conn.attached_session && conn.attached_session->is_valid()) {
                    auto* sess = conn.attached_session;
                    std::string cmd = sess->command;
                    if (!sig.process.empty()) {
                        cmd = prepare_session_command(
                            {sig.process, SessionCommandSource::ClientOverride});
                    }
                    log_event("session_restart", redact_secrets(cmd) + " on " + sess->name);
                    // Kill the old child and release every PTY resource before
                    // installing the replacement handles.
#ifdef _WIN32
                    if (sess->child_pid) {
                        sess->kill_tree();               // A3: job close kills the tree
                        TerminateProcess(sess->child_pid, 1);
                        WaitForSingleObject(sess->child_pid, 3000);
                        CloseHandle(sess->child_pid);
                        sess->child_pid = nullptr;
                    }
                    if (sess->master_fd) {
                        CloseHandle(sess->master_fd);
                        sess->master_fd = nullptr;
                    }
                    if (sess->write_handle) {
                        CloseHandle(sess->write_handle);
                        sess->write_handle = nullptr;
                    }
                    if (sess->hpcon) {
                        ClosePseudoConsole(sess->hpcon);
                        sess->hpcon = nullptr;
                    }
#else
                    if (sess->hosted) {
                        // Hosted: the shell is the worker's child — ask the
                        // worker to shut down (it group-kills the shell), then
                        // drop the socket. waitpid would ECHILD here.
                        if (sess->master_fd >= 0)
                            (void)worker::worker_send(sess->master_fd,
                                                      worker::WMSG_SHUTDOWN);
                        terminate_worker_and_reap(sess->worker_pid);
                        sess->child_pid = -1;
                        sess->worker_pid = -1;
                        sess->hosted = false;
                        sess->worker_died = false;
                        sess->worker_rx.clear();
                        sess->worker_tx.clear();
                    } else if (sess->child_pid > 0) {
                        kill(sess->child_pid, SIGTERM);
                        int status = 0;
                        for (int i = 0; i < 30; ++i) {
                            if (waitpid(sess->child_pid, &status, WNOHANG) == sess->child_pid) break;
                            usleep(100000);
                        }
                        if (waitpid(sess->child_pid, &status, WNOHANG) != sess->child_pid) {
                            kill(sess->child_pid, SIGKILL);
                            waitpid(sess->child_pid, &status, 0);
                        }
                        sess->child_pid = -1;
                    }
                    if (sess->master_fd >= 0) {
                        close(sess->master_fd);
                        sess->master_fd = -1;
                    }
                    sess->pending_input.clear();
                    sess->input_backpressured = false;
#endif
                    // Spawn the replacement. Session storage remains in place so
                    // attached transports retain a stable pointer.
                    auto new_sess = sessions_.spawn_session_runtime(sess->name, cmd, 80, 24,
                                                   "xterm-256color");
                    if (new_sess) {
                        sess->master_fd = new_sess->master_fd;
                        sess->child_pid = new_sess->child_pid;
#ifdef _WIN32
                        sess->write_handle = new_sess->write_handle;
                        sess->hpcon = new_sess->hpcon;
                        new_sess->master_fd = nullptr;  // prevent double-close
                        new_sess->child_pid = nullptr;
                        new_sess->write_handle = nullptr;
                        new_sess->hpcon = nullptr;
#else
                        sess->hosted = new_sess->hosted;
                        sess->worker_pid = new_sess->worker_pid;
                        sess->worker_died = false;
                        new_sess->master_fd = -1;
                        new_sess->child_pid = -1;
                        new_sess->hosted = false;      // prevent worker kill on dtor
                        new_sess->worker_pid = -1;
#endif
                        sess->command = new_sess->command;
                        sess->generation = new_sess->generation;
                        sess->last_output_at = new_sess->last_output_at;
                        sess->state = SessionState::Attached;
                        log_event("session_restart_ok", redact_secrets(cmd) + " respawned ok");
                    } else {
                        log_event("session_restart_failed", "cannot spawn: " + redact_secrets(cmd));
                        sess->state = SessionState::Died;
                    }
                }
            }
            return;
        }

        // ClipboardMsg — write bracketed paste to PTY
        // Also echo hash back to confirm receipt
        if (std::holds_alternative<ClipboardMsg>(msg)) {
            auto& cb = std::get<ClipboardMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
                // Write bracketed paste: ESC[200~ <data> ESC[201~
                std::string paste = "\x1b[200~" + cb.text + "\x1b[201~";
                if (!write_pty_input(*conn.attached_session,
                                     paste.data(), paste.size())) {
                    log_event("pty_input_rejected", conn.attached_session->name);
                    conn.close_requested = true;
                }
                // Echo hash back
                if (!cb.hash.empty()) {
                    ClipboardEchoMsg echo;
                    echo.hash = cb.hash;
                    (void)enqueue_frame(conn, echo, 0);
                }
            }
            return;
        }

        // ── 2.0.8 P4: Conversation messages ─────────────────────
        if (std::holds_alternative<ConversationAppendMsg>(msg)) {
            auto& ca = std::get<ConversationAppendMsg>(msg);
            // 2.0.8 MoA fix: the store is the ONLY seq authority. Honoring a
            // peer-supplied seq lets a peer hide messages from since_seq
            // queries (low value) or poison ordering (huge value).
            {
                std::lock_guard lock(conversations_mutex_);
                ca.seq = next_conv_seq_++;
            }
            if (ca.ts == 0) {
                using namespace std::chrono;
                ca.ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            }
            {
                std::lock_guard lock(conversations_mutex_);
                conversations_[ca.conv_id].push_back(ca);
                // 2.0.8 MoA fix: bound the store — cap messages per
                // conversation (drop oldest) and distinct conv_ids.
                auto& vec = conversations_[ca.conv_id];
                static constexpr size_t kMaxMsgsPerConv = 10000;
                if (vec.size() > kMaxMsgsPerConv)
                    vec.erase(vec.begin(), vec.begin() + (vec.size() - kMaxMsgsPerConv));
                static constexpr size_t kMaxConvs = 1024;
                if (conversations_.size() > kMaxConvs) {
                    // evict oldest-keyed conversation (map order is arbitrary;
                    // eviction is a backstop, not an LRU)
                    conversations_.erase(conversations_.begin());
                }
            }
            log_event("conversation_append", ca.conv_id + " seq=" + std::to_string(ca.seq));
            return;
        }
        if (std::holds_alternative<ConversationQueryMsg>(msg)) {
            auto& cq = std::get<ConversationQueryMsg>(msg);
            ConversationBatchMsg batch;
            batch.conv_id = cq.conv_id;
            {
                std::lock_guard lock(conversations_mutex_);
                auto it = conversations_.find(cq.conv_id);
                if (it != conversations_.end()) {
                    for (auto& m : it->second)
                        if (m.seq > cq.since_seq) batch.messages.push_back(m);
                }
            }
            (void)enqueue_frame(conn, batch, CONTROL_STREAM_ID);
            return;
        }
    }

    // 2. handle_outbound_session — messages from our local sessions
    //    destined for a local client (shell_peer CLI mode)
    void handle_outbound_session(Conn& conn, Message& msg) {
        // OutputMsg — write to local stdout (shell_peer display)
        if (std::holds_alternative<OutputMsg>(msg)) {
            auto& o = std::get<OutputMsg>(msg);
            fwrite(o.data.data(), 1, o.data.size(), stdout);
            fflush(stdout);
            return;
        }

        // ClipboardMsg — write to local clipboard
        if (std::holds_alternative<ClipboardMsg>(msg)) {
            auto& cb = std::get<ClipboardMsg>(msg);
#ifdef _WIN32
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                size_t wsize = MultiByteToWideChar(CP_UTF8, 0,
                    cb.text.c_str(), static_cast<int>(cb.text.size()),
                    nullptr, 0);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE,
                    (wsize + 1) * sizeof(WCHAR));
                if (hMem) {
                    auto* wstr = static_cast<WCHAR*>(GlobalLock(hMem));
                    if (wstr) {
                        MultiByteToWideChar(CP_UTF8, 0,
                            cb.text.c_str(), static_cast<int>(cb.text.size()),
                            wstr, static_cast<int>(wsize));
                        wstr[wsize] = L'\0';
                        GlobalUnlock(hMem);
                    }
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
            }
#endif
            return;
        }

        // ExitCodeMsg / SessionDiedMsg — clear remote session tracking
        if (std::holds_alternative<ExitCodeMsg>(msg) ||
            std::holds_alternative<SessionDiedMsg>(msg)) {
            conn.remote_session.clear();
            return;
        }
    }

    // 3. common_message_handler — protocol-level messages
    void common_message_handler(Conn& conn, Message& msg) {
        // PingMsg → PongMsg (already handled in dispatch, but safe to be here)
        if (std::holds_alternative<PingMsg>(msg)) {
            (void)enqueue_frame(conn, PongMsg{}, CONTROL_STREAM_ID);
            return;
        }

        // PongMsg → update last_pong (already handled, but safe)
        if (std::holds_alternative<PongMsg>(msg)) {
            const auto now = std::chrono::steady_clock::now();
            conn.last_pong = now;
            if (conn.ping_sent_at != std::chrono::steady_clock::time_point{}) {
                auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - conn.ping_sent_at);
                if (rtt > std::chrono::milliseconds(0))
                    conn.pong_rtt_ms = rtt;
            }
            return;
        }

        // ScrollbackMsg — write to local stdout (for shell_peer)
        if (std::holds_alternative<ScrollbackMsg>(msg)) {
            auto& sb = std::get<ScrollbackMsg>(msg);
            fwrite(sb.data.data(), 1, sb.data.size(), stdout);
            fflush(stdout);
            (void)enqueue_frame(conn, ScrollbackAckMsg{}, 0);
            return;
        }

        // ImageDataMsg / ImageFrameMsg — stub, render later (Phase 8)
        if (std::holds_alternative<ImageDataMsg>(msg) ||
            std::holds_alternative<ImageFrameMsg>(msg)) {
            // Phase 8 will handle rendering
            return;
        }

        // SessionListMsg — format and display
        if (std::holds_alternative<SessionListMsg>(msg)) {
            auto& sl = std::get<SessionListMsg>(msg);
            printf("=== Sessions ===\n");
            for (auto& si : sl.sessions) {
                printf("  %s  [%s]  uptime=%llus\n",
                       si.name.c_str(), si.state.c_str(),
                       (unsigned long long)si.uptime_seconds);
            }
            fflush(stdout);
            return;
        }
    }

    // 4. pty_output_poller — poll PTY output for each attached session
    void pty_output_poller() {
        // Drain every live PTY, including detached sessions, so the child never
        // blocks on a full PTY buffer and select() cannot spin on unread output.
        for (const auto& info : sessions_.list()) {
            auto* s = sessions_.get(info.name);
            if (!s || !s->is_pollable()) continue;

            auto fanout = [&](const auto& message) {
                for (auto& target : conns_) {
                    if (target.attached_session != s || target.sock_fd == INVALID_SOCKET ||
                        !target.ssl) {
                        continue;
                    }
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    if (enqueue_frame(target, message, 0)) continue;

                    // Output can be retried from its bounded logical queue.
                    if constexpr (std::is_same_v<std::decay_t<decltype(message)>, OutputMsg>) {
                        Conn::QueuedOutput qo;
                        qo.data = message.data;
                        qo.render_markdown = message.render_markdown;
                        if (target.output_queue.size() >= Conn::kOutputQueueHighWater) {
                            auto& oldest = target.output_queue.front();
                            target.output_dropped_bytes += oldest.data.size();
                            target.output_gap_pending = true;
                            target.output_queue.pop_front();
                        }
                        target.output_queue.push_back(std::move(qo));
                    } else {
                        log_event("control_frame_dropped", target.peer_name);
                    }
                }
            };

            // v1.7.1 fix: previously an early `continue` here (when no bytes
            // were pending) skipped the child-exit check further below in
            // the SAME pass, occasionally letting the final chunk of output
            // and the SessionDiedMsg race across two different daemons/
            // connections for fast one-shot commands (e.g. `hostname`),
            // observed as an empty capture with exit 0. Now we always fall
            // through to the exit check even when there's nothing to read.
            // Drain and coalesce the PTY burst in one pass. Full-screen TUIs
            // emit many small cursor-addressing writes; forwarding only 4 KiB
            // per event-loop tick visibly tears the screen apart.
            std::string buf;
#ifndef _WIN32
            if (s->hosted) {
                // Worker-hosted: the socket carries framed protocol messages.
                // SCROLLBACK frames are historical (adoption replay) — they go
                // to the ring only, never fan out as fresh output.
                auto pump = pump_hosted_session(*s);
                if (!pump.scrollback.empty()) {
                    s->scrollback.write(std::string_view(pump.scrollback));
                    s->touch_output();
                }
                buf = std::move(pump.output);
            } else
#endif
            {
                buf = read_available_pty_output(*s);
            }

            if (buf.empty()) goto check_child_exit;

            // Write to ring buffer
            s->scrollback.write(std::string_view(buf));
            s->touch_output();

            // OSC 52 scan
            {
            auto osc = scan_osc52(buf);
            if (osc.clipboard_text && !osc.clipboard_text->empty()) {
                ClipboardMsg cb;
                cb.text = *osc.clipboard_text;
                cb.hash = sha256_hex(cb.text);
                fanout(cb);
            }

            // Send OutputMsg with cleaned text
            if (!osc.cleaned_text.empty()) {
                OutputMsg om;
                om.data = std::move(osc.cleaned_text);
                // Set render_markdown flag based on heuristic or config override
                if (config_.render_hint == "markdown") om.render_markdown = true;
                else if (config_.render_hint != "raw")
                    om.render_markdown = looks_like_markdown(om.data);
                fanout(om);
            }
            } // end osc scope

            check_child_exit:
            // Check child exit
#ifdef _WIN32
            if (s->child_pid &&
                WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                // v2.0.1: ConPTY/conhost flushes command text AFTER process exit.
                // Force-close the pseudoconsole to push remaining bytes into the
                // pipe, then drain until quiet. Log fanout failures (was silent).
                auto fanout_out = [&](std::string data) {
                    if (data.empty()) return;
                    OutputMsg lom;
                    lom.data = std::move(data);
                    if (config_.render_hint == "markdown") lom.render_markdown = true;
                    else if (config_.render_hint != "raw")
                        lom.render_markdown = looks_like_markdown(lom.data);
                    int targets = 0;
                    for (auto& target : conns_) {
                        if (target.sock_fd == INVALID_SOCKET || !target.ssl) continue;
                        if (target.exec_busy && target.exec_busy->load()) continue;
                        // Prefer attached_session match; also any DirectSession
                        // peer (one-shot shell uses a dedicated TLS conn).
                        const bool match =
                            target.attached_session == s ||
                            target.purpose == ConnectionPurpose::DirectSession;
                        if (!match) continue;
                        if (enqueue_frame(target, lom, CONTROL_STREAM_ID)) {
                            ++targets;
                        } else {
                            log_event("fanout_output_queue_failed", s->name);
                        }
                    }
                    log_event("fanout_output",
                              s->name + " targets=" + std::to_string(targets) +
                                  " bytes=" + std::to_string(lom.data.size()));
                };
                auto drain_pipe = [&](int max_ms) {
                    int empty_streak = 0;
                    const auto start = std::chrono::steady_clock::now();
                    size_t total = 0;
                    std::string acc;
                    while (empty_streak < 12) {
                        const auto elapsed =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
                        if (elapsed >= max_ms) break;
                        DWORD avail = 0;
                        if (!s->master_fd ||
                            !PeekNamedPipe(s->master_fd, nullptr, 0, nullptr, &avail, nullptr) ||
                            avail == 0) {
                            Sleep(25);
                            ++empty_streak;
                            continue;
                        }
                        empty_streak = 0;
                        std::string late;
                        late.resize(avail);
                        DWORD got = 0;
                        if (ReadFile(s->master_fd, late.data(), avail, &got, nullptr) && got > 0) {
                            late.resize(got);
                            total += got;
                            s->scrollback.write(std::string_view(late));
                            acc += late;
                        }
                    }
                    // One coalesced OutputMsg after drain (plain text).
                    if (!acc.empty()) {
                        auto osc = scan_osc52(acc);
                        fanout_out(strip_ansi_escapes(osc.cleaned_text));
                    }
                    return total;
                };

                // First pass: drain whatever is already available.
                size_t drained = drain_pipe(250);
                // Closing the pseudo console forces conhost to flush remaining
                // child output into our pipe (Microsoft ConPTY behavior).
                if (s->hpcon) {
                    ClosePseudoConsole(s->hpcon);
                    s->hpcon = nullptr;
                }
                drained += drain_pipe(500);
                // Note: do NOT re-broadcast full session scrollback here — resurrected
                // Session objects retain prior one-shot text and would pollute capture.
                if (drained > 0) {
                    log_event("pty_death_drain",
                              s->name + " bytes=" + std::to_string(drained));
                }

                DWORD exit_code = 0;
                GetExitCodeProcess(s->child_pid, &exit_code);
                sessions_.record_finished(*s, static_cast<int32_t>(exit_code), "died");
                // A3: reap the whole tree BEFORE nulling child_pid. The ConPTY
                // root exited, but grandchildren (nested cmd / background jobs)
                // may still be alive — on schtasks-SYSTEM hosts the Job Object
                // no-ops, so kill_tree()'s taskkill /T /PID fallback is the only
                // thing that reaches them. Must run while child_pid is valid.
                s->kill_tree();
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
                s->release_exited_runtime();
                s->state = SessionState::Died;

                SessionDiedMsg sdm;
                sdm.exit_code = static_cast<int32_t>(exit_code);
                sdm.signal_num = 0;
                for (auto& target : conns_) {
                    if (target.sock_fd == INVALID_SOCKET || !target.ssl) continue;
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    // 2.0.9 fix: deliver SessionDied to both attached sessions
                    // AND DirectSession connections (non-interactive -x mode).
                    // Previously only matched attached_session, so -x callers
                    // spun forever waiting for a death notice that never arrived
                    // (RCA 2026-07-23: Start-Process descendants → -x hangs).
                    const bool match =
                        target.attached_session == s ||
                        target.purpose == ConnectionPurpose::DirectSession;
                    if (!match) continue;
                    (void)enqueue_frame(target, sdm, CONTROL_STREAM_ID);
                }
                log_event("session_died", s->name + " exit_code=" + std::to_string(exit_code));
            }
#else
            if (s->hosted) {
                // Hosted death comes from the worker protocol (WMSG_DIED) or
                // socket EOF — never waitpid (the shell is the worker's child).
                if (s->worker_died) {
                    SessionDiedMsg sdm;
                    sdm.exit_code = s->worker_exit_code >= 0 ? s->worker_exit_code : -1;
                    sdm.signal_num = s->worker_signal_num;
                    sessions_.record_finished(*s, sdm.exit_code, "died");
                    {
                        double dur_s = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - s->created_at).count();
                        log_event("command_complete",
                                  "session=" + s->name +
                                  (s->parent_id.empty() ? "" : " parent=" + s->parent_id) +
                                  " exit=" + std::to_string(sdm.exit_code) +
                                  (sdm.signal_num ? " signal=" + std::to_string(sdm.signal_num) : "") +
                                  " dur_s=" + std::to_string(static_cast<int>(dur_s * 10) / 10.0) +
                                  " kind=" + session_kind_str(s->kind));
                    }
                    s->child_pid = -1;
                    s->release_exited_runtime();
                    s->state = SessionState::Died;
                    fanout(sdm);
                }
            } else if (s->child_pid > 0) {
                int status = 0;
                pid_t result = waitpid(s->child_pid, &status, WNOHANG);
                if (result == s->child_pid) {
                    // The shell exited (`exit` / Ctrl-D). Kill the process group
                    // so background jobs it left behind do not outlive the
                    // session as orphans holding the PTY open.
                    if (s->child_pid > 0) {
                        ::kill(-s->child_pid, SIGHUP);
                        ::kill(-s->child_pid, SIGTERM);
                    }
                    SessionDiedMsg sdm;
                    if (WIFEXITED(status)) {
                        sdm.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        sdm.signal_num = WTERMSIG(status);
                    }
                    sessions_.record_finished(*s, sdm.exit_code, "died");
                    // C1: structured per-command record (exit + duration +
                    // lineage). parent_id is empty for primary sessions.
                    {
                        double dur_s = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - s->created_at).count();
                        log_event("command_complete",
                                  "session=" + s->name +
                                  (s->parent_id.empty() ? "" : " parent=" + s->parent_id) +
                                  " exit=" + std::to_string(sdm.exit_code) +
                                  (sdm.signal_num ? " signal=" + std::to_string(sdm.signal_num) : "") +
                                  " dur_s=" + std::to_string(static_cast<int>(dur_s * 10) / 10.0) +
                                  " kind=" + session_kind_str(s->kind));
                    }
                    s->child_pid = -1;
                    s->release_exited_runtime();
                    s->state = SessionState::Died;
                    fanout(sdm);
                }
            }
#endif
        }
    }

    // Non-blocking control/output TX: encode + queue + best-effort flush.
    // Returns false if the frame was dropped (overflow / no transport).
    bool enqueue_frame(Conn& c, const Message& msg, uint16_t stream_id = CONTROL_STREAM_ID) {
        if (c.sock_fd == INVALID_SOCKET || !c.ssl) return false;
        if (c.exec_busy && c.exec_busy->load()) return false;
        std::vector<uint8_t> frame;
        try {
            const bool allow_large = version_has_cap(c.remote_version, kCapFrm2);
            frame = encode(msg, stream_id, allow_large);
        } catch (...) {
            return false;
        }
        if (c.tx_queue_bytes + frame.size() > Conn::kTxQueueHighWaterBytes) {
            log_event("tx_queue_overflow", c.peer_name + " bytes=" +
                      std::to_string(c.tx_queue_bytes));
            return false;
        }
        c.tx_queue_bytes += frame.size();
        c.tx_queue.push_back(Conn::QueuedTxFrame{std::move(frame), 0});
        c.want_write = true;
        flush_tx_queue(c);
        return true;
    }

    // Drain encoded frames with non-blocking SSL_write. Never sleeps.
    void flush_tx_queue(Conn& c) {
        if (c.sock_fd == INVALID_SOCKET || !c.ssl) {
            c.tx_queue.clear();
            c.tx_queue_bytes = 0;
            c.want_write = false;
            return;
        }
        if (c.exec_busy && c.exec_busy->load()) return;
        while (!c.tx_queue.empty()) {
            auto& fr = c.tx_queue.front();
            if (fr.offset >= fr.data.size()) {
                c.tx_queue.pop_front();
                continue;
            }
            size_t remain = fr.data.size() - fr.offset;
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_write_ex(c.ssl.get(), fr.data.data() + fr.offset, remain, &n);
            if (ret > 0 && n > 0) {
                fr.offset += n;
                if (c.tx_queue_bytes >= n) c.tx_queue_bytes -= n;
                else c.tx_queue_bytes = 0;
                c.bytes_out += n;
                if (fr.offset >= fr.data.size()) c.tx_queue.pop_front();
                continue;
            }
            int err = SSL_get_error(c.ssl.get(), ret);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                c.want_write = true;
                return;
            }
            log_event("tx_queue_write_fail", c.peer_name + " ssl_err=" + std::to_string(err));
            c.tx_queue.clear();
            c.tx_queue_bytes = 0;
            c.want_write = false;
            close_conn(c);
            return;
        }
        c.want_write = false;
    }

    // 4b. drain_output_queues — promote logical OutputMsg queue into the
    // encoded tx_queue, then flush. Also emits pending OutputGap notices.
    void drain_output_queues() {
        for (auto& target : conns_) {
            if (target.output_queue.empty() && !target.output_gap_pending
                && target.tx_queue.empty()) continue;
            if (target.sock_fd == INVALID_SOCKET || !target.ssl) {
                target.output_queue.clear();
                target.output_gap_pending = false;
                target.tx_queue.clear();
                target.tx_queue_bytes = 0;
                target.want_write = false;
                continue;
            }
            if (target.exec_busy && target.exec_busy->load()) continue;
            if (target.output_gap_pending) {
                OutputGapMsg gap;
                gap.dropped_bytes = target.output_dropped_bytes;
                if (enqueue_frame(target, gap, 0)) {
                    target.output_gap_pending = false;
                    target.output_dropped_bytes = 0;
                }
            }
            size_t promoted = 0;
            while (!target.output_queue.empty() && promoted < 8) {
                auto& qo = target.output_queue.front();
                OutputMsg om;
                om.data = qo.data;
                om.render_markdown = qo.render_markdown;
                if (!enqueue_frame(target, om, 0)) break;
                target.output_queue.pop_front();
                ++promoted;
            }
            while (target.output_queue.size() > Conn::kOutputQueueHighWater) {
                target.output_dropped_bytes += target.output_queue.front().data.size();
                target.output_gap_pending = true;
                target.output_queue.pop_front();
            }
            if (!target.tx_queue.empty()) flush_tx_queue(target);
        }
    }

private:

    void check_conn_read(int conn_idx) {
        try {
            if (static_cast<size_t>(conn_idx) >= conns_.size()) return;
            Conn& c = conns_[static_cast<size_t>(conn_idx)];
            if (c.sock_fd == INVALID_SOCKET) return;
            if (socket_peer_half_closed(c.sock_fd)) {
                close_conn(c);
                return;
            }

            // Drain only bytes OpenSSL can provide now. Blocking read_frame() can
            // freeze the single-threaded daemon when a TLS record contains a partial
            // next protocol frame. rx_buffer preserves that partial frame until the
            // next readability event.
#ifdef _WIN32
            u_long nonblocking = 1;
            ioctlsocket(c.sock_fd, FIONBIO, &nonblocking);
#else
            const int original_flags = fcntl(c.sock_fd, F_GETFL, 0);
            if (original_flags >= 0)
                fcntl(c.sock_fd, F_SETFL, original_flags | O_NONBLOCK);
#endif
            bool fatal_read = false;
            bool oversized_read = false;
            int fatal_ssl_error = SSL_ERROR_NONE;
            std::array<uint8_t, 64 * 1024> chunk{};
            // Fairness + memory bound: process at most 1 MiB from one peer per
            // event-loop tick, then decode before returning to select().
            constexpr size_t kReadBudgetPerTick = 1024u * 1024u;
            constexpr size_t kRxBufferHardCap =
                static_cast<size_t>(MAX_FRAME_PAYLOAD_U32) + chunk.size() +
                FRAME_HEADER_SIZE_U32;
            size_t read_this_tick = 0;
            while (read_this_tick < kReadBudgetPerTick) {
                size_t n = 0;
                const size_t request = std::min(
                    chunk.size(), kReadBudgetPerTick - read_this_tick);
                clear_stale_ssl_errors_before_io();
                const int ret = SSL_read_ex(c.ssl.get(), chunk.data(), request, &n);
                if (ret > 0 && n > 0) {
                    if (c.rx_buffer.size() > kRxBufferHardCap - n) {
                        oversized_read = true;
                        break;
                    }
                    c.rx_buffer.insert(c.rx_buffer.end(), chunk.begin(), chunk.begin() +
                                       static_cast<std::ptrdiff_t>(n));
                    c.bytes_in += n;
                    read_this_tick += n;
                    continue;
                }
                const int ssl_err = SSL_get_error(c.ssl.get(), ret);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) break;
                fatal_read = true;
                fatal_ssl_error = ssl_err;
                break;
            }
#ifdef _WIN32
            nonblocking = 0;
            ioctlsocket(c.sock_fd, FIONBIO, &nonblocking);
#else
            if (original_flags >= 0)
                fcntl(c.sock_fd, F_SETFL, original_flags);
#endif
            if (fatal_read) {
                // SSL_ERROR_ZERO_RETURN is an orderly peer close (normal for a
                // completed one-shot direct session), not an operational error.
                if (fatal_ssl_error != SSL_ERROR_ZERO_RETURN) {
                    log_event("mesh_conn_close", c.peer_name + " reason=fatal_ssl_read ssl_err=" +
                              std::to_string(fatal_ssl_error) + " purpose=" +
                              std::to_string(static_cast<int>(c.purpose)));
                }
                close_conn(c);
                return;
            }

            if (oversized_read) {
                log_event("mesh_conn_close", c.peer_name +
                          " reason=rx_buffer_overflow bytes=" +
                          std::to_string(c.rx_buffer.size()));
                close_conn(c);
                return;
            }

            auto messages = drain_complete_frames(c.rx_buffer);
            for (auto& msg : messages) {
                if (static_cast<size_t>(conn_idx) >= conns_.size()) return;
                if (conns_[static_cast<size_t>(conn_idx)].sock_fd == INVALID_SOCKET) return;
                dispatch_message(conn_idx, msg);
            }
        } catch (const std::exception& e) {
            if (static_cast<size_t>(conn_idx) < conns_.size()) {
                log_event("mesh_conn_close", conns_[static_cast<size_t>(conn_idx)].peer_name +
                          " reason=read_exception detail=" + e.what());
                close_conn(conns_[static_cast<size_t>(conn_idx)]);
            }
        } catch (...) {
            if (static_cast<size_t>(conn_idx) < conns_.size())
                log_event("mesh_conn_close", conns_[static_cast<size_t>(conn_idx)].peer_name +
                          " reason=unknown_read_exception");
            if (static_cast<size_t>(conn_idx) < conns_.size())
                close_conn(conns_[static_cast<size_t>(conn_idx)]);
        }
    }

    // ── Dead-seed cooldown (B2) ─────────────────────────────────
    // See DeadSeedCooldown above. record_dead_seed_failure() is called from
    // advance_handshakes() on every handshake_deadline event for an outbound
    // dial target; record_dead_seed_success() clears the streak the moment
    // that addr actually completes a handshake. dead_seed_cooldown_active()
    // gates scheduling in try_connect_to_seeds(), before start_outbound_handshake
    // is ever called for that addr.

    // ±25% jitter around the base cooldown window, so many dead seeds
    // hitting the threshold together don't all re-probe in lockstep.
    std::chrono::milliseconds dead_seed_cooldown_duration() {
        long base_ms = static_cast<long>(kDeadSeedCooldownBaseSecs) * 1000;
        long jitter = base_ms * (static_cast<long>(rng_() % 50) - 25) / 100; // -25%..+24%
        return std::chrono::milliseconds(base_ms + jitter);
    }

    void record_dead_seed_failure(const std::string& addr) {
        if (addr.empty()) return;
        auto& dc = seed_cooldowns_[addr];
        dc.consecutive_deadline_failures++;
        if (dc.consecutive_deadline_failures >= kDeadSeedCooldownThreshold) {
            dc.in_cooldown = true;
            dc.cooldown_until = std::chrono::steady_clock::now() + dead_seed_cooldown_duration();
            log_event("mesh_seed_cooldown_enter",
                      addr + " streak=" + std::to_string(dc.consecutive_deadline_failures));
        }
    }

    void record_dead_seed_success(const std::string& addr) {
        if (addr.empty()) return;
        auto it = seed_cooldowns_.find(addr);
        if (it == seed_cooldowns_.end()) return;
        if (it->second.in_cooldown || it->second.consecutive_deadline_failures > 0) {
            log_event("mesh_seed_cooldown_reset", addr);
        }
        seed_cooldowns_.erase(it);
    }

    // True if addr is currently within an active cooldown window (dialing
    // should be skipped this pass). When the window has elapsed, clears the
    // in_cooldown flag as a side effect — the streak count is left intact,
    // so a single renewed handshake_deadline failure re-enters cooldown
    // immediately ("allow one probe" semantics).
    bool dead_seed_cooldown_active(const std::string& addr,
                                    std::chrono::steady_clock::time_point now) {
        auto it = seed_cooldowns_.find(addr);
        if (it == seed_cooldowns_.end() || !it->second.in_cooldown) return false;
        if (now < it->second.cooldown_until) return true;
        it->second.in_cooldown = false; // cooldown expired: allow exactly one probe
        return false;
    }

    // Health string for MESH_TREE / FLEET seed listings: "ok" (no failure
    // history), "backoff" (mid exponential retry delay), or
    // "cooldown(dead Nm)" (parked after repeated handshake_deadline
    // failures, N minutes remaining).
    std::string seed_dial_health(const std::string& addr,
                                  std::chrono::steady_clock::time_point now) const {
        auto cit = seed_cooldowns_.find(addr);
        if (cit != seed_cooldowns_.end() && cit->second.in_cooldown &&
            now < cit->second.cooldown_until) {
            long mins = std::chrono::duration_cast<std::chrono::minutes>(
                cit->second.cooldown_until - now).count();
            if (mins < 1) mins = 1;
            return "cooldown(dead " + std::to_string(mins) + "m)";
        }
        auto bit = backoffs_.find(addr);
        if (bit != backoffs_.end() && bit->second.attempt > 0) return "backoff";
        return "ok";
    }

    // ── Try to connect to seeds/discovered ─────────────────────

    void try_connect_to_seeds() {
        auto now = std::chrono::steady_clock::now();

        // Try seeds first
        for (auto& s : config_.seeds) {
            if (has_conn_for_addr(s.addr)) continue;
            // Also skip by pubkey: an inbound connection from this peer records the
            // peer's EPHEMERAL source port as peer_addr, which never matches the seed's
            // listen addr. Without this guard we would re-dial a peer we are already
            // connected to every loop, creating a second connection that resolve_duplicates
            // then tears down — churning forever and starving the event loop.
            if (!s.pubkey_hex.empty() && has_conn_for_pubkey(s.pubkey_hex)) continue;
            if (config_.require_seed_pins && s.pubkey_hex.empty()) {
                log_event("mesh_seed_missing_pin",
                          s.name + " " + s.addr + " (add pubkey= to seed line)");
                continue;
            }
            if (conns_.size() >= config_.max_peers) break;
            if (should_defer_outbound_for(s, now)) continue;
            // B2: dead-seed cooldown gates scheduling before start_outbound_handshake
            // is ever called — distinct from the exponential delay backoff below.
            if (dead_seed_cooldown_active(s.addr, now)) continue;

            auto& bo = backoffs_[s.addr];
            // P2 audit fix: initialize max_ms from config so the operator's
            // reconnect_backoff_max_secs actually takes effect (was hardcoded 30s).
            bo.max_ms = std::max(1000, config_.reconnect_backoff_max_secs * 1000);
            if (bo.attempt > 0 && now < bo.next_attempt) continue;

            // Attempt connect (non-blocking; handshake completes in event loop).
            bool started = start_outbound_handshake(s);
            if (started) {
                backoffs_.erase(s.addr);
            } else {
                bo.attempt++;
                // P2: ±25% jitter to prevent thundering herd on reconnect.
                // Use a seeded RNG (not rand()) so timing is not predictable
                // across daemon restarts.
                const int jitter_pct = static_cast<int>(rng_() % 50u) - 25;
                const int jitter = static_cast<int>(
                    static_cast<int64_t>(bo.delay_ms) * jitter_pct / 100);
                bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms + jitter);
                bo.delay_ms = std::min(std::max(bo.delay_ms * 2, 1000), bo.max_ms);
                break; // one failed bounded dial per loop; keep accept/read responsive
            }
        }

        // Try discovered peers too (if we have room). Snapshot by value:
        // connect_to_peer_impl → merge_peers can push_back to config_.discovered,
        // which would invalidate the loop iterator.
        if (conns_.size() < config_.max_peers) {
            auto discovered_snap = config_.discovered;
            for (auto& d : discovered_snap) {
                if (d.addr.empty()) continue;
                if (!is_trusted_pubkey(d.pubkey_hex)) continue;
                if (has_conn_for_addr(d.addr)) continue;
                if (!d.pubkey_hex.empty() && has_conn_for_pubkey(d.pubkey_hex)) continue;
                if (config_.require_seed_pins && d.pubkey_hex.empty()) continue;
                if (conns_.size() >= config_.max_peers) break;
                if (should_defer_outbound_for(d, now)) continue;
                if (dead_seed_cooldown_active(d.addr, now)) continue; // B2

                auto& bo = backoffs_[d.addr];
                if (bo.attempt > 0 && now < bo.next_attempt) continue;
                bool started = start_outbound_handshake(d);
                if (started) {
                    backoffs_.erase(d.addr);
                } else {
                    bo.attempt++;
                    // P2 audit fix: apply the same ±25% seeded jitter as seeds
                    // (was missing — discovered peers reconnected in lockstep).
                    const int jitter_pct = static_cast<int>(rng_() % 50u) - 25;
                    const int jitter = static_cast<int>(
                        static_cast<int64_t>(bo.delay_ms) * jitter_pct / 100);
                    bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms + jitter);
                    bo.delay_ms = std::min(std::max(bo.delay_ms * 2, 1000), bo.max_ms);
                    break; // one failed bounded dial per loop; keep accept/read responsive
                }
            }
        }
    }

    // ── Build local sessions summary JSON (BridgePanel v3 gossip) ──

    static std::string gossip_json_escape(const std::string& v) {
        std::string r;
        for (char ch : v) {
            switch (ch) {
                case '"': r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                        r += buf;
                    } else r += ch;
            }
        }
        return r;
    }

    std::string build_sessions_summary_json() const {
        static constexpr size_t kCapBytes = 4096;
        std::vector<std::string> entries;
        for (const auto& info : sessions_.list()) {
            auto* s = sessions_.get(info.name);
            if (!s) continue;
            std::string entry =
                "{\"name\":\"" + gossip_json_escape(s->name) + "\","
                "\"state\":\"" + gossip_json_escape(session_state_str(s->state)) + "\","
                "\"kind\":\"" + gossip_json_escape(session_kind_str(s->kind)) + "\",";
            if (!s->parent_id.empty()) {
                entry += "\"parent_id\":\"" + gossip_json_escape(s->parent_id) + "\",";
            }
            entry += "\"command\":\"" + gossip_json_escape(s->command) + "\","
                     "\"bytes\":" + std::to_string(s->scrollback.total_written()) + "}";
            entries.push_back(std::move(entry));
        }
        // Cap total size: drop oldest entries first, keep the newest that fit.
        size_t budget = kCapBytes > 2 ? kCapBytes - 2 : 0; // reserve for "[" "]"
        size_t start = entries.size();
        size_t running = 0;
        while (start > 0) {
            size_t next_len = entries[start - 1].size() + (start < entries.size() ? 1 : 0);
            if (running + next_len > budget) break;
            running += next_len;
            --start;
        }
        std::string out = "[";
        for (size_t i = start; i < entries.size(); ++i) {
            if (i > start) out += ",";
            out += entries[i];
        }
        out += "]";
        return out;
    }

    std::string build_mesh_tree_json() {
        std::ostringstream out;
        const auto now = std::chrono::steady_clock::now();
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            now - started_at_).count();
        const auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                ? config_.pong_timeout_secs : 30);
        out << "{\"node\":\"" << gossip_json_escape(config_.node_name) << "\","
            << "\"uptime_s\":" << uptime << ",\"peers\":[";
        bool first = true;
        for (const auto& c : conns_) {
            if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
            if (!first) out << ",";
            first = false;
            const bool ok = (now - c.last_pong) <= fresh;
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - c.last_pong).count();
            std::string peer_sessions = "[]";
            {
                std::shared_lock lock(gossip_sessions_mutex_);
                const auto it = gossip_sessions_json_.find(c.peer_name);
                if (it != gossip_sessions_json_.end()) peer_sessions = it->second;
            }
            out << "{\"name\":\"" << gossip_json_escape(c.peer_name) << "\","
                << "\"addr\":\"" << gossip_json_escape(c.peer_addr) << "\","
                << "\"healthy\":" << (ok ? "true" : "false") << ","
                << "\"last_pong_s\":" << age << ","
                << "\"sessions\":" << peer_sessions << "}";
        }
        // B2: dial health for configured seeds that are not currently connected
        // ("ok" / "backoff" / "cooldown(dead Nm)") — helps operators see why a
        // seed hasn't come back without grepping handshake_deadline logs.
        out << "],\"seeds\":[";
        first = true;
        for (const auto& s : config_.seeds) {
            if (s.name.empty() || has_conn_for_addr(s.addr)) continue;
            if (!first) out << ",";
            first = false;
            out << "{\"name\":\"" << gossip_json_escape(s.name) << "\","
                << "\"addr\":\"" << gossip_json_escape(s.addr) << "\","
                << "\"dial_health\":\"" << gossip_json_escape(seed_dial_health(s.addr, now)) << "\"}";
        }
        out << "],\"sessions\":[";
        first = true;
        for (const auto& info : sessions_.list()) {
            const auto* s = sessions_.get(info.name);
            if (!s) continue;
            if (!first) out << ",";
            first = false;
            out << "{\"name\":\"" << gossip_json_escape(s->name) << "\","
                << "\"state\":\"" << session_state_str(s->state) << "\","
                << "\"kind\":\"" << session_kind_str(s->kind) << "\",";
            if (!s->parent_id.empty()) {
                out << "\"parent_id\":\"" << gossip_json_escape(s->parent_id) << "\",";
            }
            out << "\"command\":\"" << gossip_json_escape(s->command) << "\","
                << "\"bytes\":" << s->scrollback.total_written() << "}";
        }
        out << "]}";
        return out.str();
    }

    // ── Send Gossip to all connections ─────────────────────────
    // Delta gossip: each connection receives only peers that changed since
    // its last_gossip_generation. First contact (gen 0) gets a full snapshot.
    void broadcast_gossip() {
        uint32_t cur_gen = gossip_generation_.load();
        HostStats hs = collect_host_stats(home_dir_);
        ServerInfoMsg info;
        info.hostname = config_.node_name;
        info.version = std::string(kBridgeSessionsVersion);
        info.load = hs.load1 >= 0 ? hs.load1 : 0.0;
        info.sessions_summary_json = build_sessions_summary_json();
        info.host_stats_json = host_stats_to_json(hs);

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (c.exec_busy && c.exec_busy->load()) continue;
            // gen 0 → full snapshot; stale gen → delta; current gen → skip
            if (c.last_gossip_generation == 0 ||
                c.last_gossip_generation < cur_gen) {
                auto g = build_gossip(c.last_gossip_generation);
                if (!g.peers.empty())
                    (void)enqueue_frame(c, g, CONTROL_STREAM_ID);
                c.last_gossip_generation = cur_gen;
            }
            (void)enqueue_frame(c, info, CONTROL_STREAM_ID);
        }
    }

    // ── Send Ping to all connections ───────────────────────────

    void broadcast_ping() {
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            // A background daemon_shell_exec thread owns this conn's SSL
            // object right now (v1.7 async exec) — it handles Ping/Pong
            // itself; writing here too would race the thread's writes.
            if (c.exec_busy && c.exec_busy->load()) continue;
            // Non-blocking enqueue — never stall the event loop on a slow peer.
            c.ping_sent_at = std::chrono::steady_clock::now();
            if (!enqueue_frame(c, PingMsg{}, CONTROL_STREAM_ID)) {
                // Overflow only: leave connection up; next tick may drain.
                log_event("mesh_ping_enqueue_drop", c.peer_name);
            }
        }
    }

    // ── Check pong timeout ─────────────────────────────────────

    void check_pong_timeouts() {
        auto now = std::chrono::steady_clock::now();
        auto base = std::chrono::seconds(config_.pong_timeout_secs);

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            // A busy worker owns the SSL stream. When it releases ownership,
            // grant a fresh timeout window because it may have consumed Pong
            // frames outside the event loop.
            if (refresh_heartbeat_after_busy(c, now)) continue;
            // B1: RTT-aware deadline. Healthy WAN peers sit at ~144ms RTT and
            // can miss a tight static window under event-loop load. Use
            // max(base, 4×measured RTT) once we have a RTT sample; fall back
            // to base until then. Cap the multiplier so a pathological RTT
            // (e.g. a single slow poll) can't push the deadline unboundedly.
            auto deadline = std::chrono::duration_cast<std::chrono::milliseconds>(base);
            if (c.pong_rtt_ms.count() > 0) {
                std::chrono::milliseconds rtt = c.pong_rtt_ms;
                if (rtt > std::chrono::milliseconds(5000)) rtt = std::chrono::milliseconds(5000); // clamp
                auto rtt_aware = 4 * rtt;
                if (rtt_aware > deadline) deadline = rtt_aware;
            }
            if (now - c.last_pong > deadline) {
                log_event("mesh_pong_timeout", c.peer_name + " " + c.peer_addr
                          + " rtt_ms=" + std::to_string(c.pong_rtt_ms.count()));
                // Attempt graceful TLS shutdown before hard close (POSIX only).
                // Best-effort: if shutdown fails or platform doesn't support it,
                // close_conn still runs.
#ifndef _WIN32
                if (c.ssl && socket_selectable(c.sock_fd)) {
                    int flags = fcntl(c.sock_fd, F_GETFL, 0);
                    fcntl(c.sock_fd, F_SETFL, flags | O_NONBLOCK);
                    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (std::chrono::steady_clock::now() < dl) {
                        int ret = SSL_shutdown(c.ssl.get());
                        if (ret == 1) break;
                        if (ret == 0) continue;
                        int err = SSL_get_error(c.ssl.get(), ret);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                            bs_pollfd pfd{c.sock_fd, static_cast<short>(
                                err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
                            (void)bs_poll(&pfd, 1, 100);
                            continue;
                        }
                        break; // unrecoverable
                    }
                    fcntl(c.sock_fd, F_SETFL, flags); // restore blocking
                }
#endif
                close_conn(c);
            }
        }
    }

    // Cancel a transport worker that has exceeded the longest legitimate
    // transfer deadline. Never clear exec_busy here: the worker owns SSL until
    // it exits and clears that flag itself.
    //
    // The deadline is measured from the LAST PROGRESS tick (exec_last_progress_at),
    // not from exec_started_at. This lets a healthy, actively-streaming transfer
    // (file send / vfolder sync / edit upload) run as long as it needs, while a
    // STALLED transfer — one that has made no progress for kExecWatchdogSecs —
    // is still force-released (the original BUG-1 guarantee: a CLI timeout that
    // outlives its worker must not leave exec_busy stuck forever).
    void check_stale_exec() {
        static constexpr auto kExecWatchdogSecs =
            std::chrono::seconds(90);
        auto now = std::chrono::steady_clock::now();
        for (auto& c : conns_) {
            if (!c.exec_busy || !c.exec_busy->load()) continue;
            // If the worker refreshes last-progress (transfers do), use that;
            // otherwise fall back to exec_started_at (non-streaming execs).
            std::chrono::steady_clock::time_point last_activity{};
            if (c.exec_last_progress_at) {
                auto rep = c.exec_last_progress_at->load();
                if (rep != 0)
                    last_activity =
                        std::chrono::steady_clock::time_point(
                            std::chrono::steady_clock::duration(rep));
            }
            if (last_activity == std::chrono::steady_clock::time_point{})
                last_activity = c.exec_started_at;
            if (last_activity == std::chrono::steady_clock::time_point{}) continue;
            if (now - last_activity > kExecWatchdogSecs) {
                // A quiet attached session is not a hung operation. Check the
                // child without blocking before cancelling its transport.
                if (c.attached_session) {
                    auto* session = c.attached_session;
                    bool child_alive = false;
#ifdef _WIN32
                    if (session->child_pid) {
                        child_alive =
                            WaitForSingleObject(session->child_pid, 0) == WAIT_TIMEOUT;
                    }
#else
                    if (session->hosted) {
                        // Hosted: the shell is the worker's child — waitpid
                        // would ECHILD here. Liveness = live worker socket.
                        child_alive =
                            !session->worker_died && session->master_fd >= 0;
                    } else if (session->child_pid > 0) {
                        int status = 0;
                        const pid_t result = waitpid(session->child_pid, &status, WNOHANG);
                        if (result == 0) {
                            child_alive = true;
                        } else if (result == session->child_pid) {
                            session->child_pid = -1;
                            session->state = SessionState::Died;
                            session->release_exited_runtime();
                        } else if (result < 0 && errno == ECHILD) {
                            session->child_pid = -1;
                        }
                    }
#endif
                    if (child_alive) continue;
                }
                log_event("exec_watchdog_timeout", c.peer_name);
                if (c.exec_cancelled) c.exec_cancelled->store(true);
                // Shut down the socket so the blocking worker thread gets an
                // error on its next read/write and exits, which releases
                // exec_busy. Do NOT force-release exec_busy here — the worker
                // owns SSL and must release it cleanly.
                c.close_requested = true;
#ifdef _WIN32
                if (c.sock_fd != INVALID_SOCKET) ::shutdown(c.sock_fd, SD_BOTH);
#else
                if (c.sock_fd != INVALID_SOCKET) ::shutdown(c.sock_fd, SHUT_RDWR);
#endif
            }
        }
    }

    // ── Clean up dead connections ──────────────────────────────

    void clean_dead_conns() {
        // Finish closes that were deferred while a detached exec worker owned
        // the SSL object. This is the only point where a deferred Conn becomes
        // eligible for erasure.
        for (auto& c : conns_) {
            if (c.close_requested &&
                (!c.exec_busy || !c.exec_busy->load())) {
                (void)close_conn(c);
            }
        }
        // Erase entries already closed via close_conn(); do not touch live sockets.
        conns_.erase(
            std::remove_if(conns_.begin(), conns_.end(),
                [](const Conn& c) { return c.sock_fd == INVALID_SOCKET; }),
            conns_.end());
        // Newly accepted links are classified after their first post-Hello
        // message. Resolve mesh duplicates only after that classification so
        // direct session transports can coexist with the background mesh link.
        resolve_duplicates();
    }

