// test_file_copy.cpp — bs cp feature tests (v26.09.15)
//
// Covers the pure-logic layers of the direct-copy feature:
//   - Codec round-trips for the +fcp trailing fields (mode, mtime, direct)
//     including the legacy-compat cases (fields absent → defaults)
//   - CopyOperand parsing (peer:path vs local, Windows drive letters)
//   - Glob matcher semantics (*, ?, **, no crossing / for single stars)
//   - Update decisions and local/remote tree expansion (no live sockets)

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include <set>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../bs-protocol.h"
#include "../bs-mesh-cli.h"

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

TEST_CASE("fcp: FileRequest mode round-trips", "[fcp][codec]") {
    FileRequestMsg req;
    req.path = "/Users/x/stmt.pdf";
    req.mode = 1;
    auto frame = encode(Message{req}, 0);
    Message decoded = decode(frame);
    REQUIRE(std::holds_alternative<FileRequestMsg>(decoded));
    auto& m = std::get<FileRequestMsg>(decoded);
    CHECK(m.path == "/Users/x/stmt.pdf");
    CHECK(m.mode == 1);
}

TEST_CASE("fcp: FileRequest mode 2 (list) round-trips", "[fcp][codec]") {
    FileRequestMsg req;
    req.path = "~/Heimdall";
    req.mode = 2;
    auto decoded = decode(encode(Message{req}, 0));
    auto& m = std::get<FileRequestMsg>(decoded);
    CHECK(m.path == "~/Heimdall");
    CHECK(m.mode == 2);
}

TEST_CASE("fcp: FileRequest legacy wire decodes with mode=0", "[fcp][codec]") {
    // Legacy-equivalent payload: mode left at default (field not serialized).
    FileRequestMsg req;
    req.path = "received/report.md";
    // mode == 0 → serializer omits the trailing byte → legacy byte stream.
    auto decoded = decode(encode(Message{req}, 0));
    auto& m = std::get<FileRequestMsg>(decoded);
    CHECK(m.path == "received/report.md");
    CHECK(m.mode == 0);
}

TEST_CASE("fcp: FileMeta mtime + direct round-trips", "[fcp][codec]") {
    FileMetaMsg meta;
    meta.filename = "video.mp4";
    meta.filesize = 24435715;
    meta.checksum = "934fc4b237d99cc40a44e103527c5316bd44bd53cb3bc7d92e1cda9c41a5578e";
    meta.total_chunks = 1909;
    meta.chunk_size = 131072;
    meta.dest_path = "~/videos/video.mp4";
    meta.src_mtime_unix = 1757500800;
    meta.direct = 1;
    auto decoded = decode(encode(Message{meta}, 0));
    REQUIRE(std::holds_alternative<FileMetaMsg>(decoded));
    auto& m = std::get<FileMetaMsg>(decoded);
    CHECK(m.dest_path == "~/videos/video.mp4");
    CHECK(m.src_mtime_unix == 1757500800);
    CHECK(m.direct == 1);
}

TEST_CASE("fcp: FileMeta direct=2 (overwrite) with empty dest_path chain",
          "[fcp][codec]") {
    // direct != 0 forces dest_path serialization even when empty — verify
    // the decoder handles the empty-string field without ambiguity.
    FileMetaMsg meta;
    meta.filename = "f.bin";
    meta.filesize = 10;
    meta.checksum = "abc";
    meta.total_chunks = 1;
    meta.chunk_size = 65536;
    meta.dest_path = "";
    meta.src_mtime_unix = 42;
    meta.direct = 2;
    auto decoded = decode(encode(Message{meta}, 0));
    auto& m = std::get<FileMetaMsg>(decoded);
    CHECK(m.dest_path.empty());
    CHECK(m.src_mtime_unix == 42);
    CHECK(m.direct == 2);
}

TEST_CASE("fcp: FileMeta legacy (chunk_size only) decodes with zero extensions",
          "[fcp][codec]") {
    FileMetaMsg meta;
    meta.filename = "legacy.bin";
    meta.filesize = 1000;
    meta.checksum = "deadbeef";
    meta.total_chunks = 1;
    meta.chunk_size = 65536;
    auto decoded = decode(encode(Message{meta}, 0));
    auto& m = std::get<FileMetaMsg>(decoded);
    CHECK(m.dest_path.empty());
    CHECK(m.src_mtime_unix == 0);
    CHECK(m.direct == 0);
}

TEST_CASE("fcp: FileMeta scp-style dest but no fcp extensions stays clean",
          "[fcp][codec]") {
    // Pre-fcp peer shape (26.08.12 dest_path, no mtime/direct).
    FileMetaMsg meta;
    meta.filename = "cfg.toml";
    meta.filesize = 512;
    meta.checksum = "feedface";
    meta.total_chunks = 1;
    meta.chunk_size = 65536;
    meta.dest_path = "configs/cfg.toml";
    auto decoded = decode(encode(Message{meta}, 0));
    auto& m = std::get<FileMetaMsg>(decoded);
    CHECK(m.dest_path == "configs/cfg.toml");
    CHECK(m.direct == 0);
    CHECK(m.src_mtime_unix == 0);
}

TEST_CASE("fcp: version caps advertise +fcp", "[fcp]") {
    const std::string v = version_string_with_local_caps();
    CHECK(version_has_cap(v, kCapFcp));
    CHECK(version_has_cap(v, kCapFrm2));
    CHECK(version_has_cap(v, kCapEnroll));
    // An r1 peer (no fcp) must not accidentally match.
    CHECK_FALSE(version_has_cap("26.09.14-r1+frm2+enroll", kCapFcp));
}

TEST_CASE("cp: operand parsing", "[cp]") {
    MeshController::CopyOperand op;
    std::string err;
    REQUIRE(MeshController::parse_copy_operand("macbook:~/Heimdall/a.pdf", op, err));
    CHECK(op.remote);
    CHECK(op.peer == "macbook");
    CHECK(op.path == "~/Heimdall/a.pdf");

    REQUIRE(MeshController::parse_copy_operand("./local/file.txt", op, err));
    CHECK_FALSE(op.remote);
    CHECK(op.path == "./local/file.txt");

    REQUIRE(MeshController::parse_copy_operand("fecv3:/srv/reports/r.md", op, err));
    CHECK(op.remote);
    CHECK(op.peer == "fecv3");
    CHECK(op.path == "/srv/reports/r.md");

    // Windows drive letter stays local.
    REQUIRE(MeshController::parse_copy_operand("C:\\Users\\jeff\\a.txt", op, err));
    CHECK_FALSE(op.remote);
    CHECK(op.path == "C:\\Users\\jeff\\a.txt");

    // ANY single-letter prefix is a drive, not a peer: "D:\x" used to parse
    // as peer "D" and got looked up as a mesh node.
    REQUIRE(MeshController::parse_copy_operand("D:\\data\\x.bin", op, err));
    CHECK_FALSE(op.remote);
    CHECK(op.path == "D:\\data\\x.bin");
    REQUIRE(MeshController::parse_copy_operand("z:/data/x.bin", op, err));
    CHECK_FALSE(op.remote);
    CHECK(op.path == "z:/data/x.bin");

    // Bare peer: is an error.
    CHECK_FALSE(MeshController::parse_copy_operand("macbook:", op, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("cp: glob matcher semantics", "[cp][glob]") {
    CHECK(MeshController::glob_match("*.pdf", "statement.pdf"));
    CHECK_FALSE(MeshController::glob_match("*.pdf", "statement.txt"));
    CHECK(MeshController::glob_match("stmt-??.pdf", "stmt-07.pdf"));
    CHECK_FALSE(MeshController::glob_match("*.pdf", "dir/statement.pdf"));  // * does not cross /
    CHECK(MeshController::glob_match("**/*.pdf", "a/b/statement.pdf"));
    CHECK(MeshController::glob_match("**/*.pdf", "statement.pdf"));
    CHECK(MeshController::glob_match("a**b", "a/x/b"));
    CHECK(MeshController::glob_match("*", "file"));
    CHECK(MeshController::glob_match("exact.bin", "exact.bin"));
    CHECK_FALSE(MeshController::glob_match("exact.bin", "exact.txt"));
    CHECK(MeshController::glob_match("202?-*", "2026-09"));
    CHECK_FALSE(MeshController::glob_match("202?-*", "2026/09"));
}

TEST_CASE("cp: update skips only known matching size and mtime", "[cp][update]") {
    using Metadata = MeshController::CopyMetadata;
    Metadata src{42, 100};
    for (int64_t delta : {-3, -2, -1, 0, 1, 2, 3}) {
        CAPTURE(delta);
        CHECK(MeshController::copy_update_skip(src, {42, 100 + delta}, true, false)
              == (delta >= -2 && delta <= 2));
    }
    CHECK_FALSE(MeshController::copy_update_skip(src, {43, 100}, true, false));
    CHECK_FALSE(MeshController::copy_update_skip(src, src, false, false));
    CHECK_FALSE(MeshController::copy_update_skip(src, src, true, true));
    for (const Metadata& missing : {Metadata{}, Metadata{42, {}}, Metadata{{}, 100}}) {
        CHECK_FALSE(MeshController::copy_update_skip(src, missing, true, false));
        CHECK_FALSE(MeshController::copy_update_skip(missing, src, true, false));
    }
    CHECK(MeshController::copy_update_skip({0, 0}, {0, 0}, true, false));
    CHECK(MeshController::copy_update_skip({42, -1}, {42, 1}, true, false));
    CHECK_FALSE(MeshController::copy_update_skip({42, INT64_MIN}, {42, INT64_MAX}, true, false));
}

TEST_CASE("cp: listing metadata is parsed conservatively", "[cp][update]") {
    std::vector<MeshController::CopyListingEntry> entries;
    REQUIRE(MeshController::parse_copy_listing(R"([
        {"name":"quote\"\u000a.txt","type":"file","size":42,"mtime":100},
        {"name":"nested","type":"dir","size":0,"mtime":100},
        {"name":"unknown","type":"file"},
        {"name":"bad-stat","type":"file","size":18446744073709551615,"mtime":0},
        {"name":"bad-type","type":"file","size":"42","mtime":100.5},
        {"name":"overflow","type":"file","size":-1,"mtime":18446744073709551615}
    ])", entries));
    REQUIRE(entries.size() == 6);
    CHECK(entries[0].name == "quote\"\n.txt");
    CHECK(entries[0].metadata.size == 42);
    CHECK(entries[0].metadata.mtime == 100);
    CHECK(entries[1].type == "dir");
    CHECK_FALSE(entries[1].metadata.size.has_value());
    for (size_t i = 2; i < entries.size(); ++i) {
        CHECK_FALSE(entries[i].metadata.size.has_value());
        CHECK_FALSE(entries[i].metadata.mtime.has_value());
    }
    CHECK_FALSE(MeshController::parse_copy_listing("ERROR unavailable", entries));
    CHECK_FALSE(MeshController::parse_copy_listing("[", entries));
    CHECK_FALSE(MeshController::parse_copy_listing(
        R"([{"name":"../escape","type":"file"}])", entries));
}

namespace {
struct CopyTreeFixture {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("bs-copy-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CopyTreeFixture() {
        std::filesystem::create_directories(dir / "src" / "nested" / "deep");
        std::ofstream(dir / "src" / "flat.txt") << "flat";
        std::ofstream(dir / "src" / "nested" / "child.txt") << "child";
        std::ofstream(dir / "src" / "nested" / "deep" / "leaf.txt") << "leaf";
    }
    ~CopyTreeFixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};
}

TEST_CASE("cp: recursive local paths preserve flat and nested mappings", "[cp][recursive]") {
    namespace fs = std::filesystem;
    CopyTreeFixture tree;
    std::string err;
    std::vector<MeshController::CopyFile> files;
    REQUIRE(MeshController::expand_copy_sources(
        {false, "", (tree.dir / "src").string() + "/"}, true, {}, files, err));
    std::map<std::string, fs::path> mapped;
    for (const auto& file : files) {
        CHECK_FALSE(file.source.remote);
        CHECK(file.metadata.size == fs::file_size(file.source.path));
        CHECK(file.metadata.mtime.has_value());
        mapped[file.relative_path] = tree.dir / "dest" / file.relative_path;
        CHECK(fs::path(file.source.path) == tree.dir / "src" / file.relative_path);
    }
    REQUIRE(mapped.size() == 3);
    CHECK(mapped["flat.txt"] == tree.dir / "dest" / "flat.txt");
    CHECK(mapped["nested/child.txt"] == tree.dir / "dest" / "nested" / "child.txt");
    CHECK(mapped["nested/deep/leaf.txt"] == tree.dir / "dest" / "nested" / "deep" / "leaf.txt");
    CHECK_FALSE(MeshController::copy_local_metadata((tree.dir / "missing").string()).size);
    CHECK_FALSE(MeshController::copy_local_metadata((tree.dir / "src").string()).size);
}

TEST_CASE("cp: local globs compose with recursion", "[cp][recursive][glob]") {
    CopyTreeFixture tree;
    std::string err;
    std::vector<MeshController::CopyFile> files;
    REQUIRE(MeshController::expand_copy_sources(
        {false, "", (tree.dir / "src" / "*").string()}, true, {}, files, err));
    std::set<std::string> relative;
    for (const auto& file : files) relative.insert(file.relative_path);
    CHECK(relative == std::set<std::string>{"flat.txt", "nested/child.txt", "nested/deep/leaf.txt"});
}

TEST_CASE("cp: directory sources without recursive flag still error", "[cp][recursive]") {
    CopyTreeFixture tree;
    for (const auto& path : {tree.dir / "src", tree.dir / "src" / "nest*"}) {
        std::string err;
        std::vector<MeshController::CopyFile> files;
        CHECK_FALSE(MeshController::expand_copy_sources({false, "", path.string()}, false, {}, files, err));
        CHECK(err.find(" is a directory (use -r)") != std::string::npos);
        CHECK(files.empty());
    }
}

TEST_CASE("cp: remote listings expand trees and retain update metadata", "[cp][recursive][update]") {
    std::map<std::string, std::string> listings{
        {"/", R"([{"name":"src","type":"dir","size":0,"mtime":100}])"},
        {"/src", R"([{"name":"flat.txt","type":"file","size":4,"mtime":100},
                      {"name":"nested","type":"dir","size":0,"mtime":100}])"},
        {"/src/nested", R"([{"name":"leaf.txt","type":"file","size":5,"mtime":200}])"}
    };
    auto list = [&](const std::string& path) {
        auto it = listings.find(path);
        return it == listings.end() ? "ERROR missing listing" : it->second;
    };
    std::vector<MeshController::CopyFile> files;
    std::string err;
    REQUIRE(MeshController::expand_copy_sources({true, "peer", "/src"}, true, list, files, err));
    REQUIRE(files.size() == 2);
    CHECK(files[0].relative_path == "flat.txt");
    CHECK(files[1].relative_path == "nested/leaf.txt");
    CHECK(files[1].source.path == "/src/nested/leaf.txt");
    CHECK(files[1].source.peer == "peer");
    CHECK(MeshController::copy_update_skip(files[0].metadata, {4, 102}, true, false));
    CHECK_FALSE(MeshController::copy_update_skip(files[1].metadata, {5, 190}, true, false));

    files.clear();
    REQUIRE(MeshController::expand_copy_sources({true, "peer", "/src/*"}, true, list, files, err));
    REQUIRE(files.size() == 2);
    CHECK(files[1].relative_path == "nested/leaf.txt");
    files.clear();
    CHECK_FALSE(MeshController::expand_copy_sources({true, "peer", "/src"}, false, list, files, err));
    CHECK(err == "ERROR /src is a directory (use -r)");

    // An inaccessible parent must not prevent listing a permitted tree root.
    listings["/"] = "ERROR refused path outside receive_dir (file.copy_scope)";
    REQUIRE(MeshController::expand_copy_sources({true, "peer", "/src"}, true, list, files, err));
    listings["/src/nested"] = "ERROR denied";
    files.clear();
    CHECK_FALSE(MeshController::expand_copy_sources({true, "peer", "/src"}, true, list, files, err));
    CHECK(err == "ERROR denied");
}

TEST_CASE("cp: remote recursive depth is bounded at 16", "[cp][recursive]") {
    for (int levels : {16, 17}) {
        std::string err;
        std::vector<MeshController::CopyFile> files;
        int deepest = -1;
        auto list = [&](const std::string& path) -> std::string {
            if (path == "/") return R"([{"name":"src","type":"dir"}])";
            int depth = int(std::count(path.begin(), path.end(), '/')) - 1;
            deepest = std::max(deepest, depth);
            if (depth < levels) return R"([{"name":"d","type":"dir"}])";
            return R"([{"name":"leaf","type":"file","size":1,"mtime":100}])";
        };
        CHECK(MeshController::expand_copy_sources({true, "peer", "/src"}, true, list, files, err)
              == (levels == 16));
        CHECK(deepest == 16);
        if (levels == 17) CHECK(err.find("recursion depth exceeds 16") != std::string::npos);
    }
}

TEST_CASE("cp: remote Windows trees use the remote path syntax", "[cp][recursive]") {
    std::vector<MeshController::CopyFile> files;
    std::string err;
    auto list = [](const std::string& path) -> std::string {
        if (path == "C:/") return R"([{"name":"src","type":"dir"}])";
        if (path == "C:/src") return R"([{"name":"file.txt","type":"file","size":4,"mtime":100}])";
        return "ERROR unexpected path: " + path;
    };
    REQUIRE(MeshController::expand_copy_sources({true, "peer", "C:\\src\\"}, true, list, files, err));
    REQUIRE(files.size() == 1);
    CHECK(files[0].source.path == "C:/src/file.txt");
    CHECK(files[0].relative_path == "file.txt");
}

// ── v26.09.16: chunked listings (#34) + push summary bytes (#35) ──

TEST_CASE("fcp: chunked listing frames round-trip with seq + more flags", "[fcp][codec][chunk]") {
    FileAckMsg chunk;
    chunk.chunk_index = 1;
    chunk.next_requested = 1;
    chunk.error = false;
    chunk.error_msg = R"({"name":"a.pdf","size":1,"mtime":2,"type":"file"})";
    auto decoded = decode(encode(Message{chunk}, 0));
    REQUIRE(std::holds_alternative<FileAckMsg>(decoded));
    auto& m = std::get<FileAckMsg>(decoded);
    CHECK(m.chunk_index == 1);
    CHECK(m.next_requested == 1);
    CHECK_FALSE(m.error);
    CHECK(m.error_msg == chunk.error_msg);
}

TEST_CASE("fcp: legacy single-frame listing decodes as chunk 0 no-more", "[fcp][codec][chunk]") {
    FileAckMsg legacy;
    legacy.error = false;
    legacy.error_msg = "[{\"name\":\"x\",\"type\":\"dir\"}]";
    auto decoded = decode(encode(Message{legacy}, 0));
    auto& m = std::get<FileAckMsg>(decoded);
    CHECK(m.chunk_index == 0);
    CHECK(m.next_requested == 0);
    CHECK(m.error_msg.front() == '[');
}

TEST_CASE("fcp: chunk frame near the u16 cap still encodes", "[fcp][codec][chunk]") {
    // 60000-byte payload must encode (the old code threw "prefixed string
    // exceeds 65535 bytes" for listings over the cap). The frame codec may
    // zstd-compress the payload, so do NOT assert on the wire size — only
    // that the round-trip survives and preserves the full payload.
    std::string big(60000, 'x');
    FileAckMsg chunk;
    chunk.error = false;
    chunk.error_msg = big;
    auto bytes = encode(Message{chunk}, 0);
    CHECK_FALSE(bytes.empty());
    auto decoded = decode(bytes);
    auto& m = std::get<FileAckMsg>(decoded);
    CHECK(m.error_msg.size() == 60000);
}

TEST_CASE("cp: concatenated chunk payloads rebuild the listing JSON", "[cp][chunk]") {
    // The server frames the array as: first frame "[...entries", middle
    // frames ",...entries", last frame "...,entries]". Concatenating the raw
    // error_msg payloads in order rebuilds the JSON array.
    std::string c0 = R"([{"name":"a","type":"file"})";
    std::string c1 = R"(,{"name":"b","type":"file"}])";
    std::vector<MeshController::CopyListingEntry> entries;
    REQUIRE(MeshController::parse_copy_listing(c0 + c1, entries));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "a");
    CHECK(entries[1].name == "b");
}

TEST_CASE("cp: push summary parses bytes from OK-sent line", "[cp][summary]") {
    // Regression for #35: "OK sent f.txt 11 bytes ..." must count 11 bytes,
    // not 0. The old rfind(' ', bp) matched the space of " bytes" itself.
    const std::string line = "OK sent f.txt 11 bytes sha256:deadbeef dest_abs=/tmp/x";
    auto bp = line.find(" bytes");
    REQUIRE(bp != std::string::npos);
    size_t num_start = line.rfind(' ', bp - 1);
    REQUIRE(num_start != std::string::npos);
    uint64_t parsed = std::strtoull(
        line.substr(num_start + 1, bp - num_start - 1).c_str(), nullptr, 10);
    CHECK(parsed == 11);
}
