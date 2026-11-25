// Portable zero-heap Sega ST-V VDP1 + VDP2 software frame producer implementation.
// Derived from saturnkit runtime (https://github.com/vs-sr-dev/saturnkit,
// commit 1b345ef949e1fa258ce3f693f03c0df8acdfb40d, MIT License) and
// .local/experiments/010-output-consumer/vdp1-experiment.cpp.
#include "stv_vdp.hpp"
#include "twc98_peru_3d_models.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace twc98 {
namespace {

struct Pt {
    int x = 0;
    int y = 0;
};

struct Rgb5 {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct V1Cmd {
    uint16_t ctrl = 0u, pmod = 0u, colr = 0u;
    uint32_t srca = 0u;
    int w = 0, h = 0;
    int cmode = 0, ccb = 0;
    bool textured = false, spd = false, ecd = false, mesh = false;
    bool clip = false, outside = false, mon = false, gouraud = false;
    bool hflip = false, vflip = false;
    uint16_t lut[16]{};
    Rgb5 g[4]{};
};

struct Pix {
    uint32_t rgb = 0u;
    uint8_t prio = 0u;
    uint8_t ratio = 0u;
    bool cc = false;
    bool offset = false;
    bool offset_b = false;
    bool spr = false;
    bool scc = false;
    bool msb = false;
    bool direct = false;
    uint16_t dcc = 0u;
};

enum { L_SPRITE, L_RBG0, L_NBG0, L_NBG1, L_NBG2, L_NBG3, L_COUNT };

inline uint16_t cw(const uint8_t* cmd, int o) {
    return static_cast<uint16_t>((cmd[o] << 8) | cmd[o + 1]);
}

inline int sx13(uint16_t v) {
    return static_cast<int32_t>(static_cast<uint32_t>(v) << 19) >> 19;
}

inline int sx11(uint16_t v) {
    return static_cast<int32_t>(static_cast<uint32_t>(v) << 21) >> 21;
}

inline Rgb5 rgb5_of(uint16_t v) {
    return {v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F};
}

inline Rgb5 mix_rgb5(const Rgb5& a, const Rgb5& b, int k, int n) {
    if (n <= 0) return a;
    return {
        a.r + (b.r - a.r) * k / n,
        a.g + (b.g - a.g) * k / n,
        a.b + (b.b - a.b) * k / n
    };
}

inline int step_to(int a, int b, int i, int n) {
    if (n <= 0) return a;
    const int d = (b - a) * i * 2;
    return a + (d >= 0 ? (d + n) / (2 * n) : -((-d + n) / (2 * n)));
}

inline uint32_t rgb555_to_888(uint16_t v) {
    return (static_cast<uint32_t>((v & 0x1F) << 3) << 16) |
           (static_cast<uint32_t>(((v >> 5) & 0x1F) << 3) << 8) |
           static_cast<uint32_t>(((v >> 10) & 0x1F) << 3);
}

inline int32_t sext_bits(uint32_t v, int bits) {
    return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits);
}

bool v1_texel(const StvVdpState& st, const V1Cmd& c, int u, int v, uint16_t& out, bool& end) {
    if (c.hflip) u = c.w - 1 - u;
    if (c.vflip) v = c.h - 1 - v;
    const uint32_t i = static_cast<uint32_t>(v * c.w + u);
    uint32_t raw = 0u;
    bool is_end = false;
    bool is_zero = false;
    switch (c.cmode) {
        case 0:
        case 1: {
            const uint8_t b = st.vdp1_vram[(c.srca + i / 2u) & 0x7FFFFu];
            raw = (i & 1u) ? (b & 0xFu) : (b >> 4);
            is_end = (raw == 0xFu);
            is_zero = (raw == 0u);
            out = (c.cmode == 0) ? static_cast<uint16_t>((c.colr & 0xFFF0u) | raw) : c.lut[raw];
            break;
        }
        case 2:
        case 3:
        case 4: {
            raw = st.vdp1_vram[(c.srca + i) & 0x7FFFFu];
            is_end = (raw == 0xFFu);
            is_zero = (raw == 0u);
            static constexpr uint16_t kMask[] = {0x3Fu, 0x7Fu, 0xFFu};
            const uint16_t m = kMask[c.cmode - 2];
            out = static_cast<uint16_t>((c.colr & ~m) | (raw & m));
            break;
        }
        default: {
            const uint32_t a = (c.srca + i * 2u) & 0x7FFFEu;
            raw = static_cast<uint16_t>((st.vdp1_vram[a] << 8) | st.vdp1_vram[a + 1u]);
            is_end = (raw == 0x7FFFu);
            is_zero = (raw == 0u);
            out = static_cast<uint16_t>(raw);
            break;
        }
    }
    end = false;
    if (is_end && !c.ecd) {
        end = true;
        return false;
    }
    return !(is_zero && !c.spd);
}

bool v1_outside_clip(const StvVdpState& st, const V1Cmd& c, int x0, int y0, int x1, int y1) {
    const int mw = (st.vdp1_regs[StvVdpState::TVMR] & 1u) ? 1024 : 512;
    const int mh = (st.vdp1_regs[StvVdpState::TVMR] & 1u) ? 512 : 256;
    if (x1 < 0 || y1 < 0 || x0 > std::min(st.sys_x, mw - 1) || y0 > std::min(st.sys_y, mh - 1)) {
        return true;
    }
    return c.clip && !c.outside && (x1 < st.ux0 || y1 < st.uy0 || x0 > st.ux1 || y0 > st.uy1);
}

void v1_plot(StvVdpState& st, const V1Cmd& c, int x, int y, uint16_t pix, const Rgb5* g) {
    const int width = (st.vdp1_regs[StvVdpState::TVMR] & 1u) ? 1024 : 512;
    const int height = (st.vdp1_regs[StvVdpState::TVMR] & 1u) ? 512 : 256;
    if (x < 0 || y < 0 || x > st.sys_x || y > st.sys_y || x >= width || y >= height) return;
    if (c.clip) {
        const bool in = (x >= st.ux0 && x <= st.ux1 && y >= st.uy0 && y <= st.uy1);
        if (in == c.outside) return;
    }
    if (c.mesh && ((x ^ y) & 1)) return;
    uint16_t& d = st.vdp1_fb[st.draw_fb][y * width + x];
    if (st.vdp1_regs[StvVdpState::TVMR] & 1u) {
        if (c.mon || c.gouraud || (c.ccb & 3)) {
            st.vdp1_unsupported = true;
            return;
        }
        d = pix & 0xFFu;
        return;
    }
    if (c.mon) {
        d |= 0x8000u;
        return;
    }
    if (g && (pix & 0x8000u)) {
        const int r = std::clamp((pix & 0x1F) + g->r - 16, 0, 31);
        const int gg = std::clamp(((pix >> 5) & 0x1F) + g->g - 16, 0, 31);
        const int b = std::clamp(((pix >> 10) & 0x1F) + g->b - 16, 0, 31);
        pix = static_cast<uint16_t>(0x8000 | (b << 10) | (gg << 5) | r);
    }
    switch (c.ccb & 3) {
        case 0: d = pix; break;
        case 1:
            if (d & 0x8000u) d = static_cast<uint16_t>(((d >> 1) & 0x3DEFu) | 0x8000u);
            break;
        case 2:
            d = (pix & 0x8000u) ? static_cast<uint16_t>(((pix >> 1) & 0x3DEFu) | 0x8000u) : pix;
            break;
        case 3:
            if ((d & 0x8000u) && (pix & 0x8000u)) {
                d = static_cast<uint16_t>((static_cast<uint32_t>(pix) + d - ((pix ^ d) & 0x8421u)) >> 1);
            } else {
                d = pix;
            }
            break;
    }
}

void v1_line(StvVdpState& st, const V1Cmd& c, Pt p0, Pt p1, int v, Rgb5 g0, Rgb5 g1) {
    if (v1_outside_clip(st, c, std::min(p0.x, p1.x), std::min(p0.y, p1.y),
                        std::max(p0.x, p1.x), std::max(p0.y, p1.y))) {
        return;
    }
    const int dx = p1.x - p0.x, dy = p1.y - p0.y;
    const int adx = std::abs(dx), ady = std::abs(dy);
    const int len = std::max(adx, ady);
    const int xi = (dx < 0) ? -1 : 1;
    const int yi = (dy < 0) ? -1 : 1;
    const int denom = len + 1;

    // Fast horizontal scanline path (comm 0x0/0x1 sprites & axis-aligned quads: 0 divisions, 1x row setup)
    if (dy == 0 && !c.gouraud && !c.mon && !(st.vdp1_regs[StvVdpState::TVMR] & 1u)) {
        const int y = p0.y;
        const int width = 512;
        const int height = 256;
        if (y < 0 || y > st.sys_y || y >= height) return;
        if (c.clip && !c.outside && (y < st.uy0 || y > st.uy1)) return;
        int x_lo = 0;
        int x_hi = std::min(st.sys_x, width - 1);
        if (c.clip && !c.outside) {
            if (st.ux0 > x_lo) x_lo = st.ux0;
            if (st.ux1 < x_hi) x_hi = st.ux1;
        }
        if (x_lo > x_hi) return;
        const bool need_hole = c.clip && c.outside && (y >= st.uy0 && y <= st.uy1);
        const int hole_x0 = st.ux0;
        const int hole_x1 = st.ux1;
        const bool mesh = c.mesh;
        const int ccb3 = c.ccb & 3;
        uint16_t* const fb_row = st.vdp1_fb[st.draw_fb] + y * width;

        auto plot_h = [&](int px, uint16_t pix) {
            if (px < x_lo || px > x_hi) return;
            if (need_hole && px >= hole_x0 && px <= hole_x1) return;
            if (mesh && ((px ^ y) & 1)) return;
            uint16_t& d = fb_row[px];
            if (ccb3 == 0) {
                d = pix;
            } else if (ccb3 == 1) {
                if (d & 0x8000u) d = static_cast<uint16_t>(((d >> 1) & 0x3DEFu) | 0x8000u);
            } else if (ccb3 == 2) {
                d = (pix & 0x8000u) ? static_cast<uint16_t>(((pix >> 1) & 0x3DEFu) | 0x8000u) : pix;
            } else {
                if ((d & 0x8000u) && (pix & 0x8000u)) {
                    d = static_cast<uint16_t>((static_cast<uint32_t>(pix) + d - ((pix ^ d) & 0x8421u)) >> 1);
                } else {
                    d = pix;
                }
            }
        };

        int x = p0.x;
        if (!c.textured) {
            const uint16_t pix = c.colr;
            if (ccb3 == 0 && !mesh && !need_hole) {
                int lx = std::min(p0.x, p1.x);
                int rx = std::max(p0.x, p1.x);
                if (lx < x_lo) lx = x_lo;
                if (rx > x_hi) rx = x_hi;
                for (int px = lx; px <= rx; ++px) fb_row[px] = pix;
            } else {
                for (int k = 0; k <= len; ++k, x += xi) plot_h(x, pix);
            }
            return;
        }

        const uint8_t* const vram = st.vdp1_vram;
        const int v_eff = c.vflip ? (c.h - 1 - v) : v;
        const uint32_t row_base_px = static_cast<uint32_t>(v_eff * c.w);
        const bool hflip = c.hflip;
        const int cw_m1 = c.w - 1;
        const bool spd = c.spd;
        const bool ecd = c.ecd;
        const uint32_t srca = c.srca;
        int cur_u = 0;
        int u_rem = 0;
        int ends = 0;

        if (c.cmode <= 1) {
            const bool is_lut = (c.cmode == 1);
            const uint16_t colr_hi = static_cast<uint16_t>(c.colr & 0xFFF0u);
            for (int k = 0; k <= len; ++k, x += xi) {
                const uint32_t u_eff = static_cast<uint32_t>(hflip ? (cw_m1 - cur_u) : cur_u);
                const uint32_t idx = row_base_px + u_eff;
                const uint8_t b = vram[(srca + (idx >> 1)) & 0x7FFFFu];
                const uint32_t raw = (idx & 1u) ? (b & 0xFu) : (b >> 4);
                if (raw == 0xFu && !ecd) {
                    if (++ends == 2) break;
                } else if (raw != 0u || spd) {
                    const uint16_t pix = is_lut ? c.lut[raw] : static_cast<uint16_t>(colr_hi | raw);
                    plot_h(x, pix);
                }
                u_rem += c.w;
                while (u_rem >= denom) { u_rem -= denom; ++cur_u; }
            }
        } else if (c.cmode <= 4) {
            static constexpr uint16_t kMask[] = {0x3Fu, 0x7Fu, 0xFFu};
            const uint16_t m = kMask[c.cmode - 2];
            const uint16_t colr_hi = static_cast<uint16_t>(c.colr & ~m);
            for (int k = 0; k <= len; ++k, x += xi) {
                const uint32_t u_eff = static_cast<uint32_t>(hflip ? (cw_m1 - cur_u) : cur_u);
                const uint32_t raw = vram[(srca + row_base_px + u_eff) & 0x7FFFFu];
                if (raw == 0xFFu && !ecd) {
                    if (++ends == 2) break;
                } else if (raw != 0u || spd) {
                    plot_h(x, static_cast<uint16_t>(colr_hi | (raw & m)));
                }
                u_rem += c.w;
                while (u_rem >= denom) { u_rem -= denom; ++cur_u; }
            }
        } else {
            for (int k = 0; k <= len; ++k, x += xi) {
                const uint32_t u_eff = static_cast<uint32_t>(hflip ? (cw_m1 - cur_u) : cur_u);
                const uint32_t a = (srca + ((row_base_px + u_eff) << 1)) & 0x7FFFEu;
                const uint16_t raw = static_cast<uint16_t>((vram[a] << 8) | vram[a + 1u]);
                if (raw == 0x7FFFu && !ecd) {
                    if (++ends == 2) break;
                } else if (raw != 0u || spd) {
                    plot_h(x, raw);
                }
                u_rem += c.w;
                while (u_rem >= denom) { u_rem -= denom; ++cur_u; }
            }
        }
        return;
    }

    int ends = 0;
    bool stop = false;
    int cur_u = 0;
    int u_rem = 0;
    auto pixel = [&](int x, int y, int k) {
        if (stop) return;
        uint16_t pix = c.colr;
        if (c.textured) {
            bool end = false;
            if (!v1_texel(st, c, cur_u, v, pix, end)) {
                if (end && ++ends == 2) stop = true;
                return;
            }
        }
        Rgb5 g{};
        if (c.gouraud) g = mix_rgb5(g0, g1, k, len);
        v1_plot(st, c, x, y, pix, c.gouraud ? &g : nullptr);
    };
    int x = p0.x, y = p0.y;
    if (adx >= ady) {
        int err = 2 * ady - adx;
        for (int k = 0; k <= len; ++k) {
            pixel(x, y, k);
            if (k == len) break;
            if (err > 0) {
                y += yi;
                err -= 2 * adx;
                pixel(x, y, k);
            }
            err += 2 * ady;
            x += xi;
            if (c.textured) {
                u_rem += c.w;
                while (u_rem >= denom) { u_rem -= denom; ++cur_u; }
            }
        }
    } else {
        int err = 2 * adx - ady;
        for (int k = 0; k <= len; ++k) {
            pixel(x, y, k);
            if (k == len) break;
            if (err > 0) {
                x += xi;
                err -= 2 * ady;
                pixel(x, y, k);
            }
            err += 2 * adx;
            y += yi;
            if (c.textured) {
                u_rem += c.w;
                while (u_rem >= denom) { u_rem -= denom; ++cur_u; }
            }
        }
    }
}

void v1_quad(StvVdpState& st, const V1Cmd& c, Pt a, Pt b, Pt cc, Pt d) {
    if (v1_outside_clip(st, c, std::min({a.x, b.x, cc.x, d.x}), std::min({a.y, b.y, cc.y, d.y}),
                        std::max({a.x, b.x, cc.x, d.x}), std::max({a.y, b.y, cc.y, d.y}))) {
        return;
    }
    const int left = std::max(std::abs(d.x - a.x), std::abs(d.y - a.y));
    const int right = std::max(std::abs(cc.x - b.x), std::abs(cc.y - b.y));
    const int n = std::max(left, right);
    if (n > 2048) return;
    for (int i = 0; i <= n; ++i) {
        const Pt l{step_to(a.x, d.x, i, n), step_to(a.y, d.y, i, n)};
        const Pt r{step_to(b.x, cc.x, i, n), step_to(b.y, cc.y, i, n)};
        const int v = c.textured ? static_cast<int>(static_cast<int64_t>(i) * c.h / (n + 1)) : 0;
        Rgb5 gl{}, gr{};
        if (c.gouraud) {
            gl = mix_rgb5(c.g[0], c.g[3], i, n);
            gr = mix_rgb5(c.g[1], c.g[2], i, n);
        }
        v1_line(st, c, l, r, v, gl, gr);
    }
}

void v1_command(StvVdpState& st, const uint8_t* cmd, bool meta_only = false) {
    V1Cmd c{};
    c.ctrl = cw(cmd, 0);
    c.pmod = cw(cmd, 4);
    c.colr = cw(cmd, 6);
    c.srca = static_cast<uint32_t>(cw(cmd, 8)) * 8u;
    const uint16_t size = cw(cmd, 10);
    c.w = ((size >> 8) & 0x3F) * 8;
    c.h = size & 0xFF;
    const int comm = c.ctrl & 0xF;
    Pt p[4];
    for (int i = 0; i < 4; ++i) {
        p[i] = {sx13(cw(cmd, 12 + i * 4)), sx13(cw(cmd, 14 + i * 4))};
    }
    switch (comm) {
        case 0x8:
        case 0xB:
            st.ux0 = cw(cmd, 12) & 0x3FF;
            st.uy0 = cw(cmd, 14) & 0x1FF;
            st.ux1 = cw(cmd, 20) & 0x3FF;
            st.uy1 = cw(cmd, 22) & 0x1FF;
            return;
        case 0x9:
            st.sys_x = cw(cmd, 20) & 0x3FF;
            st.sys_y = cw(cmd, 22) & 0x1FF;
            return;
        case 0xA:
            st.local_x = sx11(cw(cmd, 12));
            st.local_y = sx11(cw(cmd, 14));
            return;
    }
    if (comm > 7) return;
    c.cmode = (c.pmod >> 3) & 7;
    c.ccb = c.pmod & 7;
    c.spd = (c.pmod & 0x40u) != 0u;
    c.ecd = (c.pmod & 0x80u) != 0u;
    c.mesh = (c.pmod & 0x100u) != 0u;
    c.clip = (c.pmod & 0x400u) != 0u;
    c.outside = (c.pmod & 0x200u) != 0u;
    c.mon = (c.pmod & 0x8000u) != 0u;
    c.gouraud = (c.ccb & 4) != 0;
    c.hflip = (c.ctrl & 0x10u) != 0u;
    c.vflip = (c.ctrl & 0x20u) != 0u;
    c.textured = comm < 4;
    if (c.textured && (c.w == 0 || c.h == 0)) return;
    const uint32_t gouraud_addr = c.gouraud ? (static_cast<uint32_t>(cw(cmd, 28)) * 8u) : 0u;
    if (!meta_only) {
        if (c.textured && c.cmode == 1) {
            for (int i = 0; i < 16; ++i) {
                const uint32_t a = (static_cast<uint32_t>(c.colr) * 8u + static_cast<uint32_t>(i * 2)) & 0x7FFFEu;
                c.lut[i] = static_cast<uint16_t>((st.vdp1_vram[a] << 8) | st.vdp1_vram[a + 1u]);
            }
        }
        if (c.gouraud) {
            for (int i = 0; i < 4; ++i) {
                const uint32_t a = (gouraud_addr + static_cast<uint32_t>(i * 2)) & 0x7FFFEu;
                c.g[i] = rgb5_of(static_cast<uint16_t>((st.vdp1_vram[a] << 8) | st.vdp1_vram[a + 1u]));
            }
        }
    }
    for (Pt& q : p) {
        q.x += st.local_x;
        q.y += st.local_y;
    }
    auto push_gpu_cmd = [&](Pt q0, Pt q1, Pt q2, Pt q3) {
        const int minx = (comm == 0x6) ? std::min(q0.x, q1.x) : std::min({q0.x, q1.x, q2.x, q3.x});
        const int miny = (comm == 0x6) ? std::min(q0.y, q1.y) : std::min({q0.y, q1.y, q2.y, q3.y});
        const int maxx = (comm == 0x6) ? std::max(q0.x, q1.x) : std::max({q0.x, q1.x, q2.x, q3.x});
        const int maxy = (comm == 0x6) ? std::max(q0.y, q1.y) : std::max({q0.y, q1.y, q2.y, q3.y});
        if (v1_outside_clip(st, c, minx, miny, maxx, maxy)) return;
        auto& gf = st.v1_gpu_frames[st.draw_fb];
        if (gf.count >= StvVdpState::kMaxV1GpuCmds) return;
        StvVdpState::V1GpuCmd& gc = gf.cmds[gf.count++];
        gc.ctrl = c.ctrl;
        gc.pmod = c.pmod;
        gc.colr = c.colr;
        gc._pad = 0u;
        gc.srca = c.srca;
        gc.gouraud_addr = gouraud_addr;
        gc.w = static_cast<int16_t>(c.w);
        gc.h = static_cast<int16_t>(c.h);
        gc.x[0] = static_cast<int16_t>(q0.x);
        gc.x[1] = static_cast<int16_t>(q1.x);
        gc.x[2] = static_cast<int16_t>(q2.x);
        gc.x[3] = static_cast<int16_t>(q3.x);
        gc.y[0] = static_cast<int16_t>(q0.y);
        gc.y[1] = static_cast<int16_t>(q1.y);
        gc.y[2] = static_cast<int16_t>(q2.y);
        gc.y[3] = static_cast<int16_t>(q3.y);
        gc.ux0 = static_cast<int16_t>(st.ux0);
        gc.uy0 = static_cast<int16_t>(st.uy0);
        gc.ux1 = static_cast<int16_t>(st.ux1);
        gc.uy1 = static_cast<int16_t>(st.uy1);
        gc.sys_x = static_cast<int16_t>(st.sys_x);
        gc.sys_y = static_cast<int16_t>(st.sys_y);
    };
    auto handle_copa_sp_replace = [&](int x0, int y0, int x1, int y1, bool has_poly) -> bool {
        if (!st.copa_vfx_replace) return false;
        const int bx0 = std::min(x0, x1);
        const int by0 = std::min(y0, y1);
        const int bx1 = std::max(x0, x1);
        const int by1 = std::max(y0, y1);
        const int16_t cx = static_cast<int16_t>((bx0 + bx1) / 2);
        const int16_t cy = static_cast<int16_t>((by0 + by1) / 2);
        auto merge_box = [&](StvVdpState::CopaVfxBox& box) {
            box.active = true;
            box.hflip = c.hflip;
            box.min_x = static_cast<int16_t>(std::min<int>(box.min_x, bx0));
            box.min_y = static_cast<int16_t>(std::min<int>(box.min_y, by0));
            box.max_x = static_cast<int16_t>(std::max<int>(box.max_x, bx1));
            box.max_y = static_cast<int16_t>(std::max<int>(box.max_y, by1));
        };
        // PER SP 24 Heel Lift (ex-Eiffel Tower, TEXGM 0x00D3..0x00D6 -> 0x3F720..0x40740) -> 3D Animated Condor
        if (c.srca == 0x3F720u || c.srca == 0x3F740u || c.srca == 0x40140u || c.srca == 0x40740u) {
            merge_box(st.copa_sp_vfx.draw_condor);
            return true;
        }
        // PER SP 14 / 38 Super Combination (ex-Arc de Triomphe, TEXGM 0x00F3..0x00F9 -> 0x437C0..0x456E0) -> 3D Machu Picchu
        if (c.srca == 0x437C0u || c.srca == 0x43DC0u || c.srca == 0x44040u || c.srca == 0x44A00u ||
            c.srca == 0x44D00u || c.srca == 0x44D20u || c.srca == 0x456E0u) {
            merge_box(st.copa_sp_vfx.draw_machu);
            return true;
        }
        // BOL SP 3 pre-shot converging sparks (TEXGM 0x00FA -> 0x45A40, 0x009A -> 0x35260) & charge orb (0x00D0/0x009B..0x00A2)
        if (st.wind_vfx.active) {
            if (c.srca == 0x45A40u || c.srca == 0x35260u) {
                if (st.copa_sp_vfx.draw_charge_count < StvVdpState::CopaSpVfxState::kMaxChargeRays) {
                    int16_t rx0 = static_cast<int16_t>(x0), ry0 = static_cast<int16_t>(y0);
                    int16_t rx1 = static_cast<int16_t>(x1), ry1 = static_cast<int16_t>(y1);
                    if (has_poly) {
                        const int mx01 = (p[0].x + p[1].x) / 2, my01 = (p[0].y + p[1].y) / 2;
                        const int mx23 = (p[2].x + p[3].x) / 2, my23 = (p[2].y + p[3].y) / 2;
                        const int mx03 = (p[0].x + p[3].x) / 2, my03 = (p[0].y + p[3].y) / 2;
                        const int mx12 = (p[1].x + p[2].x) / 2, my12 = (p[1].y + p[2].y) / 2;
                        const int d01 = (mx23 - mx01) * (mx23 - mx01) + (my23 - my01) * (my23 - my01);
                        const int d03 = (mx12 - mx03) * (mx12 - mx03) + (my12 - my03) * (my12 - my03);
                        if (d01 >= d03) {
                            rx0 = static_cast<int16_t>(mx01); ry0 = static_cast<int16_t>(my01);
                            rx1 = static_cast<int16_t>(mx23); ry1 = static_cast<int16_t>(my23);
                        } else {
                            rx0 = static_cast<int16_t>(mx03); ry0 = static_cast<int16_t>(my03);
                            rx1 = static_cast<int16_t>(mx12); ry1 = static_cast<int16_t>(my12);
                        }
                    }
                    st.copa_sp_vfx.draw_charge_rays[st.copa_sp_vfx.draw_charge_count++] = {rx0, ry0, rx1, ry1};
                }
                return true;
            }
            if (c.srca == 0x3D3A0u || (c.srca >= 0x352E0u && c.srca <= 0x360E0u)) {
                return true;
            }
        }
        // URU SP 20 ex-Germany flag (TEXGM 0x00D8 black anchor 0x41540 + nearby 0x00DA red / 0x00DC gold)
        if (c.srca == 0x41540u) {
            st.copa_sp_vfx.uru_anchor_x = cx;
            st.copa_sp_vfx.uru_anchor_y = cy;
            st.copa_sp_vfx.uru_anchor_ttl = 4u;
            merge_box(st.copa_sp_vfx.draw_uru_flag);
            return true;
        }
        if (st.copa_sp_vfx.uru_anchor_ttl > 0u &&
            (c.srca == 0x41580u || c.srca == 0x415C0u) &&
            std::abs(static_cast<int>(cx) - static_cast<int>(st.copa_sp_vfx.uru_anchor_x)) <= 96 &&
            std::abs(static_cast<int>(cy) - static_cast<int>(st.copa_sp_vfx.uru_anchor_y)) <= 96) {
            --st.copa_sp_vfx.uru_anchor_ttl;
            merge_box(st.copa_sp_vfx.draw_uru_flag);
            return true;
        }
        return false;
    };

    switch (comm) {
        case 0x0: {
            const int x0 = p[0].x, y0 = p[0].y;
            const int x1 = x0 + c.w - 1, y1 = y0 + c.h - 1;
            if (handle_copa_sp_replace(x0, y0, x1, y1, false)) return;
            if (c.w <= 48 && c.h <= 48 && st.v1_obs_draw_count < StvVdpState::kMaxSpriteObs) {
                st.v1_obs_draw[st.v1_obs_draw_count++] = {
                    c.srca, c.pmod, static_cast<int16_t>((x0 + x1) / 2), static_cast<int16_t>((y0 + y1) / 2),
                    static_cast<uint8_t>(c.w), static_cast<uint8_t>(c.h),
                    static_cast<int16_t>(c.w), static_cast<int16_t>(c.h)
                };
            }
            if (meta_only) push_gpu_cmd({x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
            else v1_quad(st, c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
            break;
        }
        case 0x1: {
            const int zp = (c.ctrl >> 8) & 0xF;
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (!zp) {
                x0 = p[0].x; y0 = p[0].y; x1 = p[2].x; y1 = p[2].y;
            } else {
                const int w = sx13(cw(cmd, 16));
                const int h = sx13(cw(cmd, 18));
                switch (zp & 3) {
                    case 2: x0 = p[0].x - w / 2; x1 = x0 + w; break;
                    case 3: x0 = p[0].x - w; x1 = p[0].x; break;
                    default: x0 = p[0].x; x1 = p[0].x + w; break;
                }
                switch ((zp >> 2) & 3) {
                    case 2: y0 = p[0].y - h / 2; y1 = y0 + h; break;
                    case 3: y0 = p[0].y - h; y1 = p[0].y; break;
                    default: y0 = p[0].y; y1 = p[0].y + h; break;
                }
            }
            if (handle_copa_sp_replace(x0, y0, x1, y1, false)) return;
            const int rw = std::abs(x1 - x0), rh = std::abs(y1 - y0);
            const bool is_ball_src = (c.srca == 0x30680u);
            if (c.w <= 48 && c.h <= 48 && rw <= 64 && rh <= 64 &&
                ((is_ball_src && rw >= 1 && rh >= 1) || (rw >= 4 && rh >= 4)) &&
                st.v1_obs_draw_count < StvVdpState::kMaxSpriteObs) {
                st.v1_obs_draw[st.v1_obs_draw_count++] = {
                    c.srca, c.pmod, static_cast<int16_t>((x0 + x1) / 2), static_cast<int16_t>((y0 + y1) / 2),
                    static_cast<uint8_t>(c.w), static_cast<uint8_t>(c.h),
                    static_cast<int16_t>(rw), static_cast<int16_t>(rh)
                };
            }
            if (meta_only) push_gpu_cmd({x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
            else v1_quad(st, c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
            break;
        }
        case 0x2:
        case 0x3: {
            const int minx = std::min({p[0].x, p[1].x, p[2].x, p[3].x});
            const int maxx = std::max({p[0].x, p[1].x, p[2].x, p[3].x});
            const int miny = std::min({p[0].y, p[1].y, p[2].y, p[3].y});
            const int maxy = std::max({p[0].y, p[1].y, p[2].y, p[3].y});
            if (handle_copa_sp_replace(minx, miny, maxx, maxy, true)) return;
            const int rw = maxx - minx, rh = maxy - miny;
            const bool is_ball_src = (c.srca == 0x30680u);
            if (c.w <= 48 && c.h <= 48 && rw <= 64 && rh <= 64 &&
                ((is_ball_src && rw >= 1 && rh >= 1) || (rw >= 4 && rh >= 4)) &&
                st.v1_obs_draw_count < StvVdpState::kMaxSpriteObs) {
                st.v1_obs_draw[st.v1_obs_draw_count++] = {
                    c.srca, c.pmod, static_cast<int16_t>((minx + maxx) / 2), static_cast<int16_t>((miny + maxy) / 2),
                    static_cast<uint8_t>(c.w), static_cast<uint8_t>(c.h),
                    static_cast<int16_t>(rw), static_cast<int16_t>(rh)
                };
            }
            if (meta_only) push_gpu_cmd(p[0], p[1], p[2], p[3]);
            else v1_quad(st, c, p[0], p[1], p[2], p[3]);
            break;
        }
        case 0x4:
            if (meta_only) push_gpu_cmd(p[0], p[1], p[2], p[3]);
            else v1_quad(st, c, p[0], p[1], p[2], p[3]);
            break;
        case 0x5:
        case 0x7:
            if (meta_only) {
                push_gpu_cmd(p[0], p[1], p[2], p[3]);
            } else {
                for (int i = 0; i < 4; ++i) {
                    v1_line(st, c, p[i], p[(i + 1) & 3], 0, c.g[i], c.g[(i + 1) & 3]);
                }
            }
            break;
        case 0x6:
            if (meta_only) push_gpu_cmd(p[0], p[1], p[2], p[3]);
            else v1_line(st, c, p[0], p[1], 0, c.g[0], c.g[1]);
            break;
    }
}

inline uint16_t v2_reg(const StvVdpState& st, uint32_t off) {
    return static_cast<uint16_t>((st.vdp2_regs[off] << 8) | st.vdp2_regs[off + 1u]);
}
inline uint8_t v2_vb(const uint8_t vram[0x80000], uint32_t a) {
    return vram[a & 0x7FFFFu];
}
inline uint16_t v2_vw(const uint8_t vram[0x80000], uint32_t a) {
    a &= 0x7FFFEu;
    return static_cast<uint16_t>((vram[a] << 8) | vram[a + 1u]);
}
inline uint32_t v2_vl(const uint8_t vram[0x80000], uint32_t a) {
    return (static_cast<uint32_t>(v2_vw(vram, a)) << 16) | v2_vw(vram, a + 2u);
}

void build_cram_lut(StvVdpState& st) {
    auto& lut = st.compose_prep;
    const int mode = (v2_reg(st, 0x0Eu) >> 12) & 3;
    if (mode == 2) {
        lut.cram_mask = 0x3FFu;
        for (uint32_t i = 0u; i < 1024u; ++i) {
            const uint32_t a = i * 4u;
            const uint32_t v = (static_cast<uint32_t>(st.vdp2_cram[a]) << 24) |
                               (static_cast<uint32_t>(st.vdp2_cram[a + 1u]) << 16) |
                               (static_cast<uint32_t>(st.vdp2_cram[a + 2u]) << 8) |
                               st.vdp2_cram[a + 3u];
            lut.cram_msb[i] = static_cast<uint8_t>((v >> 31) != 0u);
            lut.cram_rgb[i] = ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
        }
    } else {
        const uint32_t count = (mode == 1) ? 2048u : 1024u;
        lut.cram_mask = count - 1u;
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t a = i * 2u;
            const uint16_t v = static_cast<uint16_t>((st.vdp2_cram[a] << 8) | st.vdp2_cram[a + 1u]);
            lut.cram_msb[i] = static_cast<uint8_t>((v >> 15) != 0u);
            lut.cram_rgb[i] = rgb555_to_888(v);
        }
    }
}

inline uint32_t v2_cram_fast(const StvVdpState::ComposePrepared& lut, uint32_t i, bool* msb) {
    const uint32_t idx = i & lut.cram_mask;
    if (msb) *msb = lut.cram_msb[idx] != 0u;
    return lut.cram_rgb[idx];
}

void v2_setup_line_scroll(const StvVdpState& st, const uint8_t vram[0x80000], int n,
                          StvVdpState::Nbg& s, int lines) {
    const uint8_t sc = static_cast<uint8_t>(v2_reg(st, 0x9Au) >> (n * 8));
    if (!(sc & 0x0Eu)) return;
    s.line = true;
    const int lss = (sc >> 4) & 3;
    uint32_t lsta = ((static_cast<uint32_t>(v2_reg(st, static_cast<uint32_t>(0xA0 + n * 4)) & 7u) << 16) |
                     (v2_reg(st, static_cast<uint32_t>(0xA2 + n * 4)) & 0xFFFEu)) * 2u;
    auto entry = [&](bool zoom) {
        const int32_t v = zoom
            ? static_cast<int32_t>(((v2_vw(vram, lsta) & 7u) << 8) | (v2_vw(vram, lsta + 2u) >> 8))
            : static_cast<int32_t>(((v2_vw(vram, lsta) & 0x7FFu) << 8) | (v2_vw(vram, lsta + 2u) >> 8));
        lsta += 4u;
        return v;
    };
    int32_t x = s.sx, y = s.sy, dx = s.dx, acc = 0;
    for (int l = 0; l < lines && l < StvVdpState::kMaxFrameH; ++l) {
        if ((l & ((1 << lss) - 1)) == 0) {
            if (sc & 2u) x = entry(false) + s.sx;
            if (sc & 4u) {
                acc = 0;
                y = entry(false) + s.sy;
            }
            if (sc & 8u) dx = entry(true);
        }
        s.lx[l] = x;
        s.ly[l] = y + acc;
        s.ldx[l] = dx;
        acc += s.dy;
    }
}

void v2_setup_nbg(const StvVdpState& st, const uint8_t vram[0x80000], int n, StvVdpState::Nbg& s) {
    s = StvVdpState::Nbg{};
    const uint16_t bgon = v2_reg(st, 0x20u);
    s.on = ((bgon >> n) & 1u) != 0u;
    s.opaque = ((bgon >> (8 + n)) & 1u) != 0u;
    if (!s.on) return;
    const uint16_t chctl = (n < 2) ? v2_reg(st, 0x28u) : v2_reg(st, 0x2Au);
    const int sh = (n & 1) * 8;
    if (n < 2) {
        const uint16_t c = static_cast<uint16_t>(chctl >> sh);
        s.colors = (n == 0) ? ((c >> 4) & 7) : ((c >> 4) & 3);
        s.bitmap = ((c >> 1) & 1) != 0;
        s.cell2x2 = (c & 1) != 0;
        const int bmsz = (c >> 2) & 3;
        s.bm_w = (bmsz & 2) ? 1024 : 512;
        s.bm_h = (bmsz & 1) ? 512 : 256;
    } else {
        const uint16_t c = static_cast<uint16_t>(chctl >> ((n - 2) * 4));
        s.colors = (c >> 1) & 1;
        s.cell2x2 = (c & 1) != 0;
    }
    const uint16_t pncn = v2_reg(st, static_cast<uint32_t>(0x30 + n * 2));
    s.word1 = ((pncn >> 15) & 1u) != 0u;
    s.cnsm = ((pncn >> 14) & 1u) != 0u;
    s.supp = pncn & 0x3FFu;
    const int plsz = (v2_reg(st, 0x3Au) >> (n * 2)) & 3;
    s.plane_w = (plsz == 0) ? 1 : 2;
    s.plane_h = (plsz == 3) ? 2 : 1;
    const uint32_t mpof = (v2_reg(st, 0x3Cu) >> (n * 4)) & 7u;
    s.page_bytes = (s.cell2x2 ? 0x800u : 0x2000u) * (s.word1 ? 1u : 2u);
    const uint32_t mask = static_cast<uint32_t>(s.plane_w * s.plane_h - 1);
    for (int i = 0; i < 4; ++i) {
        const uint16_t mp = v2_reg(st, static_cast<uint32_t>(0x40 + n * 4 + (i / 2) * 2));
        const uint32_t m = ((mpof << 6) | static_cast<uint32_t>((mp >> ((i & 1) * 8)) & 0x3Fu)) & ~mask;
        s.plane[i] = m * s.page_bytes;
    }
    if (s.bitmap) {
        s.bm_addr = mpof * 0x20000u;
        const uint16_t bmpn = static_cast<uint16_t>(v2_reg(st, 0x2Cu) >> sh);
        s.bm_pal = static_cast<uint16_t>((bmpn & 7u) << 4);
        s.bm_spr = ((bmpn >> 5) & 1u) != 0u;
        s.bm_scc = ((bmpn >> 4) & 1u) != 0u;
    }
    const int caos_sh = n * 4;
    s.caos = static_cast<uint32_t>((v2_reg(st, 0xE4u) >> caos_sh) & 7u) << 8;
    if (n < 2) {
        const uint32_t b = static_cast<uint32_t>(0x70 + n * 0x10);
        s.sx = static_cast<int32_t>(((v2_reg(st, b) & 0x7FFu) << 8) | (v2_reg(st, b + 2u) >> 8));
        s.sy = static_cast<int32_t>(((v2_reg(st, b + 4u) & 0x7FFu) << 8) | (v2_reg(st, b + 6u) >> 8));
        s.dx = static_cast<int32_t>(((v2_reg(st, b + 8u) & 7u) << 8) | (v2_reg(st, b + 10u) >> 8));
        s.dy = static_cast<int32_t>(((v2_reg(st, b + 12u) & 7u) << 8) | (v2_reg(st, b + 14u) >> 8));
    } else {
        const uint32_t b = static_cast<uint32_t>(0x90 + (n - 2) * 4);
        s.sx = static_cast<int32_t>(v2_reg(st, b) & 0x7FFu) << 8;
        s.sy = static_cast<int32_t>(v2_reg(st, b + 2u) & 0x7FFu) << 8;
        s.dx = s.dy = 0x100;
    }
    const uint16_t prin = v2_reg(st, static_cast<uint32_t>(0xF8 + (n / 2) * 2));
    s.prio = static_cast<uint8_t>((prin >> ((n & 1) * 8)) & 7u);
    const uint16_t ccrn = v2_reg(st, static_cast<uint32_t>(0x108 + (n / 2) * 2));
    s.ratio = static_cast<uint8_t>((ccrn >> ((n & 1) * 8)) & 0x1Fu);
    s.cc = ((v2_reg(st, 0xECu) >> n) & 1u) != 0u;
    s.offset = ((v2_reg(st, 0x110u) >> n) & 1u) != 0u;
    s.offset_b = ((v2_reg(st, 0x112u) >> n) & 1u) != 0u;
    if (n < 2) v2_setup_line_scroll(st, vram, n, s, st.vdp2_lines());
}

inline bool v2_dot(const StvVdpState::ComposePrepared& lut, const uint8_t vram[0x80000],
                   const StvVdpState::Nbg& s, uint32_t base, int x, int y, int width,
                   uint32_t pal, Pix& p) {
    const uint32_t i = static_cast<uint32_t>(y * width + x);
    uint32_t v = 0u;
    switch (s.colors) {
        case 0:
            v = v2_vb(vram, base + (i >> 1));
            v = (i & 1u) ? (v & 0xFu) : (v >> 4);
            break;
        case 1:
            v = v2_vb(vram, base + i);
            break;
        case 2:
            v = v2_vw(vram, base + (i << 1)) & 0x7FFu;
            break;
        case 3: {
            const uint16_t w = v2_vw(vram, base + (i << 1));
            if (!(w & 0x8000u) && !s.opaque) return false;
            p.rgb = rgb555_to_888(w);
            p.direct = true;
            p.msb = (w >> 15) != 0u;
            return true;
        }
        default: {
            const uint32_t w = (static_cast<uint32_t>(v2_vw(vram, base + (i << 2))) << 16) |
                               v2_vw(vram, base + (i << 2) + 2u);
            if (!(w & 0x80000000u) && !s.opaque) return false;
            p.rgb = ((w & 0xFFu) << 16) | (w & 0xFF00u) | ((w >> 16) & 0xFFu);
            p.direct = true;
            p.msb = (w >> 31) != 0u;
            return true;
        }
    }
    if (v == 0u && !s.opaque) return false;
    const uint32_t index = (s.colors == 0) ? ((pal << 4) + v) : (s.colors == 1 ? (((pal & 0x70u) << 4) + v) : v);
    p.rgb = v2_cram_fast(lut, index + s.caos, &p.msb);
    p.direct = false;
    p.dcc = static_cast<uint16_t>(v);
    return true;
}

inline bool v2_cell_pixel(const StvVdpState::ComposePrepared& lut, const uint8_t vram[0x80000],
                          const StvVdpState::Nbg& s, const uint32_t* planes, int side,
                          uint32_t sx, uint32_t sy, Pix& p, const uint16_t* pnd_over = nullptr) {
    const uint32_t pw_sh = (s.plane_w == 2) ? 10u : 9u;
    const uint32_t ph_sh = (s.plane_h == 2) ? 10u : 9u;
    const uint32_t pw = 1u << pw_sh;
    const uint32_t ph = 1u << ph_sh;
    const uint32_t cs_sh = s.cell2x2 ? 4u : 3u;
    const uint32_t cs = 1u << cs_sh;
    const uint32_t ix = sx & (pw - 1u);
    const uint32_t iy = sy & (ph - 1u);
    uint32_t pnd_addr = 0u;
    if (!pnd_over) {
        const int pl = static_cast<int>(sy >> ph_sh) * side + static_cast<int>(sx >> pw_sh);
        const uint32_t page = (iy >> 9) * static_cast<uint32_t>(s.plane_w) + (ix >> 9);
        const uint32_t cx = (ix & 511u) >> cs_sh;
        const uint32_t cy = (iy & 511u) >> cs_sh;
        const uint32_t cpr = s.cell2x2 ? 32u : 64u;
        pnd_addr = planes[pl] + page * s.page_bytes + ((cy * cpr + cx) << (s.word1 ? 1u : 2u));
    }
    uint32_t chr = 0u, pal = 0u;
    bool hf = false, vf = false;
    if (s.word1 || pnd_over) {
        const uint16_t w = pnd_over ? *pnd_over : v2_vw(vram, pnd_addr);
        p.spr = ((s.supp >> 9) & 1u) != 0u;
        p.scc = ((s.supp >> 8) & 1u) != 0u;
        pal = (s.colors == 0) ? (static_cast<uint32_t>((w >> 12) & 0xFu) | ((s.supp >> 1) & 0x70u))
                              : static_cast<uint32_t>((w >> 8) & 0x70u);
        if (!s.cnsm) {
            hf = (w & 0x400u) != 0u;
            vf = (w & 0x800u) != 0u;
            chr = s.cell2x2 ? (((w & 0x3FFu) << 2) | (s.supp & 3u) | ((s.supp & 0x1Cu) << 10))
                            : ((w & 0x3FFu) | ((s.supp & 0x1Fu) << 10));
        } else {
            hf = vf = false;
            chr = s.cell2x2 ? (((w & 0xFFFu) << 2) | (s.supp & 3u) | ((s.supp & 0x10u) << 10))
                            : ((w & 0xFFFu) | ((s.supp & 0x1Cu) << 10));
        }
    } else {
        const uint16_t w0 = v2_vw(vram, pnd_addr), w1 = v2_vw(vram, pnd_addr + 2u);
        hf = (w0 & 0x4000u) != 0u;
        vf = (w0 & 0x8000u) != 0u;
        p.spr = ((w0 >> 13) & 1u) != 0u;
        p.scc = ((w0 >> 12) & 1u) != 0u;
        pal = w0 & 0x7Fu;
        chr = w1 & 0x7FFFu;
    }
    int px = static_cast<int>(ix & (cs - 1u)), py = static_cast<int>(iy & (cs - 1u));
    if (hf) px = static_cast<int>(cs) - 1 - px;
    if (vf) py = static_cast<int>(cs) - 1 - py;
    static constexpr uint32_t kCellBytes[] = {32u, 64u, 128u, 128u, 256u};
    uint32_t base = chr << 5;
    if (s.cell2x2) base += static_cast<uint32_t>((py >> 3) * 2 + (px >> 3)) * kCellBytes[s.colors];
    return v2_dot(lut, vram, s, base, px & 7, py & 7, 8, pal, p);
}

struct NbgScanCache {
    int32_t bx = 0;
    int32_t dx = 0x100;
    uint32_t sx_mask = 0u;
    uint32_t pw_mask = 0u;
    uint32_t pw_sh = 9u;
    uint32_t cs_sh = 3u;
    uint32_t cs = 8u;
    uint32_t pl_row_off = 0u;
    uint32_t page_row_off = 0u;
    uint32_t cy_row_off = 0u;
    uint32_t sy_bm = 0u;
    int py_raw = 0;
    uint32_t last_pnd = 0xFFFFFFFFu;
    uint32_t c_chr = 0u;
    uint32_t c_pal = 0u;
    bool c_hf = false;
    bool c_vf = false;
    bool c_spr = false;
    bool c_scc = false;
};

inline void v2_nbg_begin_line(const StvVdpState::Nbg& s, int y, NbgScanCache& c) {
    const int32_t by = s.line ? s.ly[y] : (s.sy + y * s.dy);
    c.bx = s.line ? s.lx[y] : s.sx;
    c.dx = s.line ? s.ldx[y] : s.dx;
    const uint32_t sy_raw = static_cast<uint32_t>(by) >> 8;
    if (s.bitmap) {
        c.sy_bm = sy_raw & static_cast<uint32_t>(s.bm_h - 1);
        return;
    }
    c.pw_sh = (s.plane_w == 2) ? 10u : 9u;
    const uint32_t ph_sh = (s.plane_h == 2) ? 10u : 9u;
    const uint32_t pw = 1u << c.pw_sh;
    const uint32_t ph = 1u << ph_sh;
    c.sx_mask = pw * 2u - 1u;
    c.pw_mask = pw - 1u;
    c.cs_sh = s.cell2x2 ? 4u : 3u;
    c.cs = 1u << c.cs_sh;
    const uint32_t sy = sy_raw & (ph * 2u - 1u);
    const uint32_t iy = sy & (ph - 1u);
    c.pl_row_off = (sy >> ph_sh) << 1;
    c.page_row_off = (iy >> 9) * static_cast<uint32_t>(s.plane_w);
    const uint32_t cpr = s.cell2x2 ? 32u : 64u;
    c.cy_row_off = ((iy & 511u) >> c.cs_sh) * cpr;
    c.py_raw = static_cast<int>(iy & (c.cs - 1u));
    c.last_pnd = 0xFFFFFFFFu;
}

inline bool v2_nbg_pixel_fast(const StvVdpState::ComposePrepared& lut, const uint8_t vram[0x80000],
                              const StvVdpState::Nbg& s, NbgScanCache& c, int x, Pix& p) {
    const uint32_t sx_raw = static_cast<uint32_t>(c.bx + x * c.dx) >> 8;
    if (s.bitmap) {
        const uint32_t sx = sx_raw & static_cast<uint32_t>(s.bm_w - 1);
        p.spr = s.bm_spr;
        p.scc = s.bm_scc;
        return v2_dot(lut, vram, s, s.bm_addr, static_cast<int>(sx), static_cast<int>(c.sy_bm),
                      s.bm_w, s.bm_pal, p);
    }
    const uint32_t sx = sx_raw & c.sx_mask;
    const uint32_t ix = sx & c.pw_mask;
    const uint32_t pl = c.pl_row_off + (sx >> c.pw_sh);
    const uint32_t page = c.page_row_off + (ix >> 9);
    const uint32_t cx = (ix & 511u) >> c.cs_sh;
    const uint32_t pnd_addr = s.plane[pl] + page * s.page_bytes + ((c.cy_row_off + cx) << (s.word1 ? 1u : 2u));
    if (pnd_addr != c.last_pnd) {
        c.last_pnd = pnd_addr;
        if (s.word1) {
            const uint16_t w = v2_vw(vram, pnd_addr);
            c.c_spr = ((s.supp >> 9) & 1u) != 0u;
            c.c_scc = ((s.supp >> 8) & 1u) != 0u;
            c.c_pal = (s.colors == 0) ? (static_cast<uint32_t>((w >> 12) & 0xFu) | ((s.supp >> 1) & 0x70u))
                                      : static_cast<uint32_t>((w >> 8) & 0x70u);
            if (!s.cnsm) {
                c.c_hf = (w & 0x400u) != 0u;
                c.c_vf = (w & 0x800u) != 0u;
                c.c_chr = s.cell2x2 ? (((w & 0x3FFu) << 2) | (s.supp & 3u) | ((s.supp & 0x1Cu) << 10))
                                    : ((w & 0x3FFu) | ((s.supp & 0x1Fu) << 10));
            } else {
                c.c_hf = c.c_vf = false;
                c.c_chr = s.cell2x2 ? (((w & 0xFFFu) << 2) | (s.supp & 3u) | ((s.supp & 0x10u) << 10))
                                    : ((w & 0xFFFu) | ((s.supp & 0x1Cu) << 10));
            }
        } else {
            const uint16_t w0 = v2_vw(vram, pnd_addr), w1 = v2_vw(vram, pnd_addr + 2u);
            c.c_hf = (w0 & 0x4000u) != 0u;
            c.c_vf = (w0 & 0x8000u) != 0u;
            c.c_spr = ((w0 >> 13) & 1u) != 0u;
            c.c_scc = ((w0 >> 12) & 1u) != 0u;
            c.c_pal = w0 & 0x7Fu;
            c.c_chr = w1 & 0x7FFFu;
        }
    }
    p.spr = c.c_spr;
    p.scc = c.c_scc;
    int px = static_cast<int>(ix & (c.cs - 1u));
    int py = c.py_raw;
    if (c.c_hf) px = static_cast<int>(c.cs) - 1 - px;
    if (c.c_vf) py = static_cast<int>(c.cs) - 1 - py;
    static constexpr uint32_t kCellBytes[] = {32u, 64u, 128u, 128u, 256u};
    uint32_t base = c.c_chr << 5;
    if (s.cell2x2) base += static_cast<uint32_t>((py >> 3) * 2 + (px >> 3)) * kCellBytes[s.colors];
    return v2_dot(lut, vram, s, base, px & 7, py & 7, 8, c.c_pal, p);
}

void v2_setup_rbg(const StvVdpState& st, const uint8_t vram[0x80000], StvVdpState::Rbg& r) {
    r = StvVdpState::Rbg{};
    const uint16_t bgon = v2_reg(st, 0x20u);
    r.on = ((bgon >> 4) & 1u) != 0u;
    if (!r.on) return;
    r.mode = v2_reg(st, 0xB0u) & 3;
    const uint16_t ramctl = v2_reg(st, 0x0Eu);
    r.crkte = ((ramctl >> 15) & 1u) != 0u;
    for (int b = 0; b < 4; ++b) {
        const int esb = b & (2 | ((ramctl >> (8 + (b >> 1))) & 1));
        r.bank_coef[b] = r.crkte || ((ramctl >> (esb * 2)) & 3) == 1;
        r.perdot |= r.bank_coef[b];
    }
    r.prio = static_cast<uint8_t>(v2_reg(st, 0xFCu) & 7u);
    r.ratio = static_cast<uint8_t>(v2_reg(st, 0x10Cu) & 0x1Fu);
    r.cc = ((v2_reg(st, 0xECu) >> 4) & 1u) != 0u;
    r.offset = ((v2_reg(st, 0x110u) >> 4) & 1u) != 0u;
    r.offset_b = ((v2_reg(st, 0x112u) >> 4) & 1u) != 0u;
    const uint16_t chctl = static_cast<uint16_t>(v2_reg(st, 0x2Au) >> 8);
    const uint16_t pncr = v2_reg(st, 0x38u);
    const uint32_t rpta = (((static_cast<uint32_t>(v2_reg(st, 0xBCu) & 7u) << 16) |
                            (v2_reg(st, 0xBEu) & 0xFFFEu)) * 2u) & 0xFFF7Cu;
    for (int i = 0; i < 2; ++i) {
        StvVdpState::RotParam& p = r.par[i];
        StvVdpState::Nbg& s = p.chr;
        s.on = true;
        s.opaque = ((bgon >> 12) & 1u) != 0u;
        s.colors = (chctl >> 4) & 7;
        s.bitmap = (i == 0) && (((chctl >> 1) & 1u) != 0u);
        s.cell2x2 = (chctl & 1u) != 0u;
        s.bm_w = 512;
        s.bm_h = ((chctl >> 2) & 1u) ? 512 : 256;
        s.bm_pal = static_cast<uint16_t>((v2_reg(st, 0x2Eu) & 7u) << 4);
        s.bm_spr = ((v2_reg(st, 0x2Eu) >> 5) & 1u) != 0u;
        s.bm_scc = ((v2_reg(st, 0x2Eu) >> 4) & 1u) != 0u;
        s.word1 = ((pncr >> 15) & 1u) != 0u;
        s.cnsm = ((pncr >> 14) & 1u) != 0u;
        s.supp = pncr & 0x3FFu;
        s.caos = static_cast<uint32_t>(v2_reg(st, 0xE6u) & 7u) << 8;
        const int plsz = (v2_reg(st, 0x3Au) >> (8 + i * 4)) & 3;
        s.plane_w = (plsz == 0) ? 1 : 2;
        s.plane_h = (plsz == 3) ? 2 : 1;
        s.page_bytes = (s.cell2x2 ? 0x800u : 0x2000u) * (s.word1 ? 1u : 2u);
        p.over = (v2_reg(st, 0x3Au) >> (10 + i * 4)) & 3;
        p.over_pn = v2_reg(st, static_cast<uint32_t>(0xB8 + i * 2));
        const uint32_t mpof = (v2_reg(st, 0x3Eu) >> (i * 4)) & 7u;
        const uint32_t mask = static_cast<uint32_t>(s.plane_w * s.plane_h - 1);
        for (int k = 0; k < 16; ++k) {
            const uint32_t mp = (v2_reg(st, static_cast<uint32_t>(0x50 + i * 0x10 + (k / 2) * 2)) >> ((k & 1) * 8)) & 0x3Fu;
            p.plane[k] = ((mpof << 6) | mp) & ~mask;
            p.plane[k] *= s.page_bytes;
        }
        s.bm_addr = mpof * 0x20000u;
        const uint32_t a = rpta + static_cast<uint32_t>(i) * 0x80u;
        p.xst = sext_bits(v2_vl(vram, a + 0x00u) >> 6, 23);
        p.yst = sext_bits(v2_vl(vram, a + 0x04u) >> 6, 23);
        p.zst = sext_bits(v2_vl(vram, a + 0x08u) >> 6, 23);
        p.dxst = sext_bits(v2_vl(vram, a + 0x0Cu) >> 6, 13);
        p.dyst = sext_bits(v2_vl(vram, a + 0x10u) >> 6, 13);
        p.dx = sext_bits(v2_vl(vram, a + 0x14u) >> 6, 13);
        p.dy = sext_bits(v2_vl(vram, a + 0x18u) >> 6, 13);
        for (int k = 0; k < 6; ++k) {
            p.m[k] = sext_bits(v2_vl(vram, a + 0x1Cu + static_cast<uint32_t>(k * 4)) >> 6, 14);
        }
        p.px = sext_bits(v2_vw(vram, a + 0x34u), 14);
        p.py = sext_bits(v2_vw(vram, a + 0x36u), 14);
        p.pz = sext_bits(v2_vw(vram, a + 0x38u), 14);
        p.cx = sext_bits(v2_vw(vram, a + 0x3Cu), 14);
        p.cy = sext_bits(v2_vw(vram, a + 0x3Eu), 14);
        p.cz = sext_bits(v2_vw(vram, a + 0x40u), 14);
        p.mx = sext_bits(v2_vl(vram, a + 0x44u) >> 6, 24);
        p.my = sext_bits(v2_vl(vram, a + 0x48u) >> 6, 24);
        p.kx = sext_bits(v2_vl(vram, a + 0x4Cu), 24);
        p.ky = sext_bits(v2_vl(vram, a + 0x50u), 24);
        const uint16_t ktctl = static_cast<uint16_t>(v2_reg(st, 0xB4u) >> (i * 8));
        p.coef = (ktctl & 1u) != 0u;
        p.coef1w = ((ktctl >> 1) & 1u) != 0u;
        p.coef_mode = (ktctl >> 2) & 3;
        p.ka0 = (static_cast<uint32_t>((v2_reg(st, 0xB6u) >> (i * 8)) & 7u) << 26) + (v2_vl(vram, a + 0x54u) >> 6);
        p.dkast = sext_bits(v2_vl(vram, a + 0x58u) >> 6, 20);
        p.dkax = sext_bits(v2_vl(vram, a + 0x5Cu) >> 6, 20);
    }
}

inline uint32_t v2_coef_addr(const StvVdpState::Rbg& r, const StvVdpState::RotParam& p, uint32_t ka) {
    const uint32_t a = (ka >> 10) << (p.coef1w ? 0 : 1);
    return a & (r.crkte ? 0x3FFu : 0x3FFFFu);
}

uint32_t v2_coef_read(const StvVdpState& st, const uint8_t vram[0x80000],
                      const StvVdpState::Rbg& r, const StvVdpState::RotParam& p, uint32_t addr) {
    auto word = [&](uint32_t w) -> uint16_t {
        if (!r.crkte) return v2_vw(vram, w * 2u);
        const uint32_t b = (0x800u + w * 2u) & 0xFFEu;
        return static_cast<uint16_t>((st.vdp2_cram[b] << 8) | st.vdp2_cram[b + 1u]);
    };
    if (p.coef1w) {
        const uint16_t t = word(addr);
        return (static_cast<uint32_t>(sext_bits(static_cast<uint32_t>(t) << 6, 21)) & 0xFFFFFFu) |
               (static_cast<uint32_t>(t & 0x8000u) << 16);
    }
    return (static_cast<uint32_t>(word(addr)) << 16) | word(addr + 1u);
}

inline void v2_rbg_line_eval(const StvVdpState& st, const uint8_t vram[0x80000],
                             const StvVdpState::Rbg& r, int y, int y_shift,
                             StvVdpState::RbgLineParams out[2]) {
    for (int i = 0; i < 2; ++i) {
        const StvVdpState::RotParam& p = r.par[i];
        StvVdpState::RbgLineParams& lp = out[i];
        // When y_shift == 1 (optional HD pitch visual mode), evaluate RBG0 half-scanline steps at 704x448.
        const int64_t x0 = p.xst + ((static_cast<int64_t>(p.dxst) * y) >> y_shift) - p.px * 1024;
        const int64_t y0 = p.yst + ((static_cast<int64_t>(p.dyst) * y) >> y_shift) - p.py * 1024;
        const int64_t z0 = p.zst - p.pz * 1024;
        lp.xsp = static_cast<int32_t>((p.m[0] * x0 + p.m[1] * y0 + p.m[2] * z0) >> 10);
        lp.ysp = static_cast<int32_t>((p.m[3] * x0 + p.m[4] * y0 + p.m[5] * z0) >> 10);
        lp.xp = p.m[0] * (p.px - p.cx) + p.m[1] * (p.py - p.cy) + p.m[2] * (p.pz - p.cz) + p.cx * 1024 + p.mx;
        lp.yp = p.m[3] * (p.px - p.cx) + p.m[4] * (p.py - p.cy) + p.m[5] * (p.pz - p.cz) + p.cy * 1024 + p.my;
        lp.ldx = (p.m[0] * p.dx + p.m[1] * p.dy) >> 10;
        lp.ldy = (p.m[3] * p.dx + p.m[4] * p.dy) >> 10;
        lp.ka = p.ka0 + (y_shift ? static_cast<uint32_t>((static_cast<int64_t>(p.dkast) * y) >> y_shift) : static_cast<uint32_t>(p.dkast * y));
        const uint32_t ca = v2_coef_addr(r, p, lp.ka);
        lp.line_coef = v2_coef_read(st, vram, r, p, ca);
        lp.line_coef_dot = r.bank_coef[r.crkte ? 0 : ((ca >> 16) & 3u)] ? lp.line_coef : 0u;
    }
}

struct RbgCellCache {
    uint32_t last_pnd = 0xFFFFFFFFu;
    uint32_t c_chr = 0u;
    uint32_t c_pal = 0u;
    bool c_hf = false;
    bool c_vf = false;
    bool c_spr = false;
    bool c_scc = false;
};

inline bool v2_rbg_pixel(const StvVdpState& st, const uint8_t vram[0x80000], const StvVdpState::Rbg& r,
                         const StvVdpState::RbgLineParams lps[2], int x, Pix& out,
                         RbgCellCache caches[2], int x_shift = 0) {
    const auto& lut = st.compose_prep;
    int sel = (r.mode == 1) ? 1 : 0;
    auto coef_at = [&](int i) {
        const StvVdpState::RotParam& p = r.par[i];
        const StvVdpState::RbgLineParams& lp = lps[i];
        if (!r.perdot) return lp.line_coef;
        if (p.dkax == 0) return lp.line_coef_dot;
        const uint32_t ka_step = x_shift
            ? static_cast<uint32_t>((static_cast<int64_t>(p.dkax) * x) >> x_shift)
            : static_cast<uint32_t>(x * p.dkax);
        const uint32_t a = v2_coef_addr(r, p, lp.ka + ka_step);
        return r.bank_coef[r.crkte ? 0 : ((a >> 16) & 3u)] ? v2_coef_read(st, vram, r, p, a) : 0u;
    };
    uint32_t c = 0u;
    if (r.mode == 2) {
        c = r.par[0].coef ? coef_at(0) : 0u;
        if (c >> 31) {
            sel = 1;
            c = lps[1].line_coef;
        }
    } else if (r.par[sel].coef) {
        c = coef_at(sel);
    }
    const StvVdpState::RotParam& p = r.par[sel];
    const StvVdpState::RbgLineParams& lp = lps[sel];
    int32_t kx = p.kx, ky = p.ky;
    uint32_t xp = static_cast<uint32_t>(lp.xp);
    if (p.coef) {
        if (c >> 31) return false;
        const int32_t v = sext_bits(c, 24);
        switch (p.coef_mode) {
            case 0: kx = ky = v; break;
            case 1: kx = v; break;
            case 2: ky = v; break;
            default: xp = static_cast<uint32_t>(v) << 2; break;
        }
    }
    const int32_t xsp_x = x_shift
        ? static_cast<int32_t>(lp.xsp + ((static_cast<int64_t>(lp.ldx) * x) >> x_shift))
        : static_cast<int32_t>(lp.xsp + lp.ldx * x);
    const int32_t ysp_x = x_shift
        ? static_cast<int32_t>(lp.ysp + ((static_cast<int64_t>(lp.ldy) * x) >> x_shift))
        : static_cast<int32_t>(lp.ysp + lp.ldy * x);
    const uint32_t ix = (xp + static_cast<uint32_t>((static_cast<int64_t>(kx) * xsp_x) >> 16)) >> 10;
    const uint32_t iy = (static_cast<uint32_t>(lp.yp) + static_cast<uint32_t>((static_cast<int64_t>(ky) * ysp_x) >> 16)) >> 10;
    const StvVdpState::Nbg& s = p.chr;
    const uint32_t mw = s.bitmap ? static_cast<uint32_t>(s.bm_w) : (static_cast<uint32_t>(s.plane_w) << 11);
    const uint32_t mh = s.bitmap ? static_cast<uint32_t>(s.bm_h) : (static_cast<uint32_t>(s.plane_h) << 11);
    bool outside = false;
    if (p.over == 3) outside = ((ix | iy) & ~511u) != 0u;
    else if (p.over) outside = ((ix & ~(mw - 1u)) != 0u) || ((iy & ~(mh - 1u)) != 0u);
    if (outside && (p.over & 2)) return false;
    if (s.bitmap) {
        out.spr = s.bm_spr;
        out.scc = s.bm_scc;
        return v2_dot(lut, vram, s, s.bm_addr, static_cast<int>(ix & (mw - 1u)),
                      static_cast<int>(iy & (mh - 1u)), s.bm_w, s.bm_pal, out);
    }
    if (outside) {
        return v2_cell_pixel(lut, vram, s, p.plane, 4, ix & (mw - 1u), iy & (mh - 1u), out, &p.over_pn);
    }
    const uint32_t sx = ix & (mw - 1u);
    const uint32_t sy = iy & (mh - 1u);
    const uint32_t pw_sh = (s.plane_w == 2) ? 10u : 9u;
    const uint32_t ph_sh = (s.plane_h == 2) ? 10u : 9u;
    const uint32_t pw = 1u << pw_sh;
    const uint32_t ph = 1u << ph_sh;
    const uint32_t cs_sh = s.cell2x2 ? 4u : 3u;
    const uint32_t cs = 1u << cs_sh;
    const uint32_t c_ix = sx & (pw - 1u);
    const uint32_t c_iy = sy & (ph - 1u);
    const int pl = (static_cast<int>(sy >> ph_sh) << 2) + static_cast<int>(sx >> pw_sh);
    const uint32_t page = (c_iy >> 9) * static_cast<uint32_t>(s.plane_w) + (c_ix >> 9);
    const uint32_t cx = (c_ix & 511u) >> cs_sh;
    const uint32_t cy = (c_iy & 511u) >> cs_sh;
    const uint32_t cpr = s.cell2x2 ? 32u : 64u;
    const uint32_t pnd_addr = p.plane[pl] + page * s.page_bytes + ((cy * cpr + cx) << (s.word1 ? 1u : 2u));
    RbgCellCache& cc = caches[sel];
    if (pnd_addr != cc.last_pnd) {
        cc.last_pnd = pnd_addr;
        if (s.word1) {
            const uint16_t w = v2_vw(vram, pnd_addr);
            cc.c_spr = ((s.supp >> 9) & 1u) != 0u;
            cc.c_scc = ((s.supp >> 8) & 1u) != 0u;
            cc.c_pal = (s.colors == 0) ? (static_cast<uint32_t>((w >> 12) & 0xFu) | ((s.supp >> 1) & 0x70u))
                                       : static_cast<uint32_t>((w >> 8) & 0x70u);
            if (!s.cnsm) {
                cc.c_hf = (w & 0x400u) != 0u;
                cc.c_vf = (w & 0x800u) != 0u;
                cc.c_chr = s.cell2x2 ? (((w & 0x3FFu) << 2) | (s.supp & 3u) | ((s.supp & 0x1Cu) << 10))
                                     : ((w & 0x3FFu) | ((s.supp & 0x1Fu) << 10));
            } else {
                cc.c_hf = cc.c_vf = false;
                cc.c_chr = s.cell2x2 ? (((w & 0xFFFu) << 2) | (s.supp & 3u) | ((s.supp & 0x10u) << 10))
                                     : ((w & 0xFFFu) | ((s.supp & 0x1Cu) << 10));
            }
        } else {
            const uint16_t w0 = v2_vw(vram, pnd_addr), w1 = v2_vw(vram, pnd_addr + 2u);
            cc.c_hf = (w0 & 0x4000u) != 0u;
            cc.c_vf = (w0 & 0x8000u) != 0u;
            cc.c_spr = ((w0 >> 13) & 1u) != 0u;
            cc.c_scc = ((w0 >> 12) & 1u) != 0u;
            cc.c_pal = w0 & 0x7Fu;
            cc.c_chr = w1 & 0x7FFFu;
        }
    }
    out.spr = cc.c_spr;
    out.scc = cc.c_scc;
    int px = static_cast<int>(c_ix & (cs - 1u)), py = static_cast<int>(c_iy & (cs - 1u));
    if (cc.c_hf) px = static_cast<int>(cs) - 1 - px;
    if (cc.c_vf) py = static_cast<int>(cs) - 1 - py;
    static constexpr uint32_t kCellBytes[] = {32u, 64u, 128u, 128u, 256u};
    uint32_t base = cc.c_chr << 5;
    if (s.cell2x2) base += static_cast<uint32_t>((py >> 3) * 2 + (px >> 3)) * kCellBytes[s.colors];
    return v2_dot(lut, vram, s, base, px & 7, py & 7, 8, cc.c_pal, out);
}

struct SpriteType {
    int pr_shift, pr_mask, cc_shift, cc_mask;
    uint16_t dc_mask;
};

static constexpr SpriteType kSpriteTypes[16] = {
    {14, 3, 11, 7, 0x7FFu}, {13, 7, 11, 3, 0x7FFu}, {14, 1, 11, 7, 0x7FFu}, {13, 3, 11, 3, 0x7FFu},
    {13, 3, 10, 7, 0x3FFu}, {12, 7, 11, 1, 0x7FFu}, {12, 7, 10, 3, 0x3FFu}, {12, 7, 9, 7, 0x1FFu},
    {7, 1, 0, 0, 0x7Fu},    {7, 1, 6, 1, 0x3Fu},    {6, 3, 0, 0, 0x3Fu},    {0, 0, 6, 3, 0x3Fu},
    {7, 1, 0, 0, 0xFFu},    {7, 1, 6, 1, 0xFFu},    {6, 3, 0, 0, 0xFFu},    {0, 0, 6, 3, 0xFFu},
};

void v2_setup_windows(StvVdpState& st) {
    const bool hires = (v2_reg(st, 0x00u) & 6u) != 0u;
    const int sh = hires ? 0 : 1;
    for (int i = 0; i < 2; ++i) {
        StvVdpState::Window& w = st.compose_win[i];
        const uint32_t b = static_cast<uint32_t>(0xC0 + i * 8);
        w.x0 = (v2_reg(st, b) & 0x3FF) >> sh;
        w.y0 = v2_reg(st, b + 2u) & 0x1FF;
        w.x1 = (v2_reg(st, b + 4u) & 0x3FF) >> sh;
        w.y1 = v2_reg(st, b + 6u) & 0x1FF;
        const uint16_t hi = v2_reg(st, static_cast<uint32_t>(0xD8 + i * 4));
        const uint16_t lo = v2_reg(st, static_cast<uint32_t>(0xDA + i * 4));
        w.lines = (hi & 0x8000u) != 0u;
        w.table = ((static_cast<uint32_t>(hi & 7u) << 16) | (lo & 0xFFFEu)) << 1;
    }
}

uint8_t v2_wctl_of(const StvVdpState& st, int layer) {
    switch (layer) {
        case L_SPRITE: return static_cast<uint8_t>(v2_reg(st, 0xD4u) >> 8);
        case L_RBG0: return static_cast<uint8_t>(v2_reg(st, 0xD4u));
        case L_NBG0: return static_cast<uint8_t>(v2_reg(st, 0xD0u));
        case L_NBG1: return static_cast<uint8_t>(v2_reg(st, 0xD0u) >> 8);
        case L_NBG2: return static_cast<uint8_t>(v2_reg(st, 0xD2u));
        default: return static_cast<uint8_t>(v2_reg(st, 0xD2u) >> 8);
    }
}

}  // namespace

void StvVdpState::init() {
    std::memset(vdp1_vram, 0, sizeof(vdp1_vram));
    std::memset(vdp1_fb, 0, sizeof(vdp1_fb));
    std::memset(vdp1_regs, 0, sizeof(vdp1_regs));
    std::memset(vdp2_cram, 0, sizeof(vdp2_cram));
    std::memset(vdp2_regs, 0, sizeof(vdp2_regs));
    std::memset(frame_px, 0, sizeof(frame_px));
    draw_fb = 0;
    change_pending = false;
    erase_pending = false;
    erase_field = false;
    sys_x = 319;
    sys_y = 223;
    ux0 = 0;
    uy0 = 0;
    ux1 = 319;
    uy1 = 223;
    local_x = 0;
    local_y = 0;
    frame_changes = 0u;
    draws = 0u;
    vdp1_unsupported = false;
    copa_sp_vfx = CopaSpVfxState{};
    copa_bracket = CopaBracketState{};
    frame_w = 320;
    frame_h = 224;
    vdp1_regs[MODR] = 0x1000u;
    vdp1_regs[EDSR] = 0x0002u;
    invalidate_gpu_vdp1_frames();
}

void StvVdpState::invalidate_gpu_vdp1_frames() {
    for (int i = 0; i < 2; ++i) {
        v1_gpu_frames[i].valid = false;
        v1_gpu_frames[i].cpu_materialized = false;
        v1_gpu_frames[i].count = 0u;
    }
}

void StvVdpState::set_gpu_vdp1(bool enabled) {
    if (gpu_vdp1_enabled == enabled) return;
    if (!enabled) {
        materialize_cpu_fb(0);
        materialize_cpu_fb(1);
    }
    gpu_vdp1_enabled = enabled;
}

int StvVdpState::vdp2_lines() const {
    const uint16_t tvmd = v2reg(0x00u);
    int lines;
    switch ((tvmd >> 4) & 3u) {
        case 1: lines = 240; break;
        case 2: lines = 256; break;
        default: lines = 224; break;
    }
    return ((tvmd >> 6) & 3u) == 3u ? lines * 2 : lines;
}

void StvVdpState::v1_record_erase(int which) {
    const int x_scale = (vdp1_regs[TVMR] & 1u) ? 16 : 8;
    const int y_scale = (((v2reg(0x00u) >> 6) & 3u) == 3u) ? 2 : 1;
    const int x0 = ((vdp1_regs[EWLR] >> 9) & 0x7F) * x_scale;
    const int y0 = (vdp1_regs[EWLR] & 0x1FF) * y_scale;
    const int x1 = ((vdp1_regs[EWRR] >> 9) & 0x7F) * x_scale;
    const int y1 = (vdp1_regs[EWRR] & 0x1FF) * y_scale;
    const int width = fb_width(), height = fb_height();
    const int cx0 = std::max(0, x0);
    const int cx1 = std::min(width, x1);
    const int cy0 = std::max(0, y0);
    const int cy1 = std::min(height - 1, y1);
    V1GpuFrame& gf = v1_gpu_frames[which & 1];
    gf.valid = true;
    gf.cpu_materialized = false;
    gf.count = 0u;
    gf.tvmr = vdp1_regs[TVMR];
    gf.ewdr = vdp1_regs[EWDR];
    gf.fb_w = static_cast<int16_t>(width);
    gf.fb_h = static_cast<int16_t>(height);
    if (cx0 < cx1 && cy0 <= cy1) {
        gf.ew_x0 = static_cast<int16_t>(cx0);
        gf.ew_y0 = static_cast<int16_t>(cy0);
        gf.ew_x1 = static_cast<int16_t>(cx1);
        gf.ew_y1 = static_cast<int16_t>(cy1 + 1);
    } else {
        gf.ew_x0 = 0;
        gf.ew_y0 = 0;
        gf.ew_x1 = 0;
        gf.ew_y1 = 0;
    }
}

void StvVdpState::v1_erase(int which) {
    const int x_scale = (vdp1_regs[TVMR] & 1u) ? 16 : 8;
    const int y_scale = (((v2reg(0x00u) >> 6) & 3u) == 3u) ? 2 : 1;
    const int x0 = ((vdp1_regs[EWLR] >> 9) & 0x7F) * x_scale;
    const int y0 = (vdp1_regs[EWLR] & 0x1FF) * y_scale;
    const int x1 = ((vdp1_regs[EWRR] >> 9) & 0x7F) * x_scale;
    const int y1 = (vdp1_regs[EWRR] & 0x1FF) * y_scale;
    const int width = fb_width(), height = fb_height();
    const int cx0 = std::max(0, x0);
    const int cx1 = std::min(width, x1);
    const int cy0 = std::max(0, y0);
    const int cy1 = std::min(height - 1, y1);
    if (cx0 >= cx1 || cy0 > cy1) return;
    const uint16_t ewdr = vdp1_regs[EWDR];
    uint16_t* const fb = vdp1_fb[which];
    if (ewdr == 0u) {
        if (cx0 == 0 && cx1 == width) {
            std::memset(fb + static_cast<size_t>(cy0) * static_cast<size_t>(width), 0,
                        static_cast<size_t>(cy1 - cy0 + 1) * static_cast<size_t>(width) * sizeof(uint16_t));
        } else {
            const size_t row_bytes = static_cast<size_t>(cx1 - cx0) * sizeof(uint16_t);
            for (int y = cy0; y <= cy1; ++y) {
                std::memset(fb + static_cast<size_t>(y) * static_cast<size_t>(width) + cx0, 0, row_bytes);
            }
        }
    } else {
        const size_t span = static_cast<size_t>(cx1 - cx0);
        for (int y = cy0; y <= cy1; ++y) {
            std::fill_n(fb + static_cast<size_t>(y) * static_cast<size_t>(width) + cx0, span, ewdr);
        }
    }
}

void StvVdpState::v1_draw() {
    v1_gpu_frames[draw_fb].valid = false;
    v1_gpu_frames[draw_fb].cpu_materialized = true;
    v1_obs_draw_count = 0;
    copa_sp_vfx.draw_condor = CopaVfxBox{};
    copa_sp_vfx.draw_machu = CopaVfxBox{};
    copa_sp_vfx.draw_uru_flag = CopaVfxBox{};
    copa_sp_vfx.uru_anchor_ttl = 0u;
    copa_sp_vfx.draw_charge_count = 0u;
    vdp1_regs[EDSR] = static_cast<uint16_t>((vdp1_regs[EDSR] >> 1) & 1u);
    if (vdp1_regs[TVMR] & 2u) {
        vdp1_unsupported = true;
        return;
    }
    uint32_t a = 0u, ret = 0u, last = 0u;
    bool invalid = false;
    for (int guard = 0; guard < 20000; ++guard) {
        const uint16_t ctrl = v1w(a);
        last = a;
        if (ctrl & 0x8000u) break;
        const int jp = (ctrl >> 12) & 7;
        if (!(jp & 4) && (ctrl & 0xF) >= 0xC) {
            invalid = true;
            break;
        }
        if (!(jp & 4)) {
            uint8_t cmd_bytes[32]{};
            for (uint32_t i = 0; i < 32u; ++i) {
                cmd_bytes[i] = vdp1_vram[(a + i) & 0x7FFFFu];
            }
            v1_command(*this, cmd_bytes, false);
        }
        const uint32_t link = (static_cast<uint32_t>(v1w(a + 2u)) * 8u) & 0x7FFFFu;
        switch (jp & 3) {
            case 0: a += 0x20u; break;
            case 1: a = link; break;
            case 2: ret = a + 0x20u; a = link; break;
            case 3: a = ret; break;
        }
        a &= 0x7FFFFu;
    }
    vdp1_regs[LOPR] = static_cast<uint16_t>(last >> 3);
    vdp1_regs[COPR] = static_cast<uint16_t>(last >> 3);
    ++draws;
    if (!invalid) {
        vdp1_regs[EDSR] |= 2u;  // synchronous software draw completes immediately (CEF=1)
    }
}

void StvVdpState::v1_draw_meta_only() {
    V1GpuFrame& gf = v1_gpu_frames[draw_fb];
    if (!gf.valid) {
        gf.valid = true;
        gf.cpu_materialized = false;
        gf.count = 0u;
        gf.tvmr = vdp1_regs[TVMR];
        gf.ewdr = 0u;
        gf.fb_w = static_cast<int16_t>(fb_width());
        gf.fb_h = static_cast<int16_t>(fb_height());
        gf.ew_x0 = 0;
        gf.ew_y0 = 0;
        gf.ew_x1 = gf.fb_w;
        gf.ew_y1 = gf.fb_h;
    } else {
        gf.cpu_materialized = false;
        gf.tvmr = vdp1_regs[TVMR];
        gf.fb_w = static_cast<int16_t>(fb_width());
        gf.fb_h = static_cast<int16_t>(fb_height());
    }
    v1_obs_draw_count = 0;
    copa_sp_vfx.draw_condor = CopaVfxBox{};
    copa_sp_vfx.draw_machu = CopaVfxBox{};
    copa_sp_vfx.draw_uru_flag = CopaVfxBox{};
    copa_sp_vfx.uru_anchor_ttl = 0u;
    copa_sp_vfx.draw_charge_count = 0u;
    vdp1_regs[EDSR] = static_cast<uint16_t>((vdp1_regs[EDSR] >> 1) & 1u);
    if (vdp1_regs[TVMR] & 2u) {
        vdp1_unsupported = true;
        return;
    }
    uint32_t a = 0u, ret = 0u, last = 0u;
    bool invalid = false;
    for (int guard = 0; guard < 20000; ++guard) {
        const uint16_t ctrl = v1w(a);
        last = a;
        if (ctrl & 0x8000u) break;
        const int jp = (ctrl >> 12) & 7;
        if (!(jp & 4) && (ctrl & 0xF) >= 0xC) {
            invalid = true;
            break;
        }
        if (!(jp & 4)) {
            uint8_t cmd_bytes[32]{};
            for (uint32_t i = 0; i < 32u; ++i) {
                cmd_bytes[i] = vdp1_vram[(a + i) & 0x7FFFFu];
            }
            v1_command(*this, cmd_bytes, true);
        }
        const uint32_t link = (static_cast<uint32_t>(v1w(a + 2u)) * 8u) & 0x7FFFFu;
        switch (jp & 3) {
            case 0: a += 0x20u; break;
            case 1: a = link; break;
            case 2: ret = a + 0x20u; a = link; break;
            case 3: a = ret; break;
        }
        a &= 0x7FFFFu;
    }
    vdp1_regs[LOPR] = static_cast<uint16_t>(last >> 3);
    vdp1_regs[COPR] = static_cast<uint16_t>(last >> 3);
    ++draws;
    if (!invalid) {
        vdp1_regs[EDSR] |= 2u;
    }
}

void StvVdpState::materialize_cpu_fb(int which) {
    which &= 1;
    V1GpuFrame& gf = v1_gpu_frames[which];
    if (!gf.valid || gf.cpu_materialized) return;
    gf.cpu_materialized = true;

    const int width = (gf.tvmr & 1u) ? 1024 : 512;
    const int height = (gf.tvmr & 1u) ? 512 : 256;
    const int cx0 = std::max(0, static_cast<int>(gf.ew_x0));
    const int cx1 = std::min(width, static_cast<int>(gf.ew_x1));
    const int cy0 = std::max(0, static_cast<int>(gf.ew_y0));
    const int cy1 = std::min(height, static_cast<int>(gf.ew_y1));
    if (cx0 < cx1 && cy0 < cy1) {
        uint16_t* const fb = vdp1_fb[which];
        if (gf.ewdr == 0u) {
            if (cx0 == 0 && cx1 == width) {
                std::memset(fb + static_cast<size_t>(cy0) * static_cast<size_t>(width), 0,
                            static_cast<size_t>(cy1 - cy0) * static_cast<size_t>(width) * sizeof(uint16_t));
            } else {
                const size_t row_bytes = static_cast<size_t>(cx1 - cx0) * sizeof(uint16_t);
                for (int y = cy0; y < cy1; ++y) {
                    std::memset(fb + static_cast<size_t>(y) * static_cast<size_t>(width) + cx0, 0, row_bytes);
                }
            }
        } else {
            const size_t span = static_cast<size_t>(cx1 - cx0);
            for (int y = cy0; y < cy1; ++y) {
                std::fill_n(fb + static_cast<size_t>(y) * static_cast<size_t>(width) + cx0, span, gf.ewdr);
            }
        }
    }

    const int saved_draw_fb = draw_fb;
    const uint16_t saved_tvmr = vdp1_regs[TVMR];
    const int saved_ux0 = ux0, saved_uy0 = uy0, saved_ux1 = ux1, saved_uy1 = uy1;
    const int saved_sys_x = sys_x, saved_sys_y = sys_y;
    const bool saved_unsupp = vdp1_unsupported;

    draw_fb = which;
    vdp1_regs[TVMR] = gf.tvmr;

    for (uint16_t idx = 0u; idx < gf.count; ++idx) {
        const V1GpuCmd& gc = gf.cmds[idx];
        ux0 = gc.ux0;
        uy0 = gc.uy0;
        ux1 = gc.ux1;
        uy1 = gc.uy1;
        sys_x = gc.sys_x;
        sys_y = gc.sys_y;

        V1Cmd c{};
        c.ctrl = gc.ctrl;
        c.pmod = gc.pmod;
        c.colr = gc.colr;
        c.srca = gc.srca;
        c.w = gc.w;
        c.h = gc.h;
        const int comm = c.ctrl & 0xF;
        c.cmode = (c.pmod >> 3) & 7;
        c.ccb = c.pmod & 7;
        c.spd = (c.pmod & 0x40u) != 0u;
        c.ecd = (c.pmod & 0x80u) != 0u;
        c.mesh = (c.pmod & 0x100u) != 0u;
        c.clip = (c.pmod & 0x400u) != 0u;
        c.outside = (c.pmod & 0x200u) != 0u;
        c.mon = (c.pmod & 0x8000u) != 0u;
        c.gouraud = (c.ccb & 4) != 0;
        c.hflip = (c.ctrl & 0x10u) != 0u;
        c.vflip = (c.ctrl & 0x20u) != 0u;
        c.textured = comm < 4;
        if (c.textured && c.cmode == 1) {
            for (int i = 0; i < 16; ++i) {
                const uint32_t a = (static_cast<uint32_t>(c.colr) * 8u + static_cast<uint32_t>(i * 2)) & 0x7FFFEu;
                c.lut[i] = static_cast<uint16_t>((vdp1_vram[a] << 8) | vdp1_vram[a + 1u]);
            }
        }
        if (c.gouraud) {
            for (int i = 0; i < 4; ++i) {
                const uint32_t a = (gc.gouraud_addr + static_cast<uint32_t>(i * 2)) & 0x7FFFEu;
                c.g[i] = rgb5_of(static_cast<uint16_t>((vdp1_vram[a] << 8) | vdp1_vram[a + 1u]));
            }
        }
        const Pt p0{gc.x[0], gc.y[0]};
        const Pt p1{gc.x[1], gc.y[1]};
        const Pt p2{gc.x[2], gc.y[2]};
        const Pt p3{gc.x[3], gc.y[3]};
        if (comm <= 0x4) {
            v1_quad(*this, c, p0, p1, p2, p3);
        } else if (comm == 0x5 || comm == 0x7) {
            const Pt pts[4] = {p0, p1, p2, p3};
            for (int i = 0; i < 4; ++i) {
                v1_line(*this, c, pts[i], pts[(i + 1) & 3], 0, c.g[i], c.g[(i + 1) & 3]);
            }
        } else if (comm == 0x6) {
            v1_line(*this, c, p0, p1, 0, c.g[0], c.g[1]);
        }
    }

    draw_fb = saved_draw_fb;
    vdp1_regs[TVMR] = saved_tvmr;
    ux0 = saved_ux0;
    uy0 = saved_uy0;
    ux1 = saved_ux1;
    uy1 = saved_uy1;
    sys_x = saved_sys_x;
    sys_y = saved_sys_y;
    vdp1_unsupported = saved_unsupp;
}

void StvVdpState::vblank_out() {
    if (erase_field) {
        if (gpu_vdp1_enabled) {
            v1_record_erase(draw_fb ^ 1);
        } else {
            v1_erase(draw_fb ^ 1);
        }
        erase_field = false;
    }
    const bool one_cycle = !(vdp1_regs[FBCR] & 2u);
    if (erase_pending) {
        erase_field = true;
        erase_pending = false;
    }
    if (one_cycle || change_pending) {
        change_pending = false;
        draw_fb ^= 1;
        v1_obs_disp_count = v1_obs_draw_count;
        for (int i = 0; i < v1_obs_draw_count; ++i) v1_obs_disp[i] = v1_obs_draw[i];
        if (copa_sp_vfx.draw_condor.active) {
            const int16_t cur_cx = static_cast<int16_t>(
                (static_cast<int>(copa_sp_vfx.draw_condor.min_x) + static_cast<int>(copa_sp_vfx.draw_condor.max_x)) / 2);
            if (copa_sp_vfx.condor_age == 0u) {
                copa_sp_vfx.condor_dir = copa_sp_vfx.draw_condor.hflip ? -1 : 1;
            } else {
                const int dx = static_cast<int>(cur_cx) - static_cast<int>(copa_sp_vfx.condor_prev_cx);
                if (dx >= 2) copa_sp_vfx.condor_dir = 1;
                else if (dx <= -2) copa_sp_vfx.condor_dir = -1;
            }
            copa_sp_vfx.condor_prev_cx = cur_cx;
            ++copa_sp_vfx.condor_age;
            copa_sp_vfx.draw_condor.anim_age = copa_sp_vfx.condor_age;
        } else {
            copa_sp_vfx.condor_age = 0u;
            copa_sp_vfx.condor_prev_cx = 0;
            copa_sp_vfx.condor_dir = 1;
        }
        if (copa_sp_vfx.draw_machu.active) {
            ++copa_sp_vfx.machu_age;
            copa_sp_vfx.draw_machu.anim_age = copa_sp_vfx.machu_age;
        } else {
            copa_sp_vfx.machu_age = 0u;
        }
        if (copa_sp_vfx.draw_uru_flag.active) {
            ++copa_sp_vfx.uru_age;
            copa_sp_vfx.draw_uru_flag.anim_age = copa_sp_vfx.uru_age;
        } else {
            copa_sp_vfx.uru_age = 0u;
        }
        if (copa_sp_vfx.draw_charge_count > 0u) {
            ++copa_sp_vfx.charge_age;
        } else {
            copa_sp_vfx.charge_age = 0u;
        }
        copa_sp_vfx.condor = copa_sp_vfx.draw_condor;
        copa_sp_vfx.machu = copa_sp_vfx.draw_machu;
        copa_sp_vfx.uru_flag = copa_sp_vfx.draw_uru_flag;
        copa_sp_vfx.charge_count = copa_sp_vfx.draw_charge_count;
        for (int i = 0; i < copa_sp_vfx.draw_charge_count; ++i) {
            copa_sp_vfx.charge_rays[i] = copa_sp_vfx.draw_charge_rays[i];
        }
        ++frame_changes;
        if (one_cycle) erase_field = true;
        if ((vdp1_regs[PTMR] & 3u) == 2u) {
            if (gpu_vdp1_enabled) v1_draw_meta_only();
            else v1_draw();
        }
    }
}

uint32_t StvVdpState::vdp1_reg_read(uint32_t off, int size) const {
    if (size == 4) {
        return (vdp1_reg_read(off, 2) << 16) | vdp1_reg_read(off + 2u, 2);
    }
    const uint16_t v = vdp1_regs[(off >> 1) & 0xFu];
    return (size == 1) ? ((off & 1u) ? (v & 0xFFu) : (v >> 8)) : v;
}

void StvVdpState::v1_reg_write16(uint32_t off, uint16_t v) {
    const int r = static_cast<int>((off >> 1) & 0xFu);
    if (r >= EDSR) return;
    vdp1_regs[r] = v;
    switch (r) {
        case FBCR:
            if ((v & 3u) == 3u) change_pending = true;
            if ((v & 3u) == 2u) erase_pending = true;
            break;
        case PTMR:
            if ((v & 3u) == 1u) {
                if (gpu_vdp1_enabled) v1_draw_meta_only();
                else v1_draw();
            }
            break;
        default:
            break;
    }
}

void StvVdpState::vdp1_reg_write(uint32_t off, uint32_t val, int size) {
    if (size == 4) {
        v1_reg_write16(off, static_cast<uint16_t>(val >> 16));
        v1_reg_write16(off + 2u, static_cast<uint16_t>(val));
    } else if (size == 2) {
        v1_reg_write16(off, static_cast<uint16_t>(val));
    } else {
        vdp1_unsupported = true;
    }
}

uint32_t StvVdpState::vdp1_fb_read(uint32_t off, int size) const {
    if (v1_gpu_frames[draw_fb].valid && !v1_gpu_frames[draw_fb].cpu_materialized) {
        const_cast<StvVdpState*>(this)->materialize_cpu_fb(draw_fb);
    }
    const uint16_t* fb = vdp1_fb[draw_fb];
    if (size == 4) {
        return (static_cast<uint32_t>(fb[(off >> 1) & 0x1FFFFu]) << 16) |
               fb[((off >> 1) + 1u) & 0x1FFFFu];
    }
    const uint16_t v = fb[(off >> 1) & 0x1FFFFu];
    return (size == 1) ? ((off & 1u) ? (v & 0xFFu) : (v >> 8)) : v;
}

void StvVdpState::vdp1_fb_write(uint32_t off, uint32_t val, int size) {
    if (v1_gpu_frames[draw_fb].valid && !v1_gpu_frames[draw_fb].cpu_materialized) {
        materialize_cpu_fb(draw_fb);
    }
    uint16_t* fb = vdp1_fb[draw_fb];
    if (size == 4) {
        fb[(off >> 1) & 0x1FFFFu] = static_cast<uint16_t>(val >> 16);
        fb[((off >> 1) + 1u) & 0x1FFFFu] = static_cast<uint16_t>(val);
        return;
    }
    uint16_t& w = fb[(off >> 1) & 0x1FFFFu];
    if (size == 2) {
        w = static_cast<uint16_t>(val);
    } else {
        w = (off & 1u)
            ? static_cast<uint16_t>((w & 0xFF00u) | (val & 0xFFu))
            : static_cast<uint16_t>((w & 0x00FFu) | ((val & 0xFFu) << 8));
    }
}

bool StvVdpState::has_cpu_only_overlays() const {
    if (copa_bracket.progress_active != 0u || copa_group_badges) return true;
    if (copa_vfx_replace && (copa_sp_vfx.condor.active || copa_sp_vfx.machu.active ||
                             copa_sp_vfx.uru_flag.active)) {
        return true;
    }
    return false;
}

void StvVdpState::compose_frame_prepare(const uint8_t vdp2_vram[0x80000], uint8_t dbg_hide_layers) {
    ComposePrepared& prep = compose_prep;
    const uint16_t tvmd = v2reg(0x00u);
    static constexpr int kWidths[8] = {320, 352, 640, 704, 320, 352, 640, 704};
    prep.raw_w = kWidths[tvmd & 7u];
    prep.raw_h = vdp2_lines();
    prep.lsmd3 = ((tvmd >> 6) & 3u) == 3u;
    prep.hires_x = (tvmd & 2u) != 0u;
    prep.hires_rbg = (dbg_hide_layers & 0x08u) != 0u;
    prep.half_y = low_res_224p && prep.lsmd3 && (prep.raw_h >= 448);
    prep.x_shift = (low_res_half_width && prep.half_y && prep.hires_x) ? 1 : 0;
    frame_w = prep.raw_w >> prep.x_shift;
    prep.y_step = prep.half_y ? 2 : 1;
    frame_h = prep.half_y ? (prep.raw_h / 2) : prep.raw_h;
    prep.dbg_hide_layers = dbg_hide_layers;
    prep.disp_on = (tvmd & 0x8000u) != 0u;
    if (!prep.disp_on) {
        std::memset(frame_px, 0, static_cast<size_t>(frame_w * frame_h) * sizeof(uint32_t));
        return;
    }

    build_cram_lut(*this);
    for (int n = 0; n < 4; ++n) {
        v2_setup_nbg(*this, vdp2_vram, n, compose_nbg[n]);
    }
    v2_setup_rbg(*this, vdp2_vram, compose_rbg);
    v2_setup_windows(*this);

    for (int i = 0; i < L_COUNT; ++i) {
        prep.wctl[i] = v2_wctl_of(*this, i) & 0x8Fu;
    }
    prep.bk = ((static_cast<uint32_t>(v2reg(0xACu) & 7u) << 16) | v2reg(0xAEu)) * 2u;
    prep.bk_lines = (v2reg(0xACu) & 0x8000u) != 0u;
    const uint16_t cctl = v2reg(0xECu);
    prep.add = ((cctl >> 8) & 1u) != 0u;
    prep.sp_cc_en = ((cctl >> 6) & 1u) != 0u;
    const uint16_t sfprmd = v2reg(0xEAu);
    const uint16_t spctl = v2reg(0xE0u);
    prep.sp_type = spctl & 0xF;
    prep.sp_mixed = ((spctl >> 5) & 1u) != 0u;
    prep.sp_ccmode = (spctl >> 12) & 3;
    prep.sp_ccnum = (spctl >> 8) & 7;
    prep.sp_caos = static_cast<uint32_t>((v2reg(0xE6u) >> 4) & 7u) << 8;
    const uint16_t off_en_a = v2reg(0x110u);
    const uint16_t off_en_b = v2reg(0x112u);
    prep.sp_off = ((off_en_a >> 6) & 1u) != 0u;
    prep.sp_offb = ((off_en_b >> 6) & 1u) != 0u;
    for (int i = 0; i < 8; ++i) {
        prep.sp_prio[i] = static_cast<uint8_t>((v2reg(static_cast<uint32_t>(0xF0 + (i / 2) * 2)) >> ((i & 1) * 8)) & 7u);
        prep.sp_ratio[i] = static_cast<uint8_t>((v2reg(static_cast<uint32_t>(0x100 + (i / 2) * 2)) >> ((i & 1) * 8)) & 0x1Fu);
    }

    prep.back_off = ((off_en_a >> 5) & 1u) != 0u;
    prep.back_offb = ((off_en_b >> 5) & 1u) != 0u;
    for (int b = 0; b < 2; ++b) {
        const uint32_t base = b ? 0x11Au : 0x114u;
        for (int c = 0; c < 3; ++c) {
            prep.off_vals[b][c] = static_cast<int>(static_cast<uint32_t>(v2reg(base + static_cast<uint32_t>(c * 2)) & 0x1FFu) << 23) >> 23;
        }
    }

    prep.active_nbg_count = 0;
    for (int n = 0; n < 4; ++n) {
        const Nbg& s = compose_nbg[n];
        if (s.on && (s.prio || ((sfprmd >> (n * 2)) & 3u))) {
            prep.active_nbgs[prep.active_nbg_count++] = n;
        }
    }
    prep.rbg_active = ((dbg_hide_layers & 2u) == 0u) && compose_rbg.on && (compose_rbg.prio || ((sfprmd >> 8) & 3u));
    prep.rbg_perdot_x = compose_rbg.perdot &&
                        ((compose_rbg.par[0].coef && compose_rbg.par[0].dkax != 0) ||
                         (compose_rbg.par[1].coef && compose_rbg.par[1].dkax != 0));
    if (dbg_hide_layers & 4u) prep.active_nbg_count = 0;
    prep.win_sh = (tvmd & 6u) ? 0 : 1;

    const uint16_t sfccmd = v2reg(0xEEu);
    const uint16_t sfcode = v2reg(0x26u);
    const uint16_t sfsel = v2reg(0x24u);
    for (int l = 0; l < 5; ++l) {
        prep.sf_prio_mode[l] = (sfprmd >> (l * 2)) & 3;
        prep.sf_cc_mode[l] = (sfccmd >> (l * 2)) & 3;
        prep.sf_code_byte[l] = static_cast<uint8_t>(sfcode >> (((sfsel >> l) & 1u) * 8));
    }

    prep.fb_pitch = (vdp1_regs[TVMR] & 1u) ? 1024 : 512;
    prep.bk_base_rgb = prep.bk_lines ? 0u : rgb555_to_888(v2_vw(vdp2_vram, prep.bk));

    // Precompute immutable per-scanline RBG0 / Window / Back RGB tables (shared by CPU threads & GPU VDP2).
    const int y_shift = (prep.lsmd3 && prep.hires_rbg) ? 1 : 0;
    int last_rbg_y = -1;
    int out_y = 0;
    for (int y = 0; y < prep.raw_h && out_y < kMaxFrameH; y += prep.y_step, ++out_y) {
        prep.back_rgb_line[out_y] = prep.bk_lines
            ? rgb555_to_888(v2_vw(vdp2_vram, prep.bk + static_cast<uint32_t>(y) * 2u))
            : prep.bk_base_rgb;

        if (compose_rbg.on) {
            const int rbg_y = (prep.lsmd3 && !prep.hires_rbg) ? (y / 2) : y;
            if (rbg_y != last_rbg_y) {
                last_rbg_y = rbg_y;
                v2_rbg_line_eval(*this, vdp2_vram, compose_rbg, rbg_y, y_shift, prep.rbg_lines[out_y]);
            } else if (out_y > 0) {
                prep.rbg_lines[out_y][0] = prep.rbg_lines[out_y - 1][0];
                prep.rbg_lines[out_y][1] = prep.rbg_lines[out_y - 1][1];
            }
        }

        for (int wi = 0; wi < 2; ++wi) {
            const Window& w = compose_win[wi];
            const bool ok = (y >= w.y0 && y <= w.y1);
            prep.win_y_ok[out_y][wi] = ok ? 1u : 0u;
            if (ok) {
                if (w.lines) {
                    prep.win_x0[out_y][wi] = static_cast<int16_t>((v2_vw(vdp2_vram, w.table + static_cast<uint32_t>(y) * 4u) & 0x3FF) >> prep.win_sh);
                    prep.win_x1[out_y][wi] = static_cast<int16_t>((v2_vw(vdp2_vram, w.table + static_cast<uint32_t>(y) * 4u + 2u) & 0x3FF) >> prep.win_sh);
                } else {
                    prep.win_x0[out_y][wi] = static_cast<int16_t>(w.x0);
                    prep.win_x1[out_y][wi] = static_cast<int16_t>(w.x1);
                }
            }
        }
    }
}

void StvVdpState::compose_frame_band(const uint8_t vdp2_vram[0x80000], int out_y_begin, int out_y_end) {
    const ComposePrepared& prep = compose_prep;
    if (!prep.disp_on) return;
    if (out_y_begin < 0) out_y_begin = 0;
    if (out_y_end > frame_h) out_y_end = frame_h;
    if (out_y_begin >= out_y_end) return;

    materialize_cpu_display_fb();
    const SpriteType& sp_t = kSpriteTypes[prep.sp_type];
    const uint16_t* const fb = vdp1_display();
    const int fb_h = fb_height();
    const int fb_pitch = prep.fb_pitch;
    const int fw = frame_w;
    const int x_shift = prep.x_shift;
    const bool half_y = prep.half_y;
    const bool add = prep.add;
    const bool sp_cc_en = prep.sp_cc_en;
    const bool sp_mixed = prep.sp_mixed;
    const int sp_type = prep.sp_type;
    const int sp_ccmode = prep.sp_ccmode;
    const int sp_ccnum = prep.sp_ccnum;
    const uint32_t sp_caos = prep.sp_caos;
    const bool sp_off = prep.sp_off;
    const bool sp_offb = prep.sp_offb;
    const uint32_t* const cram_rgb = prep.cram_rgb;
    const uint8_t* const cram_msb = prep.cram_msb;
    const uint32_t cram_mask = prep.cram_mask;
    const uint8_t back_attr = static_cast<uint8_t>((prep.back_off ? 0x40u : 0u) | (prep.back_offb ? 0x80u : 0u));

    auto fast_sf_code = [&](int layer, const Pix& p) -> bool {
        return !p.direct && (((prep.sf_code_byte[layer] >> ((p.dcc & 0xEu) >> 1)) & 1u) != 0u);
    };
    auto fast_sf_prio = [&](int layer, uint8_t prio, const Pix& p) -> uint8_t {
        const int mode = prep.sf_prio_mode[layer];
        if (!mode) return prio;
        const bool bit = (mode == 1) ? p.spr : (mode == 2 ? (p.spr && fast_sf_code(layer, p)) : false);
        return static_cast<uint8_t>((prio & ~1u) | (bit ? 1u : 0u));
    };
    auto fast_sf_cc = [&](int layer, bool cc, const Pix& p) -> bool {
        switch (prep.sf_cc_mode[layer]) {
            case 0: return cc;
            case 1: return cc && p.scc;
            case 2: return cc && p.scc && fast_sf_code(layer, p);
            default: return cc && p.msb;
        }
    };

    alignas(16) uint32_t top_rgb[kMaxFrameW];
    alignas(16) uint32_t sec_rgb[kMaxFrameW];
    alignas(16) uint8_t top_prio[kMaxFrameW];
    alignas(16) uint8_t sec_prio[kMaxFrameW];
    alignas(16) uint8_t top_attr[kMaxFrameW];

    for (int out_y = out_y_begin; out_y < out_y_end; ++out_y) {
        const int y = out_y * prep.y_step;
        const uint32_t back_rgb = prep.back_rgb_line[out_y];
        const bool wy0 = prep.win_y_ok[out_y][0] != 0u;
        const bool wy1 = prep.win_y_ok[out_y][1] != 0u;
        const int wx0_0 = prep.win_x0[out_y][0];
        const int wx1_0 = prep.win_x1[out_y][0];
        const int wx0_1 = prep.win_x0[out_y][1];
        const int wx1_1 = prep.win_x1[out_y][1];

        auto line_windowed = [&](uint8_t ctl, int x) -> bool {
            if (!(ctl & 0x0Au)) return false;
            bool any = false, all = true;
            if (ctl & 0x02u) {
                const bool in0 = wy0 && (x >= wx0_0 && x <= wx1_0);
                const bool a0 = (ctl & 0x01u) ? !in0 : in0;
                any |= a0;
                all &= a0;
            }
            if (ctl & 0x08u) {
                const bool in1 = wy1 && (x >= wx0_1 && x <= wx1_1);
                const bool a1 = (ctl & 0x04u) ? !in1 : in1;
                any |= a1;
                all &= a1;
            }
            return (ctl & 0x80u) ? all : any;
        };

        const uint16_t* const fb_row = fb + y * fb_pitch;
        const uint16_t* const fb_row1 = (half_y && (y + 1 < prep.raw_h) && ((y + 1) < fb_h))
            ? (fb + (y + 1) * fb_pitch) : nullptr;
        uint32_t* const out_row = frame_px + out_y * fw;

        // Pass 0: Row buffer initialization (SIMD128 accelerated on Wasm)
#if defined(__wasm_simd128__)
        {
            const v128_t v_back = wasm_i32x4_splat(static_cast<int32_t>(back_rgb));
            int x = 0;
            for (; x + 4 <= fw; x += 4) {
                wasm_v128_store(top_rgb + x, v_back);
                wasm_v128_store(sec_rgb + x, v_back);
            }
            for (; x < fw; ++x) {
                top_rgb[x] = back_rgb;
                sec_rgb[x] = back_rgb;
            }
            std::memset(top_prio, 0, static_cast<size_t>(fw));
            std::memset(sec_prio, 0, static_cast<size_t>(fw));
            std::memset(top_attr, back_attr, static_cast<size_t>(fw));
        }
#else
        for (int x = 0; x < fw; ++x) {
            top_rgb[x] = back_rgb;
            sec_rgb[x] = back_rgb;
            top_prio[x] = 0u;
            sec_prio[x] = 0u;
            top_attr[x] = back_attr;
        }
#endif

        bool line_any_cc = false;
        bool line_any_off = prep.back_off;

        auto push_x = [&](int x, uint32_t rgb, uint8_t prio, uint8_t ratio, bool cc, bool off, bool offb) {
            const uint8_t tp = top_prio[x];
            if (tp == 0u || prio > tp) {
                if (tp != 0u) {
                    sec_rgb[x] = top_rgb[x];
                    sec_prio[x] = tp;
                }
                top_rgb[x] = rgb;
                top_prio[x] = prio;
                top_attr[x] = static_cast<uint8_t>((ratio & 0x1Fu) | (cc ? 0x20u : 0u) | (off ? 0x40u : 0u) | (offb ? 0x80u : 0u));
                line_any_cc |= cc;
                line_any_off |= off;
            } else if (sec_prio[x] == 0u || prio > sec_prio[x]) {
                sec_rgb[x] = rgb;
                sec_prio[x] = prio;
            }
        };

        // Pass 1: Sprites (VDP1 framebuffer)
        if ((prep.dbg_hide_layers & 1u) == 0u) {
            const uint8_t sp_wctl = prep.wctl[L_SPRITE];
            const bool sp_has_win = (sp_wctl & 0x0Au) != 0u;
            const bool sp_byte_mask = (sp_type >= 8);
            const uint16_t sp_dc_mask = sp_t.dc_mask;
            const int sp_pr_mask = sp_t.pr_mask;
            const int sp_pr_shift = sp_t.pr_shift;
            const int sp_cc_mask = sp_t.cc_mask;
            const int sp_cc_shift = sp_t.cc_shift;
            const bool sp_use_row1_add = (fb_row1 != nullptr) && add && ((out_y & 1) != 0);
            const bool sp_use_mesh_pair = (fb_row1 != nullptr) && !add;
            const uint16_t* const sp_primary = sp_use_row1_add ? fb_row1 : fb_row;

            for (int x = 0; x < fw; ++x) {
                const int src_x = x << x_shift;
                uint16_t d0 = sp_primary[src_x];
                bool half_mesh_blend = false;
                if (sp_use_mesh_pair) {
                    const uint16_t d1 = fb_row1[src_x];
                    if (d0 == 0u) {
                        if (d1 == 0u) continue;
                        d0 = d1;
                        half_mesh_blend = true;
                    } else if (d1 == 0u) {
                        half_mesh_blend = true;
                    }
                } else if (d0 == 0u) {
                    continue;
                }
                if (sp_has_win && line_windowed(sp_wctl, src_x)) continue;
                uint32_t srgb = 0u;
                uint8_t sprio = 0u;
                uint8_t sratio = 0u;
                bool msb = false;
                if (sp_mixed && (d0 & 0x8000u)) {
                    srgb = rgb555_to_888(d0);
                    sprio = prep.sp_prio[0];
                    sratio = prep.sp_ratio[0];
                    msb = true;
                } else {
                    const uint16_t d = sp_byte_mask ? (d0 & 0xFFu) : d0;
                    const uint16_t dc = d & sp_dc_mask;
                    if (dc == 0u) continue;
                    const int pr = sp_pr_mask ? ((d >> sp_pr_shift) & sp_pr_mask) : 0;
                    const int cc = sp_cc_mask ? ((d >> sp_cc_shift) & sp_cc_mask) : 0;
                    sprio = prep.sp_prio[pr];
                    sratio = prep.sp_ratio[cc];
                    const uint32_t cidx = (dc + sp_caos) & cram_mask;
                    msb = cram_msb[cidx] != 0u;
                    srgb = cram_rgb[cidx];
                }
                if (sprio == 0u) continue;
                bool scc = false;
                if (sp_cc_en) {
                    switch (sp_ccmode) {
                        case 0: scc = (sprio <= sp_ccnum); break;
                        case 1: scc = (sprio == sp_ccnum); break;
                        case 2: scc = (sprio >= sp_ccnum); break;
                        default: scc = msb; break;
                    }
                }
                if (half_mesh_blend) {
                    if (scc) {
                        const int inv = (32 - static_cast<int>(sratio & 0x1Fu)) >> 1;
                        sratio = static_cast<uint8_t>(std::min(31, 32 - inv));
                    } else {
                        scc = true;
                        sratio = 16u;
                    }
                }
                push_x(x, srgb, sprio, sratio, scc, sp_off, sp_offb);
            }
        }

        // Pass 2: RBG0
        if (prep.rbg_active) {
            const uint8_t r_wctl = prep.wctl[L_RBG0];
            const bool r_has_win = (r_wctl & 0x0Au) != 0u;
            const bool pair_cache = prep.hires_x && !prep.hires_rbg && !prep.rbg_perdot_x && !x_shift;
            const int rx_shift = (prep.hires_x && prep.hires_rbg) ? 1 : 0;
            const RbgLineParams* const r_lps = prep.rbg_lines[out_y];

            if (!prep.rbg_perdot_x) {
                // Fast scanline-constant coefficient path (100% bit-exact when dkax == 0, covering twcup98 pitch)
                int sel = (compose_rbg.mode == 1) ? 1 : 0;
                uint32_t c = 0u;
                if (compose_rbg.mode == 2) {
                    c = compose_rbg.par[0].coef
                        ? (compose_rbg.perdot ? r_lps[0].line_coef_dot : r_lps[0].line_coef)
                        : 0u;
                    if (c >> 31) {
                        sel = 1;
                        c = r_lps[1].line_coef;
                    }
                } else if (compose_rbg.par[sel].coef) {
                    c = compose_rbg.perdot ? r_lps[sel].line_coef_dot : r_lps[sel].line_coef;
                }
                const RotParam& rp_cfg = compose_rbg.par[sel];
                const RbgLineParams& lp = r_lps[sel];
                bool skip_line = false;
                int32_t kx = rp_cfg.kx, ky = rp_cfg.ky;
                uint32_t xp_base = static_cast<uint32_t>(lp.xp);
                if (rp_cfg.coef) {
                    if (c >> 31) {
                        skip_line = true;
                    } else {
                        const int32_t v = sext_bits(c, 24);
                        switch (rp_cfg.coef_mode) {
                            case 0: kx = ky = v; break;
                            case 1: kx = v; break;
                            case 2: ky = v; break;
                            default: xp_base = static_cast<uint32_t>(v) << 2; break;
                        }
                    }
                }

                if (!skip_line) {
                    const Nbg& rs = rp_cfg.chr;
                    const uint32_t yp_base = static_cast<uint32_t>(lp.yp);
                    const int64_t kx64 = static_cast<int64_t>(kx);
                    const int64_t ky64 = static_cast<int64_t>(ky);
                    const uint32_t mw = rs.bitmap ? static_cast<uint32_t>(rs.bm_w) : (static_cast<uint32_t>(rs.plane_w) << 11);
                    const uint32_t mh = rs.bitmap ? static_cast<uint32_t>(rs.bm_h) : (static_cast<uint32_t>(rs.plane_h) << 11);
                    const uint32_t mw_mask = mw - 1u;
                    const uint32_t mh_mask = mh - 1u;
                    const int over_mode = rp_cfg.over;
                    const bool fast_cell8 = !rs.bitmap && !rs.cell2x2 && (rs.colors == 0 || rs.colors == 1) &&
                                            prep.sf_prio_mode[4] == 0 && prep.sf_cc_mode[4] == 0;
                    const uint8_t r_prio = compose_rbg.prio;
                    const uint8_t r_ratio = compose_rbg.ratio;
                    const bool r_cc = compose_rbg.cc;
                    const bool r_off = compose_rbg.offset;
                    const bool r_offb = compose_rbg.offset_b;

                    if (fast_cell8 && r_prio != 0u) {
                        const uint32_t pw_sh = (rs.plane_w == 2) ? 10u : 9u;
                        const uint32_t ph_sh = (rs.plane_h == 2) ? 10u : 9u;
                        const uint32_t pw_mask = (1u << pw_sh) - 1u;
                        const uint32_t ph_mask = (1u << ph_sh) - 1u;
                        const uint32_t pnd_sh = rs.word1 ? 1u : 2u;
                        const uint32_t plane_w_u = static_cast<uint32_t>(rs.plane_w);
                        const uint32_t page_bytes = rs.page_bytes;
                        const bool is_8bpp = (rs.colors == 1);
                        const bool is_word1 = rs.word1;
                        const bool is_cnsm = rs.cnsm;
                        const uint16_t supp = rs.supp;
                        const uint32_t caos = rs.caos;
                        const bool opaque = rs.opaque;
                        uint32_t last_pnd = 0xFFFFFFFFu;
                        uint32_t c_base = 0u;
                        uint32_t c_pal_off = 0u;
                        bool c_hf = false, c_vf = false;

                        auto eval_rbg_fast = [&](int rx, uint32_t& out_rgb) -> bool {
                            const int32_t xsp_x = rx_shift
                                ? static_cast<int32_t>(lp.xsp + ((static_cast<int64_t>(lp.ldx) * rx) >> rx_shift))
                                : static_cast<int32_t>(lp.xsp + lp.ldx * rx);
                            const int32_t ysp_x = rx_shift
                                ? static_cast<int32_t>(lp.ysp + ((static_cast<int64_t>(lp.ldy) * rx) >> rx_shift))
                                : static_cast<int32_t>(lp.ysp + lp.ldy * rx);
                            const uint32_t ix = (xp_base + static_cast<uint32_t>((kx64 * xsp_x) >> 16)) >> 10;
                            const uint32_t iy = (yp_base + static_cast<uint32_t>((ky64 * ysp_x) >> 16)) >> 10;
                            bool outside = false;
                            if (over_mode == 3) outside = ((ix | iy) & ~511u) != 0u;
                            else if (over_mode) outside = ((ix & ~mw_mask) != 0u) || ((iy & ~mh_mask) != 0u);
                            if (outside) {
                                if (over_mode & 2) return false;
                                Pix op{};
                                if (!v2_cell_pixel(prep, vdp2_vram, rs, rp_cfg.plane, 4, ix & mw_mask, iy & mh_mask, op, &rp_cfg.over_pn)) {
                                    return false;
                                }
                                out_rgb = op.rgb;
                                return true;
                            }
                            const uint32_t sx = ix & mw_mask;
                            const uint32_t sy = iy & mh_mask;
                            const uint32_t c_ix = sx & pw_mask;
                            const uint32_t c_iy = sy & ph_mask;
                            const uint32_t pl = ((sy >> ph_sh) << 2) + (sx >> pw_sh);
                            const uint32_t page = (c_iy >> 9) * plane_w_u + (c_ix >> 9);
                            const uint32_t pnd_addr = rp_cfg.plane[pl] + page * page_bytes + ((((c_iy & 511u) >> 3) * 64u + ((c_ix & 511u) >> 3)) << pnd_sh);
                            if (pnd_addr != last_pnd) {
                                last_pnd = pnd_addr;
                                uint32_t chr = 0u, pal = 0u;
                                if (is_word1) {
                                    const uint16_t w = v2_vw(vdp2_vram, pnd_addr);
                                    pal = is_8bpp ? static_cast<uint32_t>((w >> 8) & 0x70u)
                                                  : (static_cast<uint32_t>((w >> 12) & 0xFu) | ((supp >> 1) & 0x70u));
                                    if (!is_cnsm) {
                                        c_hf = (w & 0x400u) != 0u;
                                        c_vf = (w & 0x800u) != 0u;
                                        chr = (w & 0x3FFu) | ((supp & 0x1Fu) << 10);
                                    } else {
                                        c_hf = c_vf = false;
                                        chr = (w & 0xFFFu) | ((supp & 0x1Cu) << 10);
                                    }
                                } else {
                                    const uint16_t w0 = v2_vw(vdp2_vram, pnd_addr);
                                    const uint16_t w1 = v2_vw(vdp2_vram, pnd_addr + 2u);
                                    c_hf = (w0 & 0x4000u) != 0u;
                                    c_vf = (w0 & 0x8000u) != 0u;
                                    pal = w0 & 0x7Fu;
                                    chr = w1 & 0x7FFFu;
                                }
                                c_base = chr << 5;
                                c_pal_off = (is_8bpp ? ((pal & 0x70u) << 4) : (pal << 4)) + caos;
                            }
                            uint32_t px = c_ix & 7u;
                            uint32_t py = c_iy & 7u;
                            if (c_hf) px = 7u - px;
                            if (c_vf) py = 7u - py;
                            const uint32_t idx_in_tile = (py << 3) | px;
                            uint32_t v;
                            if (is_8bpp) {
                                v = vdp2_vram[(c_base + idx_in_tile) & 0x7FFFFu];
                            } else {
                                const uint8_t b = vdp2_vram[(c_base + (idx_in_tile >> 1)) & 0x7FFFFu];
                                v = (idx_in_tile & 1u) ? (b & 0xFu) : (b >> 4);
                            }
                            if (v == 0u && !opaque) return false;
                            out_rgb = cram_rgb[(c_pal_off + v) & cram_mask];
                            return true;
                        };

                        if (pair_cache) {
                            for (int x = 0; x < fw; x += 2) {
                                const bool w0 = r_has_win && line_windowed(r_wctl, x);
                                const bool w1 = (x + 1 < fw) ? (r_has_win && line_windowed(r_wctl, x + 1)) : true;
                                if (w0 && w1) continue;
                                uint32_t rgb = 0u;
                                if (eval_rbg_fast(x >> 1, rgb)) {
                                    if (!w0) push_x(x, rgb, r_prio, r_ratio, r_cc, r_off, r_offb);
                                    if (!w1) push_x(x + 1, rgb, r_prio, r_ratio, r_cc, r_off, r_offb);
                                }
                            }
                        } else {
                            for (int x = 0; x < fw; ++x) {
                                const int src_x = x << x_shift;
                                if (r_has_win && line_windowed(r_wctl, src_x)) continue;
                                const int rx = (prep.hires_x && !prep.hires_rbg) ? (src_x >> 1) : src_x;
                                uint32_t rgb = 0u;
                                if (eval_rbg_fast(rx, rgb)) {
                                    push_x(x, rgb, r_prio, r_ratio, r_cc, r_off, r_offb);
                                }
                            }
                        }
                    } else {
                        // General RBG0 fallback for bitmap / cell2x2 / 16-bit / special priority-CC
                        RbgCellCache rbg_line_cache[2]{};
                        if (pair_cache) {
                            for (int x = 0; x < fw; x += 2) {
                                const bool w0 = r_has_win && line_windowed(r_wctl, x);
                                const bool w1 = (x + 1 < fw) ? (r_has_win && line_windowed(r_wctl, x + 1)) : true;
                                if (w0 && w1) continue;
                                Pix rp{};
                                if (v2_rbg_pixel(*this, vdp2_vram, compose_rbg, r_lps, x >> 1, rp, rbg_line_cache, rx_shift)) {
                                    const uint8_t rprio = fast_sf_prio(4, r_prio, rp);
                                    if (rprio) {
                                        const bool rcc = fast_sf_cc(4, r_cc, rp);
                                        if (!w0) push_x(x, rp.rgb, rprio, r_ratio, rcc, r_off, r_offb);
                                        if (!w1) push_x(x + 1, rp.rgb, rprio, r_ratio, rcc, r_off, r_offb);
                                    }
                                }
                            }
                        } else {
                            for (int x = 0; x < fw; ++x) {
                                const int src_x = x << x_shift;
                                if (r_has_win && line_windowed(r_wctl, src_x)) continue;
                                const int rx = (prep.hires_x && !prep.hires_rbg) ? (src_x >> 1) : src_x;
                                Pix rp{};
                                if (v2_rbg_pixel(*this, vdp2_vram, compose_rbg, r_lps, rx, rp, rbg_line_cache, rx_shift)) {
                                    const uint8_t rprio = fast_sf_prio(4, r_prio, rp);
                                    if (rprio) {
                                        const bool rcc = fast_sf_cc(4, r_cc, rp);
                                        push_x(x, rp.rgb, rprio, r_ratio, rcc, r_off, r_offb);
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // Per-dot coefficient table path (dkax != 0)
                RbgCellCache rbg_line_cache[2]{};
                for (int x = 0; x < fw; ++x) {
                    const int src_x = x << x_shift;
                    if (r_has_win && line_windowed(r_wctl, src_x)) continue;
                    const int rx = (prep.hires_x && !prep.hires_rbg) ? (src_x >> 1) : src_x;
                    Pix rp{};
                    if (v2_rbg_pixel(*this, vdp2_vram, compose_rbg, r_lps, rx, rp, rbg_line_cache, rx_shift)) {
                        const uint8_t rprio = fast_sf_prio(4, compose_rbg.prio, rp);
                        if (rprio) {
                            const bool rcc = fast_sf_cc(4, compose_rbg.cc, rp);
                            push_x(x, rp.rgb, rprio, compose_rbg.ratio,
                                   rcc, compose_rbg.offset, compose_rbg.offset_b);
                        }
                    }
                }
            }
        }

        // Pass 3: Active NBG0..3 layers
        for (int idx = 0; idx < prep.active_nbg_count; ++idx) {
            const int n = prep.active_nbgs[idx];
            const Nbg& s = compose_nbg[n];
            NbgScanCache nc{};
            v2_nbg_begin_line(s, y, nc);
            const uint8_t n_wctl = prep.wctl[L_NBG0 + n];
            const bool n_has_win = (n_wctl & 0x0Au) != 0u;
            const int n_sf_pm = prep.sf_prio_mode[n];
            const int n_sf_cm = prep.sf_cc_mode[n];
            const uint8_t n_sf_cb = prep.sf_code_byte[n];

            if (!s.bitmap && !s.cell2x2 && nc.dx == 0x100 && x_shift == 0 &&
                !n_has_win && n_sf_pm == 0 && n_sf_cm == 0 && (s.colors == 0 || s.colors == 1)) {
                // Fast 8-pixel cell-span NBG path (zero per-pixel tile/plane math + empty-tile skip)
                const uint32_t sx_base = static_cast<uint32_t>(nc.bx) >> 8;
                const uint32_t sx_mask = nc.sx_mask;
                const uint32_t pw_mask = nc.pw_mask;
                const uint32_t pw_sh = nc.pw_sh;
                const uint32_t pl_row_off = nc.pl_row_off;
                const uint32_t page_row_off = nc.page_row_off;
                const uint32_t cy_row_off = nc.cy_row_off;
                const uint32_t pnd_sh = s.word1 ? 1u : 2u;
                const uint32_t page_bytes = s.page_bytes;
                const bool is_8bpp = (s.colors == 1);
                const bool is_word1 = s.word1;
                const bool is_cnsm = s.cnsm;
                const uint16_t supp = s.supp;
                const uint32_t caos = s.caos;
                const bool opaque = s.opaque;
                const uint8_t nprio = s.prio;
                const uint8_t nratio = s.ratio;
                const bool ncc = s.cc;
                const bool noff = s.offset;
                const bool noffb = s.offset_b;

                int x = 0;
                while (x < fw) {
                    const uint32_t sx = (sx_base + static_cast<uint32_t>(x)) & sx_mask;
                    const uint32_t ix = sx & pw_mask;
                    const uint32_t px_start = ix & 7u;
                    const int span = std::min(static_cast<int>(8u - px_start), fw - x);
                    const uint32_t pl = pl_row_off + (sx >> pw_sh);
                    const uint32_t page = page_row_off + (ix >> 9);
                    const uint32_t cx = (ix & 511u) >> 3;
                    const uint32_t pnd_addr = s.plane[pl] + page * page_bytes + ((cy_row_off + cx) << pnd_sh);

                    uint32_t chr = 0u, pal = 0u;
                    bool hf = false, vf = false;
                    if (is_word1) {
                        const uint16_t w = v2_vw(vdp2_vram, pnd_addr);
                        pal = is_8bpp ? static_cast<uint32_t>((w >> 8) & 0x70u)
                                      : (static_cast<uint32_t>((w >> 12) & 0xFu) | ((supp >> 1) & 0x70u));
                        if (!is_cnsm) {
                            hf = (w & 0x400u) != 0u;
                            vf = (w & 0x800u) != 0u;
                            chr = (w & 0x3FFu) | ((supp & 0x1Fu) << 10);
                        } else {
                            chr = (w & 0xFFFu) | ((supp & 0x1Cu) << 10);
                        }
                    } else {
                        const uint16_t w0 = v2_vw(vdp2_vram, pnd_addr);
                        const uint16_t w1 = v2_vw(vdp2_vram, pnd_addr + 2u);
                        hf = (w0 & 0x4000u) != 0u;
                        vf = (w0 & 0x8000u) != 0u;
                        pal = w0 & 0x7Fu;
                        chr = w1 & 0x7FFFu;
                    }

                    const uint32_t py = static_cast<uint32_t>(vf ? (7 - nc.py_raw) : nc.py_raw);
                    const uint32_t pal_off = (is_8bpp ? ((pal & 0x70u) << 4) : (pal << 4)) + caos;

                    if (!is_8bpp) {
                        const uint32_t row_addr = ((chr << 5) + (py << 2)) & 0x7FFFCu;
                        const uint8_t b0 = vdp2_vram[row_addr];
                        const uint8_t b1 = vdp2_vram[row_addr + 1u];
                        const uint8_t b2 = vdp2_vram[row_addr + 2u];
                        const uint8_t b3 = vdp2_vram[row_addr + 3u];
                        if (!opaque && (b0 | b1 | b2 | b3) == 0u) {
                            x += span;
                            continue;
                        }
                        const uint8_t row_bytes[4] = {b0, b1, b2, b3};
                        for (int k = 0; k < span; ++k) {
                            const uint32_t px = hf ? (7u - (px_start + static_cast<uint32_t>(k)))
                                                   : (px_start + static_cast<uint32_t>(k));
                            const uint8_t rb = row_bytes[px >> 1];
                            const uint32_t v = (px & 1u) ? (rb & 0xFu) : (rb >> 4);
                            if (v == 0u && !opaque) continue;
                            push_x(x + k, cram_rgb[(pal_off + v) & cram_mask], nprio, nratio, ncc, noff, noffb);
                        }
                    } else {
                        const uint32_t row_addr = ((chr << 5) + (py << 3)) & 0x7FFF8u;
                        const uint8_t* const rptr = vdp2_vram + row_addr;
                        if (!opaque &&
                            ((rptr[0] | rptr[1] | rptr[2] | rptr[3] | rptr[4] | rptr[5] | rptr[6] | rptr[7]) == 0u)) {
                            x += span;
                            continue;
                        }
                        for (int k = 0; k < span; ++k) {
                            const uint32_t px = hf ? (7u - (px_start + static_cast<uint32_t>(k)))
                                                   : (px_start + static_cast<uint32_t>(k));
                            const uint32_t v = rptr[px];
                            if (v == 0u && !opaque) continue;
                            push_x(x + k, cram_rgb[(pal_off + v) & cram_mask], nprio, nratio, ncc, noff, noffb);
                        }
                    }
                    x += span;
                }
                continue;
            }

            for (int x = 0; x < fw; ++x) {
                const int src_x = x << x_shift;
                if (n_has_win && line_windowed(n_wctl, src_x)) continue;
                Pix p{};
                if (!v2_nbg_pixel_fast(prep, vdp2_vram, s, nc, src_x, p)) continue;
                uint8_t nprio = s.prio;
                if (n_sf_pm) {
                    const bool bit = (n_sf_pm == 1) ? p.spr
                        : (n_sf_pm == 2 ? (p.spr && !p.direct && (((n_sf_cb >> ((p.dcc & 0xEu) >> 1)) & 1u) != 0u)) : false);
                    nprio = static_cast<uint8_t>((nprio & ~1u) | (bit ? 1u : 0u));
                    if (!nprio) continue;
                }
                bool ncc = s.cc;
                if (n_sf_cm) {
                    switch (n_sf_cm) {
                        case 1: ncc = ncc && p.scc; break;
                        case 2: ncc = ncc && p.scc && !p.direct && (((n_sf_cb >> ((p.dcc & 0xEu) >> 1)) & 1u) != 0u); break;
                        default: ncc = ncc && p.msb; break;
                    }
                }
                push_x(x, p.rgb, nprio, s.ratio, ncc, s.offset, s.offset_b);
            }
        }

        // Pass 4: Color calculation + offset -> output row
        if (!line_any_cc && !line_any_off) {
            std::memcpy(out_row, top_rgb, static_cast<size_t>(fw) * sizeof(uint32_t));
        } else {
            for (int x = 0; x < fw; ++x) {
                uint32_t rgb = top_rgb[x];
                const uint8_t attr = top_attr[x];
                if ((attr & 0x20u) && top_prio[x] != 0u) {
                    const uint32_t sec = sec_rgb[x];
                    const int ar = static_cast<int>((rgb >> 16) & 0xFFu);
                    const int ag = static_cast<int>((rgb >> 8) & 0xFFu);
                    const int ab = static_cast<int>(rgb & 0xFFu);
                    const int br = static_cast<int>((sec >> 16) & 0xFFu);
                    const int bg = static_cast<int>((sec >> 8) & 0xFFu);
                    const int bb = static_cast<int>(sec & 0xFFu);
                    int cr, cg, cb;
                    if (add) {
                        cr = std::min(ar + br, 255);
                        cg = std::min(ag + bg, 255);
                        cb = std::min(ab + bb, 255);
                    } else {
                        const int rat = attr & 0x1Fu;
                        const int inv = 32 - rat;
                        cr = (ar * inv + br * rat) >> 5;
                        cg = (ag * inv + bg * rat) >> 5;
                        cb = (ab * inv + bb * rat) >> 5;
                    }
                    rgb = (static_cast<uint32_t>(cr) << 16) |
                          (static_cast<uint32_t>(cg) << 8) |
                          static_cast<uint32_t>(cb);
                }
                if (attr & 0x40u) {
                    const int* ov = prep.off_vals[(attr & 0x80u) ? 1 : 0];
                    const int r = std::clamp(static_cast<int>((rgb >> 16) & 0xFFu) + ov[0], 0, 255);
                    const int g = std::clamp(static_cast<int>((rgb >> 8) & 0xFFu) + ov[1], 0, 255);
                    const int b = std::clamp(static_cast<int>(rgb & 0xFFu) + ov[2], 0, 255);
                    rgb = (static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b);
                }
                out_row[x] = rgb;
            }
        }
    }
}

void StvVdpState::compose_frame(const uint8_t vdp2_vram[0x80000], uint8_t dbg_hide_layers) {
    compose_frame_prepare(vdp2_vram, dbg_hide_layers);
    if (!compose_prep.disp_on) return;
    compose_frame_band(vdp2_vram, 0, frame_h);
    compose_frame_post();
}

void StvVdpState::compose_frame_post() {
    const int x_shift = compose_prep.x_shift;
    const int y_step = compose_prep.y_step;

    if (wind_vfx.active && frame_w > 0 && frame_h > 0) {
        auto blend_px = [&](int px, int py, float a, uint8_t cr, uint8_t cg, uint8_t cb) {
            if (px < 0 || py < 0 || px >= frame_w || py >= frame_h || a <= 0.004f) return;
            const float ca = std::min(0.88f, a);
            const float ia = 1.0f - ca;
            uint32_t& dst = frame_px[py * frame_w + px];
            const int dr = static_cast<int>((dst >> 16) & 0xFFu);
            const int dg = static_cast<int>((dst >> 8) & 0xFFu);
            const int db = static_cast<int>(dst & 0xFFu);
            const int nr = std::clamp(static_cast<int>(dr * ia + cr * ca + 0.5f), 0, 255);
            const int ng = std::clamp(static_cast<int>(dg * ia + cg * ca + 0.5f), 0, 255);
            const int nb = std::clamp(static_cast<int>(db * ia + cb * ca + 0.5f), 0, 255);
            dst = (static_cast<uint32_t>(nr) << 16) | (static_cast<uint32_t>(ng) << 8) | static_cast<uint32_t>(nb);
        };

        const float bx = wind_vfx.cx;
        const float by = wind_vfx.cy;
        const float rad = std::max(10.0f, wind_vfx.radius);
        const float phase = static_cast<float>(wind_vfx.frame_age) * 0.42f;
        const float fade_life = (wind_vfx.frame_age > 55u)
            ? std::max(0.0f, 1.0f - static_cast<float>(wind_vfx.frame_age - 55u) / 18.0f)
            : 1.0f;

        // 1. Tapering translucent wind wake / aerodynamic tunnel behind the ball
        if (wind_vfx.trail_count >= 2) {
            for (int seg = 0; seg < wind_vfx.trail_count - 1; ++seg) {
                const float x0 = wind_vfx.trail_x[seg];
                const float y0 = wind_vfx.trail_y[seg];
                const float x1 = wind_vfx.trail_x[seg + 1];
                const float y1 = wind_vfx.trail_y[seg + 1];
                const float dx = x1 - x0;
                const float dy = y1 - y0;
                const float seg_len = std::sqrt(dx * dx + dy * dy);
                if (seg_len < 0.25f) continue;
                const float nx = -dy / seg_len;
                const float ny = dx / seg_len;
                const int sub_steps = std::max(2, std::min(16, static_cast<int>(seg_len * 1.3f)));
                for (int s = 0; s < sub_steps; ++s) {
                    const float t_local = (static_cast<float>(seg) + static_cast<float>(s) / sub_steps) /
                                          static_cast<float>(wind_vfx.trail_count - 1);
                    const float falloff = (1.0f - t_local);
                    const float px_c = x0 + dx * (static_cast<float>(s) / sub_steps);
                    const float py_c = y0 + dy * (static_cast<float>(s) / sub_steps);
                    const float cone_r = rad * (0.92f - 0.72f * t_local);
                    const int ir = static_cast<int>(std::ceil(cone_r)) + 1;
                    for (int k = -ir; k <= ir; ++k) {
                        const float kf = static_cast<float>(k) / std::max(1.0f, cone_r);
                        const float akf = std::abs(kf);
                        if (akf > 1.0f) continue;
                        // Rim-bright hollow wind wake + swirling ribbon streak
                        const float edge_boost = 0.25f + 0.75f * akf * akf;
                        const float wave = 0.5f + 0.5f * std::sin(t_local * 14.0f - phase * 1.5f + kf * 2.4f);
                        const float alpha = fade_life * falloff * falloff * (0.20f * edge_boost + 0.26f * wave * akf);
                        const int sx = static_cast<int>(std::lround(px_c + nx * static_cast<float>(k)));
                        const int sy = static_cast<int>(std::lround(py_c + ny * static_cast<float>(k)));
                        blend_px(sx, sy, alpha, 232, 242, 255);
                    }
                }
            }
        }

        // 2. Translucent wind vortex sphere surrounding the ball + swirling bright orbital ribbons
        const int r_bound = static_cast<int>(std::ceil(rad * 1.25f));
        const int x_min = std::max(0, static_cast<int>(std::floor(bx - r_bound)));
        const int x_max = std::min(frame_w - 1, static_cast<int>(std::ceil(bx + r_bound)));
        const int y_min = std::max(0, static_cast<int>(std::floor(by - r_bound)));
        const int y_max = std::min(frame_h - 1, static_cast<int>(std::ceil(by + r_bound)));

        // Precompute 3 swirling orbital ribbon basis vectors around the sphere
        float c_ang[3], s_ang[3], ph_off[3];
        for (int i = 0; i < 3; ++i) {
            const float tilt = 0.45f + 0.95f * static_cast<float>(i);
            c_ang[i] = std::cos(tilt);
            s_ang[i] = std::sin(tilt);
            ph_off[i] = phase * (1.15f + 0.28f * i) + static_cast<float>(i) * 2.094f;
        }

        for (int py = y_min; py <= y_max; ++py) {
            const float dy = static_cast<float>(py) - by;
            for (int px = x_min; px <= x_max; ++px) {
                const float dx = static_cast<float>(px) - bx;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float nr = dist / rad;
                if (nr > 1.18f) continue;

                float alpha = 0.0f;
                if (nr <= 1.0f) {
                    // Hollow spherical wind dome: translucent over center, brighter Fresnel rim near nr=0.85..1.0
                    const float rim = std::pow(nr, 2.3f);
                    const float outer_soft = (nr > 0.90f) ? (1.0f - (nr - 0.90f) / 0.14f) : 1.0f;
                    alpha = (0.06f + 0.34f * rim) * std::max(0.0f, outer_soft);
                }

                // Add swirling orbital wind arcs on sphere shell
                const float ux = dx / rad;
                const float uy = dy / rad;
                for (int r_idx = 0; r_idx < 3; ++r_idx) {
                    const float rx = ux * c_ang[r_idx] + uy * s_ang[r_idx];
                    const float ry = -ux * s_ang[r_idx] + uy * c_ang[r_idx];
                    const float ring_r = 0.86f + 0.10f * r_idx;
                    const float ell_y = ry / (0.38f + 0.08f * r_idx);
                    const float ring_d = std::abs(std::sqrt((rx / ring_r) * (rx / ring_r) + (ell_y / ring_r) * (ell_y / ring_r)) - 1.0f);
                    if (ring_d < 0.22f) {
                        const float ang = std::atan2(ell_y, rx) + ph_off[r_idx];
                        const float arc = 0.5f + 0.5f * std::sin(ang * 2.0f);
                        if (arc > 0.28f) {
                            const float streak = (1.0f - ring_d / 0.22f) * arc;
                            alpha += streak * 0.44f;
                        }
                    }
                }
                if (alpha > 0.01f) {
                    blend_px(px, py, alpha * fade_life, 246, 250, 255);
                }
            }
        }

        // 3. Swirling wind & turf dust particles orbiting the wind vortex
        for (int p = 0; p < 14; ++p) {
            const float p_ang = phase * (1.25f + 0.08f * p) + static_cast<float>(p) * 1.618f;
            const float orbit_r = rad * (0.85f + 0.48f * std::sin(phase * 0.5f + p * 0.9f));
            float ox = bx + std::cos(p_ang) * orbit_r;
            float oy = by + std::sin(p_ang) * orbit_r * 0.65f;
            if ((p & 3) == 0 && wind_vfx.trail_count >= 4) {
                const int ti = std::min(wind_vfx.trail_count - 1, (p / 4 + static_cast<int>(wind_vfx.frame_age / 2)) % wind_vfx.trail_count);
                ox = wind_vfx.trail_x[ti] + std::cos(p_ang * 1.5f) * (rad * 0.45f);
                oy = wind_vfx.trail_y[ti] + std::sin(p_ang * 1.5f) * (rad * 0.35f);
            }
            const int ix = static_cast<int>(std::lround(ox));
            const int iy = static_cast<int>(std::lround(oy));
            const bool warm_fleck = (p % 3) == 0;
            const uint8_t cr = warm_fleck ? 184u : 248u;
            const uint8_t cg = warm_fleck ? 148u : 252u;
            const uint8_t cb = warm_fleck ? 108u : 255u;
            blend_px(ix, iy, 0.75f * fade_life, cr, cg, cb);
            if ((p & 1) == 0) blend_px(ix + 1, iy, 0.45f * fade_life, cr, cg, cb);
        }
    }

    // Copa America National VFX replacements (zero gameplay/physics change):
    // 0. BOL SP 3 Pre-shot Wind Inflow (replacing yellow-red sparks 0x009A with converging gale streams toward player/ball)
    if (copa_vfx_replace && wind_vfx.active && (copa_sp_vfx.charge_count > 0u || wind_vfx.frame_age <= 34u) &&
        frame_w > 0 && frame_h > 0) {
        const float sx = 1.0f / static_cast<float>(1 << x_shift);
        const float sy = 1.0f / static_cast<float>(y_step);
        // Compute converging hub from scaled wind_vfx center (or ray inner endpoints)
        float hub_x = wind_vfx.cx * sx;
        float hub_y = wind_vfx.cy * sy;
        if (wind_vfx.cx <= 0.0f && wind_vfx.cy <= 0.0f && copa_sp_vfx.charge_count > 0u) {
            float sx_sum = 0.0f, sy_sum = 0.0f;
            for (int i = 0; i < static_cast<int>(copa_sp_vfx.charge_count); ++i) {
                const auto& r = copa_sp_vfx.charge_rays[i];
                sx_sum += (static_cast<float>(r.x0) + static_cast<float>(r.x1)) * 0.5f * sx;
                sy_sum += (static_cast<float>(r.y0) + static_cast<float>(r.y1)) * 0.5f * sy;
            }
            hub_x = sx_sum / static_cast<float>(copa_sp_vfx.charge_count);
            hub_y = sy_sum / static_cast<float>(copa_sp_vfx.charge_count);
        }

        auto blend_charge = [&](int px, int py, float a, uint8_t r, uint8_t g, uint8_t b) {
            if (px < 0 || py < 0 || px >= frame_w || py >= frame_h || a <= 0.005f) return;
            const float ca = std::min(0.88f, a);
            const float ia = 1.0f - ca;
            uint32_t& dst = frame_px[py * frame_w + px];
            const int dr = static_cast<int>((dst >> 16) & 0xFFu);
            const int dg = static_cast<int>((dst >> 8) & 0xFFu);
            const int db = static_cast<int>(dst & 0xFFu);
            const int nr = std::clamp(static_cast<int>(dr * ia + r * ca + 0.5f), 0, 255);
            const int ng = std::clamp(static_cast<int>(dg * ia + g * ca + 0.5f), 0, 255);
            const int nb = std::clamp(static_cast<int>(db * ia + b * ca + 0.5f), 0, 255);
            dst = (static_cast<uint32_t>(nr) << 16) | (static_cast<uint32_t>(ng) << 8) | static_cast<uint32_t>(nb);
        };

        const float age_f = static_cast<float>(std::max<uint32_t>(copa_sp_vfx.charge_age, wind_vfx.frame_age));
        const float phase = age_f * 0.45f;
        const float pre_fade = (wind_vfx.frame_age > 28u && copa_sp_vfx.charge_count == 0u)
            ? std::max(0.0f, 1.0f - static_cast<float>(wind_vfx.frame_age - 28u) / 7.0f)
            : 1.0f;

        auto draw_wind_gust_ray = [&](float fx, float fy, float nx_pt, float ny_pt, int seed_i) {
            const float vx = nx_pt - fx;
            const float vy = ny_pt - fy;
            const float len = std::sqrt(vx * vx + vy * vy);
            if (len < 3.0f) return;
            const float px_n = -vy / len;
            const float py_n = vx / len;
            const int steps = std::clamp(static_cast<int>(len * 1.45f), 16, 84);
            for (int st_i = 0; st_i <= steps; ++st_i) {
                const float t = static_cast<float>(st_i) / static_cast<float>(steps); // 0=outer -> 1=near player
                const float env = std::sin(t * 3.14159265f);
                const float pulse = 0.62f + 0.38f * std::sin(t * 7.0f - phase * 1.7f + static_cast<float>(seed_i));
                const float wave_lat = std::sin(t * 6.5f - phase * 1.4f + static_cast<float>(seed_i) * 1.3f) *
                                       (6.5f * sx * (1.0f - 0.50f * t));
                const float cx_s = fx + vx * t + px_n * wave_lat;
                const float cy_s = fy + vy * t + py_n * wave_lat;
                const float half_w = (2.0f + 2.8f * t) * sx * (0.45f + 0.55f * env);
                const int hw_i = std::max(1, static_cast<int>(std::ceil(half_w)));
                for (int k = -hw_i; k <= hw_i; ++k) {
                    const float kf = static_cast<float>(k) / std::max(1.0f, half_w);
                    if (std::abs(kf) > 1.0f) continue;
                    const float prof = 1.0f - kf * kf;
                    const float a = pre_fade * env * pulse * prof * (0.45f + 0.45f * t);
                    const uint8_t cr = static_cast<uint8_t>(210 + static_cast<int>(44 * t));
                    const uint8_t cg = static_cast<uint8_t>(238 + static_cast<int>(17 * t));
                    const uint8_t cb = 255u;
                    blend_charge(static_cast<int>(std::lround(cx_s + px_n * static_cast<float>(k))),
                                 static_cast<int>(std::lround(cy_s + py_n * static_cast<float>(k))),
                                 a, cr, cg, cb);
                }
            }
        };

        // 1) Draw wind along any captured VDP1 spark rays whose span is already extended
        const int rcnt = static_cast<int>(copa_sp_vfx.charge_count);
        for (int ri = 0; ri < rcnt; ++ri) {
            const auto& ray = copa_sp_vfx.charge_rays[ri];
            const float ax = static_cast<float>(ray.x0) * sx;
            const float ay = static_cast<float>(ray.y0) * sy;
            const float bx_p = static_cast<float>(ray.x1) * sx;
            const float by_p = static_cast<float>(ray.y1) * sy;
            const float da = (ax - hub_x) * (ax - hub_x) + (ay - hub_y) * (ay - hub_y);
            const float db_d = (bx_p - hub_x) * (bx_p - hub_x) + (by_p - hub_y) * (by_p - hub_y);
            const float fx = (da >= db_d) ? ax : bx_p;
            const float fy = (da >= db_d) ? ay : by_p;
            const float nx_pt = (da >= db_d) ? bx_p : ax;
            const float ny_pt = (da >= db_d) ? by_p : ay;
            if ((fx - nx_pt) * (fx - nx_pt) + (fy - ny_pt) * (fy - ny_pt) >= (18.0f * sx) * (18.0f * sx)) {
                draw_wind_gust_ray(fx, fy, nx_pt, ny_pt, ri);
            }
        }

        // 2) Always draw 11 radial swirling wind streams rushing inward toward the player/ball from outside
        for (int gi = 0; gi < 11; ++gi) {
            const float flow = std::fmod(age_f * 0.085f + static_cast<float>(gi) * 0.273f, 1.0f);
            const float base_ang = static_cast<float>(gi) * (6.2831853f / 11.0f) + std::sin(static_cast<float>(gi) * 1.7f) * 0.18f;
            const float r_far  = (155.0f - 85.0f * flow) * sx;
            const float r_near = std::max(14.0f * sx, r_far - (58.0f + 16.0f * (gi % 3)) * sx);
            const float ang_far  = base_ang - 0.24f * (1.0f - flow);
            const float ang_near = base_ang + 0.10f * flow;
            const float fx = hub_x + std::cos(ang_far) * r_far;
            const float fy = hub_y + std::sin(ang_far) * r_far * 0.82f;
            const float nx_pt = hub_x + std::cos(ang_near) * r_near;
            const float ny_pt = hub_y + std::sin(ang_near) * r_near * 0.82f;
            draw_wind_gust_ray(fx, fy, nx_pt, ny_pt, gi + 16);
        }

        // 3) Contracting concentric inflow gale arcs around the shooter's feet/ball before the kick
        for (int w = 0; w < 3; ++w) {
            const float cyc = std::fmod(age_f * 0.065f + static_cast<float>(w) * 0.333f, 1.0f);
            const float arc_r = (92.0f * (1.0f - cyc) + 12.0f) * sx;
            const float arc_a = pre_fade * std::sin(cyc * 3.14159265f) * 0.46f;
            if (arc_a <= 0.02f) continue;
            const int segs = 36;
            for (int s = 0; s < segs; ++s) {
                const float ang = static_cast<float>(s) * (6.2831853f / segs) + phase * (w & 1 ? -0.45f : 0.45f);
                const float gap = std::sin(ang * 3.0f + phase + static_cast<float>(w) * 2.1f);
                if (gap < 0.05f) continue;
                const int px = static_cast<int>(std::lround(hub_x + std::cos(ang) * arc_r));
                const int py = static_cast<int>(std::lround(hub_y + std::sin(ang) * arc_r * 0.68f));
                blend_charge(px, py, arc_a * gap, 224, 244, 255);
                blend_charge(px + 1, py, arc_a * gap * 0.6f, 200, 232, 255);
            }
        }
    }

    // 1. PER SP 24 (Heel Lift) -> 3D Animated Condor & SP 14/38 (Super Combination) -> 3D Machu Picchu
    auto draw_peru_3d_model = [&](const twc98::vfx::PeruModelVertex* verts, int v_cnt,
                                  const uint16_t* indices, int idx_cnt,
                                  const uint16_t* tex565,
                                  float center_x, float center_y, float half_size,
                                  float yaw, float pitch, float roll,
                                  float flap_amp, float master_a) {
        if (v_cnt <= 0 || v_cnt > 2624 || idx_cnt <= 0 || master_a <= 0.01f) return;
        const float cy = std::cos(yaw),   sy_a = std::sin(yaw);
        const float cp = std::cos(pitch), sp_a = std::sin(pitch);
        const float cr = std::cos(roll),  sr_a = std::sin(roll);

        float min_sx = static_cast<float>(frame_w), max_sx = -1.0f;
        float min_sy = static_cast<float>(frame_h), max_sy = -1.0f;

        for (int vi = 0; vi < v_cnt; ++vi) {
            float x = verts[vi].x;
            float y = verts[vi].y;
            float z = verts[vi].z;
            if (flap_amp != 0.0f) {
                const float ax = std::fabs(x);
                if (ax > 0.14f) {
                    const float t = (ax - 0.14f) / 0.86f;
                    y += flap_amp * (t * t) * 0.42f;
                }
            }
            // Apply roll (banking around Z) -> yaw (around Y) -> pitch (camera tilt around X)
            const float rx = x * cr - y * sr_a;
            const float ry = x * sr_a + y * cr;
            const float x1 = rx * cy + z * sy_a;
            const float z1 = -rx * sy_a + z * cy;
            const float y2 = ry * cp - z1 * sp_a;
            const float z2 = ry * sp_a + z1 * cp;
            const float persp = 2.65f / std::max(0.75f, 2.65f - z2 * 0.48f);
            const float px = center_x + x1 * half_size * persp;
            const float py = center_y - y2 * half_size * persp;
            peru_proj[vi] = {px, py, z2};
            if (px < min_sx) min_sx = px;
            if (px > max_sx) max_sx = px;
            if (py < min_sy) min_sy = py;
            if (py > max_sy) max_sy = py;
        }

        const int bx0 = std::max(0, static_cast<int>(std::floor(min_sx)));
        const int by0 = std::max(0, static_cast<int>(std::floor(min_sy)));
        int bx1 = std::min(frame_w - 1, static_cast<int>(std::ceil(max_sx)));
        int by1 = std::min(frame_h - 1, static_cast<int>(std::ceil(max_sy)));
        if (bx1 < bx0 || by1 < by0) return;
        if (bx1 - bx0 + 1 > kPeruZBufMaxW) bx1 = bx0 + kPeruZBufMaxW - 1;
        if (by1 - by0 + 1 > kPeruZBufMaxH) by1 = by0 + kPeruZBufMaxH - 1;
        const int bw = bx1 - bx0 + 1;
        const int bh = by1 - by0 + 1;
        std::memset(peru_zbuf, 0, static_cast<size_t>(bw * bh) * sizeof(uint16_t));

        const int ia_fp = static_cast<int>(std::lround((1.0f - master_a) * 256.0f));
        const int ca_fp = 256 - ia_fp;

        for (int ti = 0; ti + 2 < idx_cnt; ti += 3) {
            const uint16_t i0 = indices[ti];
            const uint16_t i1 = indices[ti + 1];
            const uint16_t i2 = indices[ti + 2];
            const auto& a = peru_proj[i0];
            const auto& b = peru_proj[i1];
            const auto& c = peru_proj[i2];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 0.05f) continue;

            const int tx0 = std::max(bx0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
            const int tx1 = std::min(bx1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
            const int ty0 = std::max(by0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
            const int ty1 = std::min(by1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
            if (tx0 > tx1 || ty0 > ty1) continue;

            const float inv_area = 1.0f / area;
            const float u0 = verts[i0].u, v0 = verts[i0].v;
            const float u1 = verts[i1].u, v1 = verts[i1].v;
            const float u2 = verts[i2].u, v2 = verts[i2].v;

            for (int py = ty0; py <= ty1; ++py) {
                const float pyc = static_cast<float>(py) + 0.5f;
                const int zrow = (py - by0) * bw - bx0;
                const int frow = py * frame_w;
                for (int px = tx0; px <= tx1; ++px) {
                    const float pxc = static_cast<float>(px) + 0.5f;
                    const float w0 = ((c.x - b.x) * (pyc - b.y) - (c.y - b.y) * (pxc - b.x)) * inv_area;
                    const float w1 = ((a.x - c.x) * (pyc - c.y) - (a.y - c.y) * (pxc - c.x)) * inv_area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < -0.002f || w1 < -0.002f || w2 < -0.002f) continue;

                    const float zz = w0 * a.z + w1 * b.z + w2 * c.z;
                    const int zi = std::clamp(static_cast<int>((zz + 2.0f) * 16000.0f), 1, 65535);
                    const uint16_t zq = static_cast<uint16_t>(zi);
                    uint16_t& zb = peru_zbuf[zrow + px];
                    if (zq <= zb) continue;
                    zb = zq;

                    const float uu = std::clamp(w0 * u0 + w1 * u1 + w2 * u2, 0.0f, 1.0f);
                    const float vv = std::clamp(w0 * v0 + w1 * v1 + w2 * v2, 0.0f, 1.0f);
                    const int tex_x = static_cast<int>(uu * 255.0f + 0.5f) & 0xFF;
                    const int tex_y = static_cast<int>(vv * 255.0f + 0.5f) & 0xFF;
                    const uint16_t c565 = tex565[(tex_y << 8) | tex_x];
                    const int sr = ((c565 >> 11) & 0x1Fu) * 255 / 31;
                    const int sg = ((c565 >> 5) & 0x3Fu) * 255 / 63;
                    const int sb = (c565 & 0x1Fu) * 255 / 31;
                    if (ia_fp <= 0) {
                        frame_px[frow + px] = (static_cast<uint32_t>(sr) << 16) |
                                              (static_cast<uint32_t>(sg) << 8) |
                                              static_cast<uint32_t>(sb);
                    } else {
                        uint32_t& dst = frame_px[frow + px];
                        const int dr = static_cast<int>((dst >> 16) & 0xFFu);
                        const int dg = static_cast<int>((dst >> 8) & 0xFFu);
                        const int db = static_cast<int>(dst & 0xFFu);
                        const int nr = (dr * ia_fp + sr * ca_fp) >> 8;
                        const int ng = (dg * ia_fp + sg * ca_fp) >> 8;
                        const int nb = (db * ia_fp + sb * ca_fp) >> 8;
                        dst = (static_cast<uint32_t>(nr) << 16) |
                              (static_cast<uint32_t>(ng) << 8) |
                              static_cast<uint32_t>(nb);
                    }
                }
            }
        }
    };

    if (copa_vfx_replace && copa_sp_vfx.machu.active && frame_w > 0 && frame_h > 0) {
        const auto& mb = copa_sp_vfx.machu;
        const float sx = 1.0f / static_cast<float>(1 << x_shift);
        const float sy = 1.0f / static_cast<float>(y_step);
        const float raw_cx = (static_cast<float>(mb.min_x) + static_cast<float>(mb.max_x)) * 0.5f * sx;
        const float raw_cy = (static_cast<float>(mb.min_y) + static_cast<float>(mb.max_y)) * 0.5f * sy;
        const float box_h = std::max(0.0f, static_cast<float>(mb.max_y - mb.min_y) * sy);
        const float age_f = static_cast<float>(mb.anim_age);
        const float rise = std::min(1.0f, (age_f + 1.0f) / 3.0f);
        const float half_sz = std::clamp(std::max(box_h * 0.72f, 126.0f * sy), 84.0f, 158.0f);
        const float dir = mb.hflip ? -1.0f : 1.0f;
        // Rotate gently at most ~half turn total across the whole invocation, centered around the hero front view (yaw ~0.52 rad)
        const float rot_t = std::min(1.0f, age_f / 110.0f);
        const float yaw = 0.52f + dir * (rot_t - 0.5f) * 1.45f;
        const float y_off = (1.0f - rise) * (12.0f * sy);
        const float clamp_cx = std::clamp(raw_cx, half_sz * 0.78f + 6.0f,
                                          static_cast<float>(frame_w) - half_sz * 0.78f - 6.0f);
        draw_peru_3d_model(twc98::vfx::kMachu3dVerts, twc98::vfx::kMachu3dVertCount,
                           twc98::vfx::kMachu3dIndices, twc98::vfx::kMachu3dIndexCount,
                           twc98::vfx::kMachu3dTex565,
                           clamp_cx, raw_cy + y_off, half_sz,
                           yaw, 0.40f, 0.0f, 0.0f, 0.72f + 0.28f * rise);
    }

    if (copa_vfx_replace && copa_sp_vfx.condor.active && frame_w > 0 && frame_h > 0) {
        const auto& cb = copa_sp_vfx.condor;
        const float sx = 1.0f / static_cast<float>(1 << x_shift);
        const float sy = 1.0f / static_cast<float>(y_step);
        const float raw_cx = (static_cast<float>(cb.min_x) + static_cast<float>(cb.max_x)) * 0.5f * sx;
        const float raw_cy = (static_cast<float>(cb.min_y) + static_cast<float>(cb.max_y)) * 0.5f * sy;
        const float box_h = std::max(0.0f, static_cast<float>(cb.max_y - cb.min_y) * sy);
        const float age_f = static_cast<float>(cb.anim_age);
        const float rise = std::min(1.0f, (age_f + 1.0f) / 3.0f);
        const float half_sz = std::clamp(std::max(box_h * 0.65f, 82.0f * sy), 52.0f, 130.0f);
        const float dir = (copa_sp_vfx.condor_dir < 0) ? -1.0f : 1.0f;
        // Face forward in the direction the invoking player advances (+X right / -X left) with only a subtle swaying turn
        const float yaw = dir * 1.38f + std::sin(age_f * 0.09f) * 0.16f;
        const float bank = dir * std::sin(age_f * 0.10f) * 0.14f;
        const float flap = std::sin(age_f * 0.36f) * 0.80f;
        const float swoop_y = -24.0f * sy + std::sin(age_f * 0.14f) * (5.0f * sy);
        draw_peru_3d_model(twc98::vfx::kCondorVerts, twc98::vfx::kCondorVertCount,
                           twc98::vfx::kCondorIndices, twc98::vfx::kCondorIndexCount,
                           twc98::vfx::kCondorTex565,
                           raw_cx + dir * (6.0f * sx), raw_cy + swoop_y, half_sz,
                           yaw, 0.42f, bank, flap, 0.72f + 0.28f * rise);
    }

    // 2. URU SP 20: Stylized Uruguayan Flag replacing inherited German tricolor
    if (copa_vfx_replace && copa_sp_vfx.uru_flag.active && frame_w > 0 && frame_h > 0) {
        const auto& ub = copa_sp_vfx.uru_flag;
        const float sx = 1.0f / static_cast<float>(1 << x_shift);
        const float sy = 1.0f / static_cast<float>(y_step);
        const float cx = (static_cast<float>(ub.min_x) + static_cast<float>(ub.max_x)) * 0.5f * sx;
        const float cy = (static_cast<float>(ub.min_y) + static_cast<float>(ub.max_y)) * 0.5f * sy;
        const float fw = std::clamp(std::max((static_cast<float>(ub.max_x - ub.min_x)) * sx, 46.0f * sx), 30.0f, 96.0f);
        const float fh = std::clamp(std::max((static_cast<float>(ub.max_y - ub.min_y)) * sy, 30.0f * sy), 20.0f, 68.0f);
        const float phase = static_cast<float>(ub.anim_age) * 0.36f;
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - fw * 0.5f)));
        const int x1 = std::min(frame_w - 1, static_cast<int>(std::ceil(cx + fw * 0.5f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - fh * 0.5f - 3.0f)));
        const int y1 = std::min(frame_h - 1, static_cast<int>(std::ceil(cy + fh * 0.5f + 3.0f)));

        for (int px = x0; px <= x1; ++px) {
            const float u = (static_cast<float>(px) - (cx - fw * 0.5f)) / fw;
            if (u < 0.0f || u > 1.0f) continue;
            const float wave = std::sin(u * 7.2f - phase) * 2.0f * sy;
            const float fy0 = cy - fh * 0.5f + wave;
            for (int py = y0; py <= y1; ++py) {
                const float v = (static_cast<float>(py) - fy0) / fh;
                if (v < 0.0f || v >= 1.0f) continue;
                const int stripe = static_cast<int>(v * 9.0f);
                uint8_t cr = 244, cg = 248, cb = 252;
                if (u < 0.42f && v < 5.0f / 9.0f) {
                    // White canton + Golden Sol de Mayo
                    const float sx_n = (u - 0.21f) / 0.14f;
                    const float sy_n = (v - (2.5f / 9.0f)) / 0.18f;
                    const float sr2 = sx_n * sx_n + sy_n * sy_n;
                    if (sr2 <= 0.34f) {
                        cr = 246; cg = 196; cb = 28;
                    } else if (sr2 <= 0.92f) {
                        const float ang = std::atan2(sy_n, sx_n);
                        if (std::abs(std::sin(ang * 4.0f)) > 0.72f) {
                            cr = 246; cg = 196; cb = 28;
                        }
                    }
                } else if ((stripe & 1) == 1) {
                    // 4 Celeste horizontal stripes
                    cr = 56; cg = 152; cb = 232;
                }
                if (u < 0.03f || u > 0.97f || v < 0.03f || v > 0.97f) {
                    cr = static_cast<uint8_t>(cr * 0.65f);
                    cg = static_cast<uint8_t>(cg * 0.65f);
                    cb = static_cast<uint8_t>(cb * 0.65f);
                }
                frame_px[py * frame_w + px] = (static_cast<uint32_t>(cr) << 16) |
                                              (static_cast<uint32_t>(cg) << 8) |
                                              static_cast<uint32_t>(cb);
            }
        }
    }

    // Copa America Team Select 3x4 Group Labels ("GRP A / GRP B / GRP C") aligned right of each group row.
    if (copa_group_badges && frame_w >= 320 && frame_h >= 224) {
        static constexpr uint8_t kGlyphs[6][7] = {
            {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, // 0: G
            {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // 1: R
            {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // 2: P
            {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // 3: A
            {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, // 4: B
            {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // 5: C
        };
        const float sx = static_cast<float>(frame_w) / 704.0f;
        const float sy = static_cast<float>(frame_h) / 448.0f;
        static constexpr int kRowBaseY[3] = {291, 331, 371};
        const int bx0 = static_cast<int>(582.0f * sx);
        const int bw  = std::max(44, static_cast<int>(86.0f * sx));
        const int bh  = std::max(14, static_cast<int>(26.0f * sy));
        const int gx_scale = std::max(1, static_cast<int>(2.0f * sx + 0.5f));
        const int gy_scale = std::max(1, static_cast<int>(2.0f * sy + 0.5f));

        for (int g = 0; g < 3; ++g) {
            const int by0 = static_cast<int>(static_cast<float>(kRowBaseY[g]) * sy);
            for (int py = 0; py < bh; ++py) {
                const int iy = by0 + py;
                if (iy < 0 || iy >= frame_h) continue;
                for (int px = 0; px < bw; ++px) {
                    const int ix = bx0 + px;
                    if (ix < 0 || ix >= frame_w) continue;
                    const bool border = (px <= 1 || px >= bw - 2 || py <= 1 || py >= bh - 2);
                    uint32_t col = 0x00101828u;
                    if (border) {
                        col = 0x00E8B828u;
                    } else {
                        const int t = 24 + (bh - py) * 34 / std::max(1, bh);
                        col = (static_cast<uint32_t>(t / 2) << 16) |
                              (static_cast<uint32_t>(t) << 8) |
                              static_cast<uint32_t>(std::min(255, t + 32));
                    }
                    frame_px[iy * frame_w + ix] = col;
                }
            }

            // Draw "GRP A" / "GRP B" / "GRP C" in beveled gold-to-white font
            const int ch_ids[5] = {0, 1, 2, -1, 3 + g};
            const int text_w = 4 * (5 * gx_scale) + 3 * (2 * gx_scale) + (4 * gx_scale);
            const int tx0 = bx0 + (bw - text_w) / 2;
            const int ty0 = by0 + (bh - 7 * gy_scale) / 2;
            int cx = tx0;
            for (int ci = 0; ci < 5; ++ci) {
                if (ch_ids[ci] < 0) {
                    cx += 4 * gx_scale;
                    continue;
                }
                const uint8_t* rows = kGlyphs[ch_ids[ci]];
                const uint32_t fg = (ci == 4) ? 0x00FFEA70u : 0x00F4F6FAu;
                for (int r = 0; r < 7; ++r) {
                    for (int c = 0; c < 5; ++c) {
                        if ((rows[r] & (1u << (4 - c))) == 0u) continue;
                        for (int dy = 0; dy < gy_scale; ++dy) {
                            const int py = ty0 + r * gy_scale + dy;
                            if (py < 0 || py >= frame_h) continue;
                            for (int dx = 0; dx < gx_scale; ++dx) {
                                const int px = cx + c * gx_scale + dx;
                                if (px < 0 || px >= frame_w) continue;
                                if (px + 1 < frame_w && py + 1 < frame_h) {
                                    frame_px[(py + 1) * frame_w + (px + 1)] = 0x00080C14u;
                                }
                                frame_px[py * frame_w + px] = fg;
                            }
                        }
                    }
                }
                cx += 7 * gx_scale;
            }
        }
    }

    // B-H5-09: Copa America Visual Group 3x4 & Knockout Progress Screen overlay
    if (copa_bracket.progress_active != 0u && frame_w >= 320 && frame_h >= 224) {
        struct Glyph5x7 {
            char ch;
            uint8_t rows[7];
        };
        static constexpr Glyph5x7 kProgFont[] = {
            {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
            {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
            {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
            {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
            {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
            {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
            {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
            {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
            {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
            {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
            {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
            {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
            {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
            {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
            {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
            {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
            {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
            {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
            {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
            {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
            {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
            {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
            {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
            {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
            {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
            {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
            {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
            {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
            {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
            {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
            {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
            {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
            {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
            {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
            {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
            {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}},
            {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
            {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
            {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
            {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04}},
            {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
            {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
        };
        auto find_pfont = [&](char ch) -> const uint8_t* {
            for (const auto& g : kProgFont) {
                if (g.ch == ch) return g.rows;
            }
            return nullptr;
        };
        auto team_code = [&](uint8_t s) -> const char* {
            static constexpr const char* kCodes[17] = {
                "BRA", "PER", "---", "ARG", "---", "CHI", "ECU", "VEN",
                "---", "---", "MEX", "URU", "COL", "PAR", "BOL", "USA", "ALL"
            };
            return (s < 17u) ? kCodes[s] : "TBD";
        };
        auto sx = [&](int x704) -> int { return (x704 * frame_w) / 704; };
        auto sy = [&](int y448) -> int { return (y448 * frame_h) / 448; };
        const int gx_sc = std::max(1, frame_w / 352);
        const int gy_sc = std::max(1, frame_h / 224);

        auto fill_rect = [&](int x0, int y0, int x1, int y1, uint32_t col) {
            const int ix0 = std::max(0, sx(x0));
            const int iy0 = std::max(0, sy(y0));
            const int ix1 = std::min(frame_w, sx(x1));
            const int iy1 = std::min(frame_h, sy(y1));
            for (int y = iy0; y < iy1; ++y) {
                uint32_t* row = frame_px + static_cast<size_t>(y) * static_cast<size_t>(frame_w);
                for (int x = ix0; x < ix1; ++x) row[x] = col;
            }
        };
        auto draw_box = [&](int x0, int y0, int x1, int y1, uint32_t bg, uint32_t border) {
            fill_rect(x0, y0, x1, y1, border);
            fill_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, bg);
        };
        auto draw_text = [&](int x704, int y448, const char* txt, uint32_t col, int scx, int scy) {
            int cx = sx(x704);
            const int ty = sy(y448);
            for (int i = 0; txt[i] != '\0'; ++i) {
                if (txt[i] == ' ') {
                    cx += 4 * scx;
                    continue;
                }
                const uint8_t* rows = find_pfont(txt[i]);
                if (rows) {
                    for (int r = 0; r < 7; ++r) {
                        for (int c = 0; c < 5; ++c) {
                            if ((rows[r] & (1u << (4 - c))) == 0u) continue;
                            for (int dy = 0; dy < scy; ++dy) {
                                const int py = ty + r * scy + dy;
                                if (py < 0 || py >= frame_h) continue;
                                for (int dx = 0; dx < scx; ++dx) {
                                    const int px = cx + c * scx + dx;
                                    if (px < 0 || px >= frame_w) continue;
                                    if (px + 1 < frame_w && py + 1 < frame_h) {
                                        frame_px[(py + 1) * frame_w + (px + 1)] = 0x00060A12u;
                                    }
                                    frame_px[py * frame_w + px] = col;
                                }
                            }
                        }
                    }
                }
                cx += 6 * scx;
            }
        };
        auto draw_mini_flag = [&](int x704, int y448, uint8_t slot) {
            if (slot > 16u) {
                fill_rect(x704, y448, x704 + 18, y448 + 12, 0x00243048u);
                return;
            }
            const int fx0 = sx(x704);
            const int fy0 = sy(y448);
            const int fw = std::max(9, sx(x704 + 18) - fx0);
            const int fh = std::max(6, sy(y448 + 12) - fy0);
            for (int dy = 0; dy < fh; ++dy) {
                const int iy = fy0 + dy;
                if (iy < 0 || iy >= frame_h) continue;
                for (int dx = 0; dx < fw; ++dx) {
                    const int ix = fx0 + dx;
                    if (ix < 0 || ix >= frame_w) continue;
                    if (dx == 0 || dx == fw - 1 || dy == 0 || dy == fh - 1) {
                        frame_px[iy * frame_w + ix] = 0x00101828u;
                        continue;
                    }
                    uint32_t c = 0x00304060u;
                    const int u = (dx * 16) / fw;
                    const int v = (dy * 10) / fh;
                    switch (slot) {
                        case 0u: { // BRA: green + yellow diamond + blue circle
                            const int ax = (u > 8) ? (u - 8) : (8 - u);
                            const int ay = (v > 5) ? (v - 5) : (5 - v);
                            c = (ax + ay * 2 <= 6) ? ((ax + ay <= 3) ? 0x001D4ED8u : 0x00FACC15u) : 0x0015803Du;
                            break;
                        }
                        case 1u: // PER: Red | White | Red
                            c = (u < 5 || u >= 11) ? 0x00DC2626u : 0x00F8FAFCu;
                            break;
                        case 3u: // ARG: Sky / White(+Sun) / Sky
                            c = (v < 3 || v >= 7) ? 0x0038BDF8u : ((v >= 4 && v <= 5 && u >= 7 && u <= 8) ? 0x00F59E0Bu : 0x00F8FAFCu);
                            break;
                        case 5u: // CHI: Blue canton + White / Red
                            c = (v < 5) ? ((u < 6) ? 0x001D4ED8u : 0x00F8FAFCu) : 0x00DC2626u;
                            break;
                        case 6u: // ECU: Gold(top) / Blue / Red
                            c = (v < 5) ? 0x00FACC15u : ((v < 7) ? 0x001D4ED8u : 0x00DC2626u);
                            break;
                        case 7u: // VEN: Gold / Blue / Red
                            c = (v < 3) ? 0x00FACC15u : ((v < 7) ? 0x001D4ED8u : 0x00B91C1Cu);
                            break;
                        case 10u: // MEX: Green | White | Red
                            c = (u < 5) ? 0x0015803Du : ((u < 11) ? 0x00F8FAFCu : 0x00DC2626u);
                            break;
                        case 11u: // URU: Gold canton + White/Blue stripes
                            c = (u < 6 && v < 5) ? 0x00FACC15u : (((v % 2) == 1) ? 0x002563EBu : 0x00F8FAFCu);
                            break;
                        case 12u: // COL: Gold(top) / Blue / Red
                            c = (v < 5) ? 0x00FACC15u : ((v < 7) ? 0x001E40AFu : 0x00DC2626u);
                            break;
                        case 13u: // PAR: Red / White(+star) / Blue
                            c = (v < 3) ? 0x00DC2626u : ((v < 7) ? 0x00F8FAFCu : 0x001D4ED8u);
                            break;
                        case 14u: // BOL: Red / Yellow / Dark Green
                            c = (v < 3) ? 0x00DC2626u : ((v < 7) ? 0x00FACC15u : 0x0014532Du);
                            break;
                        case 15u: // USA: Blue canton + Red/White stripes
                            c = (u < 6 && v < 5) ? 0x001E3A8Au : (((v % 2) == 0) ? 0x00DC2626u : 0x00F8FAFCu);
                            break;
                        case 16u: // ALL-STARS: Purple + Gold Star
                            c = ((u >= 6 && u <= 9 && v >= 3 && v <= 6)) ? 0x00FACC15u : 0x007E22CEu;
                            break;
                        default:
                            break;
                    }
                    frame_px[iy * frame_w + ix] = c;
                }
            }
        };
        auto fmt_signed = [&](int v, char out[6]) {
            if (v > 0) { out[0] = '+'; out[1] = static_cast<char>('0' + std::min(v, 9)); out[2] = '\0'; if (v >= 10) { out[1] = static_cast<char>('0' + v / 10); out[2] = static_cast<char>('0' + v % 10); out[3] = '\0'; } }
            else if (v < 0) { const int av = -v; out[0] = '-'; if (av >= 10) { out[1] = static_cast<char>('0' + av / 10); out[2] = static_cast<char>('0' + av % 10); out[3] = '\0'; } else { out[1] = static_cast<char>('0' + av); out[2] = '\0'; } }
            else { out[0] = '0'; out[1] = '\0'; }
        };
        auto fmt_u2 = [&](uint8_t v, char out[4]) {
            if (v >= 10u) { out[0] = static_cast<char>('0' + (v / 10u) % 10u); out[1] = static_cast<char>('0' + (v % 10u)); out[2] = '\0'; }
            else { out[0] = static_cast<char>('0' + v); out[1] = '\0'; }
        };

        // 1. Full Backdrop Gradient
        for (int y = 0; y < frame_h; ++y) {
            const int v = (y * 100) / std::max(1, frame_h);
            const uint32_t bg_r = static_cast<uint32_t>(8 + (v * 6) / 100);
            const uint32_t bg_g = static_cast<uint32_t>(16 + (v * 14) / 100);
            const uint32_t bg_b = static_cast<uint32_t>(36 + (v * 18) / 100);
            const uint32_t col = (bg_r << 16) | (bg_g << 8) | bg_b;
            uint32_t* row = frame_px + static_cast<size_t>(y) * static_cast<size_t>(frame_w);
            for (int x = 0; x < frame_w; ++x) row[x] = col;
        }

        // 2. Top Championship Header
        draw_box(12, 10, 692, 46, 0x00102246u, 0x00EAB308u);
        draw_text(24, 21, "COPA AMERICA 98 - AVANCE DEL TORNEO", 0x00FACC15u, gx_sc, gy_sc);
        static constexpr const char* kStageTitles[7] = {
            "GRUPOS: J1", "GRUPOS: J2", "GRUPOS: J3", "CUARTOS FINAL", "SEMIFINALES", "GRAN FINAL", "7A: ALL-STAR"
        };
        const uint8_t st = (copa_bracket.progress_stage < 7u) ? copa_bracket.progress_stage : 6u;
        draw_box(506, 15, 684, 41, 0x001D4ED8u, 0x00FACC15u);
        draw_text(516, 22, kStageTitles[st], 0x00FFFFFFu, gx_sc, gy_sc);

        // 3. Three Group Cards (A, B, C) [y=52..212]
        for (int g = 0; g < 3; ++g) {
            const int gx0 = 14 + g * 228;
            const int gx1 = gx0 + 220;
            const bool is_p_grp = (copa_bracket.player_group == static_cast<uint8_t>(g));
            draw_box(gx0, 52, gx1, 212, 0x000D1B36u, is_p_grp ? 0x0038BDF8u : 0x003B568Cu);
            fill_rect(gx0 + 2, 54, gx1 - 2, 78, 0x00162E5Au);
            char ghdr[8] = {'G', 'R', 'U', 'P', 'O', ' ', static_cast<char>('A' + g), '\0'};
            draw_text(gx0 + 10, 60, ghdr, 0x00FACC15u, gx_sc, gy_sc);
            draw_text(gx0 + 108, 60, "PJ DG PTS", 0x0094A3B8u, gx_sc, gy_sc);

            for (int r = 0; r < 4; ++r) {
                const auto& row = copa_bracket.standings[g][r];
                const int ry0 = 82 + r * 31;
                const int ry1 = ry0 + 27;
                const bool is_p1 = (row.team == copa_bracket.player_team);
                const uint8_t qmask = (row.team < 16u) ? copa_bracket.qual_mask[row.team] : 0u;
                const bool top_q = (row.played > 0u) && ((qmask & 3u) != 0u || r < 2);
                const uint32_t rbg = is_p1 ? 0x00133D4Au : (top_q ? 0x00122645u : 0x000E1B34u);
                fill_rect(gx0 + 6, ry0, gx1 - 6, ry1, rbg);
                if (is_p1) {
                    fill_rect(gx0 + 6, ry0, gx0 + 10, ry1, 0x0022D3EEu);
                } else if (top_q) {
                    fill_rect(gx0 + 6, ry0, gx0 + 9, ry1, (r < 2) ? 0x0022C55Eu : 0x00EAB308u);
                }
                draw_mini_flag(gx0 + 14, ry0 + 7, row.team);
                const uint32_t tcol = is_p1 ? 0x0067E8F9u : 0x00F1F5F9u;
                draw_text(gx0 + 40, ry0 + 7, team_code(row.team), tcol, gx_sc, gy_sc);
                char pj_s[4]{}, dg_s[6]{}, pts_s[4]{};
                fmt_u2(row.played, pj_s);
                fmt_signed(static_cast<int>(row.gf) - static_cast<int>(row.ga), dg_s);
                fmt_u2(row.pts, pts_s);
                draw_text(gx0 + 108, ry0 + 7, pj_s, 0x00CBD5E1u, gx_sc, gy_sc);
                draw_text(gx0 + 140, ry0 + 7, dg_s, (row.gf >= row.ga) ? 0x0086EFACu : 0x00FCA5A5u, gx_sc, gy_sc);
                draw_text(gx0 + 188, ry0 + 7, pts_s, 0x00FACC15u, gx_sc, gy_sc);
            }
        }

        // 4. Knockout Bracket Tree [y=218..404]: CUARTOS (4) -> SEMIS (2) -> FINAL (1) -> ALL-STAR (1)
        draw_box(14, 218, 690, 404, 0x000B162Eu, 0x00334C7Cu);
        draw_text(28, 224, "CUARTOS DE FINAL", 0x0094A3B8u, gx_sc, gy_sc);
        draw_text(226, 224, "SEMIFINALES", 0x0094A3B8u, gx_sc, gy_sc);
        draw_text(412, 224, "GRAN FINAL", 0x00FACC15u, gx_sc, gy_sc);
        draw_text(566, 224, "7A ALL-STAR", 0x00E879F9u, gx_sc, gy_sc);

        auto draw_match_card = [&](int bx0, int by0, int bw, int bh, const CopaMatchSlot& ms, uint32_t border_norm) {
            const bool involves_p1 = (ms.team_a == copa_bracket.player_team || ms.team_b == copa_bracket.player_team);
            draw_box(bx0, by0, bx0 + bw, by0 + bh, involves_p1 ? 0x0013344Au : 0x00112244u,
                     involves_p1 ? 0x0022D3EEu : border_norm);
            auto draw_half = [&](int ry, uint8_t team, uint8_t g, bool won, bool tbd) {
                draw_mini_flag(bx0 + 6, ry + 1, team);
                const char* cd = (team < 17u) ? team_code(team) : "TBD";
                const uint32_t col = tbd ? 0x0064748Bu : ((team == copa_bracket.player_team) ? 0x0067E8F9u : (won ? 0x00FACC15u : 0x00E2E8F0u));
                draw_text(bx0 + 30, ry, cd, col, gx_sc, gy_sc);
                if (ms.played != 0u) {
                    char gs[4]{};
                    fmt_u2(g, gs);
                    draw_text(bx0 + bw - 26, ry, gs, won ? 0x00FACC15u : 0x0094A3B8u, gx_sc, gy_sc);
                } else {
                    draw_text(bx0 + bw - 26, ry, "-", 0x00475569u, gx_sc, gy_sc);
                }
            };
            draw_half(by0 + 5, ms.team_a, ms.goals_a, ms.played && ms.winner == ms.team_a, ms.team_a >= 17u);
            draw_half(by0 + 21, ms.team_b, ms.goals_b, ms.played && ms.winner == ms.team_b, ms.team_b >= 17u);
        };

        // Connectors (QF->SF->Final->AllStar)
        for (int s = 0; s < 2; ++s) {
            const int qy0 = 260 + s * 80;
            const int qy1 = qy0 + 40;
            const int syc = (qy0 + qy1) / 2;
            fill_rect(196, qy0, 210, qy0 + 2, 0x004B6A9Bu);
            fill_rect(196, qy1, 210, qy1 + 2, 0x004B6A9Bu);
            fill_rect(208, qy0, 210, qy1 + 2, 0x004B6A9Bu);
            fill_rect(208, syc, 224, syc + 2, 0x004B6A9Bu);
        }
        fill_rect(384, 280, 396, 282, 0x004B6A9Bu);
        fill_rect(384, 360, 396, 362, 0x004B6A9Bu);
        fill_rect(394, 280, 396, 362, 0x004B6A9Bu);
        fill_rect(394, 320, 408, 322, 0x00FACC15u);
        fill_rect(552, 320, 570, 322, 0x00E879F9u);

        for (int q = 0; q < 4; ++q) {
            draw_match_card(24, 240 + q * 40, 172, 39, copa_bracket.qf[q], 0x004B6A9Bu);
        }
        for (int s = 0; s < 2; ++s) {
            draw_match_card(224, 260 + s * 80, 160, 40, copa_bracket.sf[s], 0x004B6A9Bu);
        }
        draw_match_card(408, 300, 144, 42, copa_bracket.final_match, 0x00EAB308u);
        draw_match_card(570, 300, 110, 42, copa_bracket.all_star, 0x00C084FCu);

        // 5. Footer Next Match & Continue Prompt [y=408..438]
        draw_box(14, 408, 690, 438, 0x000F2347u, 0x00EAB308u);
        if (copa_bracket.player_eliminated != 0u) {
            draw_text(28, 417, "ELIMINADO EN FASE DE GRUPOS", 0x00F87171u, gx_sc, gy_sc);
        } else {
            draw_text(28, 417, "PROXIMO:", 0x0094A3B8u, gx_sc, gy_sc);
            draw_mini_flag(130, 417, copa_bracket.player_team);
            draw_text(154, 417, team_code(copa_bracket.player_team), 0x0067E8F9u, gx_sc, gy_sc);
            draw_text(198, 417, "VS", 0x00FACC15u, gx_sc, gy_sc);
            draw_mini_flag(230, 417, copa_bracket.next_opp);
            draw_text(254, 417, team_code(copa_bracket.next_opp), 0x00FFFFFFu, gx_sc, gy_sc);
        }
        const bool blink = ((copa_bracket.progress_frames / 18u) & 1u) == 0u;
        draw_text(330, 417, "PULSA START O A PARA CONTINUAR", blink ? 0x00FACC15u : 0x00F8FAFCu, gx_sc, gy_sc);
    }
}

}  // namespace twc98
