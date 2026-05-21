#ifndef DTS_VIEWER_MATERIALS_HPP
#define DTS_VIEWER_MATERIALS_HPP

// Spec 05 (textures): MaterialResolver
//
// Resolves a DTS material name (e.g. "base.larmor.BMP") to the raw bytes of a
// PBMP file living inside one of the registered VOL archives.
//
// Multi-VOL: Tribes PBMPs live across several archives — most meshes pull from
// `Entities.vol`, but per-world variants live in `<world>World.vol` /
// `<world>DML.vol`, and shell-UI atlases live in `Shell.vol`. Callers register
// each VOL they want searched via `add_vol`; resolution scans in registration
// order and returns the first hit.
//
// Name normalization (spec 01 + 04 findings):
//   - Strip a leading "base." segment ("base.larmor.BMP" -> "larmor.BMP")
//   - Lowercase the whole thing
//   - Empty input -> std::nullopt (no warning) — a valid "no material" sentinel
//     that DTS files use for flat-shaded buckets
//
// Lookup chain against the per-VOL filename index (rules from the spec):
//   1) exact lowercase match
//   2) name + ".bmp"
//   3) if name already ends in an extension, strip it and retry (1) and (2)
// (The spec lists case-insensitive separately, but since we lowercase both the
// query and the indexed filenames at registration time, case-insensitive IS
// the exact match.)
//
// Duplicates in the DTS material list are normal (larmor lists the same
// filename twice); the resolver returns the same bytes on each call.
// De-duplication for GL upload happens at the call site (spec 06).
//
// One warning per missing material name (across the whole resolver lifetime)
// — keeps the log readable for meshes whose missing materials list dozens of
// repeats.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <variant>

#include "resources/darkstar_volume.hpp"

namespace dts_viewer
{

class MaterialResolver
{
public:
    // Register a VOL to be searched on subsequent `resolve()` calls. Opens the
    // VOL once, indexes its filenames (lowercased) -> file_info, and keeps
    // the ifstream alive for cheap re-extraction. VOLs not openable as
    // darkstar archives are skipped with a warning.
    void add_vol(const std::filesystem::path& vol_path)
    {
        namespace fs = std::filesystem;
        namespace dv = studio::resources::vol::darkstar;
        namespace sr = studio::resources;

        auto in = std::make_unique<std::ifstream>(vol_path, std::ios::binary);
        if (!*in) {
            std::fprintf(stderr,
                "MaterialResolver: cannot open %s — skipping\n",
                vol_path.string().c_str());
            return;
        }

        auto plugin = std::make_unique<dv::vol_file_archive>();
        if (!plugin->stream_is_supported(*in)) {
            std::fprintf(stderr,
                "MaterialResolver: %s is not a darkstar VOL — skipping\n",
                vol_path.string().c_str());
            return;
        }
        in->clear(); in->seekg(0);

        VolHandle handle;
        handle.path = vol_path;
        handle.stream = std::move(in);
        handle.plugin = std::move(plugin);

        auto all = sr::get_all_content(vol_path, *handle.stream, *handle.plugin);
        for (auto& entry : all) {
            auto* f = std::get_if<sr::file_info>(&entry);
            if (!f) continue;
            std::string lower = f->filename.string();
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
            // Spec 06/07 — symmetric normalisation: queries strip a
            // leading `base.` prefix (see `normalize`), so VOL entries
            // must too. Without this, `base.emblem2.bmp` in
            // human1DML.vol is keyed as-is, the query `emblem2.bmp`
            // misses, and the material resolves to magenta.
            const std::string normalized = normalize(lower);
            // Insert both keys when they differ so DTS references that
            // happen to NOT carry the prefix still hit.
            if (normalized != lower) {
                handle.by_lower_name.emplace(normalized, *f);
            }
            handle.by_lower_name.emplace(std::move(lower), *f);
        }
        vols_.push_back(std::move(handle));
    }

    // Resolve a DTS-supplied material name to the PBMP file bytes. Returns
    // std::nullopt for empty input (silent) or when no VOL contains a match
    // (one warning per name).
    std::optional<std::vector<char>> resolve(const std::string& material_name) const
    {
        if (material_name.empty()) return std::nullopt;

        const std::string key = normalize(material_name);
        if (key.empty()) return std::nullopt;

        // Candidate keys in priority order. Use a small fixed buffer + dedup
        // via the lookup loop (don't bother with a set; max 4 entries).
        std::string candidates[4];
        std::size_t n_cand = 0;
        auto add = [&](std::string s) {
            for (std::size_t i = 0; i < n_cand; ++i)
                if (candidates[i] == s) return;
            candidates[n_cand++] = std::move(s);
        };

        add(key);
        // If key has no extension, append .bmp. If it has any other extension
        // we still try +".bmp" *after* the bare lookup in case the DTS stored
        // a stale .pbmp/.tga reference. Skip when already ends in .bmp.
        if (!ends_with(key, ".bmp")) add(key + ".bmp");
        // Strip last extension and retry the two forms above.
        auto dot = key.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            std::string stem = key.substr(0, dot);
            if (!stem.empty()) {
                add(stem);
                if (!ends_with(stem, ".bmp")) add(stem + ".bmp");
            }
        }

        for (const auto& vol : vols_) {
            for (std::size_t i = 0; i < n_cand; ++i) {
                auto it = vol.by_lower_name.find(candidates[i]);
                if (it == vol.by_lower_name.end()) continue;
                return extract(vol, it->second);
            }
        }

        // Miss — warn once, then stay quiet for the lifetime of the resolver.
        if (warned_misses_.insert(key).second) {
            std::fprintf(stderr,
                "MaterialResolver: no PBMP found for material '%s' "
                "(normalized '%s') across %zu VOL(s)\n",
                material_name.c_str(), key.c_str(), vols_.size());
        }
        return std::nullopt;
    }

private:
    struct VolHandle
    {
        std::filesystem::path path;
        // The plugin is stateless wrt the stream, but we keep both alive
        // together so extract_file_contents can be called repeatedly.
        std::unique_ptr<std::ifstream> stream;
        std::unique_ptr<studio::resources::vol::darkstar::vol_file_archive> plugin;
        std::unordered_map<std::string, studio::resources::file_info> by_lower_name;
    };

    static bool ends_with(const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size()
            && std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
    }

    // DTS material strings carry a leading `base.` segment (e.g.
    // "base.larmor.BMP" -> "larmor.bmp") and uppercase extensions. VOL
    // filenames are stored lowercased at registration; normalize the query to
    // match.
    static std::string normalize(const std::string& in)
    {
        std::string s = in;
        if (s.size() >= 5 && s.compare(0, 5, "base.") == 0) s.erase(0, 5);
        // Also accept upper-case "BASE." just in case.
        else if (s.size() >= 5
            && (s[0]=='B'||s[0]=='b') && (s[1]=='A'||s[1]=='a')
            && (s[2]=='S'||s[2]=='s') && (s[3]=='E'||s[3]=='e')
            && s[4]=='.') {
            s.erase(0, 5);
        }
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::vector<char> extract(const VolHandle& vol,
                              const studio::resources::file_info& info) const
    {
        std::stringstream buf;
        vol.stream->clear();
        vol.stream->seekg(0);
        vol.plugin->extract_file_contents(*vol.stream, info, buf);
        auto s = buf.str();
        return std::vector<char>(s.begin(), s.end());
    }

    std::vector<VolHandle> vols_;
    mutable std::unordered_set<std::string> warned_misses_;
};

} // namespace dts_viewer

#endif // DTS_VIEWER_MATERIALS_HPP
