// Sega ST-V WRAM + VDP2 VRAM + SH7604 on-chip peripheral adapters implementation.
#include "stv_peripherals.hpp"

#include <algorithm>
#include <limits>

namespace twc98 {
namespace {

uint32_t read_be(const uint8_t* buf, size_t offset, uint8_t width) {
    uint32_t out = 0u;
    for (uint8_t i = 0; i < width; ++i) {
        out = (out << 8) | static_cast<uint32_t>(buf[offset + i]);
    }
    return out;
}

void write_be(uint8_t* buf, size_t offset, uint8_t width, uint32_t val) {
    for (uint8_t i = 0; i < width; ++i) {
        const uint8_t shift = static_cast<uint8_t>(8u * (width - 1u - i));
        buf[offset + i] = static_cast<uint8_t>((val >> shift) & 0xFFu);
    }
}

uint8_t frt_div_shift(uint32_t tcr) {
    static constexpr uint8_t kDivTab[4] = {3u, 5u, 7u, 0u};
    return kDivTab[tcr & 3u];
}

bool valid_width_and_align(uint32_t ea, uint8_t width) {
    if (width == 1u) return true;
    if (width == 2u) return (ea & 1u) == 0u;
    if (width == 4u) return (ea & 3u) == 0u;
    return false;
}

}  // namespace

uint32_t Sh7604Divu::read_reg(uint32_t canonical_ea, uint8_t width, BusAccessFault& fault) const {
    if (width != 4u) {
        fault.ok = false;
        fault.width = width;
        fault.address = canonical_ea;
        fault.reason = "UNSUPPORTED_DIVU_READ_WIDTH";
        return 0u;
    }
    switch (canonical_ea) {
        case 0xFFFFFF00u: return dvsr;
        case 0xFFFFFF04u: return dvdntl;
        case 0xFFFFFF08u: return ((ovfie & 1u) << 1) | (ovf & 1u);
        case 0xFFFFFF0Cu: return 0u;  // SH7604 INTC VCRDIV
        case 0xFFFFFF10u:
        case 0xFFFFFF18u: return dvdnth;
        case 0xFFFFFF14u:
        case 0xFFFFFF1Cu: return dvdntl;
        default:
            fault.ok = false;
            fault.width = width;
            fault.address = canonical_ea;
            fault.reason = "UNSUPPORTED_DIVU_READ_ADDR";
            return 0u;
    }
}

void Sh7604Divu::write_reg(uint32_t canonical_ea, uint8_t width, uint32_t val, BusAccessFault& fault) {
    if (width != 4u) {
        fault.ok = false;
        fault.is_write = true;
        fault.width = width;
        fault.address = canonical_ea;
        fault.reason = "UNSUPPORTED_DIVU_WRITE_WIDTH";
        return;
    }
    switch (canonical_ea) {
        case 0xFFFFFF00u:
            dvsr = val;
            written = 1u;
            return;
        case 0xFFFFFF04u: {
            dvdntl = val;
            written = 1u;
            const int32_t a = to_s32(dvdntl);
            const int32_t b = to_s32(dvsr);
            if (b == 0 || (a == std::numeric_limits<int32_t>::min() && b == -1)) {
                ovf = 1u;
                dvdntl = 0x7FFFFFFFu;
                dvdnth = 0x7FFFFFFFu;
            } else {
                dvdntl = static_cast<uint32_t>(a / b);
                dvdnth = static_cast<uint32_t>(a % b);
            }
            ops += 1u;
            return;
        }
        case 0xFFFFFF08u:
            ovf = val & 1u;
            ovfie = (val >> 1) & 1u;
            return;
        case 0xFFFFFF0Cu:
            // SH7604 INTC VCRDIV vector configuration
            return;
        case 0xFFFFFF10u:
            dvdnth = val;
            written = 1u;
            return;
        case 0xFFFFFF14u: {
            dvdntl = val;
            written = 1u;
            const uint64_t raw64 = (static_cast<uint64_t>(dvdnth) << 32) | static_cast<uint64_t>(dvdntl);
            const int64_t a = to_s64(raw64);
            const int64_t b = static_cast<int64_t>(to_s32(dvsr));
            if (b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1)) {
                ovf = 1u;
                dvdntl = 0x7FFFFFFFu;
                dvdnth = 0x7FFFFFFFu;
            } else {
                const int64_t q = a / b;
                if (q < std::numeric_limits<int32_t>::min() || q > std::numeric_limits<int32_t>::max()) {
                    ovf = 1u;
                    dvdntl = 0x7FFFFFFFu;
                    dvdnth = 0x7FFFFFFFu;
                } else {
                    const int64_t rem = a - q * b;
                    dvdntl = static_cast<uint32_t>(static_cast<int32_t>(q));
                    dvdnth = static_cast<uint32_t>(static_cast<int32_t>(rem));
                }
            }
            ops += 1u;
            return;
        }
        default:
            fault.ok = false;
            fault.is_write = true;
            fault.width = width;
            fault.address = canonical_ea;
            fault.reason = "UNSUPPORTED_DIVU_WRITE_ADDR";
            return;
    }
}

void Sh7604Frt::resync(uint64_t cur_cycles) {
    if (cur_cycles <= frc_base) return;
    const uint8_t shift = frt_div_shift(tcr);
    const uint64_t add = (cur_cycles - frc_base) >> shift;
    if (add > 0u) {
        frc = static_cast<uint32_t>((static_cast<uint64_t>(frc) + add) & 0xFFFFu);
        frc_base = cur_cycles;
    }
}

void Sh7604Frt::activate() {
    bool any = false;
    uint32_t min_delta = 0x10001u;
    if ((ftcsr & 0x08u) == 0u) {
        const uint32_t d = (ocra - frc) & 0xFFFFu;
        min_delta = std::min(min_delta, d == 0u ? 0x10000u : d);
        any = true;
    }
    if ((ftcsr & 0x04u) == 0u) {
        const uint32_t d = (ocrb - frc) & 0xFFFFu;
        min_delta = std::min(min_delta, d == 0u ? 0x10000u : d);
        any = true;
    }
    if ((ftcsr & 0x02u) == 0u && (ftcsr & 0x01u) == 0u) {
        min_delta = std::min(min_delta, 0x10000u - (frc & 0xFFFFu));
        any = true;
    }
    if (any) {
        has_next_event = true;
        next_event_cycle = frc_base + (static_cast<uint64_t>(min_delta) << frt_div_shift(tcr));
    } else {
        has_next_event = false;
        next_event_cycle = 0u;
    }
}

void Sh7604Frt::maybe_fire(uint64_t cur_cycles) {
    while (has_next_event && cur_cycles >= next_event_cycle) {
        resync(next_event_cycle);
        if (frc == ocrb) ftcsr |= 0x04u;
        if (frc == 0x0000u) ftcsr |= 0x02u;
        if (frc == ocra) {
            ftcsr |= 0x08u;
            if ((ftcsr & 0x01u) != 0u) {
                frc = 0u;
            }
        }
        activate();
    }
}

uint8_t Sh7604Frt::read_ftcsr(uint64_t cur_cycles) {
    maybe_fire(cur_cycles);
    return static_cast<uint8_t>(ftcsr & 0xFFu);
}

void Sh7604Frt::write_ftcsr(uint8_t val, uint64_t cur_cycles) {
    const uint32_t old = ftcsr;
    ftcsr = val;
    resync(cur_cycles);
    constexpr uint32_t kFlags = 0x8Eu;  // ICF | OCFA | OCFB | OVF
    ftcsr = (ftcsr & ~kFlags) | (old & ftcsr & kFlags);
    activate();
}

uint32_t Sh7604Dmac0::read_reg(uint32_t canonical_ea, uint8_t width, BusAccessFault& fault) const {
    if (width != 4u) {
        fault.ok = false;
        fault.width = width;
        fault.address = canonical_ea;
        fault.reason = "UNSUPPORTED_DMAC0_READ_WIDTH";
        return 0u;
    }
    switch (canonical_ea) {
        case 0xFFFFFF80u: return sar0;
        case 0xFFFFFF84u: return dar0;
        case 0xFFFFFF88u: return tcr0;
        case 0xFFFFFF8Cu: return chcr0;
        case 0xFFFFFFB0u: return dmaor & 0xFu;
        default:
            fault.ok = false;
            fault.width = width;
            fault.address = canonical_ea;
            fault.reason = "UNSUPPORTED_DMAC0_READ_ADDR";
            return 0u;
    }
}

void Sh7604Dmac0::write_reg(uint32_t canonical_ea, uint8_t width, uint32_t val, uint32_t pc,
                            uint64_t cpu_cycles, Sh2MemoryBus& bus, BusAccessFault& fault) {
    if (canonical_ea == 0xFFFFFE71u && width == 1u) {
        drcr0 = val & 3u;
        return;
    }
    if (width != 4u) {
        fault.ok = false;
        fault.is_write = true;
        fault.width = width;
        fault.address = canonical_ea;
        fault.reason = "UNSUPPORTED_DMAC0_WRITE_WIDTH";
        return;
    }
    switch (canonical_ea) {
        case 0xFFFFFF80u: sar0 = val; return;
        case 0xFFFFFF84u: dar0 = val; return;
        case 0xFFFFFF88u: tcr0 = val & 0xFFFFFFu; return;
        case 0xFFFFFF8Cu: {
            const uint32_t old = chcr0;
            chcr0 = (val & ~2u) | (old & val & 2u);
            break;
        }
        case 0xFFFFFFB0u: {
            const uint32_t old = dmaor & 0xFu;
            dmaor = ((val & ~6u) | (old & val & 6u)) & 0xFu;
            break;
        }
        default:
            fault.ok = false;
            fault.is_write = true;
            fault.width = width;
            fault.address = canonical_ea;
            fault.reason = "UNSUPPORTED_DMAC0_WRITE_ADDR";
            return;
    }

    // Synchronous burst equivalence matching hw-vdp2-dmac01 & sh7604.cpp:619-738.
    if (((chcr0 & dmaor & 1u) != 0u) && ((chcr0 & 2u) == 0u)) {
        const uint32_t incd = (chcr0 >> 14) & 3u;
        const uint32_t incs = (chcr0 >> 12) & 3u;
        const uint32_t size = (chcr0 >> 10) & 3u;
        if (incd == 3u || incs == 3u || (chcr0 & 4u) != 0u) {
            fault.ok = false;
            fault.is_write = true;
            fault.width = width;
            fault.address = canonical_ea;
            fault.pc = pc;
            fault.reason = "UNMODELED_DMAC0_MODE_OR_IRQ";
            return;
        }
        const uint8_t unit_w = (size == 0u) ? 1u : (size == 1u) ? 2u : 4u;
        const uint32_t align_mask = (unit_w == 4u) ? 0xC7FFFFFCu : (unit_w == 2u) ? 0xC7FFFFFEu : 0xC7FFFFFFu;
        uint32_t src = sar0 & align_mask;
        uint32_t dst = dar0 & align_mask;
        uint32_t count = (tcr0 != 0u) ? tcr0 : 0x01000000u;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t raddr = (incs == 2u) ? (src - unit_w) : src;
            const uint32_t waddr = (incd == 2u) ? (dst - unit_w) : dst;
            const uint32_t beat = bus.read(0, raddr, unit_w, pc, cpu_cycles, fault);
            if (!fault.ok) return;
            bus.write(0, waddr, unit_w, beat, pc, cpu_cycles, fault);
            if (!fault.ok) return;
            if (incs == 1u) src += unit_w;
            else if (incs == 2u) src -= unit_w;
            if (incd == 1u) dst += unit_w;
            else if (incd == 2u) dst -= unit_w;
        }
        tcr0 = 0u;
        chcr0 |= 2u;
        dma_irq0 |= 1u;
        transfers_completed += 1u;
    }
}

void SaturnStvBus::reset() {
    high_wram.fill(0u);
    low_wram.fill(0u);
    vdp2_vram.fill(0u);
    vdp2_written.fill(0u);
    strict_vram_init = true;
    divu = {};
    frt = {};
    dmac = {};
    ccr = {};
}

uint32_t SaturnStvBus::read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                            uint64_t cpu_cycles, BusAccessFault& fault) {
    if (is_sh2_cache_area(ea)) {
        return 0u;
    }
    const uint32_t cea = canonicalize_stv_addr(ea);
    if (!valid_width_and_align(cea, width)) {
        fault.ok = false;
        fault.width = width;
        fault.address = ea;
        fault.pc = pc;
        fault.reason = "UNALIGNED_BUS_READ";
        return 0u;
    }
    // MAME stv.cpp: workram_h 0x06000000..0x060FFFFF mirrored across 0x01F00000 (0x06000000..0x07FFFFFF).
    if (cea >= kHighWramBase && cea <= 0x07FFFFFFu) {
        const uint32_t off = (cea - kHighWramBase) & (kHighWramSize - 1u);
        if (off + width <= kHighWramSize) {
            return read_be(high_wram.data(), off, width);
        }
    }
    // MAME stv.cpp: workram_l 0x00200000..0x002FFFFF mirrored across 0x00100000 (0x00200000..0x003FFFFF).
    if (cea >= kLowWramBase && cea <= 0x003FFFFFu) {
        const uint32_t off = (cea - kLowWramBase) & (kLowWramSize - 1u);
        if (off + width <= kLowWramSize) {
            return read_be(low_wram.data(), off, width);
        }
    }
    if (cea >= kVdp2VramBase && cea + width <= kVdp2VramBase + kVdp2VramSize) {
        const uint32_t off = cea - kVdp2VramBase;
        if (strict_vram_init) {
            for (uint8_t i = 0; i < width; ++i) {
                if (vdp2_written[off + i] == 0u) {
                    fault.ok = false;
                    fault.width = width;
                    fault.address = ea;
                    fault.pc = pc;
                    fault.reason = "UNINITIALIZED_VDP2_VRAM_READ";
                    return 0u;
                }
            }
        }
        return read_be(vdp2_vram.data(), off, width);
    }
    if (cea == 0xFFFFFE11u && width == 1u) {
        return frt[static_cast<size_t>(cpu_id)].read_ftcsr(cpu_cycles);
    }
    if (cea == 0xFFFFFE80u || cea == 0xFFFFFE81u || cea == 0xFFFFFE82u || cea == 0xFFFFFE83u ||
        cea == 0xFFFFFE91u) {
        return 0u;
    }
    if (cea == 0xFFFFFEE0u && width == 2u) {
        return 0x8000u;  // SH7604 INTC ICR: NMIL=1 (NMI not asserted)
    }
    if ((cea >= 0xFFFFFE60u && cea <= 0xFFFFFE68u) ||
        (cea >= 0xFFFFFEE2u && cea <= 0xFFFFFEE4u) ||
        (cea >= 0xFFFFFFA0u && cea <= 0xFFFFFFACu)) {
        return 0u;
    }
    if (cea == 0xFFFFFE92u && width == 1u) {
        return ccr[static_cast<size_t>(cpu_id)];
    }
    if (cea >= 0xFFFFFF00u && cea <= 0xFFFFFF1Fu) {
        return divu[static_cast<size_t>(cpu_id)].read_reg(cea, width, fault);
    }
    if ((cea >= 0xFFFFFF80u && cea <= 0xFFFFFF8Fu) || cea == 0xFFFFFFB0u) {
        return dmac[static_cast<size_t>(cpu_id)].read_reg(cea, width, fault);
    }
    if (cea == 0xFFFFFFE0u && width == 4u) {
        // SH7604 BCR1: bit 15 identifies Master (0) vs Slave (1) CPU.
        return (cpu_id == 1) ? 0x000083F0u : 0x000003F0u;
    }
    if (cea >= 0xFFFFFFE4u && cea <= 0xFFFFFFFCu && (cea & 3u) == 0u && width == 4u) {
        return 0u;
    }
    fault.ok = false;
    fault.width = width;
    fault.address = ea;
    fault.pc = pc;
    fault.reason = "UNMODELED_MMIO_OR_BUS_READ";
    return 0u;
}

void SaturnStvBus::write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
                         uint64_t cpu_cycles, BusAccessFault& fault) {
    if (is_sh2_cache_area(ea)) {
        return;
    }
    const uint32_t cea = canonicalize_stv_addr(ea);
    if (!valid_width_and_align(cea, width)) {
        fault.ok = false;
        fault.is_write = true;
        fault.width = width;
        fault.address = ea;
        fault.pc = pc;
        fault.reason = "UNALIGNED_BUS_WRITE";
        return;
    }
    if (cea >= kHighWramBase && cea <= 0x07FFFFFFu) {
        const uint32_t off = (cea - kHighWramBase) & (kHighWramSize - 1u);
        if (off + width <= kHighWramSize) {
            write_be(high_wram.data(), off, width, val);
            return;
        }
    }
    if (cea >= kLowWramBase && cea <= 0x003FFFFFu) {
        const uint32_t off = (cea - kLowWramBase) & (kLowWramSize - 1u);
        if (off + width <= kLowWramSize) {
            write_be(low_wram.data(), off, width, val);
            return;
        }
    }
    if (cea >= kVdp2VramBase && cea + width <= kVdp2VramBase + kVdp2VramSize) {
        const uint32_t off = cea - kVdp2VramBase;
        write_be(vdp2_vram.data(), off, width, val);
        for (uint8_t i = 0; i < width; ++i) {
            vdp2_written[off + i] = 1u;
        }
        return;
    }
    if (cea == 0xFFFFFE11u && width == 1u) {
        frt[static_cast<size_t>(cpu_id)].write_ftcsr(static_cast<uint8_t>(val & 0xFFu), cpu_cycles);
        return;
    }
    if (cea == 0xFFFFFE80u || cea == 0xFFFFFE81u || cea == 0xFFFFFE82u || cea == 0xFFFFFE83u ||
        cea == 0xFFFFFE91u ||
        (cea >= 0xFFFFFE60u && cea <= 0xFFFFFE68u) ||
        (cea >= 0xFFFFFEE0u && cea <= 0xFFFFFEE4u) ||
        (cea >= 0xFFFFFFA0u && cea <= 0xFFFFFFACu)) {
        // SH7604 WDT / SBYCR / INTC / VCRDMA register write
        return;
    }
    if (cea == 0xFFFFFE92u && width == 1u) {
        ccr[static_cast<size_t>(cpu_id)] = static_cast<uint8_t>(val & 0xFFu);
        return;
    }
    if (cea >= 0xFFFFFF00u && cea <= 0xFFFFFF1Fu) {
        divu[static_cast<size_t>(cpu_id)].write_reg(cea, width, val, fault);
        return;
    }
    if (cea == 0xFFFFFE71u || (cea >= 0xFFFFFF80u && cea <= 0xFFFFFF8Fu) || cea == 0xFFFFFFB0u) {
        dmac[static_cast<size_t>(cpu_id)].write_reg(cea, width, val, pc, cpu_cycles, *this, fault);
        return;
    }
    if ((cea >= 0xFFFFFFE0u && cea <= 0xFFFFFFFCu && (cea & 3u) == 0u && width == 4u) ||
        (cea >= 0xFFFF8000u && cea <= 0xFFFFBFFFu && width == 2u)) {
        // SH7604 BSC / SDRAM mode register write
        return;
    }
    fault.ok = false;
    fault.is_write = true;
    fault.width = width;
    fault.address = ea;
    fault.pc = pc;
    fault.reason = "UNMODELED_MMIO_OR_BUS_WRITE";
}

}  // namespace twc98
