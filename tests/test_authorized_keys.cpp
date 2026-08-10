// test_authorized_keys.cpp — verify authorized_keys parsing with "pubkey " prefix
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <fstream>
#include <cstdio>
#include <random>

using namespace bs::mesh;

namespace {
// Generate a random 32-byte key, return as 64-char hex string
std::string random_key() {
    std::random_device rd;
    std::vector<uint8_t> raw(32);
    for (auto& b : raw) b = static_cast<uint8_t>(rd());
    return verify_bytes_hex(raw);
}

// Write content to a temp file and return its path
std::string write_temp(const std::string& content) {
    char tmpl[] = "/tmp/bs-auth-test-XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    write(fd, content.data(), content.size());
    close(fd);
    return tmpl;
}
} // namespace

TEST_CASE("authorized_keys: bare hex line (no prefix) loads correctly", "[authkeys]") {
    std::string key = random_key();
    auto path = write_temp(key + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    auto raw = hex_decode(key);
    REQUIRE(auth.contains(raw));
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: 'pubkey ' prefix is stripped (join handler format)", "[authkeys]") {
    std::string key = random_key();
    auto path = write_temp("pubkey " + key + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    auto raw = hex_decode(key);
    REQUIRE(auth.contains(raw));
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: leading whitespace before 'pubkey ' is handled", "[authkeys]") {
    std::string key = random_key();
    auto path = write_temp("  pubkey " + key + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    auto raw = hex_decode(key);
    REQUIRE(auth.contains(raw));
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: comment lines starting with # are skipped", "[authkeys]") {
    std::string key = random_key();
    auto path = write_temp("# This is a comment\n" + key + "\n# Another comment\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    auto raw = hex_decode(key);
    REQUIRE(auth.contains(raw));
    REQUIRE(auth.keys.size() == 1);
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: empty lines are skipped", "[authkeys]") {
    std::string key = random_key();
    auto path = write_temp("\n\n" + key + "\n\n\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    REQUIRE(auth.keys.size() == 1);
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: multiple keys load correctly", "[authkeys]") {
    std::string k1 = random_key(), k2 = random_key(), k3 = random_key();
    auto path = write_temp(k1 + "\npubkey " + k2 + "\n" + k3 + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    REQUIRE(auth.keys.size() == 3);
    REQUIRE(auth.contains(hex_decode(k1)));
    REQUIRE(auth.contains(hex_decode(k2)));
    REQUIRE(auth.contains(hex_decode(k3)));
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: reload picks up new keys", "[authkeys]") {
    std::string k1 = random_key();
    auto path = write_temp(k1 + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    REQUIRE(auth.keys.size() == 1);

    // Add a second key and reload
    std::string k2 = random_key();
    {
        std::ofstream f(path, std::ios::app);
        f << "pubkey " << k2 << "\n";
    }
    auth.reload();
    REQUIRE(auth.keys.size() == 2);
    REQUIRE(auth.contains(hex_decode(k2)));
    ::unlink(path.c_str());
}

TEST_CASE("authorized_keys: mixed prefixed and bare entries coexist", "[authkeys]") {
    std::string bare = random_key();
    std::string prefixed = random_key();
    auto path = write_temp(bare + "\npubkey " + prefixed + "\n");
    AuthorizedKeys auth;
    auth.load_from_file(path);
    REQUIRE(auth.contains(hex_decode(bare)));
    REQUIRE(auth.contains(hex_decode(prefixed)));
    ::unlink(path.c_str());
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
