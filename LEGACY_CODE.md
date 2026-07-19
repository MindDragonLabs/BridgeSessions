# Non-shipping modular code

BridgeSessions `2.0.6-alpha2` ships the single-file implementation in
`bridgesessions.cpp`, built by the root `CMakeLists.txt`.

The following tracked paths are an older modularization experiment and are **not
compiled, tested, packaged, or security-supported** by the 2.0.6-alpha2 root build:

- `main.cpp`
- `bs-client/`
- `bs-server/`
- `protocol/`
- `transport/`
- `session_manager_test.cpp`

They are preserved as design history rather than silently deleted. Release
archives exclude them through `.gitattributes`. Do not import code from these
paths into the shipping binary without a new security review, test integration,
and an explicit architecture decision.
