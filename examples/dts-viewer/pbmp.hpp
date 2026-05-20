#ifndef DTS_VIEWER_PBMP_HPP
#define DTS_VIEWER_PBMP_HPP

#include <cstdint>
#include <istream>
#include <vector>

#include "content/bmp/bitmap.hpp"

// Thin wrapper around studio::content::bmp::get_pbmp_data() that exposes the
// PBMP head/PiDX/DETL fields and the primary mip level's indexed pixels in a
// shape convenient for the texture pipeline. Mipmap chains beyond level 0 are
// dropped here (lib3space already returns only the primary level in
// pbmp_data.pixels for the current parser).
struct PbmpImage
{
    std::uint32_t width        = 0;
    std::uint32_t height       = 0;
    std::uint32_t bit_depth    = 0;
    std::uint32_t palette_index = 0;   // from PiDX
    std::uint32_t mip_count    = 0;    // from DETL
    std::vector<std::uint8_t> indexed_pixels; // primary level only
};

inline PbmpImage load_pbmp(std::istream& in)
{
    auto raw = studio::content::bmp::get_pbmp_data(in);
    PbmpImage out;
    out.width         = static_cast<std::uint32_t>(static_cast<std::int32_t>(raw.bmp_header.width));
    out.height        = static_cast<std::uint32_t>(static_cast<std::int32_t>(raw.bmp_header.height));
    out.bit_depth     = static_cast<std::uint32_t>(raw.bmp_header.bit_depth);
    out.palette_index = static_cast<std::uint32_t>(raw.palette_index);
    out.mip_count     = static_cast<std::uint32_t>(raw.detail_levels);

    // For 8bpp the lib3space parser stores exactly width*height bytes for the
    // primary level; copy them as plain uint8_t for downstream GL upload.
    out.indexed_pixels.resize(raw.pixels.size());
    for (std::size_t i = 0; i < raw.pixels.size(); ++i) {
        out.indexed_pixels[i] = static_cast<std::uint8_t>(raw.pixels[i]);
    }
    return out;
}

#endif // DTS_VIEWER_PBMP_HPP
