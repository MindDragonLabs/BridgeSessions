#include "bridge_native/http.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bridge_native {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket s) { ::close(s); }
#endif

struct SocketGuard {
    Socket value = kInvalidSocket;
    ~SocketGuard() { if (value != kInvalidSocket) close_socket(value); }
    SocketGuard() = default;
    explicit SocketGuard(Socket socket) : value(socket) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept : value(other.value) { other.value = kInvalidSocket; }
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this != &other) {
            if (value != kInvalidSocket) close_socket(value);
            value = other.value;
            other.value = kInvalidSocket;
        }
        return *this;
    }
};

struct ParsedUrl {
    bool tls = false;
    std::string host;
    std::string port;
    std::string target;
};

Error failure(std::string code, std::string message, int status = 0,
              bool retryable = false) {
    return Error{status, std::move(code), std::move(message), retryable};
}

Result<ParsedUrl> parse_url(std::string_view input) {
    const auto scheme_end = input.find("://");
    if (scheme_end == std::string_view::npos)
        return Result<ParsedUrl>::failure(failure("invalid_url", "URL must include http:// or https://"));
    const auto scheme = input.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https")
        return Result<ParsedUrl>::failure(failure("invalid_url", "only http and https are supported"));
    std::string_view authority_and_path = input.substr(scheme_end + 3);
    const auto slash = authority_and_path.find('/');
    const auto authority = authority_and_path.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return Result<ParsedUrl>::failure(failure("invalid_url", "URL authority is invalid"));

    ParsedUrl result;
    result.tls = scheme == "https";
    result.target = slash == std::string_view::npos ? "/" : std::string(authority_and_path.substr(slash));
    if (result.target.find('#') != std::string::npos)
        return Result<ParsedUrl>::failure(failure("invalid_url", "URL fragments are not sent to servers"));
    for (unsigned char character : result.target) {
        if (character <= 0x20 || character == 0x7f)
            return Result<ParsedUrl>::failure(failure("invalid_url", "URL target contains control or whitespace"));
    }

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
            return Result<ParsedUrl>::failure(failure("invalid_url", "invalid IPv6 host"));
        result.host = std::string(authority.substr(1, close - 1));
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return Result<ParsedUrl>::failure(failure("invalid_url", "invalid IPv6 port"));
            result.port = std::string(authority.substr(close + 2));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos && authority.find(':') == colon) {
            result.host = std::string(authority.substr(0, colon));
            result.port = std::string(authority.substr(colon + 1));
        } else {
            result.host = std::string(authority);
        }
    }
    if (result.host.empty() || result.host.find('\0') != std::string::npos)
        return Result<ParsedUrl>::failure(failure("invalid_url", "host is empty"));
    if (result.port.empty()) result.port = result.tls ? "443" : "80";
    unsigned port = 0;
    const auto [end, ec] = std::from_chars(result.port.data(), result.port.data() + result.port.size(), port);
    if (ec != std::errc{} || end != result.port.data() + result.port.size() || port == 0 || port > 65535)
        return Result<ParsedUrl>::failure(failure("invalid_url", "port is invalid"));
    return Result<ParsedUrl>::success(std::move(result));
}

bool valid_header_part(std::string_view value) {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return (c < 0x20 && c != '\t') || c == 0x7f;
    });
}

bool valid_header_name(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int timeout_value(int timeout_ms) {
    return std::max(1, timeout_ms);
}

Error socket_error(std::string code, std::string context, bool retryable = true) {
#ifdef _WIN32
    const int value = WSAGetLastError();
#else
    const int value = errno;
#endif
    return failure(std::move(code), std::move(context) + " (os error " + std::to_string(value) + ")", 0,
                   retryable);
}

Result<SocketGuard> connect_socket(const ParsedUrl& url, int timeout_ms) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return Result<SocketGuard>::failure(failure("socket_init", "WSAStartup failed"));
#endif
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses);
    if (lookup != 0)
        return Result<SocketGuard>::failure(failure("dns_failed", "host lookup failed"));

    Error last = failure("connect_failed", "no address accepted", 0, true);
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        Socket socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == kInvalidSocket) continue;
#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(socket, FIONBIO, &nonblocking);
#else
        const int flags = fcntl(socket, F_GETFL, 0);
        fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
        const int connected = ::connect(socket, address->ai_addr, static_cast<socklen_t>(address->ai_addrlen));
        bool ready = connected == 0;
        if (!ready) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket, &write_set);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#ifdef _WIN32
            const int selected = select(0, nullptr, &write_set, nullptr, &tv);
#else
            const int selected = select(socket + 1, nullptr, &write_set, nullptr, &tv);
#endif
            if (selected > 0 && FD_ISSET(socket, &write_set)) {
                int socket_error_value = 0;
                socklen_t length = sizeof(socket_error_value);
                getsockopt(socket, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                           reinterpret_cast<char*>(&socket_error_value),
#else
                           &socket_error_value,
#endif
                           &length);
                ready = socket_error_value == 0;
            }
        }
        if (!ready) {
            last = socket_error("connect_failed", "connection failed");
            close_socket(socket);
            continue;
        }
#ifdef _WIN32
        u_long blocking = 0;
        ioctlsocket(socket, FIONBIO, &blocking);
        const DWORD timeout = static_cast<DWORD>(timeout_value(timeout_ms));
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        fcntl(socket, F_SETFL, flags);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        freeaddrinfo(addresses);
        return Result<SocketGuard>::success(SocketGuard(socket));
    }
    freeaddrinfo(addresses);
    return Result<SocketGuard>::failure(std::move(last));
}

} // namespace

HttpTransport::HttpTransport(HttpOptions options) : options_(std::move(options)) {}
HttpTransport::~HttpTransport() = default;

Result<HttpResponse> HttpTransport::perform(const HttpRequest& request,
                                            CancellationToken* cancellation) const {
    if (request.method.empty() || request.method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
        return Result<HttpResponse>::failure(failure("invalid_request", "HTTP method is invalid"));
    if (request.body.size() > options_.limits.max_request_bytes)
        return Result<HttpResponse>::failure(failure("request_too_large", "request body exceeds limit"));
    for (const auto& [name, value] : request.headers) {
        if (!valid_header_name(name) || !valid_header_part(value) ||
            lower(name) == "host" || lower(name) == "content-length" ||
            lower(name) == "transfer-encoding" || lower(name) == "connection")
            return Result<HttpResponse>::failure(failure("invalid_request", "invalid or reserved HTTP header"));
    }
    if (cancellation && cancellation->cancelled())
        return Result<HttpResponse>::failure(failure("cancelled", "request cancelled"));
    auto parsed_result = parse_url(request.url);
    if (!parsed_result.ok()) return Result<HttpResponse>::failure(parsed_result.error());
    const ParsedUrl& url = parsed_result.value();
    if (url.tls && !options_.tls_verify)
        return Result<HttpResponse>::failure(failure("tls_verification_required", "TLS certificate verification cannot be disabled"));
    if (!url.tls && url.host != "localhost" && url.host != "127.0.0.1" && url.host != "::1")
        return Result<HttpResponse>::failure(failure("insecure_transport", "plain HTTP is limited to loopback development endpoints"));

    auto socket_result = connect_socket(url, timeout_value(options_.timeout_ms));
    if (!socket_result.ok()) return Result<HttpResponse>::failure(socket_result.error());
    SocketGuard socket = std::move(socket_result.value());

    SSL_CTX* context = nullptr;
    SSL* ssl = nullptr;
    if (url.tls) {
        context = SSL_CTX_new(TLS_client_method());
        if (!context)
            return Result<HttpResponse>::failure(failure("tls_init", "could not create TLS context"));
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        if (!options_.ca_file.empty() || !options_.ca_directory.empty()) {
            if (SSL_CTX_load_verify_locations(context,
                                              options_.ca_file.empty() ? nullptr : options_.ca_file.c_str(),
                                              options_.ca_directory.empty() ? nullptr : options_.ca_directory.c_str()) != 1) {
                SSL_CTX_free(context);
                return Result<HttpResponse>::failure(failure("tls_trust", "configured CA store could not be loaded"));
            }
        } else if (SSL_CTX_set_default_verify_paths(context) != 1) {
            SSL_CTX_free(context);
            return Result<HttpResponse>::failure(failure("tls_trust", "system CA store could not be loaded"));
        }
        ssl = SSL_new(context);
        if (!ssl || SSL_set_fd(ssl, static_cast<int>(socket.value)) != 1) {
            if (ssl) SSL_free(ssl);
            SSL_CTX_free(context);
            return Result<HttpResponse>::failure(failure("tls_init", "could not attach TLS socket"));
        }
        SSL_set_tlsext_host_name(ssl, url.host.c_str());
        X509_VERIFY_PARAM* verify = SSL_get0_param(ssl);
        in_addr ipv4{};
        in6_addr ipv6{};
        const bool is_ipv4 = inet_pton(AF_INET, url.host.c_str(), &ipv4) == 1;
        const bool is_ipv6 = inet_pton(AF_INET6, url.host.c_str(), &ipv6) == 1;
        if (is_ipv4 || is_ipv6) {
            if (X509_VERIFY_PARAM_set1_ip_asc(verify, url.host.c_str()) != 1) {
                SSL_free(ssl); SSL_CTX_free(context);
                return Result<HttpResponse>::failure(failure("tls_name", "could not configure TLS IP verification"));
            }
        } else if (X509_VERIFY_PARAM_set1_host(verify, url.host.c_str(), 0) != 1) {
            SSL_free(ssl); SSL_CTX_free(context);
            return Result<HttpResponse>::failure(failure("tls_name", "could not configure TLS host verification"));
        }
        if (SSL_connect(ssl) != 1 || SSL_get_verify_result(ssl) != X509_V_OK) {
            SSL_free(ssl); SSL_CTX_free(context);
            return Result<HttpResponse>::failure(failure("tls_handshake", "TLS handshake or certificate verification failed", 0, true));
        }
    }

    auto write_all = [&](std::string_view bytes) -> Result<bool> {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            if (cancellation && cancellation->cancelled())
                return Result<bool>::failure(failure("cancelled", "request cancelled"));
            int count = 0;
            if (ssl) count = SSL_write(ssl, bytes.data() + sent, static_cast<int>(std::min<std::size_t>(bytes.size() - sent, INT_MAX)));
            else count = static_cast<int>(::send(socket.value, bytes.data() + sent,
                                                 std::min<std::size_t>(bytes.size() - sent, INT_MAX), 0));
            if (count <= 0) return Result<bool>::failure(socket_error("write_failed", "HTTP request write failed"));
            sent += static_cast<std::size_t>(count);
        }
        return Result<bool>::success(true);
    };

    std::map<std::string, std::string> headers = request.headers;
    for (const auto& [name, value] : headers) {
        if (!valid_header_part(name) || !valid_header_part(value) || name.empty()) {
            if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context);
            return Result<HttpResponse>::failure(failure("invalid_request", "HTTP header contains control characters"));
        }
    }
    std::ostringstream outgoing;
    outgoing << request.method << " " << url.target << " HTTP/1.1\r\nHost: " << url.host;
    if ((url.tls && url.port != "443") || (!url.tls && url.port != "80")) outgoing << ":" << url.port;
    outgoing << "\r\nConnection: close\r\nContent-Length: " << request.body.size() << "\r\n";
    for (const auto& [name, value] : headers) outgoing << name << ": " << value << "\r\n";
    outgoing << "\r\n" << request.body;
    auto write_result = write_all(outgoing.str());
    if (!write_result.ok()) {
        if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context);
        return Result<HttpResponse>::failure(write_result.error());
    }

    auto read_some = [&](char* destination, std::size_t capacity) -> Result<std::size_t> {
        if (cancellation && cancellation->cancelled()) return Result<std::size_t>::failure(failure("cancelled", "request cancelled"));
        int count = ssl ? SSL_read(ssl, destination, static_cast<int>(std::min<std::size_t>(capacity, INT_MAX)))
                        : static_cast<int>(::recv(socket.value, destination, capacity, 0));
        if (count == 0) return Result<std::size_t>::success(0);
        if (count < 0) {
            if (cancellation && cancellation->cancelled())
                return Result<std::size_t>::failure(failure("cancelled", "request cancelled"));
            return Result<std::size_t>::failure(socket_error("read_failed", "HTTP response read failed"));
        }
        return Result<std::size_t>::success(static_cast<std::size_t>(count));
    };

    std::string buffer;
    std::array<char, 16 * 1024> scratch{};
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        auto chunk = read_some(scratch.data(), scratch.size());
        if (!chunk.ok()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(chunk.error()); }
        if (chunk.value() == 0) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("truncated_response", "server closed before headers")); }
        buffer.append(scratch.data(), chunk.value());
        const auto end = buffer.find("\r\n\r\n");
        if ((end == std::string::npos ? buffer.size() : end + 4) > options_.limits.max_header_bytes) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("headers_too_large", "HTTP response headers exceed limit")); }
    }
    const std::size_t header_end = buffer.find("\r\n\r\n");
    const std::string header_block = buffer.substr(0, header_end);
    std::size_t body_start = header_end + 4;
    const auto first_end = header_block.find("\r\n");
    if (header_block.rfind("HTTP/", 0) != 0)
        { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid HTTP status line")); }
    std::istringstream status_line(header_block.substr(0, first_end));
    std::string version, status_text; int status = 0; std::string reason;
    status_line >> version >> status_text;
    const auto [status_end, status_ec] = std::from_chars(status_text.data(), status_text.data() + status_text.size(), status);
    std::getline(status_line, reason);
    if (version != "HTTP/1.1" && version != "HTTP/1.0")
        { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "unsupported HTTP version")); }
    if (status_text.size() != 3 || status_ec != std::errc{} || status_end != status_text.data() + status_text.size() || status < 100 || status > 599)
        { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid HTTP status")); }
    HttpResponse response{status, reason, {}, {}};
    std::size_t line_start = first_end == std::string::npos ? header_block.size() : first_end + 2;
    while (line_start < header_block.size()) {
        const auto line_end = header_block.find("\r\n", line_start);
        const auto line = header_block.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid HTTP response header")); }
        const auto name = lower(line.substr(0, colon));
        if (!valid_header_name(name) || !valid_header_part(line.substr(colon + 1)))
            { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid HTTP response header")); }
        std::size_t value_start = colon + 1;
        while (value_start < line.size() && (line[value_start] == ' ' || line[value_start] == '\t')) ++value_start;
        if (response.headers.contains(name))
            { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "duplicate HTTP response header")); }
        response.headers.emplace(name, line.substr(value_start));
        if (line_end == std::string::npos) break;
        line_start = line_end + 2;
    }
    auto read_more = [&]() -> Result<bool> {
        auto chunk = read_some(scratch.data(), scratch.size());
        if (!chunk.ok()) return Result<bool>::failure(chunk.error());
        if (chunk.value() == 0) return Result<bool>::success(false);
        if (buffer.size() + chunk.value() > options_.limits.max_body_bytes + options_.limits.max_header_bytes)
            return Result<bool>::failure(failure("response_too_large", "HTTP response exceeds limit"));
        buffer.append(scratch.data(), chunk.value());
        return Result<bool>::success(true);
    };
    const auto content_length_it = response.headers.find("content-length");
    const auto transfer_it = response.headers.find("transfer-encoding");
    if (transfer_it != response.headers.end() && content_length_it != response.headers.end()) {
        if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context);
        return Result<HttpResponse>::failure(failure("invalid_response", "ambiguous HTTP body framing"));
    }
    if (transfer_it != response.headers.end() && lower(transfer_it->second) != "chunked") {
        if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context);
        return Result<HttpResponse>::failure(failure("invalid_response", "unsupported transfer encoding"));
    }
    if (transfer_it != response.headers.end() && lower(transfer_it->second) == "chunked") {
        std::size_t cursor = body_start;
        while (true) {
            auto line_end = buffer.find("\r\n", cursor);
            while (line_end == std::string::npos) {
                auto more = read_more(); if (!more.ok()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(more.error()); }
                if (!more.value()) break;
                line_end = buffer.find("\r\n", cursor);
            }
            if (line_end == std::string::npos) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("truncated_response", "truncated chunk size")); }
            const auto size_text = buffer.substr(cursor, line_end - cursor);
            std::size_t chunk_size = 0;
            const auto semi = size_text.find(';');
            const auto hex = size_text.substr(0, semi);
            const auto [hex_end, hex_ec] = std::from_chars(hex.data(), hex.data() + hex.size(), chunk_size, 16);
            if (hex_ec != std::errc{} || hex_end != hex.data() + hex.size()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid chunk size")); }
            cursor = line_end + 2;
            if (chunk_size == 0) {
                while (buffer.size() - cursor < 2) {
                    auto more = read_more();
                    if (!more.ok() || !more.value()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(more.ok() ? failure("truncated_response", "truncated final chunk") : more.error()); }
                }
                if (buffer.compare(cursor, 2, "\r\n") != 0) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "unsupported chunk trailers")); }
                break;
            }
            if (chunk_size > options_.limits.max_body_bytes - response.body.size()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "HTTP response body exceeds limit")); }
            if (chunk_size > std::numeric_limits<std::size_t>::max() - cursor - 2) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "chunk size exceeds bounds")); }
            while (buffer.size() < cursor + chunk_size + 2) { auto more = read_more(); if (!more.ok() || !more.value()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(more.ok() ? failure("truncated_response", "truncated chunk") : more.error()); } }
            if (response.body.size() + chunk_size > options_.limits.max_body_bytes) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "HTTP response body exceeds limit")); }
            response.body.append(buffer, cursor, chunk_size);
            cursor += chunk_size;
            if (buffer.compare(cursor, 2, "\r\n") != 0) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "chunk is not terminated")); }
            cursor += 2;
        }
    } else if (content_length_it != response.headers.end()) {
        std::size_t length = 0;
        const auto [end, ec] = std::from_chars(content_length_it->second.data(), content_length_it->second.data() + content_length_it->second.size(), length);
        if (ec != std::errc{} || end != content_length_it->second.data() + content_length_it->second.size()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("invalid_response", "invalid content length")); }
        if (length > options_.limits.max_body_bytes) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "HTTP response body exceeds limit")); }
        while (buffer.size() - body_start < length) { auto more = read_more(); if (!more.ok() || !more.value()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(more.ok() ? failure("truncated_response", "truncated HTTP body") : more.error()); } }
        response.body.assign(buffer, body_start, length);
    } else {
        response.body.assign(buffer, body_start, std::string::npos);
        if (response.body.size() > options_.limits.max_body_bytes) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "HTTP response body exceeds limit")); }
        while (true) { auto more = read_more(); if (!more.ok()) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(more.error()); } if (!more.value()) break; response.body.assign(buffer, body_start, std::string::npos); if (response.body.size() > options_.limits.max_body_bytes) { if (ssl) SSL_free(ssl); if (context) SSL_CTX_free(context); return Result<HttpResponse>::failure(failure("response_too_large", "HTTP response body exceeds limit")); } }
    }
    if (ssl) SSL_free(ssl);
    if (context) SSL_CTX_free(context);
    return Result<HttpResponse>::success(std::move(response));
}

std::string HttpTransport::redact(std::string_view text, std::string_view bearer) {
    std::string result(text);
    if (!bearer.empty()) {
        std::string::size_type pos = 0;
        while ((pos = result.find(bearer, pos)) != std::string::npos) {
            result.replace(pos, bearer.size(), "<redacted>");
            pos += 10;
        }
    }
    return result;
}

} // namespace bridge_native
