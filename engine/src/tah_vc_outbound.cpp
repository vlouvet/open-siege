// Spec 26/14c — per-session outbound VC packet builder (clean-room).
// See tah_vc_outbound.hpp + docs/clean-room-specs/TRIBES-VC-OUTBOUND.md.

#include "tah_vc_outbound.hpp"

#include "session_table.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace dts_viewer
{
namespace tah_vc
{

bool should_bump_send_seq(std::uint8_t base_type, bool is_ack_flag) noexcept
{
    // Spec §4.1: DataPacket / Disconnect / RequestConnect / AcceptConnect /
    // RejectConnect BUMP. Ping (7) and pure-Ack (ack flag with base 0)
    // REUSE. The is_ack_flag input distinguishes a DataPacket-with-acks
    // (NOT a pure-ack — bumps because of the DataPacket body) from a
    // pure-Ack emit (no body, no bump). A "pure-Ack" is signalled when the
    // base type is DataPacket AND the ack flag is set.
    using namespace net20::pkt_type;
    if (base_type == kPing) return false;
    if (base_type == kDataPacket) {
        // Pure-Ack convention: base 0 + Ack flag means no body, no bump.
        return !is_ack_flag;
    }
    // Disconnect / RequestConnect / AcceptConnect / RejectConnect always bump.
    if (base_type == kDisconnect || base_type == kRequestConnect
        || base_type == kAcceptConnect || base_type == kRejectConnect) {
        return true;
    }
    // Reserved (6) or any unknown base: default to no bump (spec forbids
    // emitting these; the caller should never reach here).
    return false;
}

std::uint8_t compose_type_word(std::uint8_t base_type,
                               bool         is_resend,
                               bool         is_ack) noexcept
{
    using namespace net20::pkt_type;
    std::uint8_t v = static_cast<std::uint8_t>(base_type & 0x07u);
    if (is_resend) v = static_cast<std::uint8_t>(v | kResendFlag);
    if (is_ack)    v = static_cast<std::uint8_t>(v | kAckFlag);
    return static_cast<std::uint8_t>(v & 0x1Fu);
}

namespace
{

// Connection-control types embed the 32-bit connect_handle in the body.
bool is_connect_control(std::uint8_t base_type) noexcept
{
    using namespace net20::pkt_type;
    return base_type == kDisconnect
        || base_type == kRequestConnect
        || base_type == kAcceptConnect
        || base_type == kRejectConnect;
}

}  // namespace

OutboundPacket begin_outbound_packet(Session&                  sess,
                                     const OutboundPacketPlan& plan)
{
    OutboundPacket pkt;
    pkt.type_word = compose_type_word(plan.base_type,
                                      plan.is_resend, plan.is_ack);
    pkt.bumped_seq = should_bump_send_seq(plan.base_type, plan.is_ack);

    // Spec §4.3: bumping types USE the current next_send_seq value
    // (already pre-set to the next-to-emit number; see session_table.hpp
    // — fresh sessions start at next_send_seq = 1). Non-bumping types
    // re-use the previous data emit's value, i.e. (next_send_seq - 1)
    // mod 512. For a fresh session that has never emitted, that's 0.
    std::uint16_t seq_to_write;
    if (pkt.bumped_seq) {
        seq_to_write = static_cast<std::uint16_t>(sess.next_send_seq & 0x1FFu);
    } else {
        // SPEC-AMBIGUITY: spec §4.1 says the very first ever emit uses 0
        // for the initial send_seq pre-bump. With our convention of
        // next_send_seq starting at 1, the first pure-ack on a brand-new
        // session would compute (1 - 1) = 0. Once a real data emit has
        // happened, next_send_seq is the NEXT value (e.g. data emit
        // wrote 1 then bumped to 2, so pure-ack reuses 2 - 1 = 1).
        const std::uint16_t prev = static_cast<std::uint16_t>(
            (sess.next_send_seq + 0x200u - 1u) & 0x1FFu);
        seq_to_write = prev;
    }
    pkt.written_send_seq = seq_to_write;

    // Build the VC header bit-stream directly (mirrors the existing
    // encode_vc_header path but keeps the cursor under our control so the
    // body can be appended without a byte-alignment boundary — critical
    // for the connection-control 32-bit handle write).
    auto& w = pkt.writer;
    w.write_flag(true);                         // bit 0: VC discriminator
    w.write_flag(sess.connect_parity);          // bit 1: parity
    w.write_bits(static_cast<std::uint32_t>(seq_to_write & 0x1FFu), 9);
    w.write_bits(static_cast<std::uint32_t>(sess.ack.highest_recv_mod32 & 0x1Fu), 5);

    const auto runs = net20::build_ack_runs(sess.ack.received,
                                            sess.ack.highest_recv_mod32);
    for (const auto& run : runs) {
        const std::uint8_t len = run.length == 0 ? 1
                              : (run.length > 7 ? 7 : run.length);
        w.write_bits(len, 3);
        w.write_bits(static_cast<std::uint32_t>(run.start_seq & 0x1Fu), 5);
    }
    w.write_bits(0, 3);                         // terminator
    w.write_bits(static_cast<std::uint32_t>(pkt.type_word & 0x1Fu), 5);

    // Connection-control types: emit the 32-bit connect_handle here,
    // bit-packed at the current cursor (no alignment per §8 step 6).
    if (plan.emit_connect_handle && is_connect_control(plan.base_type)) {
        w.write_bits(sess.connect_handle, 32);
    }

    return pkt;
}

namespace
{

void commit_session_state(Session&             sess,
                          const OutboundPacket& pkt,
                          std::uint64_t        now_ms)
{
    if (pkt.bumped_seq) {
        sess.next_send_seq = static_cast<std::uint16_t>(
            (sess.next_send_seq + 1u) & 0x1FFu);
        // Spec §4.3 / engine convention: don't let send_seq be 0 — the
        // first real datagram in a session always carries seq 1. Wrap
        // 0 -> 1 to mirror what the rest of server_listener.cpp does.
        if (sess.next_send_seq == 0) sess.next_send_seq = 1;
    }
    if (now_ms != 0) sess.last_outbound_ms = now_ms;
}

}  // namespace

std::vector<std::uint8_t>
finalize_outbound_packet(Session&         sess,
                         OutboundPacket&& pkt,
                         std::uint64_t    now_ms)
{
    // Pad the bit cursor up to the next byte boundary by writing zero bits.
    const std::size_t bit_pos = pkt.writer.pos();
    const std::size_t pad = (8u - (bit_pos % 8u)) % 8u;
    if (pad > 0) pkt.writer.write_bits(0, static_cast<unsigned>(pad));

    commit_session_state(sess, pkt, now_ms);
    return std::move(pkt.writer.bytes);
}

// --- OutboundPacketBuilder --------------------------------------------------

OutboundPacketBuilder::OutboundPacketBuilder(Session&                  sess,
                                             const OutboundPacketPlan& plan)
    : sess_(sess), pkt_(begin_outbound_packet(sess, plan))
{
}

std::vector<std::uint8_t> OutboundPacketBuilder::finish(std::uint64_t now_ms)
{
    if (finished_) return {};
    finished_ = true;
    return finalize_outbound_packet(sess_, std::move(pkt_), now_ms);
}

// --- Selftest ---------------------------------------------------------------

namespace
{

// Build a Session in the state required by one of the spec's test
// vectors. We populate the minimum subset of fields the encoder reads.
Session make_session(std::uint32_t connect_handle,
                     std::uint16_t next_send_seq,
                     std::uint8_t  highest_acked_of_mine,
                     const std::array<bool, 32>& window)
{
    Session s{};
    s.connect_handle = connect_handle;
    s.connect_parity = (connect_handle & 1u) != 0;
    s.next_send_seq  = next_send_seq;
    s.ack.received   = window;
    s.ack.highest_recv_mod32 = highest_acked_of_mine;
    return s;
}

bool bytes_equal(const std::vector<std::uint8_t>& got,
                 const std::vector<std::uint8_t>& want,
                 const char* tag)
{
    if (got != want) {
        std::fprintf(stderr, "[tah-vc-outbound] FAIL %s\n", tag);
        std::fprintf(stderr, "  got (%zu B):", got.size());
        for (auto b : got) std::fprintf(stderr, " %02x", (unsigned)b);
        std::fprintf(stderr, "\n  want (%zu B):", want.size());
        for (auto b : want) std::fprintf(stderr, " %02x", (unsigned)b);
        std::fprintf(stderr, "\n");
        return false;
    }
    return true;
}

bool prefix_equal(const std::vector<std::uint8_t>& got,
                  const std::vector<std::uint8_t>& want_prefix,
                  const char* tag)
{
    if (got.size() < want_prefix.size()) {
        std::fprintf(stderr,
            "[tah-vc-outbound] FAIL %s: got %zu B want >= %zu B\n",
            tag, got.size(), want_prefix.size());
        return false;
    }
    for (std::size_t i = 0; i < want_prefix.size(); ++i) {
        if (got[i] != want_prefix[i]) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL %s: byte %zu got %02x want %02x\n",
                tag, i, (unsigned)got[i], (unsigned)want_prefix[i]);
            return false;
        }
    }
    return true;
}

int run_selftest()
{
    int failures = 0;

    // ---- Vector A: minimum-size pure-ack (4 bytes) ----
    {
        std::array<bool, 32> window{};
        window[1] = true;   // peer seq 1 pending
        // next_send_seq = 2 so that the pure-ack reuses (2-1)=1.
        Session s = make_session(/*handle*/ 0x03407b2c,
                                 /*next_send_seq*/ 2,
                                 /*highest_acked*/ 1,
                                 window);
        OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        plan.is_ack    = true;       // pure-ack
        plan.is_resend = false;
        OutboundPacketBuilder b(s, plan);
        auto wire = b.finish(/*now_ms*/ 0);
        const std::vector<std::uint8_t> want = { 0x05, 0x08, 0x09, 0x80 };
        if (!bytes_equal(wire, want, "vector A (pure-ack 4B)")) ++failures;
        // pure-ack does NOT bump send_seq.
        if (s.next_send_seq != 2) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector A: send_seq bumped to %u (want 2)\n",
                (unsigned)s.next_send_seq);
            ++failures;
        }
    }

    // ---- Vector B: first AcceptConnect emit (18 bytes) ----
    {
        std::array<bool, 32> window{};
        window[1] = true;
        // next_send_seq = 1 so AcceptConnect writes 1 and bumps to 2.
        Session s = make_session(/*handle*/ 0x03407b2c,
                                 /*next_send_seq*/ 1,
                                 /*highest_acked*/ 0,
                                 window);
        OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kAcceptConnect;
        plan.is_resend = true;    // server convention per spec §5.3
        OutboundPacketBuilder b(s, plan);
        // Append the 10-byte opaque accept-payload. Since the header ended
        // exactly at byte 4 (32 bits) AND the connect_handle is written at
        // bit 32..63 (also byte-aligned in this case), our cursor sits at
        // byte 8 boundary. Writing 8 bits at a time emits raw bytes.
        const std::uint8_t body[10] = {
            0x01, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x80, 0x01, 0x01
        };
        for (auto byte : body) b.writer().write_bits(byte, 8);
        auto wire = b.finish(0);
        const std::vector<std::uint8_t> want = {
            0x05, 0x00, 0x09, 0x60,
            0x2c, 0x7b, 0x40, 0x03,
            0x01, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x80, 0x01, 0x01
        };
        if (!bytes_equal(wire, want, "vector B (AC 18B)")) ++failures;
        if (s.next_send_seq != 2) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector B: send_seq=%u after AC (want 2)\n",
                (unsigned)s.next_send_seq);
            ++failures;
        }
    }

    // ---- Vector C: first DataPacket of the burst (header + rate prefix) ----
    {
        std::array<bool, 32> window{};
        window[1] = true;
        // next_send_seq = 2 so DataPacket writes 2 and bumps to 3.
        Session s = make_session(/*handle*/ 0x03407b2c,
                                 /*next_send_seq*/ 2,
                                 /*highest_acked*/ 1,
                                 window);
        OutboundPacketPlan plan;  // DataPacket, no flags.
        OutboundPacketBuilder b(s, plan);
        // Rate prefix: R0=0 (no current-rate change), R1=1 (max-rate change),
        // then 10 bits max update-delay = 66 ms, 10 bits max packet size = 400.
        b.writer().write_flag(false);            // R0
        b.writer().write_flag(true);             // R1
        b.writer().write_bits(66, 10);           // max update-delay
        b.writer().write_bits(400, 10);          // max packet size
        auto wire = b.finish(0);
        // Spec vector C cites the first 7 bytes including 2 bits of
        // sub-stream content past the rate prefix. Our encoder produces
        // header (32 bits) + 22-bit rate prefix = 54 bits = 7 bytes with
        // the top 2 bits of byte 6 zero (padding). Verify bytes 0..5
        // exactly and the low 6 bits of byte 6 (the rate-prefix tail).
        const std::vector<std::uint8_t> want_first6 = {
            0x09, 0x08, 0x09, 0x00, 0x0a, 0x01
        };
        if (!prefix_equal(wire, want_first6, "vector C (DataPacket hdr+rate first 6B)"))
            ++failures;
        // Byte 6 low 6 bits must match the rate-prefix tail of 0xd9.
        if (wire.size() < 7 || (wire[6] & 0x3f) != (0xd9 & 0x3f)) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector C: byte6 low6=0x%02x want 0x%02x\n",
                (unsigned)(wire.size() >= 7 ? wire[6] & 0x3f : 0u),
                (unsigned)(0xd9 & 0x3f));
            ++failures;
        }
        if (s.next_send_seq != 3) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector C: send_seq=%u after DP (want 3)\n",
                (unsigned)s.next_send_seq);
            ++failures;
        }
    }

    // ---- Vector D: second DataPacket of burst (R0=R1=0 rate prefix) ----
    {
        std::array<bool, 32> window{};
        window[1] = true;
        // next_send_seq = 3 so DataPacket writes 3 and bumps to 4.
        Session s = make_session(/*handle*/ 0x03407b2c,
                                 /*next_send_seq*/ 3,
                                 /*highest_acked*/ 1,
                                 window);
        OutboundPacketPlan plan;
        OutboundPacketBuilder b(s, plan);
        b.writer().write_flag(false);            // R0
        b.writer().write_flag(false);            // R1
        // The spec specifies the first 5 wire bytes; byte 4 is the
        // first body byte. Header 4 B + 2 rate-prefix bits → bytes 0..4
        // with byte 4 having its low 2 bits = 0. The remaining 6 bits of
        // byte 4 are per-sub-stream content (0xdc = 1101 1100; bits 2..7
        // = 1,1,1,0,1,1 LSB-first). Append those 6 bits to reproduce.
        b.writer().write_bits(/*value*/ 0b110111u, 6);
        auto wire = b.finish(0);
        const std::vector<std::uint8_t> want_prefix = {
            0x0d, 0x08, 0x09, 0x00, 0xdc
        };
        if (!prefix_equal(wire, want_prefix, "vector D (DataPacket no-rate-change)"))
            ++failures;
        if (s.next_send_seq != 4) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector D: send_seq=%u after DP (want 4)\n",
                (unsigned)s.next_send_seq);
            ++failures;
        }
    }

    // ---- Vector E: synthetic multi-run pure-ack (8/6 bytes) ----
    {
        std::array<bool, 32> window{};
        for (int i = 0; i <= 6; ++i)  window[i] = true;        // run @0 len 7
        window[10] = true;                                      // run @10 len 1
        for (int i = 20; i <= 26; ++i) window[i] = true;        // run @20 len 7
        // next_send_seq = 18 so pure-ack reuses (18-1)=17.
        Session s = make_session(/*handle*/ 0x00000001,   // LSB=1 → parity 1
                                 /*next_send_seq*/ 18,
                                 /*highest_acked*/ 30,
                                 window);
        OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        plan.is_ack    = true;
        OutboundPacketBuilder b(s, plan);
        auto wire = b.finish(0);
        const std::vector<std::uint8_t> want = {
            0x47, 0xf0, 0x07, 0x51, 0xa7, 0x80
        };
        if (!bytes_equal(wire, want, "vector E (multi-run pure-ack)")) ++failures;
        if (s.next_send_seq != 18) {
            std::fprintf(stderr,
                "[tah-vc-outbound] FAIL vector E: send_seq bumped to %u (want 18)\n",
                (unsigned)s.next_send_seq);
            ++failures;
        }
    }

    // ---- Round-trip via parse_incoming_header on each vector ----
    {
        struct Case {
            const char* name;
            std::uint16_t want_seq;
            std::uint8_t  want_type_word;
            bool          want_is_ack;
            bool          want_is_resend;
            std::vector<std::uint8_t> bytes;
        };
        const Case cases[] = {
            { "A", 1, 0x10, true, false, { 0x05, 0x08, 0x09, 0x80 } },
            { "B", 1, 0x0c, false, true,  { 0x05, 0x00, 0x09, 0x60,
                                            0x2c, 0x7b, 0x40, 0x03,
                                            0x01, 0x08, 0x00, 0x00,
                                            0x00, 0x08, 0x00, 0x80,
                                            0x01, 0x01 } },
            { "C", 2, 0x00, false, false, { 0x09, 0x08, 0x09, 0x00,
                                            0x0a, 0x01, 0xd9 } },
            { "D", 3, 0x00, false, false, { 0x0d, 0x08, 0x09, 0x00, 0xdc } },
            { "E", 17,0x10, true, false,  { 0x47, 0xf0, 0x07, 0x51, 0xa7, 0x80 } },
        };
        for (const auto& c : cases) {
            net20::ParsedIncomingHeader ph;
            if (!net20::parse_incoming_header(c.bytes.data(), c.bytes.size(), ph)) {
                std::fprintf(stderr,
                    "[tah-vc-outbound] FAIL roundtrip %s: parse_incoming_header rejected\n",
                    c.name);
                ++failures;
                continue;
            }
            if (ph.send_seq != c.want_seq) {
                std::fprintf(stderr,
                    "[tah-vc-outbound] FAIL roundtrip %s: send_seq=%u want %u\n",
                    c.name, (unsigned)ph.send_seq, (unsigned)c.want_seq);
                ++failures;
            }
            if (ph.type_word != c.want_type_word) {
                std::fprintf(stderr,
                    "[tah-vc-outbound] FAIL roundtrip %s: type_word=%02x want %02x\n",
                    c.name, (unsigned)ph.type_word, (unsigned)c.want_type_word);
                ++failures;
            }
            if (ph.is_ack != c.want_is_ack || ph.is_resend != c.want_is_resend) {
                std::fprintf(stderr,
                    "[tah-vc-outbound] FAIL roundtrip %s: flags ack=%d resend=%d want ack=%d resend=%d\n",
                    c.name, (int)ph.is_ack, (int)ph.is_resend,
                    (int)c.want_is_ack, (int)c.want_is_resend);
                ++failures;
            }
        }
    }

    if (failures == 0) {
        std::fputs("[tah-vc-outbound] selftest OK (5 vectors + roundtrip)\n", stderr);
    } else {
        std::fprintf(stderr,
            "[tah-vc-outbound] selftest FAILED (%d failures)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int tah_vc_outbound_selftest()
{
    return run_selftest();
}

}  // namespace tah_vc

int tah_vc_outbound_selftest()
{
    return tah_vc::tah_vc_outbound_selftest();
}

}  // namespace dts_viewer
