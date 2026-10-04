#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bridge_native {

// Converts output bytes to UTF-8 for native text controls while retaining an
// incomplete code point between HTTP polls. Invalid bytes become U+FFFD; the
// underlying byte offset remains the server-provided offset.
class IncrementalUtf8Decoder {
public:
    std::string feed(std::string_view bytes);
    std::string finish();
    void reset();

private:
    std::string pending_;
};

} // namespace bridge_native

