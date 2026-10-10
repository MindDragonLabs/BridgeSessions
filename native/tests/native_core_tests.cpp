#include "bridge_native/client.hpp"
#include "bridge_native/utf8.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace bridge_native;
void check(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
struct Request { std::string method, target, body; std::map<std::string, std::string> headers; };
struct Reply { int status = 200; std::string body = R"({"ok":true})"; int delay_ms = 0; std::string raw; };

class Fixture {
public:
    explicit Fixture(bool tls = false) : tls_(tls) {
        if (tls_ && !certificate()) return;
        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) return;
        int reuse = 1;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(socket_, 8)) {
            close(socket_); socket_ = -1; return;
        }
        socklen_t size = sizeof(addr);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &size)) { close(socket_); socket_ = -1; return; }
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serve(); });
    }
    ~Fixture() {
        stop_ = true;
        if (socket_ >= 0) { shutdown(socket_, SHUT_RDWR); close(socket_); }
        if (worker_.joinable()) worker_.join();
        if (context_) SSL_CTX_free(context_);
    }
    bool available() const { return socket_ >= 0; }
    int port() const { return port_; }
    void once(Reply reply) { std::lock_guard lock(mutex_); replies_.push_back(std::move(reply)); }
    std::vector<Request> requests() const { std::lock_guard lock(mutex_); return requests_; }
private:
    bool certificate() {
        context_ = SSL_CTX_new(TLS_server_method());
        EVP_PKEY_CTX* generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY* key = nullptr;
        X509* cert = X509_new();
        bool ok = context_ && generator && cert && EVP_PKEY_keygen_init(generator) > 0 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) > 0 && EVP_PKEY_keygen(generator, &key) > 0;
        if (ok) {
            X509_set_version(cert, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
            X509_gmtime_adj(X509_get_notBefore(cert), 0);
            X509_gmtime_adj(X509_get_notAfter(cert), 3600);
            X509_set_pubkey(cert, key);
            X509_NAME* name = X509_get_subject_name(cert);
            X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                const_cast<char*>("IP:127.0.0.1,DNS:localhost"));
            ok = san && X509_add_ext(cert, san, -1) == 1;
            if (san) X509_EXTENSION_free(san);
            ok = ok && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1 &&
                X509_set_issuer_name(cert, name) == 1 && X509_sign(cert, key, EVP_sha256()) > 0 &&
                SSL_CTX_use_certificate(context_, cert) == 1 && SSL_CTX_use_PrivateKey(context_, key) == 1;
        }
        if (cert) X509_free(cert);
        if (key) EVP_PKEY_free(key);
        if (generator) EVP_PKEY_CTX_free(generator);
        return ok;
    }
    static bool read_request(int socket, Request& request) {
        timeval timeout{2, 0};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string raw;
        char data[4096];
        auto end = raw.find("\r\n\r\n");
        while (end == std::string::npos && raw.size() < 65536) {
            const auto got = recv(socket, data, sizeof(data), 0);
            if (got <= 0) return false;
            raw.append(data, static_cast<std::size_t>(got));
            end = raw.find("\r\n\r\n");
        }
        if (end == std::string::npos) return false;
        const auto first_end = raw.find("\r\n");
        const auto first = raw.substr(0, first_end);
        const auto split = first.find(' ');
        const auto second = first.find(' ', split + 1);
        if (split == std::string::npos || second == std::string::npos) return false;
        request.method = first.substr(0, split);
        request.target = first.substr(split + 1, second - split - 1);
        for (std::size_t cursor = first_end + 2; cursor < end;) {
            const auto line_end = raw.find("\r\n", cursor);
            if (line_end == std::string::npos || line_end > end) return false;
            const auto colon = raw.find(':', cursor);
            if (colon == std::string::npos || colon > line_end) return false;
            std::size_t value = colon + 1;
            while (value < line_end && raw[value] == ' ') ++value;
            request.headers.emplace(raw.substr(cursor, colon - cursor), raw.substr(value, line_end - value));
            cursor = line_end + 2;
        }
        const auto it = request.headers.find("Content-Length");
        if (it == request.headers.end()) return false;
        std::size_t length = 0;
        try { length = std::stoull(it->second); } catch (...) { return false; }
        if (length > 1024 * 1024) return false;
        request.body = raw.substr(end + 4);
        while (request.body.size() < length) {
            const auto got = recv(socket, data, sizeof(data), 0);
            if (got <= 0) return false;
            request.body.append(data, static_cast<std::size_t>(got));
        }
        request.body.resize(length);
        return true;
    }
    static Reply route(const Request& r) {
        if (r.headers.find("Authorization") == r.headers.end() || r.headers.at("Authorization") != "Bearer fixture-token")
            return {401, R"({"ok":false,"error":{"code":"unauthorized","message":"bad token"}})"};
        if (r.method == "GET" && r.target == "/api/v1/capabilities") return {200, R"({"ok":true,"version":1,"backend":{"connected":true},"routes":{"sessions":true,"session_input":true,"session_scrollback":true,"session_kill":true,"files":true,"chat":false}})"};
        if (r.method == "GET" && r.target == "/api/v1/peers") return {200, R"({"ok":true,"peers":[],"local":"fixture"})"};
        if (r.method == "GET" && r.target.rfind("/api/v1/sessions?machine=", 0) == 0) return {200, R"({"ok":true,"sessions":[]})"};
        if (r.method == "POST" && r.target == "/api/v1/sessions") return {200, R"({"ok":true,"session":"phase0"})"};
        if (r.method == "POST" && r.target == "/api/v1/sessions/input") return {200, R"({"ok":true})"};
        if (r.method == "GET" && r.target == "/api/v1/sessions/output?machine=fixture&session=phase0&offset=0&limit=65536")
            return {200, R"({"ok":true,"offset":15,"text_b64":"Zml4dHVyZS1vdXRwdXQK","reset":false})"};
        if (r.method == "GET" && r.target == "/api/v1/sessions/output?machine=fixture&session=phase0&offset=100&limit=65536")
            return {200, R"({"ok":true,"offset":4,"text_b64":"bmV3Cg==","reset":true})"};
        if (r.method == "DELETE" && r.target == "/api/v1/sessions?machine=fixture&session=phase0") return {200, R"({"ok":true})"};
        return {404, R"({"ok":false,"error":{"code":"not_found","message":"unexpected route"}})"};
    }
    void serve() {
        while (!stop_) {
            fd_set ready;
            FD_ZERO(&ready); FD_SET(socket_, &ready);
            timeval tv{0, 100000};
            if (select(socket_ + 1, &ready, nullptr, nullptr, &tv) <= 0) continue;
            const int peer = accept(socket_, nullptr, nullptr);
            if (peer < 0) continue;
            if (tls_) {
                SSL* ssl = SSL_new(context_);
                if (ssl) { SSL_set_fd(ssl, peer); SSL_accept(ssl); SSL_free(ssl); }
                close(peer); continue;
            }
            Request request;
            if (read_request(peer, request)) {
                Reply reply;
                {
                    std::lock_guard lock(mutex_);
                    requests_.push_back(request);
                    reply = replies_.empty() ? route(request) : std::move(replies_.front());
                    if (!replies_.empty()) replies_.pop_front();
                }
                if (reply.delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
                const std::string wire = !reply.raw.empty() ? reply.raw : "HTTP/1.1 " + std::to_string(reply.status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body;
                for (std::size_t sent = 0; sent < wire.size();) {
                    const auto count = send(peer, wire.data() + sent, wire.size() - sent, 0);
                    if (count <= 0) break;
                    sent += static_cast<std::size_t>(count);
                }
            }
            shutdown(peer, SHUT_RDWR); close(peer);
        }
    }
    bool tls_;
    SSL_CTX* context_ = nullptr;
    int socket_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::deque<Reply> replies_;
    std::vector<Request> requests_;
};

ClientOptions options(const Fixture& f) {
    ClientOptions o;
    o.base_url = "http://127.0.0.1:" + std::to_string(f.port());
    o.bearer_token = "fixture-token";
    o.http.timeout_ms = 1000;
    return o;
}
void offline_tests() {
    IncrementalUtf8Decoder decoder;
    const std::string sample = "ok: \xE2\x9C\x93";
    check(decoder.feed(std::string_view(sample).substr(0, sample.size() - 1)) == "ok: ", "UTF-8 prefix");
    check(decoder.feed(std::string_view(sample).substr(sample.size() - 1)) == "\xE2\x9C\x93", "UTF-8 suffix");
    check(decoder.finish().empty(), "UTF-8 finish");
    check(decoder.feed("\xF0\x28\x8C\x28") == "\xEF\xBF\xBD(\xEF\xBF\xBD(", "invalid UTF-8");
    HttpTransport transport;
    struct Bad { const char* name; HttpRequest request; const char* code; };
    const std::vector<Bad> bad = {
        {"remote plain HTTP", {"GET", "http://remote.example.test/", {}, {}}, "insecure_transport"},
        {"invalid scheme", {"GET", "ftp://localhost/", {}, {}}, "invalid_url"},
        {"invalid port", {"GET", "https://localhost:bad/", {}, {}}, "invalid_url"},
        {"invalid method", {"GeT", "https://localhost/", {}, {}}, "invalid_request"},
        {"header newline", {"GET", "https://localhost/", {{"X-Test", "a\nb"}}, {}}, "invalid_request"},
    };
    for (const auto& item : bad) {
        const auto got = transport.perform(item.request);
        check(!got.ok() && got.error().code == item.code, item.name);
    }
    CancellationToken cancel; cancel.cancel();
    const auto cancelled = transport.perform({"GET", "https://localhost/", {}, {}}, &cancel);
    check(!cancelled.ok() && cancelled.error().code == "cancelled", "pre-cancelled request");
    BridgePanelClient client({"http://127.0.0.1:1", "fixture-token", {}});
    for (const auto& [cols, rows] : std::vector<std::pair<int, int>>{{0,24},{-1,24},{501,24},{80,0},{80,-1},{80,301}})
        check(!client.create_session("fixture", "phase0", "cat", cols, rows).ok(), "terminal dimension guard");
    check(!client.send_input("fixture", "phase0", std::string(65537, 'x')).ok(), "input size guard");
    for (auto limit : {std::size_t(0), std::size_t(65537)})
        check(!client.read_output("fixture", "phase0", 0, limit).ok(), "output size guard");
    HttpOptions bounded; bounded.limits.max_request_bytes = 1;
    const auto large_request = HttpTransport(bounded).perform({"POST", "http://127.0.0.1:1/", {}, "ab"});
    check(!large_request.ok() && large_request.error().code == "request_too_large", "request body bound");
}
void routes(Fixture& f) {
    BridgePanelClient c(options(f));
    check(c.capabilities().ok(), "capabilities");
    check(c.peers().ok(), "peers");
    check(c.sessions("fixture").ok(), "sessions");
    check(c.sessions("lab A&?").ok(), "encoded sessions machine");
    check(c.create_session("fixture", "phase0", "cat", 100, 30).ok(), "create");
    check(c.send_input("fixture", "phase0", std::string("marker\n\0x", 9)).ok(), "input");
    const auto out = c.read_output("fixture", "phase0", 0);
    check(out.ok() && out.value().bytes == "fixture-output\n" && out.value().offset == 15 && !out.value().reset, "next-byte cursor");
    const auto reset = c.read_output("fixture", "phase0", 100);
    check(reset.ok() && reset.value().bytes == "new\n" && reset.value().offset == 4 && reset.value().reset, "reset cursor");
    check(c.kill_session("fixture", "phase0").ok(), "kill");
    const auto requests = f.requests();
    check(requests.size() == 9, "nine requests captured");
    for (const auto& r : requests) {
        check(r.headers.at("Authorization") == "Bearer fixture-token", "bearer header");
        check(r.target.find("fixture-token") == std::string::npos, "token absent from URL");
    }
    check(requests[0].target == "/api/v1/capabilities" && requests[1].target == "/api/v1/peers" &&
        requests[2].target == "/api/v1/sessions?machine=fixture" &&
        requests[3].target == "/api/v1/sessions?machine=lab%20A%26%3F", "route/query contract");
    const Json created = Json::parse(requests[4].body);
    check(requests[4].headers.at("Content-Type") == "application/json" && created.at("machine") == "fixture" &&
        created.at("name") == "phase0" && created.at("command") == "cat" &&
        created.at("cols") == 100 && created.at("rows") == 30, "create payload");
    const Json input = Json::parse(requests[5].body);
    check(input.at("machine") == "fixture" && input.at("session") == "phase0" &&
        input.at("data_b64") == "bWFya2VyCgB4", "full binary input payload");
}
void negative(Fixture& f) {
    BridgePanelClient c(options(f));
    struct Case { const char* name; int status; const char* body; const char* code; bool short_limit = false; };
    const std::vector<Case> cases = {
        {"malformed JSON", 200, "{", "invalid_api_response"},
        {"scalar JSON", 200, "[]", "invalid_api_response"},
        {"string offset", 200, R"({"ok":true,"offset":"0","text_b64":"","reset":false})", "invalid_api_response"},
        {"negative offset", 200, R"({"ok":true,"offset":-1,"text_b64":"","reset":false})", "invalid_api_response"},
        {"boolean offset", 200, R"({"ok":true,"offset":false,"text_b64":"","reset":false})", "invalid_api_response"},
        {"fractional offset", 200, R"({"ok":true,"offset":0.5,"text_b64":"","reset":false})", "invalid_api_response"},
        {"number base64", 200, R"({"ok":true,"offset":0,"text_b64":4,"reset":false})", "invalid_api_response"},
        {"bad base64", 200, R"({"ok":true,"offset":0,"text_b64":"@@==","reset":false})", "invalid_base64"},
        {"bad padding", 200, R"({"ok":true,"offset":1,"text_b64":"Zh==","reset":false})", "invalid_base64"},
        {"internal padding", 200, R"({"ok":true,"offset":0,"text_b64":"AA=A","reset":false})", "invalid_base64"},
        {"short base64", 200, R"({"ok":true,"offset":0,"text_b64":"YQ=","reset":false})", "invalid_base64"},
        {"string reset", 200, R"({"ok":true,"offset":0,"text_b64":"","reset":"false"})", "invalid_api_response"},
        {"missing reset", 200, R"({"ok":true,"offset":0,"text_b64":""})", "invalid_api_response"},
        {"wrong next cursor", 200, R"({"ok":true,"offset":1,"text_b64":"YWI=","reset":false})", "invalid_api_response"},
        {"chunk exceeds limit", 200, R"({"ok":true,"offset":2,"text_b64":"YWI=","reset":false})", "response_too_large", true},
        {"panel error", 503, R"({"ok":false,"error":{"code":"worker_unavailable","message":"offline"}})", "worker_unavailable"},
    };
    for (const auto& item : cases) {
        f.once({item.status, item.body});
        const auto result = c.read_output("fixture", "phase0", 0, item.short_limit ? 1 : 65536);
        check(!result.ok() && result.error().code == item.code, std::string(item.name) +
            (result.ok() ? ": unexpectedly succeeded" : ": got " + result.error().code));
    }
    check(c.send_input("fixture", "phase0", std::string(65536, 'a')).ok(), "maximum input accepted");
    const auto input = Json::parse(f.requests().back().body).at("data_b64").get<std::string>();
    check(input.size() == 87384 && input.ends_with("YQ=="), "large Content-Length payload captured fully");
    check(c.create_session("fixture", "phase0", "cat", 500, 300).ok(), "maximum terminal dimensions accepted");
    const auto before = f.requests().size();
    check(!c.read_output("fixture", "phase0", 0, 0).ok(), "zero limit");
    check(!c.read_output("fixture", "phase0", 0, 65537).ok(), "large limit");
    check(!c.create_session("fixture", "phase0", "cat", 0, 24).ok(), "zero columns");
    check(!c.create_session("fixture", "phase0", "cat", 501, 24).ok(), "large columns");
    check(!c.create_session("fixture", "phase0", "cat", 80, 301).ok(), "large rows");
    check(f.requests().size() == before, "invalid inputs must not send requests");
}
void timeout(Fixture& f) {
    f.once({200, R"({"ok":true})", 250});
    auto o = options(f); o.http.timeout_ms = 40;
    BridgePanelClient c(o);
    const auto result = c.capabilities();
    check(!result.ok() && result.error().code == "read_failed" && result.error().retryable, "timeout failure");
}
void parser_tests(Fixture& f) {
    struct Case { const char* name; std::string wire; const char* code; };
    const std::vector<Case> cases = {
        {"bad status", "HTTP/1.1 200junk OK\r\nContent-Length: 0\r\n\r\n", "invalid_response"},
        {"bad version", "HTTP/2 200 OK\r\nContent-Length: 0\r\n\r\n", "invalid_response"},
        {"duplicate header", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n", "invalid_response"},
        {"bad length", "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", "invalid_response"},
        {"ambiguous framing", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "invalid_response"},
        {"unsupported encoding", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", "invalid_response"},
        {"bad chunk size", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n", "invalid_response"},
        {"truncated body", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nab", "truncated_response"},
        {"truncated final chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", "truncated_response"},
        {"bad chunk terminator", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\naXX", "invalid_response"},
        {"huge chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\n", "response_too_large"},
        {"headers bound", "HTTP/1.1 200 OK\r\nX-Long: " + std::string(512, 'x') + "\r\n\r\n", "headers_too_large"},
        {"body bound", "HTTP/1.1 200 OK\r\nContent-Length: 65\r\n\r\n", "response_too_large"},
        {"unframed body bound", "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + std::string(65, 'x'), "response_too_large"},
    };
    auto o = options(f).http; o.limits.max_header_bytes = 256; o.limits.max_body_bytes = 64;
    HttpTransport transport(o);
    const HttpRequest request{"GET", options(f).base_url + "/parser", {}, {}};
    for (const auto& item : cases) {
        f.once({200, {}, 0, item.wire});
        const auto result = transport.perform(request);
        check(!result.ok() && result.error().code == item.code, std::string(item.name) +
            (result.ok() ? ": unexpectedly succeeded" : ": got " + result.error().code));
    }
    for (const std::string& wire : {
        std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n"),
        std::string("HTTP/1.0 200 OK\r\n\r\nab")}) {
        f.once({200, {}, 0, wire});
        const auto result = transport.perform(request);
        check(result.ok() && result.value().body == "ab", "valid HTTP framing");
    }
    auto coalesced = options(f).http; coalesced.limits.max_header_bytes = 128;
    const std::string bytes(1024, 'x');
    f.once({200, bytes});
    const auto result = HttpTransport(coalesced).perform(request);
    check(result.ok() && result.value().body == bytes, "body arriving with headers is not a large header");
}
void cancellation(Fixture& f) {
    f.once({200, R"({"ok":true})", 150});
    const auto before = f.requests().size();
    CancellationToken token;
    auto o = options(f).http; o.timeout_ms = 50;
    std::thread cancel([&] {
        for (int n = 0; n < 100 && f.requests().size() == before; ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        token.cancel();
    });
    const auto result = HttpTransport(o).perform({"GET", options(f).base_url + "/cancel", {}, {}}, &token);
    cancel.join();
    check(!result.ok() && result.error().code == "cancelled", "in-flight cancellation");
}
void untrusted_tls() {
    Fixture f(true);
    check(f.available(), "TLS fixture unavailable: certificate or loopback bind failed");
    HttpTransport transport;
    const auto result = transport.perform({"GET", "https://127.0.0.1:" + std::to_string(f.port()) + "/", {}, {}});
    check(!result.ok() && result.error().code == "tls_handshake", "self-signed TLS rejected");
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGPIPE, SIG_IGN);
        offline_tests();
        if (argc > 1 && std::string(argv[1]) == "--offline") {
            std::cout << "Offline parser and guard tests passed; loopback tests were not run.\n";
            return EXIT_SUCCESS;
        }
        Fixture fixture;
        check(fixture.available(), "HTTP fixture unavailable: cannot bind loopback");
        routes(fixture);
        negative(fixture);
        parser_tests(fixture);
        cancellation(fixture);
        // Use a separate listener so cancellation's delayed reply cannot affect timeout.
        Fixture timeout_fixture;
        check(timeout_fixture.available(), "timeout fixture unavailable: cannot bind loopback");
        timeout(timeout_fixture);
        untrusted_tls();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
