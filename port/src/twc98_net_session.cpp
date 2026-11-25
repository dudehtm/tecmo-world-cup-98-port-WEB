#include "twc98_net_session.hpp"

#include <algorithm>
#include <cstring>

namespace twc98 {
namespace {

inline void write_le16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void write_le32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint16_t read_le16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t read_le32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace

void pack_net_input_packet(const NetInputPacket& pkt, uint8_t dst[NetInputPacket::kWireBytes]) {
    write_le16(dst + 0u, pkt.magic);
    dst[2] = pkt.version;
    dst[3] = pkt.pkt_type;
    dst[4] = pkt.sender_player;
    dst[5] = pkt.mode_flags;
    write_le16(dst + 6u, pkt.history_mask);
    write_le32(dst + 8u, pkt.head_frame);
    write_le32(dst + 12u, pkt.ack_remote_frame);
    std::memcpy(dst + 16u, pkt.buttons, NetInputPacket::kWindowFrames);
    std::memcpy(dst + 28u, pkt.sys_c, NetInputPacket::kWindowFrames);
    write_le32(dst + 40u, pkt.sync_frame);
    write_le32(dst + 44u, pkt.sync_hash_lo);
}

bool unpack_net_input_packet(const uint8_t src[NetInputPacket::kWireBytes], size_t len, NetInputPacket& out) {
    if (src == nullptr || len < NetInputPacket::kWireBytes) return false;
    out.magic = read_le16(src + 0u);
    out.version = src[2];
    out.pkt_type = src[3];
    out.sender_player = src[4];
    out.mode_flags = src[5];
    out.history_mask = read_le16(src + 6u);
    out.head_frame = read_le32(src + 8u);
    out.ack_remote_frame = read_le32(src + 12u);
    std::memcpy(out.buttons, src + 16u, NetInputPacket::kWindowFrames);
    std::memcpy(out.sys_c, src + 28u, NetInputPacket::kWindowFrames);
    out.sync_frame = read_le32(src + 40u);
    out.sync_hash_lo = read_le32(src + 44u);
    return (out.magic == NetInputPacket::kMagic) && (out.version == NetInputPacket::kVersion);
}

uint8_t NetSession::recommend_input_delay_for_rtt_ms(uint32_t rtt_ms) {
    // One-way latency = rtt_ms / 2. Keep 2..4 frames of base input delay so micro-rollback stays <= 3f.
    if (rtt_ms <= 70u) return 2u;
    if (rtt_ms <= 165u) return 3u;
    if (rtt_ms <= 240u) return 4u;
    return 5u;
}

uint8_t NetSession::compose_mode_flags() const {
    return static_cast<uint8_t>((copa_mode_ ? 1u : 0u) |
                                (gk_ai_fix_ ? 2u : 0u) |
                                (coop_mode_ ? 4u : 0u) |
                                ((input_delay_ & 0x0Fu) << 4));
}

void NetSession::reset(uint8_t local_player, uint8_t input_delay_frames, bool copa_mode, bool gk_ai_fix,
                       bool coop_mode) {
    local_player_ = (local_player != 0u) ? 1u : 0u;
    input_delay_ = std::clamp<uint8_t>(input_delay_frames, 1u, 6u);
    copa_mode_ = copa_mode;
    gk_ai_fix_ = gk_ai_fix;
    coop_mode_ = coop_mode;

    cur_frame_ = 0u;
    local_head_frame_ = 0u;
    confirmed_remote_frame_ = static_cast<uint32_t>(input_delay_ - 1u);
    earliest_dirty_frame_ = 0xFFFFFFFFu;

    std::memset(local_buttons_, 0xFF, sizeof(local_buttons_));
    std::memset(local_sys_c_, 0xFF, sizeof(local_sys_c_));
    std::memset(local_valid_, 0, sizeof(local_valid_));

    std::memset(remote_buttons_, 0xFF, sizeof(remote_buttons_));
    std::memset(remote_sys_c_, 0xFF, sizeof(remote_sys_c_));
    std::memset(remote_received_, 0, sizeof(remote_received_));

    std::memset(used_remote_buttons_, 0xFF, sizeof(used_remote_buttons_));
    std::memset(used_remote_sys_c_, 0xFF, sizeof(used_remote_sys_c_));

    for (uint32_t f = 0u; f < input_delay_; ++f) {
        local_valid_[f] = true;
        remote_received_[f] = true;
    }

    for (uint32_t s = 0u; s < kRollbackSlots; ++s) {
        slot_frame_[s] = 0xFFFFFFFFu;
        slot_len_[s] = 0u;
    }
    arena_primed_ = false;

    for (uint32_t i = 0u; i < kHistoryCap; ++i) {
        local_sync_frame_[i] = 0xFFFFFFFFu;
        local_sync_hash_[i] = 0u;
        remote_sync_frame_[i] = 0xFFFFFFFFu;
        remote_sync_hash_[i] = 0u;
    }
    last_checked_sync_frame_ = 0u;

    peer_lead_frames_ = 0;
    recommended_usec_delta_ = 0;

    rollback_count_ = 0u;
    rollback_frames_total_ = 0u;
    max_rollback_depth_ = 0u;
    stall_frames_ = 0u;
    packets_sent_ = 0u;
    packets_received_ = 0u;
    sync_checks_passed_ = 0u;
    desync_count_ = 0u;
    first_desync_frame_ = 0u;
    first_desync_local_hash_ = 0u;
    first_desync_remote_hash_ = 0u;
    mode_mismatch_ = false;
}

void NetSession::queue_local_input(uint8_t local_buttons, uint8_t local_sys_c, NetInputPacket& out_pkt) {
    const uint32_t target_f = cur_frame_ + static_cast<uint32_t>(input_delay_);
    const uint32_t idx = target_f % kHistoryCap;
    // Never overwrite an already-committed local frame input if step_frame stalled at cur_frame_:
    // the first packet for target_f was already sent and locked in by the remote peer.
    if (!local_valid_[idx]) {
        local_buttons_[idx] = local_buttons;
        local_sys_c_[idx] = local_sys_c;
        local_valid_[idx] = true;
    }
    local_head_frame_ = target_f;

    // Clear ahead slot in ring buffer to keep history clean as window slides
    const uint32_t clear_f = target_f + 32u;
    const uint32_t cidx = clear_f % kHistoryCap;
    local_valid_[cidx] = false;
    remote_received_[cidx] = false;

    out_pkt = NetInputPacket{};
    out_pkt.magic = NetInputPacket::kMagic;
    out_pkt.version = NetInputPacket::kVersion;
    out_pkt.pkt_type = NetInputPacket::kTypeInputWindow;
    out_pkt.sender_player = local_player_;
    out_pkt.mode_flags = compose_mode_flags();
    out_pkt.head_frame = target_f;
    out_pkt.ack_remote_frame = confirmed_remote_frame_;
    out_pkt.history_mask = 0u;

    for (uint32_t k = 0u; k < NetInputPacket::kWindowFrames; ++k) {
        if (target_f >= k) {
            const uint32_t f = target_f - k;
            const uint32_t hidx = f % kHistoryCap;
            if (local_valid_[hidx]) {
                out_pkt.history_mask |= static_cast<uint16_t>(1u << k);
                out_pkt.buttons[k] = local_buttons_[hidx];
                out_pkt.sys_c[k] = local_sys_c_[hidx];
                continue;
            }
        }
        out_pkt.buttons[k] = 0xFFu;
        out_pkt.sys_c[k] = 0xFFu;
    }

    // Advertise the most recent fully confirmed sync_frame (multiple of 60)
    const uint32_t max_confirmed_end_frame = std::min(cur_frame_, confirmed_remote_frame_ + 1u);
    const uint32_t sf = (max_confirmed_end_frame / 60u) * 60u;
    if (sf > 0u && earliest_dirty_frame_ >= sf && local_sync_frame_[sf % kHistoryCap] == sf) {
        out_pkt.sync_frame = sf;
        out_pkt.sync_hash_lo = local_sync_hash_[sf % kHistoryCap];
    } else {
        out_pkt.sync_frame = 0u;
        out_pkt.sync_hash_lo = 0u;
    }

    ++packets_sent_;
}

bool NetSession::receive_wire_bytes(const uint8_t* data, size_t len) {
    NetInputPacket pkt{};
    if (!unpack_net_input_packet(data, len, pkt)) return false;
    return receive_packet(pkt);
}

void NetSession::check_sync_parity(uint32_t sf) {
    if (sf == 0u || sf <= last_checked_sync_frame_) return;
    const uint32_t idx = sf % kHistoryCap;
    if (local_sync_frame_[idx] == sf && remote_sync_frame_[idx] == sf) {
        if (local_sync_hash_[idx] == remote_sync_hash_[idx]) {
            ++sync_checks_passed_;
        } else {
            if (desync_count_ == 0u) {
                first_desync_frame_ = sf;
                first_desync_local_hash_ = local_sync_hash_[idx];
                first_desync_remote_hash_ = remote_sync_hash_[idx];
            }
            ++desync_count_;
        }
        last_checked_sync_frame_ = sf;
    }
}

bool NetSession::receive_packet(const NetInputPacket& pkt) {
    if (pkt.magic != NetInputPacket::kMagic || pkt.version != NetInputPacket::kVersion) {
        return false;
    }
    if (pkt.sender_player != (local_player_ ^ 1u)) {
        return false;
    }
    ++packets_received_;

    if ((pkt.mode_flags & 0x07u) != (compose_mode_flags() & 0x07u)) {
        mode_mismatch_ = true;
    }

    // Smooth clock-rift estimation (+/- 500 us/f when peer leads/lags by >= 2 frames)
    peer_lead_frames_ = static_cast<int32_t>(pkt.head_frame) - static_cast<int32_t>(local_head_frame_);
    if (peer_lead_frames_ >= 2) {
        recommended_usec_delta_ = -500;  // Peer is ahead -> speed up slightly (shorter frame sleep)
    } else if (peer_lead_frames_ <= -2) {
        recommended_usec_delta_ = +500;  // We are ahead -> slow down slightly (longer frame sleep)
    } else {
        recommended_usec_delta_ = 0;
    }

    // Process the 12-frame sliding input window from oldest to newest
    for (int k = static_cast<int>(NetInputPacket::kWindowFrames) - 1; k >= 0; --k) {
        if ((pkt.history_mask & (1u << k)) == 0u) continue;
        if (pkt.head_frame < static_cast<uint32_t>(k)) continue;
        const uint32_t f = pkt.head_frame - static_cast<uint32_t>(k);

        // Ignore very old frames already outside our rollback window
        if (cur_frame_ > kMaxRollbackFrames && f < (cur_frame_ - kMaxRollbackFrames)) {
            continue;
        }
        if (f >= cur_frame_ + kHistoryCap / 2u) {
            continue;
        }

        const uint32_t idx = f % kHistoryCap;
        if (!remote_received_[idx]) {
            remote_buttons_[idx] = pkt.buttons[k];
            remote_sys_c_[idx] = pkt.sys_c[k];
            remote_received_[idx] = true;

            // If we already simulated frame f with a predicted input, check if our prediction matched!
            if (f < cur_frame_) {
                if (used_remote_buttons_[idx] != remote_buttons_[idx] ||
                    used_remote_sys_c_[idx] != remote_sys_c_[idx]) {
                    if (f < earliest_dirty_frame_) {
                        earliest_dirty_frame_ = f;
                    }
                }
            }
        }
    }

    // Advance contiguous confirmed_remote_frame_
    while (remote_received_[(confirmed_remote_frame_ + 1u) % kHistoryCap]) {
        ++confirmed_remote_frame_;
    }

    if (pkt.sync_frame > 0u && (pkt.sync_frame % 60u) == 0u) {
        const uint32_t sidx = pkt.sync_frame % kHistoryCap;
        remote_sync_frame_[sidx] = pkt.sync_frame;
        remote_sync_hash_[sidx] = pkt.sync_hash_lo;
        if (earliest_dirty_frame_ >= pkt.sync_frame && confirmed_remote_frame_ + 1u >= pkt.sync_frame) {
            check_sync_parity(pkt.sync_frame);
        }
    }

    return true;
}

void NetSession::resolve_frame_inputs(uint32_t f, uint8_t& out_port_a, uint8_t& out_port_b, uint8_t& out_port_c) {
    const uint32_t idx = f % kHistoryCap;
    const uint8_t loc_btn = local_valid_[idx] ? local_buttons_[idx] : 0xFFu;
    const uint8_t loc_sys = local_valid_[idx] ? local_sys_c_[idx] : 0xFFu;

    uint8_t rem_btn = 0xFFu;
    uint8_t rem_sys = 0xFFu;
    if (remote_received_[idx]) {
        rem_btn = remote_buttons_[idx];
        rem_sys = remote_sys_c_[idx];
    } else if (f > 0u) {
        // Repeat-Last-Input prediction from frame (f - 1)
        const uint32_t pidx = (f - 1u) % kHistoryCap;
        rem_btn = remote_received_[pidx] ? remote_buttons_[pidx] : used_remote_buttons_[pidx];
        rem_sys = remote_received_[pidx] ? remote_sys_c_[pidx] : used_remote_sys_c_[pidx];
    }

    used_remote_buttons_[idx] = rem_btn;
    used_remote_sys_c_[idx] = rem_sys;

    if (local_player_ == 0u) {
        out_port_a = loc_btn;
        out_port_b = rem_btn;
    } else {
        out_port_a = rem_btn;
        out_port_b = loc_btn;
    }
    // Active-low bitwise AND combines P1 + P2 Start/Coin inputs cleanly.
    // In 2P Same-Team Co-op, strip Coin 2 (bit 1) & 2P Start (bit 5) to block 2P VS Join-In.
    out_port_c = static_cast<uint8_t>(loc_sys & rem_sys);
    if (coop_mode_) {
        out_port_c = static_cast<uint8_t>(out_port_c | 0x22u);
    }
}

bool NetSession::flush_pending_rollback(StvMachineState& m, uint8_t* rollback_arena, size_t arena_bytes, bool render_final) {
    if (rollback_arena == nullptr || arena_bytes < kRequiredArenaBytes) {
        return false;
    }
    auto slot_ptr = [&](uint32_t f) -> uint8_t* {
        return rollback_arena + static_cast<size_t>(f % kRollbackSlots) * kSlotBytes;
    };

    if (earliest_dirty_frame_ < cur_frame_) {
        const uint32_t rb_start = earliest_dirty_frame_;
        const uint32_t depth = cur_frame_ - rb_start;
        const uint32_t start_slot = rb_start % kRollbackSlots;
        if (depth <= kMaxRollbackFrames && slot_frame_[start_slot] == rb_start && slot_len_[start_slot] > 0u) {
            if (!m.load_fast_rollback(slot_ptr(rb_start), slot_len_[start_slot])) {
                return false;
            }
            for (uint32_t rf = rb_start; rf < cur_frame_; ++rf) {
                // Only save intermediate rollback slots for frames that are still unconfirmed (rf > confirmed_remote_frame_):
                // any frame <= confirmed_remote_frame_ has final inputs on both peers and can never be rolled back to again.
                if (rf > rb_start && rf > confirmed_remote_frame_) {
                    const uint32_t rslot = rf % kRollbackSlots;
                    slot_len_[rslot] = m.save_fast_rollback(slot_ptr(rf), kSlotBytes);
                    slot_frame_[rslot] = rf;
                }
                uint8_t pa = 0xFFu, pb = 0xFFu, pc = 0xFFu;
                resolve_frame_inputs(rf, pa, pb, pc);
                m.set_input_ports(pa, pb, pc, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                if (render_final && (rf + 1u == cur_frame_)) {
                    if (!m.run_rounds_ex(10000u, /*emit_audio_ring=*/false)) {
                        return false;
                    }
                    m.render_frame();
                } else {
                    if (!m.step_frame_no_render(10000u, /*emit_audio_ring=*/false)) {
                        return false;
                    }
                }
                if (post_frame_hook_ != nullptr) {
                    post_frame_hook_(m, post_frame_ctx_);
                }
                const uint32_t end_f = rf + 1u;
                if ((end_f % 60u) == 0u) {
                    const uint32_t sidx = end_f % kHistoryCap;
                    local_sync_frame_[sidx] = end_f;
                    local_sync_hash_[sidx] = static_cast<uint32_t>(m.compute_sync_hash64() & 0xFFFFFFFFULL);
                }
            }
            ++rollback_count_;
            rollback_frames_total_ += depth;
            if (depth > max_rollback_depth_) max_rollback_depth_ = depth;
        }
        earliest_dirty_frame_ = 0xFFFFFFFFu;
    }

    const uint32_t max_conf_end = std::min(cur_frame_, confirmed_remote_frame_ + 1u);
    const uint32_t conf_sf = (max_conf_end / 60u) * 60u;
    if (conf_sf > 0u) {
        check_sync_parity(conf_sf);
    }
    return true;
}

bool NetSession::step_frame(StvMachineState& m, uint8_t* rollback_arena, size_t arena_bytes, bool render_this_frame) {
    if (rollback_arena == nullptr || arena_bytes < kRequiredArenaBytes) {
        return false;
    }

    auto slot_ptr = [&](uint32_t f) -> uint8_t* {
        return rollback_arena + static_cast<size_t>(f % kRollbackSlots) * kSlotBytes;
    };

    if (!arena_primed_) {
        for (uint32_t s = 0u; s < kRollbackSlots; ++s) {
            slot_len_[s] = m.save_fast_rollback(rollback_arena + static_cast<size_t>(s) * kSlotBytes, kSlotBytes);
            slot_frame_[s] = 0xFFFFFFFFu;
        }
        arena_primed_ = true;
    }

    // 1. Ifwe are about to stall (cannot advance cur_frame_), render the final frame of any pending rollback
    const bool can_advance = (cur_frame_ <= confirmed_remote_frame_ + kMaxRollbackFrames);
    if (!flush_pending_rollback(m, rollback_arena, arena_bytes, /*render_final=*/!can_advance)) {
        return false;
    }

    // 2. If we have reached the maximum speculation depth (7 frames ahead of confirmed_remote_frame_),
    // wait for the network so we never overwrite the oldest unconfirmed rollback slot.
    if (!can_advance) {
        ++stall_frames_;
        return false;
    }

    // 3. Save fast rollback snapshot at the start of cur_frame_ and execute cur_frame_
    const uint32_t cslot = cur_frame_ % kRollbackSlots;
    slot_len_[cslot] = m.save_fast_rollback(slot_ptr(cur_frame_), kSlotBytes);
    slot_frame_[cslot] = cur_frame_;

    uint8_t pa = 0xFFu, pb = 0xFFu, pc = 0xFFu;
    resolve_frame_inputs(cur_frame_, pa, pb, pc);
    m.set_input_ports(pa, pb, pc, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
    if (render_this_frame) {
        if (!m.run_rounds_ex(10000u, /*emit_audio_ring=*/true)) {
            return false;
        }
        m.render_frame();
    } else {
        if (!m.step_frame_no_render(10000u, /*emit_audio_ring=*/true)) {
            return false;
        }
    }
    if (post_frame_hook_ != nullptr) {
        post_frame_hook_(m, post_frame_ctx_);
    }
    ++cur_frame_;

    if ((cur_frame_ % 60u) == 0u) {
        const uint32_t sidx = cur_frame_ % kHistoryCap;
        local_sync_frame_[sidx] = cur_frame_;
        local_sync_hash_[sidx] = static_cast<uint32_t>(m.compute_sync_hash64() & 0xFFFFFFFFULL);
        if (confirmed_remote_frame_ + 1u >= cur_frame_) {
            check_sync_parity(cur_frame_);
        }
    }

    return true;
}

}  // namespace twc98
