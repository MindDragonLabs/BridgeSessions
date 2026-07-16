#ifndef BS_CLIENT_IMAGE_RENDER_HPP
#define BS_CLIENT_IMAGE_RENDER_HPP

#include <bsprotocol/message.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bs::client {

struct GifMetadata {
    uint32_t delay_ms = 0;
    uint32_t loop_count = 0;  // 0 = infinite / unknown
};

[[nodiscard]] std::vector<uint8_t> read_binary_file(const std::filesystem::path& path);

[[nodiscard]] std::optional<uint8_t> detect_image_format(std::span<const uint8_t> bytes);

[[nodiscard]] GifMetadata parse_gif_metadata(std::span<const uint8_t> bytes);

[[nodiscard]] bs::protocol::ImageDataMsg make_image_data_message(const std::filesystem::path& path);
[[nodiscard]] bs::protocol::ImageFrameMsg make_image_frame_message(const std::filesystem::path& path);

[[nodiscard]] bool render_image_message(const bs::protocol::ImageDataMsg& msg,
                                        int output_fd,
                                        bool quiet);
[[nodiscard]] bool render_image_message(const bs::protocol::ImageFrameMsg& msg,
                                        int output_fd,
                                        bool quiet);

} // namespace bs::client

#endif // BS_CLIENT_IMAGE_RENDER_HPP