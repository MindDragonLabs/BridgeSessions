#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bridgesessions.cpp"

#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// Use 256 for ring buffer — power of 2
static constexpr size_t kCapacity = 256;

TEST_CASE("write + snapshot returns correct content", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;

    SECTION("write 'hello' then snapshot returns 'hello' with size=5") {
        rb.write(std::string_view("hello"));
        REQUIRE(rb.total_written() == 5);
        REQUIRE(rb.size() == 5);

        auto snap = rb.snapshot();
        std::string s(snap.begin(), snap.end());
        REQUIRE(s == "hello");
    }

    SECTION("write 'world' after 'hello' — snapshot returns 'helloworld'") {
        rb.write(std::string_view("hello"));
        rb.write(std::string_view("world"));
        REQUIRE(rb.total_written() == 10);
        REQUIRE(rb.size() == 10);

        auto snap = rb.snapshot();
        std::string s(snap.begin(), snap.end());
        REQUIRE(s == "helloworld");
    }
}

TEST_CASE("overflow wraps correctly, size capped at Capacity", "[ringbuffer]") {
    // Use a small ring buffer of 64
    RingBuffer<64> rb;

    // Write 80 'A's — this exceeds the capacity, only last 64 survive
    std::string dataA(80, 'A');
    rb.write(std::string_view(dataA));
    // total_written is monotonic (total bytes ever written) — needed by
    // read_since/SCROLLBACK absolute offsets. Ring content still caps at 64.
    REQUIRE(rb.total_written() == 80);
    REQUIRE(rb.size() == 64);

    auto snap = rb.snapshot();
    std::string s(snap.begin(), snap.end());
    REQUIRE(s.size() == 64);
    // All should be 'A'
    REQUIRE(s == std::string(64, 'A'));

    // Write more to wrap normally
    RingBuffer<64> rb2;
    rb2.write(std::string_view(std::string(40, 'A')));
    rb2.write(std::string_view(std::string(40, 'B')));
    REQUIRE(rb2.total_written() == 80);
    REQUIRE(rb2.size() == 64);

    auto snap2 = rb2.snapshot();
    std::string s2(snap2.begin(), snap2.end());
    REQUIRE(s2.size() == 64);
    // Oldest 16 A's should be overwritten: 24 A's + 40 B's
    REQUIRE(s2 == std::string(24, 'A') + std::string(40, 'B'));
}

TEST_CASE("read_last_lines returns correct line count", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;

    SECTION("read_last_lines(2) returns last 2 lines") {
        rb.write(std::string_view("line1\nline2\nline3\nline4"));
        auto result = rb.read_last_lines(2);
        // Last 2 lines: "line3\nline4"
        REQUIRE(result == "line3\nline4");
    }

    SECTION("read_last_lines(1) returns only last line") {
        rb.write(std::string_view("aaa\nbbb\nccc"));
        auto result = rb.read_last_lines(1);
        REQUIRE(result == "ccc");
    }

    SECTION("read_last_lines handles trailing newline correctly (doesn't count empty line)") {
        // If content ends with '\n', read_last_lines should not count
        // the trailing empty string as a line.
        rb.write(std::string_view("hello\nworld\n"));
        auto result = rb.read_last_lines(2);
        // The 2 actual content lines: "hello\nworld\n" — where trailing \n is not a line
        REQUIRE(result == "hello\nworld\n");

        auto result1 = rb.read_last_lines(1);
        REQUIRE(result1 == "world\n");
    }

    SECTION("read_last_lines with no newlines returns everything") {
        rb.write(std::string_view("singleline"));
        auto result = rb.read_last_lines(3);
        REQUIRE(result == "singleline");
    }

    SECTION("read_last_lines(0) returns empty") {
        rb.write(std::string_view("hello\nworld"));
        auto result = rb.read_last_lines(0);
        REQUIRE(result.empty());
    }
}

TEST_CASE("read_range returns correct slice", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;
    rb.write(std::string_view("0123456789"));

    SECTION("read first 5 chars") {
        auto r = rb.read_range(0, 5);
        REQUIRE(r == "01234");
    }

    SECTION("read middle 3 chars") {
        auto r = rb.read_range(3, 3);
        REQUIRE(r == "345");
    }

    SECTION("offset beyond size returns empty") {
        auto r = rb.read_range(100, 5);
        REQUIRE(r.empty());
    }

    SECTION("length clamped to available chars") {
        auto r = rb.read_range(8, 10);
        REQUIRE(r == "89");
    }
}

TEST_CASE("clear resets everything", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;
    rb.write(std::string_view("hello world"));
    REQUIRE(rb.size() > 0);

    rb.clear();
    REQUIRE(rb.total_written() == 0);
    REQUIRE(rb.size() == 0);
    REQUIRE(rb.snapshot().empty());
    REQUIRE(rb.read_last_lines(1).empty());
}

TEST_CASE("move semantics preserve content in destination, source is empty", "[ringbuffer]") {
    RingBuffer<kCapacity> rb1;
    rb1.write(std::string_view("move me"));

    REQUIRE(rb1.total_written() == 7);
    REQUIRE(rb1.size() == 7);

    // Move construct
    RingBuffer<kCapacity> rb2(std::move(rb1));

    // Destination has the content
    REQUIRE(rb2.total_written() == 7);
    REQUIRE(rb2.size() == 7);
    auto snap = rb2.snapshot();
    std::string s(snap.begin(), snap.end());
    REQUIRE(s == "move me");

    // Move assign
    RingBuffer<kCapacity> rb3;
    rb3 = std::move(rb2);

    REQUIRE(rb3.total_written() == 7);
    REQUIRE(rb3.size() == 7);
    auto snap2 = rb3.snapshot();
    std::string s2(snap2.begin(), snap2.end());
    REQUIRE(s2 == "move me");
}

TEST_CASE("thread safety: write from 2 threads simultaneously", "[ringbuffer]") {
    RingBuffer<4096> rb;

    constexpr size_t n_per_thread = 10000;
    std::atomic<bool> start{false};

    auto writer = [&](char ch) {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < n_per_thread; ++i) {
            char buf = ch;
            rb.write(std::span<const char>(&buf, 1));
        }
    };

    std::thread t1(writer, 'A');
    std::thread t2(writer, 'B');

    start.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    REQUIRE(rb.total_written() == 2 * n_per_thread);
    REQUIRE(rb.size() == 4096);  // capped at Capacity
}

TEST_CASE("capacity() returns template parameter", "[ringbuffer]") {
    RingBuffer<64> rb64;
    REQUIRE(rb64.capacity() == 64);

    RingBuffer<256> rb256;
    REQUIRE(rb256.capacity() == 256);

    RingBuffer<4096> rb4k;
    REQUIRE(rb4k.capacity() == 4096);
}

TEST_CASE("empty buffer returns empty snapshot and reads", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;
    REQUIRE(rb.size() == 0);
    REQUIRE(rb.total_written() == 0);
    REQUIRE(rb.snapshot().empty());
    REQUIRE(rb.read_last_lines(1).empty());
    REQUIRE(rb.read_range(0, 10).empty());
}

TEST_CASE("string_view overload works same as span", "[ringbuffer]") {
    RingBuffer<kCapacity> rb;
    rb.write(std::string_view("hello"));
    auto snap = rb.snapshot();
    std::string s(snap.begin(), snap.end());
    REQUIRE(s == "hello");
    REQUIRE(rb.size() == 5);
}
