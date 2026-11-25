// Shared inline SH-2 step template so generic buses and fast ST-V direct buses
// execute identical instruction semantics without virtual-call overhead.
#pragma once

#include "sh2_core.hpp"

namespace twc98 {

inline void set_sr_flag(Sh2CpuState& cpu, uint32_t bit, bool on) {
    if (on) {
        cpu.sr |= bit;
    } else {
        cpu.sr &= ~bit;
    }
}

inline uint32_t div1_step_core(uint32_t rn, uint32_t rm, uint32_t& sr) {
    const bool old_q = (sr & kShSrQ) != 0u;
    const bool m_flag = (sr & kShSrM) != 0u;
    if ((rn & 0x80000000u) != 0u) {
        sr |= kShSrQ;
    } else {
        sr &= ~kShSrQ;
    }
    rn = (rn << 1) | (sr & kShSrT);
    const bool cur_q = (sr & kShSrQ) != 0u;
    const uint32_t tmp = rn;
    bool overflow_bit = false;
    if (old_q == m_flag) {
        rn = rn - rm;
        overflow_bit = (rn > tmp);
    } else {
        rn = rn + rm;
        overflow_bit = (rn < tmp);
    }
    const bool new_q = (cur_q ^ m_flag ^ overflow_bit);
    if (new_q) {
        sr |= kShSrQ;
    } else {
        sr &= ~kShSrQ;
    }
    const uint32_t qm = sr & (kShSrQ | kShSrM);
    if (qm == 0u || qm == (kShSrQ | kShSrM)) {
        sr |= kShSrT;
    } else {
        sr &= ~kShSrT;
    }
    return rn;
}

template <typename BusT>
inline bool step_sh2_with_bus(int cpu_id, Sh2CpuState& cpu, BusT& bus, const Sh2OpClass* decode_lut,
                              BusAccessFault& fault) {
    if (__builtin_expect(!fault.ok, 0)) fault = BusAccessFault{};
    const uint32_t instr_pc = cpu.pc;
    const uint32_t op_word = bus.fast_read(cpu_id, instr_pc, 2, instr_pc, cpu.cycles, fault);
    if (!fault.ok) {
        return false;
    }
    const uint16_t op = static_cast<uint16_t>(op_word & 0xFFFFu);
    const Sh2OpClass cls = decode_lut[op];
    if (cls == Sh2OpClass::INVALID) {
        fault = BusAccessFault{};
        fault.ok = false;
        fault.is_unknown_opcode = true;
        fault.pc = instr_pc;
        fault.opcode = op;
        fault.reason = "UNKNOWN_SH2_OPCODE";
        return false;
    }

    const uint32_t saved_pc = cpu.pc;
    const uint32_t saved_delay = cpu.delay;
    uint32_t rollback_sr = cpu.sr;
    uint32_t rollback_rn_delta = 0u;
    const uint32_t pending_target = cpu.delay;
    cpu.delay = 0u;
    cpu.pc = (pending_target != 0u) ? pending_target : (instr_pc + 2u);

    const uint32_t n = (op >> 8) & 0xFu;
    const uint32_t m = (op >> 4) & 0xFu;
    const uint32_t d4 = op & 0xFu;
    const uint32_t d8 = op & 0xFFu;
    const uint32_t d12 = op & 0xFFFu;

    auto read_checked = [&](uint32_t ea, uint8_t width) -> uint32_t {
        return bus.fast_read(cpu_id, ea, width, instr_pc, cpu.cycles, fault);
    };
    auto write_checked = [&](uint32_t ea, uint8_t width, uint32_t val) {
        bus.fast_write(cpu_id, ea, width, val, instr_pc, cpu.cycles, fault);
    };

    uint64_t step_cycles = 1u;
    switch (cls) {
        case Sh2OpClass::NOP: break;
        case Sh2OpClass::CLRT: cpu.sr &= ~kShSrT; break;
        case Sh2OpClass::SETT: cpu.sr |= kShSrT; break;
        case Sh2OpClass::CLRMAC: cpu.mach = 0u; cpu.macl = 0u; break;
        case Sh2OpClass::STCSR: cpu.r[n] = cpu.sr & 0x3F3u; break;
        case Sh2OpClass::STCSGR: cpu.r[n] = cpu.gbr; break;
        case Sh2OpClass::STCVBR: cpu.r[n] = cpu.vbr; break;
        case Sh2OpClass::LDCSR: cpu.sr = cpu.r[n] & 0x3F3u; break;
        case Sh2OpClass::LDCGBR: cpu.gbr = cpu.r[n]; break;
        case Sh2OpClass::LDCVBR: cpu.vbr = cpu.r[n]; break;
        case Sh2OpClass::STSMACH: cpu.r[n] = cpu.mach; break;
        case Sh2OpClass::STSMACL: cpu.r[n] = cpu.macl; break;
        case Sh2OpClass::STSPR: cpu.r[n] = cpu.pr; break;
        case Sh2OpClass::LDSMACH: cpu.mach = cpu.r[n]; break;
        case Sh2OpClass::LDSMACL: cpu.macl = cpu.r[n]; break;
        case Sh2OpClass::LDSPR: cpu.pr = cpu.r[n]; break;

        case Sh2OpClass::STSMMACH:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.mach);
            break;
        case Sh2OpClass::STSMMACL:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.macl);
            break;
        case Sh2OpClass::STSMPR:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.pr);
            break;
        case Sh2OpClass::LDSMMACH: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.mach = v; cpu.r[n] += 4u; }
            break;
        }
        case Sh2OpClass::LDSMMACL: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.macl = v; cpu.r[n] += 4u; }
            break;
        }
        case Sh2OpClass::LDSMPR: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.pr = v; cpu.r[n] += 4u; }
            break;
        }
        case Sh2OpClass::STCMSR:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.sr & 0x3F3u);
            step_cycles = 2u;
            break;
        case Sh2OpClass::STCMGBR:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.gbr);
            step_cycles = 2u;
            break;
        case Sh2OpClass::STCMVBR:
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, cpu.vbr);
            step_cycles = 2u;
            break;
        case Sh2OpClass::LDCMSR: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.sr = v & 0x3F3u; cpu.r[n] += 4u; }
            step_cycles = 3u;
            break;
        }
        case Sh2OpClass::LDCMGBR: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.gbr = v; cpu.r[n] += 4u; }
            step_cycles = 3u;
            break;
        }
        case Sh2OpClass::LDCMVBR: {
            const uint32_t v = read_checked(cpu.r[n], 4);
            if (fault.ok) { cpu.vbr = v; cpu.r[n] += 4u; }
            step_cycles = 3u;
            break;
        }

        case Sh2OpClass::RTS: cpu.delay = cpu.pr; step_cycles = 2u; break;
        case Sh2OpClass::JMP: cpu.delay = cpu.r[n]; step_cycles = 2u; break;
        case Sh2OpClass::JSR:
            cpu.pr = cpu.pc + 2u;
            cpu.delay = cpu.r[n];
            step_cycles = 2u;
            break;
        case Sh2OpClass::BRA:
            cpu.delay = cpu.pc + (sign_extend_12(d12) << 1) + 2u;
            step_cycles = 2u;
            break;
        case Sh2OpClass::BRAF:
            cpu.delay = cpu.pc + cpu.r[n] + 2u;
            step_cycles = 2u;
            break;
        case Sh2OpClass::BSR:
            cpu.pr = cpu.pc + 2u;
            cpu.delay = cpu.pc + (sign_extend_12(d12) << 1) + 2u;
            step_cycles = 2u;
            break;
        case Sh2OpClass::BSRF:
            cpu.pr = cpu.pc + 2u;
            cpu.delay = cpu.pc + cpu.r[n] + 2u;
            step_cycles = 2u;
            break;
        case Sh2OpClass::BT:
            if ((cpu.sr & kShSrT) != 0u) {
                cpu.pc = cpu.pc + (sign_extend_8(d8) << 1) + 2u;
                step_cycles = 3u;
            }
            break;
        case Sh2OpClass::BF:
            if ((cpu.sr & kShSrT) == 0u) {
                cpu.pc = cpu.pc + (sign_extend_8(d8) << 1) + 2u;
                step_cycles = 3u;
            }
            break;
        case Sh2OpClass::BTS:
            if ((cpu.sr & kShSrT) != 0u) {
                cpu.delay = cpu.pc + (sign_extend_8(d8) << 1) + 2u;
                step_cycles = 2u;
            }
            break;
        case Sh2OpClass::BFS:
            if ((cpu.sr & kShSrT) == 0u) {
                cpu.delay = cpu.pc + (sign_extend_8(d8) << 1) + 2u;
                step_cycles = 2u;
            }
            break;
        case Sh2OpClass::RTE: {
            const uint32_t new_pc = read_checked(cpu.r[15], 4);
            if (!fault.ok) break;
            const uint32_t new_sr = read_checked(cpu.r[15] + 4u, 4);
            if (!fault.ok) break;
            cpu.r[15] += 8u;
            cpu.sr = new_sr & 0x3F3u;
            cpu.delay = new_pc;
            step_cycles = 4u;
            break;
        }

        case Sh2OpClass::MOV: cpu.r[n] = cpu.r[m]; break;
        case Sh2OpClass::MOVI: cpu.r[n] = sign_extend_8(d8); break;
        case Sh2OpClass::MOVWI: {
            const uint32_t v = read_checked(cpu.pc + (d8 << 1) + 2u, 2);
            if (fault.ok) cpu.r[n] = sign_extend_16(v);
            break;
        }
        case Sh2OpClass::MOVLI: {
            const uint32_t v = read_checked(((cpu.pc + 2u) & ~3u) + (d8 << 2), 4);
            if (fault.ok) cpu.r[n] = v;
            break;
        }
        case Sh2OpClass::MOVA:
            cpu.r[0] = ((cpu.pc + 2u) & ~3u) + (d8 << 2);
            break;
        case Sh2OpClass::MOVT:
            cpu.r[n] = cpu.sr & kShSrT;
            break;

        case Sh2OpClass::MOVBS: write_checked(cpu.r[n], 1, cpu.r[m] & 0xFFu); break;
        case Sh2OpClass::MOVWS: write_checked(cpu.r[n], 2, cpu.r[m] & 0xFFFFu); break;
        case Sh2OpClass::MOVLS: write_checked(cpu.r[n], 4, cpu.r[m]); break;
        case Sh2OpClass::MOVBL: {
            const uint32_t v = read_checked(cpu.r[m], 1);
            if (fault.ok) cpu.r[n] = sign_extend_8(v);
            break;
        }
        case Sh2OpClass::MOVWL: {
            const uint32_t v = read_checked(cpu.r[m], 2);
            if (fault.ok) cpu.r[n] = sign_extend_16(v);
            break;
        }
        case Sh2OpClass::MOVLL: {
            const uint32_t v = read_checked(cpu.r[m], 4);
            if (fault.ok) cpu.r[n] = v;
            break;
        }

        case Sh2OpClass::MOVBM: {
            const uint32_t data = cpu.r[m] & 0xFFu;
            cpu.r[n] -= 1u;
            rollback_rn_delta = 1u;
            write_checked(cpu.r[n], 1, data);
            break;
        }
        case Sh2OpClass::MOVWM: {
            const uint32_t data = cpu.r[m] & 0xFFFFu;
            cpu.r[n] -= 2u;
            rollback_rn_delta = 2u;
            write_checked(cpu.r[n], 2, data);
            break;
        }
        case Sh2OpClass::MOVLM: {
            const uint32_t data = cpu.r[m];
            cpu.r[n] -= 4u;
            rollback_rn_delta = 4u;
            write_checked(cpu.r[n], 4, data);
            break;
        }
        case Sh2OpClass::MOVBP: {
            const uint32_t v = read_checked(cpu.r[m], 1);
            if (fault.ok) {
                cpu.r[n] = sign_extend_8(v);
                if (n != m) cpu.r[m] += 1u;
            }
            break;
        }
        case Sh2OpClass::MOVWP: {
            const uint32_t v = read_checked(cpu.r[m], 2);
            if (fault.ok) {
                cpu.r[n] = sign_extend_16(v);
                if (n != m) cpu.r[m] += 2u;
            }
            break;
        }
        case Sh2OpClass::MOVLP: {
            const uint32_t v = read_checked(cpu.r[m], 4);
            if (fault.ok) {
                cpu.r[n] = v;
                if (n != m) cpu.r[m] += 4u;
            }
            break;
        }

        case Sh2OpClass::MOVBS0: write_checked(cpu.r[n] + cpu.r[0], 1, cpu.r[m] & 0xFFu); break;
        case Sh2OpClass::MOVWS0: write_checked(cpu.r[n] + cpu.r[0], 2, cpu.r[m] & 0xFFFFu); break;
        case Sh2OpClass::MOVLS0: write_checked(cpu.r[n] + cpu.r[0], 4, cpu.r[m]); break;
        case Sh2OpClass::MOVBL0: {
            const uint32_t v = read_checked(cpu.r[m] + cpu.r[0], 1);
            if (fault.ok) cpu.r[n] = sign_extend_8(v);
            break;
        }
        case Sh2OpClass::MOVWL0: {
            const uint32_t v = read_checked(cpu.r[m] + cpu.r[0], 2);
            if (fault.ok) cpu.r[n] = sign_extend_16(v);
            break;
        }
        case Sh2OpClass::MOVLL0: {
            const uint32_t v = read_checked(cpu.r[m] + cpu.r[0], 4);
            if (fault.ok) cpu.r[n] = v;
            break;
        }

        case Sh2OpClass::MOVLS4: write_checked(cpu.r[n] + (d4 << 2), 4, cpu.r[m]); break;
        case Sh2OpClass::MOVLL4: {
            const uint32_t v = read_checked(cpu.r[m] + (d4 << 2), 4);
            if (fault.ok) cpu.r[n] = v;
            break;
        }
        case Sh2OpClass::MOVBS4: write_checked(cpu.r[m] + d4, 1, cpu.r[0] & 0xFFu); break;
        case Sh2OpClass::MOVWS4: write_checked(cpu.r[m] + (d4 << 1), 2, cpu.r[0] & 0xFFFFu); break;
        case Sh2OpClass::MOVBL4: {
            const uint32_t v = read_checked(cpu.r[m] + d4, 1);
            if (fault.ok) cpu.r[0] = sign_extend_8(v);
            break;
        }
        case Sh2OpClass::MOVWL4: {
            const uint32_t v = read_checked(cpu.r[m] + (d4 << 1), 2);
            if (fault.ok) cpu.r[0] = sign_extend_16(v);
            break;
        }

        case Sh2OpClass::MOVBSG: write_checked(cpu.gbr + d8, 1, cpu.r[0] & 0xFFu); break;
        case Sh2OpClass::MOVWSG: write_checked(cpu.gbr + (d8 << 1), 2, cpu.r[0] & 0xFFFFu); break;
        case Sh2OpClass::MOVLSG: write_checked(cpu.gbr + (d8 << 2), 4, cpu.r[0]); break;
        case Sh2OpClass::MOVBLG: {
            const uint32_t v = read_checked(cpu.gbr + d8, 1);
            if (fault.ok) cpu.r[0] = sign_extend_8(v);
            break;
        }
        case Sh2OpClass::MOVWLG: {
            const uint32_t v = read_checked(cpu.gbr + (d8 << 1), 2);
            if (fault.ok) cpu.r[0] = sign_extend_16(v);
            break;
        }
        case Sh2OpClass::MOVLLG: {
            const uint32_t v = read_checked(cpu.gbr + (d8 << 2), 4);
            if (fault.ok) cpu.r[0] = v;
            break;
        }

        case Sh2OpClass::ADD: cpu.r[n] += cpu.r[m]; break;
        case Sh2OpClass::ADDI: cpu.r[n] += sign_extend_8(d8); break;
        case Sh2OpClass::ADDC: {
            const uint32_t tmp0 = cpu.r[n];
            const uint32_t tmp1 = tmp0 + cpu.r[m];
            const uint32_t res = tmp1 + (cpu.sr & kShSrT);
            cpu.r[n] = res;
            set_sr_flag(cpu, kShSrT, (tmp0 > tmp1) || (tmp1 > res));
            break;
        }
        case Sh2OpClass::ADDV: {
            const uint32_t a = cpu.r[n];
            const uint32_t b = cpu.r[m];
            const uint32_t res = a + b;
            cpu.r[n] = res;
            const bool overflow = (((a ^ b) & 0x80000000u) == 0u) && (((a ^ res) & 0x80000000u) != 0u);
            set_sr_flag(cpu, kShSrT, overflow);
            break;
        }
        case Sh2OpClass::SUB: cpu.r[n] -= cpu.r[m]; break;
        case Sh2OpClass::SUBC: {
            const uint32_t tmp0 = cpu.r[n];
            const uint32_t tmp1 = tmp0 - cpu.r[m];
            const uint32_t res = tmp1 - (cpu.sr & kShSrT);
            cpu.r[n] = res;
            set_sr_flag(cpu, kShSrT, (tmp0 < tmp1) || (tmp1 < res));
            break;
        }
        case Sh2OpClass::SUBV: {
            const uint32_t a = cpu.r[n];
            const uint32_t b = cpu.r[m];
            const uint32_t res = a - b;
            cpu.r[n] = res;
            const bool overflow = (((a ^ b) & 0x80000000u) != 0u) && (((a ^ res) & 0x80000000u) != 0u);
            set_sr_flag(cpu, kShSrT, overflow);
            break;
        }
        case Sh2OpClass::NEG: cpu.r[n] = 0u - cpu.r[m]; break;
        case Sh2OpClass::NEGC: {
            const uint32_t temp = 0u - cpu.r[m];
            const uint32_t res = temp - (cpu.sr & kShSrT);
            cpu.r[n] = res;
            set_sr_flag(cpu, kShSrT, (0u < temp) || (temp < res));
            break;
        }

        case Sh2OpClass::AND: cpu.r[n] &= cpu.r[m]; break;
        case Sh2OpClass::ANDI: cpu.r[0] &= d8; break;
        case Sh2OpClass::ANDM: {
            const uint32_t ea = cpu.gbr + cpu.r[0];
            const uint32_t v = read_checked(ea, 1);
            if (!fault.ok) break;
            write_checked(ea, 1, v & d8);
            break;
        }
        case Sh2OpClass::OR: cpu.r[n] |= cpu.r[m]; break;
        case Sh2OpClass::ORI: cpu.r[0] |= d8; break;
        case Sh2OpClass::ORM: {
            const uint32_t ea = cpu.gbr + cpu.r[0];
            const uint32_t v = read_checked(ea, 1);
            if (!fault.ok) break;
            write_checked(ea, 1, v | d8);
            break;
        }
        case Sh2OpClass::XOR: cpu.r[n] ^= cpu.r[m]; break;
        case Sh2OpClass::XORI: cpu.r[0] ^= d8; break;
        case Sh2OpClass::XORM: {
            const uint32_t ea = cpu.gbr + cpu.r[0];
            const uint32_t v = read_checked(ea, 1);
            if (!fault.ok) break;
            write_checked(ea, 1, v ^ d8);
            break;
        }
        case Sh2OpClass::NOT: cpu.r[n] = ~cpu.r[m]; break;

        case Sh2OpClass::TST: set_sr_flag(cpu, kShSrT, (cpu.r[n] & cpu.r[m]) == 0u); break;
        case Sh2OpClass::TSTI: set_sr_flag(cpu, kShSrT, (d8 & cpu.r[0]) == 0u); break;
        case Sh2OpClass::TSTM: {
            const uint32_t ea = cpu.gbr + cpu.r[0];
            const uint32_t v = read_checked(ea, 1);
            if (!fault.ok) break;
            set_sr_flag(cpu, kShSrT, (d8 & v) == 0u);
            break;
        }
        case Sh2OpClass::CMPEQ: set_sr_flag(cpu, kShSrT, cpu.r[n] == cpu.r[m]); break;
        case Sh2OpClass::CMPGE: set_sr_flag(cpu, kShSrT, to_s32(cpu.r[n]) >= to_s32(cpu.r[m])); break;
        case Sh2OpClass::CMPGT: set_sr_flag(cpu, kShSrT, to_s32(cpu.r[n]) > to_s32(cpu.r[m])); break;
        case Sh2OpClass::CMPHI: set_sr_flag(cpu, kShSrT, cpu.r[n] > cpu.r[m]); break;
        case Sh2OpClass::CMPHS: set_sr_flag(cpu, kShSrT, cpu.r[n] >= cpu.r[m]); break;
        case Sh2OpClass::CMPIM: set_sr_flag(cpu, kShSrT, to_s32(cpu.r[0]) == to_s32(sign_extend_8(d8))); break;
        case Sh2OpClass::CMPPL: set_sr_flag(cpu, kShSrT, to_s32(cpu.r[n]) > 0); break;
        case Sh2OpClass::CMPPZ: set_sr_flag(cpu, kShSrT, to_s32(cpu.r[n]) >= 0); break;
        case Sh2OpClass::DT:
            cpu.r[n] -= 1u;
            set_sr_flag(cpu, kShSrT, cpu.r[n] == 0u);
            break;

        case Sh2OpClass::EXTSB: cpu.r[n] = sign_extend_8(cpu.r[m]); break;
        case Sh2OpClass::EXTSW: cpu.r[n] = sign_extend_16(cpu.r[m]); break;
        case Sh2OpClass::EXTUB: cpu.r[n] = cpu.r[m] & 0xFFu; break;
        case Sh2OpClass::EXTUW: cpu.r[n] = cpu.r[m] & 0xFFFFu; break;
        case Sh2OpClass::SWAPB: {
            const uint32_t v = cpu.r[m];
            cpu.r[n] = (v & 0xFFFF0000u) | ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu);
            break;
        }
        case Sh2OpClass::SWAPW: {
            const uint32_t v = cpu.r[m];
            cpu.r[n] = ((v & 0xFFFFu) << 16) | ((v >> 16) & 0xFFFFu);
            break;
        }
        case Sh2OpClass::XTRCT:
            cpu.r[n] = ((cpu.r[m] & 0xFFFFu) << 16) | ((cpu.r[n] >> 16) & 0xFFFFu);
            break;

        case Sh2OpClass::SHLL:
        case Sh2OpClass::SHAL:
            set_sr_flag(cpu, kShSrT, ((cpu.r[n] >> 31) & 1u) != 0u);
            cpu.r[n] <<= 1;
            break;
        case Sh2OpClass::SHLR:
            set_sr_flag(cpu, kShSrT, (cpu.r[n] & 1u) != 0u);
            cpu.r[n] >>= 1;
            break;
        case Sh2OpClass::SHAR:
            set_sr_flag(cpu, kShSrT, (cpu.r[n] & 1u) != 0u);
            cpu.r[n] = arith_shift_right1(cpu.r[n]);
            break;
        case Sh2OpClass::ROTL: {
            const uint32_t bit31 = (cpu.r[n] >> 31) & 1u;
            set_sr_flag(cpu, kShSrT, bit31 != 0u);
            cpu.r[n] = (cpu.r[n] << 1) | bit31;
            break;
        }
        case Sh2OpClass::ROTR: {
            const uint32_t bit0 = cpu.r[n] & 1u;
            set_sr_flag(cpu, kShSrT, bit0 != 0u);
            cpu.r[n] = (cpu.r[n] >> 1) | (bit0 << 31);
            break;
        }
        case Sh2OpClass::ROTCL: {
            const uint32_t bit31 = (cpu.r[n] >> 31) & 1u;
            cpu.r[n] = (cpu.r[n] << 1) | (cpu.sr & kShSrT);
            set_sr_flag(cpu, kShSrT, bit31 != 0u);
            break;
        }
        case Sh2OpClass::ROTCR: {
            const uint32_t carry_in = (cpu.sr & kShSrT) << 31;
            set_sr_flag(cpu, kShSrT, (cpu.r[n] & 1u) != 0u);
            cpu.r[n] = (cpu.r[n] >> 1) | carry_in;
            break;
        }
        case Sh2OpClass::SHLL2: cpu.r[n] <<= 2; break;
        case Sh2OpClass::SHLR2: cpu.r[n] >>= 2; break;
        case Sh2OpClass::SHLL8: cpu.r[n] <<= 8; break;
        case Sh2OpClass::SHLR8: cpu.r[n] >>= 8; break;
        case Sh2OpClass::SHLL16: cpu.r[n] <<= 16; break;
        case Sh2OpClass::SHLR16: cpu.r[n] >>= 16; break;

        case Sh2OpClass::MULL: cpu.macl = cpu.r[n] * cpu.r[m]; break;
        case Sh2OpClass::MULS: {
            const int32_t a = to_s32(sign_extend_16(cpu.r[n]));
            const int32_t b = to_s32(sign_extend_16(cpu.r[m]));
            cpu.macl = static_cast<uint32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
            break;
        }
        case Sh2OpClass::MULU:
            cpu.macl = (cpu.r[n] & 0xFFFFu) * (cpu.r[m] & 0xFFFFu);
            break;
        case Sh2OpClass::DMULS: {
            const int64_t a = to_s32(cpu.r[n]);
            const int64_t b = to_s32(cpu.r[m]);
            const uint64_t prod = static_cast<uint64_t>(a * b);
            cpu.mach = static_cast<uint32_t>(prod >> 32);
            cpu.macl = static_cast<uint32_t>(prod & 0xFFFFFFFFu);
            break;
        }
        case Sh2OpClass::DMULU: {
            const uint64_t prod = static_cast<uint64_t>(cpu.r[n]) * static_cast<uint64_t>(cpu.r[m]);
            cpu.mach = static_cast<uint32_t>(prod >> 32);
            cpu.macl = static_cast<uint32_t>(prod & 0xFFFFFFFFu);
            break;
        }
        case Sh2OpClass::MAC_L: {
            const uint32_t raw_n = read_checked(cpu.r[n], 4);
            if (!fault.ok) break;
            cpu.r[n] += 4u;
            const uint32_t raw_m = read_checked(cpu.r[m], 4);
            if (!fault.ok) {
                cpu.r[n] -= 4u;
                break;
            }
            cpu.r[m] += 4u;
            const int64_t prod = static_cast<int64_t>(to_s32(raw_n)) * static_cast<int64_t>(to_s32(raw_m));
            if ((cpu.sr & kShSrS) != 0u) {
                const int64_t mach_sext = to_s32(sign_extend_16(cpu.mach));
                const int64_t acc = (mach_sext * (1LL << 32)) + static_cast<int64_t>(cpu.macl);
                const int64_t tot = acc + prod;
                constexpr int64_t kSatMin = -(1LL << 47);
                constexpr int64_t kSatMax = (1LL << 47) - 1LL;
                if (tot < kSatMin) {
                    cpu.mach = 0x00008000u;
                    cpu.macl = 0x00000000u;
                } else if (tot > kSatMax) {
                    cpu.mach = 0x00007FFFu;
                    cpu.macl = 0xFFFFFFFFu;
                } else {
                    const uint64_t u = static_cast<uint64_t>(tot);
                    cpu.mach = static_cast<uint32_t>(u >> 32);
                    cpu.macl = static_cast<uint32_t>(u & 0xFFFFFFFFu);
                }
            } else {
                const uint64_t acc = (static_cast<uint64_t>(cpu.mach) << 32) | cpu.macl;
                const uint64_t tot = acc + static_cast<uint64_t>(prod);
                cpu.mach = static_cast<uint32_t>(tot >> 32);
                cpu.macl = static_cast<uint32_t>(tot & 0xFFFFFFFFu);
            }
            break;
        }

        case Sh2OpClass::DIV0S:
            set_sr_flag(cpu, kShSrQ, (cpu.r[n] & 0x80000000u) != 0u);
            set_sr_flag(cpu, kShSrM, (cpu.r[m] & 0x80000000u) != 0u);
            set_sr_flag(cpu, kShSrT, ((cpu.r[m] ^ cpu.r[n]) & 0x80000000u) != 0u);
            break;
        case Sh2OpClass::DIV0U:
            cpu.sr &= ~(kShSrQ | kShSrM | kShSrT);
            break;
        case Sh2OpClass::DIV1:
            cpu.r[n] = div1_step_core(cpu.r[n], cpu.r[m], cpu.sr);
            break;
        case Sh2OpClass::TAS: {
            const uint32_t v = read_checked(cpu.r[n], 1);
            if (!fault.ok) break;
            rollback_sr = cpu.sr;
            set_sr_flag(cpu, kShSrT, (v & 0xFFu) == 0u);
            write_checked(cpu.r[n], 1, v | 0x80u);
            step_cycles = 4u;
            break;
        }

        case Sh2OpClass::INVALID:
            fault.ok = false;
            fault.is_unknown_opcode = true;
            break;
    }

    if (!fault.ok) {
        cpu.pc = saved_pc;
        cpu.delay = saved_delay;
        cpu.sr = rollback_sr;
        cpu.r[n] += rollback_rn_delta;
        return false;
    }
    cpu.cur_pc = instr_pc;
    cpu.retired += 1u;
    cpu.cycles += step_cycles;
    return true;
}

}  // namespace twc98
