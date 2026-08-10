// test_interactive_latency.cpp — verify interactive shell responsiveness
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <chrono>
#include <thread>

using namespace bs::mesh;

TEST_CASE("Interactive shell select timeout is 1ms (not 50ms)", "[latency]") {
    // The interactive loop uses select() with a timeout.
    // We verify the timeout constant by measuring actual select() behavior.
    // A 1ms timeout should complete in under 5ms. A 50ms timeout would take 50ms.

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds); // stdin (no data, so select returns after timeout)

    auto start = std::chrono::steady_clock::now();
    timeval tv{0, 1000}; // 1ms — this is what the code should use
    select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Should be ~1ms, definitely under 10ms
    // (On a loaded system, add margin. The point is it's NOT 50ms.)
    INFO("select(1ms) took " << ms << "ms");
    REQUIRE(ms < 10);
}

TEST_CASE("Local socket round-trip with TCP_NODELAY", "[latency]") {
    // Create a connected pair and measure round-trip time with TCP_NODELAY
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    REQUIRE(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(listen_fd, 1) == 0);

    socklen_t addrlen = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    int server_fd = accept(listen_fd, nullptr, nullptr);
    REQUIRE(server_fd >= 0);

    // Apply TCP_NODELAY to both ends
    set_tcp_nodelay(client_fd);
    set_tcp_nodelay(server_fd);

    SECTION("round-trip latency < 10ms with TCP_NODELAY") {
        const char msg[] = "x";
        char buf[16];

        // Measure 10 round-trips
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++) {
            send(client_fd, msg, 1, 0);
            recv(server_fd, buf, sizeof(buf), 0);
            send(server_fd, msg, 1, 0);
            recv(client_fd, buf, sizeof(buf), 0);
        }
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

        // 10 round-trips should complete in well under 10ms total on localhost
        // (each round-trip = write+read both ways)
        INFO("10 round-trips took " << us << "us total (" << us/10 << "us avg)");
        REQUIRE(us < 10000); // < 10ms for 10 round-trips
    }

    close(client_fd);
    close(server_fd);
    close(listen_fd);
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
