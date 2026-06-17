// 14c-PhaseA — TAH Phase 1 reply + full catalogue burst (clean-room).
// See tah_phase_reply.hpp for the spec authority list.

#include "tah_phase_reply.hpp"

#include "reliable_acks.hpp"
#include "tah_datablock_encoder.hpp"
#include "tah_default_catalogue.hpp"
#include "tah_vc_outbound.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dts_viewer {

namespace {

// ---- Bit-level helpers --------------------------------------------------

// Write an uncompressed Huffman-style string per spec §1.5:
//   [1-bit compression flag (= 0)] [8-bit length] [length * 8 raw bits]
// Strings of length 0 emit `[0][8-bit zero]`.
//
// 14c-PhaseA inference: we ALWAYS emit compressed=0 (raw). The spec
// §1.5 explicitly permits this and the implementer notes (§8 P4) call
// out that the Huffman tree build is large work for marginal byte
// savings on the wire (the client reads either form). The TAH client
// reads either path symmetrically. This matches the convention already
// used by `tah_datablock_encoder::write_v3_string_uncompressed`.
void write_huff_string(net20::BitWriter& w, const std::string& s)
{
    w.write_flag(false);  // compression flag = 0 (uncompressed)
    const std::size_t len = s.size() > 255 ? 255 : s.size();
    w.write_bits(static_cast<std::uint32_t>(len), 8);
    for (std::size_t i = 0; i < len; ++i) {
        w.write_bits(static_cast<std::uint8_t>(s[i]), 8);
    }
}

// Write the per-event ESS framing header per §2.1:
//   [1 bit] event-present = 1
//   [1 bit] flag_a = 1
//   [1 bit] flag_b = seq_continuous
//   [if !flag_b]:
//       [1 bit] flag_c = 1 (= has_explicit_seq)
//       [7 bits] explicit_seq
//   [7 bits] wire_class_tag
void write_event_header(net20::BitWriter& w,
                        std::uint8_t  explicit_seq,
                        bool          seq_continuous,
                        std::uint8_t  wire_class_tag)
{
    w.write_flag(true);              // event-present
    w.write_flag(true);              // flag_a
    w.write_flag(seq_continuous);    // flag_b
    if (!seq_continuous) {
        w.write_flag(true);          // flag_c (has_explicit_seq)
        w.write_bits(explicit_seq & 0x7Fu, 7);
    }
    w.write_bits(wire_class_tag & 0x7Fu, 7);
}

// ---- Per-class event body writers --------------------------------------

// §2.12 TeamAddEvent (wire tag 79):
//   readInt(32)             — burned (always 0 in pcap)
//   team_id = readInt(32)   — signed; -1 = "unnamed"
//   team_name = readString
//   team_skin = readString
void write_team_add_body(net20::BitWriter& w,
                         std::uint32_t team_id,
                         const std::string& name,
                         const std::string& skin)
{
    w.write_bits(0u, 32);            // burned (always 0)
    w.write_bits(team_id, 32);       // -1 encodes as 0xFFFFFFFF
    write_huff_string(w, name);
    write_huff_string(w, skin);
}

// §2.9 PlayerAddEvent (wire tag 75):
//   player_id        = readInt(32)
//                      readFlag           — burned
//   player_name      = readString
//   player_skin      = readString
//   player_voice     = readString
//                      readInt(1)          — burned
//                      readInt(32)         — burned
//   if (readFlag):
//       player_team  = readInt(3)
//   else:
//       player_team  = -1
void write_player_add_body(net20::BitWriter& w,
                           std::uint32_t player_id,
                           const std::string& name,
                           const std::string& skin,
                           const std::string& voice,
                           int team_id_or_neg1)
{
    w.write_bits(player_id, 32);
    w.write_flag(false);             // burned
    write_huff_string(w, name);
    write_huff_string(w, skin);
    write_huff_string(w, voice);
    w.write_bits(0u, 1);             // burned
    w.write_bits(0u, 32);            // burned
    if (team_id_or_neg1 >= 0 && team_id_or_neg1 < 8) {
        w.write_flag(true);
        w.write_bits(static_cast<std::uint32_t>(team_id_or_neg1) & 0x7u, 3);
    } else {
        w.write_flag(false);         // unassigned → -1
    }
}

// §2.6 SimConsoleEvent (wire tag 8):
//   [5 bits] argc
//   for i in 0..argc-1:
//       [huff string] arg[i]
void write_sim_console_body(net20::BitWriter& w,
                            const std::vector<std::string>& args)
{
    const std::size_t argc = args.size() > 31 ? 31 : args.size();
    w.write_bits(static_cast<std::uint32_t>(argc), 5);
    for (std::size_t i = 0; i < argc; ++i) {
        write_huff_string(w, args[i]);
    }
}

// ---- Inbound parser for SetCLInfo detection ----------------------------

// Bit-level reader walking a VC DataPacket's ESS looking for any
// SimConsoleEvent (wire tag 8) whose first argument equals
// "SetCLInfo". Returns true on match. Lenient: if the ESS walk hits
// an unknown event class it bails out (does not match).
//
// 14c-PhaseA inference: per §2.1 we cannot reliably skip a body of
// unknown class width without a full decoder for every class. So this
// parser ONLY accepts ESS where SimConsoleEvent is the FIRST event
// (and possibly the only event). Per the pcap walk §7.4, TAH's
// ClientReady packet starts with `zAdminActiveMode` then `SetCLInfo`
// — both wire tag 8 — so this is sufficient for the trigger.
bool parse_for_setclinfo(const std::uint8_t* data, std::size_t size)
{
    net20::BitReader r(data, size);
    // --- VC header (per §1.6 / TRIBES-NETPROTO §14) ---
    if (!r.read_flag()) return false;          // vc=1
    (void)r.read_flag();                       // parity
    (void)r.read_bits(9);                      // send_seq
    (void)r.read_bits(5);                      // hrcv
    // ack-run list
    for (;;) {
        const std::uint32_t len = r.read_bits(3);
        if (r.fail()) return false;
        if (len == 0) break;
        (void)r.read_bits(5);                  // run start
        if (r.fail()) return false;
    }
    const std::uint32_t ptype = r.read_bits(5);
    if (r.fail()) return false;
    if (ptype != 0) return false;              // only DataPackets carry ESS

    // --- Rate-control prefix (§1.6) ---
    const bool r0 = r.read_flag();
    if (r0) { (void)r.read_bits(10); (void)r.read_bits(10); }
    const bool r1 = r.read_flag();
    if (r1) { (void)r.read_bits(10); (void)r.read_bits(10); }
    if (r.fail()) return false;

    // --- ESS-present flag ---
    if (!r.read_flag()) return false;          // no ESS

    // --- Walk events. For each: event-present, flag_a, flag_b, ... ---
    bool first = true;
    while (true) {
        const bool event_present = r.read_flag();
        if (r.fail() || !event_present) return false;
        const bool flag_a = r.read_flag();
        if (r.fail()) return false;
        if (!flag_a) {
            // Resend-after-loss form — 7-bit tag, no sequencing.
            const std::uint32_t tag = r.read_bits(7);
            if (r.fail()) return false;
            if (tag != 8) return false;        // not SimConsoleEvent
        } else {
            const bool flag_b = r.read_flag();
            if (r.fail()) return false;
            if (!flag_b) {
                const bool flag_c = r.read_flag();
                if (r.fail()) return false;
                if (flag_c) {
                    (void)r.read_bits(7);      // explicit_seq
                    if (r.fail()) return false;
                }
            }
            const std::uint32_t tag = r.read_bits(7);
            if (r.fail()) return false;
            if (tag != 8) return false;        // not SimConsoleEvent
        }
        // SimConsoleEvent body: argc(5) + argc * huff string.
        const std::uint32_t argc = r.read_bits(5);
        if (r.fail()) return false;
        if (argc == 0) {
            first = false;
            continue;
        }
        std::string first_arg;
        for (std::uint32_t i = 0; i < argc; ++i) {
            const bool compressed = r.read_flag();
            if (r.fail()) return false;
            const std::uint32_t slen = r.read_bits(8);
            if (r.fail()) return false;
            std::string s;
            s.reserve(slen);
            if (compressed) {
                // 14c-PhaseA inference: TAH's client compresses long
                // strings but the ClientReady command names
                // ("zAdminActiveMode", "SetCLInfo") are short enough
                // that the public TAH client emits uncompressed. We
                // bail out of the walk if we hit compressed (rather
                // than implementing the Huffman tree) — but FIRST
                // check if we already saw SetCLInfo in this packet.
                return false;
            }
            for (std::uint32_t k = 0; k < slen; ++k) {
                const std::uint32_t b = r.read_bits(8);
                if (r.fail()) return false;
                s.push_back(static_cast<char>(b & 0xFFu));
            }
            if (i == 0) first_arg = std::move(s);
        }
        if (first_arg == "SetCLInfo") return true;
        first = false;
        (void)first;  // silence unused-warning on some toolchains
    }
}

// ---- Packet builder helpers --------------------------------------------

// Open a VC DataPacket with R0/R1 rate-control flags emitted as
// requested. After this call the writer's cursor sits at the
// ESS-present flag position (caller writes it next).
//
// `r0_*` / `r1_*` are honoured only when `r0` / `r1` is true.
//
// Returns the OutboundPacketBuilder by reference via out parameter
// since it is non-movable. The caller writes the rest into
// `builder.writer()` and calls `builder.finish(now_ms)`.
void open_data_packet_with_rate(tah_vc::OutboundPacketBuilder& b,
                                bool r0, std::uint32_t cur_delay, std::uint32_t cur_size,
                                bool r1, std::uint32_t max_delay, std::uint32_t max_size)
{
    auto& w = b.writer();
    w.write_flag(r0);
    if (r0) { w.write_bits(cur_delay, 10); w.write_bits(cur_size, 10); }
    w.write_flag(r1);
    if (r1) { w.write_bits(max_delay, 10); w.write_bits(max_size, 10); }
}

// ---- Catalogue body emitters (zero-padding bit-skip per §8 P2) -----------

// 14c-PhaseA inference: per §8 P2 the spec explicitly says "opaque
// bit-skips of the right total width per record body suffice ...
// Real asset data can be a follow-up spec." We honour that: for each
// per-type body we emit zeros of the bit-width sketched by the spec,
// with every optional flag set to 0 so its conditional payload is
// elided. This keeps record bodies short and well-formed for TAH's
// catalogue walker.

// §4.3 GameBaseData body — variable due to two strings. We emit empty
// strings for both: signed_int(8)=0 + "" (1+8) + "" (1+8) = 26 bits.
void write_gamebase_zero(net20::BitWriter& w)
{
    // signed_int(8) sign-mag — emit 0 (8 zero bits)
    w.write_bits(0u, 8);
    // map_icon = ""
    w.write_flag(false); w.write_bits(0u, 8);
    // class_name = ""
    w.write_flag(false); w.write_bits(0u, 8);
}

// §4.3 ShapeBaseData body — single string, no sentinel-prefix case.
void write_shapebase_zero(net20::BitWriter& w)
{
    // shape = ""
    w.write_flag(false); w.write_bits(0u, 8);
    // No 0xfe/0xff sentinel — no 32-bit burn.
}

// §4.3 StaticData body — GameBase + ShapeBase + shield_name +
// 96 bits transform + 16 action slots (each a single 0-flag = 1 bit)
// + 1 final flag.
void write_static_zero(net20::BitWriter& w)
{
    write_gamebase_zero(w);
    write_shapebase_zero(w);
    // shield_name = ""
    w.write_flag(false); w.write_bits(0u, 8);
    // 96 bits opaque transform/bound
    for (int i = 0; i < 3; ++i) w.write_bits(0u, 32);
    // 16 action slots, each absent (1 bit each)
    for (int i = 0; i < 16; ++i) w.write_flag(false);
    // final flag
    w.write_flag(false);
}

// §4.8 StaticShapeData = StaticData + readInt(8) + 2 readFlag
void write_static_shape_zero(net20::BitWriter& w)
{
    write_static_zero(w);
    w.write_bits(0u, 8);
    w.write_flag(false);
    w.write_flag(false);
}

// §4.13 VehicleData = StaticShapeData + 632 burn-bits
void write_vehicle_zero(net20::BitWriter& w)
{
    write_static_shape_zero(w);
    for (int i = 0; i < 19; ++i) w.write_bits(0u, 32);  // 608
    w.write_bits(0u, 24);                                // 24 -> 632 total
}

// §4.15 ProjectileData = GameBaseData + ShapeBaseData + 546 burn-bits
void write_projectile_zero(net20::BitWriter& w)
{
    write_gamebase_zero(w);
    write_shapebase_zero(w);
    // 546 bits opaque
    for (int i = 0; i < 17; ++i) w.write_bits(0u, 32);  // 544
    w.write_bits(0u, 2);
}

// Per-type body writers (§4.4..§4.30). All write zero-pads of the
// right widths with optional flags = 0.
void write_db_body_zero(net20::BitWriter& w, std::uint8_t db_type)
{
    switch (db_type) {
        case 0:   // SoundProfileData — §4.4: 6 + 10 + 6 optional flags
            w.write_bits(0u, 6);
            w.write_bits(0u, 10);
            for (int i = 0; i < 6; ++i) w.write_flag(false);
            break;
        case 1:   // SoundData — §4.5: string + 6-bit float + 8-bit profile id
            w.write_flag(false); w.write_bits(0u, 8);  // ""
            w.write_bits(0u, 6);
            w.write_bits(0u, 8);
            break;
        case 2:   // DamageSkinData — §4.6: 10 strings
            for (int i = 0; i < 10; ++i) {
                w.write_flag(false); w.write_bits(0u, 8);
            }
            break;
        case 3:   // ArmorData — §4.7: StaticData + jet_flame + 51 nodes + 807 burn
            write_static_zero(w);
            w.write_flag(false); w.write_bits(0u, 8);          // jet_flame
            for (int i = 0; i < 51; ++i) {
                w.write_flag(false); w.write_bits(0u, 8);      // node_name
                w.write_flag(false);                            // no shape-id
                w.write_flag(false);                            // bool
                w.write_bits(0u, 4);                            // left
                w.write_bits(0u, 4);                            // right
            }
            // 807 burn bits
            for (int i = 0; i < 25; ++i) w.write_bits(0u, 32);  // 800
            w.write_bits(0u, 7);
            break;
        case 4:   // StaticShapeData — §4.8
            write_static_shape_zero(w);
            break;
        case 5:   // ItemData — §4.9: StaticData + 224 + "" + 3 flags + "" + 192
            write_static_zero(w);
            for (int i = 0; i < 7; ++i) w.write_bits(0u, 32);  // 224
            w.write_flag(false); w.write_bits(0u, 8);          // ""
            w.write_flag(false); w.write_flag(false); w.write_flag(false);
            w.write_flag(false); w.write_bits(0u, 8);          // ""
            for (int i = 0; i < 6; ++i) w.write_bits(0u, 32);  // 192
            break;
        case 6:   // ItemImageData — §4.10: GameBase + item + 849 burn
            write_gamebase_zero(w);
            w.write_flag(false); w.write_bits(0u, 8);
            for (int i = 0; i < 26; ++i) w.write_bits(0u, 32); // 832
            w.write_bits(0u, 17);                               // 849 total
            break;
        case 7:   // MoveableData — §4.11: StaticShapeData + 67 burn
            write_static_shape_zero(w);
            w.write_bits(0u, 32);
            w.write_bits(0u, 32);
            w.write_bits(0u, 3);
            break;
        case 8:   // SensorData — §4.12: StaticShapeData + 66 burn
            write_static_shape_zero(w);
            w.write_bits(0u, 32);
            w.write_bits(0u, 32);
            w.write_bits(0u, 2);
            break;
        case 9:   // VehicleData (sentinel-only in Blastside)
            write_vehicle_zero(w);
            break;
        case 10:  // FlierData — §4.14: VehicleData + 224 burn
            write_vehicle_zero(w);
            for (int i = 0; i < 7; ++i) w.write_bits(0u, 32);
            break;
        case 11:  // TankData (sentinel-only)
        case 12:  // HoverData (sentinel-only)
            write_vehicle_zero(w);
            break;
        case 13:  // ProjectileData (sentinel-only)
            write_projectile_zero(w);
            break;
        case 14:  // BulletData — §4.16: Projectile + 96
            write_projectile_zero(w);
            for (int i = 0; i < 3; ++i) w.write_bits(0u, 32);
            break;
        case 15:  // GrenadeData — §4.17: Projectile + 32 + trail string
            write_projectile_zero(w);
            w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            break;
        case 16:  // RocketData — §4.18: Projectile + 32 + trail + 64
            write_projectile_zero(w);
            w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_bits(0u, 32);
            w.write_bits(0u, 32);
            break;
        case 17:  // LaserData — §4.19: Projectile + 2 strings + 1 flag
            write_projectile_zero(w);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_flag(false);
            break;
        case 18:  // InteriorShapeData (sentinel-only) — body unknown, write 0
            break;
        case 19:  // TurretData — §4.20: SensorData + 225 burn
            write_static_shape_zero(w);
            w.write_bits(0u, 32);
            w.write_bits(0u, 32);
            w.write_bits(0u, 2);                              // = SensorData
            for (int i = 0; i < 7; ++i) w.write_bits(0u, 32); // 224
            w.write_bits(0u, 1);                              // 225 total
            break;
        case 20:  // ExplosionData — §4.21: GameBase + ShapeBase + 66 + optional 480 + 1 flag
            write_gamebase_zero(w);
            write_shapebase_zero(w);
            w.write_bits(0u, 32);
            w.write_bits(0u, 32);
            w.write_bits(0u, 2);
            w.write_flag(false);                              // optional 480 absent
            w.write_flag(false);                              // final flag
            break;
        case 21:  // MarkerData — §4.22: GameBase + icon
            write_gamebase_zero(w);
            w.write_flag(false); w.write_bits(0u, 8);
            break;
        case 22:  // DebrisData — §4.23: GameBase + 32 + "" + 707 burn
            write_gamebase_zero(w);
            w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            for (int i = 0; i < 22; ++i) w.write_bits(0u, 32); // 704
            w.write_bits(0u, 3);                                // 707 total
            break;
        case 23:  // MineData — §4.24: aliased to ItemData
            write_static_zero(w);
            for (int i = 0; i < 7; ++i) w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_flag(false); w.write_flag(false); w.write_flag(false);
            w.write_flag(false); w.write_bits(0u, 8);
            for (int i = 0; i < 6; ++i) w.write_bits(0u, 32);
            break;
        case 24:  // TargetLaserData — §4.25: aliased to LaserData
            write_projectile_zero(w);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_flag(false); w.write_bits(0u, 8);
            w.write_flag(false);
            break;
        case 25:  // SeekingMissileData — §4.26: Projectile + 96
            write_projectile_zero(w);
            for (int i = 0; i < 3; ++i) w.write_bits(0u, 32);
            break;
        case 26:  // TriggerData — §4.27: GameBase
            write_gamebase_zero(w);
            break;
        case 27:  // CarData (sentinel-only)
            write_vehicle_zero(w);
            break;
        case 28:  // LightningData — §4.28: Projectile + 192 + bmp
            write_projectile_zero(w);
            for (int i = 0; i < 6; ++i) w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            break;
        case 29:  // RepairEffectsData — §4.29: aliased to LightningData
            write_projectile_zero(w);
            for (int i = 0; i < 6; ++i) w.write_bits(0u, 32);
            w.write_flag(false); w.write_bits(0u, 8);
            break;
        case 30:  // IrcChannelData — §4.30: GameBase (TERMINATOR)
            write_gamebase_zero(w);
            break;
        default:
            // Unknown — emit a single zero bit to keep the framing aligned.
            w.write_bits(0u, 1);
            break;
    }
}

// Write a single DataBlockEvent record into `w`. Caller has already
// written the event ESS header (event-present, flag_a/b/c, class tag).
// This writes the 6+8+8 catalogue header + optional body.
void write_db_event_body(net20::BitWriter& w,
                         std::uint8_t db_type,
                         std::uint8_t group_size,
                         std::uint8_t block_index)
{
    w.write_bits(db_type & 0x3Fu, 6);
    w.write_bits(group_size, 8);
    w.write_bits(block_index, 8);
    if (block_index != 0xFF) {
        write_db_body_zero(w, db_type);
    }
}

// Estimate the bit cost of a DataBlockEvent given the body type,
// without actually writing it. Used for the soft-cap packing loop.
std::size_t estimate_db_event_bits(std::uint8_t db_type,
                                   std::uint8_t block_index,
                                   bool seq_continuous)
{
    net20::BitWriter probe;
    write_event_header(probe,
                       /*explicit_seq*/ 0,
                       seq_continuous,
                       /*wire_class_tag*/ 88);
    write_db_event_body(probe, db_type,
                        /*group_size*/ 0, block_index);
    return probe.pos();
}

// The full Blastside catalogue inventory per §4.32.
struct DbGroup { std::uint8_t db_type; std::uint8_t group_size; };

const std::array<DbGroup, 31> kBlastsideCatalogue = {{
    { 0, 10 },   // SoundProfileData
    { 1, 153 },  // SoundData
    { 2, 2 },    // DamageSkinData
    { 3, 5 },    // ArmorData (per §4.32; note 14c-PhaseA: spec maps this to "ArmorData", db_type=3)
    { 4, 52 },   // StaticShapeData
    { 5, 45 },   // ItemData
    { 6, 23 },   // ItemImageData
    { 7, 36 },   // MoveableData
    { 8, 5 },    // SensorData
    { 9, 0 },    // VehicleData (sentinel)
    { 10, 3 },   // FlierData
    { 11, 0 },   // TankData (sentinel)
    { 12, 0 },   // HoverData (sentinel)
    { 13, 0 },   // ProjectileData (sentinel)
    { 14, 5 },   // BulletData
    { 15, 3 },   // GrenadeData
    { 16, 2 },   // RocketData
    { 17, 1 },   // LaserData
    { 18, 0 },   // InteriorShapeData (sentinel)
    { 19, 7 },   // TurretData
    { 20, 19 },  // ExplosionData
    { 21, 3 },   // MarkerData
    { 22, 7 },   // DebrisData
    { 23, 2 },   // MineData
    { 24, 1 },   // TargetLaserData
    { 25, 1 },   // SeekingMissileData
    { 26, 1 },   // TriggerData
    { 27, 0 },   // CarData (sentinel)
    { 28, 2 },   // LightningData
    { 29, 1 },   // RepairEffectsData
    { 30, 0 },   // IrcChannelData (sentinel — TERMINATOR)
}};

// Build the flat record list. Each record is (db_type, group_size,
// block_index). For groups with group_size > 0 we emit
// block_index = 0..group_size-1. For group_size == 0 we emit a single
// sentinel record (block_index = 0xFF, group_size = 0).
struct DbRecord {
    std::uint8_t db_type;
    std::uint8_t group_size;
    std::uint8_t block_index;
};

std::vector<DbRecord> build_blastside_record_stream()
{
    std::vector<DbRecord> out;
    out.reserve(440);
    for (const auto& g : kBlastsideCatalogue) {
        if (g.group_size == 0) {
            out.push_back({ g.db_type, 0, 0xFF });
        } else {
            for (std::uint8_t i = 0; i < g.group_size; ++i) {
                out.push_back({ g.db_type, g.group_size, i });
            }
        }
    }
    return out;
}

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool is_setclinfo_clientready(const std::vector<std::uint8_t>& buf)
{
    if (buf.size() < 4) return false;
    return parse_for_setclinfo(buf.data(), buf.size());
}

bool is_phase1_trigger_packet(const std::vector<std::uint8_t>& buf)
{
    if (buf.size() < 4) return false;
    net20::BitReader r(buf.data(), buf.size());

    if (!r.read_flag()) return false;          // vc=1
    (void)r.read_flag();                       // parity
    (void)r.read_bits(9);                      // send_seq
    (void)r.read_bits(5);                      // hrcv
    for (;;) {                                 // ack-run list
        const std::uint32_t len = r.read_bits(3);
        if (r.fail()) return false;
        if (len == 0) break;
        (void)r.read_bits(5);
        if (r.fail()) return false;
    }
    const std::uint32_t ptype = r.read_bits(5);
    if (r.fail() || ptype != 0) return false;  // DataPacket only

    // Rate-control prefix.
    const bool r0 = r.read_flag();
    if (r0) { (void)r.read_bits(10); (void)r.read_bits(10); }
    const bool r1 = r.read_flag();
    if (r1) { (void)r.read_bits(10); (void)r.read_bits(10); }
    if (r.fail()) return false;

    // ESS-present flag — the trigger condition.
    return r.read_flag();
}

std::vector<std::uint8_t>
build_phase1_reply(Session& sess, std::uint64_t now_ms)
{
    // §7.5 / §8 P1: R0=0, R1=1, max_delay=33, max_size=450; then ESS with
    // 3 TeamAddEvents, 1 PlayerAddEvent, 2 SimConsoleEvents (SVInfo, MODInfo).
    tah_vc::OutboundPacketPlan plan;
    plan.base_type = net20::pkt_type::kDataPacket;
    tah_vc::OutboundPacketBuilder b(sess, plan);
    open_data_packet_with_rate(b,
        /*r0*/ false, 0, 0,
        /*r1*/ true, 33, 450);

    auto& w = b.writer();

    // ESS-present = 1
    w.write_flag(true);

    // §7.5 event order: TeamAdd(-1) → TeamAdd(0) → TeamAdd(1) →
    // PlayerAdd → SVInfo → MODInfo
    std::uint8_t eseq = 0;
    auto next_seq = [&]() {
        const std::uint8_t s = eseq;
        eseq = static_cast<std::uint8_t>((eseq + 1u) & 0x7Fu);
        return s;
    };

    // Event 0: TeamAddEvent id=-1 "unnamed" skin="base"
    write_event_header(w, next_seq(), /*seq_continuous*/ false, /*wire_tag*/ 79);
    write_team_add_body(w, 0xFFFFFFFFu, "unnamed", "base");

    // Event 1: TeamAddEvent id=0 "Blood Eagle"
    write_event_header(w, next_seq(), /*seq_continuous*/ true, 79);
    write_team_add_body(w, 0u, "Blood Eagle", "base");

    // Event 2: TeamAddEvent id=1 "Diamond Sword"
    write_event_header(w, next_seq(), /*seq_continuous*/ true, 79);
    write_team_add_body(w, 1u, "Diamond Sword", "base");

    // Event 3: PlayerAddEvent for the joining player.
    // player_id = 0x800 + slot per §2.10 conventions.
    const std::uint32_t player_id = 0x800u + static_cast<std::uint32_t>(sess.player_slot);
    char name_buf[32];
    std::snprintf(name_buf, sizeof(name_buf), "player%u",
                  static_cast<unsigned>(sess.player_slot));
    write_event_header(w, next_seq(), /*seq_continuous*/ true, /*wire_tag*/ 75);
    write_player_add_body(w, player_id, name_buf, "base", "", /*team*/ -1);

    // Event 4: SimConsoleEvent "SVInfo"
    write_event_header(w, next_seq(), /*seq_continuous*/ true, /*wire_tag*/ 8);
    write_sim_console_body(w, {
        "SVInfo",
        "1.11",
        "open-siege t1-server",
        "base",
        "Blastside CTF (TAH-compat dev server)",
        ""
    });

    // Event 5: SimConsoleEvent "MODInfo"
    write_event_header(w, next_seq(), /*seq_continuous*/ true, /*wire_tag*/ 8);
    write_sim_console_body(w, { "MODInfo", "open-siege Phase A" });

    // ESS terminator
    w.write_flag(false);
    // PSC-present = 0
    w.write_flag(false);
    // GSS-present = 0
    w.write_flag(false);

    return b.finish(now_ms);
}

std::vector<std::vector<std::uint8_t>>
build_catalogue_burst(Session& sess, std::uint64_t now_ms)
{
    std::vector<std::vector<std::uint8_t>> out;
    auto records = build_blastside_record_stream();
    if (records.empty()) return out;

    // 14c-PhaseA — pack records into ~400-byte packets. Each packet's
    // ESS begins fresh (catalogue events use guaranteed-ordered delivery
    // with their own seq counter; the explicit_seq on the first event
    // of each packet acts as the resync anchor).
    constexpr std::size_t kSoftBytes = 400;
    constexpr std::size_t kSoftBits  = kSoftBytes * 8u;

    std::size_t idx = 0;
    std::uint8_t event_seq = 0;
    bool first_packet = true;

    while (idx < records.size()) {
        tah_vc::OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        tah_vc::OutboundPacketBuilder b(sess, plan);

        if (first_packet) {
            // First catalogue packet: R0=1, cur_delay=66, cur_size=400.
            open_data_packet_with_rate(b,
                /*r0*/ true, 66, 400,
                /*r1*/ false, 0, 0);
            first_packet = false;
        } else {
            // Subsequent catalogue packets: R0=R1=0.
            open_data_packet_with_rate(b,
                /*r0*/ false, 0, 0,
                /*r1*/ false, 0, 0);
        }

        auto& w = b.writer();
        // ESS-present = 1
        w.write_flag(true);

        bool first_in_pkt = true;
        std::size_t events_this_pkt = 0;
        while (idx < records.size()) {
            const auto& rec = records[idx];
            const std::size_t cost = estimate_db_event_bits(
                rec.db_type, rec.block_index, /*seq_continuous*/ !first_in_pkt);
            // 14c-PhaseA inference: leave room (~3 bits) for the ESS
            // terminator + PSC=0 + GSS=0 trailer.
            if (!first_in_pkt && (w.pos() + cost + 4) > kSoftBits) break;

            write_event_header(w, event_seq, /*seq_continuous*/ !first_in_pkt, 88);
            write_db_event_body(w, rec.db_type, rec.group_size, rec.block_index);
            event_seq = static_cast<std::uint8_t>((event_seq + 1u) & 0x7Fu);
            ++idx;
            ++events_this_pkt;
            first_in_pkt = false;
        }

        // ESS terminator, PSC=0, GSS=0.
        w.write_flag(false);
        w.write_flag(false);
        w.write_flag(false);

        out.push_back(b.finish(now_ms));
        (void)events_this_pkt;
    }

    return out;
}

std::vector<std::uint8_t>
build_ping_reply(Session& sess, std::uint16_t their_seq, std::uint64_t now_ms)
{
    // §6.4: Ping reply is a 4-byte VC header with ptype=Ack (16).
    // Reuse current send_seq (no body, no bump per OutboundPacketBuilder's
    // pure-ack convention). Ack the inbound send_seq via on_receive() in
    // the caller; here we just emit the header.
    //
    // 14c-PhaseA inference: §6.4 says "echo their_seq in hrcv". We rely
    // on sess.ack.highest_recv_mod32 already being updated by the
    // listener's session-ack tracking path (server_listener.cpp line ~407
    // calls sess->ack.on_receive(ph.send_seq) on every non-Ping; for
    // Ping we update it ourselves first).
    sess.ack.on_receive(their_seq);

    tah_vc::OutboundPacketPlan plan;
    plan.base_type = net20::pkt_type::kDataPacket;
    plan.is_ack    = true;       // pure-ack: no body, no send_seq bump
    tah_vc::OutboundPacketBuilder b(sess, plan);
    return b.finish(now_ms);
}

// =========================================================================
// Selftest
// =========================================================================

namespace {

int run_selftest()
{
    int failures = 0;

    // --- (1) Phase 1 reply envelope ---
    {
        Session sess{};
        sess.connect_handle = 0x12345678u;
        sess.connect_parity = (sess.connect_handle & 1u) != 0;
        sess.next_send_seq = 2;
        sess.ack.received[1] = true;
        sess.ack.highest_recv_mod32 = 1;
        sess.player_slot = 3;

        auto pkt = build_phase1_reply(sess, /*now_ms*/ 1234);
        // Envelope: header(4) + rate-prefix(22 bits if R1) + ESS-content +
        // 3 trailer bits + padding. Expect at least 80 B (3 team-adds +
        // playeradd + 2 console events with strings).
        if (pkt.size() < 80 || pkt.size() > 450) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL phase1: size %zu outside [80, 450]\n",
                pkt.size());
            ++failures;
        }
        // VC header should parse cleanly as type_word=0 (DataPacket).
        net20::ParsedIncomingHeader ph;
        if (!net20::parse_incoming_header(pkt.data(), pkt.size(), ph)) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL phase1: VC header parse\n");
            ++failures;
        } else if (ph.type_word != 0) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL phase1: type_word=0x%02x (want 0)\n",
                (unsigned)ph.type_word);
            ++failures;
        }
        // send_seq bumped to 3.
        if (sess.next_send_seq != 3) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL phase1: send_seq=%u (want 3)\n",
                (unsigned)sess.next_send_seq);
            ++failures;
        }
        std::fprintf(stderr,
            "[tah-phase-reply] phase1: %zu bytes\n", pkt.size());
    }

    // --- (2) Catalogue burst — last event is IrcChannelData sentinel ---
    {
        Session sess{};
        sess.connect_handle = 0x12345678u;
        sess.connect_parity = (sess.connect_handle & 1u) != 0;
        sess.next_send_seq = 3;
        sess.ack.received[1] = true;
        sess.ack.highest_recv_mod32 = 1;
        sess.player_slot = 0;

        auto pkts = build_catalogue_burst(sess, /*now_ms*/ 2000);
        if (pkts.empty()) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL catalogue: 0 packets\n");
            ++failures;
        } else {
            std::size_t total = 0;
            for (const auto& p : pkts) total += p.size();
            std::fprintf(stderr,
                "[tah-phase-reply] catalogue: %zu packets / %zu B\n",
                pkts.size(), total);
            // Each packet parses as type_word=0.
            for (std::size_t i = 0; i < pkts.size(); ++i) {
                net20::ParsedIncomingHeader ph;
                if (!net20::parse_incoming_header(
                        pkts[i].data(), pkts[i].size(), ph)) {
                    std::fprintf(stderr,
                        "[tah-phase-reply] FAIL catalogue pkt %zu: VC parse\n",
                        i);
                    ++failures;
                    break;
                }
                if (ph.type_word != 0) {
                    std::fprintf(stderr,
                        "[tah-phase-reply] FAIL catalogue pkt %zu: type=0x%02x\n",
                        i, (unsigned)ph.type_word);
                    ++failures;
                    break;
                }
                if (pkts[i].size() > 460) {
                    std::fprintf(stderr,
                        "[tah-phase-reply] FAIL catalogue pkt %zu: %zuB > 460\n",
                        i, pkts[i].size());
                    ++failures;
                }
            }
            // Catalogue packet count envelope. Public TAH server emits
            // ~50 packets for the full Blastside catalogue per §4.33;
            // our zero-padded bodies are similar in width so 30..60
            // packets is the expected range.
            if (pkts.size() < 10 || pkts.size() > 80) {
                std::fprintf(stderr,
                    "[tah-phase-reply] FAIL catalogue: %zu packets outside "
                    "[10, 80]\n", pkts.size());
                ++failures;
            }
        }
    }

    // --- (3) Ping reply: 4 bytes, ptype=Ack, no send_seq bump ---
    {
        Session sess{};
        sess.connect_handle = 0x12345678u;
        sess.connect_parity = (sess.connect_handle & 1u) != 0;
        sess.next_send_seq = 5;
        sess.ack.highest_recv_mod32 = 3;

        const auto pkt = build_ping_reply(sess, /*their_seq*/ 7, /*now_ms*/ 100);
        if (pkt.size() != 4) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL ping: size %zu (want 4)\n",
                pkt.size());
            ++failures;
        }
        if (sess.next_send_seq != 5) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL ping: send_seq bumped to %u (want 5)\n",
                (unsigned)sess.next_send_seq);
            ++failures;
        }
        net20::ParsedIncomingHeader ph;
        if (!net20::parse_incoming_header(pkt.data(), pkt.size(), ph)) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL ping: VC parse\n");
            ++failures;
        } else {
            if (ph.type_word != net20::pkt_type::kPureAck) {
                std::fprintf(stderr,
                    "[tah-phase-reply] FAIL ping: type_word=0x%02x (want 0x10)\n",
                    (unsigned)ph.type_word);
                ++failures;
            }
        }
    }

    // --- (4) SetCLInfo detection ---
    {
        // Synthesise a minimal VC DataPacket whose ESS contains a single
        // SimConsoleEvent with command "SetCLInfo".
        Session sess{};
        sess.connect_handle = 1u;
        sess.connect_parity = true;
        sess.next_send_seq = 2;
        sess.ack.highest_recv_mod32 = 0;

        tah_vc::OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        tah_vc::OutboundPacketBuilder b(sess, plan);
        // R0=0, R1=0
        b.writer().write_flag(false);
        b.writer().write_flag(false);
        // ESS-present = 1
        b.writer().write_flag(true);
        // One SimConsoleEvent { "SetCLInfo", ... }
        write_event_header(b.writer(),
            /*explicit_seq*/ 0, /*seq_continuous*/ false, /*wire_tag*/ 8);
        write_sim_console_body(b.writer(),
            { "SetCLInfo", "Entities/base", "", "" });
        // ESS terminator + PSC=0 + GSS=0
        b.writer().write_flag(false);
        b.writer().write_flag(false);
        b.writer().write_flag(false);
        const auto packet = b.finish(0);

        if (!is_setclinfo_clientready(packet)) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL setclinfo detect: missed SetCLInfo\n");
            ++failures;
        }

        // Negative: random small buffer should not match.
        std::vector<std::uint8_t> noise = { 0x07, 0x08, 0x09, 0x80 };
        if (is_setclinfo_clientready(noise)) {
            std::fprintf(stderr,
                "[tah-phase-reply] FAIL setclinfo detect: false positive on 4B ack\n");
            ++failures;
        }
    }

    if (failures == 0) {
        std::fputs("[tah-phase-reply] selftest OK\n", stderr);
        return 0;
    }
    std::fprintf(stderr,
        "[tah-phase-reply] selftest FAILED (%d)\n", failures);
    return 1;
}

}  // namespace

int tah_phase_reply_selftest()
{
    return run_selftest();
}

}  // namespace dts_viewer
