// Modular Verified High-Level Emulation (HLE) for hot Sega SGL 3D math loops
// in Tecmo World Cup '98 (twcup98).
//
// Traceability:
//   - Slave SH-2 SGL vertex transform + DIVU perspective loop: 0x060631DE..0x06063274
//   - Master SH-2 SGL vertex transform + DIVU perspective loop: 0x0605CD86..0x0605CE1C
//   - Clean Classic '98 EPRG High WRAM validated via 64-byte FNV-1a32 AOT line hashes.
//
// Invariants:
//   - 100% bit-exact with SH-2 interpreter & AOT (registers r0..r15, sr, mach, macl,
//     pc, cur_pc, cycles, retired, DIVU regs, High WRAM outputs, last_write_ea/val).
//   - Zero heap allocations (-DTWC98_NO_STL compatible, portable PC/Wasm).
//   - Any unmet precondition (modified code line, unaligned/non-High-WRAM pointer,
//     buffer aliasing, S-bit saturation, insufficient step/cycle budget) returns 0
//     without mutating state so normal SH-2 AOT/interpreter executes seamlessly.
#pragma once

#include "sh2_core.hpp"

#include <cstdint>

namespace twc98 {

class StvMachineState;

constexpr uint32_t kHleSglSlaveVtxLoopPc  = 0x060631DEu;
constexpr uint32_t kHleSglMasterVtxLoopPc = 0x0605CD86u;

// Attempts to execute one or more iterations of a verified SGL 3D loop at cpu.pc.
// Returns exact SH-2 retired instructions executed (>0), or 0 if not handled.
uint32_t execute_sh2_hle_sgl(int cpu_id, Sh2CpuState& cpu, StvMachineState& m,
                             int max_steps, uint64_t cycle_limit) noexcept;

}  // namespace twc98
