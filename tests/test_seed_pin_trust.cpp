// test_seed_pin_trust.cpp — regression tests for the 26.09.16 audit fixes.
//
// Covers two production incidents (AUDIT-26-09-16-windows.md F1/F3 + the
// devin-mac live join on macmini, 2026-09-16):
//
//   1. F3 root cause: a peer pinned via `seed ... pubkey=` in the config was
//      NOT accepted inbound — server_cert_verify_cb consulted only
//      authorized_keys. Two mutually-pinned peers could never connect.
//      Fix: pinned seed keys are mirrored (rebuild_pinned_seed_keys) and
//      honored by the TLS accept path.
//
//   2. F1 self-apply: the node that SIGNS an enrollment (join host or
//      `bs enroll`) never applied its own enrollment locally unless it also
//      happened to pin itself as a seed (it doesn't). Fix: a node always
//      trusts enrollments it issued itself (issuer_pubkey == our_pubkey_).
//
//   3. F1 late-peer replay: enrollments are remembered (bounded) and
//      re-sent to peers that connect after the one-shot broadcast.
//      remember_pending_enroll must bound the deque.
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
        (std::string("bs_seedpin_") + label + "_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

uint64_t now_secs() {
    return static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

} // namespace

TEST_CASE("self-issued enrollment applies even when the issuer is not its own seed",
          "[bootstrap][enroll][f1][self-apply]") {
    const auto home = unique_temp_dir("selfapply");

    // The controller's OWN identity is generated on construction into home.
    // A second identity plays the new member.
    auto [member_cert, member_key] = generate_cert_key_pair("member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    // Deliberately NO seeds: the issuer does not pin itself (the normal case
    // for a join host / `bs enroll` issuer). Before the fix this configuration
    // rejected the issuer's own enrollment with enroll_rejected_issuer_not_seed.
    MeshController controller(cfg, home.string());

    // Our own pubkey (what make_directory_enroll would stamp as issuer).
    std::string our_pk;
    {
        std::ifstream pf(home / "id_ed25519.pub");
        std::getline(pf, our_pk);
        while (!our_pk.empty() && (our_pk.back() == '\r' || our_pk.back() == '\n'))
            our_pk.pop_back();
    }
    REQUIRE_FALSE(our_pk.empty());

    // Sign with OUR key via the production helper (uses the controller's home).
    DirectoryEnrollMsg e = controller.test_make_enroll("late-member", member_pk,
                                                       "100.9.9.9:19949");
    REQUIRE(e.issuer_pubkey == our_pk);

    // The fix: apply_directory_enroll accepts our own signature.
    REQUIRE(controller.test_apply_enroll(e));
    REQUIRE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("foreign non-seed issuer is still rejected (self-apply is not a hole)",
          "[bootstrap][enroll][f1][negative]") {
    const auto home = unique_temp_dir("foreignissuer");

    auto [member_cert, member_key] = generate_cert_key_pair("m2");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;
    auto [stranger_cert, stranger_key] = generate_cert_key_pair("stranger");
    std::string stranger_pk = pubkey_hex_from_pem(stranger_key);
    (void)stranger_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "stranger-vouched";
    e.pubkey_hex = member_pk;
    e.addr = "100.1.1.1:19949";
    e.issuer_pubkey = stranger_pk;   // neither us nor a pinned seed
    e.issued_at = now_secs();
    e.signature = ed25519_sign(stranger_key, e.signed_payload());

    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("relayed enrollment enters the replay queue (Greptile P1)",
          "[bootstrap][enroll][f1][relay][replay]") {
    const auto home = unique_temp_dir("relayreplay");

    // A controller that will PLAY the relay: it has no seeds and no knowledge
    // of the issuer, but applies a valid self-issued enrollment (the relay
    // recipient path). remember_pending_enroll must then hold the entry so a
    // later-connecting peer still receives it.
    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    MeshController controller(cfg, home.string());

    std::string our_pk;
    {
        std::ifstream pf(home / "id_ed25519.pub");
        std::getline(pf, our_pk);
        while (!our_pk.empty() && (our_pk.back() == '\\r' || our_pk.back() == '\\n'))
            our_pk.pop_back();
    }
    REQUIRE_FALSE(our_pk.empty());

    auto [member_cert, member_key] = generate_cert_key_pair("relay-member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    DirectoryEnrollMsg e = controller.test_make_enroll("relay-member", member_pk,
                                                       "100.10.10.10:19949");
    REQUIRE(e.issuer_pubkey == our_pk);

    // Empty before the relay.
    REQUIRE(controller.test_pending_enroll_count() == 0);

    // Apply (relay path) then remember — the production relay does exactly this.
    REQUIRE(controller.test_apply_enroll(e));
    controller.test_remember_pending_enroll(e);

    REQUIRE(controller.test_pending_enroll_count() == 1);
    REQUIRE(controller.test_pending_enroll_contains(member_pk));

    fs::remove_all(home);
}

TEST_CASE("rebuild_pinned_seed_keys mirrors valid seed pins and skips junk",
          "[bootstrap][enroll][f3][pins]") {
    const auto home = unique_temp_dir("pins");

    auto [peer_cert, peer_key] = generate_cert_key_pair("peer");
    std::string peer_pk = pubkey_hex_from_pem(peer_key);
    (void)peer_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{.name = "good-pin", .addr = "10.0.0.1:19949",
                                  .pubkey_hex = peer_pk});
    cfg.seeds.push_back(PeerEntry{.name = "no-pin", .addr = "10.0.0.2:19949",
                                  .pubkey_hex = ""});
    cfg.seeds.push_back(PeerEntry{.name = "junk-pin", .addr = "10.0.0.3:19949",
                                  .pubkey_hex = "zzzz"});   // not hex/64

    MeshController controller(cfg, home.string());

    // The pinned key must be trusted via the cached check (seed pin)…
    REQUIRE(controller.test_is_trusted_pubkey(peer_pk));
    // …and the raw mirror built for the TLS accept callback has exactly the
    // one valid entry.
    REQUIRE(controller.test_pinned_seed_key_count() == 1);

    fs::remove_all(home);
}

int main(int argc, char** argv) {
    return Catch::Session().run(argc, argv);
}
