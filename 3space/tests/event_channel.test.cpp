// Track 20 spec 05 — typed event dispatch tests.

#include <catch2/catch.hpp>

#include "content/net/event_channel.hpp"
#include "content/net/reliable_channel.hpp"

#include <array>
#include <memory>
#include <vector>

using namespace studio::content::net;

namespace {

struct FireEvent : INetEvent
{
    static constexpr std::uint8_t kClassTag = 42;
    std::uint16_t weapon_id = 0;
    float aim_angle = 0.0f;

    std::uint8_t class_tag() const override { return kClassTag; }
    void pack(BitStream& s) const override {
        s.write_int(weapon_id, 16);
        s.write_signed_float(aim_angle, 16);
    }
    void unpack(BitStream& s) override {
        weapon_id = static_cast<std::uint16_t>(s.read_int(16));
        aim_angle = s.read_signed_float(16);
    }
};

} // anonymous namespace

TEST_CASE("EventChannel: ChatEvent dispatches to handler", "[net][event]")
{
    ReliableChannel reliable;
    EventChannel sender(reliable);

    ReliableChannel rx_reliable;
    EventChannel receiver(rx_reliable);

    std::vector<std::string> received_chat;
    receiver.register_event(ChatEvent::kClassTag,
        []{ return std::make_unique<ChatEvent>(); },
        [&](INetEvent& ev) {
            auto& c = static_cast<ChatEvent&>(ev);
            received_chat.push_back(c.sender + ": " + c.text);
        });

    ChatEvent c;
    c.sender = "Player1";
    c.text = "Hello tribes!";
    REQUIRE(sender.broadcast(c));

    // Pump send -> wire -> receive.
    std::array<std::uint8_t, 256> buf{};
    BitStream w(buf.data(), buf.size());
    REQUIRE(reliable.pack(w, w.capacity_bits()) == 1);
    BitStream r(buf.data(), w.byte_position());
    rx_reliable.unpack(r);
    receiver.tick();

    REQUIRE(received_chat.size() == 1);
    REQUIRE(received_chat[0] == "Player1: Hello tribes!");
}

TEST_CASE("EventChannel: multiple typed events dispatch correctly",
          "[net][event]")
{
    ReliableChannel tx;
    ReliableChannel rx;
    EventChannel sender(tx);
    EventChannel receiver(rx);

    int chats_seen = 0;
    int fires_seen = 0;
    std::uint16_t last_weapon = 0;
    receiver.register_event(ChatEvent::kClassTag,
        []{ return std::make_unique<ChatEvent>(); },
        [&](INetEvent&) { ++chats_seen; });
    receiver.register_event(FireEvent::kClassTag,
        []{ return std::make_unique<FireEvent>(); },
        [&](INetEvent& ev) {
            ++fires_seen;
            last_weapon = static_cast<FireEvent&>(ev).weapon_id;
        });

    ChatEvent c; c.sender = "A"; c.text = "hi";
    FireEvent f; f.weapon_id = 0x1234; f.aim_angle = 0.5f;
    sender.broadcast(c);
    sender.broadcast(f);
    sender.broadcast(c);

    std::array<std::uint8_t, 512> buf{};
    BitStream w(buf.data(), buf.size());
    REQUIRE(tx.pack(w, w.capacity_bits()) == 3);
    BitStream r(buf.data(), w.byte_position());
    rx.unpack(r);
    receiver.tick();

    REQUIRE(chats_seen == 2);
    REQUIRE(fires_seen == 1);
    REQUIRE(last_weapon == 0x1234);
}

TEST_CASE("EventChannel: unhandled tag is logged but doesn't crash",
          "[net][event]")
{
    ReliableChannel tx;
    ReliableChannel rx;
    EventChannel sender(tx);
    EventChannel receiver(rx);

    // No handler registered.
    ChatEvent c; c.sender="X"; c.text="ignored";
    REQUIRE(sender.broadcast(c));

    std::array<std::uint8_t, 256> buf{};
    BitStream w(buf.data(), buf.size());
    REQUIRE(tx.pack(w, w.capacity_bits()) == 1);
    BitStream r(buf.data(), w.byte_position());
    rx.unpack(r);
    REQUIRE_NOTHROW(receiver.tick());
}
