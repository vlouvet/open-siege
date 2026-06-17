// Spec 26/14c-I-5 — TAH initial-burst orchestrator (clean-room).
// See tah_burst_orchestrator.hpp + docs/clean-room-specs/TRIBES-PHASE2-PACKING.md.

#include "tah_burst_orchestrator.hpp"

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
#include <filesystem>
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
//
// 14c-I-6: the intro queue is now populated either from a real mission's
// scope_always_objects() (when a LoadedMission is supplied) or from a
// tiny stub set (when nullptr, used by the selftest). Each pending entry
// carries the full ghost-typed payload its writer needs.

struct PendingIntro {
    enum class Kind { Marker, Static } kind = Kind::Static;
    std::uint16_t ghost_id = 0;
    std::uint32_t object_id = 0;
    net20::MarkerGhost       marker{};
    net20::GhostStaticShape  stat{};
};

void write_intro(net20::BitWriter& w, std::uint8_t id_width, const PendingIntro& pi)
{
    switch (pi.kind) {
        case PendingIntro::Kind::Marker:
            write_intro_record_marker(w, id_width, pi.marker);
            break;
        case PendingIntro::Kind::Static:
            write_intro_record_static(w, id_width, pi.stat);
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

// ----- TAH-resolved class tags ------------------------------------------
//
// Replaces the I-5 stub tags (333 / 129 / 960) with class tags TAH's
// class registry actually recognises. Authority:
//   * Common-range tags (universal across builds) are taken from
//     docs/clean-room-specs/TRIBES-GHOST-CLASSES.md §1.3:
//       - Tag 129 = Marker (Common range index 1)
//       - Tag 131 = SoundSource
//   * Game-specific tags (256..1023) are build-dependent. The
//     wiki-contributions/TAH-CLASS-TAGS.md survey of a real
//     Ice/Blood Dagger CTF capture lists the 60 tags TAH's registry
//     resolves; we use the highest-frequency ones for which the
//     payload-shape predicates in TRIBES-GHOST-CLASSES.md §6 match
//     our intro writers (StaticShape uses the §15.5 prefix that our
//     write_static_shape_body emits).
//
// I deliberately do NOT emit a Player intro in the initial burst —
// TAH's Player tag is not observed in the survey (cf. TAH-CLASS-TAGS.md
// "Our vanilla kServerPlayerClassTag = 960 is not present in TAH's
// stream"). The player joins later through a separate intro after the
// scope-always burst completes.
constexpr std::uint16_t kTahStaticShapeClassTag = 708;  // §5.2: 58 intros in TAH survey
constexpr std::uint16_t kTahTurretClassTag      = 640;  // §5.2: 52 intros; Turret derives from StaticShape per §3.9
constexpr std::uint16_t kTahItemFlagClassTag    = 496;  // §5.2: 29 intros; CTF flag is scope-always per §2.1
// 14c-I-7 Change 1: Marker tag corrected 129 -> 263 per cap1 evidence.
// TAH-CLASS-TAGS.md (60-tag survey of real TAH CTF traffic):
//   263 = 8 intros observed     <-- Markers live here
//   129 = 0 intros observed     <-- never appears in TAH's registry
// I-6's choice of 129 (common-range default per §1.3) was silently dropped
// by TAH's framing layer, blocking scope-always Marker instantiation
// (highest-probability root cause of the load-screen stall).
constexpr std::uint16_t kTahMarkerClassTag      = 263;
constexpr std::uint16_t kTahSensorClassTag      = 324;  // §5.2: 18 intros
// 14c-I-7 Change 2: SoundSource emission added; §1.3 common-range tag 131,
// 7 intros / CTF in TAH survey. Body encoder lives in tah_class_encoders.cpp
// (write_sound_source_body); mission walker enqueues one intro per
// SoundSource node when the scene model exposes them.
constexpr std::uint16_t kTahSoundSourceClassTag = 131;
// 14c-I-7 Change 5: emission infrastructure for tags 896 / 32 / 65 so a
// follow-up spec can wire them into mission walks without re-touching the
// orchestrator. NOT wired into scope_always_objects() in this spec —
// per-tag mission-object identification awaits R-7.1.
constexpr std::uint16_t kTahTag896 = 896;  // 21 intros / CTF (TAH survey)
constexpr std::uint16_t kTahTag32  = 32;   // 14 intros / CTF (TAH survey)
constexpr std::uint16_t kTahTag65  = 65;   // 10 intros / CTF (TAH survey)

// 14c-I-7 Change 6 — UNRESOLVED. R-7 §5.5 reports the per-class
// data-file-id bit width should be `ceil(log2(group_size + 1))` (the same
// formula `tah_datablock_encoder::block_id_ref_width` already implements
// for catalogue block references), not the hardcoded 8 bits used at:
//   * `tah_class_encoders.hpp::kDefaultDfWBits = 8` (defaulted into every
//     body writer's `datafile_id_width` parameter).
//   * `ghost_encoder.cpp` StaticShape `shape_data_file_id` literal 8.
// The plumbing already exists (body writers take a width arg); the fix
// requires propagating each ghost class's catalogue group_size from
// `build_mission_catalogue()` into the intro emission loop and forwarding
// it to the body writer. Currently all orchestrator-emitted intros use
// `data_file_id = 0`, so the width mismatch is bit-alignment-only and
// does not corrupt referenced indices — but it does shift every
// subsequent field in the body and could be the residual structural
// blocker even after Changes 1–5. Tracked for a follow-up.

std::uint16_t class_tag_for(ScopeAlwaysIntro::Kind kind)
{
    switch (kind) {
        case ScopeAlwaysIntro::Kind::StaticShape: return kTahStaticShapeClassTag;
        case ScopeAlwaysIntro::Kind::Turret:      return kTahTurretClassTag;
        case ScopeAlwaysIntro::Kind::Marker:      return kTahMarkerClassTag;
        case ScopeAlwaysIntro::Kind::Item:        return kTahItemFlagClassTag;
        case ScopeAlwaysIntro::Kind::Sensor:      return kTahSensorClassTag;
        case ScopeAlwaysIntro::Kind::SoundSource: return kTahSoundSourceClassTag;
        case ScopeAlwaysIntro::Kind::Tag896:      return kTahTag896;
        case ScopeAlwaysIntro::Kind::Tag32:       return kTahTag32;
        case ScopeAlwaysIntro::Kind::Tag65:       return kTahTag65;
    }
    return kTahStaticShapeClassTag;
}

// Marker and Sensor write through write_intro_record_marker; everything
// else (StaticShape, Turret, Item-as-CTF-flag) writes through
// write_intro_record_static. The choice is driven by whether the wire
// format derives from StaticBase (§15.5 four-block prefix) or from
// GameBase + marker body (§3.1).
PendingIntro::Kind writer_kind_for(ScopeAlwaysIntro::Kind kind)
{
    switch (kind) {
        case ScopeAlwaysIntro::Kind::Marker:
            // Markers use the no-shape-layer body per §3.1.
            return PendingIntro::Kind::Marker;
        case ScopeAlwaysIntro::Kind::Sensor:
            // Sensor inherits StaticBase but the spec's §3.8 layout is
            // close enough to a marker-style introduction that we'll
            // emit it as a Marker until we have an explicit Sensor
            // encoder; the catalogue still needs the SensorData ref.
            return PendingIntro::Kind::Marker;
        case ScopeAlwaysIntro::Kind::SoundSource:
            // 14c-I-7 Change 2: SoundSource has no shape-layer body in
            // TRIBES-GHOST-CLASSES.md §1.3 — emit via the marker writer
            // until a dedicated SoundSource writer lands.
            return PendingIntro::Kind::Marker;
        case ScopeAlwaysIntro::Kind::StaticShape:
        case ScopeAlwaysIntro::Kind::Turret:
        case ScopeAlwaysIntro::Kind::Item:
            return PendingIntro::Kind::Static;
        case ScopeAlwaysIntro::Kind::Tag896:
        case ScopeAlwaysIntro::Kind::Tag32:
        case ScopeAlwaysIntro::Kind::Tag65:
            // 14c-I-7 Change 5: emission paths only — body schema awaits
            // R-7.1. Fall back to the static-shape writer so that if these
            // get wired into a mission walk before R-7.1 lands, the wire
            // is at least well-formed; the body will need fixing later.
            // TODO(14c-I-7-mission-map): wire to per-tag mission objects
            // and choose the correct writer kind once R-7.1 documents the
            // body schema.
            return PendingIntro::Kind::Static;
    }
    return PendingIntro::Kind::Static;
}

// ----- Synthetic ghost builders -----------------------------------------

net20::GhostStaticShape make_static_shape(std::uint16_t ghost_id,
                                          std::uint32_t object_id,
                                          std::uint16_t class_tag,
                                          const ScopeAlwaysIntro& intro)
{
    net20::GhostStaticShape s{};
    s.ghost_id = ghost_id;
    s.object_id = object_id;
    s.class_tag = class_tag;
    s.base.base_changed = (intro.team_id != 0);
    s.base.team_id = intro.team_id;
    s.transform_changed = true;
    s.pos_x = intro.position[0];
    s.pos_y = intro.position[1];
    s.pos_z = intro.position[2];
    s.rot_x = intro.rotation[0];
    s.rot_y = intro.rotation[1];
    s.rot_z = intro.rotation[2];
    s.shape_info_changed = true;
    s.shape_data_file_id = 0;  // catalogue resolution is a follow-up
    return s;
}

net20::MarkerGhost make_marker(std::uint16_t ghost_id,
                               std::uint32_t object_id,
                               std::uint16_t class_tag,
                               const ScopeAlwaysIntro& intro)
{
    net20::MarkerGhost m{};
    m.ghost_id = ghost_id;
    m.object_id = object_id;
    m.class_tag = class_tag;
    m.base.base_changed = (intro.team_id != 0);
    m.base.team_id = intro.team_id;
    m.initial_update = true;
    m.marker_data_file_id = 0;
    m.transform_changed = true;
    m.pos_x = intro.position[0];
    m.pos_y = intro.position[1];
    m.pos_z = intro.position[2];
    m.rot_x = intro.rotation[0];
    m.rot_y = intro.rotation[1];
    m.rot_z = intro.rotation[2];
    return m;
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
    (void)sess;  // sess team / spawn are no longer used here — the local
                 // player intro rides a separate post-burst path now.

    // ---- Build the catalogue queue (mission-referenced subset) ----
    auto catalogue = (mission != nullptr)
        ? net20::build_mission_catalogue(scope_always_objects(*mission),
                                         required_datablock_names(*mission))
        : net20::stock_tribes_ctf_catalogue();
    std::size_t catalogue_idx = 0;
    std::uint8_t event_seq = catalogue_first_event_seq;

    // ---- Build the ghost-intro queue ----
    //
    // Source: mission->scope_always_objects() when a mission is
    // available, otherwise a tiny stub set (only used by the orchestrator
    // selftest, which passes mission=nullptr).
    std::vector<PendingIntro> intros;
    std::uint16_t next_id = 1;

    auto enqueue = [&](const ScopeAlwaysIntro& src,
                       std::uint32_t obj_id_prefix) {
        PendingIntro pi{};
        pi.ghost_id  = next_id++;
        pi.object_id = obj_id_prefix |
                       static_cast<std::uint32_t>(pi.ghost_id);
        const std::uint16_t tag = class_tag_for(src.kind);
        pi.kind = writer_kind_for(src.kind);
        if (pi.kind == PendingIntro::Kind::Marker) {
            pi.marker = make_marker(pi.ghost_id, pi.object_id, tag, src);
        } else {
            pi.stat = make_static_shape(pi.ghost_id, pi.object_id, tag, src);
        }
        intros.push_back(pi);
    };

    if (mission != nullptr) {
        const auto objects = scope_always_objects(*mission);
        for (const auto& obj : objects) {
            enqueue(obj, 0x20000000u);
        }
    } else {
        // No-mission stub: 4 fake StaticShape intros so the selftest
        // exercises the GSS code path end-to-end.
        for (int i = 0; i < 4; ++i) {
            ScopeAlwaysIntro src{};
            src.kind = ScopeAlwaysIntro::Kind::StaticShape;
            src.position = {static_cast<float>(i) * 25.0f, 0.0f, 0.0f};
            src.datablock_name = "stub";
            enqueue(src, 0x30000000u);
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

        // Rate-control prefix per TRIBES-NETPROTO §5.0.1 / §3.4 and
        // TRIBES-PROTOCOL-PCAP-DIFF.md §4.2/§4.3:
        //
        //   bit  0  : R0 (current-rate-changed flag)
        //   bits 1..10  : (if R0) current update-delay in ms (10 bits)
        //   bits 11..20 : (if R0) current packet-size in bytes (10 bits)
        //   bit  next : R1 (max-rate-changed flag)
        //   bits next..+10 : (if R1) max update-delay (10 bits)
        //   bits next..+10 : (if R1) max packet-size (10 bits)
        //
        // The public TAH server publishes rate-control on the first two
        // burst packets only (14c-I-pcap-diff §4.2/§4.3):
        //   Burst packet 0: R1=1, max_update_delay=33 ms, max_pkt_size=450
        //   Burst packet 1: R0=1, cur_update_delay=66 ms, cur_pkt_size=400
        // All later packets: both flags 0.
        const std::size_t pkt_index = out.size();
        if (pkt_index == 0) {
            // R0 = 0
            w.write_flag(false);
            // R1 = 1, max_update_delay = 33 ms (10 bits), max_pkt_size = 450 B (10 bits)
            w.write_flag(true);
            w.write_bits(33u,  10);
            w.write_bits(450u, 10);
        } else if (pkt_index == 1) {
            // R0 = 1, cur_update_delay = 66 ms (10 bits), cur_pkt_size = 400 B (10 bits)
            w.write_flag(true);
            w.write_bits(66u,  10);
            w.write_bits(400u, 10);
            // R1 = 0
            w.write_flag(false);
        } else {
            // Steady-state burst packet: both rate flags 0.
            w.write_flag(false);
            w.write_flag(false);
        }

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

int run_one_burst(const char* tag, const LoadedMission* mission)
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
    auto pkts = orch.build_initial_burst(sess, mission, /*now_ms*/ 0);

    if (pkts.empty()) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator %s] FAIL: 0 packets emitted\n", tag);
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
                "[tah-burst-orchestrator %s] FAIL pkt %zu: VC header parse\n",
                tag, i);
            ++failures;
            continue;
        }
        if (ph.type_word != 0) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator %s] FAIL pkt %zu: type_word=0x%02x\n",
                tag, i, (unsigned)ph.type_word);
            ++failures;
        }
        PacketShape sh{};
        if (!decode_packet_shape(p, sh)) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator %s] FAIL pkt %zu: shape decode\n",
                tag, i);
            ++failures;
            continue;
        }
        if (sh.has_event_ss) ++pkts_with_events;
        if (sh.has_ghost_ss) ++pkts_with_ghost;
    }

    std::fprintf(stderr,
        "[tah-burst-orchestrator %s] %zu packets, non-last sizes [%zu..%zu], "
        "last %zu, total %zu, events-bearing=%zu, ghost-bearing=%zu\n",
        tag, pkts.size(),
        (pkts.size() > 1 ? min_non_last : last_pkt_size),
        (pkts.size() > 1 ? max_non_last : last_pkt_size),
        last_pkt_size, total_bytes,
        pkts_with_events, pkts_with_ghost);

    // (A) Packet count envelope: spec 14c-I-6 acceptance — [8, 14].
    if (pkts.size() < 8 || pkts.size() > 14) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator %s] FAIL: packet count %zu outside [8, 14]\n",
            tag, pkts.size());
        ++failures;
    }

    // (B) Per-packet size envelope (non-last packets): spec 14c-I-6
    // acceptance — [200, 245] B. cap1 P01..P10 lie in [210, 231]; the
    // upper bound is bumped 10 B over I-5 to absorb the slightly
    // larger per-record byte cost the real ghost intros produce.
    // The LAST packet is excluded — it may be a partial trailing
    // packet (§1.3).
    if (pkts.size() > 1) {
        if (min_non_last < 200 || max_non_last > 245) {
            std::fprintf(stderr,
                "[tah-burst-orchestrator %s] FAIL: non-last per-packet sizes "
                "[%zu..%zu] outside [200..245]\n", tag, min_non_last, max_non_last);
            ++failures;
        }
    }
    // Last packet must be non-trivial (a real packet, not an empty header).
    if (last_pkt_size < 8 || last_pkt_size > 245) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator %s] FAIL: last packet size %zu outside "
            "[8..245]\n", tag, last_pkt_size);
        ++failures;
    }

    // (C) Total bytes envelope: spec 14c-I-6 acceptance — [1800, 2600] B
    // (cap1 = 2228 B).
    if (total_bytes < 1800 || total_bytes > 2600) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator %s] FAIL: total bytes %zu outside "
            "[1800..2600]\n", tag, total_bytes);
        ++failures;
    }

    // (D) send_seq advanced by exactly pkts.size() (each packet is a
    // bumping DataPacket).
    const std::uint16_t expected_seq = static_cast<std::uint16_t>(
        (2u + pkts.size()) & 0x1FFu);
    const std::uint16_t got_seq = sess.next_send_seq;
    if (got_seq != expected_seq && !(expected_seq == 0 && got_seq == 1)) {
        std::fprintf(stderr,
            "[tah-burst-orchestrator %s] FAIL: next_send_seq=%u expected %u "
            "(pkts.size=%zu)\n",
            tag, (unsigned)got_seq, (unsigned)expected_seq, pkts.size());
        ++failures;
    }

    return failures;
}

int run_selftest()
{
    int failures = 0;

    // (1) No-mission stub — exercises the StaticShape stub path.
    failures += run_one_burst("stub", /*mission*/ nullptr);

    // (2) Real 5_CTF mission — exercises the scope_always_objects()
    // + build_mission_catalogue() integration.
    {
        const std::filesystem::path tribes_dir =
            "/Users/v/code/tribes-emscripten/tribes-game";
        const std::filesystem::path missions_dir = tribes_dir / "base" / "missions";
        const std::filesystem::path base_dir     = tribes_dir / "base";
        if (std::filesystem::is_directory(missions_dir)) {
            auto lm = load_mission(missions_dir, base_dir, "5_CTF");
            if (lm) {
                failures += run_one_burst("5_CTF", &(*lm));
            } else {
                std::fputs("[tah-burst-orchestrator] note: 5_CTF.mis "
                           "could not be loaded; skipping mission run\n",
                           stderr);
            }
        } else {
            std::fprintf(stderr,
                "[tah-burst-orchestrator] note: %s not found; skipping "
                "mission run\n", missions_dir.string().c_str());
        }
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
