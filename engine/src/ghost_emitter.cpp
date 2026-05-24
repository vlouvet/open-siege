#include <osengine/ghost_emitter.hpp>

#include <osengine/ghost_encoder.hpp>
#include <osengine/ghost_types.hpp>
#include <osengine/reliable_acks.hpp>
#include <osengine/server_world_snapshot.hpp>
#include <osengine/session_table.hpp>

#include <cmath>
#include <cstdio>

namespace dts_viewer
{

namespace
{

// Build a transient GhostPlayer from a session's authoritative state.
// initial_update is set per-record by write_one_record based on whether
// the encoder writes an intro or a delta.
net20::GhostPlayer player_from_session(const Session& peer)
{
    net20::GhostPlayer p;
    p.ghost_id  = peer.player_slot;
    p.object_id = 0x10000000u | peer.player_slot;
    p.class_tag = kServerPlayerClassTag;
    p.base.base_changed = true;
    p.base.team_id      = static_cast<std::uint8_t>(peer.team);
    p.has_pos_block = true;
    p.pos_x = peer.player_state.pos.x;
    p.pos_y = peer.player_state.pos.y;
    p.pos_z = peer.player_state.pos.z;
    p.view_pitch  = peer.player_state.pitch;
    p.yaw         = peer.player_state.yaw;
    p.on_ground   = peer.player_state.on_ground;
    p.has_velocity = false;
    p.datafile_id = 0;
    p.recharge_changed  = false;
    p.pda_mode          = false;
    p.crouch_state      = false;
    p.inventory_changed = false;
    return p;
}

}  // namespace

GhostEmitter::GhostEmitter(Session* session, Sink sink)
    : session_(session), sink_(std::move(sink))
{
}

void GhostEmitter::emit(const ServerWorldSnapshot& world)
{
    if (!session_) return;

    constexpr float kPosEps = 0.01f;
    constexpr float kYawEps = 0.005f;

    // 1. Build the list of records we want to emit.
    //    - Owning client never gets a ghost of itself.
    //    - For each other peer: dirty test (pos/yaw moved or new).
    std::vector<net20::GhostPlayer> player_buffer;
    std::vector<net20::TypedRecordOut> records;
    player_buffer.reserve(world.players.size());
    records.reserve(world.players.size());

    const bool burst = first_emit_pending_;

    for (const Session* peer : world.players) {
        if (!peer || peer == session_) continue;
        const std::uint16_t gid = peer->player_slot;
        auto& v = view_[gid];
        const auto& ps = peer->player_state;
        const bool needs_intro = !v.introduced || burst;
        const bool dirty = needs_intro
            || std::fabs(v.last_pos_x - ps.pos.x) > kPosEps
            || std::fabs(v.last_pos_y - ps.pos.y) > kPosEps
            || std::fabs(v.last_pos_z - ps.pos.z) > kPosEps
            || std::fabs(v.last_yaw   - ps.yaw)   > kYawEps;
        if (!dirty) continue;

        player_buffer.push_back(player_from_session(*peer));
        net20::TypedRecordOut rec;
        rec.kind        = net20::GhostClassKind::Player;
        rec.ghost_id    = gid;
        rec.full_update = needs_intro;
        rec.object_id   = player_buffer.back().object_id;
        rec.class_tag   = player_buffer.back().class_tag;
        // .player set after we finish push_back so the pointer is stable.
        records.push_back(rec);

        v.last_pos_x = ps.pos.x;
        v.last_pos_y = ps.pos.y;
        v.last_pos_z = ps.pos.z;
        v.last_yaw   = ps.yaw;
        v.introduced = true;
    }

    // 2. Kill records for peers no longer in the snapshot.
    std::vector<std::uint16_t> killed;
    for (auto& kv : view_) {
        bool alive = false;
        for (const Session* peer : world.players) {
            if (peer && peer != session_ && peer->player_slot == kv.first) {
                alive = true; break;
            }
        }
        if (!alive) {
            net20::TypedRecordOut rec;
            rec.kind     = net20::GhostClassKind::Player;
            rec.ghost_id = kv.first;
            rec.kill     = true;
            records.push_back(rec);
            killed.push_back(kv.first);
        }
    }
    for (auto id : killed) view_.erase(id);

    if (records.empty()) return;

    // Wire .player pointers AFTER the vector is fully populated (so
    // its storage doesn't move underneath us).
    {
        std::size_t pi = 0;
        for (auto& rec : records) {
            if (rec.kill || rec.kind != net20::GhostClassKind::Player) continue;
            rec.player = &player_buffer[pi++];
        }
    }

    // 3. Encode + send.
    net20::VcHeaderInputs hdr;
    hdr.send_seq             = session_->next_send_seq;
    hdr.connect_parity       = false;
    hdr.highest_acked_of_mine = 0;
    hdr.type_word            = net20::pkt_type::kDataPacket;
    // Ack runs stay empty so the VC header is always the same fixed
    // width — keeps the ghost-sub-stream start bit at the public
    // constant kGhostStreamStartBit.

    // Pick the smallest idW (3..10) that fits the largest ghost_id we're
    // about to write. Smaller idW reduces the scanner's risk of finding
    // a false-positive offset in the packet's framing bits (the encoder's
    // sub-stream-presence bits "0,0,1" alias as a high-confidence
    // scope-always candidate at the wrong offset for idW=10).
    std::uint16_t max_gid = 0;
    for (const auto& r : records) if (r.ghost_id > max_gid) max_gid = r.ghost_id;
    std::uint8_t id_width = 3;
    while (id_width < 10 && (1u << id_width) <= max_gid) ++id_width;

    const auto bytes = burst
        ? net20::encode_scope_always_burst(hdr, records, id_width)
        : net20::encode_normal_delta(hdr, records, id_width);

    if (sink_ && sink_(session_->peer, bytes.data(), bytes.size())) {
        stats_.packets_emitted += 1;
        stats_.bytes_emitted   += bytes.size();
        stats_.records_emitted += records.size();
        for (const auto& r : records) if (r.kill) ++stats_.kills_emitted;
        if (burst) {
            ++stats_.bursts_emitted;
            first_emit_pending_ = false;
        }
        session_->next_send_seq = static_cast<std::uint16_t>(
            (session_->next_send_seq + 1) & 0x1FFu);
        if (session_->next_send_seq == 0) session_->next_send_seq = 1;
    }
}

void GhostEmitter::on_client_ack(std::uint16_t /*recv_seq*/)
{
    // v1 stub — per-field ack tracking is a 28/04c follow-on.
}

int ghost_emitter_selftest()
{
    // 3 sessions at distinct positions; tick 5 times. After each tick,
    // bump positions so every emitter sees its peers as dirty.
    SessionTable table(8);
    const std::uint8_t n1[3] = { 0x11, 0x22, 0x33 };
    const std::uint8_t n2[3] = { 0x44, 0x55, 0x66 };
    const std::uint8_t n3[3] = { 0x77, 0x88, 0x99 };
    studio::content::net::Endpoint p1{"127.0.0.1", 51001};
    studio::content::net::Endpoint p2{"127.0.0.1", 51002};
    studio::content::net::Endpoint p3{"127.0.0.1", 51003};
    auto* s1 = table.allocate(p1, n1, 0);
    auto* s2 = table.allocate(p2, n2, 0);
    auto* s3 = table.allocate(p3, n3, 0);
    if (!s1 || !s2 || !s3) {
        std::fputs("[ghost-emit-selftest] allocate failed\n", stderr);
        return 1;
    }
    s1->team = Team::Red;
    s2->team = Team::Blue;
    s3->team = Team::Red;
    s1->player_state.pos = {10.0f, 5.0f, 0.0f};
    s2->player_state.pos = {20.0f, 5.0f, 0.0f};
    s3->player_state.pos = {30.0f, 5.0f, 0.0f};

    struct Captured {
        studio::content::net::Endpoint peer;
        std::vector<std::uint8_t>      bytes;
    };
    std::vector<Captured> sent;
    auto sink = [&](const studio::content::net::Endpoint& peer,
                    const std::uint8_t* data, std::size_t size) -> bool {
        sent.push_back({peer, std::vector<std::uint8_t>(data, data + size)});
        return true;
    };

    GhostEmitter e1(s1, sink);
    GhostEmitter e2(s2, sink);
    GhostEmitter e3(s3, sink);

    for (int t = 0; t < 5; ++t) {
        ServerWorldSnapshot world;
        world.tick = static_cast<std::uint64_t>(t);
        world.server_time_ms = 1000ull + 100ull * t;
        world.players = { s1, s2, s3 };
        e1.emit(world);
        e2.emit(world);
        e3.emit(world);
        s1->player_state.pos.z += 1.0f;
        s2->player_state.pos.z += 2.0f;
        s3->player_state.pos.z += 3.0f;
    }

    auto count_for = [&](const studio::content::net::Endpoint& peer) {
        std::size_t n = 0;
        for (const auto& c : sent) if (c.peer.port == peer.port) ++n;
        return n;
    };
    const std::size_t n1pkt = count_for(p1);
    const std::size_t n2pkt = count_for(p2);
    const std::size_t n3pkt = count_for(p3);
    std::fprintf(stderr,
        "[ghost-emit-selftest] emitted: s1=%zu s2=%zu s3=%zu (expect 5 each)\n",
        n1pkt, n2pkt, n3pkt);
    if (n1pkt != 5 || n2pkt != 5 || n3pkt != 5) {
        std::fputs("[ghost-emit-selftest] FAIL — packet count mismatch\n", stderr);
        return 1;
    }

    // Decode every captured packet with parse_typed_packet_at_offset
    // using the fixed start-bit. Each session needs its own decoded
    // registry because intros are per-receiver. Assert that after
    // walking ALL packets sent to a peer, the registry contains the
    // 2 other-player ghosts.
    auto decode_for = [&](const studio::content::net::Endpoint& peer,
                          std::uint16_t self_slot,
                          std::uint16_t other_a, std::uint16_t other_b,
                          float expect_a_z, float expect_b_z) -> bool {
        net20::GhostRegistry reg;
        reg.install_default_class_tag_map();
        for (const auto& c : sent) {
            if (c.peer.port != peer.port) continue;
            auto dec = net20::parse_typed_packet_at_offset(
                c.bytes.data(), c.bytes.size(),
                GhostEmitter::kGhostStreamStartBit, reg);
            if (!dec.walked_full_stream) {
                std::fprintf(stderr,
                    "[ghost-emit-selftest] FAIL — decoder stopped: %s\n",
                    dec.note.c_str());
                return false;
            }
        }
        if (reg.players.count(other_a) == 0 || reg.players.count(other_b) == 0) {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — peer %u registry missing "
                "ghost(s): want %u and %u, have %zu players\n",
                peer.port, other_a, other_b, reg.players.size());
            return false;
        }
        if (reg.players.count(self_slot) != 0) {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — peer %u received its own "
                "ghost (slot %u)\n", peer.port, self_slot);
            return false;
        }
        // Final positions should match expectations after 5 ticks.
        const auto& pa = reg.players.at(other_a);
        const auto& pb = reg.players.at(other_b);
        if (std::fabs(pa.pos_z - expect_a_z) > 0.05f
            || std::fabs(pb.pos_z - expect_b_z) > 0.05f)
        {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — peer %u final pos.z: "
                "gid %u got %.2f (want %.2f); gid %u got %.2f (want %.2f)\n",
                peer.port,
                other_a, pa.pos_z, expect_a_z,
                other_b, pb.pos_z, expect_b_z);
            return false;
        }
        return true;
    };

    // After cycle 4 emit, positions captured for OTHER peers are those
    // BEFORE that cycle's bump: s1.z=4, s2.z=8, s3.z=12.
    if (!decode_for(p1, s1->player_slot, s2->player_slot, s3->player_slot,
                    8.0f, 12.0f)) return 1;
    if (!decode_for(p2, s2->player_slot, s1->player_slot, s3->player_slot,
                    4.0f, 12.0f)) return 1;
    if (!decode_for(p3, s3->player_slot, s1->player_slot, s2->player_slot,
                    4.0f, 8.0f))  return 1;

    // Kill propagation: drop s3 from the world; e1 and e2 should send
    // a kill record on the next emit.
    sent.clear();
    {
        ServerWorldSnapshot world;
        world.tick = 100;
        world.server_time_ms = 9999;
        world.players = { s1, s2 };
        e1.emit(world);
        e2.emit(world);
    }
    bool e1_killed = false, e2_killed = false;
    for (const auto& c : sent) {
        net20::GhostRegistry reg;
        reg.install_default_class_tag_map();
        // Pre-seed every slot's kind so the decoder treats deltas as
        // deltas (not full intros). This mirrors a live receiver that
        // already absorbed earlier intro packets.
        for (auto* peer : {s1, s2, s3}) {
            reg.ghost_kinds[peer->player_slot] = net20::GhostClassKind::Player;
            reg.ghost_class_tags[peer->player_slot] = kServerPlayerClassTag;
        }
        auto dec = net20::parse_typed_packet_at_offset(
            c.bytes.data(), c.bytes.size(),
            GhostEmitter::kGhostStreamStartBit, reg);
        for (const auto& r : dec.records) {
            if (r.kill && r.ghost_id == s3->player_slot) {
                if (c.peer.port == p1.port) e1_killed = true;
                if (c.peer.port == p2.port) e2_killed = true;
            }
        }
    }
    if (!e1_killed || !e2_killed) {
        std::fprintf(stderr,
            "[ghost-emit-selftest] FAIL — kill propagation: e1=%d e2=%d\n",
            e1_killed ? 1 : 0, e2_killed ? 1 : 0);
        return 1;
    }

    std::fputs("[ghost-emit-selftest] OK — T1 wire format, 3 sessions, deltas, kill\n",
               stderr);
    return 0;
}

}  // namespace dts_viewer
