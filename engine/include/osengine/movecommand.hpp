// Track 20 spec 19 — client-input ("movecommand") encoder (clean-room).
//
// Builds the c->s input sub-stream + surrounding framing described in
// §17 of docs/clean-room-specs/TRIBES-NETPROTO.md. Once the connection
// is past AcceptConnect + the §16.5 progression event, the server
// will only treat the client as an active participant once it receives
// at least one DataPacket carrying an input frame (FOV + first-move-
// sequence + >=1 move record). This encoder builds that packet.
//
// Wire layout (clean-room §17.1/§17.2/§17.4):
//
//   VC header (§14.2)                  ~4 bytes (varies w/ ack-run count)
//   R0 = 0 (current-rate-changed)      1 bit
//   R1 = 0 (max-rate-changed)          1 bit
//   E  = 0 (event-sub-stream-present)  1 bit
//   P  = 1 (input-sub-stream-present)  1 bit
//     H  = 0 (input header leading bit) 1 bit
//     FOV (wire 0..255)                 8 bits
//     first-move-sequence              32 bits LSB-first
//     for each move record M[0..N):
//       another-move-follows = 1       1 bit
//       if i > 0: axes-redundant bit   1 bit
//       if i == 0 or axes-redundant==0:
//         forward axis  (0..15)        4 bits
//         backward axis (0..15)        4 bits
//         left axis     (0..15)        4 bits
//         right axis    (0..15)        4 bits
//         jet                          1 bit
//         jump-just-pressed            1 bit
//         crouch                       1 bit
//       trigger                        1 bit
//       item-action-present            1 bit
//         optionally item-action       8 bits
//       pitch-present                  1 bit
//         optionally pitch float32     32 bits LE bit-packed
//       turn-present                   1 bit
//         optionally yaw float32       32 bits LE bit-packed
//     another-move-follows = 0         1 bit (loop terminator)
//   G  = 0 (ghost-sub-stream-present)  1 bit
//   pad to byte boundary               0..7 bits
//
// Bit ordering is LSB-first within each byte (matches reliable_acks.*
// and client_events.*). The pitch / yaw floats are written as raw
// little-endian 4-byte memory at the current bit cursor — they are
// NOT byte-aligned, so on a write cursor that is mid-byte the float
// straddles five bytes on the wire.

#pragma once

#include "reliable_acks.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net20 {

// One tick of player input. Axes are non-negative analog half-scales
// in [0.0, 1.0]; mouse deltas are per-tick radians; flags are digital.
struct MoveInput {
    // 4-bit axes — encoded as `int(clamp(axis, 0, 1) * 15)`.
    float forward = 0.0f;
    float backward = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    // Digital flags.
    bool jet = false;
    bool jump = false;          // jump-just-pressed (edge, not held)
    bool crouch = false;
    bool trigger = false;       // primary fire held this tick
    // Optional item-action: 0..255; if `item_action_present` is false the
    // 8-bit byte is omitted entirely (a single 0-bit on the wire).
    bool item_action_present = false;
    std::uint8_t item_action = 0;
    // Mouse-look deltas in radians. The encoder elides the float (sets
    // the `*-present` flag to 0) when the delta is exactly 0.0f.
    float pitch_delta = 0.0f;
    float yaw_delta = 0.0f;
};

// Inputs for one c->s `DataPacket` carrying one or more input ticks.
struct MoveCommandInputs {
    // VC header fields (§14.2). The header is built into the same
    // BitWriter so the rate-control and sub-stream content can flow
    // naturally afterwards.
    std::uint16_t send_seq = 0;          // 0..511; data packets increment
    bool connect_parity = false;          // bit 1 of the VC header
    std::uint8_t highest_acked_of_mine = 0;  // bits 11..15 (mod 32)
    std::vector<AckRun> ack_runs;        // piggy-backed acks (§14.3)

    // §17.2 — FOV in degrees. Clamped to [5.625, 120.0] before
    // quantization. Default 90deg => wire 170.
    float fov_degrees = 90.0f;

    // §17.2 — first move sequence assigned to moves[0]; subsequent
    // moves get +1 each. The Implementer's outer loop maintains the
    // monotonic counter.
    std::uint32_t first_move_seq = 0;

    // Per-tick moves to send. Must be >=1 (the server expects at
    // least one move record per packet, per §17.6's "no zero-move
    // packets observed in capture").
    std::vector<MoveInput> moves;
};

// FOV quantization helper. Public for the self-test to exercise.
//   wire = round(clamp(fov, 5.625, 120.0) / 135.0 * 255)
std::uint8_t quantize_fov(float fov_degrees);

// Encode a c->s `DataPacket` carrying one or more move-command ticks.
// Returns the full wire bytes (VC header + R0/R1 + E + P + input
// sub-stream + G + pad).
std::vector<std::uint8_t> encode_movecommand(const MoveCommandInputs& inputs);

// Worked-example self-test — reproduces the §17.8 capture packet i=400
// from documented inputs. Returns the 23-byte wire bytes, NOT including
// the ghost-sub-stream content (which is out of scope for §17).
//
// The function emits bits 0..177 of the §17.8 packet (VC header + R0/R1
// + E + P + input sub-stream + move-loop terminator), then a 0 bit for
// the ghost-sub-stream-present flag, then zero-pad. The original packet
// has ghost-present=1 at bit 178 followed by ghost data; our self-test
// path therefore differs from the original only in byte 22's low bit
// onward. The self-test verifies bytes 0..21 match exactly.
std::vector<std::uint8_t> encode_movecommand_worked_example();

// Spec 28/02 — decode a c->s DataPacket back into MoveCommandInputs.
// Inverse of encode_movecommand(); used by the server listener to apply
// client input.
//
// Out-params:
//   out         — populated on success.
// Returns true on a fully-consumed valid packet, false on:
//   * VC discriminator (bit 0) != 1
//   * type word != kDataPacket
//   * any bit-read past end-of-buffer
//   * malformed move loop (negative redundant flag on first move, etc.)
//
// Wire layout reference is the comment block at the top of this header.
// The decoder does NOT parse the ghost sub-stream — once it sees the
// move-loop terminator + G bit it considers the input section complete.
//
// Inverse of axis quantization (4 bits -> float): the wire 0..15 maps
// back to float values 0, 1/15, 2/15, ... 1.0 (uniform).
bool decode_movecommand(const std::uint8_t* data, std::size_t size,
                        MoveCommandInputs& out);

}  // namespace net20
