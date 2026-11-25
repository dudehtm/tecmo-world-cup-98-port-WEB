// Sega ST-V WRAM + VDP2 VRAM + SH7604 on-chip peripheral adapters (DMAC0, DIVU, FRT, CCR).
// Reference semantics: MAME src/devices/cpu/sh/sh7604.cpp.
#pragma once

#include "sh2_core.hpp"

#include <array>
#include <cstdint>

namespace twc98 {

inline constexpr uint32_t kHighWramBase = 0x06000000u;
inline constexpr uint32_t kHighWramSize = 0x00100000u;
inline constexpr uint32_t kLowWramBase = 0x00200000u;
inline constexpr uint32_t kLowWramSize = 0x00100000u;
inline constexpr uint32_t kVdp2VramBase = 0x05E00000u;
inline constexpr uint32_t kVdp2VramSize = 0x00080000u;

// SH7604 cache address / associative purge / data array areas are strictly on-chip;
// filtering before masking avoids aliasing purge writes (e.g. 0x60000000) onto 0x00000000 BIOS.
inline bool is_sh2_cache_area(uint32_t ea) {
    return ea >= 0x40000000u && ea < 0xC0000000u;
}

inline uint32_t canonicalize_stv_addr(uint32_t ea) {
    if (ea >= 0xC0000000u) {
        return ea | 0x20000000u;
    }
    return ea & 0x07FFFFFFu;
}

struct Sh7604Divu {
    uint32_t dvsr = 0u;
    uint32_t dvdnth = 0u;
    uint32_t dvdntl = 0u;
    uint32_t ovf = 0u;
    uint32_t ovfie = 0u;
    uint32_t ops = 0u;
    uint32_t written = 0u;

    uint32_t read_reg(uint32_t canonical_ea, uint8_t width, BusAccessFault& fault) const;
    void write_reg(uint32_t canonical_ea, uint8_t width, uint32_t val, BusAccessFault& fault);
};

struct Sh7604Frt {
    uint32_t ftcsr = 0u;
    uint32_t tier = 0u;
    uint32_t tcr = 0u;
    uint32_t ocra = 0u;
    uint32_t ocrb = 0u;
    uint32_t frc = 0u;
    uint64_t frc_base = 0u;
    bool has_next_event = false;
    uint64_t next_event_cycle = 0u;

    void resync(uint64_t cur_cycles);
    void activate();
    void maybe_fire(uint64_t cur_cycles);
    uint8_t read_ftcsr(uint64_t cur_cycles);
    void write_ftcsr(uint8_t val, uint64_t cur_cycles);
};

struct Sh7604Dmac0 {
    uint32_t drcr0 = 0u;
    uint32_t sar0 = 0u;
    uint32_t dar0 = 0u;
    uint32_t tcr0 = 0u;
    uint32_t chcr0 = 0x00004A03u;
    uint32_t dmaor = 0x00000009u;
    uint32_t dma_irq0 = 0u;
    uint32_t transfers_completed = 0u;

    uint32_t read_reg(uint32_t canonical_ea, uint8_t width, BusAccessFault& fault) const;
    void write_reg(uint32_t canonical_ea, uint8_t width, uint32_t val, uint32_t pc,
                   uint64_t cpu_cycles, Sh2MemoryBus& bus, BusAccessFault& fault);
};

class SaturnStvBus final : public Sh2MemoryBus {
public:
    SaturnStvBus() = default;

    std::array<uint8_t, kHighWramSize> high_wram{};
    std::array<uint8_t, kLowWramSize> low_wram{};
    std::array<uint8_t, kVdp2VramSize> vdp2_vram{};
    std::array<uint8_t, kVdp2VramSize> vdp2_written{};
    bool strict_vram_init = true;

    std::array<Sh7604Divu, 2> divu{};
    std::array<Sh7604Frt, 2> frt{};
    std::array<Sh7604Dmac0, 2> dmac{};
    std::array<uint8_t, 2> ccr{};

    void reset();

    uint32_t read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                  uint64_t cpu_cycles, BusAccessFault& fault) override;
    void write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
               uint64_t cpu_cycles, BusAccessFault& fault) override;
};

}  // namespace twc98
