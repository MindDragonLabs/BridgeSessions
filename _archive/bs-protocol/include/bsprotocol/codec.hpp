#pragma once

#include <span>
#include <vector>
#include <cstdint>
#include "message.hpp"

namespace bs::protocol {

// Encode a Message into a wire-format Frame, then serialize to bytes.
// Frames > COMPRESSION_THRESHOLD bytes are zstd-compressed (sets FLAG_COMPRESSED).
[[nodiscard]] std::vector<uint8_t> encode(const Message& msg, uint16_t stream_id = CONTROL_STREAM_ID);

// Decode raw bytes into a Message. Validates header, decompresses if needed.
// Throws std::runtime_error on malformed frames or zstd failures.
[[nodiscard]] Message decode(std::span<const uint8_t> raw);

// Get the MessageType for a given Message variant
[[nodiscard]] MessageType message_type(const Message& msg);

// Maximum size in bytes a message may occupy after encoding (before compression).
// Used for buffer pre-allocation. Most messages are <1KB; Output may be up to MAX_FRAME_SIZE.
[[nodiscard]] size_t max_encoded_size(const Message& msg);

// SHA-256 hex digest of arbitrary data. Used for clipboard dedup hashes.
// Thread-safe, uses OpenSSL EVP.
[[nodiscard]] std::string sha256_hex(std::string_view data);

} // namespace bs::protocol
