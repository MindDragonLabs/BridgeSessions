// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon
//
// tests/test_upgrade_hashparse.cpp — contract tests for the in-C++ SHA256SUMS
// parsing introduced in the 2026-09-08 Windows-upgrade RCA (the old
// grep|awk|sha256sum shell pipeline could never run on cmd.exe).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "../bs-protocol.h"

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

static std::string write_sums(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    return p.string();
}

// Mirror of the parsing logic in main.cpp upgrade path. Kept in lockstep —
// if main.cpp changes, change this and the tests.
static std::string parse_expected_hash(const std::string& sums_path,
                                       const std::string& binary_name) {
    std::ifstream sums_in(sums_path);
    std::string line;
    while (std::getline(sums_in, line)) {
        const auto sp = line.find("  ");
        if (sp == std::string::npos) continue;
        std::string file = line.substr(sp + 2);
        while (!file.empty() && (file.back() == '\r' || file.back() == '\n'))
            file.pop_back();
        const auto base = file.find_last_of('/');
        if (base != std::string::npos) file = file.substr(base + 1);
        if (file == binary_name) return line.substr(0, sp);
    }
    return {};
}

TEST_CASE("SHA256SUMS parsing finds the windows asset hash", "[upgrade][hash]") {
    fs::path base = fs::temp_directory_path() / "bs-sums-test";
    fs::create_directories(base);
    const std::string sums = write_sums(base / "SHA256SUMS",
        "aaaa  bridgesessions-26.09.08-r2-source.tar.gz\n"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  bridgesessions-windows-x86_64.exe\n"
        "bbbb  bridgesessions-linux-x86_64\n");

    const std::string h = parse_expected_hash(sums, "bridgesessions-windows-x86_64.exe");
    REQUIRE(h == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    REQUIRE(parse_expected_hash(sums, "bridgesessions-missing") .empty());
    std::error_code ec; fs::remove_all(base, ec);
}

TEST_CASE("SHA256SUMS parsing tolerates CRLF line endings", "[upgrade][hash]") {
    // Windows: the sums file may land with CRLF; the filename must not keep \r.
    fs::path base = fs::temp_directory_path() / "bs-sums-test-crlf";
    fs::create_directories(base);
    const std::string sums = write_sums(base / "SHA256SUMS",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  bridgesessions-windows-x86_64.exe\r\n");

    const std::string h = parse_expected_hash(sums, "bridgesessions-windows-x86_64.exe");
    REQUIRE(h.size() == 64);
    REQUIRE(h == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    std::error_code ec; fs::remove_all(base, ec);
}

TEST_CASE("sha256_file_stream hashes a known vector", "[upgrade][hash]") {
    fs::path base = fs::temp_directory_path() / "bs-sums-test-vec";
    fs::create_directories(base);
    const fs::path f = base / "empty.bin";
    { std::ofstream out(f, std::ios::binary); }
    // sha256 of the empty string
    REQUIRE(bs::mesh::sha256_file_stream(f.string()) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::error_code ec; fs::remove_all(base, ec);
}
