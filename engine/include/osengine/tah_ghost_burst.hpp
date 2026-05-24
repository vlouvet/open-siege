#ifndef OSENGINE_TAH_GHOST_BURST_HPP
#define OSENGINE_TAH_GHOST_BURST_HPP

// 26/10b follow-up — captured TAH initial-ghost-burst packets, sent
// to TAH (Groove/TribesNext) sessions in lieu of the vanilla 223-byte
// canned burst (spec 26/11). See tah_ghost_burst.cpp for capture
// provenance.

#include <cstddef>
#include <cstdint>

namespace dts_viewer
{

struct TahBurstPacket {
    const std::uint8_t* data;
    std::size_t         size;
};

// 31 packets / 8809B total — captured initial-state dump from the
// real TAH dedicated server with proper ack-runs (see tah_ghost_burst.cpp).
extern const TahBurstPacket kTahFirstGhostBurst[31];
extern const std::size_t    kTahFirstGhostBurstCount;

}  // namespace dts_viewer

#endif  // OSENGINE_TAH_GHOST_BURST_HPP
