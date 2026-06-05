#pragma once

#include <bsprotocol/codec.hpp>
#include <bstransport/tls.hpp>

namespace bs::transport {

// Read a complete frame from an SSL connection.
// Blocks until a full frame header + payload arrives.
[[nodiscard]] bs::protocol::Message read_frame(SSL* ssl);

// Write a complete frame to an SSL connection.
// Encodes the message, compresses if needed, writes header + payload.
void write_frame(SSL* ssl, const bs::protocol::Message& msg, uint16_t stream_id = bs::protocol::CONTROL_STREAM_ID);

} // namespace bs::transport
