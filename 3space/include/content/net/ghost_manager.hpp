#ifndef LIB3SPACE_NET_GHOST_MANAGER_HPP
#define LIB3SPACE_NET_GHOST_MANAGER_HPP

// Track 20 spec 04 — ghost-state delta replication.
//
// Source: docs/clean-room-specs/TRIBES-NETPROTO.md §§5 and 9.
//
// The GhostManager owns a per-connection table of mirrored objects with a
// hard cap of 1024 entries (§5.1) and one reserved sentinel
// (INVALID_GHOST_INDEX = 1023). Each entry tracks scope, dirty bits, and a
// "ghosting in progress" flag.
//
// The wire format (§5.2-5.5) packs:
//   1   mode flag (1 = scope-always, 0 = normal delta)
//   3   width selector — mirror-index width = selector + 3 (so 3..10 bits)
// then for each object in the burst:
//   1   object-present (0 terminates)
//   N   mirror index   (N = selector + 3)
//   1   kill flag
//   [scope-always only]
//     32 object id        (newly-introduced mirrors)
//     10 class tag        (newly-introduced mirrors)
//   *   per-class packUpdate bits  (provided by IGhostObject::pack_update)
// followed (scope-always mode only) by a 1-bit "all done" flag.
//
// We deliberately do NOT implement the per-class field encodings for Player,
// Vehicle, etc. here — those are per-game-class work that lives in the
// engine layer. The framework is class-agnostic: callers provide an
// `IGhostObject` with pack_update / unpack_update methods.

#include "content/net/bit_stream.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace studio::content::net
{

constexpr std::size_t kMaxGhostCount     = 1024;
constexpr std::uint16_t kInvalidGhostIdx = 1023;

enum class GhostFlag : std::uint8_t
{
    Valid       = 1 << 0,
    InScope     = 1 << 1,
    ScopeAlways = 1 << 2,
    Ghosting    = 1 << 4,
    KillGhost   = 1 << 5,
};

// User-supplied per-object packer/unpacker. Real implementations attach
// this to a game class (Player, Vehicle, ...) and write only the bits
// required by `dirty_mask`. The mask is the 32-bit dirty bitfield the
// game maintains via setMaskBits().
struct IGhostObject
{
    virtual ~IGhostObject() = default;

    // Class tag in [0, 1023]. Stable identifier for the class on the wire.
    virtual std::uint16_t class_tag() const = 0;

    // Optional persistent 32-bit object id (sent during scope-always
    // introduction; recovers persistent references on the client).
    virtual std::uint32_t object_id() const { return 0; }

    // Pack the object's state into `s`. `dirty_mask` is the union of
    // currently-dirty bits and `is_initial` is true on the first pack
    // (encourages writing the full state). Implementations are free to
    // ignore the mask and always send everything — the protocol works
    // either way, just less efficiently.
    virtual void pack_update(BitStream& s, std::uint32_t dirty_mask, bool is_initial) = 0;

    // Unpack what pack_update wrote. `is_initial` is true on the very
    // first received update for this ghost.
    virtual void unpack_update(BitStream& s, bool is_initial) = 0;
};

class GhostManager
{
public:
    // Factory: the client side needs to instantiate the right C++ class
    // from a class tag when a new ghost appears. Higher layers register
    // factories at startup.
    using Factory = std::function<std::unique_ptr<IGhostObject>(std::uint16_t class_tag)>;

    GhostManager();

    void set_factory(Factory f) { factory_ = std::move(f); }

    // --- Server side -------------------------------------------------------

    // Bring `obj` into scope. Returns the assigned mirror index. The object
    // pointer is borrowed; caller retains ownership and must call
    // remove_object() before destroying it.
    std::uint16_t add_object(IGhostObject* obj, bool scope_always = false);

    // Schedule destruction. The mirror is held in the table with KillGhost
    // set until the kill packet is acknowledged.
    void remove_object(std::uint16_t mirror_index);

    // Game code calls this when a field of `mirror_index` becomes dirty.
    void set_dirty_bits(std::uint16_t mirror_index, std::uint32_t or_mask);

    // Pack one frame's worth of replication into `s`. Returns the number of
    // objects packed. `mode_scope_always` toggles between the scope-always
    // initial burst and the normal delta stream.
    std::size_t pack_replication(BitStream& s,
                                 std::size_t max_payload_bits,
                                 bool mode_scope_always);

    // Notification from the lower (VC) layer.
    void on_packet_acked(const std::vector<std::uint16_t>& mirror_indices);
    void on_packet_dropped(const std::vector<std::uint16_t>& mirror_indices,
                           const std::vector<std::uint32_t>& bits_in_packet);

    // --- Client side -------------------------------------------------------

    // Decode one replication burst from `s` and call factory()/unpack_update
    // on the ghost entries. Returns true on success.
    bool unpack_replication(BitStream& s);

    // Accessor.
    IGhostObject* mirror_for(std::uint16_t mirror_index) const;
    std::size_t   live_mirror_count() const { return populated_count_; }

private:
    struct Entry
    {
        IGhostObject* obj = nullptr;          // borrowed
        std::unique_ptr<IGhostObject> owned;  // for client-side decoded ghosts
        std::uint8_t  flags = 0;
        std::uint32_t dirty_mask = 0;
        std::uint32_t last_sent_mask = 0;
        std::uint16_t skipped_count = 0;
        bool          intro_pending = true;   // first-update marker
    };

    bool has_flag(const Entry& e, GhostFlag f) const {
        return (e.flags & static_cast<std::uint8_t>(f)) != 0;
    }
    void set_flag(Entry& e, GhostFlag f) {
        e.flags |= static_cast<std::uint8_t>(f);
    }
    void clear_flag(Entry& e, GhostFlag f) {
        e.flags &= static_cast<std::uint8_t>(~static_cast<std::uint8_t>(f));
    }

    std::uint8_t pick_index_width() const;
    std::uint16_t allocate_mirror();

    std::array<Entry, kMaxGhostCount> table_ {};
    std::size_t populated_count_ = 0;
    Factory factory_;
};

} // namespace studio::content::net

#endif // LIB3SPACE_NET_GHOST_MANAGER_HPP
