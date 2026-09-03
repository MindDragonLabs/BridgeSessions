// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-ui.h — Clipboard bridge, image render, peer helpers
// Extracted from bs-mesh-controller.h (R6b structural refactor, 2026-09-03)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace or class — parent file provides it.
#pragma once


// 10. CLIPBOARD BRIDGE (Windows only)
// ────────────────────────────────────────────────────────────────────

#ifdef _WIN32

class ClipboardBridge {
    std::string last_seen_hash_;
    std::string last_acked_hash_;

    static std::string wide_to_utf8(const wchar_t* wstr) {
        if (!wstr) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string result(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
        return result;
    }

public:
    ClipboardBridge() {
        if (OpenClipboard(nullptr)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) {
                auto* wstr = static_cast<const wchar_t*>(GlobalLock(h));
                if (wstr) {
                    auto text = wide_to_utf8(wstr);
                    if (!text.empty()) {
                        last_seen_hash_ = sha256_hex(text);
                    }
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    }

    std::optional<std::string> poll() {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (!h) { CloseClipboard(); return std::nullopt; }
        auto* wstr = static_cast<const wchar_t*>(GlobalLock(h));
        if (!wstr) { CloseClipboard(); return std::nullopt; }
        auto text = wide_to_utf8(wstr);
        GlobalUnlock(h);
        CloseClipboard();
        if (text.empty()) return std::nullopt;
        std::string hash = sha256_hex(text);
        if (hash == last_seen_hash_ || hash == last_acked_hash_) return std::nullopt;
        last_seen_hash_ = hash;
        return text;
    }

    void write_to_clipboard(const std::string& text) {
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
        last_seen_hash_ = sha256_hex(text);
    }

    void ack_hash(const std::string& hash) { last_acked_hash_ = hash; }
};

#endif // _WIN32

// ────────────────────────────────────────────────────────────────────
// 11. IMAGE RENDER (cross-platform stubs)
// ────────────────────────────────────────────────────────────────────

namespace {

static constexpr size_t kMaxImageBytes = 50 * 1024 * 1024; // 50MB cap

[[nodiscard]] inline bool is_png_magic(std::span<const uint8_t> bytes) {
    static constexpr std::array<uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= kPngMagic.size() &&
           std::equal(kPngMagic.begin(), kPngMagic.end(), bytes.begin());
}

[[nodiscard]] inline bool is_jpeg_magic(std::span<const uint8_t> bytes) {
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

[[nodiscard]] inline bool is_gif_magic_alt(std::span<const uint8_t> bytes) {
    return bytes.size() >= 6 &&
           (std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

} // anonymous namespace

[[nodiscard]] inline std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.string() + ": " + ec.message());
    if (size > kMaxImageBytes)
        throw std::runtime_error("image exceeds 50MB cap: " + path.string());

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) throw std::runtime_error("cannot read " + path.string());
    }
    return data;
}

[[nodiscard]] inline std::optional<uint8_t> detect_image_format(std::span<const uint8_t> bytes) {
    if (is_png_magic(bytes)) return static_cast<uint8_t>(0);   // PNG
    if (is_jpeg_magic(bytes)) return static_cast<uint8_t>(1);  // JPEG
    return std::nullopt;
}

struct GifMetadata {
    uint32_t delay_ms = 0;
    uint16_t loop_count = 0; // 0 = infinite
};

[[nodiscard]] inline GifMetadata parse_gif_metadata(std::span<const uint8_t> bytes) {
    GifMetadata meta{};
    if (!is_gif_magic_alt(bytes)) return meta;

    for (size_t i = 0; i + 17 < bytes.size(); ++i) {
        if (bytes[i] == 0x21 && bytes[i + 1] == 0xF9 && bytes[i + 2] == 0x04) {
            uint16_t delay_cs = static_cast<uint16_t>(bytes[i + 4]) | (static_cast<uint16_t>(bytes[i + 5]) << 8);
            if (meta.delay_ms == 0) meta.delay_ms = static_cast<uint32_t>(delay_cs) * 10u;
        }
        if (bytes[i] == 0x21 && bytes[i + 1] == 0xFF && bytes[i + 2] == 0x0B) {
            const char* app = reinterpret_cast<const char*>(bytes.data() + i + 3);
            if (std::memcmp(app, "NETSCAPE2.0", 11) == 0 || std::memcmp(app, "ANIMEXTS1.0", 11) == 0) {
                if (bytes[i + 14] == 0x03 && bytes[i + 15] == 0x01) {
                    meta.loop_count = static_cast<uint16_t>(bytes[i + 16]) |
                                      (static_cast<uint16_t>(bytes[i + 17]) << 8);
                }
            }
        }
    }
    return meta;
}

[[nodiscard]] inline ImageDataMsg make_image_data_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    auto format = detect_image_format(bytes);
    if (!format)
        throw std::runtime_error("unsupported image format for " + path.string() + " (expected PNG or JPEG)");
    ImageDataMsg msg;
    msg.format = *format;
    msg.name = path.filename().string();
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] inline ImageFrameMsg make_image_frame_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    if (!is_gif_magic_alt(bytes))
        throw std::runtime_error("unsupported animation format for " + path.string() + " (expected GIF)");
    auto meta = parse_gif_metadata(bytes);
    ImageFrameMsg msg;
    msg.format = 2;
    msg.delay_ms = meta.delay_ms;
    msg.loop_count = meta.loop_count;
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] inline bool render_image_message(const ImageDataMsg& msg, int output_fd) {
#ifdef _WIN32
    // Windows: print text placeholder
    std::string note = "[Image: " + msg.name + "]\r\n";
    HANDLE hOut = (output_fd == 1) ? GetStdHandle(STD_OUTPUT_HANDLE)
                 : reinterpret_cast<HANDLE>(_get_osfhandle(output_fd));
    DWORD written;
    WriteFile(hOut, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    return true;
#else
    // POSIX: attempt chafa render via fork/exec
    const char* chafa_path = "/usr/bin/chafa";
    if (::access(chafa_path, X_OK) != 0) {
        std::string note = "[Image: " + msg.name + "]\n";
        const uint8_t* p = reinterpret_cast<const uint8_t*>(note.data());
        size_t remaining = note.size();
        while (remaining > 0) {
            ssize_t n = ::write(output_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
    // Write image data to temp file, fork chafa
    std::string tmpl = create_private_temp_file("img", "");
    if (tmpl.empty()) return false;
    int tmp_fd = ::open(tmpl.c_str(), O_WRONLY | O_TRUNC);
    if (tmp_fd < 0) { ::unlink(tmpl.c_str()); return false; }
    if (!msg.data.empty()) {
        const uint8_t* p = msg.data.data();
        size_t remaining = msg.data.size();
        while (remaining > 0) {
            ssize_t n = ::write(tmp_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; ::close(tmp_fd); ::unlink(tmpl.c_str()); return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
    }
    ::close(tmp_fd);

    pid_t pid = ::fork();
    if (pid < 0) { ::unlink(tmpl.c_str()); return false; }
    if (pid == 0) {
        if (output_fd != STDOUT_FILENO) ::dup2(output_fd, STDOUT_FILENO);
        ::execl(chafa_path, chafa_path, "--animate", "off", tmpl.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        ::unlink(tmpl.c_str());
        return false;
    }
    ::unlink(tmpl.c_str());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

[[nodiscard]] inline bool render_image_message(const ImageFrameMsg& msg, int output_fd) {
#ifdef _WIN32
    // Windows: print text placeholder (GIF frame)
    std::string note = "[ImageFrame: delay=" + std::to_string(msg.delay_ms) + "ms]\r\n";
    HANDLE hOut = (output_fd == 1) ? GetStdHandle(STD_OUTPUT_HANDLE)
                 : reinterpret_cast<HANDLE>(_get_osfhandle(output_fd));
    DWORD written;
    WriteFile(hOut, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    return true;
#else
    const char* chafa_path = "/usr/bin/chafa";
    if (::access(chafa_path, X_OK) != 0) {
        std::string note = "[ImageFrame: delay=" + std::to_string(msg.delay_ms) + "ms]\n";
        const uint8_t* p = reinterpret_cast<const uint8_t*>(note.data());
        size_t remaining = note.size();
        while (remaining > 0) {
            ssize_t n = ::write(output_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
    std::string tmpl = create_private_temp_file("frm", "");
    if (tmpl.empty()) return false;
    int tmp_fd = ::open(tmpl.c_str(), O_WRONLY | O_TRUNC);
    if (tmp_fd < 0) { ::unlink(tmpl.c_str()); return false; }
    if (!msg.data.empty()) {
        const uint8_t* p = msg.data.data();
        size_t remaining = msg.data.size();
        while (remaining > 0) {
            ssize_t n = ::write(tmp_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; ::close(tmp_fd); ::unlink(tmpl.c_str()); return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
    }
    ::close(tmp_fd);

    pid_t pid = ::fork();
    if (pid < 0) { ::unlink(tmpl.c_str()); return false; }
    if (pid == 0) {
        if (output_fd != STDOUT_FILENO) ::dup2(output_fd, STDOUT_FILENO);
        ::execl(chafa_path, chafa_path, tmpl.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        ::unlink(tmpl.c_str());
        return false;
    }
    ::unlink(tmpl.c_str());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// Top-level CLI wrapper for image/anim commands
inline void render_image_to_terminal(const std::string& path_str) {
    std::filesystem::path path(path_str);
    if (!std::filesystem::exists(path)) { std::cerr << "File not found: " << path_str << "\n"; return; }
    auto bytes = read_binary_file(path);
    if (is_gif_magic_alt(bytes)) {
        auto frame = make_image_frame_message(path);
        if (!render_image_message(frame, STDOUT_FILENO))
            std::cerr << "failed to render image frame\n";
    } else {
        auto img = make_image_data_message(path);
        if (!render_image_message(img, STDOUT_FILENO))
            std::cerr << "failed to render image\n";
    }
}

// ────────────────────────────────────────────────────────────────────
// 12. PEER HELPERS
// ────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::vector<PeerEntry> load_peers_from_file(const std::string& path) {
    MeshConfig cfg = load_config(path);
    std::vector<PeerEntry> peers;
    peers.reserve(cfg.seeds.size() + cfg.discovered.size());
    peers.insert(peers.end(), cfg.seeds.begin(), cfg.seeds.end());
    peers.insert(peers.end(), cfg.discovered.begin(), cfg.discovered.end());
    return peers;
}

[[nodiscard]] inline std::optional<PeerEntry> find_peer(const std::vector<PeerEntry>& peers,
                                                         const std::string& name) {
    for (const auto& p : peers) {
        if (p.name == name) return p;
    }
    return std::nullopt;
}
// (bs-session-worker.h is included earlier, right after create_session, so
// SessionRegistry and MeshController can use it.)

