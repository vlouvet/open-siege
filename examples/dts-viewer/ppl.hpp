#ifndef DTS_VIEWER_PPL_HPP
#define DTS_VIEWER_PPL_HPP

#include <array>
#include <cstdint>
#include <istream>
#include <map>
#include <vector>

#include "content/pal/palette.hpp"

// Thin wrapper around studio::content::pal::get_ppl_data() that re-shapes the
// lib3space palette records into plain-uint8 RGBA entries for the texture
// pipeline. lib3space returns one std::vector<colour> per palette plus the
// PL98 index/type discriminators; we copy each palette's colours into a
// fixed-size std::array<PaletteEntry, 256> so downstream code can rely on the
// 256-entry shape without re-checking sizes.
//
// Field naming mirrors lib3space (`colours` not `colors`, `flags` for the
// fourth colour byte) so call sites that use both layers stay consistent.

struct PaletteEntry
{
    std::uint8_t r     = 0;
    std::uint8_t g     = 0;
    std::uint8_t b     = 0;
    std::uint8_t flags = 0;
};

struct Palette
{
    std::uint32_t index = 0;             // PL98 palette ID (PBMP PiDX references this)
    std::uint32_t type  = 0;             // PL98 palette type discriminator (meaning TBD)
    std::array<PaletteEntry, 256> colours{}; // primary 256-entry RGBA palette
};

inline std::vector<Palette> load_ppl(std::istream& in)
{
    auto raw = studio::content::pal::get_ppl_data(in);
    std::vector<Palette> out;
    out.reserve(raw.size());

    for (const auto& src : raw) {
        Palette p;
        p.index = static_cast<std::uint32_t>(src.index);
        p.type  = static_cast<std::uint32_t>(src.type);

        // lib3space hands back exactly 256 colours per PL98 palette (the
        // fixed_palette on-disk struct is std::array<colour, 256>). Copy in
        // and zero-pad just in case a future parser change shortens the
        // vector — the std::array stays a stable shape for callers.
        const std::size_t n = src.colours.size() < p.colours.size()
                                  ? src.colours.size()
                                  : p.colours.size();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = src.colours[i];
            PaletteEntry& dst = p.colours[i];
            dst.r     = static_cast<std::uint8_t>(c.red);
            dst.g     = static_cast<std::uint8_t>(c.green);
            dst.b     = static_cast<std::uint8_t>(c.blue);
            dst.flags = static_cast<std::uint8_t>(c.flags);
        }

        out.push_back(std::move(p));
    }

    return out;
}

inline std::map<std::uint32_t, const Palette*> by_index(const std::vector<Palette>& palettes)
{
    std::map<std::uint32_t, const Palette*> out;
    for (const auto& p : palettes) {
        out.emplace(p.index, &p);
    }
    return out;
}

#endif // DTS_VIEWER_PPL_HPP
