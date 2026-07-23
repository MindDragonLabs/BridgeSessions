#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <cstdio>
#include <fstream>
#include <filesystem>

using namespace bs::mesh;
namespace fs = std::filesystem;

// ── Test 1: bootstrap_identity creates key, cert, and pub files ──

TEST_CASE("bootstrap_identity generates identity files", "[identity]") {
    // Create a unique temp directory
    auto tmp = fs::temp_directory_path() / "bs_test_identity";
    fs::remove_all(tmp);  // clean slate
    fs::create_directories(tmp);

    // Bootstrap
    bootstrap_identity(tmp.string());

    // Verify key exists and is non-empty
    auto key_path = tmp / "id_ed25519.pem";
    REQUIRE(fs::exists(key_path));
    REQUIRE(fs::file_size(key_path) > 0);

    // Verify cert exists and is non-empty
    auto cert_path = tmp / "id_ed25519-cert.pem";
    REQUIRE(fs::exists(cert_path));
    REQUIRE(fs::file_size(cert_path) > 0);

    // Verify pub exists, is 64 hex chars
    auto pub_path = tmp / "id_ed25519.pub";
    REQUIRE(fs::exists(pub_path));
    std::ifstream pf(pub_path);
    REQUIRE(pf.is_open());
    std::string pub_hex;
    std::getline(pf, pub_hex);
    pf.close();
    REQUIRE(pub_hex.size() == 64);
    for (char c : pub_hex) {
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }

    // Verify pubkey_hex matches what pubkey_hex_from_pem(key_content) returns
    std::ifstream kf(key_path);
    REQUIRE(kf.is_open());
    std::stringstream kbuf;
    kbuf << kf.rdbuf();
    kf.close();
    std::string key_pem = kbuf.str();
    REQUIRE(!key_pem.empty());
    std::string computed_hex = pubkey_hex_from_pem(key_pem);
    REQUIRE(computed_hex == pub_hex);

    // Clean up
    fs::remove_all(tmp);
}

// ── Test 2: bootstrap_identity is idempotent ─────────────────────

TEST_CASE("bootstrap_identity is idempotent", "[identity]") {
    auto tmp = fs::temp_directory_path() / "bs_test_identity2";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // First bootstrap
    bootstrap_identity(tmp.string());

    // Record file stats
    auto key_path = tmp / "id_ed25519.pem";
    auto cert_path = tmp / "id_ed25519-cert.pem";
    auto pub_path = tmp / "id_ed25519.pub";

    auto key_size = fs::file_size(key_path);
    auto cert_size = fs::file_size(cert_path);
    auto pub_size = fs::file_size(pub_path);

    // Read pub contents
    std::ifstream pf1(pub_path);
    std::string pub_hex1;
    std::getline(pf1, pub_hex1);
    pf1.close();

    // Second bootstrap — should do nothing (no crash, files unchanged)
    bootstrap_identity(tmp.string());

    // Verify files unchanged
    REQUIRE(fs::file_size(key_path) == key_size);
    REQUIRE(fs::file_size(cert_path) == cert_size);
    REQUIRE(fs::file_size(pub_path) == pub_size);

    std::ifstream pf2(pub_path);
    std::string pub_hex2;
    std::getline(pf2, pub_hex2);
    pf2.close();
    REQUIRE(pub_hex2 == pub_hex1);

    // Clean up
    fs::remove_all(tmp);
}

// ── Test 3: bootstrap_identity creates directory if missing ───────

TEST_CASE("bootstrap_identity creates parent directory", "[identity]") {
    auto tmp = fs::temp_directory_path() / "bs_test_identity3";
    auto subdir = tmp / "nested" / "subdir";
    fs::remove_all(tmp);

    REQUIRE(!fs::exists(subdir));

    bootstrap_identity(subdir.string());

    REQUIRE(fs::exists(subdir / "id_ed25519.pem"));
    REQUIRE(fs::exists(subdir / "id_ed25519-cert.pem"));
    REQUIRE(fs::exists(subdir / "id_ed25519.pub"));

    // Clean up
    fs::remove_all(tmp);
}

// ── Test 4: Legacy migration from _bs_autocert.pem / _bs_autokey.pem ─

TEST_CASE("bootstrap_identity migrates legacy keys", "[identity][migration]") {
    auto tmp = fs::temp_directory_path() / "bs_test_identity_migrate";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Generate a fresh keypair to use as "legacy" files
    auto [cert_pem, key_pem] = generate_cert_key_pair("legacy");
    std::string expected_pub = pubkey_hex_from_pem(key_pem);

    // Write legacy files
    {
        std::ofstream f(tmp / "_bs_autocert.pem");
        f << cert_pem;
        f.close();
    }
    {
        std::ofstream f(tmp / "_bs_autokey.pem");
        f << key_pem;
        f.close();
    }

    // Bootstrap — should migrate, NOT generate new keys
    bootstrap_identity(tmp.string());

    // Verify migrated key matches legacy key content
    std::ifstream kf(tmp / "id_ed25519.pem");
    std::stringstream kbuf;
    kbuf << kf.rdbuf();
    kf.close();
    REQUIRE(kbuf.str() == key_pem);

    // Verify pub matches
    std::ifstream pf(tmp / "id_ed25519.pub");
    std::string pub_hex;
    std::getline(pf, pub_hex);
    pf.close();
    REQUIRE(pub_hex == expected_pub);

    // Clean up
    fs::remove_all(tmp);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
