// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs_tls.hpp — TLS transport, frame I/O (extracted from bs-protocol.h)
// DO NOT include directly — include bs-protocol.h instead.
#pragma once

#include "bs_msg_types.hpp"

namespace bs::mesh {

// ── RAII deleters ───────────────────────────────────────────────

struct SslCtxDeleter { void operator()(SSL_CTX* ctx) noexcept { SSL_CTX_free(ctx); } };
struct SslDeleter    { void operator()(SSL* ssl) noexcept    { SSL_free(ssl);     } };
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using SslPtr    = std::unique_ptr<SSL, SslDeleter>;

// ── enum for TLS mode ───────────────────────────────────────────

enum class TlsMode { Listen, Connect };

// ── NodeTlsConfig ───────────────────────────────────────────────

struct NodeTlsConfig {
    std::string cert_file;
    std::string key_file;
    std::string authorized_keys_file;           // for Listen mode
    std::function<bool(const std::string&)> tofu_cb;  // for Connect mode
};

// ── Internal helpers ────────────────────────────────────────────

// Forward decl at bs::mesh scope so the TLS verify callbacks (in the anonymous
// namespace below) can emit R1.1/R1.2 accept/reject logs. Defined ~line 2350.
inline void log_event(const std::string& event, const std::string& detail);

namespace {

// ── BIO helpers ──────────────────────────────────────────────

std::string bio_to_string(BIO* bio) {
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return std::string(data, len);
}

// ── PEM I/O ──────────────────────────────────────────────────

std::string cert_to_pem(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) throw std::runtime_error("BIO_new failed");
    PEM_write_bio_X509(bio, cert);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

std::string key_to_pem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) throw std::runtime_error("BIO_new failed");
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

EVP_PKEY* key_from_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// ── Key generation ────────────────────────────────────────────

std::pair<EVP_PKEY*, X509*> generate_ed25519_cert(const char* cn) {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx) throw std::runtime_error("EVP_PKEY_CTX_new_id(ED25519) failed");

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("ed25519 keygen failed");
    }
    EVP_PKEY_CTX_free(pctx);

    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); throw std::runtime_error("X509_new failed"); }
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365LL * 24LL * 3600LL * 10LL);
    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, nullptr) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed");
    }
    return {pkey, cert};
}

// ── Public key helpers ────────────────────────────────────────

std::vector<uint8_t> extract_raw_pubkey(EVP_PKEY* key) {
    std::vector<uint8_t> raw(32);
    size_t len = 32;
    if (EVP_PKEY_get_raw_public_key(key, raw.data(), &len) <= 0)
        return {};
    raw.resize(len);
    return raw;
}

std::string pubkey_hex(EVP_PKEY* key) {
    auto raw = extract_raw_pubkey(key);
    if (raw.empty()) return "";
    std::string hex;
    for (auto b : raw) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        hex += buf;
    }
    return hex;
}

std::vector<uint8_t> hex_decode(const std::string& hex) {
    if ((hex.size() % 2) != 0) return {};
    std::vector<uint8_t> raw;
    raw.reserve(hex.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return raw;
}

namespace {

bool write_private_text_file_impl(const std::string& path,
                                  std::string_view content,
                                  bool append) {
#ifdef _WIN32
    const int flags = _O_WRONLY | _O_CREAT | _O_BINARY |
                      (append ? _O_APPEND : _O_TRUNC);
    const int fd = _open(path.c_str(), flags, _S_IREAD | _S_IWRITE);
    if (fd < 0) return false;
    bool ok = _chmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
    size_t offset = 0;
    while (ok && offset < content.size()) {
        const size_t remaining = content.size() - offset;
        const unsigned int chunk = static_cast<unsigned int>(
            std::min<size_t>(remaining, 1u << 30));
        const int written = _write(fd, content.data() + offset, chunk);
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && _commit(fd) != 0) ok = false;
    if (_close(fd) != 0) ok = false;
    return ok;
#else
    const int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
                      ;
    const int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
    size_t offset = 0;
    while (ok && offset < content.size()) {
        const ssize_t written = ::write(fd, content.data() + offset,
                                        content.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    return ok;
#endif
}

} // anonymous namespace

[[nodiscard]] bool write_private_text_file(const std::string& path,
                                           std::string_view content) {
    return write_private_text_file_impl(path, content, false);
}

[[nodiscard]] bool append_private_text_file(const std::string& path,
                                            std::string_view content) {
    return write_private_text_file_impl(path, content, true);
}

[[nodiscard]] bool ensure_private_directory(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return false;
#ifndef _WIN32
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, ec);
    if (ec) return false;
#endif
    return true;
}

[[nodiscard]] bool restrict_private_file_permissions(const std::string& path) {
#ifdef _WIN32
    return ::_chmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
#else
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}

[[nodiscard]] int run_editor_process(const std::string& editor,
                                     const std::string& local_path) {
    if (editor.empty() || local_path.empty()) return -1;
#ifdef _WIN32
    const intptr_t result = _spawnlp(_P_WAIT, editor.c_str(), editor.c_str(),
                                     local_path.c_str(), nullptr);
    return result < 0 ? -1 : static_cast<int>(result);
#else
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execlp(editor.c_str(), editor.c_str(), local_path.c_str(),
                 static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

} // anonymous namespace

// ── AuthorizedKeys ────────────────────────────────────────────

struct AuthorizedKeys {
    std::vector<std::vector<uint8_t>> keys;
    std::string file_path;  // stored for R4.1 hot-reload

    void load_from_file(const std::string& path) {
        file_path = path;
        keys.clear();
        if (path.empty()) return;
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                   line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (!line.empty()) {
                auto raw = hex_decode(line);
                if (raw.size() == 32) keys.push_back(std::move(raw));
            }
        }
    }

    // R4.1: reload from disk — called per-accept so revocations take effect immediately
    void reload() { if (!file_path.empty()) load_from_file(file_path); }

    bool contains(const std::vector<uint8_t>& key) const {
        for (auto& k : keys) if (k == key) return true;
        return false;
    }

#ifdef BS_TESTING
    // Convenience for tests: accept a lowercase hex pubkey string
    bool is_authorized(const std::string& hex) const {
        if (hex.size() % 2 != 0) return false;
        std::vector<uint8_t> raw;
        raw.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            unsigned int b = 0;
            if (std::sscanf(hex.c_str() + i, "%2x", &b) != 1) return false;
            raw.push_back(static_cast<uint8_t>(b));
        }
        return contains(raw);
    }
#endif
};

// ── Custom cert verify callbacks ──────────────────────────────

namespace {

// Local bytes->hex for verify-callback logging (avoids depending on later helpers).
inline std::string verify_bytes_hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xF]); }
    return s;
}

// Server: verifies client's ed25519 raw public key against authorized_keys (R4.1: reloads per-accept)
int server_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* auth = static_cast<AuthorizedKeys*>(arg);
    auth->reload();  // R4.1: pick up key additions/revocations without restart
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (!pk) return 0;
    auto raw = extract_raw_pubkey(pk);
    if (raw.empty()) return 0;
    std::string pk_hex = verify_bytes_hex(raw);
    if (auth->contains(raw)) {
        log_event("tls_verify_server", pk_hex + " result=accept");  // R1.1
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_server", pk_hex + " result=reject");  // R1.1
    return 0;
}

// Client: TOFU via SHA-256 fingerprint callback
int client_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* cb = static_cast<std::function<bool(const std::string&)>*>(arg);
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &len)) return 0;
    std::string fp;
    for (unsigned int i = 0; i < len; ++i) {
        char h[3];
        snprintf(h, sizeof(h), "%02x", md[i]);
        fp += h;
    }
    if ((*cb)(fp)) {
        log_event("tls_verify_client", fp + " result=accept");  // R1.2
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_client", fp + " result=reject");  // R1.2
    return 0;
}

void free_owned_authorized_keys(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<AuthorizedKeys*>(ptr);
}

void free_owned_tofu_callback(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<std::function<bool(const std::string&)>*>(ptr);
}

int owned_authorized_keys_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_owned_authorized_keys);
    return index;
}

int owned_tofu_callback_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_owned_tofu_callback);
    return index;
}

} // anonymous namespace

// ── Public: generate_cert_key_pair ────────────────────────────────

std::pair<std::string, std::string> generate_cert_key_pair(const char* common_name) {
    auto [pkey, cert] = generate_ed25519_cert(common_name);
    auto c = cert_to_pem(cert);
    auto k = key_to_pem(pkey);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return {c, k};
}

// ── Public: pubkey_hex_from_pem ──────────────────────────────────

std::string pubkey_hex_from_pem(const std::string& key_pem) {
    EVP_PKEY* pkey = key_from_pem(key_pem);
    if (!pkey) return "";
    auto hex = pubkey_hex(pkey);
    EVP_PKEY_free(pkey);
    return hex;
}

// ── Public: peer_public_key_hex ──────────────────────────────────

std::string peer_public_key_hex(SSL* ssl) {
    if (!ssl) return "";
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return "";
    EVP_PKEY* pkey = X509_get0_pubkey(cert);
    std::string hex = pkey ? pubkey_hex(pkey) : "";
    X509_free(cert);
    return hex;
}

// R1.4: one-line cert subject for handshake observability logs
std::string peer_cert_subject_oneline(SSL* ssl) {
    if (!ssl) return "";
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return "";
    char* subj = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    std::string s = subj ? subj : "";
    OPENSSL_free(subj);
    X509_free(cert);
    return s;
}

// ── Public: bootstrap_identity ───────────────────────────────────
// Auto-generate ed25519 keypair on first run into ~/.bridgesessions/

void bootstrap_identity(const std::string& home_dir) {
    namespace fs = std::filesystem;
    fs::path dir(home_dir);
    if (!ensure_private_directory(dir.string()))
        throw std::runtime_error("cannot create private identity directory " + dir.string());

    // If the standard identity already exists, nothing to do
    fs::path id_key   = dir / "id_ed25519.pem";
    fs::path id_cert  = dir / "id_ed25519-cert.pem";
    fs::path id_pub   = dir / "id_ed25519.pub";

    if (fs::exists(id_key)) {
        for (const auto& path : {id_key, id_cert, id_pub}) {
            if (fs::exists(path) && !restrict_private_file_permissions(path.string()))
                throw std::runtime_error("cannot restrict permissions on " + path.string());
        }
        return;
    }

    // Migration: if legacy _bs_autocert.pem + _bs_autokey.pem exist, copy them
    fs::path legacy_cert = dir / "_bs_autocert.pem";
    fs::path legacy_key  = dir / "_bs_autokey.pem";

    if (fs::exists(legacy_cert) && fs::exists(legacy_key)) {
        const auto read_text = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + path.string());
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const std::string cert_pem = read_text(legacy_cert);
        const std::string key_pem = read_text(legacy_key);
        if (!write_private_text_file(id_cert.string(), cert_pem) ||
            !write_private_text_file(id_key.string(), key_pem))
            throw std::runtime_error("cannot securely migrate legacy identity");

        // Also generate the .pub file from the migrated key
        std::string hex = pubkey_hex_from_pem(key_pem);
        if (hex.empty() || !write_private_text_file(id_pub.string(), hex + "\n"))
            throw std::runtime_error("cannot securely write migrated public key");
        return;
    }

    // Fresh bootstrap: generate keypair
    auto [cert_pem, key_pem] = generate_cert_key_pair("bridgesessions");
    std::string pubkey_hex = pubkey_hex_from_pem(key_pem);

    if (!write_private_text_file(id_key.string(), key_pem) ||
        !write_private_text_file(id_cert.string(), cert_pem) ||
        !write_private_text_file(id_pub.string(), pubkey_hex + "\n"))
        throw std::runtime_error("cannot securely write generated identity");
}

// ── Public: unified create_node_tls ──────────────────────────────
//
// auth_storage / tofu_storage: optional caller-owned storage for the
// cert-verify callback context. When supplied, the context is written there
// and NOT heap-allocated, so the caller controls its lifetime (must outlive the
// returned SSL_CTX). When null, fallback storage is attached to SSL_CTX ex-data
// and destroyed automatically with the context.

SslCtxPtr create_node_tls(const NodeTlsConfig& cfg, TlsMode mode,
                          AuthorizedKeys* auth_storage = nullptr,
                          std::function<bool(const std::string&)>* tofu_storage = nullptr) {
    SslCtxPtr ctx;

    if (mode == TlsMode::Listen) {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_server_method()));
        if (!ctx) throw std::runtime_error("TLS_server_method failed");
    } else {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_client_method()));
        if (!ctx) throw std::runtime_error("TLS_client_method failed");
    }

    // TLS 1.2+ (prefer 1.3). TLS 1.3-only handshakes stalled as SSL_ERROR_WANT_READ
    // across macOS/Linux/Windows Tailscale paths with self-signed Ed25519 certs
    // (fleet RCA). Product docs: TLS 1.2 minimum, TLS 1.3 preferred — not 1.3-only.
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION);  // force 1.2: OpenSSL 3.3.2 vs 3.6.3 TLS1.3 data-plane breaks

    // Load own certificate + key
    if (!cfg.cert_file.empty()) {
        if (SSL_CTX_use_certificate_file(ctx.get(), cfg.cert_file.c_str(),
                                          SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("load cert: " + cfg.cert_file);
    }
    if (!cfg.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ctx.get(), cfg.key_file.c_str(),
                                         SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("load key: " + cfg.key_file);
    }

    if (mode == TlsMode::Listen) {
        // Server: verify client cert + fail if no cert presented
        SSL_CTX_set_verify(ctx.get(),
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           nullptr);

        AuthorizedKeys* auth = auth_storage;
        std::unique_ptr<AuthorizedKeys> owned_auth;
        if (!auth) {
            owned_auth = std::make_unique<AuthorizedKeys>();
            auth = owned_auth.get();
            const int index = owned_authorized_keys_index();
            if (index < 0 || SSL_CTX_set_ex_data(ctx.get(), index, auth) != 1) {
                throw std::runtime_error("attach authorized_keys callback storage failed");
            }
            (void)owned_auth.release();
        }
        if (!cfg.authorized_keys_file.empty())
            auth->load_from_file(cfg.authorized_keys_file);
        SSL_CTX_set_cert_verify_callback(ctx.get(), server_cert_verify_cb, auth);

        // TLS session cache — reuse sessions across reconnects
        SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx.get(), 256);
    } else {
        // Client: verify server cert via TOFU
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

        std::function<bool(const std::string&)>* cb = tofu_storage;
        if (cb) *cb = cfg.tofu_cb;
        else {
            auto owned_cb = std::make_unique<std::function<bool(const std::string&)>>(cfg.tofu_cb);
            cb = owned_cb.get();
            const int index = owned_tofu_callback_index();
            if (index < 0 || SSL_CTX_set_ex_data(ctx.get(), index, cb) != 1) {
                throw std::runtime_error("attach TOFU callback storage failed");
            }
            (void)owned_cb.release();
        }
        SSL_CTX_set_cert_verify_callback(ctx.get(), client_cert_verify_cb, cb);
    }

    return ctx;
}

// ────────────────────────────────────────────────────────────────────
// 4. FRAME I/O (ssl_check, read_frame, write_frame)
// ────────────────────────────────────────────────────────────────────

// SSL_get_error() is only reliable when the calling thread's OpenSSL error
// queue was empty before the I/O operation. The mesh event loop also performs
// outbound handshakes on this thread; a failed dial can otherwise poison the
// next healthy connection's SSL_read_ex() classification and turn WANT_READ
// into a fatal SSL_ERROR_SSL.
inline void clear_stale_ssl_errors_before_io() {
    ERR_clear_error();
}

namespace {

void ssl_check(int ret, SSL* ssl, const char* op) {
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        throw std::runtime_error(std::string(op) + " failed: SSL error " + std::to_string(err) + " " + buf);
    }
}

} // anonymous namespace

// Bounded WANT_READ/WANT_WRITE tolerance for frame reads. select() readiness
// only guarantees *some* bytes, not a complete TLS record — large chunk frames
// split across records can surface WANT_READ mid-frame (observed pulling 1 MiB
// from a Windows peer: transfer died on the first record boundary). Retry
// briefly instead of tearing the connection down; the budget caps the stall.
inline bool ssl_want_retry(SSL* ssl, int ret, int& budget) {
    int err = SSL_get_error(ssl, ret);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return false;
    if (--budget <= 0) return false;
    // Plain sleep (no select): this header's SOCKET/select compat layer is
    // defined further down, and 25 ms granularity is plenty for frame I/O.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

Message read_frame(SSL* ssl) {
    int want_budget = 400;  // 400 x 25 ms = 10 s worst-case mid-frame stall
    // Read header
    uint8_t header[FRAME_HEADER_SIZE];
    size_t total = 0;
    while (total < FRAME_HEADER_SIZE) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE - total, &n);
        if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget)) continue;
        ssl_check(ret, ssl, "SSL_read header");
        total += n;
    }

    uint16_t length = read_u16(header + 4);

    if (length > MAX_FRAME_SIZE)
        throw std::runtime_error("frame payload exceeds MAX_FRAME_SIZE");

    // Read payload
    std::vector<uint8_t> raw(FRAME_HEADER_SIZE + length);
    std::memcpy(raw.data(), header, FRAME_HEADER_SIZE);

    if (length > 0) {
        total = 0;
        while (total < length) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, raw.data() + FRAME_HEADER_SIZE + total, length - total, &n);
            if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget)) continue;
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

[[nodiscard]] inline bool buffered_bytes_hold_complete_frame(
    std::span<const uint8_t> bytes) {
    if (bytes.size() < FRAME_HEADER_SIZE) return false;
    const uint16_t length = read_u16(bytes.data() + 4);
    if (length > MAX_FRAME_SIZE) return true;  // let decode surface the protocol error
    return bytes.size() >= FRAME_HEADER_SIZE + length;
}

[[nodiscard]] inline std::vector<Message> drain_complete_frames(
    std::vector<uint8_t>& buffered) {
    std::vector<Message> messages;
    size_t consumed = 0;
    while (buffered.size() - consumed >= FRAME_HEADER_SIZE) {
        const uint8_t* start = buffered.data() + consumed;
        const uint16_t length = read_u16(start + 4);
        if (length > MAX_FRAME_SIZE)
            throw std::runtime_error("frame payload exceeds MAX_FRAME_SIZE");
        const size_t frame_size = FRAME_HEADER_SIZE + length;
        if (buffered.size() - consumed < frame_size) break;
        messages.push_back(decode(std::span<const uint8_t>(start, frame_size)));
        consumed += frame_size;
    }
    if (consumed > 0)
        buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
    return messages;
}

[[nodiscard]] inline bool ssl_has_complete_buffered_frame(SSL* ssl) {
    const int pending = SSL_pending(ssl);
    if (pending < static_cast<int>(FRAME_HEADER_SIZE)) return false;
    std::array<uint8_t, FRAME_HEADER_SIZE> header{};
    size_t peeked = 0;
    if (SSL_peek_ex(ssl, header.data(), header.size(), &peeked) <= 0 ||
        peeked < header.size()) return false;
    return buffered_bytes_hold_complete_frame(
        std::span<const uint8_t>(header.data(), header.size())) ||
        pending >= static_cast<int>(FRAME_HEADER_SIZE + read_u16(header.data() + 4));
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id) {
    auto frame = encode(msg, stream_id);

    size_t total = 0;
    int retries = 0;
    const int max_retries = 1000;
    while (total < frame.size() && retries < max_retries) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_write_ex(ssl, frame.data() + total, frame.size() - total, &n);
        int err = SSL_get_error(ssl, ret);
        if (ret > 0) {
            total += n;
            retries = 0;
            continue;
        }
        // v2.0.12c: retry on WANT_WRITE/WANT_READ — Windows/MinGW returns these
        // even on blocking sockets for large writes.
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            ++retries;
            std::this_thread::yield();
            continue;
        }
        ssl_check(ret, ssl, "SSL_write");
    }
    if (total < frame.size()) {
        throw std::runtime_error("SSL_write failed after retries");
    }
}

// ── Non-blocking frame I/O helpers (for handshake state machine) ────
// These variants never block; they drain or emit what is immediately
// available and buffer the rest. They are used only during the initial
// TLS + Hello handshake so the event loop stays responsive.

[[nodiscard]] inline std::optional<Message> read_frame_nonblocking(
    SSL* ssl, std::vector<uint8_t>& rx_buffer, int* want_error = nullptr) {
    if (want_error) *want_error = SSL_ERROR_WANT_READ;
    for (;;) {
        std::array<uint8_t, 4096> chunk{};
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, chunk.data(), chunk.size(), &n);
        if (ret > 0 && n > 0) {
            rx_buffer.insert(rx_buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (want_error) *want_error = err;
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) break;
        throw std::runtime_error("SSL_read failed during handshake");
    }
    if (buffered_bytes_hold_complete_frame(rx_buffer)) {
        auto messages = drain_complete_frames(rx_buffer);
        if (!messages.empty()) return std::move(messages.front());
    }
    return std::nullopt;
}

// Returns true when the encoded frame has been fully written.
// On WANT_READ/WANT_WRITE returns false and leaves unwritten bytes in tx_buffer.
[[nodiscard]] inline bool write_frame_nonblocking(
    SSL* ssl, const Message& msg, uint16_t stream_id, std::vector<uint8_t>& tx_buffer,
    int* want_error = nullptr) {
    if (want_error) *want_error = SSL_ERROR_WANT_WRITE;
    if (tx_buffer.empty()) tx_buffer = encode(msg, stream_id);
    while (!tx_buffer.empty()) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_write_ex(ssl, tx_buffer.data(), tx_buffer.size(), &n);
        if (ret > 0 && n > 0) {
            tx_buffer.erase(tx_buffer.begin(), tx_buffer.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (want_error) *want_error = err;
            return false;
        }
        throw std::runtime_error("SSL_write failed during handshake");
    }
    return true;
}
} // namespace bs::mesh
