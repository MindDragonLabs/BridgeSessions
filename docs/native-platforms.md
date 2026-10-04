# Native platform decisions — phase 0

This lane is a new, optional native subproject. It does not modify the parent
CMakeLists.txt, BridgePanel, BSMenubar, mesh protocol, or C++ backend headers.
The parent can opt in later with add_subdirectory(native).

## Common boundary

Every shell owns only presentation and platform lifecycle. The shared
bridge_native_core library owns URL parsing, OS socket I/O, OpenSSL TLS, HTTP
parsing/limits, bearer authorization, JSON requests, base64 terminal bytes,
API errors, cancellation, and incremental UTF-8 decoding. Native applications
talk only to BridgePanel /api/v1; they never link or speak the mesh protocol.

The smoke command reads its credential only from environment variables.
Production shells must use platform secure storage and inject a short-lived
per-device bearer credential into the core. A token is never put
in a URL, log line, crash string, or query parameter. HTTPS always validates
the certificate chain and hostname; there is no verify-disabled mode. Plain
HTTP is accepted only for loopback development/fixture use. Remote/tailnet
operation requires HTTPS.

## Shell matrix

| Platform | Phase-0 shell | Initial target | Distribution/signing decision |
| --- | --- | --- | --- |
| macOS | AppKit + Objective-C++ (native/shells/macos/main.mm) | macOS 13+, arm64 and x86_64 | Direct notarized DMG; Developer ID application signing and notarization are release gates, not done in this lane. |
| Windows | Win32 C++ (native/shells/windows/main.cpp) | Windows 10 1809+, x64 | Direct signed installer/zip; Authenticode signing is a release gate. No WinUI/Qt runtime. |
| Linux | Terminal-native C++ shell (native/shells/linux/main.cpp, with native_phase0_smoke for the full proof) | x86_64 glibc 2.31+ first; arm64 is a follow-up artifact | Direct tarball. The phase-0 toolkit decision is terminal-native: it is useful on servers and adds no GUI family. GTK4 is the later desktop candidate only if already present in the target distribution; it is not added here. |
| iOS | UIKit + Objective-C++ (native/shells/ios/main.mm) | iOS 16+, arm64 | TestFlight first, then App Store. App Store signing/provisioning is a release gate. No cross-platform UI framework. |
| Android | NDK C++ plus explicit small JNI bridge (native/shells/android/bridge_jni.cpp) | API 26+, arm64-v8a first | Direct signed APK/AAB distribution for v1. Android Keystore-backed credential storage and release signing are later release gates. No Kotlin UI framework is required by the core. |

The Linux choice is deliberate dependency containment: a native GUI would
require choosing and shipping another UI family, while phase 0 needs a
functional authenticated client and is expected to run on headless machines.
The shell boundary leaves room for an optional GTK4 front end without making
GTK a shared-core dependency.

## Build and integration

Standalone Linux build (compiler parallelism is intentionally two):

    cmake -S native -B build/native -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DBS_NATIVE_BUILD_TESTS=ON
    cmake --build build/native --parallel 2
    ctest --test-dir build/native --output-on-failure

The standalone build resolves OpenSSL and nlohmann_json through CMake. The
native API follows frozen BridgePanel v1 routes: capabilities have no machine
argument, peers are queried separately, and sessions are queried for a
selected machine. Terminal dimensions are limited to 500 columns by 300 rows;
session input and output reads are limited to 65536 bytes. In a parent
integration build, reuse the configured OpenSSL targets
(`BS_OPENSSL_SSL_TARGET` and `BS_OPENSSL_CRYPTO_TARGET`) and the existing
`nlohmann_json::nlohmann_json` target rather than resolving or fetching another
dependency copy. The core also depends on `BS_OPENSSL_BUILD_TARGET` when the
parent builds OpenSSL from source. The socket fixture is enabled only on POSIX
hosts; Windows builds the core, smoke executable, and Win32 shell without it.
Linux also builds the `bridge_native_linux` discovery shell. zstd, spdlog,
and fmt remain parent dependencies; the HTTP
client does not introduce another dependency family or require those
libraries.

Parent integration requirements:

1. Add add_subdirectory(native) only after the parent dependency targets are
   available.
2. Keep bridge_native_core, native_phase0_smoke, and platform target names out
   of release packaging until platform review is complete.
3. Supply secure-storage adapters and platform signing/provisioning outside
   this phase-0 core.
4. Run the canonical macOS build/run on a Mac. This Linux checkout cannot claim
   macOS verification.

## Open platform proof

The Linux fixture test binds only 127.0.0.1 and uses a disposable bearer token.
The actual smoke proof needs a running BridgePanel with the frozen API:

    BRIDGEPANEL_URL=https://panel.example.test \
    BRIDGEPANEL_TOKEN='device-token-from-secure-storage' \
    BRIDGEPANEL_MACHINE='approved-machine' \
    ./build/native/native_phase0_smoke

Do not substitute a production token in source, shell history, a URL, or a
saved config file. If the sandbox cannot bind a fixture port, the parent
should run native_core_tests in its normal local harness.
