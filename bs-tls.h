// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-tls.h — TLS transport and frame I/O
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// 3. TLS TRANSPORT (ed25519 mTLS, unified Listen/Connect)
// ────────────────────────────────────────────────────────────────────

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

// systemd unit names allow only [A-Za-z0-9:._-]. Session names are
// operator-controlled but flow into systemd-run --unit= verbatim, so map
// everything else to '_'. Lives ABOVE the #include of bs-session-worker.h so
// spawn_session_worker() can keep unit names deterministic + inject-safe.
inline std::string sanitize_systemd_unit_name(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        // keep [A-Za-z0-9:._-] verbatim; map everything else to '_'
        if (std::isalnum(u) || c == ':' || c == '.' || c == '_' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out;
}

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

// ── Bootstrap: ed25519 sign/verify (directory enrollment) ─────────
// Sign the canonical payload with an ed25519 private key (PEM), returning the
// 64-byte signature. Returns empty vector on any OpenSSL failure.
[[nodiscard]] inline std::vector<uint8_t> ed25519_sign(
    const std::string& key_pem, std::string_view payload) {
    EVP_PKEY* key = key_from_pem(key_pem);
    if (!key) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::vector<uint8_t> sig;
    if (ctx &&
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        size_t sig_len = 0;
        if (EVP_DigestSign(ctx, nullptr, &sig_len,
                           reinterpret_cast<const unsigned char*>(payload.data()),
                           payload.size()) == 1 && sig_len > 0) {
            sig.resize(sig_len);
            if (EVP_DigestSign(ctx, sig.data(), &sig_len,
                               reinterpret_cast<const unsigned char*>(payload.data()),
                               payload.size()) != 1) {
                sig.clear();
            } else {
                sig.resize(sig_len);
            }
        }
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return sig;
}

// Verify a 64-byte ed25519 signature against the canonical payload using the
// issuer's raw 32-byte public key (hex-decoded).
[[nodiscard]] inline bool ed25519_verify(
    const std::string& issuer_pubkey_hex, std::string_view payload,
    const std::vector<uint8_t>& signature) {
    if (signature.empty()) return false;
    std::vector<uint8_t> raw = hex_decode(issuer_pubkey_hex);
    if (raw.size() != 32) return false;
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                raw.data(), raw.size());
    if (!key) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx &&
        EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        ok = EVP_DigestVerify(ctx, signature.data(), signature.size(),
                              reinterpret_cast<const unsigned char*>(payload.data()),
                              payload.size()) == 1;
    }
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
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
            // Strip optional "pubkey " prefix (written by join handler)
            // Also handle leading whitespace
            while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line = line.substr(1);
            if (line.starts_with("pubkey ") || line.starts_with("pubkey\t")) {
                line = line.substr(7);
            }
            if (!line.empty()) {
                auto raw = hex_decode(line);
                if (raw.size() == 32) keys.push_back(std::move(raw));
            }
        }
    }

    // R4.1: reload from disk on every call so revocations take effect
    // immediately. The file is a handful of 64-hex lines; a mtime/size cache
    // missed same-tick equal-length key replacements (coarse-mtime FS).
    void reload() {
        if (file_path.empty()) return;
        load_from_file(file_path);
    }

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

struct ServerVerifyContext {
    AuthorizedKeys* auth = nullptr;
    std::atomic<bool>* allow_join_connections = nullptr;
};

int expected_peer_pubkey_index();

// Server: verifies client's ed25519 raw public key against authorized_keys (R4.1: reloads per-accept)
int server_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* verify = static_cast<ServerVerifyContext*>(arg);
    if (!verify || !verify->auth) return 0;
    auto* auth = verify->auth;
    auth->reload();  // R4.1: pick up key additions/revocations without restart
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (!pk) return 0;
    auto raw = extract_raw_pubkey(pk);
    if (raw.empty()) return 0;
    std::string pk_hex = verify_bytes_hex(raw);
    if (auth->contains(raw)) {
        log_event("tls_verify_server", pk_hex.substr(0, 12) + " result=accept");  // R1.1
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    // Join window open: accept unknown peers so they can present an invite token.
    // The JoinRequest handler validates the token; without a valid token the
    // connection is dropped after the join exchange (no session is created).
    if (verify->allow_join_connections &&
        verify->allow_join_connections->load(std::memory_order_relaxed)) {
        log_event("tls_verify_server", pk_hex.substr(0, 12) + " result=accept (join window)");
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_server", pk_hex.substr(0, 12) + " result=reject");  // R1.1
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
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
        ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* expected = ssl ? static_cast<std::string*>(
        SSL_get_ex_data(ssl, expected_peer_pubkey_index())) : nullptr;
    EVP_PKEY* cert_key = X509_get0_pubkey(cert);
    const std::string actual_pubkey = cert_key ? verify_bytes_hex(extract_raw_pubkey(cert_key)) : "";
    const bool accepted = expected && !expected->empty()
        ? actual_pubkey == *expected
        : (*cb)(fp);
    if (accepted) {
        log_event("tls_verify_client", fp.substr(0, 12) + " result=accept");  // R1.2
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_client", fp.substr(0, 12) + " result=reject");  // R1.2
    return 0;
}

void free_owned_authorized_keys(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<AuthorizedKeys*>(ptr);
}

void free_server_verify_context(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<ServerVerifyContext*>(ptr);
}

void free_expected_peer_pubkey(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<std::string*>(ptr);
}

int expected_peer_pubkey_index() {
    static const int index = SSL_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_expected_peer_pubkey);
    return index;
}

inline bool set_expected_peer_pubkey(SSL* ssl, const std::string& expected) {
    if (!ssl) return false;
    auto value = std::make_unique<std::string>(expected);
    if (SSL_set_ex_data(ssl, expected_peer_pubkey_index(), value.get()) != 1) return false;
    (void)value.release();
    return true;
}

void free_owned_tofu_callback(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<std::function<bool(const std::string&)>*>(ptr);
}

int owned_authorized_keys_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_owned_authorized_keys);
    return index;
}

int server_verify_context_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_server_verify_context);
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
                          std::function<bool(const std::string&)>* tofu_storage = nullptr,
                          std::atomic<bool>* allow_join_connections = nullptr) {
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
    // P2 audit note (2026-08-10): TLS 1.3 is disabled because the static Windows
    // PE links OpenSSL 3.3.2 while Linux/macOS link 3.6.x — TLS 1.3 data-plane
    // breaks between them (fleet RCA 2026-08-05). TODO: once the Windows PE is
    // rebuilt against OpenSSL >= 3.6, remove this max pin to re-enable TLS 1.3.
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
        auto verify = std::make_unique<ServerVerifyContext>();
        verify->auth = auth;
        verify->allow_join_connections = allow_join_connections;
        const int verify_index = server_verify_context_index();
        if (verify_index < 0 || SSL_CTX_set_ex_data(ctx.get(), verify_index, verify.get()) != 1)
            throw std::runtime_error("attach server verify callback storage failed");
        SSL_CTX_set_cert_verify_callback(ctx.get(), server_cert_verify_cb, verify.get());
        (void)verify.release();

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
inline thread_local const std::atomic<bool>* g_frame_io_cancelled = nullptr;

inline bool ssl_want_retry(SSL* ssl, int ret, int& budget,
                           const std::atomic<bool>* cancelled = nullptr) {
    if (!cancelled) cancelled = g_frame_io_cancelled;
    int err = SSL_get_error(ssl, ret);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return false;
    if (cancelled && cancelled->load(std::memory_order_relaxed)) return false;
    if (--budget <= 0) return false;
    // Plain sleep (no select): this header's SOCKET/select compat layer is
    // defined further down, and 25 ms granularity is plenty for frame I/O.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

Message read_frame(SSL* ssl, const std::atomic<bool>* cancelled = nullptr) {
    if (!cancelled) cancelled = g_frame_io_cancelled;
    int want_budget = 400;  // 400 x 25 ms = 10 s worst-case mid-frame stall
    // Read minimum header (6 bytes); extend to 8 if FLAG_LENGTH_U32.
    uint8_t header[FRAME_HEADER_SIZE_U32];
    size_t total = 0;
    while (total < FRAME_HEADER_SIZE_U16) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE_U16 - total, &n);
        if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget, cancelled)) continue;
        ssl_check(ret, ssl, "SSL_read header");
        total += n;
    }
    size_t hdr_size = FRAME_HEADER_SIZE_U16;
    uint32_t length = 0;
    if (header[3] & FLAG_LENGTH_U32) {
        while (total < FRAME_HEADER_SIZE_U32) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE_U32 - total, &n);
            if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget, cancelled)) continue;
            ssl_check(ret, ssl, "SSL_read u32 header");
            total += n;
        }
        hdr_size = FRAME_HEADER_SIZE_U32;
        length = read_u32be(header + 4);
        if (length > MAX_FRAME_PAYLOAD_U32)
            throw std::runtime_error("frame payload exceeds MAX_FRAME_PAYLOAD_U32");
    } else {
        length = read_u16(header + 4);
    }

    std::vector<uint8_t> raw(hdr_size + length);
    std::memcpy(raw.data(), header, hdr_size);

    if (length > 0) {
        total = 0;
        while (total < length) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, raw.data() + hdr_size + total, length - total, &n);
            if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget, cancelled)) continue;
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

// Payload length only (not including header). Requires ≥6 bytes available.
inline uint32_t detect_frame_length(const uint8_t* data, size_t available) {
    auto [hdr, length] = frame_header_layout(data, available);
    (void)hdr;
    return length;
}

[[nodiscard]] inline bool buffered_bytes_hold_complete_frame(
    std::span<const uint8_t> bytes) {
    if (bytes.size() < FRAME_HEADER_SIZE_U16) return false;
    try {
        auto [hdr, length] = frame_header_layout(bytes.data(), bytes.size());
        if (bytes.size() < hdr) return false; // need more header bytes
        return bytes.size() >= hdr + length;
    } catch (...) {
        return true; // let decode surface the protocol error
    }
}

[[nodiscard]] inline std::vector<Message> drain_complete_frames(
    std::vector<uint8_t>& buffered) {
    std::vector<Message> messages;
    size_t consumed = 0;
    while (buffered.size() - consumed >= FRAME_HEADER_SIZE_U16) {
        const uint8_t* start = buffered.data() + consumed;
        size_t avail = buffered.size() - consumed;
        size_t hdr = 0;
        uint32_t length = 0;
        try {
            auto layout = frame_header_layout(start, avail);
            hdr = layout.first;
            length = layout.second;
        } catch (...) {
            throw;
        }
        if (avail < hdr) break;
        const size_t frame_size = hdr + length;
        if (avail < frame_size) break;
        messages.push_back(decode(std::span<const uint8_t>(start, frame_size)));
        consumed += frame_size;
    }
    if (consumed > 0)
        buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
    return messages;
}

[[nodiscard]] inline bool ssl_has_complete_buffered_frame(SSL* ssl) {
    const int pending = SSL_pending(ssl);
    if (pending < static_cast<int>(FRAME_HEADER_SIZE_U16)) return false;
    std::array<uint8_t, FRAME_HEADER_SIZE_U32> header{};
    size_t peeked = 0;
    if (SSL_peek_ex(ssl, header.data(), FRAME_HEADER_SIZE_U16, &peeked) <= 0 ||
        peeked < FRAME_HEADER_SIZE_U16) return false;
    if (header[3] & FLAG_LENGTH_U32) {
        if (pending < static_cast<int>(FRAME_HEADER_SIZE_U32)) return false;
        size_t peeked2 = 0;
        if (SSL_peek_ex(ssl, header.data(), FRAME_HEADER_SIZE_U32, &peeked2) <= 0 ||
            peeked2 < FRAME_HEADER_SIZE_U32) return false;
    }
    return buffered_bytes_hold_complete_frame(
        std::span<const uint8_t>(header.data(),
            (header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16))
        || pending >= static_cast<int>(
            ((header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16)
            + detect_frame_length(header.data(),
                (header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16));
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id,
                 bool allow_large = false,
                 const std::atomic<bool>* cancelled = nullptr) {
    if (!cancelled) cancelled = g_frame_io_cancelled;
    auto frame = encode(msg, stream_id, allow_large);

    size_t total = 0;
    int retries = 0;
    const int max_retries = 1000;
    // P2: exponential backoff instead of yield() busy-loop on stalled connections.
    auto backoff = std::chrono::microseconds(1000);      // start 1ms
    const auto max_backoff = std::chrono::microseconds(100000); // cap 100ms
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (total < frame.size() && retries < max_retries) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            throw std::runtime_error("SSL_write cancelled");
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
            // P2: bounded by 30s deadline — fail instead of looping forever.
            if (std::chrono::steady_clock::now() >= deadline) break;
            if (cancelled && cancelled->load(std::memory_order_relaxed)) break;
            ++retries;
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, max_backoff);
            continue;
        }
        ssl_check(ret, ssl, "SSL_write");
    }
    if (total < frame.size()) {
        throw std::runtime_error("SSL_write failed after retries/deadline");
    }
}

// ── Non-blocking frame I/O helpers (for handshake state machine) ────
// These variants never block; they drain or emit what is immediately
// available and buffer the rest. They are used only during the initial
// TLS + Hello handshake so the event loop stays responsive.

[[nodiscard]] inline std::optional<Message> read_frame_nonblocking(
    SSL* ssl, std::vector<uint8_t>& rx_buffer, int* want_error = nullptr) {
    if (want_error) *want_error = SSL_ERROR_WANT_READ;
    constexpr size_t kHandshakeReadBudget = 64u * 1024u;
    constexpr size_t kHandshakeBufferCap = 1024u * 1024u;
    size_t read_this_call = 0;
    while (read_this_call < kHandshakeReadBudget) {
        std::array<uint8_t, 4096> chunk{};
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, chunk.data(), chunk.size(), &n);
        if (ret > 0 && n > 0) {
            if (rx_buffer.size() > kHandshakeBufferCap - n)
                throw std::runtime_error("handshake frame exceeds buffer cap");
            rx_buffer.insert(rx_buffer.end(), chunk.begin(),
                             chunk.begin() + static_cast<std::ptrdiff_t>(n));
            read_this_call += n;
            if (buffered_bytes_hold_complete_frame(rx_buffer)) break;
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

