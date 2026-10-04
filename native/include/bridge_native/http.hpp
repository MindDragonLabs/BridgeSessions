#pragma once

#include "bridge_native/result.hpp"

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bridge_native {

class CancellationToken {
public:
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

struct HttpLimits {
    std::size_t max_header_bytes = 32 * 1024;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_request_bytes = 8 * 1024 * 1024;
};

struct HttpOptions {
    int timeout_ms = 5000;
    bool tls_verify = true;
    std::string ca_file;
    std::string ca_directory;
    HttpLimits limits;
};

struct HttpRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpTransport {
public:
    explicit HttpTransport(HttpOptions options = {});
    ~HttpTransport();

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) noexcept = default;
    HttpTransport& operator=(HttpTransport&&) noexcept = default;

    Result<HttpResponse> perform(const HttpRequest& request,
                                 CancellationToken* cancellation = nullptr) const;

    static std::string redact(std::string_view text, std::string_view bearer);

private:
    HttpOptions options_;
};

} // namespace bridge_native
