// tests/test_image_encode.cpp — BMP→PNG transcode round-trip.
//
// Builds 24-bpp BMPs in memory (bottom-up and top-down), converts with
// bs::mesh::bmp_to_png, then independently decodes the PNG: magic, IHDR,
// chunk CRCs, zlib stored-block stream, Adler-32, and pixel BGR→RGB order.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-image-encode.h"

#include <cstdint>
#include <cstring>
#include <vector>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

using namespace bs::mesh;

namespace {

struct Bmp {
    std::vector<uint8_t> bytes;
    int w = 0, h = 0;
};

// rows[0] is the TOP image row; top_down selects DIB row order.
Bmp make_bmp(int w, int h, bool top_down,
             const std::vector<std::vector<std::uint8_t>>& rows) {
    const size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3);
    const size_t pixels = stride * static_cast<size_t>(h);
    std::vector<uint8_t> b(54 + pixels, 0);
    b[0] = 'B'; b[1] = 'M';
    const uint32_t total = static_cast<uint32_t>(54 + pixels);
    b[2] = total & 0xFF; b[3] = (total >> 8) & 0xFF;
    b[4] = (total >> 16) & 0xFF; b[5] = (total >> 24) & 0xFF;
    b[10] = 54; // bfOffBits
    auto le32 = [&](size_t off, uint32_t v) {
        b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF;
        b[off + 2] = (v >> 16) & 0xFF; b[off + 3] = (v >> 24) & 0xFF;
    };
    le32(14, 40);                 // biSize
    le32(18, static_cast<uint32_t>(w));
    le32(22, static_cast<uint32_t>(top_down ? -h : h));
    b[26] = 1;  b[27] = 0;        // biPlanes   (WORD)
    b[28] = 24; b[29] = 0;        // biBitCount (WORD)
    le32(34, static_cast<uint32_t>(pixels)); // biSizeImage
    for (int r = 0; r < h; ++r) {
        if (rows.empty()) break; // caller wants zeroed pixels
        const size_t dib_row = top_down ? static_cast<size_t>(r)
                                        : static_cast<size_t>(h - 1 - r);
        std::memcpy(b.data() + 54 + dib_row * stride, rows[static_cast<size_t>(r)].data(),
                    static_cast<size_t>(w) * 3);
    }
    return Bmp{b, w, h};
}

uint32_t be32(const std::vector<uint8_t>& v, size_t off) {
    return (uint32_t(v[off]) << 24) | (uint32_t(v[off + 1]) << 16) |
           (uint32_t(v[off + 2]) << 8) | uint32_t(v[off + 3]);
}

// Collect all chunks of one type.
std::vector<std::vector<uint8_t>> chunks(const std::vector<uint8_t>& png, const char type[5]) {
    std::vector<std::vector<uint8_t>> out;
    size_t p = 8;
    while (p + 8 <= png.size()) {
        const uint32_t len = be32(png, p);
        REQUIRE(p + 12 + len <= png.size());
        if (std::memcmp(png.data() + p + 4, type, 4) == 0) {
            out.emplace_back(png.begin() + static_cast<long>(p + 8),
                             png.begin() + static_cast<long>(p + 8 + len));
        }
        if (std::memcmp(png.data() + p + 4, "IEND", 4) == 0) break;
        p += 12 + len;
    }
    return out;
}

// Decode our stored-block zlib stream back to raw scanlines.
std::vector<uint8_t> inflate_stored(const std::vector<uint8_t>& idat) {
    REQUIRE(idat.size() >= 6);
    REQUIRE(idat[0] == 0x78);
    const uint32_t flg = idat[1];
    REQUIRE(((uint32_t(idat[0]) << 8) | flg) % 31 == 0);
    std::vector<uint8_t> raw;
    size_t p = 2;
    for (;;) {
        REQUIRE(p + 5 <= idat.size());
        const bool last = (idat[p] & 1) != 0;
        const uint32_t n = idat[p + 1] | (uint32_t(idat[p + 2]) << 8);
        const uint32_t ninv = idat[p + 3] | (uint32_t(idat[p + 4]) << 8);
        REQUIRE((n ^ 0xFFFFu) == ninv);
        REQUIRE(p + 5 + n <= idat.size());
        raw.insert(raw.end(), idat.begin() + static_cast<long>(p + 5),
                   idat.begin() + static_cast<long>(p + 5 + n));
        p += 5 + n;
        if (last) break;
    }
    // Adler-32 trailer over the reconstructed raw stream (exactly 4 bytes
    // follow the final stored block).
    REQUIRE(p + 4 == idat.size());
    const uint32_t adler = be32(idat, idat.size() - 4);
    REQUIRE(adler == png_adler32(raw.data(), raw.size()));
    return raw;
}

void verify_round_trip(const Bmp& src, bool top_down_used) {
    const std::vector<uint8_t> png = bmp_to_png(src.bytes);
    REQUIRE(!png.empty());

    // Magic.
    const uint8_t magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    REQUIRE(std::memcmp(png.data(), magic, 8) == 0);

    // IHDR: dims + RGB8.
    auto ihdr = chunks(png, "IHDR");
    REQUIRE(ihdr.size() == 1);
    REQUIRE(ihdr[0].size() == 13);
    REQUIRE(be32(ihdr[0], 0) == static_cast<uint32_t>(src.w));
    REQUIRE(be32(ihdr[0], 4) == static_cast<uint32_t>(src.h));
    REQUIRE(ihdr[0][8] == 8);
    REQUIRE(ihdr[0][9] == 2);
    REQUIRE(ihdr[0][12] == 0);

    // Exactly one IDAT, chunk CRCs valid.
    auto idats = chunks(png, "IDAT");
    REQUIRE(idats.size() == 1);
    size_t p = 8;
    while (p + 8 <= png.size()) {
        const uint32_t len = be32(png, p);
        std::vector<uint8_t> crc_in(png.begin() + static_cast<long>(p + 4),
                                    png.begin() + static_cast<long>(p + 8 + len));
        REQUIRE(be32(png, p + 8 + len) == png_crc32(crc_in.data(), crc_in.size()));
        if (std::memcmp(png.data() + p + 4, "IEND", 4) == 0) break;
        p += 12 + len;
    }

    // Pixels: filter byte 0 per row, BGR→RGB conversion.
    const std::vector<uint8_t> raw = inflate_stored(idats[0]);
    REQUIRE(raw.size() == static_cast<size_t>(src.h) * (1 + static_cast<size_t>(src.w) * 3));
    size_t q = 0;
    for (int r = 0; r < src.h; ++r) {
        REQUIRE(raw[q] == 0);
        ++q;
        const size_t dib_row = top_down_used ? static_cast<size_t>(r)
                                             : static_cast<size_t>(src.h - 1 - r);
        const uint8_t* brow = src.bytes.data() + 54 + dib_row *
            ((static_cast<size_t>(src.w) * 3 + 3) & ~size_t(3));
        for (int x = 0; x < src.w; ++x) {
            const uint8_t bb = brow[static_cast<size_t>(x) * 3 + 0];
            const uint8_t gg = brow[static_cast<size_t>(x) * 3 + 1];
            const uint8_t rr = brow[static_cast<size_t>(x) * 3 + 2];
            REQUIRE(raw[q + 0] == rr);
            REQUIRE(raw[q + 1] == gg);
            REQUIRE(raw[q + 2] == bb);
            q += 3;
        }
    }
}

TEST_CASE("bmp_to_png round-trips a bottom-up BMP", "[image]") {
    auto bmp = make_bmp(3, 2, false, {
        {0x00, 0x00, 0xFF,  0x00, 0xFF, 0x00,  0xFF, 0x00, 0x00},   // red green blue
        {0xFF, 0xFF, 0xFF,  0x80, 0x80, 0x80,  0x00, 0x00, 0x00},   // white gray black
    });
    verify_round_trip(bmp, false);
}

TEST_CASE("bmp_to_png round-trips a top-down BMP", "[image]") {
    auto bmp = make_bmp(5, 3, true, {
        {0x10, 0x20, 0x30,  0x40, 0x50, 0x60,  0x70, 0x80, 0x90,  0xA0, 0xB0, 0xC0,  0xD0, 0xE0, 0xF0},
        {0xFF, 0x00, 0x00,  0x00, 0xFF, 0x00,  0x00, 0x00, 0xFF,  0x11, 0x22, 0x33,  0x44, 0x55, 0x66},
        {0x01, 0x02, 0x03,  0x04, 0x05, 0x06,  0x07, 0x08, 0x09,  0x0A, 0x0B, 0x0C,  0x0D, 0x0E, 0x0F},
    });
    verify_round_trip(bmp, true);
}

TEST_CASE("bmp_to_png rejects malformed input", "[image]") {
    REQUIRE(bmp_to_png({}).empty());
    std::vector<uint8_t> not_bmp(100, 0x41);
    REQUIRE(bmp_to_png(not_bmp).empty());
    // Truncated pixel data: header claims 100x100 but only a few rows exist.
    auto big = make_bmp(100, 100, false, {});
    big.bytes.resize(54 + 4 * 30); // stride(100x3)=300... only 120 bytes of pixels
    REQUIRE(bmp_to_png(big.bytes).empty());
}

TEST_CASE("bmp_to_png handles widths needing stride padding", "[image]") {
    // width 2 → 6 bytes per row, padded to 8; padding must not leak into output.
    auto bmp = make_bmp(2, 2, false, {
        {0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF},
        {0x01, 0x02, 0x03,  0x04, 0x05, 0x06},
    });
    // Force junk into the pad bytes.
    const size_t stride = 8;
    bmp.bytes[54 + 6] = 0xDE; bmp.bytes[54 + 7] = 0xAD;
    bmp.bytes[54 + stride + 6] = 0xBE; bmp.bytes[54 + stride + 7] = 0xEF;
    verify_round_trip(bmp, false);
}
}
