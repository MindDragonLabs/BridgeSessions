add_executable(bs-server
    src/main.cpp
    src/session.cpp
    src/pty.cpp
    src/session_manager.cpp
)

# keygen.cpp shared with bs-client
target_sources(bs-server PRIVATE ${CMAKE_SOURCE_DIR}/bs-client/src/keygen.cpp)

target_include_directories(bs-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/bs-client/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-transport/include
)

# nlohmann/json for persistence (Phase 10)
if(nlohmann_json_FOUND)
    target_link_libraries(bs-server PRIVATE nlohmann_json::nlohmann_json)
endif()

# CLI11 for argument parsing (Phase 12)
if(CLI11_FOUND)
    target_link_libraries(bs-server PRIVATE CLI11::CLI11)
endif()

# spdlog for JSON logging (Phase 12)
if(spdlog_FOUND)
    target_link_libraries(bs-server PRIVATE spdlog::spdlog)
endif()

target_link_libraries(bs-server PRIVATE
    bs-transport
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ZSTD_TARGET}
)

# ── Server Tests ────────────────────────────────────────────────
add_executable(bs-server-tests
    tests/ring_buffer_test.cpp
    tests/session_manager_test.cpp
    tests/persistence_test.cpp
    tests/pty_test.cpp
    src/session.cpp
    src/pty.cpp
    src/session_manager.cpp
)

target_include_directories(bs-server-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../bs-transport/include
)

target_link_libraries(bs-server-tests PRIVATE
    bs-transport
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ZSTD_TARGET}
    Catch2::Catch2WithMain
)

if(nlohmann_json_FOUND)
    target_link_libraries(bs-server-tests PRIVATE nlohmann_json::nlohmann_json)
endif()

catch_discover_tests(bs-server-tests)
