// bs-image-encode.h — dependency-free BMP → PNG transcode (2026-09-14)
//
// The Windows CUA helper captures with GDI into a hand-rolled 24-bpp BMP
// (GDI+ Save() access-violates on recent Win11 builds; see bs-cua-helper.h
// case 6). The CLI advertises PNG as the default capture format, so before
// this header a `--output shot.png` held raw BMP bytes — the file name lied.
//
// This header converts such frames into real PNGs on the client side with
// no third-party codec: PNG permits zlib streams whose deflate blocks are
// all "stored" (BTYPE=00, uncompressed), so the encoder only needs Adler-32
// and CRC-32, both implemented below.
//
// Output is a valid RGB8 PNG (filter type 0 per scanline). Size stays close
// to the source BMP (stored blocks do not compress); callers that want
// small files should request JPEG (--format 2) on platforms that support
// it, or downscale on the peer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bs::mesh {

inline uint32_t png_crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

inline uint32_t png_adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

namespace png_detail {

inline void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

inline void put_be32_at(uint8_t* p, uint32_t x) {
    p[0] = static_cast<uint8_t>(x >> 24);
    p[1] = static_cast<uint8_t>(x >> 16);
    p[2] = static_cast<uint8_t>(x >> 8);
    p[3] = static_cast<uint8_t>(x);
}

inline uint32_t get_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline int32_t get_le32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

inline void chunk(std::vector<uint8_t>& out, const char type[4],
                  const uint8_t* data, size_t n) {
    put_be32(out, static_cast<uint32_t>(n));
    const uint8_t hdr[4] = {static_cast<uint8_t>(type[0]), static_cast<uint8_t>(type[1]),
                            static_cast<uint8_t>(type[2]), static_cast<uint8_t>(type[3])};
    out.insert(out.end(), hdr, hdr + 4);
    if (n) out.insert(out.end(), data, data + n);
    std::vector<uint8_t> crc_in(hdr, hdr + 4);
    if (n) crc_in.insert(crc_in.end(), data, data + n);
    put_be32(out, png_crc32(crc_in.data(), crc_in.size()));
}

} // namespace png_detail

// Convert a 24-bpp BMP (bottom-up or top-down) to PNG. Returns an empty
// vector on any parse failure; the caller falls back to saving raw bytes.
inline std::vector<uint8_t> bmp_to_png(const std::vector<uint8_t>& bmp) {
    using png_detail::get_le32;
    using png_detail::put_be32;
    using png_detail::put_be32_at;

    const size_t fh_size = 14; // BITMAPFILEHEADER
    const size_t ih_size = 40; // BITMAPINFOHEADER this encoder accepts
    if (bmp.size() < fh_size + ih_size) return {};
    if (bmp[0] != 'B' || bmp[1] != 'M') return {};
    const uint32_t off_bits = static_cast<uint32_t>(get_le32(bmp.data() + 10));
    if (get_le32(bmp.data() + 14) != static_cast<int32_t>(ih_size)) return {};
    // biPlanes / biBitCount are 16-bit WORDs at offsets 26/28.
    const uint16_t planes = static_cast<uint16_t>(bmp[26] | (bmp[27] << 8));
    const uint16_t bpp = static_cast<uint16_t>(bmp[28] | (bmp[29] << 8));
    if (planes != 1) return {};
    if (bpp != 24) return {};
    const int32_t w = get_le32(bmp.data() + 18);
    const int32_t h_raw = get_le32(bmp.data() + 22);
    const bool top_down = h_raw < 0;
    const int32_t h = top_down ? -h_raw : h_raw;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return {};

    const size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3);
    const size_t pixel_bytes = stride * static_cast<size_t>(h);
    if (static_cast<size_t>(off_bits) + pixel_bytes > bmp.size()) return {};

    // Raw PNG scanlines: filter byte 0 + RGB (BMP rows are BGR; bottom-up
    // BMPs store the last image row first).
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(w) * 3 + 1) * static_cast<size_t>(h));
    for (int32_t r = 0; r < h; ++r) {
        const size_t src_row = top_down ? static_cast<size_t>(r)
                                        : static_cast<size_t>(h - 1 - r);
        const uint8_t* s = bmp.data() + off_bits + src_row * stride;
        raw.push_back(0x00); // filter: None
        for (int32_t x = 0; x < w; ++x) {
            raw.push_back(s[2]); // R
            raw.push_back(s[1]); // G
            raw.push_back(s[0]); // B
            s += 3;
        }
    }

    // zlib stream with stored (uncompressed) deflate blocks: CMF=0x78,
    // FLG=0x01 (0x7801 % 31 == 0), then blocks of at most 65535 bytes.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = std::min<size_t>(raw.size() - pos, 65535);
        const bool last = (pos + n) == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.data() + pos, raw.data() + pos + n);
        pos += n;
    }
    put_be32(z, png_adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13] = {};
    put_be32_at(ihdr, static_cast<uint32_t>(w));
    put_be32_at(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // color type: truecolor RGB
    ihdr[10] = 0; // compression: deflate
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // interlace: none
    png_detail::chunk(png, "IHDR", ihdr, sizeof(ihdr));
    png_detail::chunk(png, "IDAT", z.data(), z.size());
    png_detail::chunk(png, "IEND", nullptr, 0);
    return png;
}

} // namespace bs::mesh
