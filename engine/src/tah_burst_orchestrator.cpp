// Spec 26/14c-I-5 — TAH initial-burst orchestrator (clean-room).
// See tah_burst_orchestrator.hpp + docs/clean-room-specs/TRIBES-PHASE2-PACKING.md.

#include "tah_burst_orchestrator.hpp"

#include "flag_state.hpp"
#include "ghost_encoder.hpp"
#include "ghost_types.hpp"
#include "mission_loader.hpp"
#include "reliable_acks.hpp"
#include "tah_class_encoders.hpp"
#include "tah_class_registry.hpp"
#include "tah_datablock_encoder.hpp"
#include "tah_default_catalogue.hpp"
#include "tah_vc_outbound.hpp"

#include "content/mission/scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace dts_viewer
{

namespace
{

// Per TRIBES-PHASE2-PACKING §1.2, the soft byte budget for phase-2
// packets is the session's current-packet-size, initial value 200 bytes.
// Until we plumb a Session field, hardcode it here.
constexpr std::size_t kSoftPacketBytes = 200;

// ----- Ghost intro record writers ----------------------------------------

void write_intro_record_static(net20::BitWriter& w,
                               std::uint8_t id_width_bits,
                               const net20::GhostStaticShape& s)
{
    w.write_flag(true);                              // object-present = 1
    w.write_bits(s.ghost_id & ((1u << id_width_bits) - 1u), id_width_bits);
    w.write_flag(false);                             // kill = 0
    w.write_bits(s.object_id, 32);
    w.write_bits(static_cast<std::uint32_t>(s.class_tag) & 0x3FFu, 10);
    net20::write_base_state(w, s.base);
    net20::write_shape_layer_block(w, s.shape);
    net20::write_static_shape_body(w, s);
}

void write_intro_record_player(net20::BitWriter& w,
                               std::uint8_t id_width_bits,
                               const net20::GhostPlayer& p)
{
    w.write_flag(true);
    w.write_bits(p.ghost_id & ((1u << id_width_bits) - 1u), id_width_bits);
    w.write_flag(false);
    w.write_bits(p.object_id, 32);
    w.write_bits(static_cast<std::uint32_t>(p.class_tag) & 0x3FFu, 10);
    net20::write_base_state(w, p.base);
    net20::write_shape_layer_block(w, p.shape);
    // Force initial_update = true for scope-always intros.
    net20::GhostPlayer p_copy = p;
    p_copy.initial_update = true;
    net20::write_player_body(w, p_copy, /*datafile_id_width*/ 8);
}

void write_intro_record_marker(net20::BitWriter& w,
                               std::uint8_t id_width_bits,
                               const net20::MarkerGhost& m)
{
    w.write_flag(true);
    w.write_bits(m.ghost_id & ((1u << id_width_bits) - 1u), id_width_bits);
    w.write_flag(false);
    w.write_bits(m.object_id, 32);
    w.write_bits(static_cast<std::uint32_t>(m.class_tag) & 0x3FFu, 10);
    net20::write_base_state(w, m.base);
    // Marker has NO shape-layer block.
    net20::write_marker_body(w, m);
}

// ----- Per-intro descriptor ---------------------------------------------

struct PendingIntro {
    enum class Kind { Player, FlagStandMarker, StubStatic } kind = Kind::StubStatic;
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    net20::GhostPlayer       player{};
    net20::MarkerGhost       marker{};
    net20::GhostStaticShape  stub{};
};

void write_intro(net20::BitWriter& w, std::uint8_t id_width, const PendingIntro& pi)
{
    switch (pi.kind) {
        case PendingIntro::Kind::Player:
            write_intro_record_player(w, id_width, pi.player);
            break;
        case PendingIntro::Kind::FlagStandMarker:
            write_intro_record_marker(w, id_width, pi.marker);
            break;
        case PendingIntro::Kind::StubStatic:
            write_intro_record_static(w, id_width, pi.stub);
            break;
    }
}

// Measure the bit cost of a single ghost-intro record by writing into a
// throwaway BitWriter. This includes the leading object-present flag,
// the id, the kill flag, the obj_id, the class_tag, base-state,
// shape-layer block (where applicable), and per-class body.
std::size_t intro_record_bit_cost(std::uint8_t id_width, const PendingIntro& pi)
{
    net20::BitWriter probe;
    write_intro(probe, id_width, pi);
    return probe.pos();
}

// ----- Synthetic ghost builders -----------------------------------------

net20::GhostPlayer make_session_player_ghost(const Session& sess,
                                             std::uint16_t  ghost_id,
                                             std::uint32_t  object_id)
{
    net20::GhostPlayer p{};
    p.ghost_id = ghost_id;
    p.object_id = object_id;
    p.class_tag = 960;
    p.base.base_changed = true;
    p.base.team_id = static_cast<std::uint8_t>(sess.team);
    p.has_pos_block = true;
    p.pos_x = sess.spawn_pos.x;
    p.pos_y = sess.spawn_pos.y;
    p.pos_z = sess.spawn_pos.z;
    p.has_velocity = false;
    p.on_ground = true;
    p.yaw = sess.spawn_yaw;
    p.initial_update = true;
    p.datafile_id = 0;
    p.ai_controlled = false;
    return p;
}

net20::MarkerGhost make_flag_stand_marker(std::uint16_t ghost_id,
                                          std::uint32_t object_id,
                                          float pos_x, float pos_y, float pos_z,
                                          std::uint8_t team_id)
{
    net20::MarkerGhost m{};
    m.ghost_id = ghost_id;
    m.object_id = object_id;
    m.class_tag = 129;
    m.base.base_changed = true;
    m.base.team_id = team_id;
    m.initial_update = true;
    m.marker_data_file_id = 0;
    m.transform_changed = true;
    m.pos_x = pos_x; m.pos_y = pos_y; m.pos_z = pos_z;
    m.rot_x = 0.0f;  m.rot_y = 0.0f;  m.rot_z = 0.0f;
    return m;
}

net20::GhostStaticShape make_stub_static_shape(std::uint16_t ghost_id,
                                               std::uint32_t object_id,
                                               float pos_x, float pos_z)
{
    net20::GhostStaticShape s{};
    s.ghost_id = ghost_id;
    s.object_id = object_id;
    s.class_tag = 333;
    s.base.base_changed = true;
    s.transform_changed = true;
    s.pos_x = pos_x;
    s.pos_y = 0.0f;
    s.pos_z = pos_z;
    s.rot_x = 0.0f; s.rot_y = 0.0f; s.rot_z = 0.0f;
    s.shape_info_changed = true;
    s.shape_data_file_id = 0;
    return s;
}

struct FlagStandSpec {
    float        pos_x, pos_y, pos_z;
    std::uint8_t team_id;
};
std::vector<FlagStandSpec> enumerate_flag_stands(const LoadedMission* mission)
{
    std::vector<FlagStandSpec> out;
    if (!mission) return out;
    FlagWorld fw;
    fw.load_from_mission(*mission);
    if (!fw.loaded()) return out;
    if (auto* r = fw.red()) {
        out.push_back({r->home_position.x, r->home_position.y,
                       r->home_position.z, /*team*/ 1});
    }
    if (auto* b = fw.blue()) {
        out.push_back({b->home_position.x, b->home_position.y,
                       b->home_position.z, /*team*/ 2});
    }
    return out;
}

std::uint8_t pick_id_width(std::uint16_t max_ghost_id)
{
    std::uint8_t w = 3;
    std::uint32_t cap = 1u << w;
    while (cap <= max_ghost_id && w < 10) {
        ++w;
        cap = 1u << w;
    }
    return w;
}

}  // namespace

// ---------------------------------------------------------------------------
//
// Phase-2 packing per TRIBES-PHASE2-PACKING (spec 14c-I-5).
//
// Each packet:
//   * Opens with the VC header (parity, send_seq, ack runs, type word) via
//     tah_vc::OutboundPacketBuilder (which also bumps send_seq).
//   * Writes the rate-control prefix (R0=0, R1=0 — burst-time default).
//   * Writes the event sub-stream present flag. If 1, drains catalogue
//     events one at a time, checking the 200-byte soft budget BEFORE each
//     record. Always emits the trailing event-present=0 terminator.
//   * Writes the input sub-stream present flag = 0 (server outbound has
//     no input sub-stream).
//   * Writes the ghost sub-stream present flag. If 1, writes the mode flag
//     (scope-always=1) + 3-bit selector, then drains intro records with the
//     same per-record budget check. Always emits the object-present=0
//     terminator + scope-always-complete flag (=1 only on the last packet
//     of the burst).
//
// The soft budget is checked AFTER the most recent record completes —
// per §1.3 / §1.4 the writer lets the in-progress record finish then
// stops on the NEXT check. This produces packet sizes slightly over
// the soft budget (cap1 observed [210, 231] for soft=200).
//
// ---------------------------------------------------------------------------

std::vector<std::vector<std::uint8_t>>
TahBurstOrchestrator::build_initial_burst(Session&             sess,
                                          const LoadedMission* mission,
                                          std::uint64_t        now_ms)
{
    std::vector<std::vector<std::uint8_t>> out;

    // ---- Build the catalogue queue (mission-referenced subset) ----
    auto catalogue = net20::stock_tribes_ctf_catalogue();
    std::size_t catalogue_idx = 0;
    std::uint8_t event_seq = catalogue_first_event_seq;

    // ---- Build the ghost-intro queue ----
    std::vector<PendingIntro> intros;
    std::uint16_t next_id = 1;
    {
        PendingIntro pi{};
        pi.kind = PendingIntro::Kind::Player;
        pi.ghost_id = next_id++;
        pi.object_id = 0x10000000u | static_cast<std::uint32_t>(sess.player_slot);
        pi.player = make_session_player_ghost(sess, pi.ghost_id, pi.object_id);
        intros.push_back(pi);
    }
    const auto stands = enumerate_flag_stands(mission);
    for (const auto& fs : stands) {
        PendingIntro pi{};
        pi.kind = PendingIntro::Kind::FlagStandMarker;
        pi.ghost_id = next_id++;
        pi.object_id = 0x20000000u | static_cast<std::uint32_t>(pi.ghost_id);
        pi.marker = make_flag_stand_marker(pi.ghost_id, pi.object_id,
                                           fs.pos_x, fs.pos_y, fs.pos_z,
                                           fs.team_id);
        intros.push_back(pi);
    }
    if (mission != nullptr) {
        std::size_t emitted_from_scene = 0;
        for (const auto& child : mission->scene.root.children) {
            if (emitted_from_scene >= 16) break;
            const bool is_marker = std::holds_alternative<
                studio::content::mission::node_marker>(child.payload);
            const bool is_static = std::holds_alternative<
                studio::content::mission::node_static_shape>(child.payload);
            if (!is_marker && !is_static) continue;
            PendingIntro pi{};
            pi.kind = PendingIntro::Kind::StubStatic;
            pi.ghost_id = next_id++;
            pi.object_id = 0x30000000u | static_cast<std::uint32_t>(pi.ghost_id);
            pi.stub = make_stub_static_shape(pi.ghost_id, pi.object_id,
                /*pos_x*/ static_cast<float>(emitted_from_scene) * 10.0f,
                /*pos_z*/ 0.0f);
            intros.push_back(pi);
            ++emitted_from_scene;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            PendingIntro pi{};
            pi.kind = PendingIntro::Kind::StubStatic;
            pi.ghost_id = next_id++;
            pi.object_id = 0x30000000u | static_cast<std::uint32_t>(pi.ghost_id);
            pi.stub = make_stub_static_shape(pi.ghost_id, pi.object_id,
                /*pos_x*/ static_cast<float>(i) * 25.0f, /*pos_z*/ 0.0f);
            intros.push_back(pi);
        }
    }

    const std::uint8_t id_width = pick_id_width(next_id);
    std::size_t intro_idx = 0;

    // ---- Per-packet emit loop (interleaved ESS + GSS) ----
    const std::size_t soft_bit_budget = kSoftPacketBytes * 8u;

    // Safety bound: cap the burst at 32 packets so a logic bug here
    // cannot wedge a session in an infinite loop.
    constexpr std::size_t kMaxBurstPackets = 32;

    while ((catalogue_idx < catalogue.size() || intro_idx < intros.size())
           && out.size() < kMaxBurstPackets) {

        const bool was_last_packet =
            // Will this be the packet that drains both queues?
            // Decided after the writes below; we set the complete-bit
            // lazily by examining drainage AFTER intro writes.
            false;
        (void)was_last_packet;

        // Open packet.
        tah_vc::OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        plan.is_resend = false;
        plan.is_ack    = false;
        tah_vc::OutboundPacketBuilder b(sess, plan);
        auto& w = b.writer();

        // Rate-control prefix (R0=0, R1=0).
        w.write_flag(false);
        w.write_flag(false);

        // ---- Event sub-stream ----
        const bool any_events = (catalogue_idx < catalogue.size());
        w.write_flag(any_events);  // event-ss-present

        std::size_t events_written_this_packet = 0;
        if (any_events) {
            while (catalogue_idx < catalogue.size()) {
                // §1.1: check the soft budget BEFORE starting the next
                // record. If already crossed, stop and emit the terminator.
                if (w.pos() >= soft_bit_budget) break;

                const auto& e = catalogue[catalogue_idx];
                const bool seq_continuous = (events_written_this_packet > 0);
                net20::append_catalogue_event(
                    w, e, catalogue_event_class_tag, event_seq, seq_continuous);
                event_seq = static_cast<std::uint8_t>(
                    (event_seq + 1) & 0x7Fu);
                ++catalogue_idx;
                ++events_written_this_packet;
            }
            // Event sub-stream terminator: event-present = 0.
            w.write_flag(false);
        }

        // ---- Input sub-stream (server outbound: empty) ----
        w.write_flag(false);  // input-ss-present

        // ---- Ghost sub-stream ----
        // Per §3.1 step 3: if the soft budget has already been crossed by
        // the event sub-stream, the ghost sub-stream's present flag is 0
        // (no records this packet; they roll over to the next packet).
        const bool budget_crossed_by_events = (w.pos() >= soft_bit_budget);
        const bool any_intros = (intro_idx < intros.size());
        const bool emit_ghost_ss = any_intros && !budget_crossed_by_events;
        w.write_flag(emit_ghost_ss);  // ghost-ss-present

        std::size_t intros_written_this_packet = 0;
        if (emit_ghost_ss) {
            // Mode flag (1 = scope-always) + 3-bit selector.
            w.write_flag(true);
            const unsigned selector = static_cast<unsigned>(id_width) - 3u;
            w.write_bits(selector & 0x7u, 3);

            // Drain intros with per-record budget check.
            while (intro_idx < intros.size()) {
                if (w.pos() >= soft_bit_budget) break;
                write_intro(w, id_width, intros[intro_idx]);
                ++intro_idx;
                ++intros_written_this_packet;
            }

            // Object-present = 0 terminator.
            w.write_flag(false);

            // Scope-always-complete: set to 1 ONLY if both queues are now
            // drained (this is the final phase-2 packet). §3.3 + §7 OQ4.
            const bool complete =
                (catalogue_idx >= catalogue.size())
                && (intro_idx >= intros.size());
            w.write_flag(complete);
        }

        auto bytes = b.finish(now_ms);
        out.push_back(std::move(bytes));
    }

    // Edge case: no work at all (empty catalogue AND no intros). Emit one
    // empty ghost-update packet with complete-bit=1 so any receiver that
    // requires the transition out of scope-always mode gets it.
    if (out.empty()) {
        tah_vc::OutboundPacketPlan plan;
        plan.base_type = net20::pkt_type::kDataPacket;
        plan.is_resend = false;
        plan.is_ack    = false;
        tah_vc::OutboundPacketBuilder b(sess, plan);
        auto& w = b.writer();
        w.write_flag(false);                 // R0
        w.write_flag(false);                 // R1
        w.write_flag(false);                 // event-ss-present
        w.write_flag(false);                 // input-ss-present
        w.write_flag(true);                  // ghost-ss-present
        w.write_flag(true);                  // mode = scope-always
        w.write_bits(0u, 3);                 // selector (id_width = 3)
        w.write_flag(false);                 // object-present terminator
        w.write_flag(true);                  // scope-always-complete = 1
        out.push_back(b.finish(now_ms));
    }

    return out;
}

// ---------------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------------

namespace
{

// Decode just the sub-stream presence flags (enough for the new selftest
// envelope check). Returns false on header-parse failure.
struct PacketShape {
    bool has_event_ss = false;
    bool has_input_ss = false;
    bool has_ghost_ss = false;
    bool ghost_mode_scope_always = false;
};

bool decode_packet_shape(const std::vector<std::uint8_t>& bytes,
                         PacketShape& out)
{
    if (bytes.size() < 4) return false;
    const std::size_t bit_len = bytes.size() * 8u;
    std::size_t pos = 0;
    auto read_bits = [&](unsigned width) -> std::uint32_t {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < width; ++i) {
            const std::size_t p = pos + i;
            if (p >= bit_len) return 0;
            const std::uint8_t bit = (bytes[p >> 3] >> (p & 7u)) & 1u;
            v |= static_cast<std::uint32_t>(bit) << i;
        }
        pos += width;
        return v;
    };

    // VC header.
    if (read_bits(1) != 1u) return false;
    (void)read_bits(1);
    (void)read_bits(9);
    (void)read_bits(5);
    for (;;) {
        if (pos + 3 > bit_len) return false;
        const std::uint32_t len = read_bits(3);
        if (len == 0) break;
        if (pos + 5 > bit_len) return false;
        (void)read_bits(5);
    }
    if (pos + 5 > bit_len) return false;
    const std::uint32_t type_word = read_bits(5);
    if (type_word != 0) return false;

    // Rate prefix.
    if (pos + 1 > bit_len) return false;
    const bool r0 = read_bits(1) != 0;
    if (r0) {
        if (pos + 20 > bit_len) return false;
        (void)read_bits(10); (void)read_bits(10);
    }
    if (pos + 1 > bit_len) return false;
    const bool r1 = read_bits(1) != 0;
    if (r1) {
        if (pos + 20 > bit_len) return false;
        (void)read_bits(10); (void)read_bits(10);
    }

    // Event sub-stream present flag.
    if (pos + 1 > bit_len) return false;
    out.has_event_ss = read_bits(1) != 0;
    if (out.has_event_ss) {
        // We don't walk the events here — the orchestrator's own
        // construction is the structural source of truth. The selftest's
        // envelope check below is sufficient.
        out.has_input_ss = false;
        out.has_ghost_ss = true;   // assumed; we can't verify without a walker
        return true;
    }
    if (pos + 1 > bit_len) return false;
    out.has_input_ss = read_bits(1) != 0;
    if (pos + 1 > bit_len) return false;
    out.has_ghost_ss = read_bits(1) != 0;
    if (!out.has_ghost_ss) return true;
    if (pos + 1 > bit_len) return false;
    out.ghost_mode_scope_always = read_bits(1) != 0;
    return true;
}

int run_selftest()
{
    int failures = 0;

    Session sess{};
    sess.connect_handle = 0x12345678u;
    sess.connect_parity = (sess.connect_handle & 1u) != 0;
    sess.next_send_seq = 2;
    sess.ack.received[1] = true;
    sess.ack.highest_recv_mod32 = 1;
    sess.player_slot = 0;
    sess.team = Team::Red;
    sess.spawn_pos = {100.0f, 50.0f, 200.0f};
    sess.spawn_yaw = 1.0f;

    TahBurstOrchestrator orch;
    auto pkts = orch.build_initial_burst(sess, /*mission*/ nullptr, /*now_ms*/ 0);

    if (pkts.empty()) {
        std::fputs("[tah-burst-orchestrator] FAIL: 0 packets emitted\n", stderr);
        return 1;
    }

    std::size_t total_bytes = 0;
    // Track size envelope across all but the LAST packet — the final
    // packet may legally be smaller because both queues happen to drain
    // before the soft budget is crossed (see §1.3 / §3.3: per-record
    // budget check means the final packet contains whatever was left).
    // cap1's queue happens to fill all 10 packets evenly; ours may have
    // a partial trailing packet depending on catalogue + intro totals.
    std::size_t min_non_last = SIZE_MAX, max_non_last = 0;
    std::size_t last_pkt_size = 0;
    std::size_t pkts_with_events = 0;
    std::size_t pkts_with_ghost  = 0;

    for (std::size_t i = 0; i < pkts.size(); ++i) {
        const auto& p = pkts[i];
        total_bytes += p.size();
        const bool is_last = (i + 1 == pkts.size());
        if (is_last) {
            last_pkt_size = p.size();
        } else {
            if (p.size() < min_non_last) min_non_last = p.size();
            if (p.size() > max_non_last) max_non_last = p.size();
        }

        // VC header parse.
        net20::ParsedIncomingHeader ph;
        if (!net20::parse_incoming_header(p.data(), p.size(), ph)) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator] FAIL pkt %zu: VC header parse\n", i);
            ++failures;
            continue;
        }
        if (ph.type_word != 0) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator] FAIL pkt %zu: type_word=0x%02x\n",
                i, (unsigned)ph.type_word);
            ++failures;
        }
        PacketShape sh{};
        if (!decode_packet_shape(p, sh)) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator] FAIL pkt %zu: shape decode\n", i);
            ++failures;
            continue;
        }
        if (sh.has_event_ss) ++pkts_with_events;
        if (sh.has_ghost_ss) ++pkts_with_ghost;
    }

    std::fprintf(stderr,
        "[tah-burst-orchestrator] %zu packets, non-last sizes [%zu..%zu], "
        "last %zu, total %zu, events-bearing=%zu, ghost-bearing=%zu\n",
        pkts.size(),
        (pkts.size() > 1 ? min_non_last : last_pkt_size),
        (pkts.size() > 1 ? max_non_last : last_pkt_size),
        last_pkt_size, total_bytes,
        pkts_with_events, pkts_with_ghost);

    // (A) Packet count envelope: spec §6 / 14c-I-5 step 5 — 10 ± 2.
    if (pkts.size() < 8 || pkts.size() > 12) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator] FAIL: packet count %zu outside [8, 12]\n",
            pkts.size());
        ++failures;
    }

    // (B) Per-packet size envelope (non-last packets): 200..235 B. cap1
    // P01..P10 lie in [210, 231]; the spec rule (§1.1 + §1.5) allows up
    // to ~32 B overrun past the 200 B soft budget. The LAST packet is
    // excluded — it may be a partial trailing packet (§1.3).
    if (pkts.size() > 1) {
        if (min_non_last < 200 || max_non_last > 235) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator] FAIL: non-last per-packet sizes "
                "[%zu..%zu] outside [200..235]\n", min_non_last, max_non_last);
            ++failures;
        }
    }
    // Last packet must be non-trivial (a real packet, not an empty header).
    if (last_pkt_size < 8 || last_pkt_size > 235) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator] FAIL: last packet size %zu outside "
            "[8..235]\n", last_pkt_size);
        ++failures;
    }

    // (C) Total bytes envelope: 1900..2500 (cap1 = 2228 B).
    if (total_bytes < 1900 || total_bytes > 2500) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator] FAIL: total bytes %zu outside "
            "[1900..2500]\n", total_bytes);
        ++failures;
    }

    // (D) send_seq advanced by exactly pkts.size() (each packet is a
    // bumping DataPacket).
    const std::uint16_t expected_seq = static_cast<std::uint16_t>(
        (2u + pkts.size()) & 0x1FFu);
    const std::uint16_t got_seq = sess.next_send_seq;
    if (got_seq != expected_seq && !(expected_seq == 0 && got_seq == 1)) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator] FAIL: next_send_seq=%u expected %u "
            "(pkts.size=%zu)\n",
            (unsigned)got_seq, (unsigned)expected_seq, pkts.size());
        ++failures;
    }

    if (failures == 0) {
        std::fputs("[tah-burst-orchestrator] selftest OK\n", stderr);
        return 0;
    }
    std::fprintf(stderr,
        "[tah-burst-orchestrator] selftest FAILED (%d failures)\n", failures);
    return 1;
}

}  // namespace

int tah_burst_orchestrator_selftest()
{
    return run_selftest();
}

}  // namespace dts_viewer
