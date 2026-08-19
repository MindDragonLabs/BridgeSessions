// test_bootstrap_enroll.cpp — signed mesh-directory enrollment (bootstrap).
//
// Verifies the crypto + wire roundtrip for `bs enroll`: a trusted issuer signs
// a new member's {name,pubkey,addr}; the signature verifies against the
// issuer's public key; tampering with any field breaks verification; and the
// DirectoryEnrollMsg serializes/deserializes losslessly over the frame codec.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

#include <string>

using namespace bs::mesh;

namespace {

// Build an enroll message and sign it with a freshly generated identity.
DirectoryEnrollMsg make_signed_enroll(const std::string& name,
                                      const std::string& pubkey,
                                      const std::string& addr) {
    // Generate an issuer identity in a throwaway dir.
    auto [cert_pem, key_pem] = generate_cert_key_pair("bootstrap-test-issuer");
    std::string issuer_pubkey = pubkey_hex_from_pem(key_pem);

    DirectoryEnrollMsg e;
    e.name = name;
    e.pubkey_hex = pubkey;
    e.addr = addr;
    e.issuer_pubkey = issuer_pubkey;
    e.issued_at = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    e.signature = ed25519_sign(key_pem, e.signed_payload());
    return e;
}

} // namespace

TEST_CASE("enroll signature verifies against issuer pubkey", "[bootstrap][enroll]") {
    auto e = make_signed_enroll("new-host", std::string(64, 'a'), "100.1.2.3:19949");
    REQUIRE_FALSE(e.signature.empty());
    REQUIRE(e.signature.size() == 64);
    REQUIRE(ed25519_verify(e.issuer_pubkey, e.signed_payload(), e.signature));
}

TEST_CASE("tampered enrollment fails verification", "[bootstrap][enroll]") {
    auto e = make_signed_enroll("new-host", std::string(64, 'b'), "100.1.2.3:19949");
    // Flip one field.
    e.addr = "100.1.2.4:19949";
    REQUIRE_FALSE(ed25519_verify(e.issuer_pubkey, e.signed_payload(), e.signature));

    // Tamper the signature bytes.
    auto e2 = make_signed_enroll("new-host", std::string(64, 'c'), "100.1.2.3:19949");
    e2.signature[0] ^= 0xFF;
    REQUIRE_FALSE(ed25519_verify(e2.issuer_pubkey, e2.signed_payload(), e2.signature));
}

TEST_CASE("enrollment survives wire roundtrip", "[bootstrap][enroll]") {
    auto e = make_signed_enroll("roundtrip-host", std::string(64, 'd'), "100.9.9.9:19949");
    // Encode + decode through the frame codec.
    auto frame = encode(e, CONTROL_STREAM_ID, /*allow_large=*/false);
    auto decoded = decode(frame);
    REQUIRE(std::holds_alternative<DirectoryEnrollMsg>(decoded));
    const auto& got = std::get<DirectoryEnrollMsg>(decoded);
    REQUIRE(got.name == e.name);
    REQUIRE(got.pubkey_hex == e.pubkey_hex);
    REQUIRE(got.addr == e.addr);
    REQUIRE(got.issuer_pubkey == e.issuer_pubkey);
    REQUIRE(got.issued_at == e.issued_at);
    REQUIRE(got.signature == e.signature);
    // Signature must still verify against the roundtripped payload.
    REQUIRE(ed25519_verify(got.issuer_pubkey, got.signed_payload(), got.signature));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
