// Track 20 spec 04 — ghost-state replication framework tests.
//
// Covers: scope-always burst with new-ghost introduction, normal-mode
// delta updates, kill propagation, and drop/re-dirty behaviour.

#include <catch2/catch.hpp>

#include "content/net/ghost_manager.hpp"

#include <array>
#include <memory>
#include <vector>

using namespace studio::content::net;

namespace {

// Minimal test ghost: a 32-bit position field gated by dirty bit 0 and a
// 16-bit health field gated by dirty bit 1.
struct TestGhost final : IGhostObject
{
    static constexpr std::uint16_t kClassTag = 7;

    std::uint32_t id = 0;
    std::uint32_t position = 0;
    std::uint16_t health = 0;

    std::uint16_t class_tag() const override { return kClassTag; }
    std::uint32_t object_id() const override { return id; }

    void pack_update(BitStream& s, std::uint32_t dirty_mask, bool is_initial) override
    {
        const bool send_pos = is_initial || (dirty_mask & 0x1u);
        s.write_flag(send_pos);
        if (send_pos) s.write_int(position, 32);
        const bool send_hp  = is_initial || (dirty_mask & 0x2u);
        s.write_flag(send_hp);
        if (send_hp) s.write_int(health, 16);
    }
    void unpack_update(BitStream& s, bool /*is_initial*/) override
    {
        if (s.read_flag()) position = s.read_int(32);
        if (s.read_flag()) health = static_cast<std::uint16_t>(s.read_int(16));
    }
};

GhostManager::Factory make_factory()
{
    return [](std::uint16_t tag) -> std::unique_ptr<IGhostObject> {
        if (tag == TestGhost::kClassTag) return std::make_unique<TestGhost>();
        return nullptr;
    };
}

} // anonymous namespace

TEST_CASE("GhostManager: scope-always introduction packs + unpacks new ghost",
          "[net][ghost]")
{
    GhostManager server;
    GhostManager client;
    client.set_factory(make_factory());

    TestGhost obj;
    obj.id = 0xDEADBEEFu;
    obj.position = 0x12345678u;
    obj.health = 100;
    const auto idx = server.add_object(&obj, /*scope_always=*/true);
    REQUIRE(idx != kInvalidGhostIdx);

    std::array<std::uint8_t, 128> buf{};
    BitStream w(buf.data(), buf.size());
    const auto packed = server.pack_replication(w,
        w.capacity_bits(), /*mode_scope_always=*/true);
    REQUIRE(packed == 1);
    REQUIRE(w.is_valid());

    BitStream r(buf.data(), buf.size());
    REQUIRE(client.unpack_replication(r));
    REQUIRE(client.live_mirror_count() == 1);

    auto* recv = static_cast<TestGhost*>(client.mirror_for(idx));
    REQUIRE(recv != nullptr);
    REQUIRE(recv->position == 0x12345678u);
    REQUIRE(recv->health == 100);
}

TEST_CASE("GhostManager: normal-mode delta only sends dirty bits",
          "[net][ghost]")
{
    GhostManager server;
    GhostManager client;
    client.set_factory(make_factory());

    TestGhost obj;
    obj.position = 1; obj.health = 50;
    const auto idx = server.add_object(&obj, /*scope_always=*/true);

    // Initial scope-always burst.
    {
        std::array<std::uint8_t, 128> buf{};
        BitStream w(buf.data(), buf.size());
        server.pack_replication(w, w.capacity_bits(), true);
        server.on_packet_acked({ idx });

        BitStream r(buf.data(), buf.size());
        REQUIRE(client.unpack_replication(r));
    }

    // Game tick: change only health. Expect a small normal-mode packet
    // that doesn't include the position word.
    obj.health = 75;
    server.set_dirty_bits(idx, 0x2u);   // bit 1: health

    std::array<std::uint8_t, 128> buf{};
    BitStream w(buf.data(), buf.size());
    server.pack_replication(w, w.capacity_bits(), false);
    REQUIRE(w.is_valid());
    const std::size_t bits_used = w.bit_position();

    // The delta packet should be considerably smaller than a fresh
    // introduction — at least less than `4 (header) + width + 1 + 1 + 32
    // (pos flag + value) + 1 + 16 (hp flag + value) + 1 (terminator)`
    // worst case = ~60 bits. Skipping the position word saves ~33 bits.
    INFO("delta packet bits=" << bits_used);
    REQUIRE(bits_used < 60);

    BitStream r(buf.data(), buf.size());
    REQUIRE(client.unpack_replication(r));
    auto* recv = static_cast<TestGhost*>(client.mirror_for(idx));
    REQUIRE(recv->health == 75);
    REQUIRE(recv->position == 1);   // unchanged
}

TEST_CASE("GhostManager: kill propagates and releases the slot",
          "[net][ghost]")
{
    GhostManager server;
    GhostManager client;
    client.set_factory(make_factory());

    TestGhost obj;
    const auto idx = server.add_object(&obj, true);
    // Initial introduction.
    {
        std::array<std::uint8_t, 128> buf{};
        BitStream w(buf.data(), buf.size());
        server.pack_replication(w, w.capacity_bits(), true);
        BitStream r(buf.data(), buf.size());
        REQUIRE(client.unpack_replication(r));
    }
    REQUIRE(client.live_mirror_count() == 1);

    server.remove_object(idx);
    std::array<std::uint8_t, 64> buf{};
    BitStream w(buf.data(), buf.size());
    server.pack_replication(w, w.capacity_bits(), false);
    server.on_packet_acked({ idx });

    BitStream r(buf.data(), buf.size());
    REQUIRE(client.unpack_replication(r));
    REQUIRE(client.live_mirror_count() == 0);
}

TEST_CASE("GhostManager: dropped packet re-dirties carried bits",
          "[net][ghost]")
{
    GhostManager server;
    TestGhost obj;
    obj.position = 42;
    const auto idx = server.add_object(&obj, false);
    server.set_dirty_bits(idx, 0x1u);

    {
        std::array<std::uint8_t, 64> buf{};
        BitStream w(buf.data(), buf.size());
        // We don't care about the actual bytes; we want to confirm the
        // re-dirty pathway.
        server.pack_replication(w, w.capacity_bits(), false);
    }
    // Now pretend the VC layer reports a drop for that mirror with the
    // bits we know we sent (bit 0).
    server.on_packet_dropped({ idx }, { 0x1u });

    // Pack again — should still produce an update since position is dirty.
    std::array<std::uint8_t, 64> buf{};
    BitStream w(buf.data(), buf.size());
    const auto packed = server.pack_replication(w, w.capacity_bits(), false);
    REQUIRE(packed == 1);
}
