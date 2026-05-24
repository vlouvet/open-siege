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

#include <vector>

namespace net20 {

// Build the stock Tribes 1 CTF catalogue (~387 records). Group counts
// match §4 of TRIBES-DATABLOCKS.md.
std::vector<DatablockEntry> stock_tribes_ctf_catalogue();

}  // namespace net20
