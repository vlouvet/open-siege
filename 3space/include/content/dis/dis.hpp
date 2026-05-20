#ifndef DARKSTAR_CONTENT_DIS_HPP
#define DARKSTAR_CONTENT_DIS_HPP

// Parser for Tribes 1 interior-shape MANIFEST files (`*.dis`).
//
// DIS is a flat, version-3 binary container marked with the ASCII
// magic `ITRs`. It is the only Tribes interior format that does not
// use the standard Darkstar `PERS` wrapper. The manifest holds:
//
//   - one or more LOD records, each pointing at a separate `.dig`
//     geometry file (by name-table offset),
//   - one DIL lightmap filename per light-state,
//   - a single shared material-list (`.dml`) filename,
//   - a default-state name (always `default` in shipping content),
//   - a trailing `linkedInterior` flag.
//
// Byte layout was reverse-engineered from hex inspection of all 517
// DIS files shipped in the Tribes 1.41 freeware corpus and verified
// against the community-clean reference in jamesu's Tribes Model
// Formats gist
// (https://gist.github.com/jamesu/9d25c16d5d11b402f9dc75d11df76177).
// No leaked Dynamix engine source was consulted.

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace studio::content::dis
{
  struct lod_record
  {
    // LOD switch threshold in screen-space pixels. The LOD with the
    // largest min_pixels still <= the on-screen projected size of the
    // interior is rendered. Observed range: 0..6000.
    std::uint32_t min_pixels = 0;

    // 1-indexed-by-LOD geometry filename for this detail level
    // (e.g. `catwalkA-00.dig`, `catwalkA-01.dig`, ...).
    std::string geometry_file;

    // Which lighting-state slot in the parent DIS this LOD uses. In
    // shipping content this is the index of the corresponding DIL
    // filename in lightmap_files (typically 0..numLods-1).
    std::uint32_t light_state_index = 0;

    // Universal `0xFC` constant in every shipping DIS. Preserved
    // verbatim — semantics are not pinned down (engine has an
    // internal `linkableFaces` concept that may map here).
    std::uint32_t linkable_faces_flags = 0;
  };

  struct dis_manifest
  {
    // Always 3 in shipping Tribes content.
    std::uint32_t version = 0;

    // Engine theoretically supports more than one state per interior,
    // but every shipping DIS reports exactly one.
    std::uint32_t num_states = 1;

    // The active state's display name. Always literal "default" in
    // shipping content.
    std::string default_state_name;

    // LOD records, ordered as found in the file. Lower min_pixels
    // values are coarser detail levels.
    std::vector<lod_record> lods;

    // One DIL filename per light state. lods[i].light_state_index
    // indexes into this vector. Same size as `lods` in every
    // observed sample.
    std::vector<std::string> lightmap_files;

    // The shared material list (DML) referenced by every LOD's
    // geometry. Always exactly one DML per interior.
    std::string material_list_file;

    // Trailer flag, observed values 0 or 1. Open question; likely a
    // portal-link or PVS-validity flag (see DIS-DIL.md research).
    std::uint8_t linked_interior = 0;

    // -------------------------------------------------------------
    // Convenience accessors mapping the spec API
    // (lod_dig_files / lod_distances / lightmap_file / ...).
    // -------------------------------------------------------------

    // Highest-detail-first ordering of the geometry filenames. The
    // file stores LODs in increasing min_pixels order in the
    // shipping corpus — flip if you need "best first".
    std::vector<std::string> lod_dig_files() const
    {
      std::vector<std::string> out;
      out.reserve(lods.size());
      for (auto const& lod : lods)
      {
        out.push_back(lod.geometry_file);
      }
      return out;
    }

    // Matching min_pixels thresholds, as f32 for renderer convenience.
    std::vector<float> lod_distances() const
    {
      std::vector<float> out;
      out.reserve(lods.size());
      for (auto const& lod : lods)
      {
        out.push_back(static_cast<float>(lod.min_pixels));
      }
      return out;
    }

    // Primary DIL filename (state 0). Empty when no lightmap is
    // declared (never seen in shipping content).
    std::string lightmap_file() const
    {
      return lightmap_files.empty() ? std::string{} : lightmap_files.front();
    }
  };

  // Returns true and leaves the stream position unchanged when the
  // next bytes are the `ITRs` magic. Returns false otherwise (also
  // restoring the original position).
  bool is_darkstar_dis(std::istream& stream);

  // Parse a DIS manifest from `src` starting at the current stream
  // position. Returns std::nullopt when:
  //   - the file does not begin with `ITRs`,
  //   - the version is not 3,
  //   - the stream fails before the trailer (truncated file),
  //   - any byte-offset in the manifest points outside the embedded
  //     name table.
  // On success, the stream is left positioned immediately after the
  // trailing `linkedInterior` byte.
  std::optional<dis_manifest> parse_dis(std::istream& src);
}

#endif
