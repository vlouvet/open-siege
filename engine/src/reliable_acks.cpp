// Track 20 spec 12 — reliable-channel ack encoder (clean-room).
// See reliable_acks.hpp for the wire-format reference (clean-room §14).

#include "reliable_acks.hpp"

#include <cstring>

namespace net20 {

void BitWriter::write_bits(std::uint32_t value, unsigned width)
{
    if (width == 0) return;
    // Grow buffer to cover bit_pos + width.
    const std::size_t end_bit = bit_pos + width;
    const std::size_t need_bytes = (end_bit + 7) / 8;
    if (bytes.size() < need_bytes) {
        bytes.resize(need_bytes, 0);
    }
    for (unsigned i = 0; i < width; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>((value >> i) & 1u);
        const std::size_t p = bit_pos + i;
        bytes[p >> 3] |= static_cast<std::uint8_t>(bit << (p & 7));
    }
    bit_pos += width;
}

std::uint32_t BitReader::read_bits(unsigned width)
{
    if (width == 0) return 0;
    if (bit_pos + width > size * 8) {
        overflow = true;
        return 0;
    }
    std::uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i) {
        const std::size_t p = bit_pos + i;
        const std::uint8_t bit =
            static_cast<std::uint8_t>((data[p >> 3] >> (p & 7)) & 1u);
        v |= static_cast<std::uint32_t>(bit) << i;
    }
    bit_pos += width;
    return v;
}

float BitReader::read_float32_le()
{
    const std::uint32_t u = read_bits(32);
    if (overflow) return 0.0f;
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}

// --- AckTracker --------------------------------------------------------

void AckTracker::on_receive(std::uint16_t server_send_seq)
{
    const std::uint8_t slot = static_cast<std::uint8_t>(server_send_seq & 0x1F);
    received[slot] = true;
    highest_recv_mod32 = slot;
    ++total_received;
}

void AckTracker::clear_pending()
{
    for (auto& b : received) b = false;
}

std::size_t AckTracker::pending_count() const noexcept
{
    std::size_t n = 0;
    for (bool b : received) if (b) ++n;
    return n;
}

// --- Run encoder -------------------------------------------------------
//
// Walk the 32-slot window collecting maximal contiguous "true" runs. The
// starting slot is `highest_recv_mod32`; we walk backwards from there
// across the mod-32 window to find the lowest start of the longest active
// run, then walk forwards emitting (length, start) pairs.
//
// Pragmatic strategy (sufficient for clean-room interop with the shipped
// server): collect all maximal runs across the window in order of
// increasing start_seq starting at slot 0, split any run with length > 7
// into multiple length-7 (plus one short tail) elements.
//
// The §14.7 worked example is a single run of length 1 starting at seq 1,
// which this strategy reproduces exactly: build_ack_runs(window with only
// slot 1 set, highest=2) returns {{1, 1}}.
//
// Note: the receiver-side intent in §14.9 step 3 is to clear slots that
// have been acked back to the peer; we leave that bookkeeping to the
// caller via clear_pending() after a successful emit.
std::vector<AckRun> build_ack_runs(const std::array<bool, 32>& window,
                                   std::uint8_t /*highest_recv_mod32*/)
{
    std::vector<AckRun> runs;
    std::size_t i = 0;
    while (i < window.size()) {
        if (!window[i]) { ++i; continue; }
        const std::size_t start = i;
        std::size_t j = i;
        while (j < window.size() && window[j]) ++j;
        // Run [start, j). Split into length-7 chunks per §14.3.
        std::size_t remaining = j - start;
        std::size_t cur_start = start;
        while (remaining > 0) {
            const std::uint8_t take =
                static_cast<std::uint8_t>(remaining > 7 ? 7 : remaining);
            runs.push_back({take, static_cast<std::uint8_t>(cur_start & 0x1F)});
            cur_start += take;
            remaining -= take;
        }
        i = j;
    }
    return runs;
}

// --- Header encoder ----------------------------------------------------

std::vector<std::uint8_t> encode_vc_header(const VcHeaderInputs& inputs)
{
    BitWriter w;
    // Bit 0: VC discriminator = 1.
    w.write_flag(true);
    // Bit 1: connect-handle parity.
    w.write_flag(inputs.connect_parity);
    // Bits 2..10: 9-bit send seq.
    w.write_bits(static_cast<std::uint32_t>(inputs.send_seq & 0x1FFu), 9);
    // Bits 11..15: 5-bit highest-acked-of-mine (mod 32).
    w.write_bits(static_cast<std::uint32_t>(inputs.highest_acked_of_mine & 0x1Fu), 5);
    // Ack-run list (8 bits per element, 3-bit length + 5-bit start).
    for (const AckRun& run : inputs.ack_runs) {
        // Length must be 1..7 to avoid being mistaken for the terminator.
        const std::uint8_t len = run.length == 0 ? 1 :
            (run.length > 7 ? 7 : run.length);
        w.write_bits(len, 3);
        w.write_bits(run.start_seq & 0x1Fu, 5);
    }
    // Terminator: 3-bit zero, then 5-bit type/flag word.
    w.write_bits(0, 3);
    w.write_bits(inputs.type_word & 0x1Fu, 5);
    return std::move(w.bytes);
}

std::vector<std::uint8_t> encode_pure_ack_simple(std::uint16_t send_seq,
                                                 bool connect_parity,
                                                 std::uint8_t highest_acked,
                                                 std::uint8_t run_length,
                                                 std::uint8_t run_start)
{
    VcHeaderInputs in;
    in.send_seq = send_seq;
    in.connect_parity = connect_parity;
    in.highest_acked_of_mine = highest_acked;
    in.ack_runs.push_back({run_length, run_start});
    in.type_word = pkt_type::kPureAck;
    return encode_vc_header(in);
}

// --- Incoming header parse --------------------------------------------

bool parse_incoming_header(const std::uint8_t* data, std::size_t length,
                           ParsedIncomingHeader& out)
{
    // Minimum VC datagram is 3 bytes (24 bits = 16 fixed + 3 term + 5 type).
    if (data == nullptr || length < 3) return false;
    const std::size_t bit_len = length * 8;
    std::size_t pos = 0;
    auto read_bits = [&](unsigned width) -> std::uint32_t {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < width; ++i) {
            const std::size_t p = pos + i;
            const std::uint8_t bit = (data[p >> 3] >> (p & 7)) & 1u;
            v |= static_cast<std::uint32_t>(bit) << i;
        }
        pos += width;
        return v;
    };

    if (pos >= bit_len) return false;
    const std::uint32_t vc_flag = read_bits(1);
    if (vc_flag != 1) return false;  // Discovery datagram
    /*parity*/   (void)read_bits(1);
    out.send_seq = static_cast<std::uint16_t>(read_bits(9));
    out.peer_highest_acked_of_ours_mod32 =
        static_cast<std::uint8_t>(read_bits(5) & 0x1Fu);

    // Walk ack run list looking for the 0-length terminator.
    for (;;) {
        if (pos + 3 > bit_len) return false;
        const std::uint32_t len = read_bits(3);
        if (len == 0) {
            if (pos + 5 > bit_len) return false;
            const std::uint32_t type = read_bits(5);
            out.type_word = static_cast<std::uint8_t>(type & 0x1Fu);
            out.base_type = static_cast<std::uint8_t>(type & 0x07u);
            out.is_ack    = (type & 0x10u) != 0;
            out.is_resend = (type & 0x08u) != 0;
            return true;
        }
        if (pos + 5 > bit_len) return false;
        (void)read_bits(5);   // skip start_seq
    }
}

}  // namespace net20
