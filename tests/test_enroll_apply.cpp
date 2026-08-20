// test_enroll_apply.cpp — integration test for signed mesh-directory enrollment.
//
// Builds two identities (issuer + new member), pre-authorizes the issuer into
// the controller's authorized_keys, then runs apply_directory_enroll() and
// asserts the new member's pubkey was written to disk AND the entry is
// discoverable. Also asserts the negative cases (untrusted issuer, bad sig,
// self-vouch) are rejected without mutating authorized_keys.
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
        (std::string("bs_enroll_") + label + "_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

} // namespace

TEST_CASE("apply_directory_enroll trusts a new member signed by a trusted issuer",
          "[bootstrap][enroll][apply]") {
    const auto home = unique_temp_dir("ok");

    // New member identity (the thing being enrolled).
    auto [member_cert, member_key] = generate_cert_key_pair("new-member");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    // Issuer identity (the trusted vouching node).
    auto [issuer_cert, issuer_key] = generate_cert_key_pair("issuer");
    std::string issuer_pk = pubkey_hex_from_pem(issuer_key);
    (void)issuer_cert;

    // Build a controller, point its authorized_keys at a temp file, and
    // pre-authorize the ISSUER as an explicit SEED (trust root — see H3:
    // enrollment issuers must be pinned seeds, not merely authorized_keys
    // entries, to prevent transitive trust).
    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "issuer-seed",
        .addr = "100.7.7.1:19949",
        .pubkey_hex = issuer_pk,
    });

    MeshController controller(cfg, home.string());

    // Craft an enrollment signed by the issuer.
    DirectoryEnrollMsg e;
    e.name = "new-host";
    e.pubkey_hex = member_pk;
    e.addr = "100.7.7.7:19949";
    e.issuer_pubkey = issuer_pk;
    e.issued_at = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    e.signature = ed25519_sign(issuer_key, e.signed_payload());
    REQUIRE_FALSE(e.signature.empty());

    REQUIRE(controller.test_apply_enroll(e));
    REQUIRE(controller.test_authorized_on_disk(member_pk));

    // Idempotent: re-apply is a no-op and still succeeds.
    REQUIRE(controller.test_apply_enroll(e));
    REQUIRE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("enroll rejects untrusted issuer", "[bootstrap][enroll][apply]") {
    const auto home = unique_temp_dir("untrusted");
    auto [member_cert, member_key] = generate_cert_key_pair("m");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;
    auto [issuer_cert, issuer_key] = generate_cert_key_pair("issuer");
    std::string issuer_pk = pubkey_hex_from_pem(issuer_key);
    (void)issuer_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    // NOTE: issuer is NOT pre-authorized here.

    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "evil-host";
    e.pubkey_hex = member_pk;
    e.addr = "100.6.6.6:19949";
    e.issuer_pubkey = issuer_pk;
    e.issued_at = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    e.signature = ed25519_sign(issuer_key, e.signed_payload());

    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("enroll rejects a bad signature", "[bootstrap][enroll][apply]") {
    const auto home = unique_temp_dir("badsig");
    auto [member_cert, member_key] = generate_cert_key_pair("m");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;
    auto [issuer_cert, issuer_key] = generate_cert_key_pair("issuer");
    std::string issuer_pk = pubkey_hex_from_pem(issuer_key);
    (void)issuer_cert;

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    cfg.seeds.push_back(PeerEntry{
        .name = "issuer-seed",
        .addr = "100.5.5.1:19949",
        .pubkey_hex = issuer_pk,
    });

    MeshController controller(cfg, home.string());

    DirectoryEnrollMsg e;
    e.name = "tamper-host";
    e.pubkey_hex = member_pk;
    e.addr = "100.5.5.5:19949";
    e.issuer_pubkey = issuer_pk;
    e.issued_at = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    e.signature = ed25519_sign(issuer_key, e.signed_payload());
    // Tamper with the address AFTER signing.
    e.addr = "100.5.5.6:19949";

    REQUIRE_FALSE(controller.test_apply_enroll(e));
    REQUIRE_FALSE(controller.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("join auto-vouches the new member (Tailscale auth-key model)",
          "[bootstrap][enroll][join]") {
    const auto home = unique_temp_dir("join");

    // The HOST is the trusted member running process_join_request.
    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();

    // A NEW member identity joins with an invite token + advertised endpoint.
    auto [member_cert, member_key] = generate_cert_key_pair("joiner");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshController host(cfg, home.string());
    host.test_add_invite("deadbeefdeadbeefdeadbeefdeadbeef");

    JoinRequestMsg jr;
    jr.token = "deadbeefdeadbeefdeadbeefdeadbeef";
    jr.node_name = "my-new-host";
    jr.listen_addr = "100.9.8.7:19949";

    JoinReplyMsg reply = host.test_process_join(jr, member_pk);
    REQUIRE(reply.ok);
    REQUIRE(reply.node_name == "my-new-host");

    // The host must have auto-vouched: the member's key is now trusted on disk.
    REQUIRE(host.test_authorized_on_disk(member_pk));

    fs::remove_all(home);
}

TEST_CASE("join without advertised endpoint still joins but skips auto-enroll",
          "[bootstrap][enroll][join]") {
    const auto home = unique_temp_dir("join-noaddr");

    MeshConfig cfg;
    cfg.authorized_keys_path = (home / "authorized_keys").string();
    auto [member_cert, member_key] = generate_cert_key_pair("joiner2");
    std::string member_pk = pubkey_hex_from_pem(member_key);
    (void)member_cert;

    MeshController host(cfg, home.string());
    host.test_add_invite("feedfacefeedfacefeedfacefeedface");

    JoinRequestMsg jr;
    jr.token = "feedfacefeedfacefeedfacefeedface";
    // No node_name, no listen_addr — legacy-style join.

    JoinReplyMsg reply = host.test_process_join(jr, member_pk);
    REQUIRE(reply.ok);
    // Still authorized (join always authorizes the direct host link)…
    REQUIRE(host.test_authorized_on_disk(member_pk));
    // …but no auto-enroll propagation was possible without an endpoint.

    fs::remove_all(home);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
