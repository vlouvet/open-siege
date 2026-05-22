#include "content/net/ghost_manager.hpp"

#include <algorithm>
#include <cassert>

namespace studio::content::net
{

GhostManager::GhostManager() = default;

std::uint16_t GhostManager::allocate_mirror()
{
    // Linear scan for a free slot. 1024 entries — cheap enough.
    for (std::uint16_t i = 0; i < kMaxGhostCount; ++i) {
        if (i == kInvalidGhostIdx) continue;
        if (!has_flag(table_[i], GhostFlag::Valid)) return i;
    }
    return kInvalidGhostIdx;   // table full
}

std::uint8_t GhostManager::pick_index_width() const
{
    // Choose minimum width that addresses the current population. Spec §5.2:
    // 3-bit selector encodes (width - 3), so effective widths are 3..10.
    std::size_t max_idx = 0;
    for (std::uint16_t i = 0; i < kMaxGhostCount; ++i) {
        if (i == kInvalidGhostIdx) continue;
        if (has_flag(table_[i], GhostFlag::Valid)) max_idx = i;
    }
    std::uint8_t width = 3;
    while (width < 10 && (1u << width) <= max_idx) ++width;
    return width;
}

std::uint16_t GhostManager::add_object(IGhostObject* obj, bool scope_always)
{
    if (!obj) return kInvalidGhostIdx;
    const std::uint16_t idx = allocate_mirror();
    if (idx == kInvalidGhostIdx) return idx;
    Entry& e = table_[idx];
    e.obj = obj;
    e.flags = 0;
    set_flag(e, GhostFlag::Valid);
    set_flag(e, GhostFlag::InScope);
    set_flag(e, GhostFlag::Ghosting);
    if (scope_always) set_flag(e, GhostFlag::ScopeAlways);
    e.dirty_mask = 0xFFFFFFFFu;  // all-dirty initially
    e.last_sent_mask = 0;
    e.intro_pending = true;
    ++populated_count_;
    return idx;
}

void GhostManager::remove_object(std::uint16_t mirror_index)
{
    if (mirror_index >= kMaxGhostCount) return;
    Entry& e = table_[mirror_index];
    if (!has_flag(e, GhostFlag::Valid)) return;
    set_flag(e, GhostFlag::KillGhost);
    clear_flag(e, GhostFlag::InScope);
    e.dirty_mask = 0xFFFFFFFFu;
}

void GhostManager::set_dirty_bits(std::uint16_t mirror_index,
                                  std::uint32_t or_mask)
{
    if (mirror_index >= kMaxGhostCount) return;
    table_[mirror_index].dirty_mask |= or_mask;
}

std::size_t GhostManager::pack_replication(BitStream& s,
                                           std::size_t max_payload_bits,
                                           bool mode_scope_always)
{
    const std::size_t start = s.get_cur_pos();
    const std::size_t budget_end = std::min(s.capacity_bits(),
                                            start + max_payload_bits);

    // Wire header: 1-bit mode + 3-bit width selector.
    s.write_flag(mode_scope_always);
    const std::uint8_t width = pick_index_width();
    s.write_int(static_cast<std::uint32_t>(width - 3), 3);

    // Build candidate list. In scope-always mode we only emit entries with
    // ScopeAlways flag; in normal mode we emit anything with dirty bits or
    // a pending kill.
    struct Cand {
        std::uint16_t idx;
        float priority;
    };
    std::vector<Cand> cands;
    cands.reserve(64);

    for (std::uint16_t i = 0; i < kMaxGhostCount; ++i) {
        if (i == kInvalidGhostIdx) continue;
        const Entry& e = table_[i];
        if (!has_flag(e, GhostFlag::Valid)) continue;
        if (mode_scope_always && !has_flag(e, GhostFlag::ScopeAlways)) continue;
        if (!mode_scope_always &&  has_flag(e, GhostFlag::ScopeAlways)
            && !has_flag(e, GhostFlag::Ghosting)
            && !has_flag(e, GhostFlag::KillGhost)
            && e.dirty_mask == 0) continue;
        if (e.dirty_mask == 0
            && !has_flag(e, GhostFlag::Ghosting)
            && !has_flag(e, GhostFlag::KillGhost)) continue;

        Cand c;
        c.idx = i;
        // Simple priority: skipped_count + (kill | ghosting) bonus.
        c.priority = static_cast<float>(e.skipped_count);
        if (has_flag(e, GhostFlag::KillGhost)) c.priority += 1000.0f;
        if (has_flag(e, GhostFlag::Ghosting)) c.priority += 100.0f;
        cands.push_back(c);
    }
    std::sort(cands.begin(), cands.end(),
        [](const Cand& a, const Cand& b) { return a.priority > b.priority; });

    std::size_t packed = 0;
    for (const Cand& c : cands) {
        Entry& e = table_[c.idx];
        const std::size_t obj_start = s.get_cur_pos();

        // Sketch upper bound of bits per object: present(1) + idx(width) +
        // kill(1) + scope-always-only(32+10) + class_tag fallback in scope
        // always for new mirrors + payload (unknown — let pack_update fail
        // out via stream is_full() check after).
        const std::size_t prefix = 1 + width + 1
            + (mode_scope_always && e.intro_pending ? (32 + 10) : 0);
        if (obj_start + prefix + 1 > budget_end) break;   // +1 trailing terminator

        s.write_flag(true);                                       // present
        s.write_int(c.idx, width);
        const bool is_kill = has_flag(e, GhostFlag::KillGhost);
        s.write_flag(is_kill);

        if (mode_scope_always && e.intro_pending) {
            // Object id (32 LE-style bit, MSB-first 32-bit is fine here
            // since this isn't byte-aligned). We use bit-stream's
            // write_int which is MSB-first within bits.
            s.write_int(e.obj->object_id() & 0xFFFFFFFFu, 32);
            s.write_int(e.obj->class_tag() & 0x3FFu, 10);
        }

        if (!is_kill) {
            // pack_update may grow the stream; let it do so. We don't enforce
            // the budget mid-write — if the stream overflows, the trailing
            // is_valid() check rolls everything back.
            e.obj->pack_update(s, e.dirty_mask, e.intro_pending);
            if (!s.is_valid() || s.get_cur_pos() > budget_end) {
                // Rollback this object.
                s.set_cur_pos(obj_start);
                // Mark valid-again so caller can keep going.
                break;
            }
            e.last_sent_mask = e.dirty_mask;
            e.dirty_mask = 0;
            e.intro_pending = false;
        } else {
            // Kill: still expected to be acked. Caller's on_packet_acked
            // releases the entry.
        }

        ++packed;
        clear_flag(e, GhostFlag::Ghosting);
        e.skipped_count = 0;
    }

    // Mark unpacked candidates as skipped.
    if (packed < cands.size()) {
        for (std::size_t i = packed; i < cands.size(); ++i) {
            ++table_[cands[i].idx].skipped_count;
        }
    }

    // Terminator: present=0
    s.write_flag(false);

    // In scope-always mode, follow up with the "all done" bit. We set it
    // when no ScopeAlways entries with Ghosting remain to send.
    if (mode_scope_always) {
        bool any_pending = false;
        for (std::uint16_t i = 0; i < kMaxGhostCount; ++i) {
            if (i == kInvalidGhostIdx) continue;
            const Entry& e = table_[i];
            if (!has_flag(e, GhostFlag::Valid)) continue;
            if (!has_flag(e, GhostFlag::ScopeAlways)) continue;
            if (e.intro_pending || has_flag(e, GhostFlag::Ghosting)) {
                any_pending = true;
                break;
            }
        }
        s.write_flag(!any_pending);   // 1 == all done
    }
    return packed;
}

bool GhostManager::unpack_replication(BitStream& s)
{
    const bool mode_scope_always = s.read_flag();
    const std::uint8_t width = static_cast<std::uint8_t>(s.read_int(3)) + 3;
    if (!s.is_valid()) return false;

    for (;;) {
        if (!s.is_valid()) return false;
        const bool present = s.read_flag();
        if (!present) break;

        const std::uint16_t idx = static_cast<std::uint16_t>(s.read_int(width));
        const bool is_kill = s.read_flag();
        if (idx >= kMaxGhostCount) return false;
        Entry& e = table_[idx];

        if (mode_scope_always && e.obj == nullptr && e.owned == nullptr) {
            // New ghost introduction.
            const std::uint32_t obj_id = s.read_int(32);
            const std::uint16_t class_tag = static_cast<std::uint16_t>(s.read_int(10));
            if (!factory_) return false;
            std::unique_ptr<IGhostObject> created = factory_(class_tag);
            if (!created) return false;
            (void)obj_id;
            e.owned = std::move(created);
            e.obj = e.owned.get();
            set_flag(e, GhostFlag::Valid);
            set_flag(e, GhostFlag::InScope);
            set_flag(e, GhostFlag::ScopeAlways);
            ++populated_count_;
            e.intro_pending = true;
        }

        if (is_kill) {
            // Cascade: drop the entry.
            if (has_flag(e, GhostFlag::Valid)) --populated_count_;
            e.owned.reset();
            e.obj = nullptr;
            e.flags = 0;
            continue;
        }

        if (!e.obj) {
            // Got an update for a slot we never saw scope-in'd. The spec
            // calls for a soft fail — the client refuses the rest of the
            // burst.
            return false;
        }
        e.obj->unpack_update(s, e.intro_pending);
        e.intro_pending = false;
    }

    if (mode_scope_always) {
        const bool all_done = s.read_flag();
        (void)all_done;
    }
    return s.is_valid();
}

void GhostManager::on_packet_acked(const std::vector<std::uint16_t>& mirror_indices)
{
    for (auto idx : mirror_indices) {
        if (idx >= kMaxGhostCount) continue;
        Entry& e = table_[idx];
        clear_flag(e, GhostFlag::Ghosting);
        if (has_flag(e, GhostFlag::KillGhost)) {
            // Kill confirmed: release the slot.
            if (has_flag(e, GhostFlag::Valid)) --populated_count_;
            e.owned.reset();
            e.obj = nullptr;
            e.flags = 0;
            e.dirty_mask = 0;
            e.intro_pending = true;
        }
    }
}

void GhostManager::on_packet_dropped(const std::vector<std::uint16_t>& mirror_indices,
                                     const std::vector<std::uint32_t>& bits_in_packet)
{
    const std::size_t n = std::min(mirror_indices.size(), bits_in_packet.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t idx = mirror_indices[i];
        if (idx >= kMaxGhostCount) continue;
        // OR the dropped bits back into the dirty mask (§5 drop/re-dirty).
        table_[idx].dirty_mask |= bits_in_packet[i];
    }
}

IGhostObject* GhostManager::mirror_for(std::uint16_t mirror_index) const
{
    if (mirror_index >= kMaxGhostCount) return nullptr;
    return table_[mirror_index].obj;
}

} // namespace studio::content::net
