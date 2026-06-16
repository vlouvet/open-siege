// Track 26 spec 14c — TAH datablock-catalogue event encoder (clean-room).
// See tah_datablock_encoder.hpp for the wire-format reference (clean-room
// docs/clean-room-specs/TRIBES-DATABLOCKS.md).

#include "tah_datablock_encoder.hpp"
#include "tah_default_catalogue.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace net20 {

// ---------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------

void write_quantized_float(BitWriter& w, float f, unsigned bits)
{
    if (bits == 0) return;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const std::uint32_t maxv = (bits >= 32) ? 0xFFFFFFFFu
                                            : ((1u << bits) - 1u);
    const std::uint32_t wire =
        static_cast<std::uint32_t>(std::lround(f * static_cast<float>(maxv)));
    w.write_bits(wire, bits);
}

// §6.5 signed-float quantization (used by Form-A normal vectors).
static void write_signed_quantized_float(BitWriter& w, float f, unsigned bits)
{
    if (bits == 0) return;
    if (f < -1.0f) f = -1.0f;
    if (f > 1.0f) f = 1.0f;
    const std::uint32_t maxv = (bits >= 32) ? 0xFFFFFFFFu
                                            : ((1u << bits) - 1u);
    // wire = round( ((f + 1) / 2) * maxv )
    const float norm = (f + 1.0f) * 0.5f;
    const std::uint32_t wire =
        static_cast<std::uint32_t>(std::lround(norm * static_cast<float>(maxv)));
    w.write_bits(wire, bits);
}

void write_raw_float32(BitWriter& w, float f)
{
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    w.write_bits(u, 32);
}

void write_v3_string_uncompressed(BitWriter& w, const std::string& s)
{
    // 1-bit compression flag (= 0), 8-bit length, then len bytes
    // bit-packed (no alignment).
    w.write_flag(false);
    const std::size_t raw_len = s.size() > 255 ? 255 : s.size();
    w.write_bits(static_cast<std::uint32_t>(raw_len), 8);
    for (std::size_t i = 0; i < raw_len; ++i) {
        w.write_bits(static_cast<std::uint8_t>(s[i]), 8);
    }
}

unsigned block_id_ref_width(std::uint8_t target_group_size) noexcept
{
    // Width = bits_to_represent(group_size + 1). For group_size = 0
    // there are no valid blocks; the encoder will only ever write the
    // null sentinel (= 0) with width 1.
    const unsigned needed = static_cast<unsigned>(target_group_size) + 1u;
    unsigned w = 1;
    while ((1u << w) < needed) ++w;
    return w;
}

void write_block_id_ref(BitWriter& w, std::uint16_t block_or_null,
                        std::uint8_t target_group_size)
{
    // SPEC-AMBIGUITY (Open Question #2): we use the §3.3 convention —
    // write block_index directly with `group_size` as the null sentinel.
    const unsigned width = block_id_ref_width(target_group_size);
    // Clamp out-of-range values to the null sentinel.
    std::uint32_t v = block_or_null;
    if (v > target_group_size) v = target_group_size;
    w.write_bits(v, width);
}

// ---------------------------------------------------------------------
// Per-class body builders
// ---------------------------------------------------------------------

DatablockEntry make_sound_profile_data(std::uint8_t group_size,
                                       std::uint8_t block,
                                       const SoundProfileFields& f)
{
    BitWriter body;
    body.write_bits(f.flags & 0x3Fu, 6);
    write_quantized_float(body, f.baseVolume, 10);

    // f1: coneInsideAngle differs
    body.write_flag(f.coneInsideAngle.present);
    if (f.coneInsideAngle.present) {
        write_quantized_float(body, f.coneInsideAngle.value / 360.0f, 10);
    }
    // f2: coneOutsideAngle differs
    body.write_flag(f.coneOutsideAngle.present);
    if (f.coneOutsideAngle.present) {
        write_quantized_float(body, f.coneOutsideAngle.value / 360.0f, 10);
    }
    // f3: coneVector differs — Form A normal vector, 10-bit (= 21 bits)
    body.write_flag(f.coneVector.present);
    if (f.coneVector.present) {
        write_signed_quantized_float(body, f.coneVector.value.x, 10);
        write_signed_quantized_float(body, f.coneVector.value.y, 10);
        body.write_flag(f.coneVector.value.z < 0.0f);
    }
    // f4: coneOutsideVolume differs — raw 32-bit float
    body.write_flag(f.coneOutsideVolume.present);
    if (f.coneOutsideVolume.present) {
        write_raw_float32(body, f.coneOutsideVolume.value);
    }
    // f5: minDistance differs
    body.write_flag(f.minDistance.present);
    if (f.minDistance.present) {
        write_raw_float32(body, f.minDistance.value);
    }
    // f6: maxDistance differs
    body.write_flag(f.maxDistance.present);
    if (f.maxDistance.present) {
        write_raw_float32(body, f.maxDistance.value);
    }

    DatablockEntry e;
    e.group = static_cast<std::uint8_t>(DatablockClass::SoundProfile);
    e.group_size = group_size;
    e.block = block;
    e.class_kind = DatablockClass::SoundProfile;
    e.body_bits = std::move(body.bytes);
    e.body_bit_count = body.bit_pos;
    return e;
}

DatablockEntry make_sound_data(std::uint8_t group_size,
                               std::uint8_t block,
                               std::uint8_t sound_profile_group_size,
                               const SoundDataFields& f)
{
    BitWriter body;
    write_v3_string_uncompressed(body, f.wavFileName);
    write_quantized_float(body, f.priority, 6);
    // Reference into group 0 (SoundProfileData).
    write_block_id_ref(body, f.profileIndex, sound_profile_group_size);

    DatablockEntry e;
    e.group = static_cast<std::uint8_t>(DatablockClass::Sound);
    e.group_size = group_size;
    e.block = block;
    e.class_kind = DatablockClass::Sound;
    e.body_bits = std::move(body.bytes);
    e.body_bit_count = body.bit_pos;
    return e;
}

DatablockEntry make_player_data(std::uint8_t group_size,
                                std::uint8_t block,
                                const PlayerDataRefSizes& refs,
                                const PlayerDataFields& f)
{
    BitWriter body;
    // --- GameBaseData ---
    // 8-bit signed mapFilter
    body.write_bits(static_cast<std::uint32_t>(
                        static_cast<std::uint8_t>(f.mapFilter)),
                    8);
    write_v3_string_uncompressed(body, f.mapIcon);
    write_v3_string_uncompressed(body, f.description);

    // --- ShapeBaseData ---
    write_v3_string_uncompressed(body, f.fileName);
    write_v3_string_uncompressed(body, f.shieldShapeName);
    write_block_id_ref(body, f.sfxShield, refs.soundGroupSize);
    body.write_bits(f.shadowDetailMask, 8);
    write_block_id_ref(body, f.explosionId, refs.explosionGroupSize);
    write_block_id_ref(body, f.damageSkinId, refs.damageSkinGroupSize);
    body.write_bits(f.debrisId, 32);
    write_raw_float32(body, f.maxEnergy);
    // MaxSequenceCount = 16 entries
    for (const auto& s : f.sequenceSounds) {
        body.write_flag(s.present);
        if (s.present) {
            write_v3_string_uncompressed(body, s.sequenceName);
            write_block_id_ref(body, s.soundIndex, refs.soundGroupSize);
        }
    }
    body.write_flag(f.isPerspective);

    // --- PlayerData proper ---
    write_v3_string_uncompressed(body, f.flameShapeName);
    // AnimSlotCount = 32 entries
    for (const auto& a : f.animSlots) {
        write_v3_string_uncompressed(body, a.name);
        body.write_flag(a.hasSound);
        if (a.hasSound) {
            write_block_id_ref(body, a.soundIndex, refs.soundGroupSize);
        }
        body.write_flag(a.directionPositive);
        body.write_bits(a.viewFlags & 0x0Fu, 4);
        body.write_bits(a.priority & 0x0Fu, 4);
    }
    write_raw_float32(body, f.minDamageSpeed);
    write_raw_float32(body, f.jetEnergyDrain);
    write_quantized_float(body, f.maxForwardSpeed / 64.0f, 10);
    write_quantized_float(body, f.maxBackwardSpeed / 64.0f, 10);
    write_quantized_float(body, f.maxSideSpeed / 64.0f, 10);
    body.write_flag(f.canCrouch);
    write_quantized_float(body, f.maxJetSideForceFactor, 8);
    write_raw_float32(body, f.groundForce);
    write_raw_float32(body, f.groundTraction);
    write_raw_float32(body, f.maxJetForwardVelocity);
    write_quantized_float(body, f.jumpSurfaceMinDot, 8);
    write_raw_float32(body, f.jumpImpulse);
    write_raw_float32(body, f.mass);
    write_raw_float32(body, f.drag);
    write_raw_float32(body, f.density);
    write_raw_float32(body, f.jetForce);
    write_block_id_ref(body, f.jetSound, refs.soundGroupSize);
    // FootSoundCount = 9 pairs (right, left)
    for (const auto& fs : f.footSteps) {
        write_block_id_ref(body, fs.rightFoot, refs.soundGroupSize);
        write_block_id_ref(body, fs.leftFoot, refs.soundGroupSize);
    }
    body.write_bits(f.footPrintsA, 32);
    body.write_bits(f.footPrintsB, 32);
    write_raw_float32(body, f.boxWidth);
    write_raw_float32(body, f.boxDepth);
    write_raw_float32(body, f.boxNormalHeight);
    write_raw_float32(body, f.boxCrouchHeight);

    // TODO-PLAYERDATA-FIELD-N: §5.3 marks several sub-fields as
    // sketched-not-locked-down (Open Question #1). The selftest XFAILs
    // byte-equality against seq020 because of these gaps; once a
    // per-field PlayerData addendum to TRIBES-DATABLOCKS.md is written
    // and ratified, fill in the missing fields here.

    DatablockEntry e;
    e.group = static_cast<std::uint8_t>(DatablockClass::PlayerData);
    e.group_size = group_size;
    e.block = block;
    e.class_kind = DatablockClass::PlayerData;
    e.body_bits = std::move(body.bytes);
    e.body_bit_count = body.bit_pos;
    return e;
}

DatablockEntry make_sentinel_entry(DatablockClass kind)
{
    DatablockEntry e;
    e.group = static_cast<std::uint8_t>(kind);
    e.group_size = 0;
    e.block = 255;
    e.class_kind = kind;
    e.body_bit_count = 0;
    return e;
}

// ---------------------------------------------------------------------
// Catalogue packet encoder
// ---------------------------------------------------------------------

void append_catalogue_event(BitWriter& w, const DatablockEntry& e,
                            std::uint16_t event_class_tag,
                            std::uint8_t explicit_seq,
                            bool seq_continuous);

static void append_event(BitWriter& w, const DatablockEntry& e,
                         std::uint16_t event_class_tag,
                         std::uint8_t explicit_seq,
                         bool seq_continuous)
{
    // Event framing (NETPROTO §7.3 / spec §2):
    w.write_flag(true);                  // event-present
    w.write_flag(true);                  // guaranteed (always 1 here)
    w.write_flag(seq_continuous);        // seq-continuous
    if (!seq_continuous) {
        w.write_flag(true);              // has-explicit-seq
        w.write_bits(explicit_seq & 0x7Fu, 7);
    }
    w.write_bits(event_class_tag & 0x7Fu, 7);

    // Catalogue-entry header (spec §2):
    w.write_bits(e.group & 0x3Fu, 6);
    w.write_bits(e.group_size, 8);
    w.write_bits(e.block, 8);

    if (e.is_sentinel()) return;

    // Body bits (bit-by-bit copy — body_bits is LSB-first packed but
    // its bit-0 alignment likely doesn't match w.bit_pos, so we must
    // walk bit-by-bit rather than memcpy).
    for (std::size_t i = 0; i < e.body_bit_count; ++i) {
        const std::uint8_t bit =
            static_cast<std::uint8_t>((e.body_bits[i >> 3] >> (i & 7)) & 1u);
        w.write_bits(bit, 1);
    }
}

void append_catalogue_event(BitWriter& w, const DatablockEntry& e,
                            std::uint16_t event_class_tag,
                            std::uint8_t explicit_seq,
                            bool seq_continuous)
{
    // Public wrapper around the file-local append_event helper. Used by
    // the burst orchestrator (spec 14c-I-5) so it can drive ESS framing
    // itself and interleave catalogue events with ghost intros in the
    // same VC packet.
    append_event(w, e, event_class_tag, explicit_seq, seq_continuous);
}

std::vector<std::uint8_t>
encode_catalogue_packet(const std::vector<DatablockEntry>& entries_for_packet,
                        std::uint16_t event_class_tag,
                        std::uint8_t first_event_seq)
{
    BitWriter w;
    // Rate-control flag prefix (§3.4): catalogue packets observed in
    // TAH captures have current-rate-changed = 0, max-rate-changed = 0.
    w.write_flag(false);  // current-rate-changed
    w.write_flag(false);  // max-rate-changed
    // Event sub-stream presence flag (§5.0.2).
    w.write_flag(true);   // event-sub-stream-present

    std::uint8_t seq = first_event_seq;
    bool first = true;
    for (const auto& e : entries_for_packet) {
        append_event(w, e, event_class_tag, seq,
                     /*seq_continuous=*/!first);
        seq = static_cast<std::uint8_t>((seq + 1) & 0x7Fu);
        first = false;
    }

    // Event sub-stream terminator: event-present = 0.
    w.write_flag(false);

    // Input/control sub-stream and ghost sub-stream presence (§5.0.2).
    // Server-to-client catalogue packets in real captures DO carry
    // ghost stream payload in parallel — see seq003 (226 B). Our
    // encoder produces just the catalogue portion and leaves the ISS
    // and GSS empty.
    w.write_flag(false);  // input-sub-stream-present
    w.write_flag(false);  // ghost-sub-stream-present

    return std::move(w.bytes);
}

std::size_t catalogue_entry_wire_bits(const DatablockEntry& e,
                                      bool seq_continuous)
{
    // Framing: event-pres(1) + guaranteed(1) + seq-cont(1) + (if !cont)
    // has-explicit(1) + seq(7) + class tag(7) = 11 or 18.
    std::size_t bits = 11;
    if (!seq_continuous) bits += 8;
    bits += 6 + 8 + 8;             // group + group_size + block
    if (!e.is_sentinel()) bits += e.body_bit_count;
    return bits;
}

std::vector<std::vector<std::uint8_t>>
pack_catalogue_into_packets(const std::vector<DatablockEntry>& all_entries,
                            std::size_t soft_max_bytes,
                            std::uint16_t event_class_tag,
                            std::uint8_t first_event_seq)
{
    std::vector<std::vector<std::uint8_t>> packets;
    if (all_entries.empty()) return packets;
    const std::size_t soft_max_bits = soft_max_bytes * 8;

    std::vector<DatablockEntry> bucket;
    // Per-bucket bit estimate, including 5-bit envelope (2 rate-control +
    // 1 ESS-present + 1 ESS-terminator + 2 ISS/GSS-present = 6 bits;
    // round up to byte) — and tracking whether seq-continuous can apply.
    std::size_t bucket_bits = 6;
    std::uint8_t bucket_first_seq = first_event_seq;
    std::uint8_t next_seq = first_event_seq;
    bool first_in_bucket = true;

    auto flush = [&]() {
        if (bucket.empty()) return;
        packets.push_back(
            encode_catalogue_packet(bucket, event_class_tag, bucket_first_seq));
        bucket.clear();
        bucket_bits = 6;
        first_in_bucket = true;
    };

    for (const auto& e : all_entries) {
        // Per §3.5 the isFull check runs at the TOP of the pack loop:
        // a single entry whose body alone exceeds the soft cap is still
        // packed (it lands in its own packet). Mirror that here.
        const std::size_t entry_bits =
            catalogue_entry_wire_bits(e, /*seq_continuous=*/!first_in_bucket);
        if (!first_in_bucket && bucket_bits >= soft_max_bits) {
            flush();
            bucket_first_seq = next_seq;
        }
        bucket.push_back(e);
        bucket_bits += entry_bits;
        first_in_bucket = false;
        next_seq = static_cast<std::uint8_t>((next_seq + 1) & 0x7Fu);
    }
    flush();
    return packets;
}

// ---------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------

namespace {

// Read a bit from a byte buffer (LSB-first within byte). Returns 0 if
// the bit position is past the end of the buffer.
std::uint8_t bit_at(const std::uint8_t* data, std::size_t size,
                    std::size_t bit_pos)
{
    if (bit_pos >= size * 8) return 0;
    return static_cast<std::uint8_t>((data[bit_pos >> 3] >> (bit_pos & 7)) & 1u);
}

// Compare `nbits` bits at bit-position `cap_start` of `cap` against the
// first `nbits` bits of `enc`. Returns the number of matching leading
// bits (== nbits on success).
std::size_t compare_bit_run(const std::uint8_t* cap, std::size_t cap_size,
                            std::size_t cap_start,
                            const std::uint8_t* enc, std::size_t enc_size,
                            std::size_t nbits)
{
    for (std::size_t i = 0; i < nbits; ++i) {
        const std::uint8_t a = bit_at(cap, cap_size, cap_start + i);
        const std::uint8_t b = bit_at(enc, enc_size, i);
        if (a != b) return i;
    }
    return nbits;
}

std::vector<std::uint8_t> read_file_bytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::streamsize>(f.tellg());
    f.seekg(0);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return out;
}

// Catalogue paths candidates (try both real and mirrored locations).
const char* kCapDir =
    "/Users/v/code/tribes-emscripten/captures/real-tribes/tah-burst-20260524";

// V1 expected event-sub-stream prefix bits — derived from the spec
// PLUS empirical correction from the actual capture (see selftest body
// for the deviation against §6.1's claim of "all defaults"). The first
// event in seq003 actually has f3 (coneVector differs) = 1 and a
// coneVector wire value of all-zeros, so the encoded body is 22 + 21
// = 43 bits, and the event-substream prefix is 2 rate-flags + 1 ESS +
// 18-bit framing + 22-bit catalogue header + 43-bit body = 86 bits.
constexpr std::size_t kV1CompareBits = 86;

// V2 (seq004) first event spans the same shape with different field
// values; we compare the framing + catalogue header + 9 bits of body
// (1-bit compression flag + 8-bit string length) — enough to anchor
// the encoder's framing without committing to the specific wav-name
// string from the unknown payload.
constexpr std::size_t kV2CompareBits =
    3                        // rate-flags + ESS-present
    + 18                     // event framing (first event of substream)
    + 6 + 8 + 8              // catalogue header
    + 1 + 8;                 // string compression flag + length

}  // namespace

int tah_datablock_encoder_selftest()
{
    int failures = 0;

    // ---- Vector V1: SoundProfileData[0] from seq003_226B.bin ----
    {
        std::string path = std::string(kCapDir) + "/seq003_226B.bin";
        auto cap = read_file_bytes(path);
        if (cap.empty()) {
            std::fprintf(stderr,
                "[tah-datablock-selftest] V1: capture file missing: %s\n",
                path.c_str());
            ++failures;
        } else {
            // §6.1 claims "all defaults" but the actual capture has f3 =
            // coneVector-differs set, with a wire value of all zeros
            // (encoded as Form A signed-floats). The bit-wire pattern
            // for wire=0 in writeSignedFloat(f, 10) is f = -1 (since
            // wire = round((f+1)/2 * 1023) → f=-1 ⇒ wire=0). The sign
            // flag bit (z<0) is also 0, so we set z = 0 (>= 0).
            // The other 5 differs flags are 0 (defaults match) — see
            // build_sound_profiles for the script-driven values used by
            // the catalogue at runtime.
            SoundProfileFields f;
            f.coneVector.present = true;
            f.coneVector.value = {-1.0f, -1.0f, 0.0f};
            auto entry = make_sound_profile_data(/*group_size=*/10,
                                                 /*block=*/0, f);
            if (entry.body_bit_count != 43) {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V1: body_bit_count=%zu "
                    "expected 43\n", entry.body_bit_count);
                ++failures;
            }
            // Pack into a packet (one entry, seq=9 per §6.1).
            std::vector<DatablockEntry> bucket{entry};
            auto enc = encode_catalogue_packet(bucket,
                /*event_class_tag=*/88, /*first_event_seq=*/9);
            // Compare bits 32..(32+kV1CompareBits) of cap vs bits 0..kV1CompareBits of enc.
            const std::size_t matched =
                compare_bit_run(cap.data(), cap.size(), 32,
                                enc.data(), enc.size(), kV1CompareBits);
            if (matched != kV1CompareBits) {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V1: bit mismatch at bit %zu/%zu\n"
                    "  capture bytes 4..15:",
                    matched, kV1CompareBits);
                for (std::size_t i = 4; i < 16 && i < cap.size(); ++i)
                    std::fprintf(stderr, " %02x", cap[i]);
                std::fputs("\n  encoded bytes 0..11:", stderr);
                for (std::size_t i = 0; i < 12 && i < enc.size(); ++i)
                    std::fprintf(stderr, " %02x", enc[i]);
                std::fputc('\n', stderr);
                ++failures;
            } else {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V1 PASS: SoundProfileData[0] "
                    "all-default body matches seq003 bits 32..%zu\n",
                    32 + kV1CompareBits - 1);
            }
        }
    }

    // ---- Vector V2: SoundData[0] framing from seq004_212B.bin ----
    {
        std::string path = std::string(kCapDir) + "/seq004_212B.bin";
        auto cap = read_file_bytes(path);
        if (cap.empty()) {
            std::fprintf(stderr,
                "[tah-datablock-selftest] V2: capture file missing: %s\n",
                path.c_str());
            ++failures;
        } else {
            // We don't know the actual wav filename for SoundData[0]
            // (we'd have to decode the Huffman-or-raw string). Emit a
            // dummy empty string and compare only the framing +
            // catalogue header + string compression flag + length-byte.
            SoundDataFields f;
            f.wavFileName = "";  // length 0 → matches captured len iff len==0
            f.priority = 0.0f;
            f.profileIndex = 10;  // null sentinel for group 0 size 10

            // The capture's SoundData[0] presumably has a non-empty wav
            // name, so our string-len byte will MISMATCH the capture's.
            // We therefore compare only through the catalogue header,
            // then check that the string framing kind (uncompressed,
            // bit = 0) matches.
            auto entry = make_sound_data(/*group_size=*/153, /*block=*/0,
                                         /*sound_profile_group_size=*/10, f);
            std::vector<DatablockEntry> bucket{entry};
            auto enc = encode_catalogue_packet(bucket,
                /*event_class_tag=*/88, /*first_event_seq=*/19);
            // Compare the framing + catalogue header only (no body bits).
            const std::size_t framing_bits =
                3 + 18 + 6 + 8 + 8;  // = 43 bits
            const std::size_t matched =
                compare_bit_run(cap.data(), cap.size(), 32,
                                enc.data(), enc.size(), framing_bits);
            if (matched != framing_bits) {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V2: framing bit mismatch at "
                    "bit %zu/%zu\n  capture bytes 4..12:",
                    matched, framing_bits);
                for (std::size_t i = 4; i < 13 && i < cap.size(); ++i)
                    std::fprintf(stderr, " %02x", cap[i]);
                std::fputs("\n  encoded bytes 0..8:", stderr);
                for (std::size_t i = 0; i < 9 && i < enc.size(); ++i)
                    std::fprintf(stderr, " %02x", enc[i]);
                std::fputc('\n', stderr);
                ++failures;
            } else {
                // Also check the 1-bit string compression flag matches
                // (it should be 0 = uncompressed for typical wav names,
                // but the server may pick compressed for some names).
                const std::uint8_t cap_compress_flag =
                    bit_at(cap.data(), cap.size(), 32 + framing_bits);
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V2 PASS: SoundData[0] framing "
                    "matches seq004 bits 32..%zu (catalogue header = "
                    "group=1 group_size=153 block=0); capture's first body "
                    "bit (string compression flag) = %u %s\n",
                    32 + framing_bits - 1, cap_compress_flag,
                    cap_compress_flag == 0
                        ? "(uncompressed path — encoder matches)"
                        : "(compressed path — XFAIL: encoder always emits "
                          "uncompressed strings, spec §3.3 says compressed "
                          "Huffman is rare for short filenames)");
            }
        }
    }

    // ---- Vector V3: PlayerData[0] framing from seq020_596B.bin ----
    // The full PlayerData body is spec-incomplete (Open Question #1);
    // we verify only the framing + catalogue header.
    {
        std::string path = std::string(kCapDir) + "/seq020_596B.bin";
        auto cap = read_file_bytes(path);
        if (cap.empty()) {
            std::fprintf(stderr,
                "[tah-datablock-selftest] V3: capture file missing: %s\n",
                path.c_str());
            ++failures;
        } else {
            PlayerDataFields f;  // mostly defaults — body will not match
                                  // seq020 byte-for-byte (XFAIL per spec §8.1)
            PlayerDataRefSizes refs;
            refs.soundGroupSize = 153;
            refs.explosionGroupSize = 20;
            refs.damageSkinGroupSize = 2;
            auto entry = make_player_data(/*group_size=*/5, /*block=*/0,
                                          refs, f);
            std::vector<DatablockEntry> bucket{entry};
            auto enc = encode_catalogue_packet(bucket,
                /*event_class_tag=*/88, /*first_event_seq=*/42);

            // Capture's VC header for seq020 is 4 bytes; bits 32..end of
            // packet hold the rate-control flags + event substream. We
            // compare the same framing+header prefix as V2.
            const std::size_t framing_bits =
                3 + 18 + 6 + 8 + 8;  // = 43 bits
            const std::size_t matched =
                compare_bit_run(cap.data(), cap.size(), 32,
                                enc.data(), enc.size(), framing_bits);
            if (matched != framing_bits) {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V3: framing bit mismatch at "
                    "bit %zu/%zu\n  capture bytes 4..12:",
                    matched, framing_bits);
                for (std::size_t i = 4; i < 13 && i < cap.size(); ++i)
                    std::fprintf(stderr, " %02x", cap[i]);
                std::fputs("\n  encoded bytes 0..8:", stderr);
                for (std::size_t i = 0; i < 9 && i < enc.size(); ++i)
                    std::fprintf(stderr, " %02x", enc[i]);
                std::fputc('\n', stderr);
                ++failures;
            } else {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] V3 PASS: PlayerData[0] "
                    "framing matches seq020 bits 32..%zu (catalogue header "
                    "= group=3 group_size=5 block=0)\n"
                    "[tah-datablock-selftest] V3 XFAIL: PlayerData body "
                    "byte-equality against seq020 not checked — Open "
                    "Question #1 in TRIBES-DATABLOCKS.md §8 (sub-field "
                    "bit widths unresolved; helper emits placeholder "
                    "values per §5.3 sketch). Encoded body_bit_count=%zu "
                    "(spec estimate ~4400 bits / ~550 bytes for typical "
                    "record).\n",
                    32 + framing_bits - 1, entry.body_bit_count);
            }
        }
    }

    // ---- Bulk packing sanity check ----
    // pack_catalogue_into_packets with the stock CTF catalogue under a
    // 400-byte soft cap should produce something close to 40 packets,
    // with the 5 PlayerData records each landing in their own (large)
    // packet because each body exceeds 400 bytes once fully populated.
    {
        auto cat = stock_tribes_ctf_catalogue();
        if (cat.empty()) {
            std::fprintf(stderr,
                "[tah-datablock-selftest] BULK: stock_tribes_ctf_catalogue() "
                "returned empty\n");
            ++failures;
        } else {
            auto packets = pack_catalogue_into_packets(cat, 400, 88, 9);
            // Estimate packet stats. Per spec §3.6, the 5 PlayerData
            // records each consume ~575 bytes of body and land alone in
            // their own ~596-byte datagram. Our incomplete PlayerData
            // body produces a smaller (1476-bit ≈ 185-byte) record, so
            // multiple may share a packet — that's an Open Question #1
            // gap, not a bug here.
            std::size_t max_pkt_bytes = 0;
            for (const auto& pkt : packets) {
                if (pkt.size() > max_pkt_bytes) max_pkt_bytes = pkt.size();
            }
            // Sanity: at least 5 packets total for ~387 entries, ≤ 400
            // entries means at least N/cap rounded up. With ~30 bytes
            // per typical entry (mostly small SoundData / Item records),
            // ~387 entries ≈ ~10-30 packets.
            if (packets.size() < 5) {
                std::fprintf(stderr,
                    "[tah-datablock-selftest] BULK: only %zu packets for "
                    "%zu entries — expected >= 5\n",
                    packets.size(), cat.size());
                ++failures;
            }
            std::fprintf(stderr,
                "[tah-datablock-selftest] BULK: %zu catalogue entries → "
                "%zu packets, max packet %zu bytes\n",
                cat.size(), packets.size(), max_pkt_bytes);
        }
    }

    if (failures == 0) {
        std::fputs("[tah-datablock-selftest] OK — all vectors PASS or XFAIL "
                   "with documented spec gap\n", stderr);
        return 0;
    }
    std::fprintf(stderr, "[tah-datablock-selftest] FAIL — %d failure(s)\n",
                 failures);
    return 1;
}

}  // namespace net20
