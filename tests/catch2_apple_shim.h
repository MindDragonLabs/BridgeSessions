// catch2_apple_shim.h — test-pc5's libCatch2.a predates its 3.8 headers:
// the out-of-line StringMaker<std::string_view>::convert symbol is missing
// from the archive, so any TU comparing std::string_view in a REQUIRE fails
// to link. Provide the definition once per TU. Delete when the mac Catch2
// archive is rebuilt to match its headers.
#pragma once

#if defined(__APPLE__)
#include <catch2/catch_tostring.hpp>
#include <string>
#include <string_view>
namespace Catch {
inline std::string bs_apple_string_view_convert(std::string_view v) {
    return std::string(v);
}
// Out-of-line definition of the declared-but-missing static member.
// (Non-inline member definitions are fine here: exactly one test binary
// links this TU.)
std::string StringMaker<std::string_view>::convert(std::string_view v) {
    return bs_apple_string_view_convert(v);
}
} // namespace Catch
#endif
