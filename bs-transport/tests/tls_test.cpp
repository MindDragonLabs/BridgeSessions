#include <catch2/catch_test_macros.hpp>
#include "bstransport/tls.hpp"
#include "bstransport/frame_io.hpp"
#include "bsprotocol/codec.hpp"
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <thread>
#include <fstream>
#include <cstdio>
#include <atomic>

using namespace bs::protocol;
using namespace bs::transport;

namespace {

struct TmpFile {
    std::string path;
    TmpFile(const std::string& content) {
#ifdef _WIN32
        char tmpl[MAX_PATH];
        char tmpPath[MAX_PATH];
        GetTempPathA(sizeof(tmpPath), tmpPath);
        GetTempFileNameA(tmpPath, "bst", 0, tmpl);
        path = tmpl;
        FILE* f = fopen(path.c_str(), "w");
        if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
#else
        char tmpl[] = "/tmp/bridgesessions_test_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) { write(fd, content.data(), content.size()); CLOSESOCK(fd); path = tmpl; }
#endif
    }
    ~TmpFile() { if (!path.empty()) std::remove(path.c_str()); }
};

#ifdef _WIN32
#define CLOSESOCK closesocket
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } ~WsaInit() { WSACleanup(); } };
static WsaInit _wsa;
#else
#define CLOSESOCK close
#endif

auto cert_key() { return generate_cert_key_pair("test.bridgesessions"); }

int listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    REQUIRE(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(listen(fd, 1) == 0);
    return fd;
}

int get_port(int lfd) {
    sockaddr_in addr{}; socklen_t len = sizeof(addr);
    getsockname(lfd, (sockaddr*)&addr, &len);
    return ntohs(addr.sin_port);
}

} // namespace

// ── Basic TLS tests ─────────────────────────────────────────────

TEST_CASE("generate_cert_key_pair produces valid ed25519 cert/key", "[tls]") {
    auto [cert_pem, key_pem] = cert_key();
    REQUIRE(cert_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    REQUIRE(key_pem.find("BEGIN PRIVATE KEY") != std::string::npos);
}

TEST_CASE("pubkey_hex_from_pem returns 64-char hex string", "[tls]") {
    auto [_, key_pem] = cert_key();
    auto hex = pubkey_hex_from_pem(key_pem);
    REQUIRE(hex.size() == 64);
}

// ── Integration tests ───────────────────────────────────────────

TEST_CASE("TLS loopback with mutual ed25519 auth", "[tls][integration]") {
    auto [server_cert, server_key] = cert_key();
    auto [client_cert, client_key] = cert_key();

    TmpFile sc(server_cert), sk(server_key), cc(client_cert), ck(client_key);
    auto client_hex = pubkey_hex_from_pem(client_key);
    TmpFile ak(client_hex + "\n");
    TmpFile known_servers("");

    bool tofu_accepted = false;
    std::string saved_fp;
    int lfd = listen_socket();
    int port = get_port(lfd);

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);
        ServerConfig scfg{sc.path, sk.path, ak.path};
        auto ctx = create_server_context(scfg);
        REQUIRE(ctx != nullptr);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) >= 0);
        REQUIRE(peer_public_key_hex(ssl.get()) == client_hex);
        auto msg = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<KeystrokeMsg>(msg));
        OutputMsg reply{">> " + std::get<KeystrokeMsg>(msg).data + " <<"};
        write_frame(ssl.get(), reply, 1);
        CLOSESOCK(cfd);
    });

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
    REQUIRE(connect(cfd, (sockaddr*)&addr, sizeof(addr)) == 0);

    ClientConfig ccfg{cc.path, ck.path, known_servers.path};
    auto ctx = create_client_context(ccfg, [&](const std::string& fp) { tofu_accepted = true; saved_fp = fp; return true; });
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(), cfd);
    REQUIRE(SSL_connect(ssl.get()) >= 0);
    write_frame(ssl.get(), KeystrokeMsg{"HelloServer"}, 1);
    auto reply = read_frame(ssl.get());
    REQUIRE(std::holds_alternative<OutputMsg>(reply));
    REQUIRE(std::get<OutputMsg>(reply).data == ">> HelloServer <<");
    REQUIRE(tofu_accepted); REQUIRE(!saved_fp.empty());

    CLOSESOCK(cfd); server_thread.join(); CLOSESOCK(lfd);
}

TEST_CASE("wrong server key rejected by TOFU callback", "[tls][integration]") {
    auto [server_cert, server_key] = cert_key();
    auto [client_cert, client_key] = cert_key();
    TmpFile sc(server_cert), sk(server_key), cc(client_cert), ck(client_key), ks("");
    auto client_hex = pubkey_hex_from_pem(client_key);
    TmpFile ak(client_hex + "\n");

    int lfd = listen_socket(); int port = get_port(lfd);
    std::thread server_thread([&] { int cfd = accept(lfd,nullptr,nullptr); if(cfd>=0){ServerConfig scfg{sc.path,sk.path,ak.path};auto ctx=create_server_context(scfg);auto ssl=SslPtr(SSL_new(ctx.get()));SSL_set_fd(ssl.get(),cfd);SSL_accept(ssl.get());CLOSESOCK(cfd);} });

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
    REQUIRE(connect(cfd,(sockaddr*)&addr,sizeof(addr))==0);
    ClientConfig ccfg{cc.path,ck.path,ks.path};
    auto ctx=create_client_context(ccfg,[](auto&){return false;});
    auto ssl=SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(),cfd);
    int ret=SSL_connect(ssl.get());
    REQUIRE(ret<=0);  // TOFU callback rejected
    CLOSESOCK(cfd); server_thread.join(); CLOSESOCK(lfd);
}

TEST_CASE("wrong client key rejected at handshake or first read", "[tls][integration]") {
    auto [server_cert, server_key] = cert_key();
    auto [client_cert, client_key] = cert_key();
    auto [wrong_cert, wrong_key] = cert_key();
    TmpFile sc(server_cert), sk(server_key), wc(wrong_cert), wk(wrong_key), ks("");
    auto other_hex = pubkey_hex_from_pem(client_key);  // authorized_keys has a DIFFERENT key
    TmpFile ak(other_hex + "\n");

    int lfd = listen_socket(); int port = get_port(lfd);
    std::atomic<bool> rejected{false};
    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) { rejected = true; return; }
        ServerConfig scfg{sc.path, sk.path, ak.path};
        auto ctx = create_server_context(scfg);
        auto ssl = SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { rejected = true; }
        else { try { [[maybe_unused]] auto ignored = read_frame(ssl.get()); } catch (...) { rejected = true; } }
        CLOSESOCK(cfd);
    });

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
    REQUIRE(connect(cfd,(sockaddr*)&addr,sizeof(addr))==0);
    ClientConfig ccfg{wc.path,wk.path,ks.path};
    auto ctx=create_client_context(ccfg,[](auto&){return true;});
    auto ssl=SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(),cfd);
    int ret=SSL_connect(ssl.get());
    bool client_failed=(ret<=0);
    if(ret>0){try{write_frame(ssl.get(),KeystrokeMsg{"x"},1);}catch(...){client_failed=true;}}
    CLOSESOCK(cfd); server_thread.join(); CLOSESOCK(lfd);
    REQUIRE((client_failed||rejected));
}

TEST_CASE("compressed frame roundtrip over TLS", "[tls][integration]") {
    auto [server_cert, server_key] = cert_key();
    auto [client_cert, client_key] = cert_key();
    TmpFile sc(server_cert), sk(server_key), cc(client_cert), ck(client_key), ks("");
    auto client_hex = pubkey_hex_from_pem(client_key);
    TmpFile ak(client_hex + "\n");

    int lfd = listen_socket(); int port = get_port(lfd);
    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);
        ServerConfig scfg{sc.path, sk.path, ak.path};
        auto ctx = create_server_context(scfg);
        auto ssl = SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(), cfd); SSL_accept(ssl.get());
        auto msg = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<OutputMsg>(msg));
        write_frame(ssl.get(), std::get<OutputMsg>(msg), 1);
        CLOSESOCK(cfd);
    });

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
    REQUIRE(connect(cfd,(sockaddr*)&addr,sizeof(addr))==0);
    ClientConfig ccfg{cc.path,ck.path,ks.path};
    auto ctx=create_client_context(ccfg,[](auto&){return true;});
    auto ssl=SslPtr(SSL_new(ctx.get())); SSL_set_fd(ssl.get(),cfd); SSL_connect(ssl.get());
    std::string big_data(2000, 'X');
    OutputMsg big{big_data};
    auto frame = encode(big, 1);
    REQUIRE((frame[3] & FLAG_COMPRESSED) != 0);
    write_frame(ssl.get(), big, 1);
    auto reply = read_frame(ssl.get());
    REQUIRE(std::holds_alternative<OutputMsg>(reply));
    REQUIRE(std::get<OutputMsg>(reply).data == big_data);
    CLOSESOCK(cfd); server_thread.join(); CLOSESOCK(lfd);
}
