#include "image_render.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bs::client {
namespace {

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> find_binary(std::string_view name) {
    auto can_exec = [](const std::filesystem::path& p) {
        return ::access(p.c_str(), X_OK) == 0;
    };

    if (name.find('/') != std::string_view::npos) {
        std::filesystem::path p{name};
        if (can_exec(p)) return p.string();
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return std::nullopt;

    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string_view part = end == std::string::npos
            ? std::string_view(path_str).substr(start)
            : std::string_view(path_str).substr(start, end - start);
        std::filesystem::path dir = part.empty() ? std::filesystem::path(".") : std::filesystem::path(part);
        std::filesystem::path candidate = dir / name;
        if (can_exec(candidate)) return candidate.string();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}
#endif // _WIN32

#ifdef _WIN32
[[nodiscard]] bool write_all(int fd, const uint8_t* data, size_t size) {
    HANDLE h = (fd == STDOUT_FILENO || fd == STDERR_FILENO) ? GetStdHandle(fd == STDOUT_FILENO ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE)
               : reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    DWORD written;
    return WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != FALSE && written == size;
}
#else
[[nodiscard]] bool write_all(int fd, const uint8_t* data, size_t size) {
    const uint8_t* p = data;
    while (size > 0) {
        ssize_t n = ::write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += static_cast<size_t>(n);
        size -= static_cast<size_t>(n);
    }
    return true;
}
#endif // _WIN32

[[nodiscard]] bool has_suffix_ci(std::string_view haystack, std::string_view suffix) {
    if (haystack.size() < suffix.size()) return false;
    auto start = haystack.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(haystack[start + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

[[nodiscard]] bool is_png_magic(std::span<const uint8_t> bytes) {
    static constexpr std::array<uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= kPngMagic.size() &&
           std::equal(kPngMagic.begin(), kPngMagic.end(), bytes.begin());
}

[[nodiscard]] bool is_jpeg_magic(std::span<const uint8_t> bytes) {
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

[[nodiscard]] bool is_gif_magic(std::span<const uint8_t> bytes) {
    return bytes.size() >= 6 &&
           std::memcmp(bytes.data(), "GIF87a", 6) == 0 &&
           true ||
           (bytes.size() >= 6 && std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

[[nodiscard]] bool is_gif_magic_alt(std::span<const uint8_t> bytes) {
    return bytes.size() >= 6 &&
           (std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

[[nodiscard]] bool chafa_render_file(const std::filesystem::path& file,
                                     int output_fd,
                                     bool animate,
                                     bool quiet)
{
#ifdef _WIN32
    // Windows: no chafa, emit text placeholder
    (void)animate;
    std::string note = "[Image: " + file.filename().string() + "]\r\n";
    DWORD written;
    HANDLE hOut = (output_fd == STDOUT_FILENO) ? GetStdHandle(STD_OUTPUT_HANDLE)
                 : reinterpret_cast<HANDLE>(_get_osfhandle(output_fd));
    WriteFile(hOut, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    return true;
#else
    auto chafa = find_binary("chafa");
    if (!chafa) {
        if (!quiet) {
            std::cerr << "chafa not found in PATH — cannot render " << file << std::endl;
        }
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        if (!quiet) std::cerr << "fork: " << strerror(errno) << std::endl;
        return false;
    }

    if (pid == 0) {
        if (output_fd != STDOUT_FILENO) {
            if (::dup2(output_fd, STDOUT_FILENO) < 0) _exit(127);
        }
        if (!animate) {
            ::execl(chafa->c_str(), chafa->c_str(), "--animate", "off", file.c_str(), static_cast<char*>(nullptr));
        } else {
            ::execl(chafa->c_str(), chafa->c_str(), file.c_str(), static_cast<char*>(nullptr));
        }
        _exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        if (!quiet) std::cerr << "waitpid: " << strerror(errno) << std::endl;
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

[[nodiscard]] std::string make_temp_image_path() {
#ifdef _WIN32
    char tmpl[MAX_PATH];
    char tmpPath[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "bsi", 0, tmpl);
    return tmpl;
#else
    char tmpl[] = "/tmp/bs-client-image-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        throw std::runtime_error(std::string("mkstemp: ") + std::strerror(errno));
    }
    ::close(fd);
    return tmpl;
#endif
}

[[nodiscard]] bool render_image_bytes(std::span<const uint8_t> bytes,
                                      int output_fd,
                                      bool animate,
                                      bool quiet)
{
    std::string temp_path;
    try {
        temp_path = make_temp_image_path();
    } catch (const std::exception& e) {
        if (!quiet) std::cerr << e.what() << std::endl;
        return false;
    }

    bool ok = false;
    do {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (!quiet) std::cerr << "open temp image failed: " << temp_path << std::endl;
            break;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                if (!quiet) std::cerr << "write temp image failed: " << temp_path << std::endl;
                break;
            }
        }
        out.close();
        ok = chafa_render_file(temp_path, output_fd, animate, quiet);
    } while (false);

    ::unlink(temp_path.c_str());
    return ok;
}

} // namespace

[[nodiscard]] std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("cannot stat " + path.string() + ": " + ec.message());
    }
    if (size > bs::protocol::MAX_IMAGE_BYTES) {
        throw std::runtime_error("image exceeds 50MB cap: " + path.string());
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) {
            throw std::runtime_error("cannot read " + path.string());
        }
    }
    return data;
}

[[nodiscard]] std::optional<uint8_t> detect_image_format(std::span<const uint8_t> bytes) {
    if (is_png_magic(bytes)) return static_cast<uint8_t>(0);
    if (is_jpeg_magic(bytes)) return static_cast<uint8_t>(1);
    return std::nullopt;
}

[[nodiscard]] GifMetadata parse_gif_metadata(std::span<const uint8_t> bytes) {
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

[[nodiscard]] bs::protocol::ImageDataMsg make_image_data_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    auto format = detect_image_format(bytes);
    if (!format) {
        throw std::runtime_error("unsupported image format for " + path.string() + " (expected PNG or JPEG)");
    }
    bs::protocol::ImageDataMsg msg;
    msg.format = *format;
    msg.name = path.filename().string();
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] bs::protocol::ImageFrameMsg make_image_frame_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    if (!is_gif_magic_alt(bytes)) {
        throw std::runtime_error("unsupported animation format for " + path.string() + " (expected GIF)");
    }
    auto meta = parse_gif_metadata(bytes);
    bs::protocol::ImageFrameMsg msg;
    msg.format = 2;
    msg.delay_ms = meta.delay_ms;
    msg.loop_count = meta.loop_count;
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] bool render_image_message(const bs::protocol::ImageDataMsg& msg,
                                        int output_fd,
                                        bool quiet)
{
    return render_image_bytes(msg.data.empty() ? std::span<const uint8_t>{}
                                                : std::span<const uint8_t>(msg.data.data(), msg.data.size()),
                              output_fd,
                              false,
                              quiet);
}

[[nodiscard]] bool render_image_message(const bs::protocol::ImageFrameMsg& msg,
                                        int output_fd,
                                        bool quiet)
{
    return render_image_bytes(msg.data.empty() ? std::span<const uint8_t>{}
                                                : std::span<const uint8_t>(msg.data.data(), msg.data.size()),
                              output_fd,
                              true,
                              quiet);
}

} // namespace bs::client