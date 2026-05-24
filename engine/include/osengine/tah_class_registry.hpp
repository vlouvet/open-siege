// Track 26 spec 14c-I-2 — TAH per-class registry (clean-room).
//
// Maps the 10-bit per-introduction class_tag (range 1..1023) to a
// "role" the receiver should apply when decoding the per-class payload.
// The class-tag numbering scheme (per `docs/clean-room-specs/TRIBES-
// GHOST-CLASSES.md` §1) is partitioned into three sub-ranges:
//
//   1..127     engine-core simulation primitives (Sim)
//   128..255   common simulation extensions (Common)
//   256..1023  game-specific Tribes-1.41 classes (build-dependent)
//
// The Sim and Common ranges have stable per-index meanings across
// builds and are enumerated below. The game-specific range is
// build-dependent (per §1.3) and is populated here from the
// capture-grounded observations recorded in
// `open-siege/wiki-contributions/TAH-CLASS-TAGS.md`.
//
// Per spec §6, tag→role for unknown tags should be runtime-discovered
// via the per-class payload-shape predicates. This registry only
// publishes the entries we can ground in either:
//   - the Sim/Common range tables, or
//   - the TAH-CLASS-TAGS.md observations cross-referenced against
//     the per-class field widths in §3 of the spec.

#pragma once

#include <cstddef>
#include <cstdint>

namespace net20 {

// Per spec §2.1 (scope rules) the 14 wire-class kinds the protocol
// distinguishes. `Unknown` is the sentinel for tags outside the
// published map.
enum class TahClassRole : std::uint8_t {
    Unknown     = 0,
    Player,
    StaticShape,
    Item,
    Projectile,
    Vehicle,
    Marker,
    Trigger,
    Moveable,
    Sky,
    Sound,
    Mine,
    Sensor,
    Turret,
    Explosion,
};

// A single registry entry: the wire tag, its role, and a debug name
// for log lines. The debug name is descriptive only — receivers must
// use the role for dispatch, not the name.
struct TahClassEntry {
    std::uint16_t  class_tag    = 0;
    TahClassRole   role         = TahClassRole::Unknown;
    const char*    debug_name   = "unknown";
};

// Look up the role for a class tag. Returns TahClassRole::Unknown for
// tags not in the published map. The caller can then fall back to the
// runtime payload-shape disambiguation procedure in spec §6 if needed.
TahClassRole role_for_tag(std::uint16_t class_tag) noexcept;

// Look up the full entry for a class tag. Returns a sentinel entry
// (`class_tag = 0`, `role = Unknown`, `debug_name = "unknown"`) for
// tags not in the published map.
TahClassEntry entry_for_tag(std::uint16_t class_tag) noexcept;

// Convenience: stringify the role for log output.
const char* role_debug_name(TahClassRole role) noexcept;

// For tests/diagnostics: count of entries currently in the registry.
std::size_t tah_registry_entry_count() noexcept;

}  // namespace net20
