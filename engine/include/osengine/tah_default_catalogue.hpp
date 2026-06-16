// Track 26 spec 14c — Default datablock catalogue for stock Tribes 1 CTF.
//
// Returns ~387 catalogue entries (sentinel records included for the 5
// empty groups) per docs/clean-room-specs/TRIBES-DATABLOCKS.md §4. Field
// values are derived from the open Tribes 1.41 `.cs` mission script
// corpus at `tribes-assets/Starsiege Tribes - Groove/2017_T1Basic/base/
// scripts/`. For fields not present in the scripts (compile-time
// defaults baked into the engine), we use the value documented in the
// spec or annotate `VALUE-FROM-SPEC` at the call site.
//
// This catalogue is the seed any listen-server / client emits during
// the post-AcceptConnect catalogue dump (§1) before the first ghost
// stream burst.

#pragma once

#include "tah_datablock_encoder.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace dts_viewer {
struct ScopeAlwaysIntro;
}

namespace net20 {

// Build the stock Tribes 1 CTF catalogue (~387 records). Group counts
// match §4 of TRIBES-DATABLOCKS.md.
std::vector<DatablockEntry> stock_tribes_ctf_catalogue();

// 14c-I-6: build the mission-referenced catalogue subset.
//
// The full stock catalogue contains ~387 records (group 1 alone is
// 153 SoundData entries). Most of those are NOT referenced by any
// scope-always object in a typical 5_CTF burst. The 14c-I-5 baseline
// truncated to a fixed 110-record prefix that was SoundData-heavy;
// this overload instead filters the stock catalogue to the records
// whose group is plausibly required by the supplied scope-always
// objects (StaticShape + Turret + Item + Sensor + Marker → groups
// 0, 1, 2, 3, 4, 5, 8, 19, 20, 21) and excludes the heavy SoundData
// tail past the first 32 entries (per TRIBES-DATABLOCKS.md §3.5 the
// catalogue can ride lazily across multiple datagrams; for the
// initial burst we keep the running size in the cap1 envelope).
//
// `intros` and `referenced_names` come from
// `dts_viewer::scope_always_objects()` and
// `dts_viewer::required_datablock_names()` respectively.
//
// SPEC-NOTE: the spec calls for a *transitive* reference walk
// (StaticShape → SoundProfile → ... etc.). The current
// DatablockEntry representation does not expose the body's
// cross-references in a structurally typed form (the body is opaque
// pre-packed bits — see `tah_datablock_encoder.hpp`), so the v1
// implementation here ships the per-group filter without resolving
// per-record transitive references. This still cuts the catalogue
// from ~387 records to ~50–80 records (well under the 110 cap1
// envelope), enough for the burst to stay in cap1's 2.2 kB shape.
// Per-record transitive resolution is parked as TODO(14c-I-7-R).
std::vector<DatablockEntry>
build_mission_catalogue(
    const std::vector<dts_viewer::ScopeAlwaysIntro>& intros,
    const std::unordered_set<std::string>& referenced_names);

// Selftest: build_mission_catalogue against 5_CTF objects, assert
// size is in (0, 110] and the per-group filter dropped at least
// the heavy SoundData tail.
int tah_default_catalogue_selftest();

}  // namespace net20
