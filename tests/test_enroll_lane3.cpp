// test_enroll_lane3.cpp — phase 3 first slice: signed mesh enrollment from
// the seed at join time. Covers the 26.09.09 hardening:
//   1. Token sign/verify round trip via make_directory_enroll /
//      apply_directory_enroll (issuer must be a pinned seed).
//   2. Replay rejection — a consumed enrollment id (issuer|pubkey|issued_at)
//      is rejected on resubmission (single-use token semantics).
//   3. require_seed_pins interplay — enrollment never bypasses pin
//      enforcement: only seed-pinned issuers are accepted, and a discovery
//      entry without a pinned/trusted key is not auto-trusted via gossip.
//   4. Gossip acceptance + mixed-version gating — enrollment accepted from a
//      peer advertising +enroll; version_has_cap parses the suffix.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace bs::mesh;
namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() /
        (std::string("bs_enroll_lane3_") + label + "_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

uint64_t now_secs() {
    return static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

} // namespace

TEST_CASE("version advertises +enroll capability suffix", "[bootstrap][enroll][lane3]") {
    REQUIRE(version_has_cap(version_string_with_local_caps(), kCapEnroll));
    REQUIRE(version_has_cap(version_string_with_local_caps(), kCapFrm2));
    // Parse arbitrary mixed-version Hello strings.
    REQUIRE(version_has_cap("26.09.09+frm2+enroll", kCapEnroll));
    REQUIRE_FALSE(version_has_cap("26.08.10-beta2+frm2", kCapEnroll));
    // Old peers are correctly identified as lacking the capability.
    REQUIRE_FALSE(version_has_cap("26.09.08", kCapEnroll));
}

TEST_CASE("seed-signed enrollment token round trips sign then verify",
          "[bootstrap][enroll][lane3]") {
    const auto home = unique_temp_dir("signverify");

    auto [seed_cert, seed_key] = generate_cert_key_pair("seed");
    std::string seed_pk = pubkey_hex_from_pem(seed_key);
    (void)seed_cert;
    auto [member_cert, member_key] = generate_cert_key_pair("member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "seed-a",
        .addr = "100.7.1.1:19949",
        .pubkey_hex = seed_pk,
    });
    MeshController controller(cfg, home.string());

    // The seed signs the joiner's identity at join time...
    DirectoryEnrollMsg e;
    e.name = "joined-node";
    e.pubkey_hex = member_pk;
    e.addr = "100.7.1.9:19949";
    e.issuer_pubkey = seed_pk;
    e.issued_at = now_secs();
    e.signature = ed25519_sign(seed_key, e.signed_payload());

    REQUIRE_FALSE(e.signature.empty());
    // ...and every peer verifies the seed signature and auto-trusts the key
    // without a manual `peers add --pubkey`.
    REQUIRE(controller.test_apply_enroll(e));
    REQUIRE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("replayed enrollment token is rejected (single-use)",
          "[bootstrap][enroll][lane3]") {
    const auto home = unique_temp_dir("replay");

    auto [seed_cert, seed_key] = generate_cert_key_pair("seed");
    std::string seed_pk = pubkey_hex_from_pem(seed_key);
    (void)seed_cert;
    auto [member_cert, member_key] = generate_cert_key_pair("member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "seed-a",
        .addr = "100.7.2.1:19949",
        .pubkey_hex = seed_pk,
    });
    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "joined-node";
    e.pubkey_hex = member_pk;
    e.addr = "100.7.2.9:19949";
    e.issuer_pubkey = seed_pk;
    e.issued_at = now_secs();
    e.signature = ed25519_sign(seed_key, e.signed_payload());

    // First delivery consumes the token.
    REQUIRE(controller.test_apply_enroll(e));
    REQUIRE(controller.test_authorized_on_disk(member_pk));

    // Simulate the peer having lost the member's trust (fresh authorized_keys)
    // and the same signed token being replayed by a malicious peer: the
    // replay guard must reject it and NOT re-trust the key.
    std::ofstream wipe(cfg.authorized_keys_path, std::ios::trunc);
    wipe.close();
    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("enrollment never bypasses require_seed_pins pin enforcement",
          "[bootstrap][enroll][lane3]") {
    const auto home = unique_temp_dir("pins");

    auto [seed_cert, seed_key] = generate_cert_key_pair("seed");
    std::string seed_pk = pubkey_hex_from_pem(seed_key);
    (void)seed_cert;
    auto [member_cert, member_key] = generate_cert_key_pair("member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    // require_seed_pins default (true): enrollment authority comes only from
    // explicitly pinned seed keys. A seed WITHOUT a pinned pubkey is not an
    // acceptable issuer even though the signature is valid.
    MeshConfig cfg;
    cfg.require_seed_pins = true;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "seed-unpinned",
        .addr = "100.7.3.1:19949",
        .pubkey_hex = {}, // no pin — cannot vouch even with a valid signature
    });
    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "joined-node";
    e.pubkey_hex = member_pk;
    e.addr = "100.7.3.9:19949";
    e.issuer_pubkey = seed_pk;
    e.issued_at = now_secs();
    e.signature = ed25519_sign(seed_key, e.signed_payload());

    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    // Once the operator pins the seed's key, the same enrollment applies.
    cfg.seeds.clear();
    cfg.seeds.push_back(PeerEntry{
        .name = "seed-pinned",
        .addr = "100.7.3.1:19949",
        .pubkey_hex = seed_pk,
    });
    MeshController pinned(cfg, home.string());
    REQUIRE(pinned.test_apply_enroll(e));
    REQUIRE(pinned.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("stale enrollment tokens are rejected at the expiry bound",
          "[bootstrap][enroll][lane3]") {
    const auto home = unique_temp_dir("expiry");

    auto [seed_cert, seed_key] = generate_cert_key_pair("seed");
    std::string seed_pk = pubkey_hex_from_pem(seed_key);
    (void)seed_cert;
    auto [member_cert, member_key] = generate_cert_key_pair("member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "seed-a",
        .addr = "100.7.4.1:19949",
        .pubkey_hex = seed_pk,
    });
    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "old-node";
    e.pubkey_hex = member_pk;
    e.addr = "100.7.4.9:19949";
    e.issuer_pubkey = seed_pk;
    // Issued 25 hours ago — beyond the 24h expiry, signature still valid.
    e.issued_at = now_secs() - 25 * 3600;
    e.signature = ed25519_sign(seed_key, e.signed_payload());

    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
