// fuzz_decode.cpp — libFuzzer harness for bs-protocol decode()
// Phase 12: random bytes → no crashes
//
// Build: cmake --preset fuzzer && cmake --build build/fuzzer
// Run:   ./build/fuzzer/bs-protocol/fuzz_decode -max_len=65535 -runs=1000000

#include <bsprotocol/codec.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Feed random bytes to decode() — it should either throw a std::runtime_error
    // (invalid frame) or return a valid Message.
    try {
        auto msg = bs::protocol::decode(std::span<const uint8_t>(data, size));
        // Sanity: if decode() succeeds, the result should be a well-formed Message
        // and message_type() should return a valid enum value
        auto mt = message_type(msg);
        (void)mt; // silence unused warning
    } catch (const std::exception&) {
        // Expected: invalid frames throw std::runtime_error
    }
    return 0;  // non-zero return would be reserved for future use
}
