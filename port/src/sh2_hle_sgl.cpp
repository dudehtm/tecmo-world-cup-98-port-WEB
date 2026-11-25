#include "sh2_hle_sgl.hpp"

#include "sh2_step_inl.hpp"
#include "stv_machine.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace twc98 {
namespace {

__attribute__((always_inline)) inline uint16_t hle_be16(const uint8_t* hw, uint32_t off) noexcept {
    uint16_t v;
    std::memcpy(&v, hw + off, 2);
    return __builtin_bswap16(v);
}

__attribute__((always_inline)) inline uint32_t hle_be32(const uint8_t* hw, uint32_t off) noexcept {
    uint32_t v;
    std::memcpy(&v, hw + off, 4);
    return __builtin_bswap32(v);
}

__attribute__((always_inline)) inline void hle_st32(uint8_t* hw, uint32_t off, uint32_t val) noexcept {
    const uint32_t v = __builtin_bswap32(val);
    std::memcpy(hw + off, &v, 4);
}

__attribute__((always_inline)) inline bool hle_line_ok(StvMachineState& m, uint32_t ln) noexcept {
    const uint32_t wi = ln >> 5;
    const uint32_t mask = 1u << (ln & 31u);
    if (__builtin_expect((m.aot_page_valid_bits[wi] & mask) != 0u, 1)) return true;
    return m.revalidate_aot_page(ln);
}

__attribute__((always_inline)) inline bool hle_hw_range_aligned4(uint32_t ea, uint32_t bytes, uint32_t& out_off) noexcept {
    const uint32_t h = (ea & 0xDFFFFFFFu) - kHighWramBase;
    if ((h & 3u) != 0u) return false;
    if (h > kHighWramSize || bytes > (kHighWramSize - h)) return false;
    out_off = h;
    return true;
}

__attribute__((always_inline)) inline bool ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) noexcept {
    return (a0 < b1) && (b0 < a1);
}

template <bool IsSlave>
uint32_t run_sgl_vtx_loop(int cpu_id, Sh2CpuState& cpu, StvMachineState& m,
                          int max_steps, uint64_t cycle_limit) noexcept {
    if (cpu.delay != 0u || (cpu.sr & kShSrS) != 0u || cpu.r[14] != 0xFFFFFF00u) {
        return 0u;
    }
    if (max_steps < 74) {
        return 0u;
    }
    if (cpu_id == 0 && (cpu.cycles >= cycle_limit || (cycle_limit - cpu.cycles) < 77ULL)) {
        return 0u;
    }

    // Validate the 3 64-byte AOT lines covering the 76-instruction loop body.
    constexpr uint32_t kLoopPc    = IsSlave ? 0x060631DEu : 0x0605CD86u;
    constexpr uint32_t kExitPc    = IsSlave ? 0x06063276u : 0x0605CE1Eu;
    constexpr uint32_t kSlotPc    = IsSlave ? 0x06063274u : 0x0605CE1Cu;
    constexpr uint32_t kClipBraPc = IsSlave ? 0x06062FA8u : 0x0605CB90u;
    constexpr uint32_t kClipCurPc = IsSlave ? 0x06063246u : 0x0605CDF0u;
    constexpr uint32_t kLine0     = (kLoopPc - kHighWramBase) >> StvMachineState::kAotLineShift;
    if (!hle_line_ok(m, kLine0) || !hle_line_ok(m, kLine0 + 1u) || !hle_line_ok(m, kLine0 + 2u)) {
        return 0u;
    }

    // GBR offsets differ slightly between Slave and Master copies of the SGL routine.
    constexpr uint32_t kGbrW0 = IsSlave ? (0x4Au * 2u) : (0x48u * 2u);
    constexpr uint32_t kGbrW1 = IsSlave ? (0x4Bu * 2u) : (0x49u * 2u);
    constexpr uint32_t kGbrL0 = IsSlave ? (0xE3u * 4u) : (0xE2u * 4u);
    constexpr uint32_t kGbrL1 = IsSlave ? (0xDFu * 4u) : (0xDEu * 4u);

    // Compute maximum iterations permitted by step/cycle budgets and r9 counter.
    uint32_t max_iters = static_cast<uint32_t>(max_steps / 74);
    if (cpu_id == 0) {
        const uint64_t cyc_iters = (cycle_limit - cpu.cycles) / 77ULL;
        if (cyc_iters < max_iters) max_iters = static_cast<uint32_t>(cyc_iters);
    }
    const uint32_t r9_init = cpu.r[9];
    const uint32_t rem_r9 = (r9_init == 0u) ? 0xFFFFFFFFu : r9_init;
    if (rem_r9 < max_iters) max_iters = rem_r9;
    if (max_iters == 0u) return 0u;

    // Validate static pointers in High WRAM:
    //   r6 points to matrix row 2 ([mat+32]); matrix occupies [r6-32, r6+16) = 48 bytes.
    //   r15 points to stack pair [r15+0, r15+8) = 8 bytes.
    //   gbr reads offsets up to kGbrL0 + 4.
    uint32_t mat_off = 0u, sp_off = 0u, gbr_off = 0u;
    if (!hle_hw_range_aligned4(cpu.r[6] - 32u, 48u, mat_off)) return 0u;
    if (!hle_hw_range_aligned4(cpu.r[15], 8u, sp_off)) return 0u;
    if (!hle_hw_range_aligned4(cpu.gbr, kGbrL0 + 4u, gbr_off)) return 0u;

    // Validate r10 (input vertices: stepping -12 bytes/iter, reads [r10..r10+12))
    // and r11 (output buffer: stepping -16 bytes/iter, writes [r11_after+12..r11_after+28)).
    const uint32_t r10_span = (max_iters - 1u) * 12u + 12u;
    const uint32_t r10_lo_ea = cpu.r[10] - (max_iters - 1u) * 12u;
    uint32_t vtx_lo_off = 0u;
    if (!hle_hw_range_aligned4(r10_lo_ea, r10_span, vtx_lo_off)) return 0u;

    const uint32_t r11_span = max_iters * 16u;
    const uint32_t r11_lo_ea = cpu.r[11] - max_iters * 16u + 12u;
    uint32_t out_lo_off = 0u;
    if (!hle_hw_range_aligned4(r11_lo_ea, r11_span, out_lo_off)) return 0u;
    // Output buffer must stay in High WRAM data region (>= 0x06070000) so AOT dirty bits are unaffected.
    if (out_lo_off < StvMachineState::kAotCodeRangeBytes) return 0u;

    // Ensure output buffer does not alias matrix, stack, GBR work area, or input vertices.
    const uint32_t out_hi_off = out_lo_off + r11_span;
    if (ranges_overlap(out_lo_off, out_hi_off, mat_off, mat_off + 48u) ||
        ranges_overlap(out_lo_off, out_hi_off, sp_off, sp_off + 8u) ||
        ranges_overlap(out_lo_off, out_hi_off, gbr_off, gbr_off + kGbrL0 + 4u) ||
        ranges_overlap(out_lo_off, out_hi_off, vtx_lo_off, vtx_lo_off + r10_span)) {
        return 0u;
    }

    uint8_t* __restrict const hw = m.inner.high_wram.data();

    // Hoist loop-invariant 3x4 matrix, stack screen-distance dividend, and GBR constants.
    const int64_t m00 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 0u)));
    const int64_t m01 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 4u)));
    const int64_t m02 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 8u)));
    const uint32_t tx = hle_be32(hw, mat_off + 12u);

    const int64_t m10 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 16u)));
    const int64_t m11 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 20u)));
    const int64_t m12 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 24u)));
    const uint32_t ty = hle_be32(hw, mat_off + 28u);

    const int64_t m20 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 32u)));
    const int64_t m21 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 36u)));
    const int64_t m22 = static_cast<int64_t>(to_s32(hle_be32(hw, mat_off + 40u)));
    const uint32_t tz = hle_be32(hw, mat_off + 44u);

    const uint32_t sp_lo = hle_be32(hw, sp_off + 0u);
    const uint32_t sp_hi = hle_be32(hw, sp_off + 4u);
    const int64_t div_num = to_s64((static_cast<uint64_t>(sp_hi) << 32) | static_cast<uint64_t>(sp_lo));

    const uint32_t gbr_w0_v = sign_extend_16(hle_be16(hw, gbr_off + kGbrW0));
    const uint32_t gbr_w1_v = sign_extend_16(hle_be16(hw, gbr_off + kGbrW1));
    const uint32_t gbr_l0_v = hle_be32(hw, gbr_off + kGbrL0);
    const uint32_t gbr_l1_v = hle_be32(hw, gbr_off + kGbrL1);

    // Live loop registers.
    uint32_t r0 = cpu.r[0];
    uint32_t r1 = cpu.r[1];
    uint32_t r2 = cpu.r[2];
    uint32_t r3 = cpu.r[3];
    uint32_t r4 = cpu.r[4];
    uint32_t r5 = cpu.r[5];
    uint32_t r6 = cpu.r[6];
    uint32_t r7 = cpu.r[7];
    const uint32_t r8 = cpu.r[8];
    const int32_t r8_s = to_s32(r8);
    uint32_t r9 = cpu.r[9];
    uint32_t r10 = cpu.r[10];
    uint32_t r11 = cpu.r[11];
    const uint32_t r12 = cpu.r[12];
    const uint32_t r13 = cpu.r[13];

    uint32_t vtx_off = vtx_lo_off + (max_iters - 1u) * 12u;
    uint32_t out_off = out_lo_off + max_iters * 16u - 12u;

    uint32_t div_dvsr = 0u;
    uint32_t div_nth = 0u;
    uint32_t div_ntl = 0u;
    bool div_ovf_hit = false;

    uint32_t last_wr_off = 0u;
    uint32_t last_wr_val = 0u;
    uint32_t total_steps = 0u;
    uint64_t total_cyc = 0ULL;
    uint32_t iters_done = 0u;
    uint64_t last_mac = 0ULL;

    for (uint32_t it = 0u; it < max_iters; ++it) {
        const int64_t vx = static_cast<int64_t>(to_s32(hle_be32(hw, vtx_off + 0u)));
        const int64_t vy = static_cast<int64_t>(to_s32(hle_be32(hw, vtx_off + 4u)));
        const int64_t vz = static_cast<int64_t>(to_s32(hle_be32(hw, vtx_off + 8u)));

        // Z = (row2 . vtx) >> 16 + tz, clamped to >= r8 (signed)
        const uint64_t mac_z = static_cast<uint64_t>(m20 * vx + m21 * vy + m22 * vz);
        r3 = static_cast<uint32_t>(mac_z >> 16) + tz;
        const bool ge_near = (to_s32(r3) >= r8_s);
        if (!ge_near) {
            r3 = r8;
        }

        // DIVU: (sp_hi:sp_lo) / s32(r3)
        div_dvsr = r3;
        const int64_t b = static_cast<int64_t>(to_s32(r3));
        if (__builtin_expect(b == 0 || (div_num == std::numeric_limits<int64_t>::min() && b == -1), 0)) {
            div_ovf_hit = true;
            div_ntl = 0x7FFFFFFFu;
            div_nth = 0x7FFFFFFFu;
        } else {
            const int64_t q = div_num / b;
            if (__builtin_expect(q < std::numeric_limits<int32_t>::min() ||
                                 q > std::numeric_limits<int32_t>::max(), 0)) {
                div_ovf_hit = true;
                div_ntl = 0x7FFFFFFFu;
                div_nth = 0x7FFFFFFFu;
            } else {
                const int64_t rem = div_num - q * b;
                div_ntl = static_cast<uint32_t>(static_cast<int32_t>(q));
                div_nth = static_cast<uint32_t>(static_cast<int32_t>(rem));
            }
        }

        r11 -= 16u;
        out_off -= 16u;

        // Project previous vertex (r4, r5) * r7 (previous 1/Z) and compute clip flags
        const uint64_t dm_x = static_cast<uint64_t>(static_cast<int64_t>(to_s32(r7)) * static_cast<int64_t>(to_s32(r4)));
        const uint32_t sx = static_cast<uint32_t>(dm_x >> 32);
        const uint64_t dm_y = static_cast<uint64_t>(static_cast<int64_t>(to_s32(r7)) * static_cast<int64_t>(to_s32(r5)));
        const uint32_t sy = static_cast<uint32_t>(dm_y >> 32);

        hle_st32(hw, out_off + 12u, r3);
        hle_st32(hw, out_off + 24u, r5);

        uint32_t clip = r4 >> 1;
        const uint32_t cx_sum = gbr_w0_v + sx;
        const uint32_t cx_t = ((gbr_l0_v + cx_sum) >= r12) ? 1u : 0u;
        clip = (clip + clip) + cx_t;

        const uint32_t cy_sum = gbr_w1_v + sy;
        const uint32_t cy_t = ((gbr_l0_v + cy_sum) >= r13) ? 1u : 0u;
        clip |= cy_t;

        hle_st32(hw, out_off + 20u, clip);

        // X = row0 . vtx (computed interleaved with clip in SH-2)
        const uint64_t mac_x = static_cast<uint64_t>(m00 * vx + m01 * vy + m02 * vz);

        const uint32_t clip_masked = clip & gbr_l1_v;
        const bool clip_bail = (clip_masked & 1u) != 0u;
        if (__builtin_expect(clip_bail, 0)) {
            r0 = gbr_l1_v;
            r1 = cy_sum;
            r2 = IsSlave ? static_cast<uint32_t>(mac_z >> 32) : tx;
            r3 = sx;
            r4 = arith_shift_right1(clip_masked);
            r5 = sy;
            r6 = (r6 - 32u) + (IsSlave ? 12u : 16u);
            r10 += 12u;
            ++iters_done;
            total_steps += (IsSlave ? 52u : 53u) + (ge_near ? 0u : 1u);
            total_cyc += IsSlave ? 55ULL : 56ULL;

            cpu.r[0] = r0; cpu.r[1] = r1; cpu.r[2] = r2; cpu.r[3] = r3;
            cpu.r[4] = r4; cpu.r[5] = r5; cpu.r[6] = r6; cpu.r[7] = r7;
            cpu.r[9] = r9; cpu.r[10] = r10; cpu.r[11] = r11;
            cpu.mach = static_cast<uint32_t>(mac_x >> 32);
            cpu.macl = static_cast<uint32_t>(mac_x);
            cpu.sr |= kShSrT;
            cpu.pc = kClipBraPc;
            cpu.cur_pc = kClipCurPc;
            cpu.retired += total_steps;
            cpu.cycles += total_cyc;

            auto& dv = m.inner.divu[static_cast<size_t>(cpu_id)];
            dv.dvsr = div_dvsr;
            dv.dvdnth = div_nth;
            dv.dvdntl = div_ntl;
            if (div_ovf_hit) dv.ovf = 1u;
            dv.written = 1u;
            dv.ops += iters_done;
            m.last_write_ea = kHighWramBase + out_off + 20u;
            m.last_write_val = clip;
            return total_steps;
        }

        // Pack (sx, sy) into 32-bit word and store at @(16, r11)
        const uint32_t packed_xy = ((sx & 0xFFFFu) << 16) | (sy & 0xFFFFu);
        hle_st32(hw, out_off + 16u, packed_xy);
        last_wr_off = out_off + 16u;
        last_wr_val = packed_xy;

        // Y = row1 . vtx + ty
        const uint64_t mac_y = static_cast<uint64_t>(m10 * vx + m11 * vy + m12 * vz);
        last_mac = mac_y;

        r3 = sx;
        r4 = static_cast<uint32_t>(mac_x >> 16) + tx;
        r5 = static_cast<uint32_t>(mac_y >> 16) + ty;
        r7 = div_ntl;
        r10 -= 12u;
        vtx_off -= 12u;
        r9 -= 1u;

        ++iters_done;
        total_steps += ge_near ? 73u : 74u;
        total_cyc += (r9 != 0u) ? 77ULL : 76ULL;
    }

    // Write back exact architectural SH-2 & DIVU state after completed iterations.
    cpu.r[0] = gbr_l1_v;
    cpu.r[1] = static_cast<uint32_t>(last_mac >> 32);
    cpu.r[2] = ty;
    cpu.r[3] = r3;
    cpu.r[4] = r4;
    cpu.r[5] = r5;
    cpu.r[7] = r7;
    cpu.r[9] = r9;
    cpu.r[10] = r10;
    cpu.r[11] = r11;
    cpu.mach = static_cast<uint32_t>(last_mac >> 32);
    cpu.macl = static_cast<uint32_t>(last_mac);
    if (r9 == 0u) {
        cpu.sr |= kShSrT;
        cpu.pc = kExitPc;
    } else {
        cpu.sr &= ~kShSrT;
        cpu.pc = kLoopPc;
    }
    cpu.cur_pc = kSlotPc;
    cpu.retired += total_steps;
    cpu.cycles += total_cyc;

    auto& dv = m.inner.divu[static_cast<size_t>(cpu_id)];
    dv.dvsr = div_dvsr;
    dv.dvdnth = div_nth;
    dv.dvdntl = div_ntl;
    if (div_ovf_hit) dv.ovf = 1u;
    dv.written = 1u;
    dv.ops += iters_done;

    m.last_write_ea = kHighWramBase + last_wr_off;
    m.last_write_val = last_wr_val;
    return total_steps;
}

}  // namespace

uint32_t execute_sh2_hle_sgl(int cpu_id, Sh2CpuState& cpu, StvMachineState& m,
                             int max_steps, uint64_t cycle_limit) noexcept {
    if (cpu.pc == kHleSglSlaveVtxLoopPc) {
        return run_sgl_vtx_loop<true>(cpu_id, cpu, m, max_steps, cycle_limit);
    }
    if (cpu.pc == kHleSglMasterVtxLoopPc) {
        return run_sgl_vtx_loop<false>(cpu_id, cpu, m, max_steps, cycle_limit);
    }
    return 0u;
}

}  // namespace twc98
