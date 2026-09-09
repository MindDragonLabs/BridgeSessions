// tests/test_sync_pair.cpp — `bs sync pair` lane 2 coverage
//
// Covers: pair spec persistence, dry-run manifest classification
// (create/modify/delete), default exclusion rules, logical clock
// monotonicity. Pure bs::sync logic — no daemon, no network.
//
// Design contracts under test (docs/bs-sync-design.md + PLANS.md phase 2):
//   * mtime is NEVER used for ordering — classification is sha256-based, so
//     forged/backdated mtimes cannot change the manifest.
//   * deletion is never inferred on first run (spike lesson #1).
//   * every manifest scan takes exactly one Lamport tick, strictly monotonic.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-sync-pair.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace bs::sync;
namespace fs = std::filesystem;

// Local test build links plain Catch2 (no Catch2::Main), so provide main()
// following the tests/ convention (see test_codec.cpp).
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

namespace {

struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path() /
               ("bs_sync_pair_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()) +
                "_" + std::to_string(rand()))) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

} // namespace

// ── pair spec persistence ─────────────────────────────────────────────

TEST_CASE("pair specs round-trip through state/sync-pairs.json", "[sync-pair]") {
    TempDir home;
    REQUIRE(sync_pairs_load(home.str()).empty());

    SyncPairSpec p;
    p.id = "proj-hub";
    p.local_dir = "/home/me/proj";
    p.peer = "hub";
    p.remote_dir = "/home/work/proj";
    p.clock = 7;
    p.via_run_script = true;
    p.approved = true;

    REQUIRE(sync_pairs_save(home.str(), {p}));
    // state/ subdir created, file readable, fields preserved
    auto loaded = sync_pairs_load(home.str());
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].id == "proj-hub");
    REQUIRE(loaded[0].local_dir == "/home/me/proj");
    REQUIRE(loaded[0].peer == "hub");
    REQUIRE(loaded[0].remote_dir == "/home/work/proj");
    REQUIRE(loaded[0].clock == 7);
    REQUIRE(loaded[0].via_run_script);
    REQUIRE(loaded[0].approved);
}

TEST_CASE("corrupt or missing sync-pairs.json loads as empty", "[sync-pair]") {
    TempDir home;
    fs::create_directories(fs::path(home.str()) / "state");
    {
        std::ofstream f(sync_pairs_path(home.str()));
        f << "{ not valid json";
    }
    REQUIRE(sync_pairs_load(home.str()).empty());
    REQUIRE(sync_pairs_load(home.str()).empty());
}

TEST_CASE("pair ids are deterministic and deduped", "[sync-pair]") {
    std::vector<SyncPairSpec> existing;
    REQUIRE(sync_make_pair_id(existing, "/home/me/proj", "hub") == "proj-hub");
    SyncPairSpec taken;
    taken.id = "proj-hub";
    existing.push_back(taken);
    REQUIRE(sync_make_pair_id(existing, "/home/me/proj", "hub") == "proj-hub-2");
    // unsafe chars sanitized
    // only the final path component is used
    REQUIRE(sync_make_pair_id(existing, "/tmp/weird name/x", "hub") == "x-hub");
}

// ── dry-run manifest classification ──────────────────────────────────

TEST_CASE("first scan classifies everything as create (nothing deleted)",
          "[sync-manifest]") {
    TempDir dir;
    write_file(dir.path / "a.txt", "alpha");
    write_file(dir.path / "sub" / "b.txt", "beta");

    SyncIndex idx = sync_scan_dir(dir.str());
    REQUIRE(idx.size() == 2);
    REQUIRE(idx.count("a.txt") == 1);
    REQUIRE(idx.count("sub/b.txt") == 1);

    // No previous manifest → all creates, zero deletes. A fresh pair must
    // never infer deletion (spike lesson #1: empty peer index regression).
    SyncManifest m = sync_diff("t-hub", {}, idx, 1);
    REQUIRE(m.ops.size() == 2);
    for (const auto& op : m.ops) REQUIRE(op.kind == "create");
}

TEST_CASE("modify / delete / create classification across runs", "[sync-manifest]") {
    TempDir dir;
    write_file(dir.path / "keep.txt", "same");
    write_file(dir.path / "edit.txt", "v1");
    write_file(dir.path / "gone.txt", "temp");

    SyncIndex first = sync_scan_dir(dir.str());
    SyncManifest m1 = sync_diff("t-hub", {}, first, 1);
    REQUIRE(m1.ops.size() == 3);

    // Baseline = what the previous manifest said was applied.
    SyncIndex baseline = sync_index_from_manifest(m1);
    REQUIRE(baseline.size() == 3);

    // Mutate: modify edit.txt, delete gone.txt, create new.txt.
    write_file(dir.path / "edit.txt", "v2-changed");
    std::error_code ec;
    fs::remove(dir.path / "gone.txt", ec);
    write_file(dir.path / "new.txt", "fresh");

    SyncIndex second = sync_scan_dir(dir.str());
    SyncManifest m2 = sync_diff("t-hub", baseline, second, 2);

    int mods = 0, dels = 0, creates = 0;
    for (const auto& op : m2.ops) {
        if (op.kind == "modify") {
            mods++;
            REQUIRE(op.relpath == "edit.txt");
        } else if (op.kind == "delete") {
            dels++;
            REQUIRE(op.relpath == "gone.txt");
        } else {
            creates++;
            REQUIRE(op.relpath == "new.txt");
        }
    }
    REQUIRE(mods == 1);
    REQUIRE(dels == 1);
    REQUIRE(creates == 1);
    REQUIRE(m2.ops.size() == 3);
}

TEST_CASE("unchanged trees produce empty manifests", "[sync-manifest]") {
    TempDir dir;
    write_file(dir.path / "a.txt", "alpha");
    SyncIndex idx = sync_scan_dir(dir.str());
    SyncManifest m1 = sync_diff("t-hub", {}, idx, 1);
    SyncIndex baseline = sync_index_from_manifest(m1);
    SyncManifest m2 = sync_diff("t-hub", baseline, sync_scan_dir(dir.str()), 2);
    REQUIRE(m2.ops.empty());
    REQUIRE(sync_pending_count(m2) == 0);
}

TEST_CASE("mtime is never used: forged mtimes do not change classification",
          "[sync-manifest]") {
    TempDir dir;
    write_file(dir.path / "f.txt", "v1");
    SyncIndex first = sync_scan_dir(dir.str());
    SyncManifest m1 = sync_diff("t-hub", {}, first, 1);
    SyncIndex baseline = sync_index_from_manifest(m1);

    // Same content, wildly different mtime → no modify op.
    auto old = fs::last_write_time(dir.path / "f.txt");
    fs::last_write_time(dir.path / "f.txt", old - std::chrono::hours(24 * 365));
    SyncManifest m2 = sync_diff("t-hub", baseline, sync_scan_dir(dir.str()), 2);
    REQUIRE(m2.ops.empty());
}

// ── exclusion rules ───────────────────────────────────────────────────

TEST_CASE("default exclusions: .git, secrets basenames, BS config dir",
          "[sync-exclude]") {
    TempDir dir;
    write_file(dir.path / "a.txt", "keep");
    write_file(dir.path / ".git" / "HEAD", "ref: refs/heads/main");
    write_file(dir.path / ".bridgesessions" / "config", "node.name x");
    write_file(dir.path / "sub" / ".git" / "config", "[core]");
    write_file(dir.path / ".env", "SECRET=1");
    write_file(dir.path / "id_rsa", "private");
    write_file(dir.path / "server.pem", "cert");
    write_file(dir.path / "creds.key", "key");
    write_file(dir.path / "secrets.json", "{}");
    write_file(dir.path / "thing.secret.txt", "shh");

    SyncIndex idx = sync_scan_dir(dir.str());
    REQUIRE(idx.size() == 1);
    REQUIRE(idx.count("a.txt") == 1);
}

TEST_CASE("bs-sync working files and temp files are excluded", "[sync-exclude]") {
    TempDir dir;
    write_file(dir.path / ".bs-sync" / "state", "x");
    write_file(dir.path / ".tmp-bs123", "half-written");
    write_file(dir.path / "f.conflict-hub", "conflict copy");
    write_file(dir.path / "real.txt", "keep");
    SyncIndex idx = sync_scan_dir(dir.str());
    REQUIRE(idx.size() == 1);
    REQUIRE(idx.count("real.txt") == 1);
}

TEST_CASE("symlinks are never followed", "[sync-exclude]") {
    TempDir dir;
    write_file(dir.path / "a.txt", "keep");
    TempDir outside;
    write_file(outside.path / "b.txt", "outside");
    std::error_code ec;
    fs::create_directory_symlink(outside.path, dir.path / "link", ec);
    if (!ec) {
        SyncIndex idx = sync_scan_dir(dir.str());
        REQUIRE(idx.size() == 1);
        REQUIRE(idx.count("a.txt") == 1);
    }
}

// ── logical clock monotonicity ────────────────────────────────────────

TEST_CASE("Lamport clock ticks are strictly monotonic and persisted per pair",
          "[sync-clock]") {
    TempDir home;
    std::vector<SyncPairSpec> pairs;
    SyncPairSpec p;
    p.id = "c-hub";
    p.local_dir = "/tmp/x";
    p.peer = "hub";
    p.remote_dir = "/tmp/y";
    pairs.push_back(p);

    unsigned long long c0 = sync_clock_tick(pairs[0]);
    unsigned long long c1 = sync_clock_tick(pairs[0]);
    unsigned long long c2 = sync_clock_tick(pairs[0]);
    REQUIRE(c0 == 1);
    REQUIRE(c1 == c0 + 1);
    REQUIRE(c2 == c1 + 1);

    REQUIRE(sync_pairs_save(home.str(), pairs));
    auto reloaded = sync_pairs_load(home.str());
    REQUIRE(reloaded[0].clock == 3);
    REQUIRE(sync_clock_tick(reloaded[0]) == 4); // resumes, never rewinds
}

TEST_CASE("manifests carry the tick taken at scan time and round-trip",
          "[sync-clock]") {
    TempDir home;
    TempDir dir;
    write_file(dir.path / "a.txt", "alpha");
    SyncIndex idx = sync_scan_dir(dir.str());
    SyncManifest m = sync_diff("m-hub", {}, idx, 42);
    REQUIRE(m.clock == 42);
    m.pair_id = "m-hub";
    REQUIRE(sync_manifest_save(home.str(), m));

    SyncManifest back;
    REQUIRE(sync_manifest_load(home.str(), "m-hub", back));
    REQUIRE(back.clock == 42);
    REQUIRE(back.ops.size() == m.ops.size());
    REQUIRE(back.ops[0].relpath == m.ops[0].relpath);
    REQUIRE(back.ops[0].sha256 == m.ops[0].sha256);

    // Baseline reconstruction: delete ops erase, create/modify restore.
    SyncManifest del;
    del.ops.push_back({"delete", "a.txt", 0, {}});
    SyncIndex after = sync_index_from_manifest(del);
    REQUIRE(after.empty());
}

// ── peer:dir parsing (Windows-peer gate) ──────────────────────────────

TEST_CASE("peer:dir parsing accepts POSIX remotes, rejects Windows drive paths",
          "[sync-pair]") {
    std::string peer, dir;
    REQUIRE(sync_parse_peer_dir("hub:/home/work/x", peer, dir));
    REQUIRE(peer == "hub");
    REQUIRE(dir == "/home/work/x");
    REQUIRE(sync_parse_peer_dir("hub:~/x", peer, dir));
    REQUIRE(dir == "~/x");

    REQUIRE_FALSE(sync_parse_peer_dir("no-colon", peer, dir));
    REQUIRE_FALSE(sync_parse_peer_dir(":nodir", peer, dir));
    REQUIRE_FALSE(sync_parse_peer_dir("peer:", peer, dir));
    // Windows drive-letter remote is only valid via --via-run-script, never
    // through the default POSIX path:
    REQUIRE_FALSE(sync_parse_peer_dir("winpeer:C:\\Users\\x", peer, dir));
}
