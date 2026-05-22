#ifndef LIB3SPACE_NET_RELIABLE_CHANNEL_HPP
#define LIB3SPACE_NET_RELIABLE_CHANNEL_HPP

// Track 20 spec 03 — reliable event channel layered on the VC.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §§3 and 7.
//
// The reliable channel guarantees delivery and (optionally) in-order receipt
// of bounded-size events. It rides on top of the VC's 9-bit datagram sequence
// numbers (lower layer; not this class) by using a 7-bit event sequence
// number and a 128-bit ack bitmap.
//
// Wire encoding (§7.3) for each event packed into a datagram:
//
//   1   event-present flag (1 = another event follows)
//   1   guaranteed flag
//   1   seq-continuous flag       (only if guaranteed)
//   1   has-explicit-seq          (only if guaranteed && !continuous)
//   7   explicit seq mod 128      (only if has-explicit-seq)
//   7   event class tag - 1024
//   *   per-event payload
//
// The channel takes opaque byte payloads + a class tag in [0, 127]; the +1024
// offset disambiguates events from ghost-replicated objects on the wire but
// is invisible to callers.

#include "content/net/bit_stream.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace studio::content::net
{

struct ReliableEvent
{
    std::uint8_t class_tag = 0;       // 0..127 (the wire is `class_tag` itself)
    bool         guaranteed = true;   // false -> best-effort
    bool         ordered    = true;   // true -> sequence assigned; receiver delivers in order
    std::uint8_t seq        = 0;      // assigned by channel on send; ignored at send-call
    std::vector<std::uint8_t> payload;
};

class ReliableChannel
{
public:
    static constexpr std::size_t kAckMaskBits = 128;
    static constexpr std::size_t kSeqWrap     = 128;   // 7-bit sequences

    ReliableChannel() = default;

    // Enqueue an event for transmission. Returns the assigned 7-bit
    // sequence (only meaningful for guaranteed+ordered events; 0 otherwise).
    // For ordered events, send_window_full() must be false; otherwise the
    // event is dropped and the function returns nullopt.
    std::optional<std::uint8_t> send(ReliableEvent ev);

    // Pack as many pending events as fit into `s` starting at the current
    // cursor. Always terminates with the event-present=0 flag. Returns the
    // number of events packed.
    std::size_t pack(BitStream& s, std::size_t max_payload_bits);

    // Unpack events from `s` (cursor must be positioned past the VC header
    // and any rate-control region). Walks until event-present=0. Each
    // ordered event is buffered until its predecessor arrives; calls
    // poll_delivered() to drain in-order results.
    void unpack(BitStream& s);

    // Pop the next in-order received event, if available.
    std::optional<ReliableEvent> poll();

    // Notification: the lower (VC) layer telling us a datagram landed/dropped.
    // `seqs` is the list of event sequence numbers carried by that datagram.
    void on_packet_acked(const std::vector<std::uint8_t>& seqs);
    void on_packet_dropped(std::vector<ReliableEvent>&& events);

    // Diagnostics
    std::size_t pending_send_count() const { return send_queue_.size(); }
    std::size_t in_flight_count() const;
    bool        send_window_full() const;

private:
    // 128-bit ack mask: bit `seq` is set when the receiver has acked the
    // matching event. Indexed via `(seq >> 5) & 3` for word, `seq & 31` for bit.
    std::array<std::uint32_t, 4> acked_mask_ {};

    // Send-side state
    std::deque<ReliableEvent> send_queue_;          // events waiting for first send
    std::deque<ReliableEvent> in_flight_;           // events sent but not acked
    std::uint8_t  next_send_seq_  = 0;              // ordered seq counter (mod 128)
    std::uint8_t  last_acked_seq_ = 0;              // high-water mark; ordered events only

    // Receive-side state
    std::uint8_t  next_expected_seq_ = 0;
    std::array<std::optional<ReliableEvent>, kSeqWrap> recv_holding_ {};
    std::deque<ReliableEvent> delivered_;           // poll() drains from here

    // Last-packed seq within the current pack() call (for seq-continuous bit).
    std::uint8_t last_packed_seq_ = 0;
    bool         last_packed_valid_ = false;

    static bool seq_less_eq(std::uint8_t a, std::uint8_t b);
    static std::uint8_t seq_diff(std::uint8_t a, std::uint8_t b);

    void set_acked_bit(std::uint8_t seq);
    bool acked_bit(std::uint8_t seq) const;
};

} // namespace studio::content::net

#endif // LIB3SPACE_NET_RELIABLE_CHANNEL_HPP
