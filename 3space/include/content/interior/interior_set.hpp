#ifndef DARKSTAR_CONTENT_INTERIOR_SET_HPP
#define DARKSTAR_CONTENT_INTERIOR_SET_HPP

// Cross-VOL interior shape resolver.
//
// Tribes interiors are split across four file types living in two
// different archive flavours:
//
//   - DIS (manifest, `ITRs` v3): the entry point. Names the LODs, DIL
//     filenames per light-state and the shared DML.
//   - DIG (geometry, PERS `ITRGeometry` v7): one per LOD. Always ships
//     in the per-world DML VOL.
//   - DIL (lighting, PERS `ITRLighting` / `ITRMissionLighting` v7):
//     stock bake lives in the per-world DML VOL; the per-mission
//     instance override (when present) lives in the mission VOL.
//   - DML (material list, PERS `TS::MaterialList` v2..v4): shared by
//     every LOD of an interior; lives in the per-world DML VOL.
//
// A complete renderable bundle needs all four. The resolver registers
// VOLs in mount order (caller controls ordering) and resolves filename
// references using **reverse** registration order — last-added wins.
// In practice this means: mount the per-world DML VOL first, then the
// mission VOL, and mission-local overrides will naturally take priority
// over stock per-world content for the DIS and DIL lookups.
//
// DIG and DML are never overridden by mission VOLs in shipping content —
// the resolver still scans every VOL in reverse order, so if a hand-made
// mission ever did include a custom DML or DIG it would transparently
// override the stock one.
//
// Two clean-room references:
//   - `examples/dts-viewer/materials.hpp` for the multi-VOL handle +
//     filename index pattern.
//   - The DIS / DIG / DIL parsers next door for the file-level parses.
// No leaked Dynamix engine source was consulted.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "content/dis/dis.hpp"
#include "content/dig/dig.hpp"
#include "content/dts/darkstar_structures.hpp"
#include "content/interior/dil.hpp"
#include "resources/archive_plugin.hpp"
#include "resources/darkstar_volume.hpp"

namespace studio::content::interior
{
  // Bundle holding every parsed asset needed to render a single interior.
  // Populated by `interior_resolver::resolve()` on a successful lookup.
  struct interior_shape_set
  {
    // The DIS manifest (mission override if a mission VOL also carried
    // a same-named DIS; otherwise the stock per-world copy).
    studio::content::dis::dis_manifest dis;

    // Highest-LOD DIG (largest min_pixels). Always sourced from the
    // per-world DML VOL.
    studio::content::dig::dig_file dig;

    // Additional LODs in original DIS order (lower min_pixels first).
    // Optional — empty for v1 callers that only render the best LOD.
    std::vector<studio::content::dig::dig_file> additional_lods;

    // Resolved DIL — prefers a mission-VOL per-instance lighting when
    // available, falls back to the stock per-world ITRLighting.
    studio::content::interior::dil_file dil;

    // Shared material list referenced by every LOD's surfaces.
    studio::content::dts::darkstar::material_list_variant material;

    // When `dil.is_mission_lighting == true`, this holds the parent
    // stock ITRLighting that the mission overrides remap onto. Empty
    // otherwise. Mission lighting is a sparse delta — the parent is
    // still needed for surfaces whose lightmaps the mission did not
    // touch.
    std::optional<studio::content::interior::dil_file> stock_dil_for_fallback;
  };

  // Resolver over a list of VOL archives. Mount order matters — see
  // file-level comment. Not thread-safe.
  class interior_resolver
  {
  public:
    interior_resolver();
    ~interior_resolver();

    interior_resolver(const interior_resolver&) = delete;
    interior_resolver& operator=(const interior_resolver&) = delete;
    interior_resolver(interior_resolver&&) noexcept;
    interior_resolver& operator=(interior_resolver&&) noexcept;

    // Register a VOL for subsequent resolves. The VOL is opened once,
    // its filename listing is indexed by lowercased name, and the
    // ifstream is kept alive for cheap re-extraction. Non-VOL files
    // are skipped silently.
    void add_vol(const std::filesystem::path& vol_path);

    // Resolve a DIS filename (e.g. `catwalkA.0.dis`) into a complete
    // bundle. Returns std::nullopt when:
    //   - the DIS is not found in any mounted VOL,
    //   - the DIS parse fails,
    //   - any referenced DIG / DIL / DML is missing from every VOL,
    //   - any referenced asset parse fails.
    //
    // Filename matching is case-insensitive. Mission VOLs win over
    // world VOLs by virtue of mount order (reverse-iteration semantics).
    std::optional<interior_shape_set> resolve(std::string_view dis_name) const;

    // Diagnostics: number of registered VOLs (sum across both world
    // and mission archives).
    std::size_t vol_count() const noexcept;

    // Implementation-detail handle type, exposed only because the
    // resolver's free-function helpers in the cpp need to take it by
    // pointer. Treat as opaque.
    struct vol_handle;

  private:
    std::vector<std::unique_ptr<vol_handle>> vols_;
  };
}// namespace studio::content::interior

#endif// DARKSTAR_CONTENT_INTERIOR_SET_HPP
