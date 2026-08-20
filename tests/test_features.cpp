// test_features.cpp — Catch2 tests for newly added features with zero coverage:
//   1. levenshtein() — edit distance
//   2. base64_encode() — RFC 4648 encoding
//   3. resolve_peer() — 4-tier fuzzy name resolution
//   4. on_session_erased callback — P0 UAF fix hook
//   5. detect_interpreter() — file-extension → interpreter mapping
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <filesystem>

using namespace bs::mesh;
namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ════════════════════════════════════════════════════════════════
// 1. levenshtein — pure function, known edit distances
// ════════════════════════════════════════════════════════════════

TEST_CASE("levenshtein: identical strings have distance 0", "[features][levenshtein]") {
    REQUIRE(levenshtein("", "") == 0);
    REQUIRE(levenshtein("abc", "abc") == 0);
    REQUIRE(levenshtein("test-pc1", "test-pc1") == 0);
}

TEST_CASE("levenshtein: empty vs non-empty yields length", "[features][levenshtein]") {
    REQUIRE(levenshtein("", "abc") == 3);
    REQUIRE(levenshtein("abc", "") == 3);
    REQUIRE(levenshtein("", "x") == 1);
}

TEST_CASE("levenshtein: single substitution", "[features][levenshtein]") {
    REQUIRE(levenshtein("cat", "bat") == 1);
    REQUIRE(levenshtein("test-pc1", "test-pc2") == 1);
}

TEST_CASE("levenshtein: single insertion/deletion", "[features][levenshtein]") {
    REQUIRE(levenshtein("shadow", "shadowx") == 1);  // insertion
    REQUIRE(levenshtein("shadowx", "shadow") == 1);  // deletion
}

TEST_CASE("levenshtein: case-insensitive comparison", "[features][levenshtein]") {
    // levenshtein uses std::tolower internally
    REQUIRE(levenshtein("Shadow", "shadow") == 0);
    REQUIRE(levenshtein("TEST-PC1", "test-pc1") == 0);
}

TEST_CASE("levenshtein: classic known vectors", "[features][levenshtein]") {
    REQUIRE(levenshtein("kitten", "sitting") == 3);
    REQUIRE(levenshtein("sunday", "saturday") == 3);
    REQUIRE(levenshtein("flaw", "lawn") == 2);
}

TEST_CASE("levenshtein: near-miss peer names within threshold 2", "[features][levenshtein]") {
    // These are the kinds of typos resolve_peer tier-4 must catch
    REQUIRE(levenshtein("shadow", "shadwo") == 2);   // transposition-ish
    REQUIRE(levenshtein("shadow", "shdaow") == 2);   // 2 edits
    REQUIRE(levenshtein("test-pc1", "test-pc") == 1);
}

// ════════════════════════════════════════════════════════════════
// 2. base64_encode — static method, RFC 4648 known vectors
// ════════════════════════════════════════════════════════════════

TEST_CASE("base64_encode: empty input", "[features][base64]") {
    REQUIRE(MeshController::base64_encode("") == "");
}

TEST_CASE("base64_encode: RFC 4648 known vectors", "[features][base64]") {
    // Standard test vectors from RFC 4648 Section 10
    REQUIRE(MeshController::base64_encode("f") == "Zg==");
    REQUIRE(MeshController::base64_encode("fo") == "Zm8=");
    REQUIRE(MeshController::base64_encode("foo") == "Zm9v");
    REQUIRE(MeshController::base64_encode("foob") == "Zm9vYg==");
    REQUIRE(MeshController::base64_encode("fooba") == "Zm9vYmE=");
    REQUIRE(MeshController::base64_encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64_encode: binary data with all byte values", "[features][base64]") {
    // Verify that high bytes are handled correctly (no sign extension)
    std::string input;
    for (int i = 0; i < 256; ++i)
        input.push_back(static_cast<char>(static_cast<unsigned char>(i)));
    std::string encoded = MeshController::base64_encode(input);

    // 256 bytes → 256/3 = 85 groups of 3 + 1 byte remainder → 85*4 + 4 = 344 chars
    REQUIRE(encoded.size() == 344);
    // No non-base64 chars except padding
    for (char c : encoded) {
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '=';
        REQUIRE(ok);
    }
}

TEST_CASE("base64_encode: round-trip with embedded decode", "[features][base64]") {
    // Inline minimal base64 decoder to verify round-trip without external deps
    static const int8_t b64idx[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    auto decode = [](const std::string& in) -> std::string {
        std::string out;
        int val = 0, bits = 0;
        for (char c : in) {
            if (c == '=') break;
            auto uc = static_cast<unsigned char>(c);
            if (uc >= 128 || b64idx[uc] < 0) continue;
            val = (val << 6) | b64idx[uc];
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((val >> bits) & 0xFF));
            }
        }
        return out;
    };

    std::vector<std::string> test_inputs = {
        "", "x", "hello", "test script content\nline 2\n",
        std::string(1, '\0'), std::string(3, '\xFF'),
        std::string(100, 'A'),
    };
    for (const auto& input : test_inputs) {
        std::string encoded = MeshController::base64_encode(input);
        std::string decoded = decode(encoded);
        REQUIRE(decoded == input);
    }
}

// ════════════════════════════════════════════════════════════════
// 3. detect_interpreter — static method, extension mapping
// ════════════════════════════════════════════════════════════════

TEST_CASE("detect_interpreter: .sh → bash", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("deploy.sh") == "sh");
    REQUIRE(MeshController::detect_interpreter("/tmp/test.sh") == "sh");
}

TEST_CASE("detect_interpreter: .ps1 → powershell", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("setup.ps1") == "powershell");
    REQUIRE(MeshController::detect_interpreter("C:\\scripts\\run.ps1") == "powershell");
}

TEST_CASE("detect_interpreter: .py → python", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("script.py") == "python");
    REQUIRE(MeshController::detect_interpreter("/home/user/app.py") == "python");
}

TEST_CASE("detect_interpreter: .bat and .cmd → cmd", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("build.bat") == "cmd");
    REQUIRE(MeshController::detect_interpreter("install.cmd") == "cmd");
}

TEST_CASE("detect_interpreter: case-insensitive extension", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("script.SH") == "sh");
    REQUIRE(MeshController::detect_interpreter("Setup.PS1") == "powershell");
    REQUIRE(MeshController::detect_interpreter("app.PY") == "python");
}

TEST_CASE("detect_interpreter: unknown/no extension defaults to bash", "[features][interpreter]") {
    REQUIRE(MeshController::detect_interpreter("Makefile") == "sh");
    REQUIRE(MeshController::detect_interpreter("script.unknown") == "sh");
    REQUIRE(MeshController::detect_interpreter("no_ext") == "sh");
    REQUIRE(MeshController::detect_interpreter("/tmp/my-script") == "sh");
}

// ════════════════════════════════════════════════════════════════
// 4. on_session_erased callback — SessionRegistry P0 UAF fix hook
// ════════════════════════════════════════════════════════════════

TEST_CASE("on_session_erased: callback fires on kill", "[features][session_erased]") {
    SessionRegistry registry;

    std::atomic<bool> fired{false};
    std::string erased_name;
    registry.set_on_session_erased([&](const std::string& name) {
        erased_name = name;
        fired = true;
    });

    // Create and immediately kill a session
    auto* s = registry.attach("erased-test", "echo hello", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE_FALSE(fired);

    registry.kill("erased-test");

    REQUIRE(fired);
    REQUIRE(erased_name == "erased-test");
    REQUIRE(registry.count() == 0);
}

TEST_CASE("on_session_erased: no callback set — kill still works", "[features][session_erased]") {
    // Default-constructed registry has no callback; kill must not crash
    SessionRegistry registry;

    auto* s = registry.attach("no-cb-test", "echo hi", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);

    registry.kill("no-cb-test");  // should not throw/crash
    REQUIRE(registry.count() == 0);
}

TEST_CASE("on_session_erased: callback fires for each killed session", "[features][session_erased]") {
    SessionRegistry registry;

    std::atomic<int> call_count{0};
    registry.set_on_session_erased([&](const std::string&) {
        ++call_count;
    });

    registry.attach("sess-a", "echo a", 80, 24, "xterm");
    registry.attach("sess-b", "echo b", 80, 24, "xterm");

    registry.kill("sess-a");
    REQUIRE(call_count == 1);

    registry.kill("sess-b");
    REQUIRE(call_count == 2);

    REQUIRE(registry.count() == 0);
}

TEST_CASE("on_session_erased: kill of nonexistent session does not fire callback",
          "[features][session_erased]") {
    SessionRegistry registry;

    std::atomic<bool> fired{false};
    registry.set_on_session_erased([&](const std::string&) {
        fired = true;
    });

    registry.kill("ghost-session");
    REQUIRE_FALSE(fired);
}

// ════════════════════════════════════════════════════════════════
// 5. resolve_peer — 4-tier fuzzy name resolution
//    Requires a MeshController instance (constructor bootstraps identity).
// ════════════════════════════════════════════════════════════════

// Helper: create an isolated temp home and return its path.
static std::string make_test_home(const std::string& tag) {
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    auto home = std::string("/tmp/bs_test_features_") + tag + "_" + std::to_string(ts);
    REQUIRE(ensure_private_directory(home));
    return home;
}

// Helper: authorized_keys file that trusts the given pubkey.
static void write_authorized_keys(const std::string& home, const std::string& pubkey) {
    std::string ak_path = home + "/authorized_keys";
    REQUIRE(write_private_text_file(ak_path, pubkey + "\n"));
}

using PR = MeshController::PeerResolveResult;

TEST_CASE("resolve_peer: tier 1 exact match (case-insensitive)", "[features][resolve_peer]") {
    auto home = make_test_home("exact");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "test-pc1",
        .addr = "203.0.113.10:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    // Exact match
    auto r = mc.resolve_peer("test-pc1");
    REQUIRE(r.tier == PR::Exact);
    REQUIRE(r.name == "test-pc1");
    REQUIRE(r.addr == "203.0.113.10:19949");
    REQUIRE(r.pubkey_hex == seed_pk);

    // Case-insensitive exact match
    auto r2 = mc.resolve_peer("TEST-PC1");
    REQUIRE(r2.tier == PR::Exact);
    REQUIRE(r2.name == "TEST-PC1");

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: tier 3 suffix/prefix segment match", "[features][resolve_peer]") {
    auto home = make_test_home("suffix");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "lab-shadow",   // query "shadow" should suffix-match
        .addr = "10.0.0.1:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    auto r = mc.resolve_peer("shadow");
    REQUIRE(r.tier == PR::Suffix);
    REQUIRE(r.name == "lab-shadow");
    REQUIRE(r.addr == "10.0.0.1:19949");

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: tier 3 prefix segment match", "[features][resolve_peer]") {
    auto home = make_test_home("prefix");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "shadow-worker-1",  // query "shadow" should prefix-match
        .addr = "10.0.0.2:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    auto r = mc.resolve_peer("shadow");
    REQUIRE(r.tier == PR::Suffix);
    REQUIRE(r.name == "shadow-df8uluc8");

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: tier 4 levenshtein fuzzy match", "[features][resolve_peer]") {
    auto home = make_test_home("fuzzy");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "alpha",   // "alphaa" is distance 1
        .addr = "10.0.0.3:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    auto r = mc.resolve_peer("alphaa");  // 1 edit (insertion)
    REQUIRE(r.tier == PR::Levenshtein);
    REQUIRE(r.name == "alpha");
    REQUIRE(r.addr == "10.0.0.3:19949");

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: ambiguous matches return suggestions", "[features][resolve_peer]") {
    auto home = make_test_home("ambiguous");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    // Two peers that are both levenshtein-1 from "shadow"
    cfg.seeds.push_back(PeerEntry{
        .name = "shadox",  // distance 1 from "shadow"
        .addr = "10.0.0.4:19949",
        .pubkey_hex = seed_pk});
    cfg.seeds.push_back(PeerEntry{
        .name = "shadaw",  // distance 1 from "shadow"
        .addr = "10.0.0.5:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    auto r = mc.resolve_peer("shadow");
    REQUIRE(r.tier == PR::None_);
    REQUIRE(r.name.empty());
    REQUIRE(r.suggestions.size() == 2);

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: no match returns empty result", "[features][resolve_peer]") {
    auto home = make_test_home("nomatch");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "test-pc1",
        .addr = "203.0.113.10:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    // "xyz" is too different from any seed and too short (< 3 not reached,
    // but "xyzqwerty" is >= 3 and far from everything)
    auto r = mc.resolve_peer("xyzqwerty");
    REQUIRE(r.tier == PR::None_);
    REQUIRE(r.name.empty());
    REQUIRE(r.addr.empty());
    REQUIRE(r.suggestions.empty());

    fs::remove_all(home);
}

TEST_CASE("resolve_peer: short query (<3 chars) skips fuzzy tier", "[features][resolve_peer]") {
    auto home = make_test_home("short");

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    write_authorized_keys(home, seed_pk);

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = home + "/authorized_keys";
    cfg.seeds.push_back(PeerEntry{
        .name = "ab",   // distance from "a" is 1, but query is < 3
        .addr = "10.0.0.6:19949",
        .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    // "a" is only 1 char — fuzzy tier requires query.size() >= 3
    auto r = mc.resolve_peer("a");
    REQUIRE(r.tier == PR::None_);
    REQUIRE(r.name.empty());

    fs::remove_all(home);
}
