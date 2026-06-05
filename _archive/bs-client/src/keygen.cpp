// keygen.cpp — Phase 9: key generation and authorization
// Shared utility used by both bs-client and bs-server subcommands

#include <bstransport/tls.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#endif
#include <sys/stat.h>

namespace {

// Create directory tree if needed
bool ensure_dir(const std::string& path) {
    std::string dir;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i > 0) {
            dir = path.substr(0, i);
#ifdef _WIN32
            _mkdir(dir.c_str());
#else
            mkdir(dir.c_str(), 0700);
#endif
        }
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0700);
#endif
    return true;
}

} // anonymous namespace

// ── keygen: generate ed25519 keypair ──────────────────────────────
// Returns 0 on success, writes to ~/.bridgesessions/
int cmd_keygen() {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE") ? (std::string(getenv("HOMEDRIVE")) + getenv("HOMEPATH")).c_str() : nullptr;
#else
    const char* home = getenv("HOME");
#endif
    if (!home) { std::cerr << "HOME/USERPROFILE not set\n"; return 1; }

    std::string dir = std::string(home) + "/.bridgesessions";
    ensure_dir(dir);

    auto [cert, key] = bs::transport::generate_cert_key_pair("bs-client");
    auto pubkey = bs::transport::pubkey_hex_from_pem(key);

    std::string key_path = dir + "/id_ed25519.pem";
    std::string cert_path = dir + "/id_ed25519-cert.pem";
    std::string pub_path = dir + "/id_ed25519.pub";

    // Write key
    {
        std::ofstream f(key_path);
        f << key;
    }
#ifdef _WIN32
    _chmod(key_path.c_str(), _S_IREAD | _S_IWRITE);
#else
    chmod(key_path.c_str(), 0600);
#endif

    // Write cert
    {
        std::ofstream f(cert_path);
        f << cert;
    }

    // Write public key
    {
        std::ofstream f(pub_path);
        f << pubkey << "\n";
    }

    std::cout << "Generated ed25519 keypair:\n"
              << "  Private key: " << key_path << "\n"
              << "  Certificate: " << cert_path << "\n"
              << "  Public key:  " << pub_path << "\n"
              << "  Pubkey hex:  " << pubkey << "\n";

    return 0;
}

// ── authorize: register a hex-encoded ed25519 public key ──────────
int cmd_authorize(const char* hex_pubkey) {
    if (!hex_pubkey || !*hex_pubkey) {
        std::cerr << "usage: bs-server authorize <hex-pubkey>\n";
        return 1;
    }

#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE") ? (std::string(getenv("HOMEDRIVE")) + getenv("HOMEPATH")).c_str() : nullptr;
#else
    const char* home = getenv("HOME");
#endif
    if (!home) { std::cerr << "HOME/USERPROFILE not set\n"; return 1; }

    std::string dir = std::string(home) + "/.bridgesessions";
    ensure_dir(dir);
    std::string path = dir + "/authorized_keys";

    // Check for duplicates
    {
        std::ifstream existing(path);
        std::string line;
        while (std::getline(existing, line)) {
            if (line == hex_pubkey) {
                std::cout << "Key already authorized: " << hex_pubkey << "\n";
                return 0;
            }
        }
    }

    // Append
    {
        std::ofstream f(path, std::ios::app);
        f << hex_pubkey << "\n";
    }

    std::cout << "Authorized key: " << hex_pubkey << "\n";
    std::cout << "Written to: " << path << "\n";
    return 0;
}
