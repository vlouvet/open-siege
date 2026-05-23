// Track 20 spec 14 — typed ghost-record dispatch (clean-room).
// See ghost_types.hpp for class layout references.

#include "ghost_types.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace net20 {

namespace {

// Bit cursor over a fixed-length byte buffer. LSB-first within each byte:
//   bit(N) = (data[N >> 3] >> (N & 7)) & 1.
// Duplicate of the one in ghost_stream.cpp; kept local to avoid leaking
// internal types across translation units.
struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t bit_length = 0;
    std::size_t pos = 0;
    bool overrun = false;

    explicit BitReader(const std::uint8_t* d, std::size_t bytes) noexcept
        : data(d), bit_length(bytes * 8) {}

    std::uint32_t read_bits(unsigned width) noexcept {
        if (width == 0) return 0;
        if (width > 32 || pos + width > bit_length) {
            overrun = true;
            return 0;
        }
        std::uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            const std::size_t p = pos + i;
            const std::uint8_t bit = (data[p >> 3] >> (p & 7)) & 1u;
            value |= static_cast<std::uint32_t>(bit) << i;
        }
        pos += width;
        return value;
    }

    bool read_flag() noexcept { return read_bits(1) != 0; }

    // Sign-extend a value of `width` bits.
    std::int32_t read_signed(unsigned width) noexcept {
        if (width == 0 || width > 32) { overrun = true; return 0; }
        const std::uint32_t raw = read_bits(width);
        const std::uint32_t sign = 1u << (width - 1);
        if (raw & sign) {
            // Sign-extend.
            const std::uint32_t mask = (width == 32) ? 0u : ~((1u << width) - 1u);
            return static_cast<std::int32_t>(raw | mask);
        }
        return static_cast<std::int32_t>(raw);
    }

    // Align to next byte boundary.
    void align_to_byte() noexcept {
        const std::size_t rem = pos & 7;
        if (rem != 0) pos += (8 - rem);
    }

    // Read a 32-bit IEEE-754 LE float, bit-packed at the current bit cursor
    // with NO byte alignment (per §6.9 corrected and §15.5 critical note:
    // the raw bit-stream `write(n, ptr)` primitive is just "write n*8 bits
    // at the current bit position"). Spec 24's audit traced every "96-bit
    // position" field in §15 to this primitive, so the receiver must NEVER
    // align before reading float-3 tuples — doing so desynchronises the
    // walker on every record whose upstream flags leave the cursor at a
    // non-byte-aligned position.
    float read_float_unaligned() noexcept {
        if (pos + 32 > bit_length) { overrun = true; return 0.0f; }
        const std::uint32_t bits = read_bits(32);
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // §6.5 v3 — Normalised signed float in [-1, +1].
    //
    // Wire format: an UNSIGNED int of `width` bits (NOT sign-extended). The
    // encoder rescales f from [-1, +1] to [0, 1] then packs as writeInt:
    //     wire = round( ((f + 1) / 2) * (2^width - 1) )
    // The receiver decodes:
    //     f = (raw * 2) / (2^width - 1) - 1
    // Width 1 collapses to 0 always.
    //
    // v1 of the spec used `read_signed(width)` with scale `2^(width-1) - 1`;
    // that was wrong and produced velocity-direction / shield-direction
    // garbage. The §6.5 audit corrected this.
    float read_signed_float(unsigned width) noexcept {
        if (width == 0 || width > 32) { overrun = true; return 0.0f; }
        if (width == 1) { (void)read_bits(1); return 0.0f; }
        const std::uint32_t raw = read_bits(width);
        const float denom = static_cast<float>((1u << width) - 1u);
        return denom == 0.0f ? 0.0f
                             : (static_cast<float>(raw) * 2.0f) / denom - 1.0f;
    }

    // §6.8 — Form A normal-vector decode (single-width).
    //
    // Layout: signed-normalised x of `component_bits`, signed-normalised y of
    // `component_bits`, then a 1-bit `z < 0` sign flag. Total: 2*N + 1 bits.
    // Reconstruct z = sqrt(max(0, 1 - x*x - y*y)); negate iff sign flag = 1.
    void read_normal_vec_form_a(unsigned component_bits,
                                float& x, float& y, float& z) noexcept {
        x = read_signed_float(component_bits);
        y = read_signed_float(component_bits);
        const bool z_neg = read_flag();
        const float zsq = 1.0f - x * x - y * y;
        const float zmag = zsq > 0.0f ? std::sqrt(zsq) : 0.0f;
        z = z_neg ? -zmag : zmag;
    }
};

// Decode the §15.0.1 base-state block.
GhostBaseState read_base_state(BitReader& br) {
    GhostBaseState s;
    s.base_changed = br.read_flag();
    if (!s.base_changed) return s;
    s.team_id = static_cast<std::uint8_t>(br.read_bits(5));
    s.has_control_client = br.read_flag();
    if (s.has_control_client) {
        const std::uint32_t off = br.read_bits(7);
        s.control_client_id = static_cast<std::uint16_t>(off + 2048u);
    }
    s.has_owner_client = br.read_flag();
    if (s.has_owner_client) {
        const std::uint32_t off = br.read_bits(7);
        s.owner_client_id = static_cast<std::uint16_t>(off + 2048u);
    }
    return s;
}

// Decode the §15.0.2 shape-layer block.
//
// The block opens with three 1-bit "changed" flags (shield / thread / fade);
// each gates its own sub-payload. In the scope-always burst the common
// case is a 3-bit run of zeros — but the receiver MUST consume those three
// bits unconditionally, because every shape-bearing class emits them
// (Player, Vehicle, Projectile-derived, Item, static-shape). Skipping
// this block desynchronises the walker by 3 bits per record and corrupts
// every record after the first one in a packet (spec 24's root cause).
//
// Sub-payload layout (v3 — spec 26 audit corrected shield direction width
// from 14 to 15 bits — the 1-bit z-sign flag had been missed):
//   shield-changed = 1 -> 15-bit shield direction (§6.8 Form A with
//     bits=7: signed-normalised x(7) + signed-normalised y(7) +
//     1-bit z-sign) followed by 8-bit z-offset.
//   thread-changed = 1 -> for each of 4 thread slots: 1-bit per-slot
//     present; if present, SeqW-bit sequence id + 2-bit state +
//     1-bit direction + 1-bit at-end. SeqW is build-dependent and not
//     directly verifiable from this capture (§15.0.2 open question);
//     we use 6 bits provisionally (see open question in spec).
//   fade-changed = 1 -> 1-bit fade direction.
GhostShapeLayer read_shape_layer_block(BitReader& br) {
    GhostShapeLayer s;

    s.shield_changed = br.read_flag();
    if (s.shield_changed) {
        // §15.0.2 v3: 15-bit shield direction via §6.8 Form A (2*7 + 1).
        float x, y, z;
        br.read_normal_vec_form_a(7, x, y, z);
        s.shield_dir_x = x;
        s.shield_dir_y = y;
        s.shield_dir_z = z;
        // 8-bit z-offset (normalised float of width 8 per §6.4 / §15.0.2
        // v3): decode raw to f = raw / 255, then z_off_m = f * 10 - 5
        // for a range of [-5, +5] m.
        const std::uint32_t zoff_bits = br.read_bits(8);
        const float zoff_norm = static_cast<float>(zoff_bits) / 255.0f;
        s.shield_z_offset = (zoff_norm * 10.0f) - 5.0f;
    }

    s.thread_changed = br.read_flag();
    if (s.thread_changed) {
        // Provisional SeqW: 6 bits per §15.0.2 open question.
        constexpr unsigned kSeqW = 6;
        for (auto& slot : s.threads) {
            slot.present = br.read_flag();
            if (!slot.present) continue;
            slot.sequence_id = static_cast<std::uint8_t>(br.read_bits(kSeqW));
            slot.state = static_cast<std::uint8_t>(br.read_bits(2));
            slot.forward = br.read_flag();
            slot.at_end = br.read_flag();
        }
    }

    s.fade_changed = br.read_flag();
    if (s.fade_changed) {
        s.fade_in = br.read_flag();
    }

    return s;
}

// §15.5 StaticShape per-class payload (post-base-state, post-shape-layer).
// Returns true on a clean decode; false on overrun.
//
// Per the 2026-05-22 revision of §15.5 the StaticShape body is laid out as:
//   transform-changed flag, optional bit-packed 96+96-bit position+rotation
//   damage-changed flag, optional 1+1+8 bits
//   info-changed flag, optional 1-bit is-target
//   shape-info-changed flag, optional DfW-bit data-file id + sensor-key
//
// Every concrete static-shape class emits the same wire format — there is
// no "plain static" vs. "shape-bearing static" branch. The previous
// spec-14 implementer's per-variant conditional was incorrect (§15.5).
//
// Position and rotation floats are bit-packed (NOT byte-aligned) per §6.9
// corrected. A byte-aligned read produces visually-correct results only
// when the upstream flags happen to leave the cursor byte-aligned and
// garbage in any other case.
bool read_static_shape_body(BitReader& br, GhostStaticShape& s) {
    s.transform_changed = br.read_flag();
    if (s.transform_changed) {
        s.pos_x = br.read_float_unaligned();
        s.pos_y = br.read_float_unaligned();
        s.pos_z = br.read_float_unaligned();
        s.rot_x = br.read_float_unaligned();
        s.rot_y = br.read_float_unaligned();
        s.rot_z = br.read_float_unaligned();
    }
    if (br.overrun) return false;

    s.damage_changed = br.read_flag();
    if (s.damage_changed) {
        s.state_enabled = br.read_flag();
        if (!s.state_enabled) {
            s.state_disabled = br.read_flag();
        }
        const std::uint32_t dl = br.read_bits(8);
        s.damage_level = static_cast<float>(dl) / 255.0f;
    }
    if (br.overrun) return false;

    s.info_changed = br.read_flag();
    if (s.info_changed) {
        s.is_target = br.read_flag();
    }
    if (br.overrun) return false;

    s.shape_info_changed = br.read_flag();
    if (s.shape_info_changed) {
        s.shape_data_file_id = static_cast<std::uint8_t>(br.read_bits(8));
        s.has_sensor_key = br.read_flag();
        if (s.has_sensor_key) {
            s.sensor_key = static_cast<std::uint8_t>(br.read_bits(7));
        }
    }
    if (br.overrun) return false;

    return true;
}

// §15.1 Player. Substantial; only the wire fields are decoded — receivers
// can interpret view_pitch / yaw / etc.
//
// `initial_update` here means the per-class introduction (datafile-changed = 1).
// Wire `not_initial` = !record.full_update from the framing parser.
bool read_player_body(BitReader& br, GhostPlayer& p,
                      const std::uint8_t datafile_id_width)
{
    // Per §15.1 the datafile-changed flag is "1 on the initial introduction
    // of this Player ghost (and only then)". The framing-layer `full_update`
    // signals the same thing. The flag itself still appears on the wire in
    // every Player update.
    p.initial_update = br.read_flag();
    if (p.initial_update) {
        p.datafile_id = static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
        p.ai_controlled = br.read_flag();
    }

    p.mount_changed = br.read_flag();
    if (p.mount_changed) {
        p.has_mount = br.read_flag();
        if (p.has_mount) {
            p.mount_target_ghost = static_cast<std::uint16_t>(br.read_bits(10));
            p.mount_point = static_cast<std::uint8_t>(br.read_bits(5));
        }
    }

    p.damage_changed = br.read_flag();
    if (p.damage_changed) {
        p.dead = br.read_flag();
        if (!p.dead) {
            p.damage_level = static_cast<float>(br.read_bits(6)) / 63.0f;
        } else {
            p.blown_up = br.read_flag();
            if (!p.blown_up) {
                p.death_anim_index = static_cast<std::uint8_t>(br.read_bits(6));
            }
        }
    }

    p.has_pos_block = br.read_flag();
    if (p.has_pos_block) {
        // §15.1 v3: signed 5-bit normalised float (§6.5 v3) * max-pitch
        // (88° in radians). Width 5 → denom = 31 (NOT 15 as v1 implied).
        const float view_pitch_norm = br.read_signed_float(5);
        constexpr float kMaxPitch = 88.0f * 3.14159265358979323846f / 180.0f;
        p.view_pitch = view_pitch_norm * kMaxPitch;

        // Full IEEE-754 position (96 bits bit-packed per §6.9 corrected).
        p.pos_x = br.read_float_unaligned();
        p.pos_y = br.read_float_unaligned();
        p.pos_z = br.read_float_unaligned();

        p.has_velocity = br.read_flag();
        if (p.has_velocity) {
            const std::uint32_t mag_bits = br.read_bits(17);
            const float mag = static_cast<float>(mag_bits) / 512.0f;
            // §15.1 v3 corrected: velocity direction is a Form A
            // normal-vector with bits=10. Total width 21 bits
            // (2*10 + 1). v1 mis-classified this as Form B (10, 10) =
            // 20 bits, which left the bit cursor off by one and
            // desynchronised every subsequent record in the packet.
            float dx, dy, dz;
            br.read_normal_vec_form_a(10, dx, dy, dz);
            p.velocity_x = dx * mag;
            p.velocity_y = dy * mag;
            p.velocity_z = dz * mag;
        }

        p.on_ground = br.read_flag();
        const std::uint32_t yaw_bits = br.read_bits(9);
        p.yaw = static_cast<float>(yaw_bits) * (2.0f * 3.14159265358979323846f / 511.0f);

        if (!p.initial_update) {
            // §15.1 v3 correction: there is NO move-redundancy flag in
            // the Player ghost record. The axes + button block is
            // emitted unconditionally on non-initial updates with
            // pos-block-present = 1. v1 incorrectly included a flag
            // borrowed from the §17 input-frame encoding.
            p.has_move_block = true;
            p.move_redundant = false;
            p.fwd_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            p.back_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            p.left_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            p.right_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            p.jet_held = br.read_flag();
            p.jump_held = br.read_flag();
            p.crouch_held = br.read_flag();
            p.energy = static_cast<float>(br.read_bits(7)) / 127.0f;
            p.skip_count = static_cast<std::uint8_t>(br.read_bits(4));
            p.no_interp = br.read_flag();
        }
    }

    if (!p.initial_update) {
        p.anim_changed = br.read_flag();
        if (p.anim_changed) {
            p.anim_index = static_cast<std::uint8_t>(br.read_bits(6));
        }
    }

    p.recharge_changed = br.read_flag();
    if (p.recharge_changed) {
        p.recharge_rate = br.read_float_unaligned();
    }

    p.pda_mode = br.read_flag();
    p.crouch_state = br.read_flag();

    p.inventory_changed = br.read_flag();
    if (p.inventory_changed) {
        for (std::size_t i = 0; i < p.inventory.size(); ++i) {
            auto& slot = p.inventory[i];
            slot.changed = br.read_flag();
            if (!slot.changed) continue;
            // 8-bit item-type-id wire = type_id + 1 (so wire 0 = "no item").
            const std::uint32_t raw = br.read_bits(8);
            slot.item_type_id = static_cast<std::uint8_t>(
                raw == 0 ? 0 : (raw - 1));
            slot.has_team_tint = br.read_flag();
            if (slot.has_team_tint) {
                slot.team_id = static_cast<std::uint8_t>(br.read_bits(3));
            }
            slot.trigger_down = br.read_flag();
            slot.ammo_present = br.read_flag();
            slot.fire_count = static_cast<std::uint8_t>(br.read_bits(3));
            if (p.initial_update) {
                slot.initial_state_is_fire = br.read_flag();
            }
        }
    }

    return !br.overrun;
}

// §15.3 Item.
bool read_item_body(BitReader& br, GhostItem& it,
                    const std::uint8_t datafile_id_width)
{
    it.info_changed = br.read_flag();
    if (it.info_changed) {
        it.item_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
        it.has_sensor_key = br.read_flag();
        if (it.has_sensor_key) {
            it.sensor_key = static_cast<std::uint8_t>(br.read_bits(7));
        }
        it.has_thrower = br.read_flag();
        if (it.has_thrower) {
            it.thrower_ghost = static_cast<std::uint16_t>(br.read_bits(10));
        }
        it.rotate = br.read_flag();
        it.collideable = br.read_flag();
    }
    it.rotation_changed = br.read_flag();
    if (it.rotation_changed) {
        const std::uint32_t yaw_bits = br.read_bits(8);
        it.yaw = static_cast<float>(yaw_bits) / 255.0f
               * (2.0f * 3.14159265358979323846f);
    }
    it.position_changed = br.read_flag();
    if (it.position_changed) {
        it.pos_x = br.read_float_unaligned();
        it.pos_y = br.read_float_unaligned();
        it.pos_z = br.read_float_unaligned();
    }
    it.at_rest_or_rotates = br.read_flag();
    if (!it.at_rest_or_rotates) {
        it.velocity_changed = br.read_flag();
        if (it.velocity_changed) {
            it.velocity_x = br.read_float_unaligned();
            it.velocity_y = br.read_float_unaligned();
            it.velocity_z = br.read_float_unaligned();
        }
    }
    return !br.overrun;
}

// §15.2 Projectile (grenade-style variant).
bool read_projectile_body(BitReader& br, GhostProjectile& pr,
                          const std::uint8_t datafile_id_width)
{
    pr.initial_update = br.read_flag();
    if (pr.initial_update) {
        pr.projectile_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
        pr.has_shooter = br.read_flag();
        if (pr.has_shooter) {
            pr.shooter_ghost = static_cast<std::uint16_t>(br.read_bits(10));
        }
        // Grenade-style: 96-bit init pos + 96-bit init velocity (per §15.2).
        pr.init_pos_x = br.read_float_unaligned();
        pr.init_pos_y = br.read_float_unaligned();
        pr.init_pos_z = br.read_float_unaligned();
        pr.init_vel_x = br.read_float_unaligned();
        pr.init_vel_y = br.read_float_unaligned();
        pr.init_vel_z = br.read_float_unaligned();
    } else {
        pr.position_changed = br.read_flag();
        if (pr.position_changed) {
            pr.pos_x = br.read_float_unaligned();
            pr.pos_y = br.read_float_unaligned();
            pr.pos_z = br.read_float_unaligned();
        }
        pr.velocity_full = br.read_flag();
        if (pr.velocity_full) {
            pr.velocity_x = br.read_float_unaligned();
            pr.velocity_y = br.read_float_unaligned();
            pr.velocity_z = br.read_float_unaligned();
        } else {
            // §15.2 v3: compressed velocity direction is a Form A
            // normal-vector with bits=9. Total 19 bits (2*9 + 1). v1
            // mis-classified as Form B "(9, 9) = 18 bits" which
            // dropped the z-sign flag and read garbage.
            float dx, dy, dz;
            br.read_normal_vec_form_a(9, dx, dy, dz);
            pr.velocity_dir_x = dx;
            pr.velocity_dir_y = dy;
            pr.velocity_dir_z = dz;
        }
        pr.collision_sound_changed = br.read_flag();
        if (pr.collision_sound_changed) {
            pr.surface_material = static_cast<std::uint8_t>(br.read_bits(4));
        }
    }
    return !br.overrun;
}

// §15.4 Vehicle (v3 — spec 26 audit).
//
// Layout corrections vs v1:
//   1) suppress-update flag is FIRST. When 1, no §15.0.1 base-state, no
//      §15.0.2 shape-layer, and no Vehicle-specific bits follow — the
//      record body is exactly 1 bit.
//   2) When suppress = 0, the §15.0.1 base-state AND §15.0.2 shape-layer
//      blocks ARE emitted (v1 elided the shape-layer block from the
//      Vehicle path).
//   3) The has-move sub-block has NO move-redundancy flag — the
//      Vehicle's player-move sub-block is written with no prev pointer,
//      which suppresses the redundancy bit.
//
// To keep this body self-contained the function reads the base-state and
// shape-layer itself. The dispatcher for Vehicle therefore must NOT
// pre-read base/shape (see parse_typed_packet).
bool read_vehicle_body(BitReader& br, GhostVehicle& v) {
    v.suppress_update = br.read_flag();
    if (v.suppress_update) return !br.overrun;

    // §15.0.1 base-state + §15.0.2 shape-layer (only when not suppressed).
    v.base = read_base_state(br);
    v.shape = read_shape_layer_block(br);
    if (br.overrun) return false;

    v.orientation_changed = br.read_flag();
    if (v.orientation_changed) {
        v.rot_x = br.read_float_unaligned();
        v.rot_y = br.read_float_unaligned();
        v.rot_z = br.read_float_unaligned();
        v.pos_x = br.read_float_unaligned();
        v.pos_y = br.read_float_unaligned();
        v.pos_z = br.read_float_unaligned();
        v.has_move = br.read_flag();
        if (v.has_move) {
            // §15.4 v3: no move-redundancy flag here.
            v.move_redundant = false;
            v.fwd_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            v.back_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            v.left_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            v.right_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
            v.jet_held = br.read_flag();
            v.jump_held = br.read_flag();
            v.crouch_held = br.read_flag();
            v.speed_fraction = static_cast<float>(br.read_bits(10)) / 1023.0f;
            v.lift = br.read_float_unaligned();
            v.skip_count = static_cast<std::uint8_t>(br.read_bits(4));
        }
    }

    v.status_changed = br.read_flag();
    if (v.status_changed) {
        v.current_speed = static_cast<float>(br.read_bits(10)) / 1023.0f;
        v.desired_speed = static_cast<float>(br.read_bits(10)) / 1023.0f;
    }
    v.fire_count_changed = br.read_flag();
    if (v.fire_count_changed) {
        v.fire_count = static_cast<std::uint8_t>(br.read_bits(2));
    }
    v.damage_changed = br.read_flag();
    if (v.damage_changed) {
        v.damage_level = static_cast<float>(br.read_bits(6)) / 63.0f;
    }
    v.sound_changed = br.read_flag();
    if (v.sound_changed) {
        v.sound_state = static_cast<std::uint8_t>(br.read_bits(2));
    }
    return !br.overrun;
}

// Re-implement the outer-framing scan from ghost_stream.cpp inline so we
// can produce the BitReader+offset that lets us continue walking the
// per-object loop past the first record.
//
// Returns std::nullopt if no plausible ghost-sub-stream start was found.
struct GhostStreamLocation {
    std::size_t bit_offset = 0;     // bit offset of the mode flag
    bool mode_scope_always = false;
    std::uint8_t id_width_bits = 0;
};

// (We re-use the framing parser's output rather than reimplementing the
// scan. parse_ghost_packet found the start of the ghost sub-stream as
// `updates[0].start_bit`. From that offset we read mode + selector + the
// first per-object record and continue.)

const char* kind_name(GhostClassKind k) {
    switch (k) {
        case GhostClassKind::Player:      return "player";
        case GhostClassKind::Projectile:  return "proj";
        case GhostClassKind::Item:        return "item";
        case GhostClassKind::Vehicle:     return "vehicle";
        case GhostClassKind::StaticShape: return "static";
        default:                          return "unknown";
    }
}

// Format a one-line log summary for a typed record. The caller's log path
// can prefix this with the packet index.
std::string format_record(const TypedRecord& rec,
                          const GhostRegistry& reg)
{
    std::ostringstream os;
    os << "[" << kind_name(rec.kind) << "]"
       << " ghost=" << rec.ghost_id;
    if (rec.full_update) {
        os << " tag=" << rec.class_tag;
    }
    if (rec.kill) {
        os << " KILL";
        return os.str();
    }
    char buf[160];
    switch (rec.kind) {
        case GhostClassKind::StaticShape: {
            const auto it = reg.statics.find(rec.ghost_id);
            if (it == reg.statics.end()) break;
            const auto& s = it->second;
            std::snprintf(buf, sizeof(buf),
                " pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f) team=%u",
                s.pos_x, s.pos_y, s.pos_z,
                s.rot_x, s.rot_y, s.rot_z,
                static_cast<unsigned>(s.base.team_id));
            os << buf;
            break;
        }
        case GhostClassKind::Player: {
            const auto it = reg.players.find(rec.ghost_id);
            if (it == reg.players.end()) break;
            const auto& p = it->second;
            std::snprintf(buf, sizeof(buf),
                " pos=(%.2f,%.2f,%.2f) yaw=%.3f energy=%.2f team=%u",
                p.pos_x, p.pos_y, p.pos_z, p.yaw, p.energy,
                static_cast<unsigned>(p.base.team_id));
            os << buf;
            break;
        }
        case GhostClassKind::Item: {
            const auto it = reg.items.find(rec.ghost_id);
            if (it == reg.items.end()) break;
            const auto& m = it->second;
            std::snprintf(buf, sizeof(buf),
                " pos=(%.2f,%.2f,%.2f) df=%u thrower=%u",
                m.pos_x, m.pos_y, m.pos_z,
                static_cast<unsigned>(m.item_data_file_id),
                m.has_thrower ? static_cast<unsigned>(m.thrower_ghost) : 0u);
            os << buf;
            break;
        }
        case GhostClassKind::Vehicle: {
            const auto it = reg.vehicles.find(rec.ghost_id);
            if (it == reg.vehicles.end()) break;
            const auto& v = it->second;
            std::snprintf(buf, sizeof(buf),
                " pos=(%.2f,%.2f,%.2f) rot=(%.2f,%.2f,%.2f) speed=%.2f",
                v.pos_x, v.pos_y, v.pos_z,
                v.rot_x, v.rot_y, v.rot_z, v.current_speed);
            os << buf;
            break;
        }
        case GhostClassKind::Projectile: {
            const auto it = reg.projectiles.find(rec.ghost_id);
            if (it == reg.projectiles.end()) break;
            const auto& pr = it->second;
            if (pr.initial_update) {
                std::snprintf(buf, sizeof(buf),
                    " df=%u init_pos=(%.2f,%.2f,%.2f) shooter=%u",
                    static_cast<unsigned>(pr.projectile_data_file_id),
                    pr.init_pos_x, pr.init_pos_y, pr.init_pos_z,
                    pr.has_shooter ? static_cast<unsigned>(pr.shooter_ghost) : 0u);
            } else {
                std::snprintf(buf, sizeof(buf),
                    " pos=(%.2f,%.2f,%.2f)",
                    pr.pos_x, pr.pos_y, pr.pos_z);
            }
            os << buf;
            break;
        }
        default: break;
    }
    return os.str();
}

}  // namespace

void GhostRegistry::install_default_class_tag_map() {
    // Capture-grounded defaults.
    //
    // Spec 14 (2026-05-22 spectator-only capture): five StaticShape tags
    // observed in scope-always intros.
    class_tag_kinds[96]  = GhostClassKind::StaticShape;
    class_tag_kinds[333] = GhostClassKind::StaticShape;
    class_tag_kinds[512] = GhostClassKind::StaticShape;
    class_tag_kinds[615] = GhostClassKind::StaticShape;
    class_tag_kinds[896] = GhostClassKind::StaticShape;
    // Spec 28 (2026-05-22 PvP capture pvp-session-20260522-202151.json):
    // discovery harness identified tag 960 as Player — 52 samples across
    // 14 distinct ghosts (matches a typical mid-size PvP match's roster),
    // bounded velocities, and the only candidate whose consecutive-pair
    // smoothness checks actually fired (evidence of real movement deltas).
    // Tags 560 (8 ghosts) and 80 (8 ghosts) co-passed the relaxed Player
    // probe but probably represent Vehicle/Projectile — they're left
    // unmapped pending a per-kind probe extension.
    class_tag_kinds[960] = GhostClassKind::Player;
}

void GhostRegistry::clear() {
    ghost_kinds.clear();
    ghost_class_tags.clear();
    players.clear();
    projectiles.clear();
    items.clear();
    vehicles.clear();
    statics.clear();
}

void apply_update(GhostRegistry& reg, const TypedRecord& rec) {
    if (rec.kill) {
        reg.ghost_kinds.erase(rec.ghost_id);
        reg.ghost_class_tags.erase(rec.ghost_id);
        reg.players.erase(rec.ghost_id);
        reg.projectiles.erase(rec.ghost_id);
        reg.items.erase(rec.ghost_id);
        reg.vehicles.erase(rec.ghost_id);
        reg.statics.erase(rec.ghost_id);
        return;
    }
    // Non-kill updates were already merged into the relevant table by
    // parse_typed_packet (which had the in-flight struct). This function
    // is mainly the kill-side handler + the registration of kind/tag.
    reg.ghost_kinds[rec.ghost_id] = rec.kind;
    if (rec.full_update) {
        reg.ghost_class_tags[rec.ghost_id] = rec.class_tag;
    }
}

TypedPacketDecode parse_typed_packet(const std::uint8_t* data,
                                     std::size_t length,
                                     GhostRegistry& registry)
{
    TypedPacketDecode out;
    // Spec 29: pass the registry so the scanner can accept normal-mode
    // delta candidates whose first ghost_id is already known.
    out.framing = parse_ghost_packet(data, length, &registry);
    if (!out.framing.ghost_stream_start_bit.has_value()) {
        out.note = out.framing.note.empty()
                   ? std::string("no ghost stream found")
                   : out.framing.note;
        return out;
    }
    const std::size_t ghost_start_bit = *out.framing.ghost_stream_start_bit;

    BitReader br(data, length);
    br.pos = ghost_start_bit;

    // mode + selector are part of the framing parser's "first candidate"
    // discovery but were not advanced past — re-read them here so our
    // cursor lands at the first object-present bit.
    const bool mode_scope_always = br.read_flag();
    const std::uint32_t selector = br.read_bits(3);
    const unsigned id_width = static_cast<unsigned>(selector + 3);
    if (br.overrun || id_width < 3 || id_width > 10) {
        out.note = "mode/selector read overran";
        return out;
    }

    // Walk the per-object loop. Each iteration:
    //   1-bit object-present; if 0: in scope-always mode read trailing
    //     "complete" flag and stop.
    //   idW-bit ghost id
    //   1-bit kill
    //   if scope-always-mode AND ghost-id was-not-seen-before:
    //     32-bit object id + 10-bit class tag
    //   per-class payload (decoded by kind)
    int safety_max_records = 256;   // hard cap to avoid runaway scans
    while (safety_max_records-- > 0) {
        const std::size_t rec_start_bit = br.pos;
        const bool present = br.read_flag();
        if (br.overrun) {
            out.note = "object-present read overran";
            return out;
        }
        if (!present) {
            if (mode_scope_always) {
                (void)br.read_flag();  // scope-always-complete bit
            }
            out.walked_full_stream = true;
            return out;
        }

        if (br.pos + id_width + 1 > br.bit_length) {
            out.note = "id/kill read would overrun";
            return out;
        }
        const std::uint32_t ghost_id = br.read_bits(id_width);
        const bool kill = br.read_flag();

        TypedRecord rec;
        rec.ghost_id = static_cast<std::uint16_t>(ghost_id);
        rec.kill = kill;
        rec.start_bit = rec_start_bit;
        rec.kind = GhostClassKind::Unknown;

        if (kill) {
            // No further fields for this object per §5.0.3.
            apply_update(registry, rec);
            rec.end_bit = br.pos;
            rec.log_line = "[" + std::string(kind_name(rec.kind))
                         + "] ghost=" + std::to_string(rec.ghost_id)
                         + " KILL";
            out.records.push_back(std::move(rec));
            continue;
        }

        // Per §5.0.3: a 32-bit object number + 10-bit class tag is emitted
        // for "ghost identifiers that have not been seen before". In
        // scope-always mode the per-connection mirror table is being populated
        // by definition, so every record IS a new introduction — even if the
        // 10-bit ghost_id slot happens to collide with a slot we tagged
        // earlier (the server's pool reused it because the previous occupant
        // was killed). In normal-delta mode `full_update` is true only when
        // the receiver has no prior knowledge of this slot.
        const bool seen_before = registry.ghost_kinds.count(rec.ghost_id) > 0;
        const bool full_update = mode_scope_always || !seen_before;
        rec.full_update = full_update;

        if (full_update) {
            if (br.pos + 32 + 10 > br.bit_length) {
                out.note = "obj_id/class_tag read would overrun";
                return out;
            }
            const std::uint32_t obj_id = br.read_bits(32);
            const std::uint16_t class_tag =
                static_cast<std::uint16_t>(br.read_bits(10));
            if (class_tag == 0 || class_tag > 1023) {
                out.note = "class_tag out of range (1..1023)";
                return out;
            }
            rec.class_tag = class_tag;

            // Look up kind for this class tag (caller-supplied). Unknown
            // tags default to StaticShape — see §15.5.1.
            auto kit = registry.class_tag_kinds.find(class_tag);
            rec.kind = (kit != registry.class_tag_kinds.end())
                       ? kit->second : GhostClassKind::StaticShape;

            // Decode the per-class payload, seeding the registry's typed
            // table on success.
            const std::uint8_t kDataFileWidth = 8;  // §15.1 / §15.2 / §15.3
            bool ok = false;
            switch (rec.kind) {
                case GhostClassKind::StaticShape: {
                    GhostStaticShape s;
                    s.ghost_id = rec.ghost_id;
                    s.object_id = obj_id;
                    s.class_tag = class_tag;
                    s.base = read_base_state(br);
                    s.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_static_shape_body(br, s);
                    if (ok) registry.statics[rec.ghost_id] = std::move(s);
                    break;
                }
                case GhostClassKind::Player: {
                    GhostPlayer p;
                    p.ghost_id = rec.ghost_id;
                    p.object_id = obj_id;
                    p.class_tag = class_tag;
                    p.base = read_base_state(br);
                    p.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_player_body(br, p, kDataFileWidth);
                    if (ok) registry.players[rec.ghost_id] = std::move(p);
                    break;
                }
                case GhostClassKind::Item: {
                    GhostItem it;
                    it.ghost_id = rec.ghost_id;
                    it.object_id = obj_id;
                    it.class_tag = class_tag;
                    it.base = read_base_state(br);
                    it.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_item_body(br, it, kDataFileWidth);
                    if (ok) registry.items[rec.ghost_id] = std::move(it);
                    break;
                }
                case GhostClassKind::Projectile: {
                    GhostProjectile pr;
                    pr.ghost_id = rec.ghost_id;
                    pr.object_id = obj_id;
                    pr.class_tag = class_tag;
                    pr.base = read_base_state(br);
                    pr.shape = read_shape_layer_block(br);
                    ok = !br.overrun
                         && read_projectile_body(br, pr, kDataFileWidth);
                    if (ok) registry.projectiles[rec.ghost_id] = std::move(pr);
                    break;
                }
                case GhostClassKind::Vehicle: {
                    // §15.4 v3: Vehicle has a suppress-update flag FIRST
                    // that gates §15.0.1 base + §15.0.2 shape. Let the
                    // Vehicle body reader own the entire payload.
                    GhostVehicle v;
                    v.ghost_id = rec.ghost_id;
                    v.object_id = obj_id;
                    v.class_tag = class_tag;
                    ok = read_vehicle_body(br, v);
                    if (ok) registry.vehicles[rec.ghost_id] = std::move(v);
                    break;
                }
                default:
                    out.note = "unknown class kind for tag";
                    return out;
            }
            if (!ok) {
                out.note = "per-class introduction payload overran";
                rec.end_bit = br.pos;
                out.records.push_back(std::move(rec));
                return out;
            }

            // Apply / register the kind+tag for future delta lookups.
            apply_update(registry, rec);
        } else {
            // Delta update: look up the previously-recorded kind for this
            // ghost; if we have no record (e.g. mid-stream introduction we
            // missed) abort decoding past this record so we don't corrupt
            // the bit cursor.
            auto kit = registry.ghost_kinds.find(rec.ghost_id);
            if (kit == registry.ghost_kinds.end()) {
                out.note = "delta update for unknown ghost_id";
                rec.end_bit = br.pos;
                out.records.push_back(std::move(rec));
                return out;
            }
            rec.kind = kit->second;
            auto ctit = registry.ghost_class_tags.find(rec.ghost_id);
            if (ctit != registry.ghost_class_tags.end()) {
                rec.class_tag = ctit->second;
            }
            const std::uint8_t kDataFileWidth = 8;
            bool ok = false;
            switch (rec.kind) {
                case GhostClassKind::StaticShape: {
                    auto& s = registry.statics[rec.ghost_id];
                    s.ghost_id = rec.ghost_id;
                    s.base = read_base_state(br);
                    s.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_static_shape_body(br, s);
                    break;
                }
                case GhostClassKind::Player: {
                    auto& p = registry.players[rec.ghost_id];
                    p.ghost_id = rec.ghost_id;
                    p.base = read_base_state(br);
                    p.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_player_body(br, p, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Item: {
                    auto& it = registry.items[rec.ghost_id];
                    it.ghost_id = rec.ghost_id;
                    it.base = read_base_state(br);
                    it.shape = read_shape_layer_block(br);
                    ok = !br.overrun && read_item_body(br, it, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Projectile: {
                    auto& pr = registry.projectiles[rec.ghost_id];
                    pr.ghost_id = rec.ghost_id;
                    pr.base = read_base_state(br);
                    pr.shape = read_shape_layer_block(br);
                    ok = !br.overrun
                         && read_projectile_body(br, pr, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Vehicle: {
                    // §15.4 v3: same as the full-update path — suppress
                    // flag gates base + shape, so read_vehicle_body owns
                    // the entire payload.
                    auto& v = registry.vehicles[rec.ghost_id];
                    v.ghost_id = rec.ghost_id;
                    ok = read_vehicle_body(br, v);
                    break;
                }
                default:
                    out.note = "delta for unknown kind";
                    return out;
            }
            if (!ok) {
                out.note = "per-class delta payload overran";
                rec.end_bit = br.pos;
                out.records.push_back(std::move(rec));
                return out;
            }
        }
        rec.end_bit = br.pos;
        rec.log_line = format_record(rec, registry);
        out.records.push_back(std::move(rec));
    }
    out.note = "hit record cap (256)";
    return out;
}

}  // namespace net20
