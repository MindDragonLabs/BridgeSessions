#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Test: codec roundtrip for all file message types ────────────

TEST_CASE("FileMetaMsg: encode / decode roundtrip", "[file][codec]") {
    FileMetaMsg original;
    original.filename = "data.bin";
    original.filesize = 1048576;
    original.checksum = "abc123def456";
    original.total_chunks = 20;

    auto frame = encode(Message(original), 0);
    auto decoded = decode(frame);
    REQUIRE(std::holds_alternative<FileMetaMsg>(decoded));
    auto& m = std::get<FileMetaMsg>(decoded);
    REQUIRE(m.filename == original.filename);
    REQUIRE(m.filesize == original.filesize);
    REQUIRE(m.checksum == original.checksum);
    REQUIRE(m.total_chunks == original.total_chunks);
}

TEST_CASE("FileChunkMsg: encode / decode roundtrip", "[file][codec]") {
    FileChunkMsg original;
    original.chunk_index = 5;
    original.total_chunks = 10;
    original.data = {0x01, 0x02, 0x03, 0x04};

    auto frame = encode(Message(original), 0);
    auto decoded = decode(frame);
    REQUIRE(std::holds_alternative<FileChunkMsg>(decoded));
    auto& m = std::get<FileChunkMsg>(decoded);
    REQUIRE(m.chunk_index == 5);
    REQUIRE(m.total_chunks == 10);
    REQUIRE(m.data == original.data);
}

TEST_CASE("FileAckMsg: encode / decode roundtrip", "[file][codec]") {
    FileAckMsg original;
    original.chunk_index = 3;
    original.next_requested = 4;
    original.error = true;
    original.error_msg = "disk full";

    auto frame = encode(Message(original), 0);
    auto decoded = decode(frame);
    REQUIRE(std::holds_alternative<FileAckMsg>(decoded));
    auto& m = std::get<FileAckMsg>(decoded);
    REQUIRE(m.chunk_index == 3);
    REQUIRE(m.next_requested == 4);
    REQUIRE(m.error == true);
    REQUIRE(m.error_msg == "disk full");
}

TEST_CASE("FileRequestMsg: encode / decode roundtrip", "[file][codec]") {
    FileRequestMsg original;
    original.path = "/home/agent/file.txt";

    auto frame = encode(Message(original), 0);
    auto decoded = decode(frame);
    REQUIRE(std::holds_alternative<FileRequestMsg>(decoded));
    auto& m = std::get<FileRequestMsg>(decoded);
    REQUIRE(m.path == "/home/agent/file.txt");
}

TEST_CASE("sha256_hex works for file content", "[file]") {
    std::string empty = "";
    REQUIRE(sha256_hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    std::string hello = "hello";
    REQUIRE(sha256_hex(hello) == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}
