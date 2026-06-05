#ifdef _WIN32

#include "clipboard_bridge.hpp"

#include <bsprotocol/codec.hpp>

#include <windows.h>
#include <string>
#include <string_view>
#include <mutex>

namespace bs::client {
namespace {

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
}

} // namespace

ClipboardBridge::ClipboardBridge(ClipboardCallback on_copy)
    : on_copy_(std::move(on_copy))
{
    // Seed with current clipboard hash
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            auto text = wide_to_utf8(static_cast<const wchar_t*>(GlobalLock(h)));
            GlobalUnlock(h);
            if (!text.empty()) {
                last_seen_hash_ = bs::protocol::sha256_hex(text);
            }
        }
        CloseClipboard();
    }
}

ClipboardBridge::~ClipboardBridge() = default;

bool ClipboardBridge::poll() {
    if (!OpenClipboard(nullptr)) return false;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return false; }

    auto* wstr = static_cast<const wchar_t*>(GlobalLock(h));
    if (!wstr) { CloseClipboard(); return false; }

    auto text = wide_to_utf8(wstr);
    GlobalUnlock(h);
    CloseClipboard();

    if (text.empty()) return false;

    std::string hash = bs::protocol::sha256_hex(text);
    {
        std::lock_guard<std::mutex> lk(hash_mutex_);
        if (hash == last_seen_hash_ || hash == last_acked_hash_) return false;
        last_seen_hash_ = hash;
    }

    on_copy_(std::move(text), std::move(hash));
    return true;
}

void ClipboardBridge::write_to_clipboard(std::string_view text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen > 0) {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen + 1) * sizeof(wchar_t));
        if (hMem) {
            auto* wstr = static_cast<wchar_t*>(GlobalLock(hMem));
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wstr, wlen);
            wstr[wlen] = L'\0';
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();

    std::lock_guard<std::mutex> lk(hash_mutex_);
    last_seen_hash_ = bs::protocol::sha256_hex(text);
}

void ClipboardBridge::ack_hash(std::string_view hash) {
    std::lock_guard<std::mutex> lk(hash_mutex_);
    last_acked_hash_ = hash;
}

void ClipboardBridge::stop() {}

} // namespace bs::client

#endif // _WIN32
