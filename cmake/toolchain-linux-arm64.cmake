# Linux/arm64 (aarch64) cross-compilation toolchain for bridgesessions.
#
# Override the sysroot when configuring if your target packages live outside the
# compiler's default search paths:
#   cmake -DBRIDGESESSIONS_SYSROOT=/path/to/aarch64-linux-gnu-sysroot ...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH "Cross C compiler" FORCE)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH "Cross C++ compiler" FORCE)
set(CMAKE_AR aarch64-linux-gnu-ar CACHE FILEPATH "Cross archiver" FORCE)
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib CACHE FILEPATH "Cross ranlib" FORCE)
set(CMAKE_STRIP aarch64-linux-gnu-strip CACHE FILEPATH "Cross strip" FORCE)

# Avoid try_run during compiler detection in cross builds.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Prefer config-mode packages from the target sysroot.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(BRIDGESESSIONS_SYSROOT "" CACHE PATH "Linux sysroot root for target packages")
if(BRIDGESESSIONS_SYSROOT)
    set(CMAKE_SYSROOT "${BRIDGESESSIONS_SYSROOT}")
    set(CMAKE_SYSROOT_COMPILE "${BRIDGESESSIONS_SYSROOT}")
    set(CMAKE_SYSROOT_LINK "${BRIDGESESSIONS_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${BRIDGESESSIONS_SYSROOT}")

    list(PREPEND CMAKE_PREFIX_PATH
        "${BRIDGESESSIONS_SYSROOT}/usr"
        "${BRIDGESESSIONS_SYSROOT}/usr/local"
    )

    # Package-specific hints for sysroot-installed config packages.
    set(OPENSSL_ROOT_DIR "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "OpenSSL root in sysroot" FORCE)
    set(zstd_ROOT "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "zstd root in sysroot" FORCE)
    set(Catch2_ROOT "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "Catch2 root in sysroot" FORCE)
    set(nlohmann_json_ROOT "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "nlohmann_json root in sysroot" FORCE)
    set(CLI11_ROOT "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "CLI11 root in sysroot" FORCE)
    set(spdlog_ROOT "${BRIDGESESSIONS_SYSROOT}/usr" CACHE PATH "spdlog root in sysroot" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
