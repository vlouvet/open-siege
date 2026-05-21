// Cross-VOL interior shape resolver — see interior_set.hpp.
//
// Implementation notes:
//
// * A VOL is opened lazily on first `add_vol()` call; the ifstream is
//   retained as a member of the handle so subsequent `extract` calls
//   can rewind and re-read without paying for re-open.
//
// * Filename indexing lowercases at registration time. Resolution
//   lowercases at lookup time. Both sides share the same case folding
//   (ASCII-only — VOL filenames never carry non-ASCII bytes in any
//   shipping content).
//
// * Resolution order is **reverse** of registration. The mounting
//   convention recorded in interior_set.hpp tells callers: world VOL
//   first, mission VOL last. Reverse iteration then naturally prefers
//   mission overrides while still falling back to stock per-world
//   content (DIG, DML, stock DIL) when the mission VOL doesn't carry
//   that entry.

#include "content/interior/interior_set.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

#include "content/dts/darkstar.hpp"

namespace studio::content::interior
{
  namespace
  {
    std::string to_lower_copy(std::string_view s)
    {
      std::string out;
      out.reserve(s.size());
      for (char c : s)
      {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      return out;
    }
  }

  struct interior_resolver::vol_handle
  {
    std::filesystem::path path;
    std::unique_ptr<std::ifstream> stream;
    std::unique_ptr<studio::resources::vol::darkstar::vol_file_archive> plugin;
    std::unordered_map<std::string, studio::resources::file_info> by_lower_name;
  };

  interior_resolver::interior_resolver() = default;
  interior_resolver::~interior_resolver() = default;
  interior_resolver::interior_resolver(interior_resolver&&) noexcept = default;
  interior_resolver& interior_resolver::operator=(interior_resolver&&) noexcept = default;

  void interior_resolver::add_vol(const std::filesystem::path& vol_path)
  {
    namespace dv = studio::resources::vol::darkstar;
    namespace sr = studio::resources;

    auto in = std::make_unique<std::ifstream>(vol_path, std::ios::binary);
    if (!*in)
    {
      std::fprintf(stderr,
        "interior_resolver: cannot open %s — skipping\n",
        vol_path.string().c_str());
      return;
    }

    auto plugin = std::make_unique<dv::vol_file_archive>();
    if (!plugin->stream_is_supported(*in))
    {
      std::fprintf(stderr,
        "interior_resolver: %s is not a darkstar VOL — skipping\n",
        vol_path.string().c_str());
      return;
    }
    in->clear();
    in->seekg(0);

    auto handle = std::make_unique<vol_handle>();
    handle->path = vol_path;
    handle->stream = std::move(in);
    handle->plugin = std::move(plugin);

    auto all = sr::get_all_content(vol_path, *handle->stream, *handle->plugin);
    for (auto& entry : all)
    {
      auto* f = std::get_if<sr::file_info>(&entry);
      if (!f) continue;
      // file_info::filename is a std::filesystem::path. Index by the
      // bare leaf filename in lowercase — VOL records are flat and
      // matchers always work off the leaf only.
      std::string lower = to_lower_copy(f->filename.filename().string());
      // First insertion wins on duplicates within a single VOL (none
      // observed in any Tribes archive, but cheap to be defensive).
      handle->by_lower_name.emplace(std::move(lower), *f);
    }

    vols_.push_back(std::move(handle));
  }

  std::size_t interior_resolver::vol_count() const noexcept
  {
    return vols_.size();
  }

  namespace
  {
    // Extract a file's raw bytes from `vol` into a memory buffer.
    std::string extract_to_string(const interior_resolver::vol_handle& vol,
                                  const studio::resources::file_info& info)
    {
      std::stringstream buf;
      vol.stream->clear();
      vol.stream->seekg(0);
      vol.plugin->extract_file_contents(*vol.stream, info, buf);
      return buf.str();
    }

    struct vol_hit
    {
      const interior_resolver::vol_handle* vol;
      const studio::resources::file_info* info;
    };

    // Locate `name` (case-insensitive, leaf only) across the registered
    // VOLs. Iterates in reverse so the most-recently-added VOL wins —
    // gives mission-over-world override semantics for callers who mount
    // the world VOL first.
    template<typename Vols>
    std::optional<vol_hit> find_reverse(const Vols& vols, std::string_view name)
    {
      std::string key = to_lower_copy(name);
      for (auto it = vols.rbegin(); it != vols.rend(); ++it)
      {
        const auto& vol = **it;
        auto found = vol.by_lower_name.find(key);
        if (found != vol.by_lower_name.end())
        {
          return vol_hit{ &vol, &found->second };
        }
      }
      return std::nullopt;
    }

    // Same as find_reverse but only considers VOLs whose path leaf does
    // not look like a per-mission archive. The convention recorded in
    // the project: per-world DML VOLs end in `DML.vol` (any case);
    // mission VOLs live under a `missions/` directory and are named
    // after the mission. Since callers control mount order and naming
    // we can't fully classify; the lightweight heuristic here checks
    // for a `missions` directory anywhere in the VOL's path. World
    // VOLs always live directly in `tribes-game/base/` in shipping
    // installs.
    template<typename Vols>
    std::optional<vol_hit> find_in_world_vols(const Vols& vols, std::string_view name)
    {
      std::string key = to_lower_copy(name);
      for (auto it = vols.rbegin(); it != vols.rend(); ++it)
      {
        const auto& vol = **it;
        // Heuristic: skip VOLs whose path contains a `missions` segment.
        bool is_mission_vol = false;
        for (const auto& seg : vol.path)
        {
          std::string lower = to_lower_copy(seg.string());
          if (lower == "missions")
          {
            is_mission_vol = true;
            break;
          }
        }
        if (is_mission_vol) continue;
        auto found = vol.by_lower_name.find(key);
        if (found != vol.by_lower_name.end())
        {
          return vol_hit{ &vol, &found->second };
        }
      }
      return std::nullopt;
    }
  }

  std::optional<interior_shape_set> interior_resolver::resolve(std::string_view dis_name) const
  {
    if (dis_name.empty() || vols_.empty()) return std::nullopt;

    // ---- DIS: mission VOLs win, world VOLs fall back. ----------------
    auto dis_hit = find_reverse(vols_, dis_name);
    if (!dis_hit)
    {
      std::fprintf(stderr,
        "interior_resolver: DIS '%.*s' not found in any of %zu mounted VOL(s)\n",
        static_cast<int>(dis_name.size()), dis_name.data(), vols_.size());
      return std::nullopt;
    }

    auto dis_bytes = extract_to_string(*dis_hit->vol, *dis_hit->info);
    std::stringstream dis_stream(dis_bytes, std::ios::in | std::ios::out | std::ios::binary);
    auto parsed_dis = studio::content::dis::parse_dis(dis_stream);
    if (!parsed_dis)
    {
      std::fprintf(stderr,
        "interior_resolver: DIS '%.*s' failed to parse (source VOL: %s)\n",
        static_cast<int>(dis_name.size()), dis_name.data(),
        dis_hit->vol->path.string().c_str());
      return std::nullopt;
    }

    if (parsed_dis->lods.empty())
    {
      std::fprintf(stderr,
        "interior_resolver: DIS '%.*s' has no LOD records — skipping\n",
        static_cast<int>(dis_name.size()), dis_name.data());
      return std::nullopt;
    }

    // Pick the highest-resolution LOD (largest min_pixels). Track its
    // index so we can pull the corresponding DIL filename from the
    // light_state_index table.
    std::size_t best_lod_idx = 0;
    std::uint32_t best_pixels = parsed_dis->lods[0].min_pixels;
    for (std::size_t i = 1; i < parsed_dis->lods.size(); ++i)
    {
      if (parsed_dis->lods[i].min_pixels > best_pixels)
      {
        best_lod_idx = i;
        best_pixels = parsed_dis->lods[i].min_pixels;
      }
    }
    const auto& best_lod = parsed_dis->lods[best_lod_idx];

    interior_shape_set out;
    out.dis = std::move(*parsed_dis);

    // ---- DIG: only ever from a world VOL. ----------------------------
    auto dig_hit = find_in_world_vols(vols_, best_lod.geometry_file);
    if (!dig_hit)
    {
      // Fall back to any VOL — some hand-built test setups may not
      // segment world vs mission archives by path.
      dig_hit = find_reverse(vols_, best_lod.geometry_file);
    }
    if (!dig_hit)
    {
      std::fprintf(stderr,
        "interior_resolver: DIG '%s' (highest LOD of '%.*s') not found\n",
        best_lod.geometry_file.c_str(),
        static_cast<int>(dis_name.size()), dis_name.data());
      return std::nullopt;
    }
    {
      auto bytes = extract_to_string(*dig_hit->vol, *dig_hit->info);
      std::stringstream ss(bytes, std::ios::in | std::ios::out | std::ios::binary);
      auto parsed = studio::content::dig::read_dig_file(ss);
      if (!parsed)
      {
        std::fprintf(stderr,
          "interior_resolver: DIG '%s' failed to parse\n",
          best_lod.geometry_file.c_str());
        return std::nullopt;
      }
      out.dig = std::move(*parsed);
    }

    // ---- Additional LODs (best-effort, never fatal). -----------------
    for (std::size_t i = 0; i < out.dis.lods.size(); ++i)
    {
      if (i == best_lod_idx) continue;
      const auto& lod = out.dis.lods[i];
      auto hit = find_in_world_vols(vols_, lod.geometry_file);
      if (!hit) hit = find_reverse(vols_, lod.geometry_file);
      if (!hit) continue;
      auto bytes = extract_to_string(*hit->vol, *hit->info);
      std::stringstream ss(bytes, std::ios::in | std::ios::out | std::ios::binary);
      if (auto parsed = studio::content::dig::read_dig_file(ss))
      {
        out.additional_lods.push_back(std::move(*parsed));
      }
    }

    // ---- DIL: prefer mission VOL; fall back to world VOL. ------------
    if (best_lod.light_state_index >= out.dis.lightmap_files.size())
    {
      std::fprintf(stderr,
        "interior_resolver: best LOD references light state %u but DIS only declares %zu — skipping\n",
        best_lod.light_state_index, out.dis.lightmap_files.size());
      return std::nullopt;
    }
    const std::string& dil_name = out.dis.lightmap_files[best_lod.light_state_index];

    auto dil_hit = find_reverse(vols_, dil_name);
    if (!dil_hit)
    {
      std::fprintf(stderr,
        "interior_resolver: DIL '%s' (for '%.*s') not found\n",
        dil_name.c_str(),
        static_cast<int>(dis_name.size()), dis_name.data());
      return std::nullopt;
    }
    {
      auto bytes = extract_to_string(*dil_hit->vol, *dil_hit->info);
      std::stringstream ss(bytes, std::ios::in | std::ios::out | std::ios::binary);
      auto parsed = parse_dil(ss);
      if (!parsed)
      {
        std::fprintf(stderr,
          "interior_resolver: DIL '%s' failed to parse\n",
          dil_name.c_str());
        return std::nullopt;
      }
      out.dil = std::move(*parsed);
    }

    // If the resolved DIL is a mission-lighting override, try to also
    // pick up the stock per-world ITRLighting so the renderer can fall
    // back for surfaces the mission didn't remap. Best-effort.
    if (out.dil.is_mission_lighting)
    {
      auto stock_hit = find_in_world_vols(vols_, dil_name);
      if (stock_hit && stock_hit->vol != dil_hit->vol)
      {
        auto bytes = extract_to_string(*stock_hit->vol, *stock_hit->info);
        std::stringstream ss(bytes, std::ios::in | std::ios::out | std::ios::binary);
        if (auto parsed = parse_dil(ss))
        {
          out.stock_dil_for_fallback = std::move(*parsed);
        }
      }
    }

    // ---- DML: shared material list, always per-world. ----------------
    auto dml_hit = find_in_world_vols(vols_, out.dis.material_list_file);
    if (!dml_hit) dml_hit = find_reverse(vols_, out.dis.material_list_file);
    if (!dml_hit)
    {
      std::fprintf(stderr,
        "interior_resolver: DML '%s' (for '%.*s') not found\n",
        out.dis.material_list_file.c_str(),
        static_cast<int>(dis_name.size()), dis_name.data());
      return std::nullopt;
    }
    {
      auto bytes = extract_to_string(*dml_hit->vol, *dml_hit->info);
      std::stringstream ss(bytes, std::ios::in | std::ios::out | std::ios::binary);
      try
      {
        auto result = studio::content::dts::darkstar::read_shape(ss);
        if (auto* ml = std::get_if<studio::content::dts::darkstar::material_list_variant>(&result))
        {
          out.material = std::move(*ml);
        }
        else
        {
          std::fprintf(stderr,
            "interior_resolver: DML '%s' parsed as a shape, not a material list\n",
            out.dis.material_list_file.c_str());
          return std::nullopt;
        }
      }
      catch (const std::exception& e)
      {
        std::fprintf(stderr,
          "interior_resolver: DML '%s' failed to parse: %s\n",
          out.dis.material_list_file.c_str(), e.what());
        return std::nullopt;
      }
    }

    // ---- Validate the build_id chain (warn, don't fail). -------------
    if (out.dig.build_id != out.dil.geometry_build_id)
    {
      std::fprintf(stderr,
        "interior_resolver: build_id mismatch for '%.*s': DIG=%d DIL=%d "
        "(geometry may be out of sync with lighting)\n",
        static_cast<int>(dis_name.size()), dis_name.data(),
        out.dig.build_id, out.dil.geometry_build_id);
    }

    return out;
  }

}// namespace studio::content::interior
