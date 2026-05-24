#include <osengine/ghost_emitter.hpp>

#include <osengine/server_world_snapshot.hpp>
#include <osengine/session_table.hpp>
#include <osengine/reliable_acks.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dts_viewer
{

namespace
{

void put_u16_le(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void put_u64_le(std::uint8_t* p, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

void put_f32_le(std::uint8_t* p, float f)
{
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

std::uint16_t get_u16_le(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (std::uint32_t(p[1]) << 8));
}

std::uint64_t get_u64_le(const std::uint8_t* p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

float get_f32_le(const std::uint8_t* p)
{
    std::uint32_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Build a 4-byte VC header carrying our send-seq + a custom 5-bit
// type-word. We don't carry any ack runs (the OSGB stream piggybacks
// no acks; pure-acks ride the existing path).
std::vector<std::uint8_t> build_vc_header_osgb(std::uint16_t send_seq,
                                               bool parity)
{
    net20::VcHeaderInputs hdr{};
    hdr.send_seq = send_seq & 0x1FFu;
    hdr.connect_parity = parity;
    hdr.highest_acked_of_mine = 0;
    hdr.type_word = kOpenSiegeGhostBurstType;
    return net20::encode_vc_header(hdr);
}

} // namespace

std::vector<std::uint8_t> encode_osgb_packet(const OSGBPacket& pkt)
{
    auto buf = build_vc_header_osgb(pkt.vc_send_seq, pkt.vc_parity != 0);
    const std::size_t header_offset = buf.size();
    buf.resize(header_offset + kOSGBHeaderSize
               + kOSGBRecordSize * pkt.records.size());
    std::uint8_t* p = buf.data() + header_offset;

    p[0] = 'O'; p[1] = 'S'; p[2] = 'G'; p[3] = 'B';
    p[4] = 0x01;
    p[5] = static_cast<std::uint8_t>(pkt.records.size());
    put_u64_le(p + 6, pkt.server_time_ms);
    // 2 bytes pad to round header to 18B (and keep records 8-byte aligned
    // relative to the OSGB start). We zero them so future versions can
    // claim those slots.
    p[14] = 0; p[15] = 0; p[16] = 0; p[17] = 0;

    p += kOSGBHeaderSize;
    for (const auto& r : pkt.records) {
        put_u16_le(p + 0, r.ghost_id);
        p[2] = r.flags;
        p[3] = r.team_id;
        put_f32_le(p + 4,  r.pos_x);
        put_f32_le(p + 8,  r.pos_y);
        put_f32_le(p + 12, r.pos_z);
        put_f32_le(p + 16, r.yaw);
        p[20] = r.damage;
        p[21] = r.anim_index;
        p[22] = 0;
        p[23] = 0;
        p += kOSGBRecordSize;
    }
    return buf;
}

bool decode_osgb_packet(const std::uint8_t* data, std::size_t size,
                        OSGBPacket& out)
{
    // VC header is variable-length (3-8 bytes depending on ack runs).
    // Scan for the OSGB magic within the first 16 bytes — won't collide
    // with VC-header content (header is type-word-tagged separately).
    std::size_t vc_bytes = 0;
    bool found = false;
    for (std::size_t i = 0; i + 4 + kOSGBHeaderSize <= size && i < 16; ++i) {
        if (data[i] == 'O' && data[i+1] == 'S'
            && data[i+2] == 'G' && data[i+3] == 'B') {
            vc_bytes = i;
            found = true;
            break;
        }
    }
    if (!found) return false;
    const std::uint8_t* p = data + vc_bytes;
    if (p[4] != 0x01) return false;
    const std::size_t n = p[5];
    if (size < vc_bytes + kOSGBHeaderSize + kOSGBRecordSize * n) {
        return false;
    }
    out.vc_send_seq = 0;   // selftest doesn't introspect the VC header
    out.vc_parity   = 0;
    out.server_time_ms = get_u64_le(p + 6);
    out.records.clear();
    out.records.resize(n);
    p += kOSGBHeaderSize;
    for (std::size_t i = 0; i < n; ++i) {
        auto& r = out.records[i];
        r.ghost_id   = get_u16_le(p + 0);
        r.flags      = p[2];
        r.team_id    = p[3];
        r.pos_x      = get_f32_le(p + 4);
        r.pos_y      = get_f32_le(p + 8);
        r.pos_z      = get_f32_le(p + 12);
        r.yaw        = get_f32_le(p + 16);
        r.damage     = p[20];
        r.anim_index = p[21];
        p += kOSGBRecordSize;
    }
    return true;
}

GhostEmitter::GhostEmitter(Session* session, Sink sink)
    : session_(session), sink_(std::move(sink))
{
}

void GhostEmitter::emit(const ServerWorldSnapshot& world)
{
    if (!session_) return;

    // v1 dirty rule: a ghost is dirty if (a) we've never sent it to
    // this client, or (b) any of pos.x/y/z/yaw changed by > ε since
    // last cached. Other fields (damage, anim) ride along whenever
    // any ghost in the packet is dirty, so a single delta packet
    // carries the full current state of the dirty set.
    constexpr float kPosEps = 0.01f;
    constexpr float kYawEps = 0.005f;

    OSGBPacket pkt;
    pkt.vc_send_seq    = session_->next_send_seq;
    pkt.vc_parity      = 0;
    pkt.server_time_ms = world.server_time_ms;

    for (const Session* peer : world.players) {
        if (!peer) continue;
        // The owning client gets no ghost of itself — it simulates
        // itself locally. The "ghost_id" we publish for each peer is
        // its player_slot; that's stable per-session lifetime.
        if (peer == session_) continue;

        const std::uint16_t gid = peer->player_slot;
        auto& view = view_[gid];
        const auto& ps = peer->player_state;
        const bool dirty = !view.ever_sent
            || std::fabs(view.last_pos_x - ps.pos.x) > kPosEps
            || std::fabs(view.last_pos_y - ps.pos.y) > kPosEps
            || std::fabs(view.last_pos_z - ps.pos.z) > kPosEps
            || std::fabs(view.last_yaw   - ps.yaw)   > kYawEps;
        if (!dirty) continue;

        OSGBRecord r;
        r.ghost_id   = gid;
        r.flags      = ps.on_ground ? 0x02u : 0x00u;
        r.team_id    = 0;            // spec 28/05 fills this in
        r.pos_x      = ps.pos.x;
        r.pos_y      = ps.pos.y;
        r.pos_z      = ps.pos.z;
        r.yaw        = ps.yaw;
        r.damage     = 0;            // spec 28/07 fills this in
        r.anim_index = 0;
        pkt.records.push_back(r);

        view.last_pos_x   = ps.pos.x;
        view.last_pos_y   = ps.pos.y;
        view.last_pos_z   = ps.pos.z;
        view.last_yaw     = ps.yaw;
        view.last_sent_seq = pkt.vc_send_seq;
        view.ever_sent    = true;
    }

    // Drop view entries for peers no longer in the world (disconnects).
    // v1 inference: client sees the absence and removes; no explicit
    // kill bit yet. Emit a kill-flag record for completeness so we don't
    // depend on the absent-from-snapshot signal alone.
    std::vector<std::uint16_t> killed;
    for (auto& kv : view_) {
        bool still_alive = false;
        for (const Session* peer : world.players) {
            if (peer && peer != session_ && peer->player_slot == kv.first) {
                still_alive = true;
                break;
            }
        }
        if (!still_alive) {
            OSGBRecord r;
            r.ghost_id = kv.first;
            r.flags    = 0x01u;      // kill bit
            pkt.records.push_back(r);
            killed.push_back(kv.first);
        }
    }
    for (auto id : killed) view_.erase(id);

    if (pkt.records.empty()) return;

    const auto bytes = encode_osgb_packet(pkt);
    if (sink_ && sink_(session_->peer, bytes.data(), bytes.size())) {
        stats_.packets_emitted += 1;
        stats_.bytes_emitted   += bytes.size();
        stats_.records_emitted += pkt.records.size();
        for (const auto& r : pkt.records) {
            if (r.flags & 0x01u) ++stats_.kills_emitted;
        }
        // VC send-seq advances on every data-bearing packet (§14.2).
        session_->next_send_seq = static_cast<std::uint16_t>(
            (session_->next_send_seq + 1) & 0x1FFu);
        if (session_->next_send_seq == 0) session_->next_send_seq = 1;
    }
}

void GhostEmitter::on_client_ack(std::uint16_t /*recv_seq*/)
{
    // v1: per-field ack tracking deferred to 28/04b. The next emit()
    // re-checks dirty against cached snapshot anyway, so out-of-date
    // bits naturally resend.
}

int ghost_emitter_selftest()
{
    // Spawn 3 sessions, each at a unique position. Run 5 emit cycles
    // with synthetic position changes between cycles. Capture bytes
    // into an in-memory sink; assert per-session emission count and
    // round-trip decode fidelity.
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
        std::fputs("[ghost-emit-selftest] session allocate failed\n", stderr);
        return 1;
    }
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

    // 5 cycles. On each cycle bump every session's pos.z so all three
    // emitters see all three peers as dirty.
    for (int t = 0; t < 5; ++t) {
        ServerWorldSnapshot world;
        world.tick = static_cast<std::uint64_t>(t);
        world.server_time_ms = 1000ull + 100ull * static_cast<std::uint64_t>(t);
        world.players = { s1, s2, s3 };
        e1.emit(world);
        e2.emit(world);
        e3.emit(world);
        s1->player_state.pos.z += 1.0f;
        s2->player_state.pos.z += 2.0f;
        s3->player_state.pos.z += 3.0f;
    }

    // Each emitter should have emitted exactly 5 packets (we bumped pos
    // every cycle so the dirty set is always non-empty, and the
    // owning-client filter means each packet carries 2 records: the
    // other two sessions).
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

    // Decode every captured packet and assert it has exactly 2 records
    // (since each session sees 2 peers).
    for (const auto& c : sent) {
        OSGBPacket pkt;
        if (!decode_osgb_packet(c.bytes.data(), c.bytes.size(), pkt)) {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — decode_osgb_packet returned false "
                "for %zuB packet to port %u\n",
                c.bytes.size(), c.peer.port);
            return 1;
        }
        if (pkt.records.size() != 2) {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — expected 2 records, got %zu "
                "(port %u, %zuB)\n",
                pkt.records.size(), c.peer.port, c.bytes.size());
            return 1;
        }
    }

    // Verify the LAST cycle's packets contain the correct final positions.
    // s1's last packet should carry s2 at z=10 and s3 at z=15.
    // s2's last packet should carry s1 at z=5  and s3 at z=15.
    // s3's last packet should carry s1 at z=5  and s2 at z=10.
    auto last_for = [&](const studio::content::net::Endpoint& peer) -> const Captured* {
        const Captured* last = nullptr;
        for (const auto& c : sent) if (c.peer.port == peer.port) last = &c;
        return last;
    };
    auto check_last = [&](const studio::content::net::Endpoint& peer,
                          std::uint16_t expect_gid_a, float expect_z_a,
                          std::uint16_t expect_gid_b, float expect_z_b) -> bool {
        const Captured* last = last_for(peer);
        if (!last) return false;
        OSGBPacket pkt;
        if (!decode_osgb_packet(last->bytes.data(), last->bytes.size(), pkt)) return false;
        bool found_a = false, found_b = false;
        for (const auto& r : pkt.records) {
            if (r.ghost_id == expect_gid_a && std::fabs(r.pos_z - expect_z_a) < 0.01f) found_a = true;
            if (r.ghost_id == expect_gid_b && std::fabs(r.pos_z - expect_z_b) < 0.01f) found_b = true;
        }
        if (!found_a || !found_b) {
            std::fprintf(stderr,
                "[ghost-emit-selftest] FAIL — peer %u last pkt missing record: "
                "gid_a=%u(z=%.2f) found=%d  gid_b=%u(z=%.2f) found=%d\n",
                peer.port,
                expect_gid_a, expect_z_a, found_a ? 1 : 0,
                expect_gid_b, expect_z_b, found_b ? 1 : 0);
            return false;
        }
        return true;
    };
    // After cycle 4 (last emit), positions captured for OTHER peers are
    // those BEFORE that cycle's bump: s1.z=4, s2.z=8, s3.z=12.
    if (!check_last(p1, s2->player_slot, 8.0f,  s3->player_slot, 12.0f)) return 1;
    if (!check_last(p2, s1->player_slot, 4.0f,  s3->player_slot, 12.0f)) return 1;
    if (!check_last(p3, s1->player_slot, 4.0f,  s2->player_slot, 8.0f))  return 1;

    // Now drop s3 from the snapshot and emit one more cycle to verify
    // a kill record goes out to s1 and s2.
    sent.clear();
    {
        ServerWorldSnapshot world;
        world.tick = 100;
        world.server_time_ms = 9999;
        world.players = { s1, s2 };          // s3 absent
        e1.emit(world);
        e2.emit(world);
    }
    bool e1_killed_s3 = false, e2_killed_s3 = false;
    for (const auto& c : sent) {
        OSGBPacket pkt;
        if (!decode_osgb_packet(c.bytes.data(), c.bytes.size(), pkt)) continue;
        for (const auto& r : pkt.records) {
            if (r.ghost_id == s3->player_slot && (r.flags & 0x01u)) {
                if (c.peer.port == p1.port) e1_killed_s3 = true;
                if (c.peer.port == p2.port) e2_killed_s3 = true;
            }
        }
    }
    if (!e1_killed_s3 || !e2_killed_s3) {
        std::fprintf(stderr,
            "[ghost-emit-selftest] FAIL — kill propagation: e1=%d e2=%d\n",
            e1_killed_s3 ? 1 : 0, e2_killed_s3 ? 1 : 0);
        return 1;
    }

    std::fputs("[ghost-emit-selftest] OK — 3 sessions, 5 cycles, kill propagation\n",
               stderr);
    return 0;
}

} // namespace dts_viewer
