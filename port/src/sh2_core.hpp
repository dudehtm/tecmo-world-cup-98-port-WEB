// Portable SH-2 core for TWC98BO G2 reconstruction.
// Reference semantics: MAME src/devices/cpu/sh/sh.cpp & Renesas SH-2 manual.
#pragma once

#include <cstdint>

namespace twc98 {

inline constexpr uint32_t kShSrT = 0x00000001u;
inline constexpr uint32_t kShSrS = 0x00000002u;
inline constexpr uint32_t kShSrImaskMask = 0x000000F0u;
inline constexpr uint32_t kShSrQ = 0x00000100u;
inline constexpr uint32_t kShSrM = 0x00000200u;

enum class Sh2OpClass : uint8_t {
    INVALID = 0,
    NOP, CLRT, SETT, CLRMAC,
    STSMACH, STSMACL, STSPR,
    STCSGR, STCVBR, STCSR,
    LDCSR, LDCGBR, LDCVBR,
    LDSMACH, LDSMACL, LDSPR,
    STSMMACH, STSMMACL, STSMPR,
    LDSMMACH, LDSMMACL, LDSMPR,
    STCMSR, STCMGBR, STCMVBR,
    LDCMSR, LDCMGBR, LDCMVBR,
    RTS, JMP, JSR, BRA, BRAF, BSR, BSRF, BT, BF, BTS, BFS, RTE,
    MOV, MOVI, MOVWI, MOVLI, MOVA, MOVT,
    MOVBS, MOVWS, MOVLS,
    MOVBL, MOVWL, MOVLL,
    MOVBM, MOVWM, MOVLM,
    MOVBP, MOVWP, MOVLP,
    MOVBS0, MOVWS0, MOVLS0,
    MOVBL0, MOVWL0, MOVLL0,
    MOVLS4, MOVLL4, MOVBS4, MOVWS4, MOVBL4, MOVWL4,
    MOVBSG, MOVWSG, MOVLSG, MOVBLG, MOVWLG, MOVLLG,
    ADD, ADDI, ADDC, ADDV,
    SUB, SUBC, SUBV,
    NEG, NEGC,
    AND, ANDI, ANDM, OR, ORI, ORM, XOR, XORI, XORM, NOT,
    TST, TSTI, TSTM, CMPEQ, CMPGE, CMPGT, CMPHI, CMPHS, CMPIM, CMPPL, CMPPZ, DT,
    EXTSB, EXTSW, EXTUB, EXTUW,
    SWAPB, SWAPW, XTRCT,
    SHLL, SHLR, SHAL, SHAR, ROTL, ROTR, ROTCL, ROTCR,
    SHLL2, SHLR2, SHLL8, SHLR8, SHLL16, SHLR16,
    MULL, MULS, MULU, DMULS, DMULU, MAC_L,
    DIV0S, DIV0U, DIV1, TAS
};

struct Sh2CpuState {
    uint32_t r[16] = {};
    uint32_t pc = 0;
    uint32_t pr = 0;
    uint32_t gbr = 0;
    uint32_t vbr = 0;
    uint32_t mach = 0;
    uint32_t macl = 0;
    uint32_t sr = 0;
    uint32_t delay = 0;
    uint32_t cur_pc = 0;
    uint32_t imask_meta = 0;
    uint32_t context_loaded = 0;
    uint64_t retired = 0;
    uint64_t cycles = 0;
};

struct BusAccessFault {
    bool ok = true;
    bool is_write = false;
    bool is_unknown_opcode = false;
    uint8_t width = 0;
    uint32_t address = 0;
    uint32_t pc = 0;
    uint16_t opcode = 0;
    const char* reason = nullptr;
};

class Sh2MemoryBus {
public:
    virtual ~Sh2MemoryBus() = default;
    virtual uint32_t read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                          uint64_t cpu_cycles, BusAccessFault& fault) = 0;
    virtual void write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
                       uint64_t cpu_cycles, BusAccessFault& fault) = 0;
};

inline int32_t to_s32(uint32_t v) {
    return static_cast<int32_t>(v);
}

inline int64_t to_s64(uint64_t v) {
    return static_cast<int64_t>(v);
}

inline uint32_t sign_extend_8(uint32_t v) {
    const uint32_t x = v & 0xFFu;
    return (x & 0x80u) ? (x | 0xFFFFFF00u) : x;
}

inline uint32_t sign_extend_12(uint32_t v) {
    const uint32_t x = v & 0xFFFu;
    return (x & 0x800u) ? (x | 0xFFFFF000u) : x;
}

inline uint32_t sign_extend_16(uint32_t v) {
    const uint32_t x = v & 0xFFFFu;
    return (x & 0x8000u) ? (x | 0xFFFF0000u) : x;
}

inline uint32_t arith_shift_right1(uint32_t v) {
    return (v >> 1) | (v & 0x80000000u);
}

Sh2OpClass decode_sh2_class(uint16_t op);
const Sh2OpClass* sh2_decode_lut();

// Execute one SH-2 instruction at `cpu.pc`. On fault, stops cleanly without incrementing `retired`.
bool step_sh2(int cpu_id, Sh2CpuState& cpu, Sh2MemoryBus& bus, BusAccessFault& fault);

}  // namespace twc98
