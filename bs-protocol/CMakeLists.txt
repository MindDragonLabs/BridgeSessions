add_library(bs-protocol STATIC
    src/codec.cpp
)

target_include_directories(bs-protocol PUBLIC include)
target_link_libraries(bs-protocol PUBLIC ${ZSTD_TARGET} OpenSSL::SSL OpenSSL::Crypto)

# ── Tests ────────────────────────────────────────────────────────

add_executable(bs-protocol-tests
    tests/message_test.cpp
    tests/codec_test.cpp
)

target_include_directories(bs-protocol-tests PRIVATE include)
target_link_libraries(bs-protocol-tests PRIVATE bs-protocol Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bs-protocol-tests)

# ── Fuzz target (Phase 12) ───────────────────────────────────────

if(BUILD_FUZZER)
    add_executable(fuzz-decode
        tests/fuzz_decode.cpp
    )
    target_include_directories(fuzz-decode PRIVATE include)
    target_link_libraries(fuzz-decode PRIVATE bs-protocol)
endif()
