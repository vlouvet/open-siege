// Track 26 spec 14c — TAH datablock-catalogue event encoder (clean-room).
//
// Implements the post-AcceptConnect "catalogue dump" event-class encoder
// described in docs/clean-room-specs/TRIBES-DATABLOCKS.md. Each
// catalogue-entry event carries one (group, block) record describing a
// single datablock template (SoundProfileData, SoundData, PlayerData, ...)
// that the server pre-loaded for the mission. The events ride the
// reliable-event sub-stream (NETPROTO §7) but the per-event body is
// described by this spec.
//
// What this header provides:
//
//   * DatablockClass — enum of the 31 catalogue groups (§3.2).
//   * DatablockEntry — a single record: (group, block, body bits).
//   * make_*_data(...) helpers for the three fully-decoded groups in the
//     spec (SoundProfile §5.1, Sound §5.2, PlayerData §5.3 sketch).
//   * encode_catalogue_packet(...) — wrap a sequence of entries in the
//     event sub-stream framing for a single outbound UDP datagram.
//   * pack_catalogue_into_packets(...) — bin a full catalogue into packets
//     respecting the soft-cap rule (§3.5).
//
// All encoders use LSB-first bit ordering via net20::BitWriter (matches the
// rest of the reliable_acks / ghost_stream stack).
//
// Spec ambiguities (not blockers but flagged for follow-up):
//
//   - Block-id cross-reference encoding (§3.3 vs Open Question #2): two
//     conventions are wire-distinguishable. We follow §3.3: write
//     block_index as a fixed-width int of width = bits_to_represent(
//     group_size + 1), with `group_size` itself as the null sentinel.
//   - PlayerData has open gaps per §5.3 / §8.1. The make_player_data
//     helper covers what the spec documents and emits placeholder bits
//     where the spec sketch is incomplete; see TODO-PLAYERDATA-FIELD-N
//     comments in the implementation.
//   - Class tag value (88 in TAH captures) is per-build (§7); the caller
//     supplies it.

#pragma once

#include "reliable_acks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace net20 {

// The 31 catalogue groups (§3.2). Values are the wire-level group index
// (the 6-bit `group` field in the catalogue-entry event header).
enum class DatablockClass : std::uint8_t {
    SoundProfile     = 0,
    Sound            = 1,
    DamageSkin       = 2,
    PlayerData       = 3,
    StaticShape      = 4,
    Item             = 5,
    ItemImage        = 6,
    Moveable         = 7,
    Sensor           = 8,
    Vehicle          = 9,
    Flier            = 10,
    Tank             = 11,  // reserved/unused in T1
    Hover            = 12,  // reserved/unused in T1
    Projectile       = 13,
    Bullet           = 14,
    Grenade          = 15,
    Rocket           = 16,
    Laser            = 17,
    InteriorShape    = 18,  // reserved/unused in T1
    Turret           = 19,
    Explosion        = 20,
    Marker           = 21,
    Debris           = 22,
    Mine             = 23,
    TargetLaser      = 24,
    SeekingMissile   = 25,
    Trigger          = 26,
    Car              = 27,  // reserved/unused in T1
    Lightning        = 28,
    RepairEffect     = 29,
    IRCChannel       = 30,
};

constexpr std::uint8_t kCatalogueGroupCount = 31;

// One catalogue-entry record. `body_bits` is the raw bit-payload that
// follows the (group, group_size, block) header — already packed
// LSB-first into bytes, with `body_bit_count` valid bits starting at bit
// 0 of `body_bits[0]` (any trailing high bits in the last byte are
// padding and ignored).
struct DatablockEntry {
    std::uint8_t group;            // 0..30, see DatablockClass
    std::uint8_t group_size;       // total blocks in this group (0..255)
    std::uint8_t block;            // 0..group_size-1, OR 255 = sentinel
    DatablockClass class_kind;     // diagnostic; == group for type safety
    std::vector<std::uint8_t> body_bits;  // raw LSB-first packed body
    std::size_t body_bit_count = 0;       // valid bits in body_bits

    bool is_sentinel() const noexcept { return block == 255; }
};

// Helpers — append a single entry body to a BitWriter using the spec's
// per-group encoding rules. Each `make_*_body` writes ONLY the body bits
// (no group/block header) into a fresh BitWriter and returns the result
// wrapped in a DatablockEntry (group/block left for the caller to fill).

// §5.1 SoundProfileData body. All "differs_from_default" flags default
// to false; pass an OptionalF<float> with `present=true` to override.
template <typename T>
struct Optional {
    bool present = false;
    T value{};
};

struct SoundProfileFields {
    // Fixed-presence fields:
    std::uint32_t flags = 0;             // 6-bit playback flags
    float baseVolume = 0.0f;             // [0..1] gain → 10-bit quantized

    // Optional fields (present iff `.present == true`):
    Optional<float> coneInsideAngle;     // degrees, packed as angle/360 in 10 bits
    Optional<float> coneOutsideAngle;    // degrees, packed as angle/360 in 10 bits
    struct Vec3 { float x = 0, y = 0, z = 0; };
    Optional<Vec3> coneVector;           // Form-A normal vector, 10-bit, 21 bits
    Optional<float> coneOutsideVolume;   // raw 32-bit float
    Optional<float> minDistance;         // raw 32-bit float
    Optional<float> maxDistance;         // raw 32-bit float
};
DatablockEntry make_sound_profile_data(std::uint8_t group_size,
                                       std::uint8_t block,
                                       const SoundProfileFields& f);

// §5.2 SoundData body. profile_group_size is needed to compute the
// 4-bit (or N-bit) width of the SoundProfileData cross-reference. Pass
// the catalogue-wide group_size for group 0 (typically 10 for a stock
// TAH/T1 CTF mission).
struct SoundDataFields {
    std::string wavFileName;      // §6.7 v3 string (we always use uncompressed)
    float priority = 0.0f;        // [0..1] → 6-bit quantized
    std::uint16_t profileIndex;   // block index into group 0, or
                                  // sound_profile_group_size for null
};
DatablockEntry make_sound_data(std::uint8_t group_size,
                               std::uint8_t block,
                               std::uint8_t sound_profile_group_size,
                               const SoundDataFields& f);

// §5.3 PlayerData body. The spec only sketches the GameBase + ShapeBase
// + Player parent-class chain; many sub-field widths are open per
// Open Question #1. This helper packs the documented fields and emits
// `body_bit_count` reflecting only what was written — the catalogue
// dump produced from this is therefore SHORTER than the real-server
// emit and will not byte-match seq020_596B.bin without the field
// widths being locked down (the selftest XFAILs this comparison and
// logs the gap).
struct PlayerDataFields {
    // GameBaseData (§5.3):
    std::int8_t mapFilter = 0;
    std::string mapIcon;
    std::string description;
    // ShapeBaseData:
    std::string fileName;            // .dts shape filename
    std::string shieldShapeName;
    std::uint16_t sfxShield;         // group 1 block index, or sound_group_size for null
    std::uint8_t shadowDetailMask = 0;
    std::uint16_t explosionId;       // group 20 block index, or explosion_group_size for null
    std::uint16_t damageSkinId;      // group 2 block index, or damage_skin_group_size for null
    std::uint32_t debrisId = 0;
    float maxEnergy = 0.0f;
    static constexpr std::size_t kMaxSequenceCount = 16;
    struct SequenceSound {
        bool present = false;
        std::string sequenceName;
        std::uint16_t soundIndex = 0;  // group 1 block index
    };
    std::array<SequenceSound, kMaxSequenceCount> sequenceSounds{};
    bool isPerspective = false;
    // PlayerData:
    std::string flameShapeName;
    static constexpr std::size_t kAnimSlotCount = 32;
    struct AnimSlot {
        std::string name;
        bool hasSound = false;
        std::uint16_t soundIndex = 0;
        bool directionPositive = false;
        std::uint8_t viewFlags = 0;
        std::uint8_t priority = 0;
    };
    std::array<AnimSlot, kAnimSlotCount> animSlots{};
    float minDamageSpeed = 0.0f;
    float jetEnergyDrain = 0.0f;
    float maxForwardSpeed = 0.0f;     // /64 then 10-bit quantize
    float maxBackwardSpeed = 0.0f;
    float maxSideSpeed = 0.0f;
    bool canCrouch = false;
    float maxJetSideForceFactor = 0.0f;  // [0..1] → 8-bit
    float groundForce = 0.0f;
    float groundTraction = 0.0f;
    float maxJetForwardVelocity = 0.0f;
    float jumpSurfaceMinDot = 0.0f;      // [0..1] → 8-bit
    float jumpImpulse = 0.0f;
    float mass = 0.0f;
    float drag = 0.0f;
    float density = 0.0f;
    float jetForce = 0.0f;
    std::uint16_t jetSound = 0;          // group 1 block index
    static constexpr std::size_t kFootSoundCount = 9;
    struct FootStep {
        std::uint16_t rightFoot = 0;     // group 1 block index
        std::uint16_t leftFoot = 0;      // group 1 block index
    };
    std::array<FootStep, kFootSoundCount> footSteps{};
    std::uint32_t footPrintsA = 0;       // raw 32-bit bitmap-id
    std::uint32_t footPrintsB = 0;       // raw 32-bit bitmap-id
    float boxWidth = 0.0f;
    float boxDepth = 0.0f;
    float boxNormalHeight = 0.0f;
    float boxCrouchHeight = 0.0f;
};

struct PlayerDataRefSizes {
    std::uint8_t soundGroupSize = 153;       // group 1
    std::uint8_t explosionGroupSize = 20;    // group 20
    std::uint8_t damageSkinGroupSize = 2;    // group 2
};
DatablockEntry make_player_data(std::uint8_t group_size,
                                std::uint8_t block,
                                const PlayerDataRefSizes& refs,
                                const PlayerDataFields& f);

// Helper: build an empty-group sentinel record (block = 255, no body).
DatablockEntry make_sentinel_entry(DatablockClass kind);

// --- Encoder primitives (exposed for selftest / advanced callers) ---

// Append a v3 string (uncompressed form) to `w` per §6.7. Bits:
// 1 compression-flag (= 0), 8 length, 8*len raw body bits at the current
// bit position with NO byte alignment.
void write_v3_string_uncompressed(BitWriter& w, const std::string& s);

// Append a [0..1] quantized float to `w` per §6.4.
void write_quantized_float(BitWriter& w, float f, unsigned bits);

// Append a raw IEEE-754 32-bit float to `w` per §6.9 (no alignment).
void write_raw_float32(BitWriter& w, float f);

// Append a block-id cross-reference per §3.3. `target_group_size` is the
// catalogue-wide group_size of the *target* group. `block_or_null` is
// either a valid block_index in [0..target_group_size-1] OR the value
// target_group_size itself (the null sentinel). The width written is
// bits_to_represent(target_group_size + 1).
//
// SPEC-AMBIGUITY (Open Question #2): the alternative convention
// "block_index + 1 with 0 = null" is wire-distinguishable from this one
// once the SoundData record's profile reference can be ground-truthed.
// We pick this convention (the §3.3 description) pending live capture.
void write_block_id_ref(BitWriter& w, std::uint16_t block_or_null,
                        std::uint8_t target_group_size);

// Width in bits of a block-id cross-reference into a group of size N.
unsigned block_id_ref_width(std::uint8_t target_group_size) noexcept;

// --- Catalogue packet encoder ---

// Pack `entries` into a single outbound UDP datagram body — i.e. one
// catalogue-dump packet. The output bytes are the bits of (in order):
//
//   * 1-bit event-sub-stream-present (= 1) [the rate-control + VC
//     header bytes are NOT included here; the caller prepends them]
//   * For each entry: full event framing (per NETPROTO §7.3 and §2 of
//     this spec) — event-present, guaranteed, seq-continuous,
//     has-explicit-seq + 7-bit explicit seq, 7-bit class tag, then the
//     6/8/8 catalogue header + body bits.
//   * 1-bit event-present = 0 terminator after the last entry.
//
// `event_class_tag` is the per-build class tag for the catalogue-entry
// event (88 in TAH captures per §6.1). `first_event_seq` is the
// ordered-channel sequence number for the first entry; each subsequent
// entry increments it by 1 (and uses seq-continuous = 1 to save bits).
//
// NOTE: this returns the EVENT SUB-STREAM bytes only. To produce a full
// wire datagram, prepend the 32-bit VC header + 2 rate-control flag bits
// (see TRIBES-NETPROTO §3.1 / §3.4) using e.g. encode_vc_header(...) from
// reliable_acks.hpp.
std::vector<std::uint8_t>
encode_catalogue_packet(const std::vector<DatablockEntry>& entries_for_packet,
                        std::uint16_t event_class_tag,
                        std::uint8_t first_event_seq);

// Bin a full catalogue into a sequence of packets per §3.5: greedy pack
// until the running event-sub-stream byte size reaches `soft_max_bytes`,
// then start a new packet. Per §3.5 the isFull check runs at the TOP of
// the loop, so a single entry whose body crosses the cap is still
// emitted (and lands alone in its packet).
//
// Returns one vector<uint8_t> per packet, each holding the event-sub-
// stream bytes (caller wraps with VC header / rate-control bits).
std::vector<std::vector<std::uint8_t>>
pack_catalogue_into_packets(const std::vector<DatablockEntry>& all_entries,
                            std::size_t soft_max_bytes,
                            std::uint16_t event_class_tag,
                            std::uint8_t first_event_seq);

// Approximate width (in bits) of a single catalogue-entry event in the
// event-sub-stream encoding, used by pack_catalogue_into_packets to
// estimate whether the next entry fits in the current packet. Counts the
// event framing (≤17 bits) + the 6+8+8 catalogue header + body_bit_count.
std::size_t catalogue_entry_wire_bits(const DatablockEntry& e,
                                      bool seq_continuous);

// Run the spec's validation vectors against the encoder. Wires into the
// net-test-client's --tah-datablock-selftest CLI flag.
//
// Returns 0 on success, non-zero on any vector mismatch (or partial
// success flagged as XFAIL per spec gaps).
int tah_datablock_encoder_selftest();

}  // namespace net20
