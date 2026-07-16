#include "bstransport/tls.hpp"
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace bs::transport {

namespace {

// ── Key generation ─────────────────────────────────────────────

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
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365 * 24 * 3600 * 10L);
    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, nullptr) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed");
    }
    return {pkey, cert};
}

// ── PEM I/O ──────────────────────────────────────────────────

std::string bio_to_string(BIO* bio) {
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return std::string(data, len);
}

std::string cert_to_pem(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

std::string key_to_pem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

EVP_PKEY* key_from_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// ── Public key helpers ────────────────────────────────────────

std::vector<uint8_t> extract_raw_pubkey(EVP_PKEY* key) {
    std::vector<uint8_t> raw(32);
    size_t len = 32;
    if (EVP_PKEY_get_raw_public_key(key, raw.data(), &len) <= 0)
        return {};
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
    std::vector<uint8_t> raw;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int byte;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> byte;
        raw.push_back(static_cast<uint8_t>(byte));
    }
    return raw;
}

// ── authorized_keys ───────────────────────────────────────────

struct AuthorizedKeys {
    std::vector<std::vector<uint8_t>> keys;
    void load_from_file(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            // Strip trailing whitespace and any inline comment (after first '#').
            // Note: '#' inside a hex pubkey never appears, so the first '#' is the
            // comment start. Authorized-keys files commonly have key + " # comment".
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (!line.empty()) {
                auto raw = hex_decode(line);
                if (raw.size() == 32) keys.push_back(std::move(raw));
            }
        }
    }
    bool contains(const std::vector<uint8_t>& key) const {
        for (auto& k : keys) if (k == key) return true;
        return false;
    }
};

// ── Custom cert verification (bypasses OpenSSL CA trust store) ──

// Server: verifies client's ed25519 raw public key against authorized_keys
int server_cert_verify(X509_STORE_CTX* ctx, void* arg) {
    auto* auth = static_cast<AuthorizedKeys*>(arg);
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) { std::cerr << "[auth] no cert" << std::endl; return 0; }
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (!pk) { std::cerr << "[auth] no pubkey" << std::endl; return 0; }
    auto raw = extract_raw_pubkey(pk);
    if (raw.empty()) { std::cerr << "[auth] empty raw key" << std::endl; return 0; }
    bool ok = auth->contains(raw);
    std::cerr << "[auth] keys=" << auth->keys.size() << " match=" << ok << std::endl;
    return ok ? 1 : 0;
}

// Client: TOFU via fingerprint callback
int client_cert_verify(X509_STORE_CTX* ctx, void* arg) {
    auto* cb = static_cast<TofuCallback*>(arg);
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &len)) return 0;
    std::string fp;
    for (unsigned int i = 0; i < len; ++i) {
        char h[3];
        snprintf(h, sizeof(h), "%02x", md[i]);
        fp += h;
    }
    return (*cb)(fp) ? 1 : 0;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────────

SslCtxPtr create_server_context(const ServerConfig& cfg) {
    auto ctx = SslCtxPtr(SSL_CTX_new(TLS_server_method()));
    if (!ctx) throw std::runtime_error("TLS_server_method failed");

    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_file(ctx.get(), cfg.cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("load server cert: " + cfg.cert_file);
    if (SSL_CTX_use_PrivateKey_file(ctx.get(), cfg.key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("load server key: " + cfg.key_file);

    // Request client cert + fail if no cert presented.
    // Use cert_verify_callback to bypass CA trust store — we verify the raw ed25519 key.
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);

    auto* auth = new AuthorizedKeys{};
    auth->load_from_file(cfg.authorized_keys_file);
    SSL_CTX_set_cert_verify_callback(ctx.get(), server_cert_verify, auth);

    // P12-1: TLS session cache — reuse sessions across reconnects
    SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
    SSL_CTX_sess_set_cache_size(ctx.get(), 256);

    return ctx;
}

SslCtxPtr create_client_context(const ClientConfig& cfg, TofuCallback tofu) {
    auto ctx = SslCtxPtr(SSL_CTX_new(TLS_client_method()));
    if (!ctx) throw std::runtime_error("TLS_client_method failed");

    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_file(ctx.get(), cfg.cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("load client cert: " + cfg.cert_file);
    if (SSL_CTX_use_PrivateKey_file(ctx.get(), cfg.key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        throw std::runtime_error("load client key: " + cfg.key_file);

    // Request server cert, verify via TOFU callback.
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    auto* cb = new TofuCallback(std::move(tofu));
    SSL_CTX_set_cert_verify_callback(ctx.get(), client_cert_verify, cb);

    return ctx;
}

// ── Utility functions ────────────────────────────────────────────

std::pair<std::string, std::string> generate_cert_key_pair(const char* common_name) {
    auto [pkey, cert] = generate_ed25519_cert(common_name);
    auto c = cert_to_pem(cert);
    auto k = key_to_pem(pkey);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return {c, k};
}

std::string pubkey_hex_from_pem(const std::string& key_pem) {
    EVP_PKEY* pkey = key_from_pem(key_pem);
    if (!pkey) return "";
    auto hex = pubkey_hex(pkey);
    EVP_PKEY_free(pkey);
    return hex;
}

std::string peer_public_key_hex(SSL* ssl) {
    if (!ssl) return "";
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return "";
    EVP_PKEY* pkey = X509_get0_pubkey(cert);
    std::string hex = pkey ? pubkey_hex(pkey) : "";
    X509_free(cert);
    return hex;
}

} // namespace bs::transport
