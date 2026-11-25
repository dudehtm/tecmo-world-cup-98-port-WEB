#include "stv_scsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twc98 {

namespace {

// Read-only precomputed synthesis tables initialized once on first reset.
bool g_tables_inited = false;
float g_eg_gain[1024]{};
float g_tl_gain[256]{};
float g_sdl_gain[8]{};
float g_pan_l[32]{};
float g_pan_r[32]{};
float g_mvol[16]{};
int32_t g_ar_step[64]{};
int32_t g_dr_step[64]{};
int g_alfo[4][256]{};
int g_plfo[4][256]{};
int g_ascale[8][256]{};
int g_pscale[8][256]{};

constexpr double kArMs[64] = {
    1e5,    1e5,    8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0,
    1700.0, 1500.0, 1200.0, 1000.0, 860.0,  760.0,  600.0,  500.0,  430.0,  380.0,  300.0,
    250.0,  220.0,  190.0,  150.0,  130.0,  110.0,  95.0,   76.0,   63.0,   55.0,   47.0,
    38.0,   31.0,   27.0,   24.0,   19.0,   15.0,   13.0,   12.0,   9.4,    7.9,    6.8,
    6.0,    4.7,    3.8,    3.4,    3.0,    2.4,    2.0,    1.8,    1.6,    1.3,    1.1,
    0.93,   0.85,   0.65,   0.53,   0.44,   0.40,   0.35,   0.0,    0.0};

constexpr double kDrMs[64] = {
    1e5,     1e5,     118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0,
    29600.0, 25300.0, 22200.0,  17700.0,  14800.0, 12700.0, 11100.0, 8900.0,  7400.0,  6300.0,
    5500.0,  4400.0,  3700.0,   3200.0,   2800.0,  2200.0,  1800.0,  1600.0,  1400.0,  1100.0,
    920.0,   790.0,   690.0,    550.0,    460.0,   390.0,   340.0,   270.0,   230.0,   200.0,
    170.0,   140.0,   110.0,    98.0,     85.0,    68.0,    57.0,    49.0,    43.0,    34.0,
    28.0,    25.0,    22.0,     18.0,     14.0,    12.0,    11.0,    8.5,     7.1,     6.1,
    5.4,     4.3,     3.6,      3.1};

constexpr float kLfoHz[32] = {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f,  0.55f,  0.68f,  0.78f,  0.92f,
    1.10f, 1.39f, 1.60f, 1.87f, 2.27f, 2.87f, 3.31f,  3.92f,  4.79f,  6.15f,  7.18f,
    8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f,  57.4f,  86.1f,  172.3f};

constexpr float kAScaleDb[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
constexpr float kPScaleCents[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

inline float db_to_amp(float d) { return std::pow(10.0f, d / 20.0f); }

void ensure_tables_initialized() {
    if (g_tables_inited) return;
    g_tables_inited = true;
    for (int i = 0; i < 1024; ++i) {
        g_eg_gain[i] = db_to_amp(3.0f * static_cast<float>(i - 0x3FF) / 32.0f);
    }
    for (int i = 0; i < 256; ++i) {
        static constexpr float kBits[8] = {0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f, 48.0f};
        float d = 0.0f;
        for (int b = 0; b < 8; ++b) {
            if ((i >> b) & 1) d -= kBits[b];
        }
        g_tl_gain[i] = db_to_amp(d);
    }
    g_sdl_gain[0] = 0.0f;
    for (int i = 1; i < 8; ++i) {
        g_sdl_gain[i] = db_to_amp(-6.0f * static_cast<float>(7 - i));
    }
    for (int p = 0; p < 32; ++p) {
        const int a = p & 15;
        const float g = (a == 15)
            ? 0.0f
            : db_to_amp(-(3.0f * (a & 1) + 6.0f * ((a >> 1) & 1) + 12.0f * ((a >> 2) & 1) +
                          24.0f * ((a >> 3) & 1)));
        g_pan_l[p] = (p < 16) ? g : 1.0f;
        g_pan_r[p] = (p < 16) ? 1.0f : g;
    }
    g_mvol[0] = 0.0f;
    for (int i = 1; i < 16; ++i) {
        g_mvol[i] = db_to_amp(-3.0f * static_cast<float>(15 - i));
    }
    for (int i = 0; i < 64; ++i) {
        g_ar_step[i] = (i < 2) ? 0
            : (kArMs[i] == 0.0) ? (1024 << StvScspState::kEgShift)
                                : static_cast<int32_t>(1023.0 * 1000.0 / (44100.0 * kArMs[i]) *
                                                       static_cast<double>(1 << StvScspState::kEgShift));
        g_dr_step[i] = (i < 2)
            ? 0
            : static_cast<int32_t>(1023.0 * 1000.0 / (44100.0 * kDrMs[i]) *
                                   static_cast<double>(1 << StvScspState::kEgShift));
    }
    uint32_t rnd = 0x12345678u;
    for (int i = 0; i < 256; ++i) {
        g_alfo[0][i] = 255 - i;
        g_plfo[0][i] = (i < 128) ? i : (i - 256);
        g_alfo[1][i] = (i < 128) ? 255 : 0;
        g_plfo[1][i] = (i < 128) ? 127 : -128;
        g_alfo[2][i] = (i < 128) ? (255 - i * 2) : (i * 2 - 256);
        g_plfo[2][i] = (i < 64) ? (i * 2) : (i < 128) ? (255 - i * 2) : (i < 192) ? (256 - i * 2) : (i * 2 - 511);
        rnd = rnd * 1103515245u + 12345u;
        const int a = static_cast<int>((rnd >> 16) & 0xFFu);
        g_alfo[3][i] = a;
        g_plfo[3][i] = 128 - a;
    }
    for (int s = 0; s < 8; ++s) {
        for (int i = 0; i < 256; ++i) {
            g_pscale[s][i] = static_cast<int>(
                256.0f * std::pow(2.0f, kPScaleCents[s] * static_cast<float>(i - 128) / 128.0f / 1200.0f));
            g_ascale[s][i] = static_cast<int>(256.0f * db_to_amp(-kAScaleDb[s] * static_cast<float>(i) / 256.0f));
        }
    }
}

inline uint32_t step_of(const StvScspState::Slot& s) {
    const uint32_t fn = static_cast<uint32_t>(s.fns()) + 1024u;
    const int sh = s.oct() + StvScspState::kShift - 10;
    return (sh >= 0) ? (fn << sh) : (fn >> -sh);
}

void compute_eg(StvScspState::Slot& s) {
    const int base = (s.krs() == 15) ? 0 : (s.oct() + 2 * s.krs() + ((s.fns() >> 9) & 1));
    auto rate = [&](int r, const int32_t* table) -> int32_t {
        if (r == 0) return 0;
        const int idx = std::max(0, std::min(63, base + 2 * r));
        return table[idx];
    };
    s.ar = rate(s.ar_r(), g_ar_step);
    s.d1r = rate(s.d1r_r(), g_dr_step);
    s.d2r = rate(s.d2r_r(), g_dr_step);
    s.rr = rate(s.rr_r(), g_dr_step);
    s.dl = 0x1F - s.dl_r();
}

void compute_lfo(StvScspState::Slot& s) {
    const uint32_t step = static_cast<uint32_t>(kLfoHz[s.lfof()] * 256.0f / 44100.0f * 256.0f);
    s.plfo.step = step;
    s.alfo.step = step;
    s.plfo.table_idx = static_cast<uint8_t>(s.plfows() & 3);
    s.plfo.scale_idx = static_cast<uint8_t>(s.plfos() & 7);
    s.alfo.table_idx = static_cast<uint8_t>(s.alfows() & 3);
    s.alfo.scale_idx = static_cast<uint8_t>(s.alfos() & 7);
}

void key_on(StvScspState::Slot& s) {
    s.active = true;
    s.pos = 0;
    s.backwards = false;
    compute_eg(s);
    compute_lfo(s);
    s.plfo.phase = 0u;
    s.alfo.phase = 0u;
    s.eg_state = StvScspState::EG_ATTACK;
    s.eg_vol = 0x17F << StvScspState::kEgShift;
}

void key_off(StvScspState::Slot& s) {
    if (s.active) s.eg_state = StvScspState::EG_RELEASE;
}

float eg_step(StvScspState::Slot& s) {
    switch (s.eg_state) {
    case StvScspState::EG_ATTACK:
        s.eg_vol += s.ar;
        if (s.eg_vol >= (0x3FF << StvScspState::kEgShift)) {
            s.eg_vol = 0x3FF << StvScspState::kEgShift;
            if (!s.lpslnk()) {
                s.eg_state = (s.d1r >= (1024 << StvScspState::kEgShift))
                    ? StvScspState::EG_DECAY2
                    : StvScspState::EG_DECAY1;
            }
        }
        if (s.eghold()) return 1.0f;
        return static_cast<float>(s.eg_vol >> StvScspState::kEgShift) / 1024.0f;
    case StvScspState::EG_DECAY1:
        s.eg_vol = std::max(0, s.eg_vol - s.d1r);
        if ((s.eg_vol >> (StvScspState::kEgShift + 5)) <= s.dl) {
            s.eg_state = StvScspState::EG_DECAY2;
        }
        break;
    case StvScspState::EG_DECAY2:
        s.eg_vol = std::max(0, s.eg_vol - s.d2r);
        break;
    case StvScspState::EG_RELEASE:
        s.eg_vol -= s.rr;
        if (s.eg_vol <= 0) {
            s.eg_vol = 0;
            s.active = false;
        }
        break;
    }
    const int idx = std::max(0, std::min(1023, s.eg_vol >> StvScspState::kEgShift));
    return g_eg_gain[idx];
}

inline int lfo_next(StvScspState::Lfo& l, bool reset, const int table[4][256]) {
    if (reset) l.phase = 0u;
    else l.phase += l.step;
    return table[l.table_idx][(l.phase >> 8) & 0xFFu];
}

inline int32_t sext_bits(int32_t v, int bits) {
    const int sh = 32 - bits;
    return static_cast<int32_t>(static_cast<uint32_t>(v) << sh) >> sh;
}

uint16_t dsp_pack(int32_t val) {
    const int sign = (val >> 23) & 1;
    uint32_t t = static_cast<uint32_t>(val ^ (val << 1)) & 0xFFFFFFu;
    int e = 0;
    while (e < 12 && !(t & 0x800000u)) {
        t <<= 1;
        ++e;
    }
    val = (e < 12) ? ((val << e) & 0x3FFFFF) : (val << 11);
    val >>= 11;
    return static_cast<uint16_t>((val & 0x7FF) | (sign << 15) | (e << 11));
}

int32_t dsp_unpack(uint16_t v) {
    const int sign = (v >> 15) & 1;
    int e = (v >> 11) & 15;
    int32_t u = (v & 0x7FF) << 11;
    if (e > 11) {
        e = 11;
        u |= sign << 22;
    } else {
        u |= (sign ^ 1) << 22;
    }
    u |= sign << 23;
    return sext_bits(u, 24) >> e;
}

}  // namespace

void StvScspState::reset() {
    ensure_tables_initialized();
    for (Slot& s : slots) {
        s = Slot{};
        compute_lfo(s);
    }
    std::memset(com, 0, sizeof(com));
    for (Timer& t : timers) {
        t = Timer{0, 0, -1};
    }
    scieb = 0u;
    scipd = 0u;
    scilv[0] = 0u;
    scilv[1] = 0u;
    scilv[2] = 0u;
    mcieb = 0u;
    mcipd = 0u;
    main_line = false;
    samples = 0u;
    std::memset(stack, 0, sizeof(stack));
    stack_pos = 0u;
    dsp = Dsp{};
    noise = 1u;
    rbl = 8192u;
    rbp = 0u;
    cpu_level = 0;
}

int32_t StvScspState::read_sample(const Slot& s, int64_t idx, const uint8_t* sound_ram) {
    if (!sound_ram) return 0;
    if (s.ssctl() == 1) {
        noise = noise * 1103515245u + 12345u;
        return static_cast<int16_t>(noise >> 16);
    }
    if (s.ssctl() != 0) return 0;
    int32_t v = 0;
    if (s.pcm8()) {
        v = static_cast<int32_t>(static_cast<int8_t>(
            sound_ram[(s.sa() + static_cast<uint32_t>(idx)) & kSoundRamMask])) << 8;
    } else {
        const uint32_t a = (s.sa() + 2u * static_cast<uint32_t>(idx)) & (kSoundRamMask & ~1u);
        v = static_cast<int16_t>((static_cast<uint16_t>(sound_ram[a]) << 8) | sound_ram[a + 1u]);
    }
    if (s.sbctl() & 1) v ^= 0x7FFF;
    if (s.sbctl() & 2) v = static_cast<int16_t>(v ^ 0x8000);
    return v;
}

int32_t StvScspState::slot_sample(Slot& s, const uint8_t* sound_ram) {
    const int64_t lsa = static_cast<int64_t>(s.lsa()) << kShift;
    const int64_t lea = static_cast<int64_t>(s.lea()) << kShift;
    int64_t idx = s.pos >> kShift;
    if (s.mdl() >= 5) {
        const int32_t m = (stack[(stack_pos + static_cast<uint32_t>(s.mdxsl())) & 63u] +
                           stack[(stack_pos + static_cast<uint32_t>(s.mdysl())) & 63u]) / 2;
        idx += (m << 10) >> (0x1A - s.mdl());
    }
    const int32_t frac = static_cast<int32_t>(s.pos & ((1 << kShift) - 1));
    const int32_t a = read_sample(s, idx, sound_ram);
    const int32_t b = read_sample(s, idx + (s.backwards ? -1 : 1), sound_ram);
    int32_t v = static_cast<int32_t>(
        (static_cast<int64_t>(a) * ((1 << kShift) - frac) + static_cast<int64_t>(b) * frac) >> kShift);

    int64_t step = static_cast<int64_t>(step_of(s));
    if (s.plfos()) {
        const int lv = lfo_next(s.plfo, s.lfore(), g_plfo);
        const int sc = g_pscale[s.plfo.scale_idx][std::max(0, std::min(255, lv + 128))];
        step = step * sc / 256;
    }
    s.pos += s.backwards ? -step : step;
    if (!s.backwards && s.pos >= lsa && s.lpslnk() && s.eg_state == EG_ATTACK) {
        s.eg_state = EG_DECAY1;
    }
    switch (s.lpctl()) {
    case 0:
        if (s.pos >= lea && s.pos >= lsa) s.active = false;
        break;
    case 1:
        if (s.pos >= lea) {
            const int64_t span = std::max<int64_t>(lea - lsa, 1 << kShift);
            s.pos -= span;
        }
        break;
    case 2:
        if (!s.backwards && s.pos >= lsa) {
            s.pos = lea - (s.pos - lsa);
            s.backwards = true;
        } else if (s.backwards && s.pos < lsa) {
            s.pos = lea - (lsa - s.pos);
        }
        break;
    case 3:
        if (!s.backwards && s.pos >= lea) {
            s.pos = 2 * lea - s.pos;
            s.backwards = true;
        } else if (s.backwards && s.pos < lsa) {
            s.pos = 2 * lsa - s.pos;
            s.backwards = false;
        }
        break;
    }
    if (s.sdir()) return v;
    if (s.alfos()) {
        const int lv = lfo_next(s.alfo, s.lfore(), g_alfo);
        v = v * g_ascale[s.alfo.scale_idx][lv & 255] / 256;
    }
    return static_cast<int32_t>(static_cast<float>(v) * eg_step(s));
}

void StvScspState::dsp_program_changed() {
    int i = 127;
    while (i >= 0 &&
           !(dsp.mpro[i * 4] | dsp.mpro[i * 4 + 1] | dsp.mpro[i * 4 + 2] | dsp.mpro[i * 4 + 3])) {
        --i;
    }
    dsp.last_step = i + 1;
}

void StvScspState::dsp_step(uint8_t* sound_ram) {
    Dsp& d = dsp;
    std::memset(d.efreg, 0, sizeof(d.efreg));
    if (d.last_step <= 0) {
        std::memset(d.mixs, 0, sizeof(d.mixs));
        return;
    }
    int32_t acc = 0, memval = 0, frc = 0, yreg = 0;
    uint32_t adrs = 0u;
    for (int st = 0; st < d.last_step; ++st) {
        const uint16_t* ip = d.mpro + st * 4;
        const uint32_t tra = (ip[0] >> 8) & 0x7Fu;
        const uint32_t twt = (ip[0] >> 7) & 1u;
        const uint32_t twa = ip[0] & 0x7Fu;
        const uint32_t xsel = (ip[1] >> 15) & 1u;
        const uint32_t ysel = (ip[1] >> 13) & 3u;
        const uint32_t ira = (ip[1] >> 6) & 0x3Fu;
        const uint32_t iwt = (ip[1] >> 5) & 1u;
        const uint32_t iwa = ip[1] & 0x1Fu;
        const uint32_t table = (ip[2] >> 15) & 1u;
        const uint32_t mwt = (ip[2] >> 14) & 1u;
        const uint32_t mrd = (ip[2] >> 13) & 1u;
        const uint32_t ewt = (ip[2] >> 12) & 1u;
        const uint32_t ewa = (ip[2] >> 8) & 15u;
        const uint32_t adrl = (ip[2] >> 7) & 1u;
        const uint32_t frcl = (ip[2] >> 6) & 1u;
        const uint32_t shift = (ip[2] >> 4) & 3u;
        const uint32_t yrl = (ip[2] >> 3) & 1u;
        const uint32_t negb = (ip[2] >> 2) & 1u;
        const uint32_t zero = (ip[2] >> 1) & 1u;
        const uint32_t bsel = ip[2] & 1u;
        const uint32_t nofl = (ip[3] >> 15) & 1u;
        const uint32_t coef = (ip[3] >> 9) & 0x3Fu;
        const uint32_t masa = (ip[3] >> 2) & 0x1Fu;
        const uint32_t adreb = (ip[3] >> 1) & 1u;
        const uint32_t nxadr = ip[3] & 1u;

        int32_t in = 0;
        if (ira <= 0x1Fu) in = d.mems[ira];
        else if (ira <= 0x2Fu) in = d.mixs[ira - 0x20u] << 4;
        else if (ira <= 0x31u) in = static_cast<int32_t>(d.exts[ira - 0x30u]) << 8;
        else break;
        in = sext_bits(in, 24);
        if (iwt) {
            d.mems[iwa] = memval;
            if (ira == iwa) in = memval;
        }
        int32_t b = 0;
        if (!zero) {
            b = bsel ? acc : sext_bits(d.temp[(tra + d.dec) & 0x7Fu], 24);
            if (negb) b = -b;
        }
        const int32_t x = xsel ? in : sext_bits(d.temp[(tra + d.dec) & 0x7Fu], 24);
        const int32_t y = (ysel == 0u) ? frc
            : (ysel == 1u) ? (d.coef[coef] >> 3)
            : (ysel == 2u) ? ((yreg >> 11) & 0x1FFF)
                           : ((yreg >> 4) & 0x0FFF);
        if (yrl) yreg = in;
        const int32_t acc2 = static_cast<int32_t>(static_cast<uint32_t>(acc) * 2u);
        const int32_t shifted = (shift == 0u) ? std::max(-0x800000, std::min(0x7FFFFF, acc))
            : (shift == 1u) ? std::max(-0x800000, std::min(0x7FFFFF, acc2))
            : (shift == 2u) ? sext_bits(acc2, 24)
                            : sext_bits(acc, 24);
        acc = static_cast<int32_t>((static_cast<int64_t>(x) * sext_bits(y, 13)) >> 12) + b;
        if (twt) d.temp[(twa + d.dec) & 0x7Fu] = shifted;
        if (frcl) frc = (shift == 3u) ? (shifted & 0x0FFF) : ((shifted >> 11) & 0x1FFF);
        if ((mrd || mwt) && (st & 1) && sound_ram) {
            uint32_t addr = d.madrs[masa];
            if (!table) addr += d.dec;
            if (adreb) addr += adrs & 0x0FFFu;
            if (nxadr) ++addr;
            addr &= table ? 0xFFFFu : (rbl - 1u);
            addr = ((addr + (rbp << 12)) << 1) & (kSoundRamMask & ~1u);
            if (mrd) {
                const uint16_t w = static_cast<uint16_t>(
                    (static_cast<uint16_t>(sound_ram[addr]) << 8) | sound_ram[addr + 1u]);
                memval = nofl ? (static_cast<int32_t>(w) << 8) : dsp_unpack(w);
            }
            if (mwt) {
                const uint16_t w = nofl ? static_cast<uint16_t>(shifted >> 8) : dsp_pack(shifted);
                sound_ram[addr] = static_cast<uint8_t>(w >> 8);
                sound_ram[addr + 1u] = static_cast<uint8_t>(w & 0xFFu);
            }
        }
        if (adrl) adrs = (shift == 3u) ? ((static_cast<uint32_t>(shifted) >> 12) & 0xFFFu)
                                       : (static_cast<uint32_t>(in) >> 16);
        if (ewt) d.efreg[ewa] = static_cast<int16_t>(d.efreg[ewa] + (shifted >> 8));
    }
    --d.dec;
    std::memset(d.mixs, 0, sizeof(d.mixs));
}

void StvScspState::irq_update(bool& out_main_irq_rise) {
    uint32_t m = scipd & scieb;
    if (m & ~0xFFu) m = (m & 0xFFu) | 0x80u;
    int level = 0;
    for (int b = 0; b < 8; ++b) {
        if ((m >> b) & 1u) {
            const int lv = static_cast<int>(
                ((scilv[0] >> b) & 1u) |
                (((scilv[1] >> b) & 1u) << 1) |
                (((scilv[2] >> b) & 1u) << 2));
            if (lv > level) level = lv;
        }
    }
    cpu_level = level;
    const bool cur_main = (mcipd & mcieb) != 0u;
    if (cur_main && !main_line) out_main_irq_rise = true;
    main_line = cur_main;
}

void StvScspState::pend(uint32_t bits) {
    scipd |= bits;
    mcipd |= bits;
}

uint16_t StvScspState::monitor() {
    const Slot& s = slots[(com[4] >> 11) & 31u];
    const uint32_t ca = static_cast<uint32_t>(s.pos >> (kShift + 12)) & 0xFu;
    const uint32_t sgc = s.active ? static_cast<uint32_t>(s.eg_state) : static_cast<uint32_t>(EG_RELEASE);
    const uint32_t eg = (0x1Fu - static_cast<uint32_t>(s.eg_vol >> (kEgShift + 5))) & 0x1Fu;
    return static_cast<uint16_t>((com[4] & 0xF800u) | (ca << 7) | (sgc << 5) | eg);
}

void StvScspState::dma(uint8_t* sound_ram) {
    const uint32_t mem = ((static_cast<uint32_t>(com[0x14 / 2] & 0xF000u) << 4) |
                          (com[0x12 / 2] & 0xFFFEu)) & (kSoundRamMask & ~1u);
    const uint32_t reg = com[0x14 / 2] & 0x0FFEu;
    const uint32_t len = com[0x16 / 2] & 0x0FFEu;
    const bool to_mem = (com[0x16 / 2] & 0x2000u) != 0u;
    const bool gate = (com[0x16 / 2] & 0x4000u) != 0u;
    for (uint32_t i = 0u; i < len; i += 2u) {
        if (to_mem) {
            const uint16_t v = gate ? 0u : static_cast<uint16_t>(read((reg + i) & 0xFFEu, 2));
            if (sound_ram) {
                sound_ram[(mem + i) & kSoundRamMask] = static_cast<uint8_t>(v >> 8);
                sound_ram[(mem + i + 1u) & kSoundRamMask] = static_cast<uint8_t>(v & 0xFFu);
            }
        } else {
            uint16_t v = 0u;
            if (!gate && sound_ram) {
                const uint32_t a = (mem + i) & (kSoundRamMask & ~1u);
                v = static_cast<uint16_t>((static_cast<uint16_t>(sound_ram[a]) << 8) | sound_ram[a + 1u]);
            }
            write((reg + i) & 0xFFEu, v, 2, sound_ram);
        }
    }
    com[0x16 / 2] &= static_cast<uint16_t>(~0x1000u);
    pend(0x10u);
    bool dummy_rise = false;
    irq_update(dummy_rise);
}

uint16_t StvScspState::read16(uint32_t off) {
    if (off < 0x400u) {
        const Slot& s = slots[off >> 5];
        const int w = static_cast<int>((off >> 1) & 15u);
        return (w == 0) ? static_cast<uint16_t>(s.r[0] & ~0x1000u) : ((w < 12) ? s.r[w] : 0u);
    }
    if (off < 0x430u) {
        switch (off) {
        case 0x404u: return 0x0900u;
        case 0x408u: return monitor();
        case 0x418u:
        case 0x41Au:
        case 0x41Cu: return 0u;
        case 0x41Eu: return static_cast<uint16_t>(scieb);
        case 0x420u: return static_cast<uint16_t>(scipd);
        case 0x42Au: return static_cast<uint16_t>(mcieb);
        case 0x42Cu: return static_cast<uint16_t>(mcipd);
        case 0x422u:
        case 0x424u:
        case 0x426u:
        case 0x428u:
        case 0x42Eu: return 0u;
        default: break;
        }
        return com[(off - 0x400u) >> 1];
    }
    if (off >= 0x600u && off < 0x680u) return static_cast<uint16_t>(stack[(off - 0x600u) >> 1]);
    if (off >= 0x700u && off < 0x780u) return static_cast<uint16_t>(dsp.coef[(off - 0x700u) >> 1]);
    if (off >= 0x780u && off < 0x800u) return dsp.madrs[((off - 0x780u) >> 1) & 31u];
    if (off >= 0x800u && off < 0xC00u) return dsp.mpro[(off - 0x800u) >> 1];
    auto half = [&](int32_t v) -> uint16_t {
        return static_cast<uint16_t>((off & 2u) ? (static_cast<uint32_t>(v) & 0xFFFFu)
                                                : (static_cast<uint32_t>(v) >> 16));
    };
    if (off >= 0xC00u && off < 0xE00u) return half(dsp.temp[(off >> 2) & 0x7Fu]);
    if (off >= 0xE00u && off < 0xE80u) return half(dsp.mems[(off >> 2) & 0x1Fu]);
    if (off >= 0xE80u && off < 0xEC0u) return half(dsp.mixs[(off >> 2) & 0x0Fu]);
    if (off >= 0xEC0u && off < 0xEE0u) return static_cast<uint16_t>(dsp.efreg[(off - 0xEC0u) >> 1]);
    if (off >= 0xEE0u && off < 0xEE4u) return static_cast<uint16_t>(dsp.exts[(off - 0xEE0u) >> 1]);
    return 0u;
}

void StvScspState::write16(uint32_t off, uint16_t v, uint16_t mask, uint8_t* sound_ram) {
    ensure_tables_initialized();
    if (off < 0x400u) {
        Slot& s = slots[off >> 5];
        const int w = static_cast<int>((off >> 1) & 15u);
        if (w >= 12) return;
        s.r[w] = static_cast<uint16_t>((s.r[w] & ~mask) | (v & mask));
        if (w == 0 && (s.r[0] & 0x1000u)) {
            s.r[0] &= static_cast<uint16_t>(~0x1000u);
            for (Slot& t : slots) {
                const bool kb = (t.r[0] & 0x0800u) != 0u;
                if (kb && !t.keyed) key_on(t);
                else if (!kb && t.keyed) key_off(t);
                t.keyed = kb;
            }
        }
        if (w == 8 || w == 4 || w == 5) compute_eg(s);
        if (w == 9) compute_lfo(s);
        return;
    }
    if (off < 0x430u) {
        uint16_t& r = com[(off - 0x400u) >> 1];
        const uint16_t nv = static_cast<uint16_t>((r & ~mask) | (v & mask));
        bool rise = false;
        switch (off) {
        case 0x402u:
            r = nv;
            rbl = 8192u << ((nv >> 7) & 3u);
            rbp = nv & 0x3Fu;
            return;
        case 0x408u:
            r = static_cast<uint16_t>(nv & 0xF800u);
            return;
        case 0x416u:
            r = nv;
            if (nv & 0x1000u) dma(sound_ram);
            return;
        case 0x418u:
        case 0x41Au:
        case 0x41Cu: {
            Timer& t = timers[(off - 0x418u) >> 1];
            if (mask & 0xFF00u) t.ctl = (v >> 8) & 7;
            if (mask & 0x00FFu) t.reload = v & 0xFF;
            return;
        }
        case 0x41Eu: scieb = nv & 0x7FFu; irq_update(rise); return;
        case 0x420u: scipd |= (v & mask & 0x20u); irq_update(rise); return;
        case 0x422u: scipd &= ~static_cast<uint32_t>(v & mask); irq_update(rise); return;
        case 0x424u:
        case 0x426u:
        case 0x428u: {
            uint32_t& l = scilv[(off - 0x424u) >> 1];
            l = ((l & ~mask) | (v & mask)) & 0xFFu;
            irq_update(rise);
            return;
        }
        case 0x42Au: mcieb = nv & 0x7FFu; irq_update(rise); return;
        case 0x42Cu: mcipd |= (v & mask & 0x20u); irq_update(rise); return;
        case 0x42Eu: mcipd &= ~static_cast<uint32_t>(v & mask); irq_update(rise); return;
        default: break;
        }
        r = nv;
        return;
    }
    auto merge = [&](uint16_t old_v) -> uint16_t {
        return static_cast<uint16_t>((old_v & ~mask) | (v & mask));
    };
    if (off >= 0x600u && off < 0x680u) {
        int32_t& x = stack[(off - 0x600u) >> 1];
        x = static_cast<int16_t>(merge(static_cast<uint16_t>(x)));
        return;
    }
    if (off >= 0x700u && off < 0x780u) {
        int16_t& x = dsp.coef[(off - 0x700u) >> 1];
        x = static_cast<int16_t>(merge(static_cast<uint16_t>(x)));
        return;
    }
    if (off >= 0x780u && off < 0x800u) {
        uint16_t& x = dsp.madrs[((off - 0x780u) >> 1) & 31u];
        x = merge(x);
        return;
    }
    if (off >= 0x800u && off < 0xC00u) {
        uint16_t& x = dsp.mpro[(off - 0x800u) >> 1];
        x = merge(x);
        dsp_program_changed();
        return;
    }
    auto half = [&](int32_t& x) {
        uint32_t u = static_cast<uint32_t>(x);
        u = (off & 2u)
            ? ((u & 0xFFFF0000u) | merge(static_cast<uint16_t>(u)))
            : ((u & 0x0000FFFFu) | (static_cast<uint32_t>(merge(static_cast<uint16_t>(u >> 16))) << 16));
        x = static_cast<int32_t>(u);
    };
    if (off >= 0xC00u && off < 0xE00u) { half(dsp.temp[(off >> 2) & 0x7Fu]); return; }
    if (off >= 0xE00u && off < 0xE80u) { half(dsp.mems[(off >> 2) & 0x1Fu]); return; }
    if (off >= 0xE80u && off < 0xEC0u) { half(dsp.mixs[(off >> 2) & 0x0Fu]); return; }
    if (off >= 0xEC0u && off < 0xEE0u) {
        int16_t& x = dsp.efreg[(off - 0xEC0u) >> 1];
        x = static_cast<int16_t>(merge(static_cast<uint16_t>(x)));
        return;
    }
}

uint32_t StvScspState::read(uint32_t off, int size) {
    off &= 0xFFFu;
    if (size == 4) {
        return (static_cast<uint32_t>(read16(off)) << 16) | read16((off + 2u) & 0xFFEu);
    }
    const uint16_t w = read16(off & ~1u);
    if (size == 2) return w;
    return (off & 1u) ? (w & 0xFFu) : (w >> 8);
}

void StvScspState::write(uint32_t off, uint32_t v, int size, uint8_t* sound_ram) {
    off &= 0xFFFu;
    if (size == 4) {
        write16(off, static_cast<uint16_t>(v >> 16), 0xFFFFu, sound_ram);
        write16((off + 2u) & 0xFFEu, static_cast<uint16_t>(v & 0xFFFFu), 0xFFFFu, sound_ram);
        return;
    }
    if (size == 2) {
        write16(off, static_cast<uint16_t>(v & 0xFFFFu), 0xFFFFu, sound_ram);
        return;
    }
    if (off & 1u) write16(off & ~1u, static_cast<uint16_t>(v & 0xFFu), 0x00FFu, sound_ram);
    else write16(off, static_cast<uint16_t>((v & 0xFFu) << 8), 0xFF00u, sound_ram);
}

uint32_t StvScspState::active_slots_mask() const {
    uint32_t m = 0u;
    for (int i = 0; i < 32; ++i) {
        if (slots[i].active) m |= (1u << i);
    }
    return m;
}

void StvScspState::step_sample(uint8_t* sound_ram, int16_t out_lr[2], bool& out_main_irq_rise) {
    ensure_tables_initialized();
    for (int ti = 0; ti < 3; ++ti) {
        Timer& t = timers[ti];
        if ((samples & ((1u << t.ctl) - 1u)) != 0u) continue;
        if (t.reload >= 0) {
            t.counter = t.reload;
            t.reload = -1;
        } else {
            t.counter = (t.counter + 1) & 0xFF;
        }
        if (t.counter == 0xFF) pend(0x40u << ti);
    }
    pend(0x400u);
    irq_update(out_main_irq_rise);
    ++samples;

    float l = 0.0f;
    float r = 0.0f;
    const bool has_dsp = (dsp.last_step > 0);
    for (Slot& s : slots) {
        int32_t stacked = 0;
        if (s.active) {
            const int32_t v = slot_sample(s, sound_ram);
            const float tl = g_tl_gain[s.tl()];
            const float lv = static_cast<float>(v) * tl;
            l += lv * g_sdl_gain[s.disdl()] * g_pan_l[s.dipan()];
            r += lv * g_sdl_gain[s.disdl()] * g_pan_r[s.dipan()];
            if (has_dsp && s.imxl()) {
                dsp.mixs[s.isel()] += static_cast<int32_t>(lv * g_sdl_gain[s.imxl()] * 16.0f);
            }
            stacked = static_cast<int32_t>(2.0f * (s.sdir() ? static_cast<float>(v) : lv));
        }
        if (!s.stwinh()) stack[stack_pos] = stacked;
        stack_pos = (stack_pos + 1u) & 63u;
    }
    dsp.exts[0] = 0;
    dsp.exts[1] = 0;
    if (has_dsp) {
        dsp_step(sound_ram);
        for (int i = 0; i < 16; ++i) {
            const Slot& s = slots[i];
            if (!s.efsdl()) continue;
            l += static_cast<float>(dsp.efreg[i]) * g_sdl_gain[s.efsdl()] * g_pan_l[s.efpan()];
            r += static_cast<float>(dsp.efreg[i]) * g_sdl_gain[s.efsdl()] * g_pan_r[s.efpan()];
        }
    }
    const float mv = g_mvol[com[0] & 15u];
    const int32_t il = static_cast<int32_t>(l * mv);
    const int32_t ir = static_cast<int32_t>(r * mv);
    out_lr[0] = static_cast<int16_t>(std::max(-32768, std::min(32767, il)));
    out_lr[1] = static_cast<int16_t>(std::max(-32768, std::min(32767, ir)));
}

}  // namespace twc98
