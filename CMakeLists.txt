cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BRIDGESESSIONS_VERSION
     LIMIT_COUNT 1)
string(STRIP "${BRIDGESESSIONS_VERSION}" BRIDGESESSIONS_VERSION)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+"
       BRIDGESESSIONS_NUMERIC_VERSION "${BRIDGESESSIONS_VERSION}")
if(NOT BRIDGESESSIONS_NUMERIC_VERSION)
    message(FATAL_ERROR "VERSION must start with MAJOR.MINOR.PATCH")
endif()
project(bridgesessions VERSION ${BRIDGESESSIONS_NUMERIC_VERSION} LANGUAGES C CXX)
add_compile_definitions(BS_VERSION="${BRIDGESESSIONS_VERSION}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BS_ENABLE_NAT "Enable miniupnpc NAT traversal" OFF)
option(BS_ENABLE_WEBRTC "Enable libdatachannel WebRTC transport" OFF)
option(BS_ENABLE_DHT "Enable DHT prototype" OFF)
option(BS_CATCH2_APPLE_SHIM
       "Enable the legacy macOS shim for mismatched Catch2 headers/archive" OFF)

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

# ── Core library (header-only) + binary ──────────────────────────
add_executable(bridgesessions main.cpp bs-protocol.h)
# Console subsystem on Windows: this is a CLI/daemon (int main), not WinMain GUI.
# WIN32_EXECUTABLE TRUE would require WinMain and hides stdout from shells.
if(APPLE)
    enable_language(OBJCXX)
    target_sources(bridgesessions PRIVATE macos-capture.mm)
    target_link_libraries(bridgesessions
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework ScreenCaptureKit"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
        "-framework ApplicationServices")
endif()
if(NOT BS_ENABLE_NAT)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_NAT)
endif()
if(NOT BS_ENABLE_WEBRTC)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_WEBRTC)
endif()
if(NOT BS_ENABLE_DHT)
    target_compile_definitions(bridgesessions PRIVATE BS_NO_DHT)
endif()
# ── Strip build-machine paths from release binaries ─────────────
# __FILE__ and assert messages embed the compile-time source path.
# -ffile-prefix-map rewrites them so dist/ binaries don't leak /home/<user>
# or /Users/<user>. Applied to the main binary only (not tests).
# Map common local home prefixes so CI/dev usernames never bake into dist/.
# Prefer CMAKE_SOURCE_DIR + $ENV{HOME}; avoid hardcoding a real account name.
target_compile_options(bridgesessions PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<BOOL:$ENV{HOME}>>:-ffile-prefix-map=$ENV{HOME}=.>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffile-prefix-map=/home/agent=.>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffile-prefix-map=/Users/builder=.>)

target_link_libraries(bridgesessions
    OpenSSL::SSL OpenSSL::Crypto
    ${ZSTD_TARGET}
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    spdlog::spdlog
)
if(WIN32)
    target_link_libraries(bridgesessions ws2_32.lib gdiplus.lib gdi32.lib)
endif()

add_test(NAME cli_quick_connect_help COMMAND bridgesessions --help)
set_tests_properties(cli_quick_connect_help PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[peer\\] \\[session\\]")
add_test(NAME cli_quick_connect_requires_trust
    COMMAND bridgesessions bs-untrusted-test shell)
set_tests_properties(cli_quick_connect_requires_trust PROPERTIES
    WILL_FAIL TRUE
    PASS_REGULAR_EXPRESSION "Refusing untrusted first contact")
add_test(NAME cli_keygen_refuses_identity_overwrite
    COMMAND ${CMAKE_COMMAND}
        -DBS_BINARY=$<TARGET_FILE:bridgesessions>
        -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/keygen-refuse-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keygen_refuse.cmake)

# ── Tests ──────────────────────────────────────────────────────
file(GLOB TEST_SOURCES "tests/*.cpp")
include(Catch)
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    if(WIN32 AND TEST_NAME STREQUAL "test_mesh_responsiveness")
        continue()
    endif()
    add_executable(${TEST_NAME} ${TEST_SRC})
    if(APPLE AND BS_CATCH2_APPLE_SHIM)
        target_compile_options(${TEST_NAME} PRIVATE
            -include ${CMAKE_CURRENT_SOURCE_DIR}/tests/catch2_apple_shim.h)
    endif()
    target_compile_definitions(${TEST_NAME} PRIVATE BS_TESTING BS_NO_NAT BS_NO_WEBRTC BS_NO_DHT
        BS_VERSION_FILE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
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
    if(APPLE)
        target_link_libraries(${TEST_NAME}
            "-framework CoreGraphics"
            "-framework ScreenCaptureKit"
            "-framework Foundation"
            "-framework ApplicationServices")
    endif()
    if(TEST_NAME STREQUAL "test_mesh" OR
       TEST_NAME STREQUAL "test_mesh_responsiveness")
        catch_discover_tests(${TEST_NAME}
            PROPERTIES RESOURCE_LOCK bridgesessions_ipc)
    else()
        catch_discover_tests(${TEST_NAME})
    endif()
endforeach()

# ── Python release-engineering tests ─────────────────────────────────
# Use the PATH-resolved python3 so the test runs with the interpreter that
# actually has pytest, rather than whichever version CMake found first.
find_program(BRIDGESESSIONS_PYTHON python3)
if(BRIDGESESSIONS_PYTHON)
    execute_process(
        COMMAND ${BRIDGESESSIONS_PYTHON} -c "import pytest"
        RESULT_VARIABLE BRIDGESESSIONS_PYTEST_AVAILABLE
        OUTPUT_QUIET ERROR_QUIET)
    if(BRIDGESESSIONS_PYTEST_AVAILABLE EQUAL 0)
        add_test(NAME release_python_tests
            COMMAND ${BRIDGESESSIONS_PYTHON} -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_script.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_regression_install.py" -v)
        set_tests_properties(release_python_tests PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")
        # Panel tests: typing latency, chained relay, daemon restart disconnect
        add_test(NAME panel_typing_latency
            COMMAND ${BRIDGESESSIONS_PYTHON} -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_typing_latency.py" -v
                --timeout=60)
        set_tests_properties(panel_typing_latency PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME panel_chained_relay
            COMMAND ${BRIDGESESSIONS_PYTHON} -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chained_relay.py" -v
                --timeout=120)
        set_tests_properties(panel_chained_relay PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME panel_daemon_restart_disconnect
            COMMAND ${BRIDGESESSIONS_PYTHON} -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_daemon_restart_disconnect.py" -v
                --timeout=60)
        set_tests_properties(panel_daemon_restart_disconnect PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
