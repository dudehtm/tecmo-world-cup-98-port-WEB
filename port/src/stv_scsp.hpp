#pragma once

#include <cstdint>

namespace twc98 {

// Portable, zero-heap, serializable Yamaha YMF292 (SCSP) state for Sega ST-V.
// Derived from MAME scsp.cpp/scspdsp.cpp (ElSemi, R. Belmont, BSD-3-Clause) and
// saturnkit runtime/scsp.cpp (MIT). No mutable global state is used.
struct StvScspState {
    static constexpr int kShift = 12;
    static constexpr int kEgShift = 16;
    static constexpr uint32_t kSoundRamMask = 0x7FFFFu;  // 512 KiB ST-V SCSP RAM mirror window

    enum EgPhase : int32_t {
        EG_ATTACK = 0,
        EG_DECAY1 = 1,
        EG_DECAY2 = 2,
        EG_RELEASE = 3
    };

    struct Lfo {
        uint32_t phase = 0u;
        uint32_t step = 0u;
        uint8_t table_idx = 0u;
        uint8_t scale_idx = 0u;
    };

    struct Slot {
        uint16_t r[16]{};
        bool active = false;
        bool keyed = false;
        bool backwards = false;
        int64_t pos = 0;
        int32_t eg_state = EG_ATTACK;
        int32_t eg_vol = 0;
        int32_t ar = 0;
        int32_t d1r = 0;
        int32_t d2r = 0;
        int32_t rr = 0;
        int32_t dl = 0;
        Lfo plfo{};
        Lfo alfo{};

        bool pcm8() const { return (r[0] & 0x10u) != 0u; }
        int sbctl() const { return (r[0] >> 9) & 3; }
        int ssctl() const { return (r[0] >> 7) & 3; }
        int lpctl() const { return (r[0] >> 5) & 3; }
        uint32_t sa() const { return (static_cast<uint32_t>(r[0] & 0x0Fu) << 16) | r[1]; }
        uint32_t lsa() const { return r[2]; }
        uint32_t lea() const { return r[3]; }
        int d2r_r() const { return (r[4] >> 11) & 31; }
        int d1r_r() const { return (r[4] >> 6) & 31; }
        bool eghold() const { return (r[4] & 0x20u) != 0u; }
        int ar_r() const { return r[4] & 31; }
        bool lpslnk() const { return (r[5] & 0x4000u) != 0u; }
        int krs() const { return (r[5] >> 10) & 15; }
        int dl_r() const { return (r[5] >> 5) & 31; }
        int rr_r() const { return r[5] & 31; }
        bool stwinh() const { return (r[6] & 0x200u) != 0u; }
        bool sdir() const { return (r[6] & 0x100u) != 0u; }
        int tl() const { return r[6] & 0xFF; }
        int mdl() const { return (r[7] >> 12) & 15; }
        int mdxsl() const { return (r[7] >> 6) & 63; }
        int mdysl() const { return r[7] & 63; }
        int oct() const { return (((r[8] >> 11) & 15) ^ 8) - 8; }
        int fns() const { return r[8] & 0x3FF; }
        bool lfore() const { return (r[9] & 0x8000u) != 0u; }
        int lfof() const { return (r[9] >> 10) & 31; }
        int plfows() const { return (r[9] >> 8) & 3; }
        int plfos() const { return (r[9] >> 5) & 7; }
        int alfows() const { return (r[9] >> 3) & 3; }
        int alfos() const { return r[9] & 7; }
        int isel() const { return (r[10] >> 3) & 15; }
        int imxl() const { return r[10] & 7; }
        int disdl() const { return (r[11] >> 13) & 7; }
        int dipan() const { return (r[11] >> 8) & 31; }
        int efsdl() const { return (r[11] >> 5) & 7; }
        int efpan() const { return r[11] & 31; }
    };

    struct Timer {
        int32_t ctl = 0;
        int32_t counter = 0;
        int32_t reload = -1;
    };

    struct Dsp {
        int16_t coef[64]{};
        uint16_t madrs[32]{};
        uint16_t mpro[128 * 4]{};
        int32_t temp[128]{};
        int32_t mems[32]{};
        int32_t mixs[16]{};
        int16_t exts[2]{};
        int16_t efreg[16]{};
        uint32_t dec = 0u;
        int32_t last_step = 0;
    };

    Slot slots[32]{};
    uint16_t com[0x18]{};
    Timer timers[3]{};
    uint32_t scieb = 0u;
    uint32_t scipd = 0u;
    uint32_t scilv[3]{};
    uint32_t mcieb = 0u;
    uint32_t mcipd = 0u;
    bool main_line = false;
    uint32_t samples = 0u;
    int32_t stack[64]{};
    uint32_t stack_pos = 0u;
    Dsp dsp{};
    uint32_t noise = 1u;
    uint32_t rbl = 8192u;
    uint32_t rbp = 0u;
    int32_t cpu_level = 0;

    void reset();
    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t v, int size, uint8_t* sound_ram);
    void step_sample(uint8_t* sound_ram, int16_t out_lr[2], bool& out_main_irq_rise);

    int m68k_irq_level() const { return cpu_level; }
    uint32_t active_slots_mask() const;

private:
    uint16_t monitor();
    uint16_t read16(uint32_t off);
    void write16(uint32_t off, uint16_t v, uint16_t mask, uint8_t* sound_ram);
    void irq_update(bool& out_main_irq_rise);
    void pend(uint32_t bits);
    void dma(uint8_t* sound_ram);
    void dsp_program_changed();
    void dsp_step(uint8_t* sound_ram);
    int32_t read_sample(const Slot& s, int64_t idx, const uint8_t* sound_ram);
    int32_t slot_sample(Slot& s, const uint8_t* sound_ram);
};

}  // namespace twc98
