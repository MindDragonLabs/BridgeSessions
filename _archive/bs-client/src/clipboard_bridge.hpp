#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace bs::client {

class ClipboardBridge {
public:
    using ClipboardCallback = std::function<void(std::string text, std::string hash)>;

    explicit ClipboardBridge(ClipboardCallback on_copy);
    ~ClipboardBridge();

    bool poll();
    void write_to_clipboard(std::string_view text);
    void ack_hash(std::string_view hash);
    void stop();

private:
    ClipboardCallback on_copy_;
    std::string last_acked_hash_;
    std::mutex hash_mutex_;

#if defined(__APPLE__)
    void* pb_ = nullptr;
    long last_change_count_ = 0;
#elif defined(__linux__) || defined(_WIN32)
    std::string last_seen_hash_;
#endif
};

} // namespace bs::client
