// Portable zero-heap deterministic P2P netplay session (Puerta 2 / B-H6-02).
// Hybrid Adaptive Input Delay (1..6f) + Repeat-Last-Input Prediction +
// 8-slot Fast Rollback + 12-frame Sliding Window Redundancy (~20 Kbps).
// Compiles identically on Native PC and standalone Wasm (-DTWC98_NO_STL).
#pragma once

#include "stv_machine.hpp"

#include <cstddef>
#include <cstdint>

namespace twc98 {

struct NetInputPacket {
    static constexpr uint16_t kMagic = 0x5457u;  // "TW"
    static constexpr uint8_t kVersion = 1u;
    static constexpr uint8_t kTypeInputWindow = 1u;
    static constexpr uint8_t kTypePing = 2u;
    static constexpr uint8_t kTypePong = 3u;
    static constexpr uint32_t kWindowFrames = 12u;
    static constexpr size_t kWireBytes = 48u;

    uint16_t magic = kMagic;
    uint8_t version = kVersion;
    uint8_t pkt_type = kTypeInputWindow;
    uint8_t sender_player = 0u;  // 0 = P1 (Host), 1 = P2 (Guest)
    uint8_t mode_flags = 0u;     // bit0=copa_mode, bit1=gk_ai_fix, bit2=coop_mode, bits4..7=input_delay
    uint16_t history_mask = 0u;  // bit k (0..11) set if head_frame - k is valid
    uint32_t head_frame = 0u;
    uint32_t ack_remote_frame = 0xFFFFFFFFu;
    uint8_t buttons[kWindowFrames]{};  // Active-low port_a (P1) or port_b (P2) for head_frame - k
    uint8_t sys_c[kWindowFrames]{};    // Active-low port_c bits for head_frame - k
    uint32_t sync_frame = 0u;          // Confirmed frame index (multiple of 60)
    uint32_t sync_hash_lo = 0u;        // Lower 32 bits of compute_sync_hash64() at sync_frame
};

void pack_net_input_packet(const NetInputPacket& pkt, uint8_t dst[NetInputPacket::kWireBytes]);
bool unpack_net_input_packet(const uint8_t src[NetInputPacket::kWireBytes], size_t len, NetInputPacket& out);

class NetSession {
public:
    static constexpr uint32_t kRollbackSlots = 12u;
    static constexpr uint32_t kMaxRollbackFrames = 11u;
    static constexpr uint32_t kHistoryCap = 256u;
    static constexpr size_t kSlotBytes = StvMachineState::kMaxFastRollbackBytes;
    static constexpr size_t kRequiredArenaBytes = static_cast<size_t>(kRollbackSlots) * kSlotBytes;

    // Computes recommended adaptive input delay (2..4 frames for typical connections, clamped 1..6) from RTT in ms.
    static uint8_t recommend_input_delay_for_rtt_ms(uint32_t rtt_ms);

    using PostFrameHook = void (*)(StvMachineState&, void*);
    void set_post_frame_hook(PostFrameHook hook, void* ctx = nullptr) {
        post_frame_hook_ = hook;
        post_frame_ctx_ = ctx;
    }

    void reset(uint8_t local_player, uint8_t input_delay_frames, bool copa_mode, bool gk_ai_fix,
               bool coop_mode = false);

    // Queues local input for (cur_frame + input_delay) and builds a 48-byte redundant outgoing packet.
    void queue_local_input(uint8_t local_buttons, uint8_t local_sys_c, NetInputPacket& out_pkt);

    // Processes an incoming wire packet from the remote peer (handles duplicates, loss recovery, and out-of-order).
    bool receive_packet(const NetInputPacket& pkt);
    bool receive_wire_bytes(const uint8_t* data, size_t len);

    // Flushes any pending micro-rollback re-simulation up to cur_frame_ without advancing cur_frame_.
    bool flush_pending_rollback(StvMachineState& m, uint8_t* rollback_arena, size_t arena_bytes, bool render_final = true);

    // Executes any pending micro-rollback re-simulation (using step_frame_no_render without duplicate audio)
    // and advances the current frame if within the rollback window. Returns true if cur_frame advanced.
    bool step_frame(StvMachineState& m, uint8_t* rollback_arena, size_t arena_bytes, bool render_this_frame = true);

    uint32_t cur_frame() const { return cur_frame_; }
    uint32_t local_head_frame() const { return local_head_frame_; }
    uint32_t confirmed_remote_frame() const { return confirmed_remote_frame_; }
    uint8_t local_player() const { return local_player_; }
    uint8_t input_delay() const { return input_delay_; }

    uint32_t rollback_count() const { return rollback_count_; }
    uint32_t rollback_frames_total() const { return rollback_frames_total_; }
    uint32_t max_rollback_depth() const { return max_rollback_depth_; }
    uint32_t stall_frames() const { return stall_frames_; }
    uint32_t packets_sent() const { return packets_sent_; }
    uint32_t packets_received() const { return packets_received_; }
    uint32_t sync_checks_passed() const { return sync_checks_passed_; }
    uint32_t desync_count() const { return desync_count_; }
    uint32_t first_desync_frame() const { return first_desync_frame_; }
    uint32_t first_desync_local_hash() const { return first_desync_local_hash_; }
    uint32_t first_desync_remote_hash() const { return first_desync_remote_hash_; }
    bool mode_mismatch() const { return mode_mismatch_; }
    bool coop_mode() const { return coop_mode_; }
    int32_t peer_lead_frames() const { return peer_lead_frames_; }
    int32_t recommended_usec_delta() const { return recommended_usec_delta_; }

private:
    uint8_t compose_mode_flags() const;
    void resolve_frame_inputs(uint32_t f, uint8_t& out_port_a, uint8_t& out_port_b, uint8_t& out_port_c);
    void check_sync_parity(uint32_t frame_idx);

    uint8_t local_player_ = 0u;
    uint8_t input_delay_ = 3u;
    bool copa_mode_ = false;
    bool gk_ai_fix_ = true;
    bool coop_mode_ = false;

    uint32_t cur_frame_ = 0u;
    uint32_t local_head_frame_ = 0u;
    uint32_t confirmed_remote_frame_ = 0u;
    uint32_t earliest_dirty_frame_ = 0xFFFFFFFFu;

    uint8_t local_buttons_[kHistoryCap]{};
    uint8_t local_sys_c_[kHistoryCap]{};
    bool local_valid_[kHistoryCap]{};

    uint8_t remote_buttons_[kHistoryCap]{};
    uint8_t remote_sys_c_[kHistoryCap]{};
    bool remote_received_[kHistoryCap]{};

    uint8_t used_remote_buttons_[kHistoryCap]{};
    uint8_t used_remote_sys_c_[kHistoryCap]{};

    uint32_t slot_frame_[kRollbackSlots]{};
    uint32_t slot_len_[kRollbackSlots]{};
    bool arena_primed_ = false;

    uint32_t local_sync_frame_[kHistoryCap]{};
    uint32_t local_sync_hash_[kHistoryCap]{};
    uint32_t remote_sync_frame_[kHistoryCap]{};
    uint32_t remote_sync_hash_[kHistoryCap]{};
    uint32_t last_checked_sync_frame_ = 0u;

    int32_t peer_lead_frames_ = 0;
    int32_t recommended_usec_delta_ = 0;

    uint32_t rollback_count_ = 0u;
    uint32_t rollback_frames_total_ = 0u;
    uint32_t max_rollback_depth_ = 0u;
    uint32_t stall_frames_ = 0u;
    uint32_t packets_sent_ = 0u;
    uint32_t packets_received_ = 0u;
    uint32_t sync_checks_passed_ = 0u;
    uint32_t desync_count_ = 0u;
    uint32_t first_desync_frame_ = 0u;
    uint32_t first_desync_local_hash_ = 0u;
    uint32_t first_desync_remote_hash_ = 0u;
    bool mode_mismatch_ = false;
    PostFrameHook post_frame_hook_ = nullptr;
    void* post_frame_ctx_ = nullptr;
};

}  // namespace twc98
