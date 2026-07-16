// ring_buffer_test.cpp — Phase 5: unit tests for RingBuffer
#include "ring_buffer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace bs::server;

TEST_CASE("RingBuffer: basic write and snapshot", "[ring_buffer]") {
    RingBuffer<4096> rb;
    rb.write(std::string_view("hello world"));
    auto snap = rb.snapshot();
    REQUIRE(std::string(snap.begin(), snap.end()) == "hello world");
}

TEST_CASE("RingBuffer: read_last_lines", "[ring_buffer]") {
    RingBuffer<4096> rb;
    rb.write(std::string_view("line1\nline2\nline3\nline4\nline5\n"));

    auto last1 = rb.read_last_lines(1);
    REQUIRE(last1 == "line5\n");

    auto last2 = rb.read_last_lines(2);
    REQUIRE(last2 == "line4\nline5\n");

    auto last10 = rb.read_last_lines(10);
    // All lines, starts at beginning
    REQUIRE(last10 == "line1\nline2\nline3\nline4\nline5\n");
}

TEST_CASE("RingBuffer: read_last_lines with no trailing newline", "[ring_buffer]") {
    RingBuffer<4096> rb;
    rb.write(std::string_view("a\nb\nc"));  // no trailing newline

    auto last = rb.read_last_lines(1);
    // "c" is the last line even without trailing \n
    REQUIRE(last == "c");
}

TEST_CASE("RingBuffer: wrap around", "[ring_buffer]") {
    RingBuffer<1024> rb;
    // Write 2048 bytes — forces wrap
    std::string data(2048, 'x');
    rb.write(std::string_view(data));

    // Should only keep last 1024 bytes
    REQUIRE(rb.size() == 1024);
    auto snap = rb.snapshot();
    REQUIRE(snap.size() == 1024);
    // All 'x', from the second half
    for (auto c : snap) REQUIRE(c == 'x');
}

TEST_CASE("RingBuffer: wrap lines across boundary", "[ring_buffer]") {
    // Capacity 1024, write 900 bytes of A then 128-byte lines that wrap
    RingBuffer<1024> rb;
    std::string prefix(900, 'A');
    rb.write(std::string_view(prefix));

    // Write marker lines — these will cross the wrap boundary
    rb.write(std::string_view("MARKER_ONE\n"));
    rb.write(std::string_view("MARKER_TWO\n"));
    rb.write(std::string_view("MARKER_THREE\n"));

    auto last2 = rb.read_last_lines(2);
    REQUIRE(last2.find("MARKER_TWO") != std::string::npos);
    REQUIRE(last2.find("MARKER_THREE") != std::string::npos);
}

TEST_CASE("RingBuffer: read_range for chunked replay", "[ring_buffer]") {
    RingBuffer<4096> rb;
    rb.write(std::string_view("0123456789"));

    auto chunk = rb.read_range(2, 4);
    REQUIRE(chunk == "2345");

    // Offset past end
    auto over = rb.read_range(100, 10);
    REQUIRE(over.empty());

    // Default at start
    auto start = rb.read_range(0, 3);
    REQUIRE(start == "012");
}

TEST_CASE("RingBuffer: clear", "[ring_buffer]") {
    RingBuffer<4096> rb;
    rb.write(std::string_view("data"));
    REQUIRE(rb.size() == 4);

    rb.clear();
    REQUIRE(rb.size() == 0);
    REQUIRE(rb.snapshot().empty());
}

TEST_CASE("RingBuffer: empty buffer operations", "[ring_buffer]") {
    RingBuffer<4096> rb;
    REQUIRE(rb.size() == 0);
    REQUIRE(rb.snapshot().empty());
    REQUIRE(rb.read_last_lines(10).empty());
    REQUIRE(rb.read_range(0, 10).empty());
}

TEST_CASE("RingBuffer: exact capacity write", "[ring_buffer]") {
    RingBuffer<1024> rb;
    std::string data(1024, 'Z');
    rb.write(std::string_view(data));
    REQUIRE(rb.size() == 1024);

    auto snap = rb.snapshot();
    REQUIRE(snap.size() == 1024);
    for (auto c : snap) REQUIRE(c == 'Z');
}

TEST_CASE("RingBuffer: single large write exceeds capacity", "[ring_buffer]") {
    RingBuffer<1024> rb;
    std::string data(2048, 'Y');
    rb.write(std::string_view(data));

    // Only last 1024 bytes retained
    REQUIRE(rb.size() == 1024);
    auto snap = rb.snapshot();
    REQUIRE(snap.size() == 1024);
    for (auto c : snap) REQUIRE(c == 'Y');
}

TEST_CASE("RingBuffer: thread safety — concurrent writes", "[ring_buffer]") {
    RingBuffer<65536> rb;
    constexpr int kThreads = 4;
    constexpr int kIters = 1000;

    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&rb, t]() {
            for (int i = 0; i < kIters; ++i) {
                std::string msg = "thread" + std::to_string(t) + "-" + std::to_string(i) + "\n";
                rb.write(std::string_view(msg));
            }
        });
    }

    threads.clear(); // join all

    // All writes should have completed
    REQUIRE(rb.size() > 0);
    // Snapshot should be readable without data races
    auto snap = rb.snapshot();
    REQUIRE(snap.size() > 0);
}
