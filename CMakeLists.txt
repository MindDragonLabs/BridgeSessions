cmake_minimum_required(VERSION 3.25)
project(bridgesessions VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BS_ENABLE_NAT "Enable miniupnpc NAT traversal" OFF)
option(BS_ENABLE_WEBRTC "Enable libdatachannel WebRTC transport" OFF)
option(BS_ENABLE_DHT "Enable DHT prototype" OFF)

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
target_include_directories(bridgesessions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT BS_ENABLE_NAT)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_NAT)
endif()
if(NOT BS_ENABLE_WEBRTC)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_WEBRTC)
endif()
if(NOT BS_ENABLE_DHT)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_DHT)
endif()
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
include(Catch)
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${TEST_NAME} PRIVATE BS_TESTING BS_NO_NAT BS_NO_WEBRTC BS_NO_DHT)
    target_link_libraries(${TEST_NAME}
        OpenSSL::SSL OpenSSL::Crypto
        ${ZSTD_TARGET}
        nlohmann_json::nlohmann_json
        CLI11::CLI11
        spdlog::spdlog
        Catch2::Catch2
    )
    if(WIN32)
        target_link_libraries(${TEST_NAME} ws2_32.lib)
    endif()
    catch_discover_tests(${TEST_NAME})
endforeach()
