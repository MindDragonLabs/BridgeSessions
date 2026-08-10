// test_tcp_nodelay.cpp — verify TCP_NODELAY is set for interactive shell performance
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <netinet/tcp.h>

using namespace bs::mesh;

TEST_CASE("TCP_NODELAY: set_tcp_nodelay handles INVALID_SOCKET", "[nodelay]") {
    // Should not crash on invalid socket
    set_tcp_nodelay(INVALID_SOCKET);
    REQUIRE(true); // If we got here, no crash
}

TEST_CASE("TCP_NODELAY: verify option is set after calling set_tcp_nodelay", "[nodelay]") {
    // Create a pair of connected sockets
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ephemeral

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    REQUIRE(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(listen_fd, 1) == 0);

    socklen_t addrlen = sizeof(addr);
    REQUIRE(getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen) == 0);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client_fd >= 0);
    REQUIRE(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    int server_fd = accept(listen_fd, nullptr, nullptr);
    REQUIRE(server_fd >= 0);

    SECTION("client socket gets TCP_NODELAY") {
        set_tcp_nodelay(client_fd);
        int flag = 0;
        socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, &len) == 0);
        REQUIRE(flag != 0);
    }

    SECTION("server socket gets TCP_NODELAY") {
        set_tcp_nodelay(server_fd);
        int flag = 0;
        socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &flag, &len) == 0);
        REQUIRE(flag != 0);
    }

    SECTION("default is Nagle ON (flag=0) before calling set_tcp_nodelay") {
        int flag = -1;
        socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, &len) == 0);
        // Most systems default to Nagle ON (flag=0). This confirms we NEED the fix.
        // (Don't hard-assert 0 — some kernels might differ, but log it)
        INFO("Default TCP_NODELAY value: " << flag);
    }

    close(client_fd);
    close(server_fd);
    close(listen_fd);
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
