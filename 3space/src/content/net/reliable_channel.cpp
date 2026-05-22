#include "content/net/reliable_channel.hpp"

#include <algorithm>

namespace studio::content::net
{

namespace
{
constexpr std::uint8_t kSeqMask = 0x7F;  // 7-bit sequence
}

bool ReliableChannel::seq_less_eq(std::uint8_t a, std::uint8_t b)
{
    // Modular comparison over 7-bit space: a <= b within a 64-step window.
    return static_cast<std::uint8_t>(b - a) <= 64;
}

std::uint8_t ReliableChannel::seq_diff(std::uint8_t a, std::uint8_t b)
{
    return static_cast<std::uint8_t>((a - b) & kSeqMask);
}

void ReliableChannel::set_acked_bit(std::uint8_t seq)
{
    seq &= kSeqMask;
    const std::size_t word = (seq >> 5) & 3;
    const std::uint32_t bit = 1u << (seq & 31);
    acked_mask_[word] |= bit;
}

bool ReliableChannel::acked_bit(std::uint8_t seq) const
{
    seq &= kSeqMask;
    const std::size_t word = (seq >> 5) & 3;
    const std::uint32_t bit = 1u << (seq & 31);
    return (acked_mask_[word] & bit) != 0;
}

bool ReliableChannel::send_window_full() const
{
    // Spec §3 and §7.2 — at most 126 ordered events ahead of last_acked.
    const std::uint8_t span = seq_diff(next_send_seq_, last_acked_seq_);
    return span >= 126;
}

std::size_t ReliableChannel::in_flight_count() const
{
    return in_flight_.size();
}

std::optional<std::uint8_t> ReliableChannel::send(ReliableEvent ev)
{
    if (ev.guaranteed && ev.ordered) {
        if (send_window_full()) return std::nullopt;
        ev.seq = next_send_seq_;
        next_send_seq_ = (next_send_seq_ + 1) & kSeqMask;
        send_queue_.push_back(std::move(ev));
        return send_queue_.back().seq;
    }
    // Non-ordered events: don't burn a sequence number; receiver just
    // delivers them as-they-arrive.
    ev.seq = 0;
    send_queue_.push_back(std::move(ev));
    return std::nullopt;
}

std::size_t ReliableChannel::pack(BitStream& s, std::size_t max_payload_bits)
{
    last_packed_valid_ = false;
    last_packed_seq_   = 0;

    const std::size_t start_pos = s.get_cur_pos();
    const std::size_t budget_end = std::min(s.capacity_bits(),
                                            start_pos + max_payload_bits);
    std::size_t packed_count = 0;

    while (!send_queue_.empty()) {
        const std::size_t event_start = s.get_cur_pos();
        ReliableEvent& ev = send_queue_.front();

        // Each event needs at minimum:
        //   1 present + 1 guaranteed + (1 cont + 1 has_seq + 7 seq if guaranteed)
        //   + 7 class tag + payload + final 1-bit terminator
        const std::size_t payload_bits = ev.payload.size() * 8;
        // Worst-case prefix (guaranteed, non-continuous, explicit seq):
        //   1 + 1 + 1 + 1 + 7 + 7 = 18 bits
        // (For non-guaranteed: 1 + 1 + 7 = 9 bits.) We use 18 as the upper
        // bound; the actual write may use fewer.
        const std::size_t prefix_bits_max = ev.guaranteed ? 18 : 9;
        // +1 for the trailing event-present=0 terminator bit.
        if (event_start + prefix_bits_max + payload_bits + 1 > budget_end) {
            break;
        }

        // Pack: present=1
        s.write_flag(true);
        // guaranteed
        s.write_flag(ev.guaranteed);

        bool continuous = false;
        if (ev.guaranteed) {
            if (ev.ordered) {
                if (last_packed_valid_
                    && ev.seq == ((last_packed_seq_ + 1) & kSeqMask))
                {
                    continuous = true;
                }
                s.write_flag(continuous);
                if (!continuous) {
                    // has-explicit-seq = 1 (we never send unsequenced
                    // guaranteed events in this iteration; that path would
                    // write a 0 instead).
                    s.write_flag(true);
                    s.write_int(ev.seq, 7);
                }
                last_packed_seq_ = ev.seq;
                last_packed_valid_ = true;
            } else {
                // guaranteed but unordered: spec says seq-continuous=1 if
                // continuous, else 0+has-explicit-seq=0 (no sequence at all).
                s.write_flag(false);   // not continuous
                s.write_flag(false);   // has-explicit-seq = 0
            }
        }
        // class tag (7 bits)
        s.write_int(ev.class_tag & 0x7F, 7);
        // Payload: 16-bit length prefix (LE byte-aligned) + raw bytes.
        // The spec leaves the per-event payload encoding to the event class
        // itself; for the opaque byte-array API we prepend a length so the
        // receiver can sync. Typed-event subclasses can suppress this if
        // they encode their own length.
        const std::uint16_t plen = static_cast<std::uint16_t>(ev.payload.size());
        std::uint8_t plen_bytes[2] = {
            static_cast<std::uint8_t>(plen & 0xFF),
            static_cast<std::uint8_t>((plen >> 8) & 0xFF),
        };
        s.write_bytes(plen_bytes, 2);
        if (!ev.payload.empty()) {
            s.write_bytes(ev.payload.data(), ev.payload.size());
        }

        if (!s.is_valid() || s.get_cur_pos() > budget_end) {
            // Overflow: rewind to event_start, restore the present bit to 0,
            // and stop.
            s.set_cur_pos(event_start);
            break;
        }

        // Move into in-flight (for retransmit support) or drop.
        if (ev.guaranteed) {
            in_flight_.push_back(std::move(ev));
        }
        send_queue_.pop_front();
        ++packed_count;
    }

    // Terminator: present=0
    if (s.get_cur_pos() < s.capacity_bits()) {
        s.write_flag(false);
    }
    return packed_count;
}

void ReliableChannel::unpack(BitStream& s)
{
    std::uint8_t last_received_seq = 0;
    bool last_received_valid = false;

    while (s.is_valid()) {
        const bool present = s.read_flag();
        if (!present) break;
        const bool guaranteed = s.read_flag();

        ReliableEvent ev;
        ev.guaranteed = guaranteed;
        ev.ordered = false;

        if (guaranteed) {
            const bool continuous = s.read_flag();
            if (continuous) {
                if (last_received_valid) {
                    ev.seq = (last_received_seq + 1) & kSeqMask;
                } else {
                    // Continuous with no predecessor — corpus-edge case.
                    // Treat as fresh sequence 0.
                    ev.seq = 0;
                }
                ev.ordered = true;
            } else {
                const bool has_explicit = s.read_flag();
                if (has_explicit) {
                    ev.seq = static_cast<std::uint8_t>(s.read_int(7));
                    ev.ordered = true;
                } else {
                    ev.ordered = false;   // unordered guaranteed
                }
            }
        }

        ev.class_tag = static_cast<std::uint8_t>(s.read_int(7));
        // Symmetric with the pack side: 16-bit length prefix + payload.
        std::uint8_t plen_bytes[2]{};
        s.read_bytes(plen_bytes, 2);
        const std::uint16_t plen = static_cast<std::uint16_t>(plen_bytes[0])
            | (static_cast<std::uint16_t>(plen_bytes[1]) << 8);
        if (plen > 0) {
            ev.payload.resize(plen);
            s.read_bytes(ev.payload.data(), plen);
        }

        if (ev.ordered) {
            last_received_seq = ev.seq;
            last_received_valid = true;
        }

        if (!s.is_valid()) break;

        if (ev.ordered) {
            // Holding-buffer drain pattern. A 7-bit sequence space wraps,
            // so we test "already delivered" via modular distance: an
            // incoming seq is a duplicate iff `(next_expected_seq_ - seq)
            // mod 128` lies in [1, 64). The 64-wide window matches the
            // spec's reliable-channel send-window cap (126/2).
            const std::uint8_t backstep = static_cast<std::uint8_t>(
                (next_expected_seq_ - ev.seq) & kSeqMask);
            const bool duplicate = (backstep > 0 && backstep < 64);
            if (duplicate) continue;
            // Already buffered (out-of-order duplicate)?
            if (recv_holding_[ev.seq].has_value()) continue;
            recv_holding_[ev.seq] = std::move(ev);

            while (true) {
                auto& slot = recv_holding_[next_expected_seq_];
                if (!slot.has_value()) break;
                delivered_.push_back(std::move(*slot));
                slot.reset();
                next_expected_seq_ = (next_expected_seq_ + 1) & kSeqMask;
            }
        } else {
            // Unordered events bypass the holding buffer.
            delivered_.push_back(std::move(ev));
        }
    }
}

std::optional<ReliableEvent> ReliableChannel::poll()
{
    if (delivered_.empty()) return std::nullopt;
    ReliableEvent ev = std::move(delivered_.front());
    delivered_.pop_front();
    return ev;
}

void ReliableChannel::on_packet_acked(const std::vector<std::uint8_t>& seqs)
{
    for (const auto seq : seqs) {
        set_acked_bit(seq);
    }
    // Update last_acked high-water by scanning in-flight: any contiguous
    // run that is now acked can be retired.
    while (!in_flight_.empty()) {
        const auto& head = in_flight_.front();
        if (!head.ordered) {
            in_flight_.pop_front();
            continue;
        }
        if (!acked_bit(head.seq)) break;
        last_acked_seq_ = head.seq;
        in_flight_.pop_front();
    }
}

void ReliableChannel::on_packet_dropped(std::vector<ReliableEvent>&& events)
{
    // Re-prepend in original order so they retain their sequence numbers.
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        send_queue_.push_front(std::move(*it));
    }
}

} // namespace studio::content::net
