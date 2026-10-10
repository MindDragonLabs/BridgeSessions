#include "bridge_native/utf8.hpp"

#include <array>

namespace bridge_native {
namespace {

constexpr std::string_view replacement = "\xEF\xBF\xBD";

bool continuation(unsigned char value) { return (value & 0xC0) == 0x80; }

} // namespace

std::string IncrementalUtf8Decoder::feed(std::string_view bytes) {
    pending_.append(bytes);
    std::string output;
    output.reserve(pending_.size());
    std::size_t index = 0;
    while (index < pending_.size()) {
        const unsigned char first = static_cast<unsigned char>(pending_[index]);
        std::size_t width = 0;
        if (first <= 0x7F) width = 1;
        else if (first >= 0xC2 && first <= 0xDF) width = 2;
        else if (first >= 0xE0 && first <= 0xEF) width = 3;
        else if (first >= 0xF0 && first <= 0xF4) width = 4;
        else { output += replacement; ++index; continue; }
        if (pending_.size() - index < width) break;
        bool valid = true;
        for (std::size_t part = 1; part < width; ++part)
            valid = valid && continuation(static_cast<unsigned char>(pending_[index + part]));
        if (valid && width == 3) {
            const unsigned char second = static_cast<unsigned char>(pending_[index + 1]);
            valid = !(first == 0xE0 && second < 0xA0) && !(first == 0xED && second >= 0xA0);
        }
        if (valid && width == 4) {
            const unsigned char second = static_cast<unsigned char>(pending_[index + 1]);
            valid = !(first == 0xF0 && second < 0x90) && !(first == 0xF4 && second > 0x8F);
        }
        if (!valid) { output += replacement; ++index; continue; }
        output.append(pending_, index, width);
        index += width;
    }
    pending_.erase(0, index);
    return output;
}

std::string IncrementalUtf8Decoder::finish() {
    if (pending_.empty()) return {};
    pending_.clear();
    return std::string(replacement);
}

void IncrementalUtf8Decoder::reset() { pending_.clear(); }

} // namespace bridge_native

