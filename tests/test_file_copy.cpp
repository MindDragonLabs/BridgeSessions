// test_file_copy.cpp — bs cp feature tests (v26.09.15)
//
// Covers the pure-logic layers of the direct-copy feature:
//   - Codec round-trips for the +fcp trailing fields (mode, mtime, direct)
//     including the legacy-compat cases (fields absent → defaults)
//   - CopyOperand parsing (peer:path vs local, Windows drive letters)
//   - Glob matcher semantics (*, ?, **, no crossing / for single stars)

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

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
