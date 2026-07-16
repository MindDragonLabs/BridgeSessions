#include "clipboard_bridge.hpp"

#ifdef __APPLE__

#import <AppKit/NSPasteboard.h>

#include <bsprotocol/codec.hpp>

#include <string>
#include <utility>

namespace bs::client {

ClipboardBridge::ClipboardBridge(ClipboardCallback on_copy)
    : on_copy_(std::move(on_copy))
{
    @autoreleasepool {
        pb_ = [NSPasteboard generalPasteboard];
        last_change_count_ = static_cast<long>([(NSPasteboard*)pb_ changeCount]);
    }
}

ClipboardBridge::~ClipboardBridge() { stop(); }

bool ClipboardBridge::poll() {
    @autoreleasepool {
        const auto* pb = static_cast<NSPasteboard*>(pb_);
        const long count = static_cast<long>([pb changeCount]);
        if (count == last_change_count_) return false;
        last_change_count_ = count;

        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (!str) return false;

        const char* utf8 = [str UTF8String];
        if (!utf8 || !*utf8) return false;

        std::string text(utf8);
        std::string hash = bs::protocol::sha256_hex(text);
        {
            std::lock_guard<std::mutex> lk(hash_mutex_);
            if (hash == last_acked_hash_) return false;
        }

        on_copy_(std::move(text), std::move(hash));
        return true;
    }
}

void ClipboardBridge::write_to_clipboard(std::string_view text) {
    @autoreleasepool {
        auto* pb = static_cast<NSPasteboard*>(pb_);
        [pb clearContents];
        NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                                length:text.size()
                                              encoding:NSUTF8StringEncoding];
        if (!ns) return;
        [pb setString:ns forType:NSPasteboardTypeString];
        last_change_count_ = static_cast<long>([pb changeCount]);
    }
}

void ClipboardBridge::ack_hash(std::string_view hash) {
    std::lock_guard<std::mutex> lk(hash_mutex_);
    last_acked_hash_ = hash;
}

void ClipboardBridge::stop() {}

} // namespace bs::client

#endif // __APPLE__
