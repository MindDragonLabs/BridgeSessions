#include "bstransport/frame_io.hpp"
#include <openssl/err.h>
#include <stdexcept>
#include <string>
#include <cstring>

namespace bs::transport {

using namespace bs::protocol;

static void ssl_check(int ret, SSL* ssl, const char* op) {
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        throw std::runtime_error(std::string(op) + " failed: SSL error " + std::to_string(err) + " " + buf);
    }
}

Message read_frame(SSL* ssl) {
    // Read header
    uint8_t header[FRAME_HEADER_SIZE];
    size_t total = 0;
    while (total < FRAME_HEADER_SIZE) {
        size_t n = 0;
        int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE - total, &n);
        ssl_check(ret, ssl, "SSL_read header");
        total += n;
    }

    uint16_t length    = (static_cast<uint16_t>(header[4]) << 8) | header[5];

    if (length > MAX_FRAME_SIZE)
        throw std::runtime_error("frame payload exceeds MAX_FRAME_SIZE");

    // Read payload
    std::vector<uint8_t> raw(FRAME_HEADER_SIZE + length);
    std::memcpy(raw.data(), header, FRAME_HEADER_SIZE);

    if (length > 0) {
        total = 0;
        while (total < length) {
            size_t n = 0;
            int ret = SSL_read_ex(ssl, raw.data() + FRAME_HEADER_SIZE + total, length - total, &n);
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id) {
    auto frame = encode(msg, stream_id);

    size_t total = 0;
    while (total < frame.size()) {
        size_t n = 0;
        int ret = SSL_write_ex(ssl, frame.data() + total, frame.size() - total, &n);
        ssl_check(ret, ssl, "SSL_write");
        total += n;
    }
}

} // namespace bs::transport
