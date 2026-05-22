// Track 20 spec 03 — reliable event channel tests.
//
// Spec test paths from docs/clean-room-specs/TRIBES-NETPROTO.md §7 and
// the spec acceptance criteria: 100 events with simulated 5% loss arrive
// in order; sequence wrap-around at 128; send window stalls at 126.

#include <catch2/catch.hpp>

#include "content/net/bit_stream.hpp"
#include "content/net/reliable_channel.hpp"

#include <array>
#include <random>
#include <vector>

using namespace studio::content::net;

namespace {

ReliableEvent make_event(std::uint8_t tag, std::uint32_t counter)
{
    ReliableEvent ev;
    ev.class_tag = tag;
    ev.guaranteed = true;
    ev.ordered = true;
    ev.payload.resize(4);
    ev.payload[0] = static_cast<std::uint8_t>(counter & 0xFF);
    ev.payload[1] = static_cast<std::uint8_t>((counter >> 8) & 0xFF);
    ev.payload[2] = static_cast<std::uint8_t>((counter >> 16) & 0xFF);
    ev.payload[3] = static_cast<std::uint8_t>((counter >> 24) & 0xFF);
    return ev;
}

std::uint32_t event_counter(const ReliableEvent& ev)
{
    REQUIRE(ev.payload.size() >= 4);
    return  static_cast<std::uint32_t>(ev.payload[0])
         | (static_cast<std::uint32_t>(ev.payload[1]) << 8)
         | (static_cast<std::uint32_t>(ev.payload[2]) << 16)
         | (static_cast<std::uint32_t>(ev.payload[3]) << 24);
}

} // anonymous namespace

TEST_CASE("ReliableChannel: pack+unpack round-trips a single event",
          "[net][reliable]")
{
    ReliableChannel tx;
    REQUIRE(tx.send(make_event(/*tag=*/3, 42)).has_value());

    std::array<std::uint8_t, 64> buf{};
    BitStream w(buf.data(), buf.size());
    REQUIRE(tx.pack(w, w.capacity_bits()) == 1);

    BitStream r(buf.data(), buf.size());
    ReliableChannel rx;
    rx.unpack(r);

    auto got = rx.poll();
    REQUIRE(got.has_value());
    REQUIRE(got->class_tag == 3);
    REQUIRE(event_counter(*got) == 42);
}

TEST_CASE("ReliableChannel: 100 events with 5% simulated loss arrive in order",
          "[net][reliable]")
{
    constexpr int kEvents = 100;
    constexpr double kLossRate = 0.05;
    std::mt19937 rng(42);   // deterministic
    std::uniform_real_distribution<double> chance(0.0, 1.0);

    ReliableChannel tx, rx;
    for (int i = 1; i <= kEvents; ++i) {
        REQUIRE(tx.send(make_event(/*tag=*/1, i)).has_value());
    }

    std::vector<std::uint32_t> received_in_order;

    // Round-robin: pack a packet, maybe drop it; if delivered, unpack +
    // ack. If dropped, re-prepend its events to the send queue.
    int safety = 1000;
    while (received_in_order.size() < kEvents && safety-- > 0) {
        std::array<std::uint8_t, 200> buf{};
        BitStream w(buf.data(), buf.size());
        const std::size_t before = w.get_cur_pos();
        const std::size_t packed = tx.pack(w, w.capacity_bits());
        if (packed == 0) break;
        const std::size_t bits_written = w.get_cur_pos() - before;

        const bool dropped = (chance(rng) < kLossRate);

        // Pull the seqs out of the packet by re-parsing — easier than
        // mirroring the pack-side bookkeeping in the test.
        // For ack: walk in-flight by reading the packet a second time.
        // Simpler: hand the channel the contiguous range we just packed.
        std::vector<std::uint8_t> seqs;
        {
            BitStream peek(buf.data(), w.byte_position());
            std::uint8_t last_seq = 0; bool last_valid = false;
            while (peek.is_valid() && peek.bit_position() < bits_written) {
                if (!peek.read_flag()) break;
                if (!peek.read_flag()) break;   // unguaranteed events ignored here
                const bool cont = peek.read_flag();
                std::uint8_t seq = 0;
                if (cont) {
                    seq = last_valid ? static_cast<std::uint8_t>((last_seq + 1) & 0x7F) : 0;
                } else {
                    const bool has_seq = peek.read_flag();
                    if (!has_seq) { (void)peek.read_int(7); continue; }
                    seq = static_cast<std::uint8_t>(peek.read_int(7));
                }
                (void)peek.read_int(7);   // class tag
                std::uint8_t plen_bytes[2]{}; peek.read_bytes(plen_bytes, 2);
                const std::uint16_t plen = static_cast<std::uint16_t>(plen_bytes[0])
                    | (static_cast<std::uint16_t>(plen_bytes[1]) << 8);
                std::vector<std::uint8_t> payload(plen);
                if (plen) peek.read_bytes(payload.data(), plen);
                seqs.push_back(seq);
                last_seq = seq; last_valid = true;
            }
        }

        if (dropped) {
            // Drop the packet — re-prepend the events back into the send
            // queue at their original positions. We have to recreate the
            // events from the seqs we just observed. The channel doesn't
            // expose a "what was in that packet" API, so for the test we
            // reconstruct synthetically.
            std::vector<ReliableEvent> redelivered;
            for (auto s : seqs) {
                // Map back to original counter (1-based) — we know seq i
                // corresponds to event counter (i + 1) since they were sent
                // in order and we tagged sequentially starting at 0.
                redelivered.push_back(make_event(1, static_cast<std::uint32_t>(s) + 1));
            }
            tx.on_packet_dropped(std::move(redelivered));
            continue;
        }

        // Delivered: ack and unpack.
        tx.on_packet_acked(seqs);

        BitStream r(buf.data(), w.byte_position());
        rx.unpack(r);

        while (auto got = rx.poll()) {
            received_in_order.push_back(event_counter(*got));
        }
    }

    REQUIRE(received_in_order.size() == kEvents);
    for (int i = 0; i < kEvents; ++i) {
        REQUIRE(received_in_order[i] == static_cast<std::uint32_t>(i + 1));
    }
}

TEST_CASE("ReliableChannel: send window stalls at 126 unacked events",
          "[net][reliable]")
{
    ReliableChannel tx;
    int admitted = 0;
    for (int i = 0; i < 200; ++i) {
        if (tx.send(make_event(1, static_cast<std::uint32_t>(i))).has_value()) {
            ++admitted;
        }
    }
    REQUIRE(admitted == 126);
    REQUIRE(tx.send_window_full());
}

TEST_CASE("ReliableChannel: sequence wraps through 127 cleanly",
          "[net][reliable]")
{
    ReliableChannel tx, rx;
    constexpr int kEvents = 200;
    for (int i = 0; i < 126; ++i) {
        REQUIRE(tx.send(make_event(1, static_cast<std::uint32_t>(i + 1))).has_value());
    }

    std::vector<std::uint32_t> received;
    int sent_total = 126;
    int safety = 2000;

    while (received.size() < kEvents && safety-- > 0) {
        std::array<std::uint8_t, 1500> buf{};
        BitStream w(buf.data(), buf.size());
        std::size_t packed = tx.pack(w, w.capacity_bits());
        if (packed == 0) {
            // No more to pack right now — refill from the test source.
            while (sent_total < kEvents
                   && tx.send(make_event(1,
                       static_cast<std::uint32_t>(sent_total + 1))).has_value())
            {
                ++sent_total;
            }
            continue;
        }

        // Lossless delivery — collect the seqs and ack immediately.
        BitStream peek(buf.data(), w.byte_position());
        std::vector<std::uint8_t> seqs;
        std::uint8_t last_seq = 0; bool last_valid = false;
        while (peek.is_valid()) {
            if (!peek.read_flag()) break;
            (void)peek.read_flag();
            const bool cont = peek.read_flag();
            std::uint8_t seq = 0;
            if (cont) {
                seq = last_valid ? static_cast<std::uint8_t>((last_seq + 1) & 0x7F) : 0;
            } else {
                const bool has_seq = peek.read_flag();
                if (!has_seq) {
                    (void)peek.read_int(7);
                    std::uint8_t pl[2]{}; peek.read_bytes(pl, 2);
                    std::uint16_t pn = pl[0] | (pl[1] << 8);
                    std::vector<std::uint8_t> p(pn);
                    if (pn) peek.read_bytes(p.data(), pn);
                    continue;
                }
                seq = static_cast<std::uint8_t>(peek.read_int(7));
            }
            (void)peek.read_int(7);
            std::uint8_t pl[2]{}; peek.read_bytes(pl, 2);
            std::uint16_t pn = pl[0] | (pl[1] << 8);
            std::vector<std::uint8_t> p(pn);
            if (pn) peek.read_bytes(p.data(), pn);
            seqs.push_back(seq);
            last_seq = seq; last_valid = true;
        }
        tx.on_packet_acked(seqs);

        BitStream r(buf.data(), w.byte_position());
        rx.unpack(r);
        while (auto got = rx.poll()) {
            received.push_back(event_counter(*got));
        }

        // After acks, fill the window again from the remaining test events.
        while (sent_total < kEvents
               && tx.send(make_event(1,
                   static_cast<std::uint32_t>(sent_total + 1))).has_value())
        {
            ++sent_total;
        }
    }

    REQUIRE(received.size() == kEvents);
    for (int i = 0; i < kEvents; ++i) {
        REQUIRE(received[i] == static_cast<std::uint32_t>(i + 1));
    }
}
