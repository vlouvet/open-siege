// Track 20 spec 10 — ghost-stream framing parser (clean-room).
// Spec 29 extension: optional registry-aware scoring lets the scanner
// accept normal-mode delta candidates (the bulk per-tick traffic),
// not just scope-always introductions.
// See ghost_stream.hpp for the wire format reference.

#include "ghost_stream.hpp"
#include "ghost_types.hpp"   // for GhostRegistry::ghost_kinds lookup

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace net20 {

namespace {

// Bit cursor over a fixed-length byte buffer. LSB-first within each byte:
//   bit(N) = (data[N >> 3] >> (N & 7)) & 1.
struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t bit_length = 0;   // total bits in `data`
    std::size_t pos = 0;          // current bit position
    bool overrun = false;

    explicit BitReader(const std::uint8_t* d, std::size_t bytes) noexcept
        : data(d), bit_length(bytes * 8) {}

    std::size_t remaining() const noexcept {
        return pos > bit_length ? 0 : bit_length - pos;
    }

    // Read `width` bits (1..32) as an unsigned integer.
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

    // Peek without advancing.
    std::uint32_t peek_bits(std::size_t at, unsigned width) const noexcept {
        if (width == 0) return 0;
        if (width > 32 || at + width > bit_length) return 0;
        std::uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i) {
            const std::size_t p = at + i;
            const std::uint8_t bit = (data[p >> 3] >> (p & 7)) & 1u;
            value |= static_cast<std::uint32_t>(bit) << i;
        }
        return value;
    }
};

// Walk the VC header. Per spec §3.1/§5.0.1 the header is:
//   bit 0       : 1-bit VC-present flag (must be 1 for in-band traffic)
//   bit 1       : 1-bit connect-seq parity
//   bits 2..10  : 9-bit send seq
//   bits 11..15 : 5-bit highest received seq
//   bits 16..   : run-length ack list of (3-bit count, 5-bit start) entries
//                 terminated by a 3-bit zero count.
//   then        : 5-bit packet type.
//
// Each ack-list count field is the literal count in 1..7 (per §5.0.1 amendment).
// Returns false on framing failure.
bool decode_vc_header(BitReader& br, GhostPacketDecode& out) {
    if (br.bit_length < 32) {
        out.note = "packet too short for VC header";
        return false;
    }
    out.vc_present = br.read_flag();
    if (!out.vc_present) {
        out.note = "VC-present flag is 0 (discovery datagram, not in-band)";
        return false;
    }
    out.connect_parity = br.read_flag();
    out.send_seq = static_cast<std::uint16_t>(br.read_bits(9));
    out.highest_recv = static_cast<std::uint8_t>(br.read_bits(5));

    // Ack run-length list. Read (3-bit count, 5-bit start) until count == 0,
    // then the 5-bit packet type follows in place of the trailing start field.
    while (true) {
        const std::uint32_t count = br.read_bits(3);
        if (br.overrun) {
            out.note = "ack list overran header";
            return false;
        }
        if (count == 0) {
            out.packet_type = static_cast<std::uint8_t>(br.read_bits(5));
            break;
        }
        // Skip the 5-bit start-seq for this run; we don't surface ack state here.
        (void)br.read_bits(5);
        if (br.overrun) {
            out.note = "ack list overran header";
            return false;
        }
    }
    out.header_bits = br.pos;
    return true;
}

// Walk the rate-control prefix (two 1+20-bit conditional fields per §3.4).
void decode_rate_prefix(BitReader& br, GhostPacketDecode& out) {
    out.rate_changed = br.read_flag();
    if (out.rate_changed) {
        out.rate_update_delay_ms = static_cast<std::uint16_t>(br.read_bits(10));
        out.rate_packet_size = static_cast<std::uint16_t>(br.read_bits(10));
    }
    out.max_rate_changed = br.read_flag();
    if (out.max_rate_changed) {
        out.max_update_delay_ms = static_cast<std::uint16_t>(br.read_bits(10));
        out.max_packet_size = static_cast<std::uint16_t>(br.read_bits(10));
    }
    out.rate_prefix_end_bit = br.pos;
}

// Try to interpret the bit range starting at `at` as the body of a
// ghost-sub-stream. Returns a candidate score:
//   * 0  = does not look like a ghost stream at all.
//   * >0 = looks plausible; higher score = more confident.
//
// Decodes the first record per the spec §5.0.3 framing. Fills `record` on
// success. Does NOT attempt to walk the per-class payload (spec 13 deferred).
//
// Validation rules (per spec §5.0.3, §5.0.4):
//   * mode flag is in {0, 1}.
//   * width selector S in 0..7 → idW = S + 3.
//   * first object-present must be 1 (else this packet had no ghost records,
//     unlikely for a large s→c packet).
//   * ghost_id < (1 << idW).
//   * if scope-always mode AND new-id (assumed true for any first-record on
//     the bulk stream), class_tag in 1..1023.
int score_ghost_start_candidate(const BitReader& br, std::size_t at,
                                GhostUpdate& record,
                                const GhostRegistry* known) {
    const std::size_t total = br.bit_length;
    if (at >= total) return 0;
    // Minimum bits needed to score: mode(1) + selector(3) + presence(1)
    // + id(3..10) + kill(1) = at most 16 bits before deciding on payload.
    if (total - at < 16) return 0;

    BitReader local = br;
    local.pos = at;

    const bool mode = local.read_flag();
    const std::uint32_t selector = local.read_bits(3);
    const unsigned id_width = static_cast<unsigned>(selector + 3);
    if (local.overrun) return 0;

    const bool object_present = local.read_flag();
    if (!object_present) {
        // An immediate terminator is technically legal but means the
        // sender packed no records. Score very low.
        return 1;
    }

    if (local.pos + id_width + 1 > total) return 0;
    const std::uint32_t ghost_id = local.read_bits(id_width);
    if (ghost_id >= (1u << id_width)) return 0;  // tautology, but defensive

    const bool kill = local.read_flag();
    if (local.overrun) return 0;

    record.start_bit = at;
    record.ghost_id = static_cast<std::uint16_t>(ghost_id);
    record.kill = kill;
    record.scope_always_mode = mode;
    record.id_width_bits = static_cast<std::uint8_t>(id_width);
    record.full_update = false;
    record.object_id = 0;
    record.class_tag = 0;

    // Base plausibility score. mode==1 + valid ghost_id + valid kill = +1.
    int score = 1;

    if (mode && !kill) {
        // Scope-always mode + not-killed = new introduction expected.
        // 32-bit object_id + 10-bit class_tag must follow.
        if (local.pos + 32 + 10 > total) return score;
        record.object_id = local.read_bits(32);
        record.class_tag = static_cast<std::uint16_t>(local.read_bits(10));
        record.full_update = true;

        // Class tag must be in 1..1023 (0 reserved per §5.0.3).
        if (record.class_tag == 0 || record.class_tag > 1023) {
            return 0;  // disqualified
        }
        // Boost score: a 10-bit field that must be nonzero is a strong
        // signal. We're roughly 1024x more confident than a random offset.
        score += 1024;
    } else if (!mode) {
        // Normal-mode delta. We can't disambiguate further without the
        // per-class payload; treat as moderately plausible if reachable.
        score += 4;
        // Spec 29: if the registry knows this ghost_id, that's strong
        // evidence we found a valid delta-stream start. Boost score
        // enough to clear the acceptance threshold but stay below the
        // scope-always-intro signal (1024).
        if (known) {
            auto it = known->ghost_kinds.find(
                static_cast<std::uint16_t>(ghost_id));
            if (it != known->ghost_kinds.end()) {
                score += 512;
            }
        }
    } else {
        // Scope-always + kill: rare but legal.
        score += 2;
    }

    return score;
}

// Scan over candidate bit offsets and return the highest-scoring one.
// Strategy: try every bit offset in [start, bit_length - 16). For each,
// score it; remember the best. This is O(N) bits × O(1) per check.
//
// Returns std::nullopt if no plausible candidate was found.
std::optional<std::size_t> scan_for_ghost_stream(const BitReader& br,
                                                 std::size_t start_bit,
                                                 GhostUpdate& best_record,
                                                 const GhostRegistry* known) {
    int best_score = 0;
    std::optional<std::size_t> best_at;
    GhostUpdate scratch;

    const std::size_t end = br.bit_length > 16 ? br.bit_length - 16 : 0;
    for (std::size_t at = start_bit; at < end; ++at) {
        scratch = GhostUpdate{};
        const int s = score_ghost_start_candidate(br, at, scratch, known);
        if (s > best_score) {
            best_score = s;
            best_at = at;
            best_record = scratch;
        }
    }
    // Acceptance thresholds:
    //   * 1024 = scope-always + valid 10-bit class_tag (very high confidence)
    //   * 512  = normal-mode-delta + first ghost_id matches a known one
    //            (spec 29: only available when caller passes a non-null
    //            registry, otherwise normal-mode candidates max out at 5).
    const int threshold = known ? 256 : 1024;
    if (best_score < threshold) {
        return std::nullopt;
    }
    return best_at;
}

}  // namespace

GhostPacketDecode parse_ghost_packet(const std::uint8_t* data,
                                     std::size_t length,
                                     const GhostRegistry* known) {
    GhostPacketDecode out;
    if (data == nullptr || length == 0) {
        out.note = "empty packet";
        return out;
    }

    BitReader br(data, length);

    if (!decode_vc_header(br, out)) {
        return out;
    }
    if (out.packet_type != 0) {
        // Non-DataPacket carries no ghost stream. Caller may still want
        // the decoded header.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "packet_type=%u (not DataPacket)",
                      static_cast<unsigned>(out.packet_type));
        out.note = buf;
        return out;
    }

    decode_rate_prefix(br, out);

    if (br.overrun) {
        out.note = "rate prefix overran packet";
        return out;
    }

    // The event-sub-stream-present flag is at br.pos. We don't know how to
    // walk past the event sub-stream or the input/control sub-stream
    // without per-class payload widths (spec 13). Use the scanning strategy
    // documented in spec §5.0.7 #1.
    GhostUpdate first;
    auto found = scan_for_ghost_stream(br, br.pos, first, known);
    if (!found) {
        out.note = "no plausible ghost-sub-stream found";
        return out;
    }
    out.ghost_stream_start_bit = *found;
    out.updates.push_back(first);
    return out;
}

std::string format_update(const GhostUpdate& u) {
    std::ostringstream os;
    os << "id=" << u.ghost_id
       << " mode=" << (u.scope_always_mode ? "scope-always" : "normal")
       << " idW=" << static_cast<int>(u.id_width_bits)
       << " kill=" << (u.kill ? "Y" : "N")
       << " full=" << (u.full_update ? "Y" : "N");
    if (u.full_update) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " obj_id=0x%08x class_tag=%u",
                      u.object_id, static_cast<unsigned>(u.class_tag));
        os << buf;
    }
    os << " bit_offset=" << u.start_bit;
    return os.str();
}

}  // namespace net20
