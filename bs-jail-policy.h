// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-jail-policy.h — jail policy model, no system includes of its own.
//
// This header is included INSIDE namespace bs::mesh (parent provides the
// namespace, like bs-mesh-transfer.h) and must therefore not #include C
// system headers — everything it needs is already included at global scope
// by bs-protocol.h (<cstdlib>, <cstring>, <unistd.h>, <sys/stat.h>,
// <sys/syscall.h>). The Landlock enforcement half lives in bs-jail.h, which
// opens its own namespace block and is included at global scope.

#pragma once

// Peer-user home for jail purposes: $HOME as seen by the daemon.
inline std::string jail_home_dir() {
    const char* h = std::getenv("HOME");
    return (h && *h) ? h : "";
}

// The daemon's current working directory — a "user's working directory" by
// definition (operators start the daemon from their project root).
inline std::string jail_daemon_cwd() {
    std::string buf(4096, '\0');
    if (!::getcwd(buf.data(), buf.size())) return "";
    buf.resize(std::strlen(buf.data()));
    return buf;
}

namespace detail {

inline bool jail_path_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

inline std::string jail_expand_tilde(const std::string& p, const std::string& home) {
    if (p.rfind("~/", 0) == 0 && !home.empty()) return home + p.substr(1);
    if (p == "~" && !home.empty()) return home;
    return p;
}

} // namespace detail

// Policy: session shells keep READ access everywhere ("read to know where
// things are") but WRITE only inside allowed roots ($HOME, daemon cwd, /tmp,
// BS_JAIL_RW extras) unless explicitly extended. BS_JAIL=0 disables.
struct FilesystemJailPolicy {
    bool enabled = true;
    std::vector<std::string> writable_roots;

    // Join the roots back into BS_JAIL_RW form (absolute, ~ expanded).
    [[nodiscard]] std::string writable_roots_env() const {
        std::string out;
        for (size_t i = 0; i < writable_roots.size(); ++i) {
            if (i) out += ":";
            out += writable_roots[i];
        }
        return out;
    }
};

// Build the jail policy from the environment.
//   home_dir:  the peer user's home ($HOME on the peer).
//   daemon_cwd: the daemon's working directory (a "user working directory").
//
// OPT-IN since 26.09.06-r5: the jail previously defaulted ON and set
// PR_SET_NO_NEW_PRIVS in every session child, which silently broke
// `sudo` inside sessions ("The no new privileges flag is set") and was
// heavier-handed than intended (operator, 2026-09-07: "the jail wasn't
// supposed to be so massive"). Confinement now runs ONLY when the
// session opts in via BS_JAIL=1; set BS_JAIL_RW for extra writable roots.
inline FilesystemJailPolicy jail_policy_from_env(const std::string& home_dir,
                                                 const std::string& daemon_cwd) {
    FilesystemJailPolicy p;
    const char* dis = std::getenv("BS_JAIL");
    if (!dis || !*dis || std::string(dis) == "0") {
        p.enabled = false;
        return p;
    }
    // The policy is DECLARATIVE: roots are kept regardless of existence.
    // A root that cannot be opened O_DIRECTORY at enforcement time is simply
    // not granted (apply_filesystem_jail skips it), so a typoed BS_JAIL_RW
    // entry can never silently broaden the jail — it grants nothing.
    auto add = [&](const std::string& path) {
        if (path.empty()) return;
        for (auto& r : p.writable_roots)
            if (r == path) return;  // dedupe (cwd == home is the common case)
        p.writable_roots.push_back(path);
    };
    add(home_dir);
    add(daemon_cwd);
    add("/tmp");
    // Explicit extra grants: BS_JAIL_RW="path1:path2:~/notes" (write on explicit).
    if (const char* rw = std::getenv("BS_JAIL_RW"); rw && *rw) {
        std::string cur;
        const std::string rw_s(rw);
        for (size_t i = 0; i <= rw_s.size(); ++i) {
            if (i == rw_s.size() || rw_s[i] == ':') {
                add(detail::jail_expand_tilde(cur, home_dir));
                cur.clear();
            } else {
                cur += rw_s[i];
            }
        }
    }
    return p;
}

// Enforcement (defined in bs-jail.h, included at global scope after the
// facade). Returns false when the kernel cannot enforce — callers proceed
// WITHOUT the jail (best-effort availability degrade).
inline bool landlock_available();
inline bool apply_filesystem_jail(const FilesystemJailPolicy& p);
