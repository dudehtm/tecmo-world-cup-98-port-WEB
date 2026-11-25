#include "twc98_coop_mod.hpp"

#include "stv_machine.hpp"

#include <cstdint>

namespace twc98 {
namespace coop {
namespace {

constexpr uint32_t kHighWramBase = 0x06000000u;

// Verified High WRAM addresses in twcup98 EPRG (Phase 0 audit on twcup98_cart_20m.bin)
constexpr uint32_t kOffTeam0Ctrl    = 0x0007657Fu;  // u8: 0=P1 human, 0xFF=CPU
constexpr uint32_t kOffTeam1Ctrl    = 0x00076580u;  // u8: 1=P2 human, 0xFF=CPU
constexpr uint32_t kOffTeam0Active  = 0x00076584u;  // u8: P1 active actor index (0..10)
constexpr uint32_t kOffBallOwner    = 0x00076F46u;  // u16 BE: 0..21 actor index, 22=loose
constexpr uint32_t kOffActorBase    = 0x00076F48u;  // 24 actors x 0xB0 stride
constexpr uint32_t kActorStride     = 0x000000B0u;
constexpr uint32_t kOffBallPosX     = 0x00077E68u + 0x14u;  // s32 16.16
constexpr uint32_t kOffBallPosZ     = 0x00077E68u + 0x18u;  // s32 16.16
constexpr uint32_t kOffMatchLiveFlg = 0x00079448u;  // u8: 1 when live match human control is active
constexpr uint32_t kOffBallStateW   = 0x00095774u;  // u32 BE: bits 12..19 = ball_action (4=dribble, 9=set-piece)

// Patch sites in High WRAM (EPRG decrypted code):
// 1. 0x0601418E..0x06014191 (inside Team 0 human-switch coroutine 0x06014178):
//    Original: E30B D215 (mov #11, r3; mov.l @(0x060141E8), r2)
//    Co-op ON: A0F3 0009 (bra 0x06014378; nop) -> clean 1-frame yield loop so C++ CoopSupervisor owns Team 0.
constexpr uint32_t kOffTeam0MgrLoop = 0x0001418Eu;
constexpr uint16_t kOrigMgrW0       = 0xE30Bu;
constexpr uint16_t kOrigMgrW1       = 0xD215u;
constexpr uint16_t kCoopMgrW0       = 0xA0F3u;
constexpr uint16_t kCoopMgrW1       = 0x0009u;

// 2. Native VDP1 2P overhead sprite drawer (0x0600CF28..0x0600CF5E):
//    0x0600CF2E: 8914 (bt 0x0600CF5A when Team1==CPU) -> 0009 (nop)
//    0x0600CF42..44: D312 6D30 (Team1 pass-target arrow) -> A00A 0009 (bra 0x0600CF5A; nop)
//    0x0600CF84: 06076585 (literal ptr to Team1 active actor) -> 0607658F (coop P2 marker byte)
constexpr uint32_t kOffMarker2pSkip = 0x0000CF2Eu;
constexpr uint32_t kOffMarker2pTgt  = 0x0000CF42u;
constexpr uint32_t kOffMarker2pLit  = 0x0000CF84u;
constexpr uint16_t kOrigMarkerSkip  = 0x8914u;
constexpr uint16_t kOrigMarkerTgt0  = 0xD312u;
constexpr uint16_t kOrigMarkerTgt1  = 0x6D30u;
constexpr uint16_t kCoopMarkerTgt0  = 0xA00Au;
constexpr uint16_t kCoopMarkerTgt1  = 0x0009u;
constexpr uint32_t kOrigMarkerLit   = 0x06076585u;
constexpr uint32_t kCoopMarkerLit   = kHighWramBase + kP2MarkerIdxHighWramOff;

inline uint16_t rd16(const uint8_t* hw, uint32_t off) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(hw[off]) << 8) | hw[off + 1u]);
}

inline uint32_t rd32(const uint8_t* hw, uint32_t off) noexcept {
    return (static_cast<uint32_t>(hw[off]) << 24) |
           (static_cast<uint32_t>(hw[off + 1u]) << 16) |
           (static_cast<uint32_t>(hw[off + 2u]) << 8) |
           static_cast<uint32_t>(hw[off + 3u]);
}

inline int32_t rd32s(const uint8_t* hw, uint32_t off) noexcept {
    return static_cast<int32_t>(rd32(hw, off));
}

inline void wr16(uint8_t* hw, uint32_t off, uint16_t v) noexcept {
    hw[off] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    hw[off + 1u] = static_cast<uint8_t>(v & 0xFFu);
}

inline void wr32(uint8_t* hw, uint32_t off, uint32_t v) noexcept {
    hw[off + 0u] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    hw[off + 1u] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    hw[off + 2u] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    hw[off + 3u] = static_cast<uint8_t>(v & 0xFFu);
}

inline uint32_t actor_off(uint8_t idx) noexcept {
    return kOffActorBase + static_cast<uint32_t>(idx) * kActorStride;
}

inline bool is_eprg_resident(const uint8_t* hw) noexcept {
    // Check invariant surrounding words at 0x0601418C (2120) and 0x0600CF2C (88FF)
    return rd16(hw, 0x0001418Cu) == 0x2120u &&
           rd16(hw, 0x0000CF2Cu) == 0x88FFu &&
           rd16(hw, 0x0000CF30u) == 0xD114u;
}

inline void release_human_actor(uint8_t* hw, uint8_t idx) noexcept {
    if (idx > 10u) return;
    const uint32_t a = actor_off(idx);
    const uint16_t fl = rd16(hw, a);
    wr16(hw, a, static_cast<uint16_t>(fl & 0xFFF8u));
    // Matches 0x06014F80 (ReleaseHuman): GK1 returns to 0x1A (gk_auto), outfield returns to 0x02 (AI)
    hw[a + 4u] = (idx == 0u) ? 0x1Au : 0x02u;
}

inline void assign_human_actor(uint8_t* hw, uint8_t idx, uint8_t pad_slot, bool force_state6) noexcept {
    if (idx > 10u) return;
    const uint32_t a = actor_off(idx);
    const uint16_t fl = rd16(hw, a);
    wr16(hw, a, static_cast<uint16_t>((fl & 0xFFF8u) | 0x04u | (pad_slot & 0x01u)));
    if (force_state6 && idx >= 1u) {
        hw[a + 4u] = 0x06u;
    }
}

uint8_t pick_best_outfield(const uint8_t* hw, uint8_t exclude_a, uint8_t exclude_b, bool prefer_onscreen) noexcept {
    const int64_t bx = static_cast<int64_t>(rd32s(hw, kOffBallPosX)) >> 12;
    const int64_t bz = static_cast<int64_t>(rd32s(hw, kOffBallPosZ)) >> 12;
    int64_t best_on_d2 = 0x7FFFFFFFFFFFFFFFLL;
    int64_t best_any_d2 = 0x7FFFFFFFFFFFFFFFLL;
    uint8_t best_on = 0xFFu;
    uint8_t best_any = 0xFFu;

    for (uint8_t i = 1u; i <= 10u; ++i) {
        if (i == exclude_a || i == exclude_b) continue;
        const uint32_t a = actor_off(i);
        const uint16_t fl = rd16(hw, a);
        if ((fl & 0x8000u) == 0u) continue;
        const int64_t px = static_cast<int64_t>(rd32s(hw, a + 0x14u)) >> 12;
        const int64_t pz = static_cast<int64_t>(rd32s(hw, a + 0x18u)) >> 12;
        const int64_t dx = px - bx;
        const int64_t dz = pz - bz;
        const int64_t d2 = dx * dx + dz * dz;
        if (prefer_onscreen && (fl & 0x2000u) == 0u && d2 < best_on_d2) {
            best_on_d2 = d2;
            best_on = i;
        }
        if (d2 < best_any_d2) {
            best_any_d2 = d2;
            best_any = i;
        }
    }
    if (best_on != 0xFFu) return best_on;
    return best_any;
}

// Deterministic manual switch picker for Btn C (L / Num3):
// If the player holds a direction on the pad, prefer the nearest teammate in that directional cone;
// otherwise (or if none lies in that direction), pick the closest free outfield teammate to the ball.
uint8_t pick_manual_switch_target(const uint8_t* hw, uint8_t cur_actor, uint8_t partner_actor,
                                  uint8_t port_byte) noexcept {
    const int64_t dir_x = ((port_byte & 0x40u) == 0u ? 1 : 0) - ((port_byte & 0x80u) == 0u ? 1 : 0);
    const int64_t dir_z = ((port_byte & 0x10u) == 0u ? 1 : 0) - ((port_byte & 0x20u) == 0u ? 1 : 0);

    if ((dir_x != 0 || dir_z != 0) && cur_actor >= 1u && cur_actor <= 10u) {
        const uint32_t ca = actor_off(cur_actor);
        const int64_t cx = static_cast<int64_t>(rd32s(hw, ca + 0x14u)) >> 12;
        const int64_t cz = static_cast<int64_t>(rd32s(hw, ca + 0x18u)) >> 12;
        const int64_t bx = static_cast<int64_t>(rd32s(hw, kOffBallPosX)) >> 12;
        const int64_t bz = static_cast<int64_t>(rd32s(hw, kOffBallPosZ)) >> 12;
        const int64_t dir_len2 = dir_x * dir_x + dir_z * dir_z;

        int64_t best_score = 0x7FFFFFFFFFFFFFFFLL;
        uint8_t best_dir = 0xFFu;
        for (uint8_t i = 1u; i <= 10u; ++i) {
            if (i == cur_actor || i == partner_actor) continue;
            const uint32_t a = actor_off(i);
            const uint16_t fl = rd16(hw, a);
            if ((fl & 0x8000u) == 0u) continue;
            const int64_t px = static_cast<int64_t>(rd32s(hw, a + 0x14u)) >> 12;
            const int64_t pz = static_cast<int64_t>(rd32s(hw, a + 0x18u)) >> 12;
            const int64_t vx = px - cx;
            const int64_t vz = pz - cz;
            const int64_t dot = vx * dir_x + vz * dir_z;
            if (dot <= 0) continue;
            const int64_t d2_cur = vx * vx + vz * vz;
            // Require teammate inside a clean +/-60 deg directional cone: dot^2 * 4 >= d2_cur * dir_len2
            if (dot * dot * 4 < d2_cur * dir_len2) continue;
            const int64_t dbx = px - bx;
            const int64_t dbz = pz - bz;
            const int64_t d2_ball = dbx * dbx + dbz * dbz;
            // Prioritize proximity to current player in pressed direction + closeness to ball + on-screen bonus
            int64_t score = d2_cur + (d2_ball >> 1);
            if ((fl & 0x2000u) != 0u) score += 200000000LL;
            if (score < best_score) {
                best_score = score;
                best_dir = i;
            }
        }
        if (best_dir != 0xFFu) return best_dir;
    }

    return pick_best_outfield(hw, cur_actor, partner_actor, true);
}

inline int64_t actor_ball_dist_sq_16(const uint8_t* hw, uint8_t idx) noexcept {
    const uint32_t a = actor_off(idx);
    const int64_t px = static_cast<int64_t>(rd32s(hw, a + 0x14u)) >> 16;
    const int64_t pz = static_cast<int64_t>(rd32s(hw, a + 0x18u)) >> 16;
    const int64_t bx = static_cast<int64_t>(rd32s(hw, kOffBallPosX)) >> 16;
    const int64_t bz = static_cast<int64_t>(rd32s(hw, kOffBallPosZ)) >> 16;
    const int64_t dx = px - bx;
    const int64_t dz = pz - bz;
    return dx * dx + dz * dz;
}

bool is_live_match_1p_playable(const uint8_t* hw) noexcept {
    if (hw[kOffTeam0Ctrl] != 0x00u || hw[kOffTeam1Ctrl] != 0xFFu) {
        return false;
    }
    if (hw[kOffMatchLiveFlg] != 0x01u) {
        return false;
    }
    for (uint8_t i = 0u; i < 23u; ++i) {
        if ((rd16(hw, actor_off(i)) & 0x8000u) == 0u) {
            return false;
        }
    }
    return true;
}

}  // namespace

void reset_runtime_trackers(CoopState& st) noexcept {
    st.p1_actor = 0xFFu;
    st.p2_actor = 0xFFu;
    st.last_passer_slot = 0u;
    st.prev_ball_owner = 0xFFu;
    st.p1_switch_cd = 0u;
    st.p2_switch_cd = 0u;
    st.p1_auto_cd = 0u;
    st.p2_auto_cd = 0u;
    st.prev_pa_btn = 0xFFu;
    st.prev_pb_btn = 0xFFu;
    st.call_pass_timer[0] = 0u;
    st.call_pass_timer[1] = 0u;
}

void apply_coop_patches(StvMachineState& m) noexcept {
    uint8_t* hw = m.inner.high_wram.data();
    if (!is_eprg_resident(hw)) {
        return;
    }
    const bool on = m.coop.enabled;

    // 1. Team 0 human-switch manager (0x0601418E..0x06014191)
    const uint16_t mw0 = rd16(hw, kOffTeam0MgrLoop);
    const uint16_t mw1 = rd16(hw, kOffTeam0MgrLoop + 2u);
    if ((mw0 == kOrigMgrW0 && mw1 == kOrigMgrW1) || (mw0 == kCoopMgrW0 && mw1 == kCoopMgrW1)) {
        const uint16_t want0 = on ? kCoopMgrW0 : kOrigMgrW0;
        const uint16_t want1 = on ? kCoopMgrW1 : kOrigMgrW1;
        if (mw0 != want0 || mw1 != want1) {
            wr16(hw, kOffTeam0MgrLoop, want0);
            wr16(hw, kOffTeam0MgrLoop + 2u, want1);
            m.mark_high_wram_dirty(kOffTeam0MgrLoop, 4u);
        }
    }

    // 2. Native VDP1 2P overhead sprite drawer (0x0600CF2E, 0x0600CF42..44, 0x0600CF84)
    const uint16_t sk = rd16(hw, kOffMarker2pSkip);
    if (sk == kOrigMarkerSkip || sk == 0x0009u) {
        const uint16_t want_sk = on ? 0x0009u : kOrigMarkerSkip;
        if (sk != want_sk) {
            wr16(hw, kOffMarker2pSkip, want_sk);
            m.mark_high_wram_dirty(kOffMarker2pSkip, 2u);
        }
    }
    const uint16_t t0 = rd16(hw, kOffMarker2pTgt);
    const uint16_t t1 = rd16(hw, kOffMarker2pTgt + 2u);
    if ((t0 == kOrigMarkerTgt0 && t1 == kOrigMarkerTgt1) || (t0 == kCoopMarkerTgt0 && t1 == kCoopMarkerTgt1)) {
        const uint16_t want_t0 = on ? kCoopMarkerTgt0 : kOrigMarkerTgt0;
        const uint16_t want_t1 = on ? kCoopMarkerTgt1 : kOrigMarkerTgt1;
        if (t0 != want_t0 || t1 != want_t1) {
            wr16(hw, kOffMarker2pTgt, want_t0);
            wr16(hw, kOffMarker2pTgt + 2u, want_t1);
            m.mark_high_wram_dirty(kOffMarker2pTgt, 4u);
        }
    }
    const uint32_t lit = rd32(hw, kOffMarker2pLit);
    if (lit == kOrigMarkerLit || lit == kCoopMarkerLit) {
        const bool was_patched = (lit == kCoopMarkerLit);
        const uint32_t want_lit = on ? kCoopMarkerLit : kOrigMarkerLit;
        if (lit != want_lit) {
            wr32(hw, kOffMarker2pLit, want_lit);
            m.mark_high_wram_dirty(kOffMarker2pLit, 4u);
        }
        if (!on && was_patched) {
            hw[kP2MarkerIdxHighWramOff] = 0x00u;
            m.mark_high_wram_dirty(kP2MarkerIdxHighWramOff, 1u);
        }
    }
}

void set_coop_mode(StvMachineState& m, bool enabled) noexcept {
    const bool was_enabled = m.coop.enabled;
    m.coop.enabled = enabled;
    if (was_enabled && !enabled) {
        // Release P2's controlled outfield actor back to AI cleanly if we are live in a match
        uint8_t* hw = m.inner.high_wram.data();
        if (m.coop.p2_actor >= 1u && m.coop.p2_actor <= 10u && is_eprg_resident(hw)) {
            release_human_actor(hw, m.coop.p2_actor);
        }
        reset_runtime_trackers(m.coop);
    } else if (!was_enabled && enabled) {
        reset_runtime_trackers(m.coop);
    }
    apply_coop_patches(m);
}

void on_frame_tick(StvMachineState& m) noexcept {
    if (!m.coop.enabled) {
        return;
    }

    apply_coop_patches(m);

    uint8_t* hw = m.inner.high_wram.data();
    auto& st = m.coop;
    const uint8_t cur_pa = m.inputs.port_a;
    const uint8_t cur_pb = m.inputs.port_b;

    if (!is_live_match_1p_playable(hw)) {
        // Outside live gameplay (menus, cutscenes, goal replays, set-piece walk-ups, halftime):
        // Release P2's actor back to AI so the referee/set-piece formation never waits on a frozen human actor!
        if (is_eprg_resident(hw)) {
            if (st.p2_actor >= 1u && st.p2_actor <= 10u) {
                release_human_actor(hw, st.p2_actor);
            }
            hw[kP2MarkerIdxHighWramOff] = 0xFFu;
        }
        st.p1_actor = 0xFFu;
        st.p2_actor = 0xFFu;
        st.prev_ball_owner = 0xFFu;
        st.p1_switch_cd = 0u;
        st.p2_switch_cd = 0u;
        st.p1_auto_cd = 0u;
        st.p2_auto_cd = 0u;
        st.call_pass_timer[0] = 0u;
        st.call_pass_timer[1] = 0u;
        st.prev_pa_btn = cur_pa;
        st.prev_pb_btn = cur_pb;
        return;
    }

    // Decrement debounce / pulse timers (p1_auto_cd / p2_auto_cd track sustained off-screen frames)
    if (st.p1_switch_cd > 0u) --st.p1_switch_cd;
    if (st.p2_switch_cd > 0u) --st.p2_switch_cd;
    if (st.call_pass_timer[0] > 0u) --st.call_pass_timer[0];
    if (st.call_pass_timer[1] > 0u) --st.call_pass_timer[1];

    const uint16_t bo_raw = rd16(hw, kOffBallOwner);
    const uint8_t ball_owner = (bo_raw <= 22u) ? static_cast<uint8_t>(bo_raw) : 22u;
    const uint8_t ball_action = static_cast<uint8_t>((rd32(hw, kOffBallStateW) >> 12) & 0xFFu);
    const bool is_set_piece = (ball_action == 9u);

    // 1. Initialize P1 & P2 actor indices on first live frame (or recover from invalid index)
    if (st.p1_actor > 10u) {
        const uint8_t game_p1 = hw[kOffTeam0Active];
        if (game_p1 <= 10u) {
            st.p1_actor = game_p1;
        } else {
            st.p1_actor = pick_best_outfield(hw, 0xFFu, 0xFFu, true);
        }
        if (st.p1_actor <= 10u) {
            assign_human_actor(hw, st.p1_actor, 0u, (st.p1_actor >= 1u && !is_set_piece));
        }
    }
    if (st.p2_actor < 1u || st.p2_actor > 10u || st.p2_actor == st.p1_actor) {
        const uint8_t pick = pick_best_outfield(hw, st.p1_actor, 0xFFu, true);
        if (pick >= 1u && pick <= 10u) {
            st.p2_actor = pick;
            assign_human_actor(hw, st.p2_actor, 1u, true);
        }
    }

    // Track who last held the ball on Team 0 (for AI-receiver pass handoff)
    if (st.prev_ball_owner == st.p1_actor && st.p1_actor <= 10u) {
        st.last_passer_slot = 0u;
    } else if (st.prev_ball_owner == st.p2_actor && st.p2_actor >= 1u && st.p2_actor <= 10u) {
        st.last_passer_slot = 1u;
    }

    // 2. GK1 possession / Set-Piece ownership rules (no softlocks!)
    if (ball_owner == 0u) {
        // GK1 caught the ball or is taking a goal kick -> P1 temporarily directs GK1, P2 stays free outfield
        if (st.p1_actor != 0u) {
            if (st.p1_actor >= 1u && st.p1_actor <= 10u) {
                release_human_actor(hw, st.p1_actor);
            }
            st.p1_actor = 0u;
            assign_human_actor(hw, 0u, 0u, false);
        }
    } else {
        // If P1 was previously on GK1 and GK1 released the ball, hand P1 back to an outfield player immediately
        if (st.p1_actor == 0u) {
            release_human_actor(hw, 0u);
            uint8_t next_p1 = 0xFFu;
            if (ball_owner >= 1u && ball_owner <= 10u && ball_owner != st.p2_actor) {
                next_p1 = ball_owner;
            } else {
                next_p1 = pick_best_outfield(hw, st.p2_actor, 0xFFu, true);
            }
            st.p1_actor = next_p1;
            assign_human_actor(hw, st.p1_actor, 0u, true);
        }

        if (is_set_piece && ball_owner >= 1u && ball_owner <= 10u) {
            // Kickoff / throw-in / corner / free kick / penalty: P1 executes set piece, P2 moves freely in the box!
            if (st.p1_actor != ball_owner) {
                if (st.p1_actor >= 1u && st.p1_actor <= 10u) {
                    release_human_actor(hw, st.p1_actor);
                }
                if (st.p2_actor == ball_owner) {
                    const uint8_t next_p2 = pick_best_outfield(hw, ball_owner, 0xFFu, true);
                    st.p2_actor = next_p2;
                    assign_human_actor(hw, st.p2_actor, 1u, true);
                }
                st.p1_actor = ball_owner;
                // Do not overwrite +0x04 during set-piece animation state
                assign_human_actor(hw, st.p1_actor, 0u, false);
            }
        } else if (ball_owner >= 1u && ball_owner <= 10u) {
            // Open play: Team 0 outfield player has the ball!
            if (ball_owner == st.p1_actor) {
                // P1 already controls ball carrier -> keep both P1 and P2 untouched!
            } else if (ball_owner == st.p2_actor) {
                // P2 already controls ball carrier -> P2 keeps the ball! P1 stays on P1's runner!
            } else {
                // An AI teammate received the ball -> hand it to whoever made the pass (or closest if contested)
                uint8_t target_slot = st.last_passer_slot;
                if (st.prev_ball_owner >= 11u && st.prev_ball_owner <= 21u) {
                    // Interception/steal by AI teammate -> give to whichever of P1/P2 is closer
                    const int64_t bx = static_cast<int64_t>(rd32s(hw, actor_off(ball_owner) + 0x14u)) >> 12;
                    const int64_t bz = static_cast<int64_t>(rd32s(hw, actor_off(ball_owner) + 0x18u)) >> 12;
                    const int64_t p1x = static_cast<int64_t>(rd32s(hw, actor_off(st.p1_actor) + 0x14u)) >> 12;
                    const int64_t p1z = static_cast<int64_t>(rd32s(hw, actor_off(st.p1_actor) + 0x18u)) >> 12;
                    const int64_t p2x = static_cast<int64_t>(rd32s(hw, actor_off(st.p2_actor) + 0x14u)) >> 12;
                    const int64_t p2z = static_cast<int64_t>(rd32s(hw, actor_off(st.p2_actor) + 0x18u)) >> 12;
                    const int64_t d1 = (p1x - bx) * (p1x - bx) + (p1z - bz) * (p1z - bz);
                    const int64_t d2 = (p2x - bx) * (p2x - bx) + (p2z - bz) * (p2z - bz);
                    target_slot = (d2 < d1) ? 1u : 0u;
                }
                if (target_slot == 1u) {
                    release_human_actor(hw, st.p2_actor);
                    st.p2_actor = ball_owner;
                    assign_human_actor(hw, st.p2_actor, 1u, true);
                } else {
                    release_human_actor(hw, st.p1_actor);
                    st.p1_actor = ball_owner;
                    assign_human_actor(hw, st.p1_actor, 0u, true);
                }
            }
        }
    }

    // 3. Independent Manual Player Switching with Btn C (L / Num3 = bit 2 active-low, rising edge)
    //    Works anytime the player is off-ball (both in defense and off-ball attacking runs!).
    const bool p1_btn_c_edge = ((st.prev_pa_btn & 0x04u) != 0u) && ((cur_pa & 0x04u) == 0u);
    const bool p2_btn_c_edge = ((st.prev_pb_btn & 0x04u) != 0u) && ((cur_pb & 0x04u) == 0u);

    if (p1_btn_c_edge && st.p1_switch_cd == 0u && st.p1_actor >= 1u && ball_owner != st.p1_actor) {
        const uint8_t cand = pick_manual_switch_target(hw, st.p1_actor, st.p2_actor, cur_pa);
        if (cand >= 1u && cand <= 10u) {
            release_human_actor(hw, st.p1_actor);
            st.p1_actor = cand;
            assign_human_actor(hw, st.p1_actor, 0u, true);
            st.p1_switch_cd = 8u;
            st.p1_auto_cd = 0u;
        }
    }
    if (p2_btn_c_edge && st.p2_switch_cd == 0u && st.p2_actor >= 1u && ball_owner != st.p2_actor) {
        const uint8_t cand = pick_manual_switch_target(hw, st.p2_actor, st.p1_actor, cur_pb);
        if (cand >= 1u && cand <= 10u) {
            release_human_actor(hw, st.p2_actor);
            st.p2_actor = cand;
            assign_human_actor(hw, st.p2_actor, 1u, true);
            st.p2_switch_cd = 8u;
            st.p2_auto_cd = 0u;
        }
    }

    // 4. Sustained Off-Screen Rescue ONLY (never randomly hijacks an on-screen or nearby player!):
    //    Only auto-switches in defense/loose ball if the player stays off-screen AND >520 units from the ball for >=45f (0.75s).
    constexpr int64_t kFarDistSq = 520LL * 520LL;
    if (ball_owner >= 11u) {
        if (st.p1_actor >= 1u && st.p1_actor <= 10u) {
            const bool off_far = ((rd16(hw, actor_off(st.p1_actor)) & 0x2000u) != 0u) &&
                                 (actor_ball_dist_sq_16(hw, st.p1_actor) > kFarDistSq);
            if (off_far) {
                if (st.p1_auto_cd < 255u) ++st.p1_auto_cd;
                if (st.p1_auto_cd >= 45u) {
                    const uint8_t cand = pick_best_outfield(hw, st.p1_actor, st.p2_actor, true);
                    if (cand >= 1u && cand <= 10u && (rd16(hw, actor_off(cand)) & 0x2000u) == 0u) {
                        release_human_actor(hw, st.p1_actor);
                        st.p1_actor = cand;
                        assign_human_actor(hw, st.p1_actor, 0u, true);
                        st.p1_auto_cd = 0u;
                    }
                }
            } else {
                st.p1_auto_cd = 0u;
            }
        }
        if (st.p2_actor >= 1u && st.p2_actor <= 10u) {
            const bool off_far = ((rd16(hw, actor_off(st.p2_actor)) & 0x2000u) != 0u) &&
                                 (actor_ball_dist_sq_16(hw, st.p2_actor) > kFarDistSq);
            if (off_far) {
                if (st.p2_auto_cd < 255u) ++st.p2_auto_cd;
                if (st.p2_auto_cd >= 45u) {
                    const uint8_t cand = pick_best_outfield(hw, st.p2_actor, st.p1_actor, true);
                    if (cand >= 1u && cand <= 10u && (rd16(hw, actor_off(cand)) & 0x2000u) == 0u) {
                        release_human_actor(hw, st.p2_actor);
                        st.p2_actor = cand;
                        assign_human_actor(hw, st.p2_actor, 1u, true);
                        st.p2_auto_cd = 0u;
                    }
                }
            } else {
                st.p2_auto_cd = 0u;
            }
        }
    } else {
        st.p1_auto_cd = 0u;
        st.p2_auto_cd = 0u;
    }

    // 5. Tactical "Call Pass" visual signal (Btn D / Extra = bit 3 active-low, rising edge off-ball)
    const bool p1_btn_d_edge = ((st.prev_pa_btn & 0x08u) != 0u) && ((cur_pa & 0x08u) == 0u);
    const bool p2_btn_d_edge = ((st.prev_pb_btn & 0x08u) != 0u) && ((cur_pb & 0x08u) == 0u);
    if (p1_btn_d_edge && ball_owner != st.p1_actor) {
        st.call_pass_timer[0] = 28u;
    }
    if (p2_btn_d_edge && ball_owner != st.p2_actor) {
        st.call_pass_timer[1] = 28u;
    }

    // 6. Enforce Team 0 invariants & mirror active indices to High WRAM (for native VDP1 1P & 2P markers!)
    for (uint8_t i = 0u; i <= 10u; ++i) {
        const uint32_t a = actor_off(i);
        const uint16_t fl = rd16(hw, a);
        if (i == st.p1_actor) {
            const uint16_t want = static_cast<uint16_t>((fl & 0xFFF8u) | 0x04u);
            if (fl != want) wr16(hw, a, want);
            if (i >= 1u && hw[a + 4u] == 0x02u) hw[a + 4u] = 0x06u;
        } else if (i == st.p2_actor) {
            const uint16_t want = static_cast<uint16_t>((fl & 0xFFF8u) | 0x05u);
            if (fl != want) wr16(hw, a, want);
            if (i >= 1u && hw[a + 4u] == 0x02u) hw[a + 4u] = 0x06u;
        } else if ((fl & 0x0004u) != 0u) {
            release_human_actor(hw, i);
        }
    }

    hw[kOffTeam0Active] = st.p1_actor;
    hw[kP2MarkerIdxHighWramOff] = st.p2_actor;

    st.prev_ball_owner = ball_owner;
    st.prev_pa_btn = cur_pa;
    st.prev_pb_btn = cur_pb;
}

}  // namespace coop
}  // namespace twc98
