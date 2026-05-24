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

// 10 packets / ~2.2kB total — clean baseline capture from real TAH
// dedicated server. The previous 31-packet capture had session-polluted
// bytes (verified by diff against ground-truth pcap). cap1's bytes
// match ground-truth to within one 12-byte session-keyed insert.
extern const TahBurstPacket kTahFirstGhostBurst[10];
extern const std::size_t    kTahFirstGhostBurstCount;

}  // namespace dts_viewer

#endif  // OSENGINE_TAH_GHOST_BURST_HPP
