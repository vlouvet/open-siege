// Track 26 spec 14c-I-2 — TAH per-class registry (clean-room).
// See tah_class_registry.hpp for the role enum and lookup signatures.

#include "tah_class_registry.hpp"

namespace net20 {

namespace {

// Sim range (1..17): enumerated indices 0..16 per spec §1.1 / §1.3
// (engine-core simulation primitives, deterministically allocated by
// the engine and stable across builds).
//
// Common range (128..134): enumerated indices 0..6 per spec §1.1 /
// §1.3 (common simulation extensions; also stable across builds).
//
// Game-specific range (256..1023): the high-traffic tags observed in
// `open-siege/wiki-contributions/TAH-CLASS-TAGS.md` are listed below
// with best-guess role assignments. Per spec §5.2 the tag-to-role
// correspondence in this range is build-dependent and these entries
// are PRELIMINARY — they encode the most-likely role per the
// disambiguation procedure in §6. Receivers that need exact mapping
// should run the runtime predicate disambiguation against the live
// stream and override this table.
//
// SPEC-AMBIGUITY: the exact role for the high-frequency tags 708 /
// 640 / 496 / 324 is "almost certainly a StaticShape variant" per
// spec §5.2 / §6, but the spec explicitly says the data-file-id
// field of each record identifies the concrete game-class. Pending
// runtime probe results we assign StaticShape (the simplest
// interpretation that survives the predicate check); a follow-up
// pass should narrow these to Turret / Sensor / etc. as evidence
// arrives.
// Size deduced — adjust by simply adding/removing rows below.
constexpr TahClassEntry kTahRegistry[] = {
    // ----- Sim range (1..17) -----
    { 1,   TahClassRole::Unknown,    "Volume" },          // resource volume
    { 2,   TahClassRole::Unknown,    "Terrain" },
    { 3,   TahClassRole::Unknown,    "Palette" },
    { 4,   TahClassRole::Unknown,    "Interior" },
    { 5,   TahClassRole::Sky,        "Sky" },
    { 6,   TahClassRole::Unknown,    "Light" },
    { 7,   TahClassRole::Sky,        "Planet" },
    { 8,   TahClassRole::Sky,        "StarField" },
    { 9,   TahClassRole::Explosion,  "Explosion" },
    { 10,  TahClassRole::Explosion,  "ExplosionCloud" },
    { 11,  TahClassRole::Explosion,  "Fire" },
    { 12,  TahClassRole::Explosion,  "Debris" },
    { 13,  TahClassRole::Explosion,  "DebrisCloud" },
    { 14,  TahClassRole::Unknown,    "InteriorGrouping" },
    { 15,  TahClassRole::Unknown,    "MovingInterior" },
    { 16,  TahClassRole::Unknown,    "InteriorShape" },
    { 17,  TahClassRole::Unknown,    "ShapeGroupRep" },

    // ----- Common range (128..134) -----
    { 128, TahClassRole::Projectile, "ESFProjectile" },
    { 129, TahClassRole::Marker,     "Marker" },
    { 130, TahClassRole::Marker,     "DropPoint" },       // marker-like
    { 131, TahClassRole::Sound,      "SoundSource" },
    { 132, TahClassRole::Trigger,    "Trigger" },
    { 133, TahClassRole::Unknown,    "Magnet" },
    { 134, TahClassRole::Unknown,    "Volumetric" },

    // ----- Game-specific range (256..1023): TAH-observed -----
    // The capture in `open-siege/wiki-contributions/TAH-CLASS-TAGS.md`
    // shows these tags appearing as scope-always introductions. The
    // role assignments here are best-guess per the spec §6 predicates;
    // see SPEC-AMBIGUITY note above.
    { 263, TahClassRole::Marker,      "Marker?" },         // 8 intros
    { 324, TahClassRole::StaticShape, "StaticShape?" },    // 18 intros
    { 333, TahClassRole::StaticShape, "StaticShape" },     // verified in
                                                           // 2026-05-22 capture
    { 376, TahClassRole::StaticShape, "StaticShape?" },    // 2 intros
    { 496, TahClassRole::StaticShape, "StaticShape?" },    // 29 intros
    { 512, TahClassRole::StaticShape, "StaticShape" },     // verified
    { 615, TahClassRole::StaticShape, "StaticShape" },     // verified
    { 640, TahClassRole::StaticShape, "StaticShape?" },    // 52 intros
    { 708, TahClassRole::StaticShape, "StaticShape?" },    // 58 intros — most-common
    { 896, TahClassRole::StaticShape, "StaticShape" },     // verified
    // The 960 entry was the Player tag observed in the 2026-05-22 PvP
    // capture (spec 28 discovery harness). It is build-specific to
    // the open-siege canned server but kept here for self-consistency.
    { 960, TahClassRole::Player,      "Player" },

    // 96 is in the Sim range so we'll cross-reference: it could be
    // Light (Sim index 5 = tag 6 — NOT 96). The 96 in TAH is
    // game-specific in spite of being below 128: it's in the
    // "sparse 1..127 hits" pool (spec §1.2). Per the spec §1.2 the
    // observed 1..127 tags correspond to engine simulation primitives —
    // sky, lights, interior shapes, explosions, debris clouds. We
    // leave it as StaticShape for parser-compat with the 2026-05-22
    // capture-derived defaults; the runtime probe should narrow.
    { 96,  TahClassRole::StaticShape, "StaticShape?(sim)" }, // 5 intros
    { 100, TahClassRole::StaticShape, "StaticShape?(sim)" }, // 5 intros
    { 109, TahClassRole::StaticShape, "StaticShape?(sim)" }, // 4 intros
    { 65,  TahClassRole::StaticShape, "StaticShape?(sim)" }, // 10 intros
    { 20,  TahClassRole::Unknown,     "Sim?" },              // sparse
    { 40,  TahClassRole::Unknown,     "Sim?" },
    { 53,  TahClassRole::Unknown,     "Sim?" },
    { 76,  TahClassRole::Unknown,     "Sim?" },
};

constexpr TahClassEntry kSentinel = { 0, TahClassRole::Unknown, "unknown" };

}  // namespace

TahClassRole role_for_tag(std::uint16_t class_tag) noexcept
{
    for (const auto& e : kTahRegistry) {
        if (e.class_tag == class_tag) return e.role;
    }
    return TahClassRole::Unknown;
}

TahClassEntry entry_for_tag(std::uint16_t class_tag) noexcept
{
    for (const auto& e : kTahRegistry) {
        if (e.class_tag == class_tag) return e;
    }
    return kSentinel;
}

const char* role_debug_name(TahClassRole role) noexcept
{
    switch (role) {
        case TahClassRole::Player:      return "player";
        case TahClassRole::StaticShape: return "static";
        case TahClassRole::Item:        return "item";
        case TahClassRole::Projectile:  return "projectile";
        case TahClassRole::Vehicle:     return "vehicle";
        case TahClassRole::Marker:      return "marker";
        case TahClassRole::Trigger:     return "trigger";
        case TahClassRole::Moveable:    return "moveable";
        case TahClassRole::Sky:         return "sky";
        case TahClassRole::Sound:       return "sound";
        case TahClassRole::Mine:        return "mine";
        case TahClassRole::Sensor:      return "sensor";
        case TahClassRole::Turret:      return "turret";
        case TahClassRole::Explosion:   return "explosion";
        case TahClassRole::Unknown:
        default:                        return "unknown";
    }
}

std::size_t tah_registry_entry_count() noexcept
{
    return sizeof(kTahRegistry) / sizeof(kTahRegistry[0]);
}

}  // namespace net20
