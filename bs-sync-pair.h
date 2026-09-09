// bs-sync-pair.h — `bs sync pair` cross-machine folder mirroring (lane 2)
//
// Implements roadmap phase 2 per docs/bs-sync-design.md:
//   * pair specs persisted in <BS home>/state/sync-pairs.json
//   * dry-run manifest scan (create / modify / delete classification) that
//     must be approved (--approve or `bs sync pair approve <id>`) before any
//     byte moves — the 10-GB sandbox is a projection cache, never a source
//     of truth, and default is "do nothing".
//   * POSIX push for Unix peers; Windows peers only via --via-run-script
//     (no PowerShell bodies are ever pushed).
//   * Lamport-style logical clock persisted per pair. Every manifest gets a
//     clock tick. mtime is NEVER used for ordering: modify classification is
//     purely content-hash based, so cross-host clock skew cannot regress data
//     (design lesson #2 from the spike).
//   * default exclusions: .git, secrets basenames, the BridgeSessions config
//     dir (.bridgesessions), and bs-sync working files.
//
// This header is intentionally daemon-free (no MeshController dependency) so
// tests/test_sync_pair.cpp can exercise the pure logic directly.

#pragma once

#include <openssl/evp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace bs::sync {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────── hashing ───────────────────────────

inline std::string sync_sha256_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[1 << 16];
    while (f) {
        f.read(buf, sizeof buf);
        const size_t n = static_cast<size_t>(f.gcount());
        if (n) EVP_DigestUpdate(ctx, buf, n);
    }
    unsigned char out[32];
    unsigned int olen = 0;
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (unsigned i = 0; i < olen; i++)
        s.width(2), s << static_cast<unsigned>(out[i]);
    return s.str();
}

// ─────────────────────────── exclusions ───────────────────────────

// Directory names never descended into (any depth).
inline bool sync_excluded_dir(const std::string& name) {
    return name == ".git" ||            // version control internals
           name == ".bridgesessions" || // the BridgeSessions config dir
           name == ".bs-sync" ||        // bs-sync working files
           name == "node_modules";      // regenerated build artifacts
}

// File basenames never transferred (secrets hygiene per design §6).
inline bool sync_excluded_file(const std::string& name) {
    if (name.rfind(".env", 0) == 0) return true;            // .env, .env.local
    if (name == "id_rsa" || name == "id_ed25519") return true;
    if (name == "secrets" || name == "secrets.json" ||
        name == "credentials.json" || name == ".netrc")
        return true;
    if (name.size() > 4 && name.rfind(".pem") == name.size() - 4) return true;
    if (name.size() > 4 && name.rfind(".key") == name.size() - 4) return true;
    if (name.find(".secret") != std::string::npos) return true;
    if (name.find(".conflict-") != std::string::npos) return true; // spike conflict copies
    if (name.rfind(".tmp-bs", 0) == 0) return true;         // atomic-write temp files
    return false;
}

// ─────────────────────────── index / manifest ───────────────────────────

struct SyncEntry {
    std::string relpath;
    unsigned long long size = 0;
    std::string sha256;
};

using SyncIndex = std::map<std::string, SyncEntry>; // relpath → entry

// Walk root (non-recursive into excluded dirs), skip symlinks (never follow
// outside the root — design §6) and non-regular files.
inline SyncIndex sync_scan_dir(const std::string& root, std::string* err = nullptr) {
    SyncIndex idx;
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        if (err) *err = "not a directory: " + root;
        return idx;
    }
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::directory_entry& de = *it;
        const std::string name = de.path().filename().string();
        if (de.is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (ec) { ec.clear(); continue; }
        if (de.is_directory(ec)) {
            if (sync_excluded_dir(name)) it.disable_recursion_pending();
            continue;
        }
        if (ec) { ec.clear(); continue; }
        if (!de.is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (sync_excluded_file(name)) continue;
        std::error_code sec;
        const auto sz = de.file_size(sec);
        if (sec) continue;
        std::string rel = fs::relative(de.path(), root, sec).generic_string();
        if (sec) continue;
        idx[rel] = SyncEntry{rel, static_cast<unsigned long long>(sz),
                             sync_sha256_file(de.path())};
    }
    return idx;
}

// One op to apply on the peer. mtime is deliberately absent everywhere:
// ordering is by logical clock + content hash, never wall-clock.
struct SyncOp {
    std::string kind; // "create" | "modify" | "delete"
    std::string relpath;
    unsigned long long size = 0;
    std::string sha256;
};

struct SyncManifest {
    std::string pair_id;
    unsigned long long clock = 0; // Lamport tick taken at scan time
    std::vector<SyncOp> ops;
};

// Diff two indexes. Classification is hash-based:
//   missing locally but in previous manifest → delete (explicit tombstone;
//   first run has no previous manifest, so nothing is ever inferred-deleted —
//   spike lesson #1)
//   present locally with new/unknown hash      → create or modify
inline SyncManifest sync_diff(const std::string& pair_id,
                              const SyncIndex& prev,
                              const SyncIndex& current,
                              unsigned long long clock_after_tick) {
    SyncManifest m;
    m.pair_id = pair_id;
    m.clock = clock_after_tick;
    for (const auto& [rel, e] : current) {
        auto it = prev.find(rel);
        if (it == prev.end()) {
            m.ops.push_back({"create", rel, e.size, e.sha256});
        } else if (it->second.sha256 != e.sha256) {
            m.ops.push_back({"modify", rel, e.size, e.sha256});
        }
    }
    for (const auto& [rel, e] : prev) {
        (void)e;
        if (current.find(rel) == current.end())
            m.ops.push_back({"delete", rel, 0, {}});
    }
    return m;
}

inline size_t sync_pending_count(const SyncManifest& m) { return m.ops.size(); }

// ─────────────────────────── pair specs (state/sync-pairs.json) ───────

struct SyncPairSpec {
    std::string id;
    std::string local_dir;
    std::string peer;       // peer name from config
    std::string remote_dir; // path on the peer
    unsigned long long clock = 0;    // Lamport counter, persisted per pair
    bool via_run_script = false;     // Windows peers: run-script path only
    bool approved = false;           // first-transfer gate
    std::string created_at;          // informational only, never ordering
};

inline std::string sync_pairs_path(const std::string& home_dir) {
    return (fs::path(home_dir) / "state" / "sync-pairs.json").string();
}

inline std::vector<SyncPairSpec> sync_pairs_load(const std::string& home_dir) {
    std::vector<SyncPairSpec> out;
    std::ifstream f(sync_pairs_path(home_dir));
    if (!f) return out;
    json j;
    try { f >> j; } catch (...) { return out; }
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        SyncPairSpec p;
        p.id = e.value("id", "");
        p.local_dir = e.value("local_dir", "");
        p.peer = e.value("peer", "");
        p.remote_dir = e.value("remote_dir", "");
        p.clock = e.value("clock", 0ULL);
        p.via_run_script = e.value("via_run_script", false);
        p.approved = e.value("approved", false);
        p.created_at = e.value("created_at", "");
        if (!p.id.empty()) out.push_back(p);
    }
    return out;
}

inline bool sync_pairs_save(const std::string& home_dir,
                            const std::vector<SyncPairSpec>& pairs) {
    std::error_code ec;
    fs::create_directories(fs::path(home_dir) / "state", ec);
    json j = json::array();
    for (const auto& p : pairs) {
        j.push_back({{"id", p.id},
                     {"local_dir", p.local_dir},
                     {"peer", p.peer},
                     {"remote_dir", p.remote_dir},
                     {"clock", p.clock},
                     {"via_run_script", p.via_run_script},
                     {"approved", p.approved},
                     {"created_at", p.created_at}});
    }
    const std::string path = sync_pairs_path(home_dir);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp);
        if (!o) return false;
        o << j.dump(2) << "\n";
        if (!o.good()) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

// Deterministic pair id: <local-dir-basename>-<peer>, deduped with -2, -3…
inline std::string sync_make_pair_id(const std::vector<SyncPairSpec>& existing,
                                     const std::string& local_dir,
                                     const std::string& peer) {
    std::string base = fs::path(local_dir).filename().string();
    if (base.empty() || base == "/" || base == ".") base = "dir";
    std::string safe;
    for (char c : base)
        safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                 c == '_' || c == '-')
                    ? c
                    : '-';
    std::string id = safe + "-" + peer;
    int n = 2;
    auto taken = [&](const std::string& s) {
        for (const auto& p : existing)
            if (p.id == s) return true;
        return false;
    };
    while (taken(id)) id = safe + "-" + peer + "-" + std::to_string(n++);
    return id;
}

// ─────────────────────────── manifest persistence ─────────────────────

inline fs::path sync_manifest_dir(const std::string& home_dir) {
    return fs::path(home_dir) / "state" / "sync-manifests";
}

// Persist the last approved+applied (or pending) manifest for a pair.
inline bool sync_manifest_save(const std::string& home_dir,
                               const SyncManifest& m) {
    std::error_code ec;
    fs::create_directories(sync_manifest_dir(home_dir), ec);
    json ops = json::array();
    for (const auto& op : m.ops)
        ops.push_back({{"kind", op.kind},
                       {"relpath", op.relpath},
                       {"size", op.size},
                       {"sha256", op.sha256}});
    json j = {{"pair_id", m.pair_id}, {"clock", m.clock}, {"ops", ops}};
    std::ofstream o(sync_manifest_dir(home_dir) / (m.pair_id + ".json"),
                    std::ios::trunc);
    if (!o) return false;
    o << j.dump(2) << "\n";
    return o.good();
}

inline bool sync_manifest_load(const std::string& home_dir,
                               const std::string& pair_id,
                               SyncManifest& m) {
    std::ifstream f(sync_manifest_dir(home_dir) / (pair_id + ".json"));
    if (!f) return false;
    json j;
    try { f >> j; } catch (...) { return false; }
    m.pair_id = j.value("pair_id", pair_id);
    m.clock = j.value("clock", 0ULL);
    m.ops.clear();
    if (j.contains("ops"))
        for (const auto& e : j["ops"])
            m.ops.push_back({e.value("kind", ""),
                             e.value("relpath", ""),
                             e.value("size", 0ULL),
                             e.value("sha256", "")});
    return true;
}

inline SyncIndex sync_index_from_manifest(const SyncManifest& m) {
    // Reconstruct the "previous" index view: creates/modifies were pushed,
    // deletes removed the path. Hash+size is all we need for the next diff.
    SyncIndex idx;
    for (const auto& op : m.ops) {
        if (op.kind == "delete") idx.erase(op.relpath);
        else idx[op.relpath] = SyncEntry{op.relpath, op.size, op.sha256};
    }
    return idx;
}

// ─────────────────────────── helpers for the CLI ──────────────────────

// Parse `peer:/remote/dir` (or `peer:C:\dir` for run-script peers).
inline bool sync_parse_peer_dir(const std::string& spec,
                                std::string& peer,
                                std::string& remote_dir) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size())
        return false;
    peer = spec.substr(0, colon);
    remote_dir = spec.substr(colon + 1);
    // Reject a Windows drive-letter style remote for the default POSIX path;
    // Windows peers must go through --via-run-script explicitly.
    if (remote_dir.size() >= 2 && remote_dir[1] == ':') return false;
    return peer.find('/') == std::string::npos;
}

// Lamport tick: strictly monotonic per pair. Called once per manifest scan.
inline unsigned long long sync_clock_tick(SyncPairSpec& p) { return ++p.clock; }

inline std::string sync_manifest_text(const SyncManifest& m) {
    std::ostringstream o;
    o << "manifest " << m.pair_id << " (logical clock " << m.clock << ")\n";
    for (const auto& op : m.ops) {
        o << "  " << op.kind << "  " << op.relpath;
        if (op.kind != "delete") o << "  (" << op.size << " bytes)";
        o << "\n";
    }
    if (m.ops.empty()) o << "  (no changes — trees already converge)\n";
    return o.str();
}

} // namespace bs::sync
