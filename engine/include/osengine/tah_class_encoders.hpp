// Track 26 spec 14c-I-2 — per-class ghost encoders for the TAH-extended
// class roster (clean-room).
//
// Extends `ghost_encoder.hpp`'s five-class set (Player / StaticShape /
// Item / Projectile / Vehicle) with the additional classes documented
// in `docs/clean-room-specs/TRIBES-GHOST-CLASSES.md`:
//
//   §3.1  Marker            (engine common range, tag 129)
//   §3.2  Moveable          (game-specific; Door / Elevator / Car)
//   §3.3  Trigger           (engine common range, tag 132)
//   §3.6  SoundSource       (engine common range, tag 131)
//   §3.7  Mine              (game-specific; inherits Item layout)
//   §3.8  Sensor            (game-specific; inherits StaticBase chain)
//   §3.9  Turret            (game-specific; inherits StaticShape layout)
//
// Mine / Turret reuse the existing Item / StaticShape encoders;
// Sensor adds a 1-bit initial flag + DfW-bit data-file id at the
// tail of the StaticShape body.
//
// Wire format reference: clean-room spec
// `docs/clean-room-specs/TRIBES-GHOST-CLASSES.md` §3.

#ifndef OSENGINE_TAH_CLASS_ENCODERS_HPP
#define OSENGINE_TAH_CLASS_ENCODERS_HPP

#include <osengine/ghost_encoder.hpp>
#include <osengine/ghost_types.hpp>
#include <osengine/reliable_acks.hpp>

#include <array>
#include <cstdint>

namespace net20 {

// ----- Per-class typed structs (mirrors of ghost_types.hpp style) -----

// §3.1 Marker — mission triggers, CTF capture zones, spawn-point
// markers, drop-points, team bases. Does NOT derive from ShapeBase,
// so the wire format has the §15.0.1 base-state block but NO
// shape-layer block.
struct MarkerGhost {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;

    bool initial_update = false;
    std::uint8_t marker_data_file_id = 0;       // `DfW` bits (8 typical)

    bool transform_changed = false;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;   // 96-bit bit-packed
    float rot_x = 0.0f, rot_y = 0.0f, rot_z = 0.0f;   // 96-bit bit-packed
};

// §3.2 Moveable — mission objects that follow a pre-defined path
// (doors, elevators, scripted vehicles). No shape-layer.
struct MoveableGhost {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;

    bool initial_update = false;
    std::uint8_t moveable_data_file_id = 0;     // `DfW` bits (8 typical)

    bool path_changed = false;
    std::uint8_t path_id = 0;                   // 8 bits

    bool position_on_path_changed = false;
    std::uint16_t waypoint_index = 0;           // `WpW` bits
    float waypoint_time = 0.0f;                 // 32-bit IEEE-754 LE, bit-packed

    bool state_changed = false;
    bool is_blocked = false;
    std::uint16_t stop_waypoint_index = 0;      // `WpW` bits
    std::uint8_t movement_state = 0;            // 2 bits: 0 stopped / 1 fwd /
                                                //         2 reverse / 3 paused
};

// §3.3 Trigger — mission-defined invisible volumes. No shape-layer.
//
// The transform block is laid out as 12 bit-packed floats forming a
// 3x4 column-major matrix (rotation 3x3 + translation 3x1). The
// receiver consumes them in stream order; semantics are left to the
// caller. (Per spec open question §7.6 the exact column-vs-row
// convention should be verified by the shape-renderer port; encoder
// just emits the 12 floats in the order supplied.)
struct TriggerGhost {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;

    bool transform_changed = false;
    std::array<float, 12> transform_matrix{};   // 12 floats, 384 bits

    bool bbox_changed = false;
    std::array<float, 6> bounding_box{};        // min-X/Y/Z, max-X/Y/Z; 192 bits
};

// §3.6 Sound source — networked positional sound emitter.
struct SoundSourceGhost {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;

    bool initial_update = false;
    std::uint8_t sound_data_file_id = 0;        // `DfW` bits (8 typical)
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    bool is_looping = false;
    bool follow_source = false;
    std::uint16_t parent_ghost_id = 0;          // 10 bits, only when follow_source = 1

    bool state_changed = false;
    bool is_playing = false;
};

// §3.7 Mine — inherits Item layout verbatim. Provided as a distinct
// alias so dispatchers can be table-driven; on the wire it IS a
// `GhostItem`.
using MineGhost = GhostItem;

// §3.8 Sensor — inherits StaticBase chain. Wire layout is the
// StaticShape body (base+shape+transform+damage+info, minus the
// shape-info-changed sub-block) plus a 1-bit initial flag + DfW-bit
// sensor data-file id at the tail.
struct SensorGhost {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;

    bool transform_changed = false;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    float rot_x = 0.0f, rot_y = 0.0f, rot_z = 0.0f;

    bool damage_changed = false;
    bool state_enabled = false;
    bool state_disabled = false;
    float damage_level = 0.0f;          // 8-bit, in [0, 1]

    bool info_changed = false;
    bool is_target = false;

    bool initial_update = false;
    std::uint8_t sensor_data_file_id = 0;       // `DfW` bits (8 typical)
};

// §3.9 Turret — inherits StaticShape layout verbatim. Alias for
// table-driven dispatch.
using TurretGhost = GhostStaticShape;

// ----- Body writers (mirror of ghost_types.cpp read_* style) -----

// SPEC-AMBIGUITY: `DfW` (data-file id width) is build-dependent
// (spec §7 open question 1). Stock Tribes 1.41 uses 8 bits per
// channel, which is what these encoders default to. Callers with a
// custom datablock set should pass an explicit width.
constexpr std::uint8_t kDefaultDfWBits = 8;

// SPEC-AMBIGUITY: `WpW` (waypoint-index width) for Moveable
// (spec §7 open question 2). The spec says typical missions use
// 5..7 bits; without a Moveable in the available capture window we
// pick 6 bits as the default — the median of the suggested range
// and the width the spec uses in its in-line worked example.
constexpr std::uint8_t kDefaultWpWBits = 6;

// §3.1 Marker body.
void write_marker_body(BitWriter& w, const MarkerGhost& g,
                       std::uint8_t datafile_id_width = kDefaultDfWBits);

// §3.2 Moveable body.
void write_moveable_body(BitWriter& w, const MoveableGhost& g,
                         std::uint8_t datafile_id_width = kDefaultDfWBits,
                         std::uint8_t waypoint_bit_width = kDefaultWpWBits);

// §3.3 Trigger body.
void write_trigger_body(BitWriter& w, const TriggerGhost& g);

// §3.6 SoundSource body.
void write_sound_source_body(BitWriter& w, const SoundSourceGhost& g,
                             std::uint8_t datafile_id_width = kDefaultDfWBits);

// §3.8 Sensor body. The shape-layer block IS emitted (Sensor derives
// from ShapeBase via StaticBase), so callers writing a full Sensor
// record must also emit `write_base_state` + `write_shape_layer_block`
// before this body.
void write_sensor_body(BitWriter& w, const SensorGhost& g,
                       std::uint8_t datafile_id_width = kDefaultDfWBits);

// §3.7 / §3.9 Mine / Turret: callers reuse the Item / StaticShape
// encoders from ghost_encoder.hpp. No new writers needed.

// ----- Selftest -----

// Roundtrip-encode and re-decode a Marker, a SoundSource, and a
// Trigger; assert byte-for-byte output equality with the synthesised
// inputs by parsing the bytes back via a local body reader. Returns
// 0 on success, non-zero on failure.
//
// Also verifies the class-tag registry returns the expected roles for
// the enumerated Sim / Common / known-game-specific tags.
int tah_class_encoders_selftest();

}  // namespace net20

#endif  // OSENGINE_TAH_CLASS_ENCODERS_HPP
