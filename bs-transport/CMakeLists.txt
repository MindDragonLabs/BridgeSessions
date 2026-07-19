add_library(bs-transport STATIC
    src/tls.cpp
    src/frame_io.cpp
)

target_include_directories(bs-transport PUBLIC include)
target_link_libraries(bs-transport PUBLIC bs-protocol OpenSSL::SSL OpenSSL::Crypto)

# ── Tests ────────────────────────────────────────────────────────

add_executable(bs-transport-tests
    tests/tls_test.cpp
    tests/e2e_test.cpp
)

target_include_directories(bs-transport-tests PRIVATE include)
target_link_libraries(bs-transport-tests PRIVATE bs-transport Catch2::Catch2WithMain)

option(BUILD_REMOTE_E2E "Build the opt-in, environment-configured remote E2E test" OFF)
if(BUILD_REMOTE_E2E)
    add_executable(bs-remote-e2e
        tests/remote_e2e_test.cpp
    )
    target_include_directories(bs-remote-e2e PRIVATE include)
    target_link_libraries(bs-remote-e2e PRIVATE bs-transport Catch2::Catch2WithMain)
endif()

# ── Benchmark ─────────────────────────────────────────────────────

add_executable(bs-transport-bench
    tests/bench_frame_io.cpp
)

target_include_directories(bs-transport-bench PRIVATE include)
target_link_libraries(bs-transport-bench PRIVATE bs-transport Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(bs-transport-tests)
catch_discover_tests(bs-transport-bench)
