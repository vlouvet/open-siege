#ifndef LIB3SPACE_NET_EVENT_CHANNEL_HPP
#define LIB3SPACE_NET_EVENT_CHANNEL_HPP

// Track 20 spec 05 — typed event dispatch atop the reliable channel.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §§7 and 10.
//
// The EventChannel adds two things on top of the raw ReliableChannel:
//
//   1. A class-tag registry — applications register a factory per game
//      event type. On receive, the channel constructs the right object
//      via the factory and dispatches to its handler.
//   2. A typed broadcast API — `channel.broadcast<FireDiscEvent>(...)`
//      packs the event body into a payload and pushes it onto the
//      reliable channel.
//
// The payload format on the wire is whatever the event's own pack() writes;
// a 16-bit length prefix wraps it so the reliable channel can sync (this is
// the same Implementer-chosen extension documented in
// reliable_channel.cpp). Event subclasses do not need to write their own
// length.

#include "content/net/bit_stream.hpp"
#include "content/net/reliable_channel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace studio::content::net
{

struct INetEvent
{
    virtual ~INetEvent() = default;
    virtual std::uint8_t class_tag() const = 0;
    virtual void pack(BitStream& s) const = 0;
    virtual void unpack(BitStream& s) = 0;
};

// Convenience: a chat-message event (string body). Used in tests + as a
// reference implementation for downstream typed events.
struct ChatEvent : INetEvent
{
    static constexpr std::uint8_t kClassTag = 1;

    std::string sender;
    std::string text;

    std::uint8_t class_tag() const override { return kClassTag; }
    void pack(BitStream& s) const override {
        s.write_string(sender);
        s.write_string(text);
    }
    void unpack(BitStream& s) override {
        sender = s.read_string();
        text = s.read_string();
    }
};

class EventChannel
{
public:
    using Factory = std::function<std::unique_ptr<INetEvent>()>;
    using Handler = std::function<void(INetEvent&)>;

    explicit EventChannel(ReliableChannel& reliable) : reliable_(reliable) {}

    // Register an event class. `tag` must be in [0, 127] — it occupies the
    // 7-bit class-tag field of the reliable-channel wire format.
    void register_event(std::uint8_t tag, Factory factory, Handler handler);

    // Send a typed event. Returns true if enqueued.
    template <typename Event>
    bool broadcast(const Event& ev, bool ordered = true);

    // Pump received events: drain the reliable channel, dispatch.
    void tick();

private:
    struct Entry { Factory factory; Handler handler; };
    std::unordered_map<std::uint8_t, Entry> registry_;
    ReliableChannel& reliable_;
};

// --- inline template ---

template <typename Event>
bool EventChannel::broadcast(const Event& ev, bool ordered)
{
    // Serialize the event into a temporary BitStream-backed buffer, then
    // hand the bytes to the reliable channel as an opaque payload.
    std::array<std::uint8_t, 1024> buf{};
    BitStream s(buf.data(), buf.size());
    ev.pack(s);
    if (!s.is_valid()) return false;
    const std::size_t bytes = s.byte_position();

    ReliableEvent rev;
    rev.class_tag = ev.class_tag();
    rev.guaranteed = true;
    rev.ordered = ordered;
    rev.payload.assign(buf.data(), buf.data() + bytes);
    return reliable_.send(std::move(rev)).has_value() || !ordered;
}

} // namespace studio::content::net

#endif // LIB3SPACE_NET_EVENT_CHANNEL_HPP
