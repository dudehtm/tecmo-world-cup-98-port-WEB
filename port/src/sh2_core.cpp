// Portable SH-2 instruction decoder and executor for TWC98BO G2 reconstruction.
#include "sh2_core.hpp"
#include "sh2_step_inl.hpp"

namespace twc98 {

Sh2OpClass decode_sh2_class(uint16_t op) {
    const uint32_t t = (op >> 12) & 0xFu;
    const uint32_t n = (op >> 8) & 0xFu;
    const uint32_t lo1 = op & 0xFu;
    const uint32_t lo2 = op & 0xFFu;
    switch (t) {
        case 0x0u: {
            switch (op & 0x3Fu) {
                case 0x02u: return Sh2OpClass::STCSR;
                case 0x08u: return Sh2OpClass::CLRT;
                case 0x09u: return Sh2OpClass::NOP;
                case 0x0Au: return Sh2OpClass::STSMACH;
                case 0x0Bu: return Sh2OpClass::RTS;
                case 0x12u: return Sh2OpClass::STCSGR;
                case 0x18u: return Sh2OpClass::SETT;
                case 0x19u: return Sh2OpClass::DIV0U;
                case 0x1Au: return Sh2OpClass::STSMACL;
                case 0x1Bu: return Sh2OpClass::NOP;  // SLEEP (BIOS idle before NMI)
                case 0x22u: return Sh2OpClass::STCVBR;
                case 0x23u: return Sh2OpClass::BRAF;
                case 0x28u: return Sh2OpClass::CLRMAC;
                case 0x29u: return Sh2OpClass::MOVT;
                case 0x2Au: return Sh2OpClass::STSPR;
                case 0x2Bu: return Sh2OpClass::RTE;
                default: break;
            }
            switch (lo1) {
                case 0x3u: return (op & 0xFFu) == 0x03u ? Sh2OpClass::BSRF : Sh2OpClass::INVALID;
                case 0x4u: return Sh2OpClass::MOVBS0;
                case 0x5u: return Sh2OpClass::MOVWS0;
                case 0x6u: return Sh2OpClass::MOVLS0;
                case 0x7u: return Sh2OpClass::MULL;
                case 0xCu: return Sh2OpClass::MOVBL0;
                case 0xDu: return Sh2OpClass::MOVWL0;
                case 0xEu: return Sh2OpClass::MOVLL0;
                case 0xFu: return Sh2OpClass::MAC_L;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x1u: return Sh2OpClass::MOVLS4;
        case 0x2u: {
            switch (lo1) {
                case 0x0u: return Sh2OpClass::MOVBS;
                case 0x1u: return Sh2OpClass::MOVWS;
                case 0x2u: return Sh2OpClass::MOVLS;
                case 0x4u: return Sh2OpClass::MOVBM;
                case 0x5u: return Sh2OpClass::MOVWM;
                case 0x6u: return Sh2OpClass::MOVLM;
                case 0x7u: return Sh2OpClass::DIV0S;
                case 0x8u: return Sh2OpClass::TST;
                case 0x9u: return Sh2OpClass::AND;
                case 0xAu: return Sh2OpClass::XOR;
                case 0xBu: return Sh2OpClass::OR;
                case 0xDu: return Sh2OpClass::XTRCT;
                case 0xEu: return Sh2OpClass::MULU;
                case 0xFu: return Sh2OpClass::MULS;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x3u: {
            switch (lo1) {
                case 0x0u: return Sh2OpClass::CMPEQ;
                case 0x2u: return Sh2OpClass::CMPHS;
                case 0x3u: return Sh2OpClass::CMPGE;
                case 0x4u: return Sh2OpClass::DIV1;
                case 0x5u: return Sh2OpClass::DMULU;
                case 0x6u: return Sh2OpClass::CMPHI;
                case 0x7u: return Sh2OpClass::CMPGT;
                case 0x8u: return Sh2OpClass::SUB;
                case 0xAu: return Sh2OpClass::SUBC;
                case 0xBu: return Sh2OpClass::SUBV;
                case 0xCu: return Sh2OpClass::ADD;
                case 0xDu: return Sh2OpClass::DMULS;
                case 0xEu: return Sh2OpClass::ADDC;
                case 0xFu: return Sh2OpClass::ADDV;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x4u: {
            switch (lo2 & 0x3Fu) {
                case 0x00u: return Sh2OpClass::SHLL;
                case 0x01u: return Sh2OpClass::SHLR;
                case 0x02u: return Sh2OpClass::STSMMACH;
                case 0x03u: return Sh2OpClass::STCMSR;
                case 0x04u: return Sh2OpClass::ROTL;
                case 0x05u: return Sh2OpClass::ROTR;
                case 0x06u: return Sh2OpClass::LDSMMACH;
                case 0x07u: return Sh2OpClass::LDCMSR;
                case 0x08u: return Sh2OpClass::SHLL2;
                case 0x09u: return Sh2OpClass::SHLR2;
                case 0x0Au: return Sh2OpClass::LDSMACH;
                case 0x0Bu: return Sh2OpClass::JSR;
                case 0x0Eu: return Sh2OpClass::LDCSR;
                case 0x10u: return Sh2OpClass::DT;
                case 0x11u: return Sh2OpClass::CMPPZ;
                case 0x12u: return Sh2OpClass::STSMMACL;
                case 0x13u: return Sh2OpClass::STCMGBR;
                case 0x15u: return Sh2OpClass::CMPPL;
                case 0x16u: return Sh2OpClass::LDSMMACL;
                case 0x17u: return Sh2OpClass::LDCMGBR;
                case 0x18u: return Sh2OpClass::SHLL8;
                case 0x19u: return Sh2OpClass::SHLR8;
                case 0x1Au: return Sh2OpClass::LDSMACL;
                case 0x1Bu: return Sh2OpClass::TAS;
                case 0x1Eu: return Sh2OpClass::LDCGBR;
                case 0x20u: return Sh2OpClass::SHAL;
                case 0x21u: return Sh2OpClass::SHAR;
                case 0x22u: return Sh2OpClass::STSMPR;
                case 0x23u: return Sh2OpClass::STCMVBR;
                case 0x24u: return Sh2OpClass::ROTCL;
                case 0x25u: return Sh2OpClass::ROTCR;
                case 0x26u: return Sh2OpClass::LDSMPR;
                case 0x27u: return Sh2OpClass::LDCMVBR;
                case 0x28u: return Sh2OpClass::SHLL16;
                case 0x29u: return Sh2OpClass::SHLR16;
                case 0x2Au: return Sh2OpClass::LDSPR;
                case 0x2Bu: return Sh2OpClass::JMP;
                case 0x2Eu: return Sh2OpClass::LDCVBR;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x5u: return Sh2OpClass::MOVLL4;
        case 0x6u: {
            switch (lo1) {
                case 0x0u: return Sh2OpClass::MOVBL;
                case 0x1u: return Sh2OpClass::MOVWL;
                case 0x2u: return Sh2OpClass::MOVLL;
                case 0x3u: return Sh2OpClass::MOV;
                case 0x4u: return Sh2OpClass::MOVBP;
                case 0x5u: return Sh2OpClass::MOVWP;
                case 0x6u: return Sh2OpClass::MOVLP;
                case 0x7u: return Sh2OpClass::NOT;
                case 0x8u: return Sh2OpClass::SWAPB;
                case 0x9u: return Sh2OpClass::SWAPW;
                case 0xAu: return Sh2OpClass::NEGC;
                case 0xBu: return Sh2OpClass::NEG;
                case 0xCu: return Sh2OpClass::EXTUB;
                case 0xDu: return Sh2OpClass::EXTUW;
                case 0xEu: return Sh2OpClass::EXTSB;
                case 0xFu: return Sh2OpClass::EXTSW;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x7u: return Sh2OpClass::ADDI;
        case 0x8u: {
            switch (n) {
                case 0x0u: return Sh2OpClass::MOVBS4;
                case 0x1u: return Sh2OpClass::MOVWS4;
                case 0x4u: return Sh2OpClass::MOVBL4;
                case 0x5u: return Sh2OpClass::MOVWL4;
                case 0x8u: return Sh2OpClass::CMPIM;
                case 0x9u: return Sh2OpClass::BT;
                case 0xBu: return Sh2OpClass::BF;
                case 0xDu: return Sh2OpClass::BTS;
                case 0xFu: return Sh2OpClass::BFS;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0x9u: return Sh2OpClass::MOVWI;
        case 0xAu: return Sh2OpClass::BRA;
        case 0xBu: return Sh2OpClass::BSR;
        case 0xCu: {
            switch (n) {
                case 0x0u: return Sh2OpClass::MOVBSG;
                case 0x1u: return Sh2OpClass::MOVWSG;
                case 0x2u: return Sh2OpClass::MOVLSG;
                case 0x4u: return Sh2OpClass::MOVBLG;
                case 0x5u: return Sh2OpClass::MOVWLG;
                case 0x6u: return Sh2OpClass::MOVLLG;
                case 0x7u: return Sh2OpClass::MOVA;
                case 0x8u: return Sh2OpClass::TSTI;
                case 0x9u: return Sh2OpClass::ANDI;
                case 0xAu: return Sh2OpClass::XORI;
                case 0xBu: return Sh2OpClass::ORI;
                case 0xCu: return Sh2OpClass::TSTM;
                case 0xDu: return Sh2OpClass::ANDM;
                case 0xEu: return Sh2OpClass::XORM;
                case 0xFu: return Sh2OpClass::ORM;
                default: return Sh2OpClass::INVALID;
            }
        }
        case 0xDu: return Sh2OpClass::MOVLI;
        case 0xEu: return Sh2OpClass::MOVI;
        default: return Sh2OpClass::INVALID;
    }
}

namespace {

struct Sh2DecodeLut {
    Sh2OpClass table[65536]{};
    bool ready = false;
};
Sh2DecodeLut g_decode_lut{};

struct VirtualBusAdapter {
    Sh2MemoryBus& inner;
    uint32_t fast_read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                       uint64_t cpu_cycles, BusAccessFault& fault) {
        return inner.read(cpu_id, ea, width, pc, cpu_cycles, fault);
    }
    void fast_write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val,
                    uint32_t pc, uint64_t cpu_cycles, BusAccessFault& fault) {
        inner.write(cpu_id, ea, width, val, pc, cpu_cycles, fault);
    }
};

}  // namespace

const Sh2OpClass* sh2_decode_lut() {
    if (!g_decode_lut.ready) {
        for (uint32_t op = 0u; op < 65536u; ++op) {
            g_decode_lut.table[op] = decode_sh2_class(static_cast<uint16_t>(op));
        }
        g_decode_lut.ready = true;
    }
    return g_decode_lut.table;
}

bool step_sh2(int cpu_id, Sh2CpuState& cpu, Sh2MemoryBus& bus, BusAccessFault& fault) {
    fault = BusAccessFault{};
    VirtualBusAdapter adapter{bus};
    return step_sh2_with_bus(cpu_id, cpu, adapter, sh2_decode_lut(), fault);
}

}  // namespace twc98
