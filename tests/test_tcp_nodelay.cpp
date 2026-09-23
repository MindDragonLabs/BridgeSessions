// test_tcp_nodelay.cpp — verify TCP_NODELAY is set for interactive shell performance
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

using namespace bs::mesh;

#ifdef _WIN32
using test_socket_t = SOCKET;
using test_socklen_t = int;
struct WinsockLifetime {
    WSADATA data{};
    int status = WSAStartup(MAKEWORD(2, 2), &data);
    ~WinsockLifetime() { if (status == 0) WSACleanup(); }
};
static WinsockLifetime winsock_lifetime;
static void close_test_socket(test_socket_t fd) { closesocket(fd); }
#else
using test_socket_t = int;
using test_socklen_t = socklen_t;
static void close_test_socket(test_socket_t fd) { ::close(fd); }
#endif

TEST_CASE("TCP_NODELAY: set_tcp_nodelay handles INVALID_SOCKET", "[nodelay]") {
#ifdef _WIN32
    REQUIRE(winsock_lifetime.status == 0);
#endif
    // Should not crash on invalid socket
    set_tcp_nodelay(INVALID_SOCKET);
    REQUIRE(true); // If we got here, no crash
}

TEST_CASE("TCP_NODELAY: verify option is set after calling set_tcp_nodelay", "[nodelay]") {
    // Create a pair of connected sockets
    test_socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd != INVALID_SOCKET);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // ephemeral

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    REQUIRE(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(listen_fd, 1) == 0);

    test_socklen_t addrlen = sizeof(addr);
    REQUIRE(getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen) == 0);

    test_socket_t client_fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client_fd != INVALID_SOCKET);
    REQUIRE(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    test_socket_t server_fd = accept(listen_fd, nullptr, nullptr);
    REQUIRE(server_fd != INVALID_SOCKET);

    SECTION("client socket gets TCP_NODELAY") {
        set_tcp_nodelay(client_fd);
        int flag = 0;
        test_socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<char*>(&flag), &len) == 0);
        REQUIRE(flag != 0);
    }

    SECTION("server socket gets TCP_NODELAY") {
        set_tcp_nodelay(server_fd);
        int flag = 0;
        test_socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<char*>(&flag), &len) == 0);
        REQUIRE(flag != 0);
    }

    SECTION("default is Nagle ON (flag=0) before calling set_tcp_nodelay") {
        int flag = -1;
        test_socklen_t len = sizeof(flag);
        REQUIRE(getsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<char*>(&flag), &len) == 0);
        // Most systems default to Nagle ON (flag=0). This confirms we NEED the fix.
        // (Don't hard-assert 0 — some kernels might differ, but log it)
        INFO("Default TCP_NODELAY value: " << flag);
    }

    close_test_socket(client_fd);
    close_test_socket(server_fd);
    close_test_socket(listen_fd);
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
