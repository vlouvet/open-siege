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

    // Read a 32-bit IEEE-754 LE float, byte-aligned (per §15 conventions:
    // "32-bit float byte-aligned" = align then read 4 LE bytes).
    float read_float_aligned() noexcept {
        align_to_byte();
        if (pos + 32 > bit_length) { overrun = true; return 0.0f; }
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= static_cast<std::uint32_t>(data[(pos >> 3) + i]) << (i * 8);
        }
        pos += 32;
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
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

// §15.5 StaticShape per-class payload (post-base-state). Returns true on a
// clean decode; false on overrun.
//
// Per §15.5 the StaticShape body is laid out as:
//   transform-changed flag, optional 96+96-bit position+rotation
//   damage-changed flag, optional 1+1+8 bits
//   info-changed flag, optional 1-bit is-target
//   shape-info-changed flag, optional 8-bit data-file id + sensor-key block
//
// This function decodes the union of "plain static + shape variant" — i.e.
// it always reads the shape-info block. The scope-always burst's static
// objects in the 2026-05-22 capture are all shape-bearing variants.
bool read_static_shape_body(BitReader& br, GhostStaticShape& s) {
    s.transform_changed = br.read_flag();
    if (s.transform_changed) {
        s.pos_x = br.read_float_aligned();
        s.pos_y = br.read_float_aligned();
        s.pos_z = br.read_float_aligned();
        s.rot_x = br.read_float_aligned();
        s.rot_y = br.read_float_aligned();
        s.rot_z = br.read_float_aligned();
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
        // Signed 5-bit normalised float * max-pitch (88° in radians).
        const std::int32_t signed5 = br.read_signed(5);
        constexpr float kMaxPitch = 88.0f * 3.14159265358979323846f / 180.0f;
        p.view_pitch = (static_cast<float>(signed5) / 15.0f) * kMaxPitch;

        // Full IEEE-754 position (96 bits byte-aligned).
        p.pos_x = br.read_float_aligned();
        p.pos_y = br.read_float_aligned();
        p.pos_z = br.read_float_aligned();

        p.has_velocity = br.read_flag();
        if (p.has_velocity) {
            const std::uint32_t mag_bits = br.read_bits(17);
            const float mag = static_cast<float>(mag_bits) / 512.0f;
            // Normal-vector of widths (10, 10).
            const std::int32_t z_signed = br.read_signed(10);
            const std::uint32_t az_bits = br.read_bits(10);
            const float z = static_cast<float>(z_signed) / 511.0f;
            const float az = static_cast<float>(az_bits) / 1023.0f;
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float angle = az * 2.0f * 3.14159265358979323846f;
            const float x_dir = r * std::cos(angle);
            const float y_dir = r * std::sin(angle);
            p.velocity_x = x_dir * mag;
            p.velocity_y = y_dir * mag;
            p.velocity_z = z * mag;
        }

        p.on_ground = br.read_flag();
        const std::uint32_t yaw_bits = br.read_bits(9);
        p.yaw = static_cast<float>(yaw_bits) * (2.0f * 3.14159265358979323846f / 511.0f);

        if (!p.initial_update) {
            p.has_move_block = true;
            p.move_redundant = br.read_flag();
            if (!p.move_redundant) {
                p.fwd_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                p.back_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                p.left_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                p.right_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                p.jet_held = br.read_flag();
                p.jump_held = br.read_flag();
                p.crouch_held = br.read_flag();
            }
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
        p.recharge_rate = br.read_float_aligned();
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
        it.pos_x = br.read_float_aligned();
        it.pos_y = br.read_float_aligned();
        it.pos_z = br.read_float_aligned();
    }
    it.at_rest_or_rotates = br.read_flag();
    if (!it.at_rest_or_rotates) {
        it.velocity_changed = br.read_flag();
        if (it.velocity_changed) {
            it.velocity_x = br.read_float_aligned();
            it.velocity_y = br.read_float_aligned();
            it.velocity_z = br.read_float_aligned();
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
        pr.init_pos_x = br.read_float_aligned();
        pr.init_pos_y = br.read_float_aligned();
        pr.init_pos_z = br.read_float_aligned();
        pr.init_vel_x = br.read_float_aligned();
        pr.init_vel_y = br.read_float_aligned();
        pr.init_vel_z = br.read_float_aligned();
    } else {
        pr.position_changed = br.read_flag();
        if (pr.position_changed) {
            pr.pos_x = br.read_float_aligned();
            pr.pos_y = br.read_float_aligned();
            pr.pos_z = br.read_float_aligned();
        }
        pr.velocity_full = br.read_flag();
        if (pr.velocity_full) {
            pr.velocity_x = br.read_float_aligned();
            pr.velocity_y = br.read_float_aligned();
            pr.velocity_z = br.read_float_aligned();
        } else {
            // Compressed velocity direction (9,9 normal-vector).
            const std::int32_t z_signed = br.read_signed(9);
            const std::uint32_t az_bits = br.read_bits(9);
            const float z = static_cast<float>(z_signed) / 255.0f;
            const float az = static_cast<float>(az_bits) / 511.0f;
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float angle = az * 2.0f * 3.14159265358979323846f;
            pr.velocity_dir_x = r * std::cos(angle);
            pr.velocity_dir_y = r * std::sin(angle);
            pr.velocity_dir_z = z;
        }
        pr.collision_sound_changed = br.read_flag();
        if (pr.collision_sound_changed) {
            pr.surface_material = static_cast<std::uint8_t>(br.read_bits(4));
        }
    }
    return !br.overrun;
}

// §15.4 Vehicle.
bool read_vehicle_body(BitReader& br, GhostVehicle& v) {
    v.suppress_update = br.read_flag();
    if (v.suppress_update) return true;

    v.orientation_changed = br.read_flag();
    if (v.orientation_changed) {
        v.rot_x = br.read_float_aligned();
        v.rot_y = br.read_float_aligned();
        v.rot_z = br.read_float_aligned();
        v.pos_x = br.read_float_aligned();
        v.pos_y = br.read_float_aligned();
        v.pos_z = br.read_float_aligned();
        v.has_move = br.read_flag();
        if (v.has_move) {
            v.move_redundant = br.read_flag();
            if (!v.move_redundant) {
                v.fwd_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                v.back_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                v.left_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                v.right_axis = static_cast<float>(br.read_bits(4)) / 15.0f;
                v.jet_held = br.read_flag();
                v.jump_held = br.read_flag();
                v.crouch_held = br.read_flag();
            }
            v.speed_fraction = static_cast<float>(br.read_bits(10)) / 1023.0f;
            v.lift = br.read_float_aligned();
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
    // Capture-grounded defaults from `groove-session-20260522-100230.json`
    // per §15.5.1: all five observed scope-always class tags resolve to
    // "Mission-pinned static" — i.e. StaticShape.
    class_tag_kinds[96]  = GhostClassKind::StaticShape;
    class_tag_kinds[333] = GhostClassKind::StaticShape;
    class_tag_kinds[512] = GhostClassKind::StaticShape;
    class_tag_kinds[615] = GhostClassKind::StaticShape;
    class_tag_kinds[896] = GhostClassKind::StaticShape;
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
    out.framing = parse_ghost_packet(data, length);
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
                    ok = !br.overrun
                         && read_projectile_body(br, pr, kDataFileWidth);
                    if (ok) registry.projectiles[rec.ghost_id] = std::move(pr);
                    break;
                }
                case GhostClassKind::Vehicle: {
                    GhostVehicle v;
                    v.ghost_id = rec.ghost_id;
                    v.object_id = obj_id;
                    v.class_tag = class_tag;
                    v.base = read_base_state(br);
                    ok = !br.overrun && read_vehicle_body(br, v);
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
                    ok = !br.overrun && read_static_shape_body(br, s);
                    break;
                }
                case GhostClassKind::Player: {
                    auto& p = registry.players[rec.ghost_id];
                    p.ghost_id = rec.ghost_id;
                    p.base = read_base_state(br);
                    ok = !br.overrun && read_player_body(br, p, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Item: {
                    auto& it = registry.items[rec.ghost_id];
                    it.ghost_id = rec.ghost_id;
                    it.base = read_base_state(br);
                    ok = !br.overrun && read_item_body(br, it, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Projectile: {
                    auto& pr = registry.projectiles[rec.ghost_id];
                    pr.ghost_id = rec.ghost_id;
                    pr.base = read_base_state(br);
                    ok = !br.overrun
                         && read_projectile_body(br, pr, kDataFileWidth);
                    break;
                }
                case GhostClassKind::Vehicle: {
                    auto& v = registry.vehicles[rec.ghost_id];
                    v.ghost_id = rec.ghost_id;
                    v.base = read_base_state(br);
                    ok = !br.overrun && read_vehicle_body(br, v);
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
