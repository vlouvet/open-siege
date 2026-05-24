// Spec 28/04b — real T1-byte-compatible ghost-stream encoder.
// Every helper here is the inverse of the matching read_* function in
// engine/src/ghost_types.cpp. See ghost_encoder.hpp for the wire-format
// reference.

#include "ghost_encoder.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net20 {

// ----- Bit-level helpers ------------------------------------------------

void write_float_unaligned(BitWriter& w, float f)
{
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    w.write_bits(bits, 32);
}

void write_signed_float(BitWriter& w, float f, unsigned width)
{
    if (width == 0 || width > 32) return;
    if (width == 1) { w.write_bits(0u, 1); return; }
    // Clamp to [-1, +1] then rescale to [0, 2^width - 1].
    float c = f;
    if (c < -1.0f) c = -1.0f;
    if (c >  1.0f) c =  1.0f;
    const float denom = static_cast<float>((1u << width) - 1u);
    const float scaled = ((c + 1.0f) * 0.5f) * denom;
    // Round to nearest, clamp into the unsigned range.
    std::int64_t raw = static_cast<std::int64_t>(std::lround(scaled));
    if (raw < 0) raw = 0;
    if (raw > static_cast<std::int64_t>(denom)) raw = static_cast<std::int64_t>(denom);
    w.write_bits(static_cast<std::uint32_t>(raw), width);
}

void write_normal_vec_form_a(BitWriter& w, unsigned component_bits,
                             float x, float y, float z)
{
    write_signed_float(w, x, component_bits);
    write_signed_float(w, y, component_bits);
    w.write_flag(z < 0.0f);
}

// ----- §15.0.1 base-state block ----------------------------------------

void write_base_state(BitWriter& w, const GhostBaseState& s)
{
    w.write_flag(s.base_changed);
    if (!s.base_changed) return;
    w.write_bits(static_cast<std::uint32_t>(s.team_id & 0x1Fu), 5);
    w.write_flag(s.has_control_client);
    if (s.has_control_client) {
        const std::uint16_t off = static_cast<std::uint16_t>(s.control_client_id - 2048u);
        w.write_bits(off & 0x7Fu, 7);
    }
    w.write_flag(s.has_owner_client);
    if (s.has_owner_client) {
        const std::uint16_t off = static_cast<std::uint16_t>(s.owner_client_id - 2048u);
        w.write_bits(off & 0x7Fu, 7);
    }
}

// ----- §15.0.2 shape-layer block ---------------------------------------

void write_shape_layer_block(BitWriter& w, const GhostShapeLayer& s)
{
    // Three "changed" flags, then sub-payloads. The common case is
    // three zero bits (no shape changes). Inverse of read_shape_layer_block.
    w.write_flag(s.shield_changed);
    if (s.shield_changed) {
        // §15.0.2 v3: 15-bit shield direction via §6.8 Form A (2*7 + 1).
        write_normal_vec_form_a(w, 7, s.shield_dir_x, s.shield_dir_y, s.shield_dir_z);
        // 8-bit z-offset normalised: wire = (z_off_m + 5) / 10 * 255.
        float zoff_norm = (s.shield_z_offset + 5.0f) / 10.0f;
        if (zoff_norm < 0.0f) zoff_norm = 0.0f;
        if (zoff_norm > 1.0f) zoff_norm = 1.0f;
        const std::uint32_t zoff_bits =
            static_cast<std::uint32_t>(std::lround(zoff_norm * 255.0f));
        w.write_bits(zoff_bits & 0xFFu, 8);
    }

    w.write_flag(s.thread_changed);
    if (s.thread_changed) {
        constexpr unsigned kSeqW = 6;
        for (const auto& slot : s.threads) {
            w.write_flag(slot.present);
            if (!slot.present) continue;
            w.write_bits(slot.sequence_id & 0x3Fu, kSeqW);
            w.write_bits(static_cast<std::uint32_t>(slot.state & 0x3u), 2);
            w.write_flag(slot.forward);
            w.write_flag(slot.at_end);
        }
    }

    w.write_flag(s.fade_changed);
    if (s.fade_changed) {
        w.write_flag(s.fade_in);
    }
}

// ----- §15.5 StaticShape body ------------------------------------------

void write_static_shape_body(BitWriter& w, const GhostStaticShape& s)
{
    w.write_flag(s.transform_changed);
    if (s.transform_changed) {
        write_float_unaligned(w, s.pos_x);
        write_float_unaligned(w, s.pos_y);
        write_float_unaligned(w, s.pos_z);
        write_float_unaligned(w, s.rot_x);
        write_float_unaligned(w, s.rot_y);
        write_float_unaligned(w, s.rot_z);
    }

    w.write_flag(s.damage_changed);
    if (s.damage_changed) {
        w.write_flag(s.state_enabled);
        if (!s.state_enabled) {
            w.write_flag(s.state_disabled);
        }
        float dl = s.damage_level;
        if (dl < 0.0f) dl = 0.0f;
        if (dl > 1.0f) dl = 1.0f;
        const std::uint32_t bits =
            static_cast<std::uint32_t>(std::lround(dl * 255.0f));
        w.write_bits(bits & 0xFFu, 8);
    }

    w.write_flag(s.info_changed);
    if (s.info_changed) {
        w.write_flag(s.is_target);
    }

    w.write_flag(s.shape_info_changed);
    if (s.shape_info_changed) {
        w.write_bits(s.shape_data_file_id, 8);
        w.write_flag(s.has_sensor_key);
        if (s.has_sensor_key) {
            w.write_bits(s.sensor_key & 0x7Fu, 7);
        }
    }
}

// ----- §15.1 Player body -----------------------------------------------

void write_player_body(BitWriter& w, const GhostPlayer& p,
                       std::uint8_t datafile_id_width)
{
    w.write_flag(p.initial_update);
    if (p.initial_update) {
        w.write_bits(p.datafile_id, datafile_id_width);
        w.write_flag(p.ai_controlled);
    }

    w.write_flag(p.mount_changed);
    if (p.mount_changed) {
        w.write_flag(p.has_mount);
        if (p.has_mount) {
            w.write_bits(p.mount_target_ghost & 0x3FFu, 10);
            w.write_bits(p.mount_point & 0x1Fu, 5);
        }
    }

    w.write_flag(p.damage_changed);
    if (p.damage_changed) {
        w.write_flag(p.dead);
        if (!p.dead) {
            float dl = p.damage_level;
            if (dl < 0.0f) dl = 0.0f;
            if (dl > 1.0f) dl = 1.0f;
            const std::uint32_t bits =
                static_cast<std::uint32_t>(std::lround(dl * 63.0f));
            w.write_bits(bits & 0x3Fu, 6);
        } else {
            w.write_flag(p.blown_up);
            if (!p.blown_up) {
                w.write_bits(p.death_anim_index & 0x3Fu, 6);
            }
        }
    }

    w.write_flag(p.has_pos_block);
    if (p.has_pos_block) {
        // §15.1 v3 signed 5-bit normalised view_pitch * max-pitch (88° rad).
        constexpr float kMaxPitch = 88.0f * 3.14159265358979323846f / 180.0f;
        const float view_pitch_norm = (kMaxPitch == 0.0f) ? 0.0f
                                      : (p.view_pitch / kMaxPitch);
        write_signed_float(w, view_pitch_norm, 5);

        write_float_unaligned(w, p.pos_x);
        write_float_unaligned(w, p.pos_y);
        write_float_unaligned(w, p.pos_z);

        w.write_flag(p.has_velocity);
        if (p.has_velocity) {
            // Encode magnitude as 17-bit unsigned scaled to /512.0.
            const float mag = std::sqrt(p.velocity_x * p.velocity_x
                                      + p.velocity_y * p.velocity_y
                                      + p.velocity_z * p.velocity_z);
            float mag_scaled = mag * 512.0f;
            if (mag_scaled < 0.0f) mag_scaled = 0.0f;
            if (mag_scaled > 131071.0f) mag_scaled = 131071.0f;  // 17 bits max
            const std::uint32_t mag_bits =
                static_cast<std::uint32_t>(std::lround(mag_scaled));
            w.write_bits(mag_bits & 0x1FFFFu, 17);

            // §15.1 v3: velocity direction is Form A normal-vector,
            // bits=10. Direction = velocity / |velocity| (or (0,0,0)
            // when mag is zero — encoder writes whatever the receiver
            // would multiply by mag=0, so direction values are
            // irrelevant in that case).
            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            if (mag > 1e-6f) {
                dx = p.velocity_x / mag;
                dy = p.velocity_y / mag;
                dz = p.velocity_z / mag;
            }
            write_normal_vec_form_a(w, 10, dx, dy, dz);
        }

        w.write_flag(p.on_ground);
        // yaw: 9-bit, wire = round(yaw / (2π / 511)).
        float yaw = p.yaw;
        constexpr float k2Pi = 2.0f * 3.14159265358979323846f;
        // Normalise into [0, 2π).
        yaw = std::fmod(yaw, k2Pi);
        if (yaw < 0.0f) yaw += k2Pi;
        const std::uint32_t yaw_bits =
            static_cast<std::uint32_t>(std::lround(yaw / (k2Pi / 511.0f)));
        w.write_bits(yaw_bits & 0x1FFu, 9);

        if (!p.initial_update) {
            // §15.1 v3 correction: NO move-redundancy flag.
            auto axis_bits = [](float a) -> std::uint32_t {
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                return static_cast<std::uint32_t>(std::lround(a * 15.0f)) & 0xFu;
            };
            w.write_bits(axis_bits(p.fwd_axis),   4);
            w.write_bits(axis_bits(p.back_axis),  4);
            w.write_bits(axis_bits(p.left_axis),  4);
            w.write_bits(axis_bits(p.right_axis), 4);
            w.write_flag(p.jet_held);
            w.write_flag(p.jump_held);
            w.write_flag(p.crouch_held);
            float e = p.energy;
            if (e < 0.0f) e = 0.0f;
            if (e > 1.0f) e = 1.0f;
            w.write_bits(static_cast<std::uint32_t>(std::lround(e * 127.0f)) & 0x7Fu, 7);
            w.write_bits(p.skip_count & 0xFu, 4);
            w.write_flag(p.no_interp);
        }
    }

    if (!p.initial_update) {
        w.write_flag(p.anim_changed);
        if (p.anim_changed) {
            w.write_bits(p.anim_index & 0x3Fu, 6);
        }
    }

    w.write_flag(p.recharge_changed);
    if (p.recharge_changed) {
        write_float_unaligned(w, p.recharge_rate);
    }

    w.write_flag(p.pda_mode);
    w.write_flag(p.crouch_state);

    w.write_flag(p.inventory_changed);
    if (p.inventory_changed) {
        for (const auto& slot : p.inventory) {
            w.write_flag(slot.changed);
            if (!slot.changed) continue;
            // Wire 8-bit = type_id + 1, with 0 representing "no item".
            const std::uint32_t raw = slot.item_type_id == 0
                ? 0u : static_cast<std::uint32_t>(slot.item_type_id + 1u);
            w.write_bits(raw & 0xFFu, 8);
            w.write_flag(slot.has_team_tint);
            if (slot.has_team_tint) {
                w.write_bits(slot.team_id & 0x7u, 3);
            }
            w.write_flag(slot.trigger_down);
            w.write_flag(slot.ammo_present);
            w.write_bits(slot.fire_count & 0x7u, 3);
            if (p.initial_update) {
                w.write_flag(slot.initial_state_is_fire);
            }
        }
    }
}

// ----- Outer-packet encoders -------------------------------------------

namespace {

// Write a single typed record's outer bits + per-class body. Mirrors
// the dispatch loop in parse_typed_packet.
//
// `mode_scope_always` is the packet-wide mode flag; combined with the
// record's `full_update` it determines whether obj_id + class_tag bits
// must appear before the body. (In scope-always mode every record is
// treated as a full update by the receiver regardless of full_update.)
void write_one_record(BitWriter& w,
                      const TypedRecordOut& rec,
                      std::uint8_t id_width_bits,
                      bool mode_scope_always)
{
    w.write_flag(true);                                  // object-present = 1
    w.write_bits(rec.ghost_id & ((1u << id_width_bits) - 1u), id_width_bits);
    w.write_flag(rec.kill);
    if (rec.kill) return;

    const bool emit_intro = mode_scope_always || rec.full_update;

    if (emit_intro) {
        w.write_bits(rec.object_id, 32);
        w.write_bits(static_cast<std::uint32_t>(rec.class_tag) & 0x3FFu, 10);
    }

    // Per-class body — match the dispatcher in parse_typed_packet.
    switch (rec.kind) {
        case GhostClassKind::Player: {
            // Player intro: base + shape + body(initial_update=1).
            // Player delta: base + shape + body(initial_update=0).
            if (!rec.player) return;
            GhostPlayer p = *rec.player;
            p.initial_update = emit_intro;
            write_base_state(w, p.base);
            write_shape_layer_block(w, p.shape);
            write_player_body(w, p, /*datafile_id_width*/ 8);
            break;
        }
        case GhostClassKind::StaticShape: {
            if (!rec.statics) return;
            const GhostStaticShape& s = *rec.statics;
            write_base_state(w, s.base);
            write_shape_layer_block(w, s.shape);
            write_static_shape_body(w, s);
            break;
        }
        default:
            // Item / Projectile / Vehicle bodies are 28/04c.
            return;
    }
}

std::vector<std::uint8_t> encode_packet_common(
    const VcHeaderInputs& hdr,
    const std::vector<TypedRecordOut>& records,
    std::uint8_t id_width_bits,
    bool mode_scope_always)
{
    if (id_width_bits < 3) id_width_bits = 3;
    if (id_width_bits > 10) id_width_bits = 10;

    BitWriter w;

    // VC header — write bits directly so we stay bit-cursor-aligned
    // with the rate prefix that follows (encode_vc_header returns a
    // byte vector + leaves us at a byte boundary, which would forbid
    // the rate-prefix flag bits from sitting at the right bit offset).
    w.write_flag(true);                                      // VC discriminator
    w.write_flag(hdr.connect_parity);                        // parity
    w.write_bits(static_cast<std::uint32_t>(hdr.send_seq & 0x1FFu), 9);
    w.write_bits(static_cast<std::uint32_t>(hdr.highest_acked_of_mine & 0x1Fu), 5);
    for (const AckRun& r : hdr.ack_runs) {
        const std::uint8_t len = r.length == 0 ? 1
                              : (r.length > 7 ? 7 : r.length);
        w.write_bits(len, 3);
        w.write_bits(r.start_seq & 0x1Fu, 5);
    }
    w.write_bits(0u, 3);                                     // ack-list terminator
    w.write_bits(static_cast<std::uint32_t>(hdr.type_word & 0x1Fu), 5);

    // Rate-control prefix (no changes in v1).
    w.write_flag(false);          // rate_changed = 0
    w.write_flag(false);          // max_rate_changed = 0

    // Sub-stream-present flags.
    w.write_flag(false);          // event-sub-stream-present = 0
    w.write_flag(false);          // input-sub-stream-present = 0
    w.write_flag(true);           // ghost-sub-stream-present = 1

    // Ghost sub-stream body.
    w.write_flag(mode_scope_always);
    const unsigned selector = static_cast<unsigned>(id_width_bits) - 3u;
    w.write_bits(selector & 0x7u, 3);

    for (const auto& rec : records) {
        write_one_record(w, rec, id_width_bits, mode_scope_always);
    }

    // Loop terminator: object-present = 0. In scope-always mode the
    // decoder also reads 1 extra "complete" flag, so we must emit it.
    w.write_flag(false);
    if (mode_scope_always) w.write_flag(true);

    return std::move(w.bytes);
}

}  // namespace

std::vector<std::uint8_t> encode_scope_always_burst(
    const VcHeaderInputs& hdr,
    const std::vector<TypedRecordOut>& records,
    std::uint8_t id_width_bits)
{
    return encode_packet_common(hdr, records, id_width_bits,
                                /*mode_scope_always*/ true);
}

std::vector<std::uint8_t> encode_normal_delta(
    const VcHeaderInputs& hdr,
    const std::vector<TypedRecordOut>& records,
    std::uint8_t id_width_bits)
{
    return encode_packet_common(hdr, records, id_width_bits,
                                /*mode_scope_always*/ false);
}

// ----- Round-trip selftest ---------------------------------------------

int ghost_encoder_roundtrip_selftest()
{
    // Build a synthetic Player + StaticShape with non-default fields.
    GhostPlayer p_in;
    p_in.ghost_id    = 7;
    p_in.object_id   = 0xDEADBEEF;
    p_in.class_tag   = 960;        // Player tag from spec 28 capture
    p_in.base.base_changed = true;
    p_in.base.team_id      = 3;
    p_in.has_pos_block = true;
    p_in.pos_x = 12.5f;
    p_in.pos_y = 7.25f;
    p_in.pos_z = -33.75f;
    p_in.view_pitch  = 0.0f;       // 0 is exact in the signed-float quant
    p_in.has_velocity = false;
    p_in.on_ground    = true;
    p_in.yaw = 1.5f;               // ~86°
    p_in.recharge_changed = false;
    p_in.pda_mode    = false;
    p_in.crouch_state = false;
    p_in.inventory_changed = false;

    GhostStaticShape s_in;
    s_in.ghost_id   = 42;
    s_in.object_id  = 0x12345678;
    s_in.class_tag  = 333;          // StaticShape tag from spec 14
    s_in.base.base_changed = true;
    s_in.base.team_id      = 0;
    s_in.transform_changed = true;
    s_in.pos_x = -100.0f; s_in.pos_y = 5.5f; s_in.pos_z = 250.25f;
    s_in.rot_x = 0.0f;    s_in.rot_y = 0.5f; s_in.rot_z = 0.0f;
    s_in.shape_info_changed = true;
    s_in.shape_data_file_id = 17;

    TypedRecordOut player_rec;
    player_rec.kind        = GhostClassKind::Player;
    player_rec.ghost_id    = p_in.ghost_id;
    player_rec.full_update = true;
    player_rec.object_id   = p_in.object_id;
    player_rec.class_tag   = p_in.class_tag;
    player_rec.player      = &p_in;

    TypedRecordOut static_rec;
    static_rec.kind        = GhostClassKind::StaticShape;
    static_rec.ghost_id    = s_in.ghost_id;
    static_rec.full_update = true;
    static_rec.object_id   = s_in.object_id;
    static_rec.class_tag   = s_in.class_tag;
    static_rec.statics     = &s_in;

    VcHeaderInputs hdr;
    hdr.send_seq             = 1;
    hdr.connect_parity       = false;
    hdr.highest_acked_of_mine = 0;
    hdr.type_word            = pkt_type::kDataPacket;

    // Encode scope-always burst with both records.
    const auto bytes = encode_scope_always_burst(
        hdr, {player_rec, static_rec}, /*id_width_bits*/ 10);
    std::fprintf(stderr,
        "[ghost-encoder-selftest] scope-always packet = %zu bytes\n",
        bytes.size());

    GhostRegistry reg;
    reg.install_default_class_tag_map();
    // VC header (24 bits, no acks) + rate prefix (2 bits) + sub-stream
    // presence (event=0, input=0, ghost=1, 3 bits) = 29 bits. The ghost
    // sub-stream body (mode + selector + records) starts at bit 29.
    constexpr std::size_t kKnownGhostStartBit = 29;
    auto decoded = parse_typed_packet_at_offset(
        bytes.data(), bytes.size(), kKnownGhostStartBit, reg);
    if (!decoded.walked_full_stream) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] decoder did NOT reach end-of-stream: '%s'\n",
            decoded.note.c_str());
        return 1;
    }
    if (decoded.records.size() != 2) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] expected 2 records, got %zu\n",
            decoded.records.size());
        return 1;
    }

    // Validate Player fields.
    auto pit = reg.players.find(p_in.ghost_id);
    if (pit == reg.players.end()) {
        std::fputs("[ghost-encoder-selftest] Player not in registry\n", stderr);
        return 1;
    }
    const auto& p_out = pit->second;
    const float pos_eps = 1e-4f;
    const float yaw_eps = 2.0f * 3.14159265358979323846f / 511.0f;  // 9-bit quant
    if (std::fabs(p_out.pos_x - p_in.pos_x) > pos_eps
        || std::fabs(p_out.pos_y - p_in.pos_y) > pos_eps
        || std::fabs(p_out.pos_z - p_in.pos_z) > pos_eps) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] Player pos mismatch: in=(%.4f,%.4f,%.4f) out=(%.4f,%.4f,%.4f)\n",
            p_in.pos_x, p_in.pos_y, p_in.pos_z,
            p_out.pos_x, p_out.pos_y, p_out.pos_z);
        return 1;
    }
    if (std::fabs(p_out.yaw - p_in.yaw) > yaw_eps) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] Player yaw mismatch: in=%.4f out=%.4f (eps=%.4f)\n",
            p_in.yaw, p_out.yaw, yaw_eps);
        return 1;
    }
    if (p_out.on_ground != p_in.on_ground
        || p_out.base.team_id != p_in.base.team_id) {
        std::fputs("[ghost-encoder-selftest] Player on_ground / team mismatch\n", stderr);
        return 1;
    }

    // Validate StaticShape fields.
    auto sit = reg.statics.find(s_in.ghost_id);
    if (sit == reg.statics.end()) {
        std::fputs("[ghost-encoder-selftest] StaticShape not in registry\n", stderr);
        return 1;
    }
    const auto& s_out = sit->second;
    if (std::fabs(s_out.pos_x - s_in.pos_x) > pos_eps
        || std::fabs(s_out.pos_y - s_in.pos_y) > pos_eps
        || std::fabs(s_out.pos_z - s_in.pos_z) > pos_eps) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] StaticShape pos mismatch: in=(%.4f,%.4f,%.4f) out=(%.4f,%.4f,%.4f)\n",
            s_in.pos_x, s_in.pos_y, s_in.pos_z,
            s_out.pos_x, s_out.pos_y, s_out.pos_z);
        return 1;
    }
    if (s_out.shape_data_file_id != s_in.shape_data_file_id) {
        std::fputs("[ghost-encoder-selftest] StaticShape datafile id mismatch\n", stderr);
        return 1;
    }

    // Round-trip the same Player as a normal-delta packet (decoder
    // recognises it as already-introduced via the registry we just
    // populated, but we still encode full_update=true so it carries
    // obj_id + class_tag mid-stream).
    GhostPlayer p_delta = p_in;
    p_delta.initial_update = false;
    p_delta.pos_z = 0.0f;
    TypedRecordOut delta_rec;
    delta_rec.kind        = GhostClassKind::Player;
    delta_rec.ghost_id    = p_delta.ghost_id;
    delta_rec.full_update = false;     // delta only — receiver has it from above
    delta_rec.player      = &p_delta;

    const auto delta_bytes = encode_normal_delta(hdr, {delta_rec}, 10);
    std::fprintf(stderr,
        "[ghost-encoder-selftest] normal-delta packet = %zu bytes\n",
        delta_bytes.size());
    auto delta_dec = parse_typed_packet_at_offset(
        delta_bytes.data(), delta_bytes.size(), kKnownGhostStartBit, reg);
    if (!delta_dec.walked_full_stream) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] normal-delta decoder failed: '%s'\n",
            delta_dec.note.c_str());
        return 1;
    }
    if (delta_dec.records.size() != 1) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] expected 1 delta record, got %zu\n",
            delta_dec.records.size());
        return 1;
    }
    const auto& p_after = reg.players[p_in.ghost_id];
    if (std::fabs(p_after.pos_z - p_delta.pos_z) > pos_eps) {
        std::fprintf(stderr,
            "[ghost-encoder-selftest] delta pos_z mismatch: in=%.4f out=%.4f\n",
            p_delta.pos_z, p_after.pos_z);
        return 1;
    }

    std::fputs("[ghost-encoder-selftest] OK — scope-always + normal-delta round-trip\n",
               stderr);
    return 0;
}

}  // namespace net20
