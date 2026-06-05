cmake_minimum_required(VERSION 3.25)
project(bridgesessions VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── MSVC-specific ──────────────────────────────────────────────
if(MSVC)
    add_compile_options(/std:c++latest /EHsc /W4 /wd4068 /wd4146 /wd4244 /wd4267 /wd4996)
endif()

# ── Dependencies ────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(zstd REQUIRED)
find_package(Catch2 REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog REQUIRED)

# Resolve zstd target name
if(TARGET zstd::libzstd_static)
    set(ZSTD_TARGET zstd::libzstd_static)
else()
    set(ZSTD_TARGET zstd::libzstd)
endif()

include(CTest)
enable_testing()

# ── Single-file mesh binary ────────────────────────────────────
add_executable(bridgesessions bridgesessions.cpp)
target_link_libraries(bridgesessions
    OpenSSL::SSL OpenSSL::Crypto
    ${ZSTD_TARGET}
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    spdlog::spdlog
)
if(WIN32)
    target_link_libraries(bridgesessions ws2_32.lib)
endif()

# ── Tests ──────────────────────────────────────────────────────
file(GLOB TEST_SOURCES "tests/*.cpp")
add_executable(bridgesessions-tests
    bridgesessions.cpp
    ${TEST_SOURCES}
)
target_compile_definitions(bridgesessions-tests PRIVATE BS_TESTING)
target_link_libraries(bridgesessions-tests
    OpenSSL::SSL OpenSSL::Crypto
    ${ZSTD_TARGET}
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    spdlog::spdlog
    Catch2::Catch2WithMain
)
if(WIN32)
    target_link_libraries(bridgesessions-tests ws2_32.lib)
endif()

# Register Catch2 tests
include(Catch)
catch_discover_tests(bridgesessions-tests)
