# ── Hermetic dependency resolution ───────────────────────────────────────────
#
# This file is the single source of truth for every third-party dependency of
# the `bridgesessions` binary and its tests. A fresh clone must build with no
# machine-local edits and no hand-installed packages.
#
# BS_DEPS_MODE selects where dependencies come from:
#
#   fetch   (default) Pin and build every dependency from source. Non-system
#                     libraries are linked statically, so the resulting binary
#                     runs on any host with a compatible libc. This is what
#                     release artifacts use on every platform.
#   system            Use find_package() against packages installed on the
#                     build host. Fast for day-to-day development; the binary
#                     then depends on that host's library sonames.
#   auto              Try system packages first, fall back to fetch per
#                     dependency. Convenient, but not reproducible.
#
# OpenSSL is the one exception. BS_OPENSSL selects it independently:
#
#   system  (default) find_package(OpenSSL). libssl/libcrypto are ABI-stable
#                     across every supported distro (libssl.so.3 on Debian 12+,
#                     Ubuntu 22.04+, Arch), so this keeps builds fast.
#   fetch             Build OpenSSL from source and link it statically. Slow
#                     (several minutes) but removes the last runtime library
#                     dependency beyond libc.
#
# Every dependency below is pinned to an exact upstream tag. Bumping a pin is a
# deliberate, reviewable change — that is the point.

set(BS_DEPS_MODE "fetch" CACHE STRING
    "Where dependencies come from: fetch | system | auto")
set_property(CACHE BS_DEPS_MODE PROPERTY STRINGS fetch system auto)

set(BS_OPENSSL "system" CACHE STRING "OpenSSL source: system | fetch")
set_property(CACHE BS_OPENSSL PROPERTY STRINGS system fetch)

option(BS_STATIC_DEPS
       "Link non-system dependencies statically for portable artifacts" ON)

if(NOT BS_DEPS_MODE MATCHES "^(fetch|system|auto)$")
    message(FATAL_ERROR "BS_DEPS_MODE must be fetch, system, or auto")
endif()
if(NOT BS_OPENSSL MATCHES "^(system|fetch)$")
    message(FATAL_ERROR "BS_OPENSSL must be system or fetch")
endif()

# Pinned upstream versions. Keep alphabetical.
set(BS_CLI11_TAG        "v2.4.2"   CACHE STRING "CLI11 pin")
set(BS_CATCH2_TAG       "v3.8.0"   CACHE STRING "Catch2 pin")
set(BS_JSON_TAG         "v3.11.3"  CACHE STRING "nlohmann/json pin")
set(BS_SPDLOG_TAG       "v1.15.3"  CACHE STRING "spdlog pin")
set(BS_ZSTD_TAG         "v1.5.6"   CACHE STRING "zstd pin")
set(BS_OPENSSL_TAG      "openssl-3.0.16" CACHE STRING "OpenSSL pin")

include(FetchContent)

# Decide, per dependency, whether to use a system package.
# Sets <prefix>_USE_SYSTEM to TRUE/FALSE in the caller's scope.
function(bs_resolve_source prefix pkgname)
    if(BS_DEPS_MODE STREQUAL "fetch")
        set(${prefix}_USE_SYSTEM FALSE PARENT_SCOPE)
    elseif(BS_DEPS_MODE STREQUAL "system")
        set(${prefix}_USE_SYSTEM TRUE PARENT_SCOPE)
    else() # auto
        find_package(${pkgname} QUIET)
        if(${pkgname}_FOUND)
            set(${prefix}_USE_SYSTEM TRUE PARENT_SCOPE)
        else()
            set(${prefix}_USE_SYSTEM FALSE PARENT_SCOPE)
        endif()
    endif()
endfunction()

# Declare a source dependency fetched from GitHub at a pinned tag.
function(bs_fetch name tag)
    FetchContent_Declare(${name}
        GIT_REPOSITORY "${ARGN}"
        GIT_TAG        "${tag}"
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
endfunction()

# Static linking only makes sense when we control how a dependency is built.
if(BS_STATIC_DEPS AND BS_DEPS_MODE STREQUAL "fetch")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
endif()

# ── OpenSSL ─────────────────────────────────────────────────────────────────
if(BS_OPENSSL STREQUAL "fetch" AND NOT BS_DEPS_MODE STREQUAL "system")
    bs_fetch(openssl "${BS_OPENSSL_TAG}"
             "https://github.com/openssl/openssl.git")
    FetchContent_GetProperties(openssl)
    if(NOT openssl_POPULATED)
        FetchContent_Populate(openssl)
    endif()
    # OpenSSL ships Perl Configure, not CMake. Build it out of band and expose
    # the results as ordinary imported targets so nothing downstream cares.
    set(_ossl_prefix "${CMAKE_CURRENT_BINARY_DIR}/openssl-install")
    set(_ossl_no_asm "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_ossl_target "linux-aarch64")
    else()
        set(_ossl_target "linux-x86_64")
    endif()
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_ossl_target "darwin-arm64")
        else()
            set(_ossl_target "darwin64-x86_64-cc")
        endif()
    endif()
    add_custom_command(
        OUTPUT "${_ossl_prefix}/lib/libssl.a"
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${_ossl_prefix}"
        COMMAND ./Configure ${_ossl_target} no-shared no-tests no-docs
                no-apps --prefix=${_ossl_prefix} --libdir=lib
        COMMAND ${CMAKE_MAKE_PROGRAM} -j
        COMMAND ${CMAKE_MAKE_PROGRAM} install_sw
        WORKING_DIRECTORY "${openssl_SOURCE_DIR}"
        COMMENT "Building pinned OpenSSL ${BS_OPENSSL_TAG} (static)"
        VERBATIM)
    add_custom_target(bs_openssl_build DEPENDS "${_ossl_prefix}/lib/libssl.a")
    add_library(bs_openssl_ssl STATIC IMPORTED GLOBAL)
    add_library(bs_openssl_crypto STATIC IMPORTED GLOBAL)
    set_target_properties(bs_openssl_ssl PROPERTIES
        IMPORTED_LOCATION "${_ossl_prefix}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_ossl_prefix}/include"
        INTERFACE_LINK_LIBRARIES bs_openssl_crypto)
    set_target_properties(bs_openssl_crypto PROPERTIES
        IMPORTED_LOCATION "${_ossl_prefix}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_ossl_prefix}/include")
    add_dependencies(bs_openssl_ssl bs_openssl_build)
    if(UNIX AND NOT APPLE)
        set_property(TARGET bs_openssl_ssl APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES dl pthread)
    endif()
    set(BS_OPENSSL_SSL_TARGET bs_openssl_ssl)
    set(BS_OPENSSL_CRYPTO_TARGET bs_openssl_crypto)
    message(STATUS "deps: OpenSSL from source (${BS_OPENSSL_TAG}, static)")
else()
    find_package(OpenSSL REQUIRED)
    set(BS_OPENSSL_SSL_TARGET OpenSSL::SSL)
    set(BS_OPENSSL_CRYPTO_TARGET OpenSSL::Crypto)
    message(STATUS "deps: OpenSSL ${OPENSSL_VERSION} from system packages")
endif()
if(NOT BS_OPENSSL_SSL_TARGET)
    message(FATAL_ERROR
        "OpenSSL was not found. Install the development headers:\n"
        "  Debian/Ubuntu:  apt-get install libssl-dev\n"
        "  Arch:           pacman -S openssl\n"
        "  macOS:          brew install openssl@3\n"
        "  Windows/mingw:  run scripts/ci-win-deps.sh <prefix> and pass\n"
        "                  -DOPENSSL_ROOT_DIR=<prefix>\n"
        "Or build OpenSSL from source with -DBS_OPENSSL=fetch.")
endif()

# ── zstd ────────────────────────────────────────────────────────────────────
bs_resolve_source(ZSTD zstd)
if(ZSTD_USE_SYSTEM)
    find_package(zstd REQUIRED)
    if(TARGET zstd::libzstd_static)
        set(ZSTD_TARGET zstd::libzstd_static)
    else()
        set(ZSTD_TARGET zstd::libzstd)
    endif()
    message(STATUS "deps: zstd from system packages")
else()
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    bs_fetch(zstd "${BS_ZSTD_TAG}" "https://github.com/facebook/zstd.git")
    # zstd has no CMakeLists.txt at the repo root; its canonical CMake entry is
    # build/cmake. FetchContent_MakeAvailable would silently configure nothing.
    FetchContent_GetProperties(zstd)
    if(NOT zstd_POPULATED)
        FetchContent_Populate(zstd)
    endif()
    add_subdirectory("${zstd_SOURCE_DIR}/build/cmake"
                     "${zstd_BINARY_DIR}/cmake" EXCLUDE_FROM_ALL)

    add_library(bs_zstd INTERFACE)
    # zstd's exported targets do not always carry the public include dir, which
    # surfaces as "zstd.h: No such file or directory".
    target_include_directories(bs_zstd SYSTEM INTERFACE
        "${zstd_SOURCE_DIR}/lib" "${zstd_BINARY_DIR}/cmake/lib")
    if(TARGET libzstd_static)
        target_link_libraries(bs_zstd INTERFACE libzstd_static)
    elseif(TARGET zstd::libzstd_static)
        target_link_libraries(bs_zstd INTERFACE zstd::libzstd_static)
    elseif(TARGET libzstd_shared)
        target_link_libraries(bs_zstd INTERFACE libzstd_shared)
    else()
        message(FATAL_ERROR
            "zstd configured at ${zstd_BINARY_DIR} but defines no library target. "
            "Remove the build directory and reconfigure.")
    endif()
    set(ZSTD_TARGET bs_zstd)
    message(STATUS "deps: zstd ${BS_ZSTD_TAG} from source")
endif()

# ── spdlog ──────────────────────────────────────────────────────────────────
# Built with its own bundled fmt. Using Homebrew/system spdlog instead drags in
# an external shared fmt whose soname differs per distro, which is exactly how
# the 26.09.10-r1 Linux artifact ended up needing libspdlog.so.1 + libfmt.so.8.
bs_resolve_source(SPDLOG spdlog)
if(SPDLOG_USE_SYSTEM)
    find_package(spdlog REQUIRED)
    message(STATUS "deps: spdlog from system packages")
else()
    set(SPDLOG_BUILD_SHARED   OFF  CACHE BOOL "" FORCE)
    set(SPDLOG_FMT_EXTERNAL   OFF  CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS    OFF  CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE  OFF  CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL        OFF  CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH    OFF  CACHE BOOL "" FORCE)
    bs_fetch(spdlog "${BS_SPDLOG_TAG}" "https://github.com/gabime/spdlog.git")
    FetchContent_MakeAvailable(spdlog)
    message(STATUS "deps: spdlog ${BS_SPDLOG_TAG} from source (bundled fmt)")
endif()

# ── nlohmann/json (header-only) ─────────────────────────────────────────────
bs_resolve_source(JSON nlohmann_json)
if(JSON_USE_SYSTEM)
    find_package(nlohmann_json 3.2.0 REQUIRED)
    message(STATUS "deps: nlohmann/json from system packages")
else()
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install    OFF CACHE BOOL "" FORCE)
    set(JSON_MultipleHeaders ON CACHE BOOL "" FORCE)
    bs_fetch(nlohmann_json "${BS_JSON_TAG}"
             "https://github.com/nlohmann/json.git")
    FetchContent_MakeAvailable(nlohmann_json)
    message(STATUS "deps: nlohmann/json ${BS_JSON_TAG} from source")
endif()

# ── CLI11 (header-only) ─────────────────────────────────────────────────────
bs_resolve_source(CLI11 CLI11)
if(CLI11_USE_SYSTEM)
    find_package(CLI11 REQUIRED)
    message(STATUS "deps: CLI11 from system packages")
else()
    set(CLI11_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(CLI11_SINGLE_FILE    OFF CACHE BOOL "" FORCE)
    bs_fetch(CLI11 "${BS_CLI11_TAG}" "https://github.com/CLIUtils/CLI11.git")
    FetchContent_MakeAvailable(CLI11)
    message(STATUS "deps: CLI11 ${BS_CLI11_TAG} from source")
endif()

# ── Catch2 (tests only) ─────────────────────────────────────────────────────
if(BUILD_TESTING)
    bs_resolve_source(CATCH2 Catch2)
    if(CATCH2_USE_SYSTEM)
        find_package(Catch2 3 REQUIRED)
        message(STATUS "deps: Catch2 from system packages")
    else()
        set(CATCH_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_DOCS   OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
        bs_fetch(Catch2 "${BS_CATCH2_TAG}"
                 "https://github.com/catchorg/Catch2.git")
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
        message(STATUS "deps: Catch2 ${BS_CATCH2_TAG} from source")
    endif()
endif()
