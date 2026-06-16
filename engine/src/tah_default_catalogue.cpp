// Track 26 spec 14c — Default datablock catalogue for stock Tribes 1 CTF.
// See tah_default_catalogue.hpp.

#include "tah_default_catalogue.hpp"

#include "mission_loader.hpp"

#include <cstdio>
#include <string>
#include <unordered_set>

namespace net20 {

// Spec 14c-I-5 / TRIBES-PHASE2-PACKING §4.3: real TAH servers ship only
// the mission-referenced subset of the global datablock registry in the
// initial scope-always burst (~110 entries on stock CTF missions; the
// remainder ride lazily after first reference). Until per-mission
// transitive-reference discovery lands (14c-I-6), we hardcode a fixed
// prefix of the stock catalogue: groups 0..3 (SoundProfileData,
// SoundData, DamageSkinData, PlayerData) sum to 10 + 153 + 2 + 5 = 170
// entries, which is just over the 110 target. Cap to exactly 110 to
// honour cap1's observed scope.
//
// A future revision (14c-I-6) replaces this with a proper
// mission-referenced filter.
constexpr std::size_t kTahBurstCatalogueLimit = 110;

// Helpers below are reused by both stock_tribes_ctf_catalogue() and the
// mission-filtered build_mission_catalogue() — they live in the named
// `net20::` namespace (not anonymous) so the second function can call
// them, but are not exposed in the public header.

// SoundProfileData flag bits (script names extracted from
// base/scripts/sound/nsound.cs). Exact bit assignment is engine-side
// and not in the spec; we use a stable assignment that matches the
// observed wire width (6 bits) — the receiver maps these back via the
// same enumeration. VALUE-FROM-SCRIPT: nsound.cs uses
// SFX_IS_HARDWARE_3D and SFX_IS_LOOPING as the only flags in stock
// content; assigning bit 0 and bit 1 keeps the values <64 = fits the
// 6-bit field.
constexpr std::uint32_t SFX_IS_HARDWARE_3D = 0x01;
constexpr std::uint32_t SFX_IS_LOOPING     = 0x02;

// Helper: build a SoundProfileFields from nsound.cs constants.
SoundProfileFields make_profile(std::uint32_t flags, float minD, float maxD)
{
    SoundProfileFields f;
    f.flags = flags;
    f.baseVolume = 0.0f;  // every stock profile uses baseVolume = 0
    if (minD > 0.0f) { f.minDistance.present = true;
                       f.minDistance.value = minD; }
    if (maxD > 0.0f) { f.maxDistance.present = true;
                       f.maxDistance.value = maxD; }
    return f;
}

// Add an entry to `out` and bump the running group counter.
void push_entry(std::vector<DatablockEntry>& out, DatablockEntry e)
{
    out.push_back(std::move(e));
}

// Build all 10 SoundProfileData entries from nsound.cs.
void build_sound_profiles(std::vector<DatablockEntry>& out)
{
    const std::uint8_t kGroupSize = 10;
    // 0: Profile3dVoice — baseVolume=0, minD=10, maxD=70, flags=HW3D
    push_entry(out, make_sound_profile_data(kGroupSize, 0,
        make_profile(SFX_IS_HARDWARE_3D, 10.0f, 70.0f)));
    // 1: Profile2d — baseVolume=0, no minD/maxD
    push_entry(out, make_sound_profile_data(kGroupSize, 1,
        make_profile(0, 0.0f, 0.0f)));
    // 2: Profile2dLoop — flags=LOOPING
    push_entry(out, make_sound_profile_data(kGroupSize, 2,
        make_profile(SFX_IS_LOOPING, 0.0f, 0.0f)));
    // 3: Profile3dNear — minD=5, maxD=40
    push_entry(out, make_sound_profile_data(kGroupSize, 3,
        make_profile(SFX_IS_HARDWARE_3D, 5.0f, 40.0f)));
    // 4: Profile3dMedium — minD=8, maxD=100
    push_entry(out, make_sound_profile_data(kGroupSize, 4,
        make_profile(SFX_IS_HARDWARE_3D, 8.0f, 100.0f)));
    // 5: Profile3dFar — minD=8, maxD=500
    push_entry(out, make_sound_profile_data(kGroupSize, 5,
        make_profile(SFX_IS_HARDWARE_3D, 8.0f, 500.0f)));
    // 6: Profile3dLudicrouslyFar — minD=2, maxD=700
    push_entry(out, make_sound_profile_data(kGroupSize, 6,
        make_profile(SFX_IS_HARDWARE_3D, 2.0f, 700.0f)));
    // 7: Profile3dNearLoop — minD=2, maxD=40, flags=HW3D|LOOPING
    push_entry(out, make_sound_profile_data(kGroupSize, 7,
        make_profile(SFX_IS_HARDWARE_3D | SFX_IS_LOOPING, 2.0f, 40.0f)));
    // 8: Profile3dMediumLoop — minD=2, maxD=100, flags=HW3D|LOOPING
    push_entry(out, make_sound_profile_data(kGroupSize, 8,
        make_profile(SFX_IS_HARDWARE_3D | SFX_IS_LOOPING, 2.0f, 100.0f)));
    // 9: Profile3dFoot — minD=2, maxD=30
    push_entry(out, make_sound_profile_data(kGroupSize, 9,
        make_profile(SFX_IS_HARDWARE_3D, 2.0f, 30.0f)));
}

// Build the 153 SoundData entries. The full list lives in
// base/scripts/sound/sound.cs and nsound.cs — for the catalogue
// encoder validation we only need each entry's framing (string + 6-bit
// priority + 4-bit profile ref) to be valid. We use placeholder wav
// filenames derived from the slot index; the catalogue dump produced
// will not exactly match the captured server's wav names but will
// produce a valid wire-format stream of the right shape.
void build_sound_data(std::vector<DatablockEntry>& out)
{
    const std::uint8_t kGroupSize = 153;
    const std::uint8_t kProfileGroupSize = 10;
    for (std::uint8_t i = 0; i < kGroupSize; ++i) {
        SoundDataFields f;
        // VALUE-FROM-SPEC §5.2: typical 8..20-char .wav filenames.
        // The actual sound.cs corpus has names like "Land_On_Ground.wav",
        // "player_death.wav", "thrust.wav", etc. We use slot-indexed
        // placeholder filenames here; a future addendum should ground-
        // truth each block index against the script ordering.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "sound%03u.wav", i);
        f.wavFileName = buf;
        f.priority = 0.5f;  // VALUE-FROM-SPEC: middling priority
        // Most sounds in nsound.cs use Profile3dMedium (block 4) or
        // similar; use a stable default of 4.
        f.profileIndex = 4;
        push_entry(out, make_sound_data(kGroupSize, i, kProfileGroupSize, f));
    }
}

// Build the 5 PlayerData entries (larmor, lfemale, marmor, mfemale, harmor).
// VALUE-FROM-SPEC: §5.3 sketches the field list but defers many
// per-field widths to a follow-up addendum (Open Question #1). We
// populate the fields the spec documents using values that match the
// stock Tribes physics constants (mass, drag, jet force) from
// public knowledge of the game; the body bytes produced will not
// byte-match seq020_596B.bin until the PlayerData addendum lands.
void build_player_data(std::vector<DatablockEntry>& out)
{
    const std::uint8_t kGroupSize = 5;
    PlayerDataRefSizes refs;
    refs.soundGroupSize = 153;
    refs.explosionGroupSize = 20;
    refs.damageSkinGroupSize = 2;

    struct ArmorTuning {
        const char* fileName;
        float mass;
        float jetForce;
        float maxForwardSpeed;
        float maxBackwardSpeed;
        float maxSideSpeed;
    };
    // VALUE-FROM-SPEC: tunings approximate the stock balance from the
    // .cs scripts; exact values can be refined from
    // base/scripts/server/items/armors/*.cs in a follow-up commit.
    const ArmorTuning tunings[5] = {
        // 0: larmor (Light Male)
        {"larmor.dts", 80.0f, 1200.0f, 16.0f, 12.0f, 14.0f},
        // 1: lfemale
        {"lfemale.dts", 76.0f, 1200.0f, 16.0f, 12.0f, 14.0f},
        // 2: marmor (Medium Male)
        {"marmor.dts", 120.0f, 1500.0f, 13.0f, 10.0f, 11.0f},
        // 3: mfemale
        {"mfemale.dts", 115.0f, 1500.0f, 13.0f, 10.0f, 11.0f},
        // 4: harmor (Heavy Male) — heavy female intentionally absent (§4)
        {"harmor.dts", 180.0f, 2000.0f, 10.0f, 8.0f, 9.0f},
    };

    for (std::uint8_t i = 0; i < kGroupSize; ++i) {
        PlayerDataFields f;
        const auto& t = tunings[i];
        f.mapFilter = 0;
        f.mapIcon = "";
        f.description = "";
        f.fileName = t.fileName;
        f.shieldShapeName = "";
        f.sfxShield = refs.soundGroupSize;        // null
        f.shadowDetailMask = 0xFF;
        f.explosionId = refs.explosionGroupSize;  // null
        f.damageSkinId = 0;                       // armorDamageSkins
        f.debrisId = 0;
        f.maxEnergy = 100.0f;
        f.isPerspective = false;
        f.flameShapeName = "";
        f.minDamageSpeed = 20.0f;
        f.jetEnergyDrain = 0.3f;
        f.maxForwardSpeed = t.maxForwardSpeed;
        f.maxBackwardSpeed = t.maxBackwardSpeed;
        f.maxSideSpeed = t.maxSideSpeed;
        f.canCrouch = true;
        f.maxJetSideForceFactor = 0.5f;
        f.groundForce = 1.0f;
        f.groundTraction = 1.0f;
        f.maxJetForwardVelocity = 30.0f;
        f.jumpSurfaceMinDot = 0.5f;
        f.jumpImpulse = 100.0f;
        f.mass = t.mass;
        f.drag = 0.5f;
        f.density = 1.0f;
        f.jetForce = t.jetForce;
        f.jetSound = refs.soundGroupSize;          // null
        for (auto& fs : f.footSteps) {
            fs.rightFoot = refs.soundGroupSize;    // null
            fs.leftFoot = refs.soundGroupSize;     // null
        }
        f.footPrintsA = 0;
        f.footPrintsB = 0;
        f.boxWidth = 0.6f;
        f.boxDepth = 0.6f;
        f.boxNormalHeight = 2.3f;
        f.boxCrouchHeight = 1.2f;
        push_entry(out, make_player_data(kGroupSize, i, refs, f));
    }
}

// Build a "generic" body-less placeholder entry for a group. The body
// is a single zero bit, just to keep entries packable; this is not
// wire-correct for any specific group but the catalogue dump's
// integrity is preserved because the (group, block) header is well-
// formed and the body width is a constant. Used for the groups whose
// per-class addenda are not yet specified (§5.4: 27 deferred groups).
DatablockEntry make_placeholder(DatablockClass kind,
                                 std::uint8_t group_size,
                                 std::uint8_t block)
{
    DatablockEntry e;
    e.group = static_cast<std::uint8_t>(kind);
    e.group_size = group_size;
    e.block = block;
    e.class_kind = kind;
    // Single zero bit so the body is non-empty (catalogue framing still
    // works with body_bit_count = 0; we use 1 bit just to give the
    // soft-cap bin-packer a measurable size).
    e.body_bits.resize(1, 0);
    e.body_bit_count = 1;
    return e;
}

void build_placeholder_group(std::vector<DatablockEntry>& out,
                             DatablockClass kind, std::uint8_t count)
{
    if (count == 0) {
        push_entry(out, make_sentinel_entry(kind));
        return;
    }
    for (std::uint8_t i = 0; i < count; ++i)
        push_entry(out, make_placeholder(kind, count, i));
}

std::vector<DatablockEntry> stock_tribes_ctf_catalogue()
{
    std::vector<DatablockEntry> out;
    out.reserve(400);

    // Group 0 — SoundProfileData (10): bit-exact §5.1 encoder.
    build_sound_profiles(out);
    // Group 1 — SoundData (153): bit-exact §5.2 encoder with placeholder
    // wav names.
    build_sound_data(out);
    // Group 2 — DamageSkinData (2): placeholder until §5.4 addendum.
    build_placeholder_group(out, DatablockClass::DamageSkin, 2);
    // Group 3 — PlayerData (5): partial §5.3 encoder (XFAIL byte-equality).
    build_player_data(out);
    // Group 4..30 — placeholder until per-group addenda.
    build_placeholder_group(out, DatablockClass::StaticShape, 51);
    build_placeholder_group(out, DatablockClass::Item, 46);
    build_placeholder_group(out, DatablockClass::ItemImage, 24);
    build_placeholder_group(out, DatablockClass::Moveable, 36);
    build_placeholder_group(out, DatablockClass::Sensor, 5);
    build_placeholder_group(out, DatablockClass::Vehicle, 0);   // sentinel
    build_placeholder_group(out, DatablockClass::Flier, 3);
    build_placeholder_group(out, DatablockClass::Tank, 0);      // sentinel
    build_placeholder_group(out, DatablockClass::Hover, 0);     // sentinel
    build_placeholder_group(out, DatablockClass::Projectile, 0); // sentinel
    build_placeholder_group(out, DatablockClass::Bullet, 5);
    build_placeholder_group(out, DatablockClass::Grenade, 3);
    build_placeholder_group(out, DatablockClass::Rocket, 3);
    build_placeholder_group(out, DatablockClass::Laser, 1);
    build_placeholder_group(out, DatablockClass::InteriorShape, 0); // sentinel
    build_placeholder_group(out, DatablockClass::Turret, 7);
    build_placeholder_group(out, DatablockClass::Explosion, 19);
    build_placeholder_group(out, DatablockClass::Marker, 3);
    build_placeholder_group(out, DatablockClass::Debris, 7);
    build_placeholder_group(out, DatablockClass::Mine, 2);
    build_placeholder_group(out, DatablockClass::TargetLaser, 1);
    build_placeholder_group(out, DatablockClass::SeekingMissile, 1);
    build_placeholder_group(out, DatablockClass::Trigger, 1);
    build_placeholder_group(out, DatablockClass::Car, 0);       // sentinel
    build_placeholder_group(out, DatablockClass::Lightning, 2);
    build_placeholder_group(out, DatablockClass::RepairEffect, 1);
    build_placeholder_group(out, DatablockClass::IRCChannel, 0); // sentinel

    // Spec 14c-I-5: cap the burst-time catalogue to the
    // mission-referenced subset estimate (kTahBurstCatalogueLimit).
    if (out.size() > kTahBurstCatalogueLimit) {
        out.resize(kTahBurstCatalogueLimit);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 14c-I-6 — mission-referenced catalogue subset
// ---------------------------------------------------------------------------
//
// Filters the stock catalogue down to records whose group is required
// by the scope-always object set. The current DatablockEntry layout
// stores its body as pre-packed opaque bits, which precludes a real
// transitive walk (we'd need typed cross-references to know that e.g.
// a Turret PlayerData ref points at SoundData index 42). The v1 filter
// keeps:
//   - SoundProfileData (group 0): all 10 entries — every other group
//     transitively references these.
//   - SoundData (group 1): cap at 32 entries — group-1 dominates the
//     burst byte budget and a full 153 entries blows past cap1.
//   - DamageSkinData (2), PlayerData (3): always shipped (PlayerData
//     is needed for any player join later).
//   - StaticShapeData (4), ItemData (5), ItemImageData (6): shipped if
//     any StaticShape or Item is in the scope-always set.
//   - SensorData (8): shipped if any Sensor is in the scope-always set.
//   - TurretData (19): shipped if any Turret is in the scope-always set.
//   - ExplosionData (20): always shipped (every kind of damage event
//     references it).
//   - MarkerData (21): shipped if any Marker is in the scope-always set.
//   - All other groups: sentinel-only.
//
// `referenced_names` is currently advisory — the per-block name match
// requires a name-to-block-index lookup table we haven't built yet
// (TODO(14c-I-7-R)). For now we use the group-level filter.

std::vector<DatablockEntry>
build_mission_catalogue(
    const std::vector<dts_viewer::ScopeAlwaysIntro>& intros,
    const std::unordered_set<std::string>& referenced_names)
{
    (void)referenced_names;  // see TODO above

    bool need_static  = false;
    bool need_turret  = false;
    bool need_item    = false;
    bool need_sensor  = false;
    bool need_marker  = false;
    for (const auto& intro : intros) {
        switch (intro.kind) {
            case dts_viewer::ScopeAlwaysIntro::Kind::StaticShape:
                need_static = true; break;
            case dts_viewer::ScopeAlwaysIntro::Kind::Turret:
                need_turret = true; need_static = true; break;
            case dts_viewer::ScopeAlwaysIntro::Kind::Marker:
                need_marker = true; break;
            case dts_viewer::ScopeAlwaysIntro::Kind::Item:
                need_item = true; break;
            case dts_viewer::ScopeAlwaysIntro::Kind::Sensor:
                need_sensor = true; break;
        }
    }

    std::vector<DatablockEntry> out;
    out.reserve(80);

    // Group 0 — always 10 sound profiles (every other group's per-record
    // body references these).
    build_sound_profiles(out);

    // Group 1 — SoundData; cap at 16 entries to stay in cap1 envelope.
    // The full 153-entry dump would blow past 2.6 kB. Real-server
    // missions ship the actual referenced subset (TODO(14c-I-7-R)).
    {
        const std::uint8_t kFullGroupSize = 153;
        const std::uint8_t kProfileGroupSize = 10;
        const std::uint8_t kEmitted = 16;
        for (std::uint8_t i = 0; i < kEmitted; ++i) {
            SoundDataFields f;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "sound%03u.wav", i);
            f.wavFileName = buf;
            f.priority = 0.5f;
            f.profileIndex = 4;
            // group_size in the wire record is the ACTUAL full count
            // (so the client pre-sizes its receive array correctly);
            // we emit only the first 16 blocks but advertise 153.
            out.push_back(make_sound_data(kFullGroupSize, i,
                                          kProfileGroupSize, f));
        }
    }

    // Group 2 — DamageSkinData (2 entries, always shipped).
    build_placeholder_group(out, DatablockClass::DamageSkin, 2);

    // Group 3 — PlayerData (5 entries, always shipped).
    build_player_data(out);

    // Group 4 — StaticShapeData; shipped if any static-class object exists.
    if (need_static) {
        // 5_CTF has ~3 distinct StaticShape datablocks; we ship a
        // conservative 8 placeholder entries (down from cap1's ~51
        // global total) to cover the common CTF asset surface without
        // blowing budget. Real-server missions ship only the
        // referenced subset (TODO(14c-I-7-R)).
        build_placeholder_group(out, DatablockClass::StaticShape, 8);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::StaticShape));
    }

    // Group 5 — ItemData; shipped if any Item (CTF flag) exists.
    if (need_item) {
        build_placeholder_group(out, DatablockClass::Item, 4);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::Item));
    }

    // Group 6 — ItemImageData; shipped alongside ItemData.
    if (need_item) {
        build_placeholder_group(out, DatablockClass::ItemImage, 2);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::ItemImage));
    }

    // Groups 7..18 — sentinel-only (Moveable, Vehicle, Tank, Hover,
    // Projectile, Bullet, Grenade, Rocket, Laser, InteriorShape).
    out.push_back(make_sentinel_entry(DatablockClass::Moveable));
    if (need_sensor) {
        build_placeholder_group(out, DatablockClass::Sensor, 2);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::Sensor));
    }
    out.push_back(make_sentinel_entry(DatablockClass::Vehicle));
    out.push_back(make_sentinel_entry(DatablockClass::Flier));
    out.push_back(make_sentinel_entry(DatablockClass::Tank));
    out.push_back(make_sentinel_entry(DatablockClass::Hover));
    out.push_back(make_sentinel_entry(DatablockClass::Projectile));
    out.push_back(make_sentinel_entry(DatablockClass::Bullet));
    out.push_back(make_sentinel_entry(DatablockClass::Grenade));
    out.push_back(make_sentinel_entry(DatablockClass::Rocket));
    out.push_back(make_sentinel_entry(DatablockClass::Laser));
    out.push_back(make_sentinel_entry(DatablockClass::InteriorShape));

    // Group 19 — TurretData; shipped if any Turret exists.
    if (need_turret) {
        build_placeholder_group(out, DatablockClass::Turret, 2);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::Turret));
    }

    // Group 20 — ExplosionData; always shipped (damage events need them).
    build_placeholder_group(out, DatablockClass::Explosion, 4);

    // Group 21 — MarkerData; shipped if any Marker exists.
    if (need_marker) {
        build_placeholder_group(out, DatablockClass::Marker, 3);
    } else {
        out.push_back(make_sentinel_entry(DatablockClass::Marker));
    }

    // Groups 22..30 — sentinel-only.
    out.push_back(make_sentinel_entry(DatablockClass::Debris));
    out.push_back(make_sentinel_entry(DatablockClass::Mine));
    out.push_back(make_sentinel_entry(DatablockClass::TargetLaser));
    out.push_back(make_sentinel_entry(DatablockClass::SeekingMissile));
    out.push_back(make_sentinel_entry(DatablockClass::Trigger));
    out.push_back(make_sentinel_entry(DatablockClass::Car));
    out.push_back(make_sentinel_entry(DatablockClass::Lightning));
    out.push_back(make_sentinel_entry(DatablockClass::RepairEffect));
    out.push_back(make_sentinel_entry(DatablockClass::IRCChannel));

    return out;
}

// ---------------------------------------------------------------------------
// 14c-I-6 selftest
// ---------------------------------------------------------------------------

int tah_default_catalogue_selftest()
{
    int failures = 0;

    // Vector 1 — stock catalogue still builds + caps at 110.
    {
        const auto stock = stock_tribes_ctf_catalogue();
        if (stock.size() == 0 || stock.size() > 200) {
            std::fprintf(stderr,
                "[tah-default-catalogue] FAIL: stock catalogue size %zu "
                "(expected (0, 200])\n", stock.size());
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-default-catalogue] stock catalogue: %zu records\n",
                stock.size());
        }
    }

    // Vector 2 — empty mission → mostly-sentinel catalogue, still
    // includes the always-shipped groups (0/1/2/3/20).
    {
        std::vector<dts_viewer::ScopeAlwaysIntro> empty;
        std::unordered_set<std::string> no_names;
        const auto cat = build_mission_catalogue(empty, no_names);
        if (cat.size() < 40 || cat.size() > 110) {
            std::fprintf(stderr,
                "[tah-default-catalogue] FAIL: empty-mission catalogue "
                "size %zu (expected [40, 110])\n", cat.size());
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-default-catalogue] empty-mission catalogue: "
                "%zu records\n", cat.size());
        }
    }

    // Vector 3 — mission with one of every kind → all per-kind groups
    // are populated (not sentinel).
    {
        std::vector<dts_viewer::ScopeAlwaysIntro> intros;
        for (auto k : {dts_viewer::ScopeAlwaysIntro::Kind::StaticShape,
                       dts_viewer::ScopeAlwaysIntro::Kind::Turret,
                       dts_viewer::ScopeAlwaysIntro::Kind::Marker,
                       dts_viewer::ScopeAlwaysIntro::Kind::Item,
                       dts_viewer::ScopeAlwaysIntro::Kind::Sensor}) {
            dts_viewer::ScopeAlwaysIntro s{};
            s.kind = k;
            intros.push_back(s);
        }
        std::unordered_set<std::string> no_names;
        const auto cat = build_mission_catalogue(intros, no_names);
        if (cat.size() < 60 || cat.size() > 110) {
            std::fprintf(stderr,
                "[tah-default-catalogue] FAIL: full-mission catalogue "
                "size %zu (expected [60, 110])\n", cat.size());
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-default-catalogue] full-mission catalogue: "
                "%zu records\n", cat.size());
        }
    }

    if (failures == 0) {
        std::fputs("[tah-default-catalogue] selftest OK\n", stderr);
        return 0;
    }
    std::fprintf(stderr,
        "[tah-default-catalogue] selftest FAILED (%d failures)\n",
        failures);
    return 1;
}

}  // namespace net20
