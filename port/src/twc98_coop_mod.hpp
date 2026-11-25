#pragma once
// Portable zero-heap 2-Player Same-Team Co-op Mode (Team 0 = P1 + P2 vs Team 1 = CPU).
// Compiles identically on Native PC x64 and standalone WebAssembly (-DTWC98_NO_STL).
// Reversible SH-2 patches preserve 100% bit-exact parity when coop.enabled == false.

#include <cstdint>

namespace twc98 {

class StvMachineState;

struct CoopState {
    bool enabled = false;
    uint8_t p1_actor = 0xFFu;         // Active Team 0 actor index for P1 (0..10, 0xFF = inactive)
    uint8_t p2_actor = 0xFFu;         // Active Team 0 outfield actor index for P2 (1..10, 0xFF = inactive)
    uint8_t last_passer_slot = 0u;    // 0 = P1, 1 = P2 (who last held possession on Team 0)
    uint8_t prev_ball_owner = 0xFFu;  // Previous frame's ball_owner (0..22)
    uint8_t p1_switch_cd = 0u;        // Manual switch debounce frames (P1)
    uint8_t p2_switch_cd = 0u;        // Manual switch debounce frames (P2)
    uint8_t p1_auto_cd = 0u;          // Off-screen auto-switch cooldown frames (P1)
    uint8_t p2_auto_cd = 0u;          // Off-screen auto-switch cooldown frames (P2)
    uint8_t prev_pa_btn = 0xFFu;      // Previous frame Port A byte (active-low)
    uint8_t prev_pb_btn = 0xFFu;      // Previous frame Port B byte (active-low)
    uint8_t call_pass_timer[2]{};     // Tactical "Call Pass" visual pulse timer for P1 / P2
};

namespace coop {

// Scratch byte in High WRAM (0x0607658F, 0 literal refs in EPRG) holding P2's active Team 0 actor index
// for the native VDP1 2P overhead sprite patched at 0x0600CF84.
static constexpr uint32_t kP2MarkerIdxHighWramOff = 0x0007658Fu;

// Enables or disables 2P Same-Team Co-op mode and applies/restores SH-2 patches immediately if EPRG is resident.
void set_coop_mode(StvMachineState& m, bool enabled) noexcept;

// Applies or restores reversible SH-2 EPRG patches in High WRAM based on m.coop.enabled.
// Safe to call during BIOS cold boot (no-op until twcup98 EPRG code is decrypted in High WRAM).
void apply_coop_patches(StvMachineState& m) noexcept;

// Deterministic per-frame supervisor called once per emulated frame (both normal and rollback re-sim).
// Zero side effects when m.coop.enabled == false.
void on_frame_tick(StvMachineState& m) noexcept;

// Releases transient runtime actor tracking while keeping m.coop.enabled intact.
void reset_runtime_trackers(CoopState& st) noexcept;

}  // namespace coop
}  // namespace twc98
