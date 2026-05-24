// Track 26 spec 14c-I-2 — per-class ghost encoders for the TAH-extended
// class roster (clean-room).
// See tah_class_encoders.hpp for the wire-format reference.

#include "tah_class_encoders.hpp"

#include "ghost_encoder.hpp"
#include "tah_class_registry.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net20 {

// ----- Body writers ---------------------------------------------------------

void write_marker_body(BitWriter& w, const MarkerGhost& g,
                       std::uint8_t datafile_id_width)
{
    w.write_flag(g.initial_update);
    if (g.initial_update) {
        w.write_bits(static_cast<std::uint32_t>(g.marker_data_file_id),
                     datafile_id_width);
    }
    w.write_flag(g.transform_changed);
    if (g.transform_changed) {
        // 96-bit position (3 floats bit-packed) + 96-bit rotation
        // (3 floats bit-packed). Per spec §3.1 and the §6.9 corrected
        // raw-bytes primitive, NO byte alignment surrounds these.
        write_float_unaligned(w, g.pos_x);
        write_float_unaligned(w, g.pos_y);
        write_float_unaligned(w, g.pos_z);
        write_float_unaligned(w, g.rot_x);
        write_float_unaligned(w, g.rot_y);
        write_float_unaligned(w, g.rot_z);
    }
}

void write_moveable_body(BitWriter& w, const MoveableGhost& g,
                         std::uint8_t datafile_id_width,
                         std::uint8_t waypoint_bit_width)
{
    w.write_flag(g.initial_update);
    if (g.initial_update) {
        w.write_bits(static_cast<std::uint32_t>(g.moveable_data_file_id),
                     datafile_id_width);
    }
    w.write_flag(g.path_changed);
    if (g.path_changed) {
        w.write_bits(static_cast<std::uint32_t>(g.path_id), 8);
    }
    w.write_flag(g.position_on_path_changed);
    if (g.position_on_path_changed) {
        // Mask to fit the supplied waypoint width (avoid bit leak if
        // caller passed a struct with a too-wide index).
        const std::uint32_t mask =
            waypoint_bit_width >= 32 ? 0xFFFFFFFFu
                                     : ((1u << waypoint_bit_width) - 1u);
        w.write_bits(static_cast<std::uint32_t>(g.waypoint_index) & mask,
                     waypoint_bit_width);
        write_float_unaligned(w, g.waypoint_time);
    }
    w.write_flag(g.state_changed);
    if (g.state_changed) {
        w.write_flag(g.is_blocked);
        const std::uint32_t mask =
            waypoint_bit_width >= 32 ? 0xFFFFFFFFu
                                     : ((1u << waypoint_bit_width) - 1u);
        w.write_bits(static_cast<std::uint32_t>(g.stop_waypoint_index) & mask,
                     waypoint_bit_width);
        w.write_bits(static_cast<std::uint32_t>(g.movement_state) & 0x3u, 2);
    }
}

void write_trigger_body(BitWriter& w, const TriggerGhost& g)
{
    w.write_flag(g.transform_changed);
    if (g.transform_changed) {
        // 12 bit-packed IEEE-754 LE floats (384 bits). Per spec §3.3
        // they form a 3x4 column-major matrix; encoder is order-only,
        // semantic interpretation is the caller's.
        for (float f : g.transform_matrix) {
            write_float_unaligned(w, f);
        }
    }
    w.write_flag(g.bbox_changed);
    if (g.bbox_changed) {
        // 6 bit-packed floats (192 bits) — min-X/Y/Z then max-X/Y/Z.
        for (float f : g.bounding_box) {
            write_float_unaligned(w, f);
        }
    }
}

void write_sound_source_body(BitWriter& w, const SoundSourceGhost& g,
                             std::uint8_t datafile_id_width)
{
    w.write_flag(g.initial_update);
    if (g.initial_update) {
        w.write_bits(static_cast<std::uint32_t>(g.sound_data_file_id),
                     datafile_id_width);
        write_float_unaligned(w, g.pos_x);
        write_float_unaligned(w, g.pos_y);
        write_float_unaligned(w, g.pos_z);
        w.write_flag(g.is_looping);
        w.write_flag(g.follow_source);
        if (g.follow_source) {
            w.write_bits(static_cast<std::uint32_t>(g.parent_ghost_id) & 0x3FFu, 10);
        }
    }
    w.write_flag(g.state_changed);
    if (g.state_changed) {
        w.write_flag(g.is_playing);
    }
}

void write_sensor_body(BitWriter& w, const SensorGhost& g,
                       std::uint8_t datafile_id_width)
{
    // Per spec §3.8 the layout mirrors the StaticShape body's first
    // four sub-blocks (transform / damage / info) but OMITS the
    // shape-info-changed sub-block, then appends a 1-bit initial flag
    // and a `DfW`-bit sensor data-file id.
    w.write_flag(g.transform_changed);
    if (g.transform_changed) {
        write_float_unaligned(w, g.pos_x);
        write_float_unaligned(w, g.pos_y);
        write_float_unaligned(w, g.pos_z);
        write_float_unaligned(w, g.rot_x);
        write_float_unaligned(w, g.rot_y);
        write_float_unaligned(w, g.rot_z);
    }
    w.write_flag(g.damage_changed);
    if (g.damage_changed) {
        w.write_flag(g.state_enabled);
        if (!g.state_enabled) {
            w.write_flag(g.state_disabled);
        }
        float dl = g.damage_level;
        if (dl < 0.0f) dl = 0.0f;
        if (dl > 1.0f) dl = 1.0f;
        const std::uint32_t bits =
            static_cast<std::uint32_t>(std::lround(dl * 255.0f));
        w.write_bits(bits & 0xFFu, 8);
    }
    w.write_flag(g.info_changed);
    if (g.info_changed) {
        w.write_flag(g.is_target);
    }
    w.write_flag(g.initial_update);
    if (g.initial_update) {
        w.write_bits(static_cast<std::uint32_t>(g.sensor_data_file_id),
                     datafile_id_width);
    }
}

// ----- Selftest: roundtrip via local body readers ---------------------------

namespace {

// Local BitReader (LSB-first within byte) — duplicate of the one in
// ghost_types.cpp. Kept private to the selftest so we don't leak it
// out of this TU.
struct LocalBitReader {
    const std::uint8_t* data = nullptr;
    std::size_t bit_length = 0;
    std::size_t pos = 0;
    bool overrun = false;

    LocalBitReader(const std::uint8_t* d, std::size_t bytes) noexcept
        : data(d), bit_length(bytes * 8) {}

    std::uint32_t read_bits(unsigned width) noexcept {
        if (width == 0) return 0;
        if (width > 32 || pos + width > bit_length) {
            overrun = true;
            return 0;
        }
        std::uint32_t v = 0;
        for (unsigned i = 0; i < width; ++i) {
            const std::size_t p = pos + i;
            const std::uint8_t bit = (data[p >> 3] >> (p & 7)) & 1u;
            v |= static_cast<std::uint32_t>(bit) << i;
        }
        pos += width;
        return v;
    }
    bool read_flag() noexcept { return read_bits(1) != 0; }
    float read_float_unaligned() noexcept {
        if (pos + 32 > bit_length) { overrun = true; return 0.0f; }
        const std::uint32_t bits = read_bits(32);
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }
};

GhostBaseState read_base_state_local(LocalBitReader& br) {
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

MarkerGhost read_marker_body(LocalBitReader& br,
                             std::uint8_t datafile_id_width) {
    MarkerGhost g;
    g.initial_update = br.read_flag();
    if (g.initial_update) {
        g.marker_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
    }
    g.transform_changed = br.read_flag();
    if (g.transform_changed) {
        g.pos_x = br.read_float_unaligned();
        g.pos_y = br.read_float_unaligned();
        g.pos_z = br.read_float_unaligned();
        g.rot_x = br.read_float_unaligned();
        g.rot_y = br.read_float_unaligned();
        g.rot_z = br.read_float_unaligned();
    }
    return g;
}

SoundSourceGhost read_sound_source_body(LocalBitReader& br,
                                        std::uint8_t datafile_id_width) {
    SoundSourceGhost g;
    g.initial_update = br.read_flag();
    if (g.initial_update) {
        g.sound_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
        g.pos_x = br.read_float_unaligned();
        g.pos_y = br.read_float_unaligned();
        g.pos_z = br.read_float_unaligned();
        g.is_looping = br.read_flag();
        g.follow_source = br.read_flag();
        if (g.follow_source) {
            g.parent_ghost_id = static_cast<std::uint16_t>(br.read_bits(10));
        }
    }
    g.state_changed = br.read_flag();
    if (g.state_changed) {
        g.is_playing = br.read_flag();
    }
    return g;
}

TriggerGhost read_trigger_body(LocalBitReader& br) {
    TriggerGhost g;
    g.transform_changed = br.read_flag();
    if (g.transform_changed) {
        for (auto& f : g.transform_matrix) f = br.read_float_unaligned();
    }
    g.bbox_changed = br.read_flag();
    if (g.bbox_changed) {
        for (auto& f : g.bounding_box) f = br.read_float_unaligned();
    }
    return g;
}

MoveableGhost read_moveable_body(LocalBitReader& br,
                                 std::uint8_t datafile_id_width,
                                 std::uint8_t waypoint_bit_width) {
    MoveableGhost g;
    g.initial_update = br.read_flag();
    if (g.initial_update) {
        g.moveable_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
    }
    g.path_changed = br.read_flag();
    if (g.path_changed) {
        g.path_id = static_cast<std::uint8_t>(br.read_bits(8));
    }
    g.position_on_path_changed = br.read_flag();
    if (g.position_on_path_changed) {
        g.waypoint_index =
            static_cast<std::uint16_t>(br.read_bits(waypoint_bit_width));
        g.waypoint_time = br.read_float_unaligned();
    }
    g.state_changed = br.read_flag();
    if (g.state_changed) {
        g.is_blocked = br.read_flag();
        g.stop_waypoint_index =
            static_cast<std::uint16_t>(br.read_bits(waypoint_bit_width));
        g.movement_state = static_cast<std::uint8_t>(br.read_bits(2));
    }
    return g;
}

SensorGhost read_sensor_body(LocalBitReader& br,
                             std::uint8_t datafile_id_width) {
    SensorGhost g;
    g.transform_changed = br.read_flag();
    if (g.transform_changed) {
        g.pos_x = br.read_float_unaligned();
        g.pos_y = br.read_float_unaligned();
        g.pos_z = br.read_float_unaligned();
        g.rot_x = br.read_float_unaligned();
        g.rot_y = br.read_float_unaligned();
        g.rot_z = br.read_float_unaligned();
    }
    g.damage_changed = br.read_flag();
    if (g.damage_changed) {
        g.state_enabled = br.read_flag();
        if (!g.state_enabled) {
            g.state_disabled = br.read_flag();
        }
        const std::uint32_t dl = br.read_bits(8);
        g.damage_level = static_cast<float>(dl) / 255.0f;
    }
    g.info_changed = br.read_flag();
    if (g.info_changed) {
        g.is_target = br.read_flag();
    }
    g.initial_update = br.read_flag();
    if (g.initial_update) {
        g.sensor_data_file_id =
            static_cast<std::uint8_t>(br.read_bits(datafile_id_width));
    }
    return g;
}

// Compare two floats bit-for-bit. The encoders use bit-packed IEEE-754
// LE writes (no quantization), so roundtrip should be exact.
bool float_eq_exact(float a, float b) {
    std::uint32_t ab, bb;
    std::memcpy(&ab, &a, 4);
    std::memcpy(&bb, &b, 4);
    return ab == bb;
}

}  // anonymous namespace

int tah_class_encoders_selftest()
{
    int failures = 0;

    // ----- Class-tag registry sanity --------------------------------
    {
        // Sim range: Sky should be tag 5.
        if (role_for_tag(5) != TahClassRole::Sky) {
            std::fputs("[tah-class-selftest] tag 5 (Sky) wrong role\n", stderr);
            ++failures;
        }
        // Common range: Marker = 129, Trigger = 132, SoundSource = 131.
        if (role_for_tag(129) != TahClassRole::Marker) {
            std::fputs("[tah-class-selftest] tag 129 (Marker) wrong role\n", stderr);
            ++failures;
        }
        if (role_for_tag(131) != TahClassRole::Sound) {
            std::fputs("[tah-class-selftest] tag 131 (Sound) wrong role\n", stderr);
            ++failures;
        }
        if (role_for_tag(132) != TahClassRole::Trigger) {
            std::fputs("[tah-class-selftest] tag 132 (Trigger) wrong role\n", stderr);
            ++failures;
        }
        // Player tag from the 2026-05-22 PvP capture.
        if (role_for_tag(960) != TahClassRole::Player) {
            std::fputs("[tah-class-selftest] tag 960 (Player) wrong role\n", stderr);
            ++failures;
        }
        // Tag not in the published map.
        if (role_for_tag(999) != TahClassRole::Unknown) {
            std::fputs("[tah-class-selftest] tag 999 should be Unknown\n", stderr);
            ++failures;
        }
        std::fprintf(stderr,
            "[tah-class-selftest] registry has %zu entries\n",
            tah_registry_entry_count());
    }

    // ----- Marker roundtrip -----------------------------------------
    {
        MarkerGhost m_in;
        m_in.ghost_id = 12;
        m_in.object_id = 0xA1B2C3D4;
        m_in.class_tag = 129;
        m_in.base.base_changed = true;
        m_in.base.team_id = 2;
        m_in.initial_update = true;
        m_in.marker_data_file_id = 0x42;
        m_in.transform_changed = true;
        m_in.pos_x = 100.5f; m_in.pos_y = -45.25f; m_in.pos_z = 12.75f;
        m_in.rot_x = 0.0f;   m_in.rot_y = 1.5707963f; m_in.rot_z = -0.785398f;

        BitWriter w;
        write_base_state(w, m_in.base);
        write_marker_body(w, m_in);

        LocalBitReader br(w.bytes.data(), w.bytes.size());
        GhostBaseState  b_out = read_base_state_local(br);
        MarkerGhost     m_out = read_marker_body(br, kDefaultDfWBits);

        if (br.overrun) {
            std::fputs("[tah-class-selftest] Marker decode overran\n", stderr);
            ++failures;
        }
        if (b_out.base_changed != m_in.base.base_changed
            || b_out.team_id != m_in.base.team_id) {
            std::fputs("[tah-class-selftest] Marker base-state mismatch\n", stderr);
            ++failures;
        }
        if (m_out.initial_update != m_in.initial_update
            || m_out.marker_data_file_id != m_in.marker_data_file_id
            || m_out.transform_changed != m_in.transform_changed
            || !float_eq_exact(m_out.pos_x, m_in.pos_x)
            || !float_eq_exact(m_out.pos_y, m_in.pos_y)
            || !float_eq_exact(m_out.pos_z, m_in.pos_z)
            || !float_eq_exact(m_out.rot_x, m_in.rot_x)
            || !float_eq_exact(m_out.rot_y, m_in.rot_y)
            || !float_eq_exact(m_out.rot_z, m_in.rot_z)) {
            std::fputs("[tah-class-selftest] Marker payload mismatch\n", stderr);
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-class-selftest] Marker roundtrip OK (%zu bytes, %zu bits)\n",
                w.bytes.size(), w.bit_pos);
        }
    }

    // ----- SoundSource roundtrip ------------------------------------
    {
        SoundSourceGhost s_in;
        s_in.ghost_id = 7;
        s_in.object_id = 0x11223344;
        s_in.class_tag = 131;
        s_in.base.base_changed = false;
        s_in.initial_update = true;
        s_in.sound_data_file_id = 0x17;
        s_in.pos_x = -10.5f; s_in.pos_y = 0.25f; s_in.pos_z = 5.0f;
        s_in.is_looping = true;
        s_in.follow_source = true;
        s_in.parent_ghost_id = 0x2A;
        s_in.state_changed = true;
        s_in.is_playing = true;

        BitWriter w;
        write_base_state(w, s_in.base);
        write_sound_source_body(w, s_in);

        LocalBitReader br(w.bytes.data(), w.bytes.size());
        GhostBaseState      b_out = read_base_state_local(br);
        SoundSourceGhost    s_out = read_sound_source_body(br, kDefaultDfWBits);

        if (br.overrun) {
            std::fputs("[tah-class-selftest] SoundSource decode overran\n", stderr);
            ++failures;
        }
        if (b_out.base_changed != s_in.base.base_changed) {
            std::fputs("[tah-class-selftest] SoundSource base mismatch\n", stderr);
            ++failures;
        }
        if (s_out.initial_update != s_in.initial_update
            || s_out.sound_data_file_id != s_in.sound_data_file_id
            || !float_eq_exact(s_out.pos_x, s_in.pos_x)
            || !float_eq_exact(s_out.pos_y, s_in.pos_y)
            || !float_eq_exact(s_out.pos_z, s_in.pos_z)
            || s_out.is_looping != s_in.is_looping
            || s_out.follow_source != s_in.follow_source
            || s_out.parent_ghost_id != s_in.parent_ghost_id
            || s_out.state_changed != s_in.state_changed
            || s_out.is_playing != s_in.is_playing) {
            std::fputs("[tah-class-selftest] SoundSource payload mismatch\n",
                       stderr);
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-class-selftest] SoundSource roundtrip OK (%zu bytes, %zu bits)\n",
                w.bytes.size(), w.bit_pos);
        }
    }

    // ----- Trigger roundtrip ----------------------------------------
    {
        TriggerGhost t_in;
        t_in.ghost_id = 99;
        t_in.object_id = 0xDEADC0DE;
        t_in.class_tag = 132;
        t_in.base.base_changed = true;
        t_in.base.team_id = 5;
        t_in.transform_changed = true;
        // 3x4 column-major (12 floats). Identity rotation + (10, 20, 30)
        // translation: col0 = (1, 0, 0), col1 = (0, 1, 0),
        // col2 = (0, 0, 1), col3 = (10, 20, 30).
        t_in.transform_matrix = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
            10.0f, 20.0f, 30.0f
        };
        t_in.bbox_changed = true;
        t_in.bounding_box = { -1.0f, -2.0f, -3.0f, 4.0f, 5.0f, 6.0f };

        BitWriter w;
        write_base_state(w, t_in.base);
        write_trigger_body(w, t_in);

        LocalBitReader br(w.bytes.data(), w.bytes.size());
        GhostBaseState  b_out = read_base_state_local(br);
        TriggerGhost    t_out = read_trigger_body(br);

        if (br.overrun) {
            std::fputs("[tah-class-selftest] Trigger decode overran\n", stderr);
            ++failures;
        }
        bool ok = (b_out.base_changed == t_in.base.base_changed)
               && (b_out.team_id == t_in.base.team_id)
               && (t_out.transform_changed == t_in.transform_changed)
               && (t_out.bbox_changed == t_in.bbox_changed);
        for (std::size_t i = 0; i < 12 && ok; ++i) {
            ok = ok && float_eq_exact(t_out.transform_matrix[i],
                                      t_in.transform_matrix[i]);
        }
        for (std::size_t i = 0; i < 6 && ok; ++i) {
            ok = ok && float_eq_exact(t_out.bounding_box[i],
                                      t_in.bounding_box[i]);
        }
        if (!ok) {
            std::fputs("[tah-class-selftest] Trigger payload mismatch\n", stderr);
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-class-selftest] Trigger roundtrip OK (%zu bytes, %zu bits)\n",
                w.bytes.size(), w.bit_pos);
        }
    }

    // ----- Moveable roundtrip ---------------------------------------
    {
        MoveableGhost mv_in;
        mv_in.ghost_id = 33;
        mv_in.object_id = 0xCAFEBABE;
        mv_in.class_tag = 640;       // game-specific TAH-observed
        mv_in.base.base_changed = true;
        mv_in.base.team_id = 0;
        mv_in.initial_update = true;
        mv_in.moveable_data_file_id = 0x55;
        mv_in.path_changed = true;
        mv_in.path_id = 0x07;
        mv_in.position_on_path_changed = true;
        mv_in.waypoint_index = 0x12;       // fits 6 bits (0x3F max)
        mv_in.waypoint_time = 0.375f;
        mv_in.state_changed = true;
        mv_in.is_blocked = false;
        mv_in.stop_waypoint_index = 0x21;
        mv_in.movement_state = 1;           // forward

        BitWriter w;
        write_base_state(w, mv_in.base);
        write_moveable_body(w, mv_in);

        LocalBitReader br(w.bytes.data(), w.bytes.size());
        GhostBaseState  b_out  = read_base_state_local(br);
        MoveableGhost   mv_out = read_moveable_body(br,
                                                    kDefaultDfWBits,
                                                    kDefaultWpWBits);

        if (br.overrun) {
            std::fputs("[tah-class-selftest] Moveable decode overran\n", stderr);
            ++failures;
        }
        if (b_out.base_changed != mv_in.base.base_changed
            || mv_out.initial_update != mv_in.initial_update
            || mv_out.moveable_data_file_id != mv_in.moveable_data_file_id
            || mv_out.path_id != mv_in.path_id
            || mv_out.waypoint_index != mv_in.waypoint_index
            || !float_eq_exact(mv_out.waypoint_time, mv_in.waypoint_time)
            || mv_out.is_blocked != mv_in.is_blocked
            || mv_out.stop_waypoint_index != mv_in.stop_waypoint_index
            || mv_out.movement_state != mv_in.movement_state) {
            std::fputs("[tah-class-selftest] Moveable payload mismatch\n",
                       stderr);
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-class-selftest] Moveable roundtrip OK (%zu bytes, %zu bits, WpW=%u)\n",
                w.bytes.size(), w.bit_pos,
                (unsigned)kDefaultWpWBits);
        }
    }

    // ----- Sensor roundtrip -----------------------------------------
    {
        SensorGhost sn_in;
        sn_in.ghost_id = 55;
        sn_in.object_id = 0xFEEDFACE;
        sn_in.class_tag = 324;       // game-specific
        sn_in.base.base_changed = true;
        sn_in.base.team_id = 1;
        sn_in.transform_changed = true;
        sn_in.pos_x = 0.0f; sn_in.pos_y = 100.0f; sn_in.pos_z = -50.0f;
        sn_in.rot_x = 0.0f; sn_in.rot_y = 0.0f;   sn_in.rot_z = 1.0f;
        sn_in.damage_changed = true;
        sn_in.state_enabled = true;
        sn_in.damage_level = 0.5f;          // ~127 in 8-bit
        sn_in.info_changed = true;
        sn_in.is_target = true;
        sn_in.initial_update = true;
        sn_in.sensor_data_file_id = 0x09;

        BitWriter w;
        write_base_state(w, sn_in.base);
        write_shape_layer_block(w, sn_in.shape);
        write_sensor_body(w, sn_in);

        LocalBitReader br(w.bytes.data(), w.bytes.size());
        GhostBaseState  b_out  = read_base_state_local(br);
        // Sensor's shape-layer block is 3 zero bits here (all "changed"
        // flags false). Consume them manually rather than pulling in the
        // full reader, to keep this TU self-contained.
        for (int i = 0; i < 3; ++i) (void)br.read_flag();
        SensorGhost sn_out = read_sensor_body(br, kDefaultDfWBits);

        if (br.overrun) {
            std::fputs("[tah-class-selftest] Sensor decode overran\n", stderr);
            ++failures;
        }
        const float dl_eps = 1.0f / 255.0f;
        if (b_out.base_changed != sn_in.base.base_changed
            || b_out.team_id != sn_in.base.team_id
            || sn_out.transform_changed != sn_in.transform_changed
            || !float_eq_exact(sn_out.pos_x, sn_in.pos_x)
            || !float_eq_exact(sn_out.pos_y, sn_in.pos_y)
            || !float_eq_exact(sn_out.pos_z, sn_in.pos_z)
            || sn_out.damage_changed != sn_in.damage_changed
            || sn_out.state_enabled != sn_in.state_enabled
            || std::fabs(sn_out.damage_level - sn_in.damage_level) > dl_eps
            || sn_out.info_changed != sn_in.info_changed
            || sn_out.is_target != sn_in.is_target
            || sn_out.initial_update != sn_in.initial_update
            || sn_out.sensor_data_file_id != sn_in.sensor_data_file_id) {
            std::fputs("[tah-class-selftest] Sensor payload mismatch\n",
                       stderr);
            ++failures;
        } else {
            std::fprintf(stderr,
                "[tah-class-selftest] Sensor roundtrip OK (%zu bytes, %zu bits)\n",
                w.bytes.size(), w.bit_pos);
        }
    }

    // ----- Byte-for-byte determinism across two encode passes -------
    // Re-encode the same Marker twice; the output bytes must match
    // bit-for-bit. Catches accidental nondeterminism in the encoders.
    {
        MarkerGhost m;
        m.base.base_changed = true;
        m.base.team_id = 4;
        m.initial_update = true;
        m.marker_data_file_id = 0x11;
        m.transform_changed = true;
        m.pos_x = 1.0f; m.pos_y = 2.0f; m.pos_z = 3.0f;
        m.rot_x = 0.0f; m.rot_y = 0.0f; m.rot_z = 0.0f;

        BitWriter w1, w2;
        write_base_state(w1, m.base); write_marker_body(w1, m);
        write_base_state(w2, m.base); write_marker_body(w2, m);
        if (w1.bit_pos != w2.bit_pos || w1.bytes != w2.bytes) {
            std::fputs("[tah-class-selftest] determinism check failed\n",
                       stderr);
            ++failures;
        }
    }

    if (failures == 0) {
        std::fputs("[tah-class-selftest] OK — all subtests passed\n", stderr);
    } else {
        std::fprintf(stderr,
            "[tah-class-selftest] FAILED — %d subtest(s) failed\n",
            failures);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace net20
