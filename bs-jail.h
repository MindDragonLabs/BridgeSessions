// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-jail.h — Landlock enforcement for the session filesystem jail.
//
// Included at GLOBAL scope (after bs-protocol.h's namespace close) because it
// #includes C system headers directly (<sys/prctl.h> etc.). The policy model
// (FilesystemJailPolicy, jail_policy_from_env) lives in bs-jail-policy.h and
// is included inside the facade's namespace — keep the two halves in sync.
//
// Enforcement is Linux-only Landlock, BEST-EFFORT: if the syscall is
// unavailable we degrade to env markers only — a session must never fail to
// start because the jail could not be applied. See docs/session-jail.md.

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(__linux__)
// Non-Linux hosts: enforcement is a documented no-op (env markers only).
// Keep the API callable so bs-pty.h needs no platform branches.
inline bool landlock_available() { return false; }
namespace bs::mesh {
inline bool apply_filesystem_jail(const FilesystemJailPolicy& p) {
    (void)p;
    return false;  // caller proceeds WITHOUT the jail (availability degrade)
}
} // namespace bs::mesh
#else
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <initializer_list>

// ── Linux enforcement (Landlock, best-effort) ──────────────────────

namespace bs::mesh {

// Query the Landlock ABI version; >=1 means the LSM is usable.
inline bool landlock_available() {
    long ver = ::syscall(444 /* __NR_landlock_create_ruleset */, nullptr, 0,
                         1 /* LANDLOCK_CREATE_RULESET_VERSION */);
    return ver >= 1;
}

namespace detail {
// Minimal landlock UAPI definitions (linux/landlock.h may be absent).
constexpr unsigned long kLandlockRulePathBeneath = 1;
constexpr unsigned long kLandlockAccessFileWrite =
    0x0002 /* WRITE_FILE */ | 0x0008 /* REMOVE_FILE */ | 0x0010 /* REMOVE_DIR */ |
    0x0020 /* MAKE_CHAR */ | 0x0040 /* MAKE_DIR */ | 0x0080 /* MAKE_REG */ |
    0x0100 /* MAKE_SOCK */ | 0x0200 /* MAKE_FIFO */ | 0x0400 /* MAKE_BLOCK */ |
    0x0800 /* MAKE_SYM */;
struct LandlockPathBeneathAttr {
    unsigned long allowed_access;
    int parent_fd;
} __attribute__((packed));
} // namespace detail

// Apply the jail to the CURRENT process (call in the forkpty child, before
// exec). Non-reversible for the child. Returns false if enforcement could
// not be installed (caller proceeds WITHOUT the jail — availability degrade).
inline bool apply_filesystem_jail(const FilesystemJailPolicy& p) {
    if (!p.enabled) return true;
    if (!landlock_available()) return false;

    // Device nodes that session shells legitimately write to must stay
    // writable: /dev/null (every `2>/dev/null`), /dev/full, /dev/zero,
    // /dev/tty. These are opened directly (O_RDWR), NOT under an allowed
    // root, so without explicit rules the 26.09.06-r2/r3 jail broke
    // `curl -o … 2>/dev/null` and any other redirect inside a session.
    // Char devices carry no directory-beneath semantics — each rule
    // covers exactly that node.
    //
    // 1. Create a ruleset that denies file writes by default.
    unsigned long attr = detail::kLandlockAccessFileWrite;
    int ruleset_fd = static_cast<int>(
        ::syscall(444 /* __NR_landlock_create_ruleset */, &attr, sizeof(attr), 0));
    if (ruleset_fd < 0) return false;

    for (const char* dev : {"/dev/null", "/dev/full", "/dev/zero", "/dev/tty"}) {
        int fd = ::open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;  // node absent on this host
        // Char devices accept only WRITE_FILE in a PathBeneath rule — any
        // REMOVE_*/MAKE_* bit makes add_rule fail with EINVAL (2026-09-07
        // jail EINVAL hunt; bisected on ABI 9). WRITE_FILE is exactly what
        // `2>/dev/null` needs; removes/creates don't apply to device nodes.
        detail::LandlockPathBeneathAttr rule{0x0002 /* WRITE_FILE */, fd};
        long rc = ::syscall(445 /* __NR_landlock_add_rule */, ruleset_fd,
                            detail::kLandlockRulePathBeneath, &rule, 0);
        ::close(fd);
        if (rc != 0) { ::close(ruleset_fd); return false; }
    }

    // 2. Grant write-beneath for each allowed root.
    for (const auto& root : p.writable_roots) {
        int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;  // vanished between policy build and apply
        detail::LandlockPathBeneathAttr rule{detail::kLandlockAccessFileWrite, fd};
        long rc = ::syscall(445 /* __NR_landlock_add_rule */, ruleset_fd,
                            detail::kLandlockRulePathBeneath, &rule, 0);
        ::close(fd);
        if (rc != 0) { ::close(ruleset_fd); return false; }
    }

    // 3. Enforce (no_new_privs is required for unprivileged self-jailing).
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        ::close(ruleset_fd);
        return false;
    }
    long rc = ::syscall(446 /* __NR_landlock_restrict_self */, ruleset_fd, 0);
    ::close(ruleset_fd);
    return rc == 0;
}

} // namespace bs::mesh

#endif // __linux__
