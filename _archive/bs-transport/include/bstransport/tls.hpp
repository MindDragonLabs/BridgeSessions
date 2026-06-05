#pragma once

#include <bsprotocol/message.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <memory>
#include <string>
#include <functional>

namespace bs::transport {

// ── RAII SSL Contexts ─────────────────────────────────────────────

struct SslCtxDeleter { void operator()(SSL_CTX* ctx) noexcept { SSL_CTX_free(ctx); } };
struct SslDeleter    { void operator()(SSL* ssl) noexcept    { SSL_free(ssl);     } };

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using SslPtr    = std::unique_ptr<SSL, SslDeleter>;

// ── Server Context ────────────────────────────────────────────────

struct ServerConfig {
    std::string cert_file;           // PEM: X.509 cert chain
    std::string key_file;            // PEM: ed25519 private key
    std::string authorized_keys_file; // hex pubkeys, one per line
};

// Create a TLS 1.3 server context with ed25519 mutual auth.
// Client must present a valid certificate signed by a key in authorized_keys.
[[nodiscard]] SslCtxPtr create_server_context(const ServerConfig& cfg);

// ── Client Context ────────────────────────────────────────────────

struct ClientConfig {
    std::string cert_file;           // PEM: X.509 cert
    std::string key_file;            // PEM: ed25519 private key
    std::string known_servers_file;  // host fingerprint cache ("host:port hex" per line)
};

// Create a TLS 1.3 client context with ed25519 client certificate.
// On first connect, server fingerprint is stored (TOFU).
// On mismatch, callback is invoked.
using TofuCallback = std::function<bool(const std::string& fingerprint)>;
[[nodiscard]] SslCtxPtr create_client_context(const ClientConfig& cfg, TofuCallback tofu);

// ── Utility: in-memory key+cert generation for testing ────────────

// Returns {cert_pem, key_pem} — self-signed ed25519 X.509 cert + private key
[[nodiscard]] std::pair<std::string, std::string> generate_cert_key_pair(const char* common_name);

// Extracts hex-encoded public key from an EVP_PKEY (for authorized_keys)
[[nodiscard]] std::string pubkey_hex_from_pem(const std::string& key_pem);

// Extracts the verified peer certificate public key from an established TLS connection.
// Server uses this as the authorized-client identity for per-user session namespaces.
[[nodiscard]] std::string peer_public_key_hex(SSL* ssl);

} // namespace bs::transport
