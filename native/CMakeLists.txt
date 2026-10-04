cmake_minimum_required(VERSION 3.25)
project(bridge_native_phase0 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BS_NATIVE_BUILD_MACOS "Build the AppKit phase-0 shell on macOS" ON)
option(BS_NATIVE_BUILD_WINDOWS "Build the Win32 phase-0 shell on Windows" ON)
option(BS_NATIVE_BUILD_TESTS "Build native core fixture tests" ON)

if(BS_OPENSSL_SSL_TARGET AND BS_OPENSSL_CRYPTO_TARGET)
    if(NOT TARGET ${BS_OPENSSL_SSL_TARGET} OR NOT TARGET ${BS_OPENSSL_CRYPTO_TARGET})
        message(FATAL_ERROR "Parent OpenSSL targets must exist before adding native")
    endif()
else()
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        find_package(OpenSSL REQUIRED)
    endif()
    set(BS_OPENSSL_SSL_TARGET OpenSSL::SSL)
    set(BS_OPENSSL_CRYPTO_TARGET OpenSSL::Crypto)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.2.0 REQUIRED)
endif()
include(CTest)

add_library(bridge_native_core STATIC
    src/client.cpp
    src/http.cpp
    src/utf8.cpp
    include/bridge_native/client.hpp
    include/bridge_native/http.hpp
    include/bridge_native/result.hpp
    include/bridge_native/utf8.hpp)
target_include_directories(bridge_native_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bridge_native_core PUBLIC ${BS_OPENSSL_SSL_TARGET} ${BS_OPENSSL_CRYPTO_TARGET} nlohmann_json::nlohmann_json)
if(BS_OPENSSL_BUILD_TARGET)
    add_dependencies(bridge_native_core ${BS_OPENSSL_BUILD_TARGET})
endif()
target_compile_features(bridge_native_core PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(bridge_native_core PUBLIC ws2_32)
endif()

add_executable(native_phase0_smoke src/smoke.cpp)
target_link_libraries(native_phase0_smoke PRIVATE bridge_native_core)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(bridge_native_linux shells/linux/main.cpp)
    target_link_libraries(bridge_native_linux PRIVATE bridge_native_core)
endif()

if(BS_NATIVE_BUILD_TESTS AND BUILD_TESTING AND UNIX)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(native_core_tests tests/native_core_tests.cpp)
    target_link_libraries(native_core_tests PRIVATE bridge_native_core Threads::Threads)
    if(MSVC)
        target_compile_options(native_core_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(native_core_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME native_core_fixture COMMAND native_core_tests)
    set_tests_properties(native_core_fixture PROPERTIES TIMEOUT 20)
endif()

if(APPLE AND BS_NATIVE_BUILD_MACOS)
    enable_language(OBJCXX)
    add_executable(bridge_native_macos MACOSX_BUNDLE shells/macos/main.mm)
    target_link_libraries(bridge_native_macos PRIVATE bridge_native_core
        "-framework AppKit" "-framework Foundation")
    target_compile_options(bridge_native_macos PRIVATE -fobjc-arc)
endif()

if(WIN32 AND BS_NATIVE_BUILD_WINDOWS)
    add_executable(bridge_native_win32 WIN32 shells/windows/main.cpp)
    target_link_libraries(bridge_native_win32 PRIVATE bridge_native_core user32)
endif()
