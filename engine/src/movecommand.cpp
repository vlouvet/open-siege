// Track 20 spec 19 — client-input ("movecommand") encoder (clean-room).
// See movecommand.hpp for the wire reference (clean-room §17).

#include "movecommand.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace net20 {

namespace {

// Map analog axis in [0.0, 1.0] to 4-bit wire value 0..15. Saturating
// clamp: <=0 -> 0, >=1 -> 15, else round to nearest int.
std::uint8_t quantize_axis(float v)
{
    if (!(v > 0.0f)) return 0;  // covers NaN and <= 0
    if (v >= 1.0f) return 15;
    const float scaled = v * 15.0f + 0.5f;
    int q = static_cast<int>(scaled);
    if (q < 0) q = 0;
    if (q > 15) q = 15;
    return static_cast<std::uint8_t>(q);
}

// Compare two moves for "axes/buttons identical" — used to set the
// per-move axes-redundant flag on the second+ moves (§17.4). Mouse
// deltas, trigger, and item-action are NOT part of this comparison;
// they are sent every move regardless.
bool same_axes_block(const MoveInput& a, const MoveInput& b)
{
    return
        quantize_axis(a.forward)  == quantize_axis(b.forward)
        && quantize_axis(a.backward) == quantize_axis(b.backward)
        && quantize_axis(a.left)     == quantize_axis(b.left)
        && quantize_axis(a.right)    == quantize_axis(b.right)
        && a.jet    == b.jet
        && a.jump   == b.jump
        && a.crouch == b.crouch;
}

// Write the per-move axes-and-button block (19 bits): forward,
// backward, left, right (4 bits each, LSB-first) + jet, jump, crouch
// (1 bit each).
void write_axes_block(BitWriter& w, const MoveInput& m)
{
    w.write_bits(quantize_axis(m.forward),  4);
    w.write_bits(quantize_axis(m.backward), 4);
    w.write_bits(quantize_axis(m.left),     4);
    w.write_bits(quantize_axis(m.right),    4);
    w.write_flag(m.jet);
    w.write_flag(m.jump);
    w.write_flag(m.crouch);
}

// Write a single 32-bit IEEE-754 LE float into the bitstream at the
// current cursor — NOT byte-aligned (§17.4). The engine writes raw
// memory little-endian; we replicate by writing the host-LE u32 32 bits
// LSB-first.
void write_float32_le(BitWriter& w, float f)
{
    static_assert(sizeof(float) == 4, "float must be 4 bytes");
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    // The host is little-endian on macOS arm64 / Linux x86_64. If we
    // ever target a big-endian host, we would byteswap here so the
    // wire u32 always matches the little-endian byte order the engine
    // wrote. Since the protocol's bitstream packs bit-by-bit, "byte
    // order" only matters in how we interpret the 32 bits — and we
    // write u LSB-first below, which yields LE byte order when those
    // 32 bits later land on a byte boundary.
    w.write_bits(u, 32);
}

// Write one move record (§17.4). `is_first_in_packet` controls whether
// the axes-redundant flag is emitted (it's only present from move #2
// onwards).
void write_move(BitWriter& w, const MoveInput& m, bool is_first_in_packet,
                const MoveInput& prev)
{
    bool redundant = false;
    if (!is_first_in_packet) {
        redundant = same_axes_block(m, prev);
        w.write_flag(redundant);
    }
    if (is_first_in_packet || !redundant) {
        write_axes_block(w, m);
    }
    // trigger / item-action-present / pitch-present / turn-present are
    // emitted on every move regardless of redundancy.
    w.write_flag(m.trigger);
    w.write_flag(m.item_action_present);
    if (m.item_action_present) {
        w.write_bits(m.item_action, 8);
    }
    const bool pitch_present = (m.pitch_delta != 0.0f);
    w.write_flag(pitch_present);
    if (pitch_present) {
        write_float32_le(w, m.pitch_delta);
    }
    const bool turn_present = (m.yaw_delta != 0.0f);
    w.write_flag(turn_present);
    if (turn_present) {
        write_float32_le(w, m.yaw_delta);
    }
}

}  // namespace

std::uint8_t quantize_fov(float fov_degrees)
{
    // Clamp range per §17.5 — engine clamps before quantization.
    float clamped = fov_degrees;
    if (!(clamped > 5.625f)) clamped = 5.625f;
    if (clamped > 120.0f)    clamped = 120.0f;
    const float wire_f = clamped / 135.0f * 255.0f;
    int q = static_cast<int>(wire_f + 0.5f);
    if (q < 0)   q = 0;
    if (q > 255) q = 255;
    return static_cast<std::uint8_t>(q);
}

std::vector<std::uint8_t> encode_movecommand(const MoveCommandInputs& inputs)
{
    BitWriter w;

    // ----- VC header (§14.2) -----
    w.write_flag(true);                       // bit 0: VC discriminator
    w.write_flag(inputs.connect_parity);       // bit 1: parity
    w.write_bits(inputs.send_seq & 0x1FFu, 9);                   // bits 2..10
    w.write_bits(inputs.highest_acked_of_mine & 0x1Fu, 5);       // bits 11..15
    for (const AckRun& r : inputs.ack_runs) {
        const std::uint8_t len = r.length == 0 ? 1
            : (r.length > 7 ? 7 : r.length);
        w.write_bits(len, 3);
        w.write_bits(r.start_seq & 0x1Fu, 5);
    }
    w.write_bits(0u, 3);                       // ack-list terminator
    w.write_bits(static_cast<std::uint32_t>(pkt_type::kDataPacket) & 0x1Fu, 5);

    // ----- Rate-control prefix (§3.4) -----
    // Steady-state: rate has been negotiated. R0 = 0, R1 = 0.
    w.write_flag(false);   // R0 = current-rate-changed = 0
    w.write_flag(false);   // R1 = max-rate-changed = 0

    // ----- Event sub-stream present (§16.2) -----
    w.write_flag(false);   // E = 0 — steady-state, no events

    // ----- Input sub-stream (§17.2) -----
    w.write_flag(true);    // P = 1 — input sub-stream present
    w.write_flag(false);   // input header leading bit = 0 (§17.7)
    w.write_bits(quantize_fov(inputs.fov_degrees), 8);
    w.write_bits(inputs.first_move_seq & 0xFFFFFFFFu, 32);

    // Per-move loop.
    MoveInput zero{};
    const MoveInput* prev = &zero;
    for (std::size_t i = 0; i < inputs.moves.size(); ++i) {
        w.write_flag(true);   // another-move-follows = 1
        write_move(w, inputs.moves[i], /*is_first_in_packet=*/i == 0, *prev);
        prev = &inputs.moves[i];
    }
    w.write_flag(false);   // another-move-follows = 0 (loop terminator)

    // ----- Ghost sub-stream present (§5.0.3) -----
    w.write_flag(false);   // G = 0 — client doesn't author ghosts

    // Trailing zero-pad to next byte boundary is implicit: the buffer is
    // sized to (bit_pos + 7) / 8 bytes; unused high bits stay zero.

    return std::move(w.bytes);
}

// Spec 28/02 — inverse of quantize_axis: wire 0..15 -> float in [0, 1].
static float dequantize_axis(std::uint8_t v)
{
    if (v >= 15) return 1.0f;
    return static_cast<float>(v) / 15.0f;
}

bool decode_movecommand(const std::uint8_t* data, std::size_t size,
                        MoveCommandInputs& out)
{
    BitReader r(data, size);

    // ----- VC header (§14.2) -----
    if (!r.read_flag()) return false;             // bit 0 must be 1
    out.connect_parity = r.read_flag();           // bit 1
    out.send_seq = static_cast<std::uint16_t>(r.read_bits(9));
    out.highest_acked_of_mine = static_cast<std::uint8_t>(r.read_bits(5));
    out.ack_runs.clear();
    while (true) {
        const std::uint8_t len = static_cast<std::uint8_t>(r.read_bits(3));
        if (r.fail()) return false;
        if (len == 0) break;                      // ack-list terminator
        const std::uint8_t start = static_cast<std::uint8_t>(r.read_bits(5));
        if (r.fail()) return false;
        AckRun run{};
        run.length = len;
        run.start_seq = start;
        out.ack_runs.push_back(run);
        if (out.ack_runs.size() > 64) return false;  // sanity
    }
    const std::uint8_t type_word = static_cast<std::uint8_t>(r.read_bits(5));
    if (type_word != pkt_type::kDataPacket) return false;

    // ----- Rate-control prefix -----
    (void)r.read_flag();                          // R0
    (void)r.read_flag();                          // R1

    // ----- Event sub-stream -----
    const bool e = r.read_flag();
    if (e) {
        // v1 server: we don't decode event-substream movecommand
        // payloads (acks/chat). Reject and leave them for spec 28/10
        // to wire up the inverse client_events::parse path.
        return false;
    }

    // ----- Input sub-stream -----
    const bool p = r.read_flag();
    if (!p) return false;
    if (r.read_flag()) return false;              // input header leading bit must be 0 (§17.7)
    const std::uint8_t fov_wire = static_cast<std::uint8_t>(r.read_bits(8));
    out.fov_degrees = (static_cast<float>(fov_wire) / 255.0f) * 135.0f;
    out.first_move_seq = r.read_bits(32);
    if (r.fail()) return false;

    out.moves.clear();
    MoveInput prev{};
    bool first = true;
    while (true) {
        const bool another = r.read_flag();
        if (r.fail()) return false;
        if (!another) break;
        MoveInput m{};
        bool redundant = false;
        if (!first) {
            redundant = r.read_flag();
        }
        if (first || !redundant) {
            const std::uint8_t fwd  = static_cast<std::uint8_t>(r.read_bits(4));
            const std::uint8_t back = static_cast<std::uint8_t>(r.read_bits(4));
            const std::uint8_t lf   = static_cast<std::uint8_t>(r.read_bits(4));
            const std::uint8_t rt   = static_cast<std::uint8_t>(r.read_bits(4));
            m.forward  = dequantize_axis(fwd);
            m.backward = dequantize_axis(back);
            m.left     = dequantize_axis(lf);
            m.right    = dequantize_axis(rt);
            m.jet      = r.read_flag();
            m.jump     = r.read_flag();
            m.crouch   = r.read_flag();
        } else {
            // Re-use prev's axes/buttons.
            m.forward  = prev.forward;
            m.backward = prev.backward;
            m.left     = prev.left;
            m.right    = prev.right;
            m.jet      = prev.jet;
            m.jump     = prev.jump;
            m.crouch   = prev.crouch;
        }
        m.trigger              = r.read_flag();
        m.item_action_present  = r.read_flag();
        if (m.item_action_present) {
            m.item_action = static_cast<std::uint8_t>(r.read_bits(8));
        }
        if (r.read_flag()) m.pitch_delta = r.read_float32_le();
        if (r.read_flag()) m.yaw_delta   = r.read_float32_le();
        if (r.fail()) return false;
        prev = m;
        out.moves.push_back(m);
        if (out.moves.size() > 64) return false;  // sanity cap
        first = false;
    }

    // Ghost sub-stream G flag follows; ignore for v1 server-side.
    (void)r.read_flag();
    // Trailing zero-pad to byte boundary is implicit.

    return !r.fail();
}

std::vector<std::uint8_t> encode_movecommand_worked_example()
{
    // §17.8 worked example — capture packet i=400. Inputs that recover
    // the documented bit-by-bit decode:
    //   VC header: send_seq=257, parity=1, highest_acked=31,
    //              one ack run (len=1, start=11), type=DataPacket
    //   Rate: R0=0, R1=0; Event present=0; Input present=1
    //   Input header bit=0; FOV wire=204 (108deg); first_move_seq=326
    //   Three moves:
    //     M1: all axes/buttons 0, no item, no mouse
    //     M2: axes-redundant (same as M1), trigger=0, no item, no mouse
    //     M3: axes-redundant, trigger=0, no item,
    //         pitch_delta=float-from-bytes(5e d1 76 bc) = -0.01506 rad,
    //         yaw_delta  =float-from-u32 0x3CB91D07     =  +0.02260 rad
    //   Loop terminator=0. Ghost-present we emit as 0 (out of scope).
    MoveCommandInputs in;
    in.send_seq = 257;
    in.connect_parity = true;
    in.highest_acked_of_mine = 31;
    in.ack_runs.push_back({1, 11});
    // Reproduce FOV wire=204 directly. quantize_fov(108.0) =
    //   round(108/135 * 255) = round(204.0) = 204. Use 108deg as input.
    in.fov_degrees = 108.0f;
    in.first_move_seq = 326;

    auto u32_to_float = [](std::uint32_t u) {
        float f = 0.0f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    MoveInput m1{};
    MoveInput m2{};   // identical axes => redundant
    MoveInput m3{};
    m3.pitch_delta = u32_to_float(0xBC76D15Eu);   // bytes 14..17 = 5e d1 76 bc LE
    m3.yaw_delta   = u32_to_float(0x3CB91D07u);   // recovered from bit-packed bits 145..176
    in.moves = { m1, m2, m3 };
    return encode_movecommand(in);
}

}  // namespace net20
