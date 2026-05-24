// Track 20 spec 14 — typed ghost-record dispatch (clean-room).
//
// Builds on the outer-framing parser in `ghost_stream.{hpp,cpp}` (spec 10)
// by walking past the leading per-object record and decoding each per-class
// payload into a strongly-typed struct, per the per-class layouts documented
// in §15 of `docs/clean-room-specs/TRIBES-NETPROTO.md` (spec 13).
//
// Only the five game-relevant ghost classes are decoded here:
//   - GhostPlayer       (§15.1)
//   - GhostProjectile   (§15.2)  — grenade-style variant only
//   - GhostItem         (§15.3)
//   - GhostVehicle      (§15.4)
//   - GhostStaticShape  (§15.5)
//
// In Tribes 1.41 the 10-bit class-tag is assigned at server-build time and
// is therefore build-specific (§15.5.1, open question). For the 2026-05-22
// capture (`captures/real-tribes/groove-session-20260522-100230.json`) only
// five distinct class tags were observed:
//   96, 333, 512, 615, 896
// all of which represent "mission-pinned static" objects — i.e. the
// StaticShape class. The registry below lets callers (a) supply a known
// `class_tag -> ghost_class_kind` mapping, and (b) defaults unknown tags
// to GhostStaticShape so the scope-always burst still parses.

#pragma once

#include "ghost_stream.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace net20 {

enum class GhostClassKind : std::uint8_t {
    Unknown      = 0,
    Player       = 1,
    Projectile   = 2,
    Item         = 3,
    Vehicle      = 4,
    StaticShape  = 5,
};

// §15.0.1 base-state block (every replicated game object update begins
// with this block; fields are gated by `base_changed`).
struct GhostBaseState {
    bool base_changed = false;
    std::uint8_t team_id = 0;            // 5 bits
    bool has_control_client = false;
    std::uint16_t control_client_id = 0; // raw + 2048
    bool has_owner_client = false;
    std::uint16_t owner_client_id = 0;   // raw + 2048
};

// §15.0.2 shape-layer block (every Player / Vehicle / Projectile-derived /
// Item / StaticShape update emits this block immediately after §15.0.1).
// In the scope-always burst the typical wire pattern is 3 zero bits
// (shield/thread/fade all unchanged); deltas may carry sub-payloads.
struct GhostShapeLayer {
    bool shield_changed = false;
    float shield_dir_x = 0.0f, shield_dir_y = 0.0f, shield_dir_z = 0.0f;
    float shield_z_offset = 0.0f;        // 8-bit normalised, decoded m

    bool thread_changed = false;
    struct ThreadSlot {
        bool present = false;
        std::uint8_t sequence_id = 0;    // SeqW (provisional 6 bits per §15.0.2)
        std::uint8_t state = 0;          // 2 bits — 0 stop / 1 play / 2 pause
        bool forward = false;
        bool at_end = false;
    };
    std::array<ThreadSlot, 4> threads{};

    bool fade_changed = false;
    bool fade_in = false;
};

// §15.5 StaticShape per-class payload.
struct GhostStaticShape {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;                              // §15.0.2

    bool transform_changed = false;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;     // §15.5: bit-packed IEEE-754 floats (NOT byte-aligned per §6.9 corrected)
    float rot_x = 0.0f, rot_y = 0.0f, rot_z = 0.0f;     // Euler radians, same bit-packed form

    bool damage_changed = false;
    bool state_enabled = false;
    bool state_disabled = false;
    float damage_level = 0.0f;                          // 8-bit normalised in [0, 1]

    bool info_changed = false;
    bool is_target = false;

    // Shape-info sub-block (always emitted; the previous "variant" branch
    // was incorrect per §15.5 — there is one wire layout for every static).
    bool shape_info_changed = false;
    std::uint8_t shape_data_file_id = 0;
    bool has_sensor_key = false;
    std::uint8_t sensor_key = 0;
};

// §15.1 Player.  Most numeric fields are decoded at their wire-quantised
// precision and re-exposed as float / int so callers can render directly.
struct GhostPlayer {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;                              // §15.0.2

    bool initial_update = false;
    std::uint8_t datafile_id = 0;
    bool ai_controlled = false;

    bool mount_changed = false;
    bool has_mount = false;
    std::uint16_t mount_target_ghost = 0;       // 10-bit
    std::uint8_t mount_point = 0;               // 5-bit

    bool damage_changed = false;
    bool dead = false;
    float damage_level = 0.0f;                  // 6-bit, normalised in [0,1]
    bool blown_up = false;
    std::uint8_t death_anim_index = 0;          // 6-bit

    bool has_pos_block = false;
    float view_pitch = 0.0f;                    // signed 5-bit, * 88° / (2^4-1) (radians)
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;  // full IEEE-754 floats
    bool has_velocity = false;
    float velocity_x = 0.0f, velocity_y = 0.0f, velocity_z = 0.0f;
    bool on_ground = false;
    float yaw = 0.0f;                           // 9-bit, in radians [0..2π)

    // Non-initial only:
    bool has_move_block = false;
    bool move_redundant = false;
    float fwd_axis = 0.0f, back_axis = 0.0f, left_axis = 0.0f, right_axis = 0.0f; // 4-bit each / 15.0
    bool jet_held = false, jump_held = false, crouch_held = false;
    float energy = 0.0f;                        // 7-bit / 127.0
    std::uint8_t skip_count = 0;                // 4-bit
    bool no_interp = false;

    bool anim_changed = false;
    std::uint8_t anim_index = 0;                // 6-bit

    bool recharge_changed = false;
    float recharge_rate = 0.0f;                 // 32-bit IEEE-754 float

    bool pda_mode = false;
    bool crouch_state = false;

    bool inventory_changed = false;
    struct ItemSlot {
        bool changed = false;
        std::uint8_t item_type_id = 0;    // raw - 1; 0 wire = no item
        bool has_team_tint = false;
        std::uint8_t team_id = 0;
        bool trigger_down = false;
        bool ammo_present = false;
        std::uint8_t fire_count = 0;      // 3-bit
        bool initial_state_is_fire = false;
    };
    std::array<ItemSlot, 4> inventory{};
};

// §15.2 Projectile (grenade-style variant — common shape).
struct GhostProjectile {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;                              // §15.0.2

    bool initial_update = false;
    // Introduction (initial_update = 1):
    std::uint8_t projectile_data_file_id = 0;
    bool has_shooter = false;
    std::uint16_t shooter_ghost = 0;            // 10-bit
    float init_pos_x = 0.0f, init_pos_y = 0.0f, init_pos_z = 0.0f;
    float init_vel_x = 0.0f, init_vel_y = 0.0f, init_vel_z = 0.0f;

    // Steady-state (initial_update = 0):
    bool position_changed = false;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    bool velocity_full = false;
    float velocity_x = 0.0f, velocity_y = 0.0f, velocity_z = 0.0f;
    // Compressed-velocity (velocity_full=0) is a normal-vector (9,9). We
    // capture the reconstructed direction vector here; the receiver must
    // multiply by its locally-tracked speed.
    float velocity_dir_x = 0.0f, velocity_dir_y = 0.0f, velocity_dir_z = 0.0f;
    bool collision_sound_changed = false;
    std::uint8_t surface_material = 0;
};

// §15.3 Item (CTF flag / pickup / health kit / inventory station).
struct GhostItem {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;                              // §15.0.2

    bool info_changed = false;
    std::uint8_t item_data_file_id = 0;
    bool has_sensor_key = false;
    std::uint8_t sensor_key = 0;
    bool has_thrower = false;
    std::uint16_t thrower_ghost = 0;            // 10-bit
    bool rotate = false;
    bool collideable = false;

    bool rotation_changed = false;
    float yaw = 0.0f;                           // 8-bit, * 2π / 255

    bool position_changed = false;
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    bool at_rest_or_rotates = false;
    bool velocity_changed = false;
    float velocity_x = 0.0f, velocity_y = 0.0f, velocity_z = 0.0f;
};

// §15.4 Vehicle (flyer / tank / hover).
struct GhostVehicle {
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    std::uint16_t class_tag = 0;
    GhostBaseState base;
    GhostShapeLayer shape;                              // §15.0.2

    bool suppress_update = false;

    bool orientation_changed = false;
    float rot_x = 0.0f, rot_y = 0.0f, rot_z = 0.0f;  // pitch, roll, yaw (radians)
    float pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    bool has_move = false;
    bool move_redundant = false;
    float fwd_axis = 0.0f, back_axis = 0.0f, left_axis = 0.0f, right_axis = 0.0f;
    bool jet_held = false, jump_held = false, crouch_held = false;
    float speed_fraction = 0.0f;
    float lift = 0.0f;
    std::uint8_t skip_count = 0;

    bool status_changed = false;
    float current_speed = 0.0f, desired_speed = 0.0f;

    bool fire_count_changed = false;
    std::uint8_t fire_count = 0;

    bool damage_changed = false;
    float damage_level = 0.0f;

    bool sound_changed = false;
    std::uint8_t sound_state = 0;
};

// The registry: per-class tables keyed by ghost_id, plus the
// class_tag<->ghost mapping established on initial introduction.
//
// The class-tag-to-class-kind map MUST be populated by the caller (it is
// build-specific per §15.5.1 — the open question). Tags not in the map
// default to StaticShape on first introduction (which is what the
// 2026-05-22 capture's five known scope-always tags resolve to).
struct GhostRegistry {
    // class_tag -> kind. Caller-supplied. Defaults can be installed via
    // install_default_class_tag_map().
    std::unordered_map<std::uint16_t, GhostClassKind> class_tag_kinds;

    // ghost_id -> kind (set on first introduction).
    std::unordered_map<std::uint16_t, GhostClassKind> ghost_kinds;

    // ghost_id -> class_tag (set on first introduction).
    std::unordered_map<std::uint16_t, std::uint16_t> ghost_class_tags;

    // Per-class typed tables.
    std::unordered_map<std::uint16_t, GhostPlayer>       players;
    std::unordered_map<std::uint16_t, GhostProjectile>   projectiles;
    std::unordered_map<std::uint16_t, GhostItem>         items;
    std::unordered_map<std::uint16_t, GhostVehicle>      vehicles;
    std::unordered_map<std::uint16_t, GhostStaticShape>  statics;

    // Install the capture-grounded default class-tag map: the 5 known
    // scope-always tags from the 2026-05-22 capture all map to StaticShape.
    void install_default_class_tag_map();

    // Reset all state.
    void clear();
};

// One decoded record, kind-tagged. Returned in a vector by parse_typed_packet
// so the caller can log per-record / per-kind diagnostics.
struct TypedRecord {
    GhostClassKind kind = GhostClassKind::Unknown;
    std::uint16_t ghost_id = 0;
    bool kill = false;
    bool full_update = false;          // true if this was a new-id introduction
    std::uint16_t class_tag = 0;       // raw class tag (valid iff full_update)
    std::size_t start_bit = 0;
    std::size_t end_bit = 0;
    std::string log_line;              // one-line summary suitable for stderr
};

struct TypedPacketDecode {
    GhostPacketDecode framing;        // VC + rate-prefix + scan result
    std::vector<TypedRecord> records;
    std::string note;                  // diagnostics
    bool walked_full_stream = false;   // true if the per-object loop reached
                                       // the 0-object-present terminator without
                                       // overruns or class-payload failures
};

// Full per-packet decode pipeline. Updates `registry` with everything
// successfully decoded. Returns a `TypedPacketDecode` for caller logging.
TypedPacketDecode parse_typed_packet(const std::uint8_t* data,
                                     std::size_t length,
                                     GhostRegistry& registry);

// Spec 28/04b helper: same decode pipeline but skips the heuristic
// scanner in parse_ghost_packet and walks the per-object loop directly
// from `ghost_stream_start_bit`. Required for self-tests + future
// own-client integration where we know exactly where the ghost
// sub-stream begins. The framing fields (VC header, rate prefix)
// in the returned struct are LEFT EMPTY — caller decoded those
// upstream.
TypedPacketDecode parse_typed_packet_at_offset(const std::uint8_t* data,
                                               std::size_t length,
                                               std::size_t ghost_stream_start_bit,
                                               GhostRegistry& registry);

// Convenience: apply a decoded record to the registry. parse_typed_packet
// calls this internally; exposed for tests.
void apply_update(GhostRegistry& reg, const TypedRecord& rec);

}  // namespace net20
