#ifndef OSENGINE_GHOST_ENCODER_HPP
#define OSENGINE_GHOST_ENCODER_HPP

// Spec 28/04b — real T1-byte-compatible ghost-stream encoder.
//
// Mirror of the read_* functions in engine/src/ghost_types.cpp. Every
// helper here pairs 1:1 with the BitReader path so a round-trip
// (encode → parse_typed_packet → decoded struct) reproduces the input.
//
// Outer-packet framing:
//   * VC header (variable; encode_vc_header in reliable_acks)
//   * rate-control prefix: two 1-bit flags, both 0 in v1 (no rate change)
//   * event-sub-stream-present:  1 bit, 0 in v1 (chat encoder is 28/10b)
//   * input-sub-stream-present:  1 bit, 0 in v1 (server has no input)
//   * ghost-sub-stream-present:  1 bit, 1
//   * ghost-sub-stream body:
//       mode (1 bit; 1 = scope-always, 0 = normal-delta)
//       width selector (3 bits; encodes idW - 3)
//       per-object loop:
//         object-present (1 bit; 0 terminates)
//         (if scope-always-mode AND object-present == 0: 1 more
//          "complete" bit)
//         ghost_id (idW bits)
//         kill (1 bit)
//         if !kill AND (mode == scope-always OR ghost not previously
//                       introduced to this receiver):
//           object_id (32 bits) + class_tag (10 bits)
//           per-class introduction body (base + shape + class-payload)
//         else if !kill:
//           per-class delta body (base + shape + class-payload)
//
// v1 coverage:
//   - GhostPlayer  (full intro + steady-state delta) — DONE
//   - GhostStaticShape (intro + delta)               — DONE
//   - GhostItem / GhostProjectile / GhostVehicle     — header-only,
//     bodies deferred to 28/04c (don't ship encoders for classes we
//     don't yet emit from server-authoritative state).

#include <osengine/ghost_types.hpp>
#include <osengine/reliable_acks.hpp>

#include <cstdint>
#include <vector>

namespace net20 {

// ----- Bit-level helpers (mirror of ghost_types.cpp BitReader extras) ----

// Write a 32-bit IEEE-754 LE float at the current bit cursor with NO
// byte alignment (§6.9 corrected). Inverse of BitReader::read_float_unaligned.
void write_float_unaligned(BitWriter& w, float f);

// §6.5 v3 normalised signed float in [-1, +1]. Inverse of read_signed_float.
//
//   wire = round( ((f + 1) / 2) * (2^width - 1) )
//
// width 1 always collapses to a single 0 bit per the decoder's special case.
void write_signed_float(BitWriter& w, float f, unsigned width);

// §6.8 Form A normal-vector encode. Mirror of read_normal_vec_form_a.
//
//   write_signed_float(x, bits)
//   write_signed_float(y, bits)
//   write_flag(z < 0)
//
// Caller is responsible for ensuring (x, y, z) is unit-length; receiver
// reconstructs z from sqrt(1 - x^2 - y^2) and uses the sign flag.
void write_normal_vec_form_a(BitWriter& w, unsigned component_bits,
                             float x, float y, float z);

// ----- Per-class body writers (mirror of ghost_types.cpp read_*_body) ----

// §15.0.1 base-state block.
void write_base_state(BitWriter& w, const GhostBaseState& s);

// §15.0.2 shape-layer block.
void write_shape_layer_block(BitWriter& w, const GhostShapeLayer& s);

// §15.5 StaticShape body.
void write_static_shape_body(BitWriter& w, const GhostStaticShape& s);

// §15.1 Player body. `not_initial` MUST match the decoder's
// `!record.full_update` — controls whether the move-block + anim-changed
// bits are written.
void write_player_body(BitWriter& w, const GhostPlayer& p,
                       std::uint8_t datafile_id_width = 8);

// ----- Outer-packet encoders --------------------------------------------

// One typed record to encode. Exactly one of the per-class struct
// pointers must be non-null; `kind` selects which.
struct TypedRecordOut {
    GhostClassKind     kind        = GhostClassKind::Unknown;
    std::uint16_t      ghost_id    = 0;
    bool               kill        = false;
    bool               full_update = false;  // true = write obj_id + class_tag + intro body
    std::uint32_t      object_id   = 0;      // valid iff full_update
    std::uint16_t      class_tag   = 0;      // valid iff full_update
    const GhostPlayer*       player  = nullptr;
    const GhostStaticShape*  statics = nullptr;
};

// Build a complete VC DataPacket whose ghost-sub-stream is in
// scope-always mode (every record is treated as an introduction by
// the receiver). Used for the very first packet sent to a fresh
// connection and for forced re-syncs after long ack gaps.
//
// `id_width_bits` must be in 3..10 inclusive; pick the smallest width
// that fits the largest ghost_id in `records`.
std::vector<std::uint8_t> encode_scope_always_burst(
    const VcHeaderInputs& hdr,
    const std::vector<TypedRecordOut>& records,
    std::uint8_t id_width_bits = 10);

// Build a complete VC DataPacket whose ghost-sub-stream is in normal
// (delta) mode. Records flagged `full_update = true` carry their
// obj_id + class_tag inline, so a receiver who has never heard of
// the ghost can still introduce it without us having to fall back to
// scope-always.
std::vector<std::uint8_t> encode_normal_delta(
    const VcHeaderInputs& hdr,
    const std::vector<TypedRecordOut>& records,
    std::uint8_t id_width_bits = 10);

// Run the spec 28/04b round-trip selftest: encode 1 GhostPlayer intro
// in scope-always mode, decode via parse_typed_packet, assert every
// non-zero field matches. Then encode the same Player in normal-delta
// mode (with a pre-seeded registry), decode, assert again. Repeats
// for GhostStaticShape.
int ghost_encoder_roundtrip_selftest();

}  // namespace net20

#endif  // OSENGINE_GHOST_ENCODER_HPP
