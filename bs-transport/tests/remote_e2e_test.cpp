// remote_e2e_test.cpp — opt-in remote E2E test via a caller-provided tunnel
#include <catch2/catch_test_macros.hpp>
#include "bstransport/tls.hpp"
#include "bstransport/frame_io.hpp"
#include "bsprotocol/codec.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>

using namespace bs::protocol;
using namespace bs::transport;

TEST_CASE("Remote E2E: configured peer", "[remote-e2e]") {
    const char* cert = std::getenv("BS_REMOTE_E2E_CERT");
    const char* key = std::getenv("BS_REMOTE_E2E_KEY");
    INFO("set BS_REMOTE_E2E_CERT to a test certificate");
    REQUIRE(cert != nullptr);
    REQUIRE(*cert != '\0');
    INFO("set BS_REMOTE_E2E_KEY to the matching private key");
    REQUIRE(key != nullptr);
    REQUIRE(*key != '\0');
    const char* host = std::getenv("BS_REMOTE_E2E_HOST");
    const char* port = std::getenv("BS_REMOTE_E2E_PORT");
    if (!host || !*host) host = "127.0.0.1";
    if (!port || !*port) port = "9948";

    struct addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* ai;
    REQUIRE(getaddrinfo(host, port, &hints, &ai) == 0);
    int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol); REQUIRE(sock >= 0);
    REQUIRE(connect(sock, ai->ai_addr, ai->ai_addrlen) == 0);
    freeaddrinfo(ai);

    ClientConfig cfg; cfg.cert_file = cert; cfg.key_file = key;
    cfg.known_servers_file = "/tmp/bs-remote.json";
    ::unlink(cfg.known_servers_file.c_str());

    auto ctx = create_client_context(cfg, [](auto& fp){ fprintf(stderr,"TOFU %s\n",fp.c_str()); return true; });
    REQUIRE(ctx);
    auto ssl = SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(), sock);
    REQUIRE(SSL_connect(ssl.get()) > 0);

    AttachMsg at;
    at.session_name = "e2e-remote:echo MAC2LINUX_OK; exit 0";
    at.cols = 80; at.rows = 24; at.term = "xterm-256color";
    write_frame(ssl.get(), at);

    // Read frames — handle Ping, collect Output, stop on ExitCode
    std::string out;
    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(25);

    while (std::chrono::steady_clock::now() < dl) {
        struct pollfd pfd; pfd.fd = sock; pfd.events = POLLIN;
        if (poll(&pfd, 1, 2000) <= 0) continue;

        try {
            auto m = read_frame(ssl.get());
            if (auto* o = std::get_if<OutputMsg>(&m))        out += o->data;
            else if (std::get_if<ExitCodeMsg>(&m))           break;
            else if (std::get_if<SessionDiedMsg>(&m))        break;
            else if (std::get_if<PingMsg>(&m))               write_frame(ssl.get(), PongMsg{});
        } catch (...) { break; }
    }

    SSL_shutdown(ssl.get()); close(sock);

    INFO("output: " << out);
    REQUIRE_FALSE(out.empty());
    REQUIRE(out.find("MAC2LINUX_OK") != std::string::npos);
}
