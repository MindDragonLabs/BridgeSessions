#ifdef __linux__

#include "clipboard_bridge.hpp"

#include <bsprotocol/codec.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <utility>

namespace bs::client {
namespace {

enum class Backend {
    Xclip,
    Wayland,
};

struct ReadResult {
    bool success = false;
    std::string text;
};

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

bool is_wayland_session() {
    if (const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        wayland_display && *wayland_display) {
        return true;
    }
    if (const char* session_type = std::getenv("XDG_SESSION_TYPE");
        session_type && equals_ignore_case(session_type, "wayland")) {
        return true;
    }
    return false;
}

Backend preferred_backend() {
    return is_wayland_session() ? Backend::Wayland : Backend::Xclip;
}

Backend fallback_backend(Backend backend) {
    return backend == Backend::Wayland ? Backend::Xclip : Backend::Wayland;
}

const char* read_command(Backend backend) {
    switch (backend) {
        case Backend::Wayland: return "wl-paste -n 2>/dev/null";
        case Backend::Xclip:   return "xclip -selection clipboard -o 2>/dev/null";
    }
    return "xclip -selection clipboard -o 2>/dev/null";
}

const char* write_command(Backend backend) {
    switch (backend) {
        case Backend::Wayland: return "wl-copy 2>/dev/null";
        case Backend::Xclip:   return "xclip -selection clipboard 2>/dev/null";
    }
    return "xclip -selection clipboard 2>/dev/null";
}

bool exited_successfully(int status) {
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

ReadResult run_read_command(const char* command) {
    ReadResult result;
    FILE* pipe = ::popen(command, "r");
    if (!pipe) return result;

    char buf[4096];
    while (true) {
        const size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n > 0) result.text.append(buf, n);
        if (n < sizeof(buf)) break;
    }

    const int status = ::pclose(pipe);
    result.success = exited_successfully(status);
    return result;
}

bool run_write_command(const char* command, std::string_view text) {
    FILE* pipe = ::popen(command, "w");
    if (!pipe) return false;

    size_t written = 0;
    while (written < text.size()) {
        const size_t n = std::fwrite(text.data() + written, 1, text.size() - written, pipe);
        if (n == 0) break;
        written += n;
    }

    const int status = ::pclose(pipe);
    return written == text.size() && exited_successfully(status);
}

ReadResult read_clipboard_via(Backend backend) {
    return run_read_command(read_command(backend));
}

bool write_clipboard_via(Backend backend, std::string_view text) {
    return run_write_command(write_command(backend), text);
}

ReadResult read_clipboard() {
    const Backend primary = preferred_backend();
    const Backend secondary = fallback_backend(primary);

    ReadResult current = read_clipboard_via(primary);
    if (current.success || !current.text.empty()) return current;

    return read_clipboard_via(secondary);
}

bool write_clipboard(std::string_view text) {
    const Backend primary = preferred_backend();
    const Backend secondary = fallback_backend(primary);

    if (write_clipboard_via(primary, text)) return true;
    return write_clipboard_via(secondary, text);
}

} // namespace

ClipboardBridge::ClipboardBridge(ClipboardCallback on_copy)
    : on_copy_(std::move(on_copy))
{
    auto current = read_clipboard();
    if (current.success || !current.text.empty()) {
        last_seen_hash_ = bs::protocol::sha256_hex(current.text);
    }
}

ClipboardBridge::~ClipboardBridge() { stop(); }

bool ClipboardBridge::poll() {
    ReadResult current = read_clipboard();
    if (!current.success && current.text.empty()) return false;

    std::string hash = bs::protocol::sha256_hex(current.text);
    {
        std::lock_guard<std::mutex> lk(hash_mutex_);
        if (hash == last_seen_hash_) return false;
        last_seen_hash_ = hash;
        if (hash == last_acked_hash_) return false;
    }

    if (current.text.empty()) return false;
    on_copy_(std::move(current.text), std::move(hash));
    return true;
}

void ClipboardBridge::write_to_clipboard(std::string_view text) {
    if (!write_clipboard(text)) return;

    std::lock_guard<std::mutex> lk(hash_mutex_);
    last_seen_hash_ = bs::protocol::sha256_hex(text);
}

void ClipboardBridge::ack_hash(std::string_view hash) {
    std::lock_guard<std::mutex> lk(hash_mutex_);
    last_acked_hash_ = hash;
}

void ClipboardBridge::stop() {}

} // namespace bs::client

#endif // __linux__
