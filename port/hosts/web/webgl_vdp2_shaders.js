// TWC98BO Modular WebGL2 VDP2 GLSL ES 3.00 Shader Builder.
// Splits lightweight mobile-friendly ('lite') and lazy 'ultra' (Ultra HD 2x + RGSS + De-Mesh + Scale2x)
// fragment shaders, eliminates dynamic uniform-array indexing via a 64x1 RGBA32I header texture,
// and supports both R16UI and portable RGBA8 packed VDP1 FBO inputs.

(() => {
  const VS_VDP2 = `#version 300 es
    const vec2 POS[4] = vec2[4](
      vec2(-1.0, -1.0),
      vec2( 1.0, -1.0),
      vec2(-1.0,  1.0),
      vec2( 1.0,  1.0)
    );
    void main() {
      gl_Position = vec4(POS[gl_VertexID], 0.0, 1.0);
    }
  `;

  function buildVdp1SamplerBlock(vdp1Rgba8) {
    if (vdp1Rgba8) {
      return `
    uniform sampler2D uVdp1Fb;          // fb_w x fb_h RGBA8 packed (R=lo, G=hi)
    uint fetch_vdp1_raw(int x, int y) {
      vec2 rg = texelFetch(uVdp1Fb, ivec2(x, y), 0).rg;
      uint lo = uint(rg.r * 255.0 + 0.5);
      uint hi = uint(rg.g * 255.0 + 0.5);
      return lo | (hi << 8u);
    }
      `;
    }
    return `
    uniform highp usampler2D uVdp1Fb;   // fb_w_packed x fb_rows R16UI
    uint fetch_vdp1_raw(int x, int y) {
      return texelFetch(uVdp1Fb, ivec2(x, y), 0).r;
    }
    `;
  }

  const ULTRA_HELPERS_GLSL = `
    bool eval_rbg_row_params(int ry, int r_mode, out int sel_out, out bool skip_out,
                             out ivec4 rpa_out, out ivec4 rpb_out, out int kx_out, out int ky_out, out uint xp_out) {
      ivec4 r0a = texelFetch(uLineTable, ivec2(1, ry), 0);
      ivec4 r0b = texelFetch(uLineTable, ivec2(2, ry), 0);
      ivec4 r1a = texelFetch(uLineTable, ivec2(3, ry), 0);
      ivec4 r1b = texelFetch(uLineTable, ivec2(4, ry), 0);
      int sel = (r_mode == 1) ? 1 : 0;
      uint coef_val = 0u;
      if (r_mode == 2) {
        bool p0_coef = (H(132 + 11) & 1) != 0;
        coef_val = p0_coef ? uint(r0b.w) : 0u;
        if ((coef_val >> 31u) != 0u) {
          sel = 1;
          coef_val = uint(r1b.w);
        }
      } else {
        int b_sel = 132 + sel * 40;
        if ((H(b_sel + 11) & 1) != 0) {
          coef_val = (sel == 0) ? uint(r0b.w) : uint(r1b.w);
        }
      }
      int pb = 132 + sel * 40;
      ivec4 rpa = (sel == 0) ? r0a : r1a;
      ivec4 rpb = (sel == 0) ? r0b : r1b;
      int p_cfg = H(pb + 11);
      int kx = H(pb + 8);
      int ky = H(pb + 9);
      uint xp = uint(rpa.z);
      bool skip = false;
      if ((p_cfg & 1) != 0) {
        if ((coef_val >> 31u) != 0u) {
          skip = true;
        } else {
          int cv = sext_bits(coef_val, 24);
          int cmode = (p_cfg >> 2) & 3;
          if (cmode == 0) { kx = cv; ky = cv; }
          else if (cmode == 1) { kx = cv; }
          else if (cmode == 2) { ky = cv; }
          else { xp = uint(cv) << 2u; }
        }
      }
      sel_out = sel;
      skip_out = skip;
      rpa_out = rpa;
      rpb_out = rpb;
      kx_out = kx;
      ky_out = ky;
      xp_out = xp;
      return !skip;
    }
  `;

  const ULTRA_DEMESH_GLSL = `
        // Real De-Mesh (Modo HD): turn VDP1 (x^y)&1 checkerboard shadows into smooth 50% alpha
        if ((uDeMesh != 0 || uUltraScale >= 2) && !has_fb_row1 && !cc_add) {
          uint dL = fetch_vdp1_safe(src_x - 1, fb_y, fb_w, fb_rows);
          uint dR = fetch_vdp1_safe(src_x + 1, fb_y, fb_w, fb_rows);
          uint dU = fetch_vdp1_safe(src_x, fb_y - 1, fb_w, fb_rows);
          uint dD = fetch_vdp1_safe(src_x, fb_y + 1, fb_w, fb_rows);
          if (uDeMesh != 0) {
            if (d0 == 0u) {
              if (dL != 0u && dL == dR) {
                d0 = dL;
                half_mesh = true;
              } else if (dU != 0u && dU == dD) {
                d0 = dU;
                half_mesh = true;
              }
            } else {
              int zCnt = (dL == 0u ? 1 : 0) + (dR == 0u ? 1 : 0) + (dU == 0u ? 1 : 0) + (dD == 0u ? 1 : 0);
              if (zCnt >= 3) {
                uint dUL = fetch_vdp1_safe(src_x - 1, fb_y - 1, fb_w, fb_rows);
                uint dUR = fetch_vdp1_safe(src_x + 1, fb_y - 1, fb_w, fb_rows);
                uint dDL = fetch_vdp1_safe(src_x - 1, fb_y + 1, fb_w, fb_rows);
                uint dDR = fetch_vdp1_safe(src_x + 1, fb_y + 1, fb_w, fb_rows);
                int diagSame = (dUL == d0 ? 1 : 0) + (dUR == d0 ? 1 : 0) + (dDL == d0 ? 1 : 0) + (dDR == d0 ? 1 : 0);
                if (diagSame >= 2) half_mesh = true;
              }
            }
          }
          // Scale2x / EPX diagonal beveling on raw VDP1 indices at 2x Ultra HD
          if (uUltraScale >= 2 && !half_mesh) {
            uint dH = (sub_f.x < 0.5) ? dL : dR;
            uint dV = (sub_f.y < 0.5) ? dU : dD;
            uint dOH = (sub_f.x < 0.5) ? dR : dL;
            uint dOV = (sub_f.y < 0.5) ? dD : dU;
            if (dH == dV && dH != dOH && dV != dOV) {
              d0 = dH;
            }
          }
        }
  `;

  const ULTRA_RGSS_BRANCH_GLSL = `
            if (uUltraScale >= 2 && !rbg_perdot_x) {
              // Ultra HD 2x (1408x896) + 2x2 RGSS Supersampling on exact 3D RBG0 perspective
              int y_next = min(fh - 1, out_y + 1);
              int y_prev = max(0, out_y - 1);
              int sel_n = sel, kx_n = kx, ky_n = ky;
              bool skip_n = true;
              ivec4 rpa_n = rpa, rpb_n = rpb;
              uint xp_n = xp;
              bool ok_n = (y_next != out_y) && eval_rbg_row_params(y_next, r_mode, sel_n, skip_n, rpa_n, rpb_n, kx_n, ky_n, xp_n) && (sel_n == sel);
              int sel_p = sel, kx_p = kx, ky_p = ky;
              bool skip_p = true;
              ivec4 rpa_p = rpa, rpb_p = rpb;
              uint xp_p = xp;
              bool ok_p = (y_prev != out_y) && eval_rbg_row_params(y_prev, r_mode, sel_p, skip_p, rpa_p, rpb_p, kx_p, ky_p, xp_p) && (sel_p == sel);

              const vec2 RGSS[4] = vec2[4](
                vec2(-0.1875, -0.3125),
                vec2( 0.3125, -0.1875),
                vec2(-0.3125,  0.1875),
                vec2( 0.1875,  0.3125)
              );
              float x_mul = float(1 << x_shift);
              float rx_scale = (hires_x && !hires_rbg) ? 0.5 : 1.0;
              float rx_div = (hires_x && hires_rbg) ? 0.5 : 1.0;
              vec3 acc_rgb = vec3(0.0);
              int hits = 0;

              for (int si = 0; si < 4; ++si) {
                float sx_f = clamp(out_f.x + RGSS[si].x, 0.0, float(fw) - 0.001) * x_mul;
                float rx_f = sx_f * rx_scale;
                float dy_f = (out_f.y + RGSS[si].y) - (float(out_y) + 0.5);
                float xsp_s = float(rpa.x);
                float ysp_s = float(rpa.y);
                float ldx_s = float(rpb.x);
                float ldy_s = float(rpb.y);
                float kx_s = float(kx);
                float ky_s = float(ky);
                float xp_s = float(int(xp));
                float yp_s = float(rpa.w);
                if (dy_f > 0.0 && ok_n) {
                  float t = min(1.0, dy_f);
                  xsp_s = mix(xsp_s, float(rpa_n.x), t);
                  ysp_s = mix(ysp_s, float(rpa_n.y), t);
                  ldx_s = mix(ldx_s, float(rpb_n.x), t);
                  ldy_s = mix(ldy_s, float(rpb_n.y), t);
                  kx_s = mix(kx_s, float(kx_n), t);
                  ky_s = mix(ky_s, float(ky_n), t);
                  xp_s = mix(xp_s, float(int(xp_n)), t);
                  yp_s = mix(yp_s, float(rpa_n.w), t);
                } else if (dy_f < 0.0 && ok_p) {
                  float t = min(1.0, -dy_f);
                  xsp_s = mix(xsp_s, float(rpa_p.x), t);
                  ysp_s = mix(ysp_s, float(rpa_p.y), t);
                  ldx_s = mix(ldx_s, float(rpb_p.x), t);
                  ldy_s = mix(ldy_s, float(rpb_p.y), t);
                  kx_s = mix(kx_s, float(kx_p), t);
                  ky_s = mix(ky_s, float(ky_p), t);
                  xp_s = mix(xp_s, float(int(xp_p)), t);
                  yp_s = mix(yp_s, float(rpa_p.w), t);
                }
                float xsp_xf = xsp_s + ldx_s * rx_f * rx_div;
                float ysp_xf = ysp_s + ldy_s * rx_f * rx_div;
                uint ix_s = uint(int(floor((xp_s + (kx_s * xsp_xf) * 0.0000152587890625) * 0.0009765625)));
                uint iy_s = uint(int(floor((yp_s + (ky_s * ysp_xf) * 0.0000152587890625) * 0.0009765625)));
                uvec3 s_rgb;
                bool s_dir, s_msb, s_spr, s_scc;
                uint s_dcc;
                if (sample_rbg_texel(pb, p_cfg, ix_s, iy_s, cram_mask, s_rgb, s_dir, s_msb, s_spr, s_scc, s_dcc)) {
                  acc_rgb += vec3(s_rgb);
                  if (hits == 0) {
                    rp_direct = s_dir; rp_msb = s_msb; rp_spr = s_spr; rp_scc = s_scc; rp_dcc = s_dcc;
                  }
                  hits++;
                }
              }
              if (hits > 0) {
                rp_rgb = uvec3(acc_rgb / float(hits) + 0.5);
                rp_hit = true;
              }
            } else {
              int xsp_x = rpa.x + ((rpb.x * rx) >> rx_shift);
              int ysp_x = rpa.y + ((rpb.y * rx) >> rx_shift);
              uint ix = (xp + uint(mulShift16(kx, xsp_x))) >> 10u;
              uint iy = (uint(rpa.w) + uint(mulShift16(ky, ysp_x))) >> 10u;
              rp_hit = sample_rbg_texel(pb, p_cfg, ix, iy, cram_mask, rp_rgb, rp_direct, rp_msb, rp_spr, rp_scc, rp_dcc);
            }
  `;

  const LITE_RBG_BRANCH_GLSL = `
            {
              int xsp_x = rpa.x + ((rpb.x * rx) >> rx_shift);
              int ysp_x = rpa.y + ((rpb.y * rx) >> rx_shift);
              uint ix = (xp + uint(mulShift16(kx, xsp_x))) >> 10u;
              uint iy = (uint(rpa.w) + uint(mulShift16(ky, ysp_x))) >> 10u;
              rp_hit = sample_rbg_texel(pb, p_cfg, ix, iy, cram_mask, rp_rgb, rp_direct, rp_msb, rp_spr, rp_scc, rp_dcc);
            }
  `;

  function buildHdrBlock(useUbo) {
    if (useUbo) {
      return `
    layout(std140) uniform Vdp2Hdr {
      ivec4 uHdr[64];
    };
    int H(int idx) {
      ivec4 v = uHdr[idx >> 2];
      int c = idx & 3;
      if (c == 0) return v.x;
      if (c == 1) return v.y;
      if (c == 2) return v.z;
      return v.w;
    }
      `;
    }
    return `
    uniform highp isampler2D uHdrTex;   // 64 x 1 RGBA32I fallback
    int H(int idx) {
      ivec4 v = texelFetch(uHdrTex, ivec2(idx >> 2, 0), 0);
      int c = idx & 3;
      if (c == 0) return v.x;
      if (c == 1) return v.y;
      if (c == 2) return v.z;
      return v.w;
    }
    `;
  }

  function buildFragmentSource(opts = {}) {
    const ultra = Boolean(opts.ultra);
    const vdp1Rgba8 = Boolean(opts.vdp1Rgba8);
    const useUbo = opts.useUbo !== false;
    const vdp1SamplerBlock = buildVdp1SamplerBlock(vdp1Rgba8);
    const hdrBlock = buildHdrBlock(useUbo);
    const ultraUniforms = ultra
      ? `uniform int uUltraScale;\n    uniform int uDeMesh;`
      : '';
    const coordSetup = ultra
      ? `float fScale = float(max(1, uUltraScale));
      vec2 out_f = gl_FragCoord.xy / fScale;
      int out_x = int(out_f.x);
      int out_y = int(out_f.y);
      vec2 sub_f = fract(out_f);`
      : `int out_x = int(gl_FragCoord.x);
      int out_y = int(gl_FragCoord.y);`;

    return `#version 300 es
    precision highp float;
    precision highp int;
    precision highp usampler2D;
    precision highp isampler2D;

    uniform highp usampler2D uVdp2Vram; // 1024 x 512 R8UI
    uniform sampler2D uCramLut;         // 2048 x 1 RGBA8 (A = msb)
    ${vdp1SamplerBlock}
    uniform highp isampler2D uLineTable;// 8 x frame_h RGBA32I
    ${ultraUniforms}

    out vec4 fragColor;
    ${hdrBlock}

    uint v2_vb(uint a) {
      uint m = a & 0x7FFFFu;
      return texelFetch(uVdp2Vram, ivec2(int(m & 1023u), int(m >> 10u)), 0).r;
    }

    uint v2_vw(uint a) {
      uint m = a & 0x7FFFEu;
      int y = int(m >> 10u);
      int x = int(m & 1023u);
      return (texelFetch(uVdp2Vram, ivec2(x, y), 0).r << 8u) |
             texelFetch(uVdp2Vram, ivec2(x + 1, y), 0).r;
    }

    uvec3 rgb555_to_888(uint v) {
      return uvec3((v & 0x1Fu) << 3u, ((v >> 5u) & 0x1Fu) << 3u, ((v >> 10u) & 0x1Fu) << 3u);
    }

    int sext_bits(uint v, int bits) {
      uint sh = uint(32 - bits);
      return int(v << sh) >> int(sh);
    }

    int mulShift16(int a, int b) {
      bool neg = (a < 0) != (b < 0);
      uint ua = (a < 0) ? uint(-a) : uint(a);
      uint ub = (b < 0) ? uint(-b) : uint(b);
      uint a_hi = ua >> 16u;
      uint a_lo = ua & 0xFFFFu;
      uint b_hi = ub >> 16u;
      uint b_lo = ub & 0xFFFFu;
      uint lo = a_lo * b_lo;
      uint mid = a_hi * b_lo + a_lo * b_hi;
      uint hi = a_hi * b_hi;
      uint res = (hi << 16u) + mid + (lo >> 16u);
      if (neg) {
        uint round_up = ((lo & 0xFFFFu) != 0u) ? 1u : 0u;
        return -int(res + round_up);
      }
      return int(res);
    }

    bool line_windowed(uint ctl, int x, int w0p, int w1p) {
      if ((ctl & 0x0Au) == 0u) return false;
      bool any_w = false;
      bool all_w = true;
      if ((ctl & 0x02u) != 0u) {
        bool in0 = (w0p != -1) && (x >= (w0p & 0xFFFF) && x <= (w0p >> 16));
        bool a0 = ((ctl & 0x01u) != 0u) ? !in0 : in0;
        any_w = any_w || a0;
        all_w = all_w && a0;
      }
      if ((ctl & 0x08u) != 0u) {
        bool in1 = (w1p != -1) && (x >= (w1p & 0xFFFF) && x <= (w1p >> 16));
        bool a1 = ((ctl & 0x04u) != 0u) ? !in1 : in1;
        any_w = any_w || a1;
        all_w = all_w && a1;
      }
      return ((ctl & 0x80u) != 0u) ? all_w : any_w;
    }

    void cram_fetch(uint idx, uint cram_mask, out uvec3 rgb, out bool msb) {
      vec4 c = texelFetch(uCramLut, ivec2(int(idx & cram_mask), 0), 0);
      rgb = uvec3(uint(c.r * 255.0 + 0.5), uint(c.g * 255.0 + 0.5), uint(c.b * 255.0 + 0.5));
      msb = (c.a > 0.5);
    }

    bool v2_dot(uint colors, bool opaque, uint caos, uint base, int x, int y, int width,
                uint pal, uint cram_mask, out uvec3 rgb, out bool direct, out bool msb, out uint dcc) {
      uint i = uint(y * width + x);
      uint v = 0u;
      if (colors == 0u) {
        uint b = v2_vb(base + (i >> 1u));
        v = ((i & 1u) != 0u) ? (b & 0xFu) : (b >> 4u);
      } else if (colors == 1u) {
        v = v2_vb(base + i);
      } else if (colors == 2u) {
        v = v2_vw(base + (i << 1u)) & 0x7FFu;
      } else if (colors == 3u) {
        uint w = v2_vw(base + (i << 1u));
        if ((w & 0x8000u) == 0u && !opaque) return false;
        rgb = rgb555_to_888(w);
        direct = true;
        msb = (w >> 15u) != 0u;
        dcc = 0u;
        return true;
      } else {
        uint w = (v2_vw(base + (i << 2u)) << 16u) | v2_vw(base + (i << 2u) + 2u);
        if ((w & 0x80000000u) == 0u && !opaque) return false;
        rgb = uvec3(w & 0xFFu, (w >> 8u) & 0xFFu, (w >> 16u) & 0xFFu);
        direct = true;
        msb = (w >> 31u) != 0u;
        dcc = 0u;
        return true;
      }
      if (v == 0u && !opaque) return false;
      uint index = (colors == 0u) ? ((pal << 4u) + v) : ((colors == 1u) ? (((pal & 0x70u) << 4u) + v) : v);
      cram_fetch(index + caos, cram_mask, rgb, msb);
      direct = false;
      dcc = v;
      return true;
    }

    uint cell_bytes_for(uint colors) {
      if (colors == 0u) return 32u;
      if (colors == 1u) return 64u;
      if (colors == 2u || colors == 3u) return 128u;
      return 256u;
    }

    bool eval_sf_code(int layer, bool direct, uint dcc) {
      if (direct) return false;
      uint cb = uint(H(54 + layer));
      return ((cb >> ((dcc & 0xEu) >> 1u)) & 1u) != 0u;
    }

    uint eval_sf_prio(int layer, uint prio, bool spr, bool direct, uint dcc) {
      int mode = H(44 + layer);
      if (mode == 0) return prio;
      bool bit = (mode == 1) ? spr : ((mode == 2) ? (spr && eval_sf_code(layer, direct, dcc)) : false);
      return (prio & ~1u) | (bit ? 1u : 0u);
    }

    bool eval_sf_cc(int layer, bool cc, bool scc, bool msb, bool direct, uint dcc) {
      int mode = H(49 + layer);
      if (mode == 0) return cc;
      if (mode == 1) return cc && scc;
      if (mode == 2) return cc && scc && eval_sf_code(layer, direct, dcc);
      return cc && msb;
    }

    void get_sprite_type(int st, out int pr_shift, out int pr_mask, out int cc_shift, out int cc_mask, out uint dc_mask) {
      if (st == 0)      { pr_shift=14; pr_mask=3; cc_shift=11; cc_mask=7; dc_mask=0x7FFu; }
      else if (st == 1) { pr_shift=13; pr_mask=7; cc_shift=11; cc_mask=3; dc_mask=0x7FFu; }
      else if (st == 2) { pr_shift=14; pr_mask=1; cc_shift=11; cc_mask=7; dc_mask=0x7FFu; }
      else if (st == 3) { pr_shift=13; pr_mask=3; cc_shift=11; cc_mask=3; dc_mask=0x7FFu; }
      else if (st == 4) { pr_shift=13; pr_mask=3; cc_shift=10; cc_mask=7; dc_mask=0x3FFu; }
      else if (st == 5) { pr_shift=12; pr_mask=7; cc_shift=11; cc_mask=1; dc_mask=0x7FFu; }
      else if (st == 6) { pr_shift=12; pr_mask=7; cc_shift=10; cc_mask=3; dc_mask=0x3FFu; }
      else if (st == 7) { pr_shift=12; pr_mask=7; cc_shift=9;  cc_mask=7; dc_mask=0x1FFu; }
      else if (st == 8) { pr_shift=7;  pr_mask=1; cc_shift=0;  cc_mask=0; dc_mask=0x7Fu; }
      else if (st == 9) { pr_shift=7;  pr_mask=1; cc_shift=6;  cc_mask=1; dc_mask=0x3Fu; }
      else if (st == 10){ pr_shift=6;  pr_mask=3; cc_shift=0;  cc_mask=0; dc_mask=0x3Fu; }
      else if (st == 11){ pr_shift=0;  pr_mask=0; cc_shift=6;  cc_mask=3; dc_mask=0x3Fu; }
      else if (st == 12){ pr_shift=7;  pr_mask=1; cc_shift=0;  cc_mask=0; dc_mask=0xFFu; }
      else if (st == 13){ pr_shift=7;  pr_mask=1; cc_shift=6;  cc_mask=1; dc_mask=0xFFu; }
      else if (st == 14){ pr_shift=6;  pr_mask=3; cc_shift=0;  cc_mask=0; dc_mask=0xFFu; }
      else              { pr_shift=0;  pr_mask=0; cc_shift=6;  cc_mask=3; dc_mask=0xFFu; }
    }

    uint v2_coef_read_gpu(bool crkte, bool coef1w, uint addr) {
      uint w0 = !crkte ? v2_vw(addr * 2u) : 0u;
      if (coef1w) {
        return (uint(sext_bits(w0 << 6u, 21)) & 0xFFFFFFu) | ((w0 & 0x8000u) << 16u);
      }
      uint w1 = !crkte ? v2_vw((addr + 1u) * 2u) : 0u;
      return (w0 << 16u) | w1;
    }

    uint fetch_vdp1_safe(int x, int y, int fb_w, int fb_rows) {
      if (x < 0 || x >= fb_w || y < 0 || y >= fb_rows) return 0u;
      return fetch_vdp1_raw(x, y);
    }

    ${ultra ? ULTRA_HELPERS_GLSL : ''}

    bool sample_rbg_texel(int pb, int p_cfg, uint ix, uint iy, uint cram_mask,
                          out uvec3 rp_rgb, out bool rp_direct, out bool rp_msb,
                          out bool rp_spr, out bool rp_scc, out uint rp_dcc) {
      uint cbits = uint(H(pb + 0));
      bool opaque = (cbits & 2u) != 0u;
      bool is_bm = (cbits & 4u) != 0u;
      bool cell2x2 = (cbits & 8u) != 0u;
      bool word1 = (cbits & 16u) != 0u;
      bool cnsm = (cbits & 32u) != 0u;
      uint colors = (cbits >> 12u) & 7u;
      uint plane_w = (cbits >> 16u) & 3u;
      uint plane_h = (cbits >> 18u) & 3u;
      uint supp = uint(H(pb + 1));
      uint page_bytes = uint(H(pb + 2));
      uint caos = uint(H(pb + 3));
      int over_mode = (p_cfg >> 4) & 3;
      uint over_pn = uint(p_cfg >> 8) & 0xFFFFu;

      uint mw = is_bm ? uint(H(pb + 4)) : (plane_w << 11u);
      uint mh = is_bm ? uint(H(pb + 5)) : (plane_h << 11u);
      bool outside = false;
      if (over_mode == 3) outside = ((ix | iy) & ~511u) != 0u;
      else if (over_mode != 0) outside = ((ix & ~(mw - 1u)) != 0u) || ((iy & ~(mh - 1u)) != 0u);

      if (outside && (over_mode & 2) != 0) return false;

      rp_rgb = uvec3(0u);
      rp_direct = false; rp_msb = false; rp_spr = false; rp_scc = false;
      rp_dcc = 0u;

      if (is_bm) {
        rp_spr = (cbits & 512u) != 0u;
        rp_scc = (cbits & 1024u) != 0u;
        return v2_dot(colors, opaque, caos, uint(H(pb + 6)),
                      int(ix & (mw - 1u)), int(iy & (mh - 1u)),
                      H(pb + 4), uint(H(pb + 7)), cram_mask,
                      rp_rgb, rp_direct, rp_msb, rp_dcc);
      }

      uint sx = ix & (mw - 1u);
      uint sy = iy & (mh - 1u);
      uint pw_sh = (plane_w == 2u) ? 10u : 9u;
      uint ph_sh = (plane_h == 2u) ? 10u : 9u;
      uint cs_sh = cell2x2 ? 4u : 3u;
      uint cs = 1u << cs_sh;
      uint c_ix = sx & ((1u << pw_sh) - 1u);
      uint c_iy = sy & ((1u << ph_sh) - 1u);
      uint chr = 0u, pal = 0u;
      bool hf = false, vf = false;

      if (outside || word1) {
        uint w = over_pn;
        if (!outside) {
          int pl = int((sy >> ph_sh) << 2u) + int(sx >> pw_sh);
          uint page = (c_iy >> 9u) * plane_w + (c_ix >> 9u);
          uint cpr = cell2x2 ? 32u : 64u;
          uint pnd_addr = uint(H(pb + 12 + pl)) + page * page_bytes +
                          ((((c_iy & 511u) >> cs_sh) * cpr + ((c_ix & 511u) >> cs_sh)) << 1u);
          w = v2_vw(pnd_addr);
        }
        rp_spr = ((supp >> 9u) & 1u) != 0u;
        rp_scc = ((supp >> 8u) & 1u) != 0u;
        pal = (colors == 0u) ? (((w >> 12u) & 0xFu) | ((supp >> 1u) & 0x70u)) : ((w >> 8u) & 0x70u);
        if (!cnsm) {
          hf = (w & 0x400u) != 0u;
          vf = (w & 0x800u) != 0u;
          chr = cell2x2 ? (((w & 0x3FFu) << 2u) | (supp & 3u) | ((supp & 0x1Cu) << 10u))
                        : ((w & 0x3FFu) | ((supp & 0x1Fu) << 10u));
        } else {
          chr = cell2x2 ? (((w & 0xFFFu) << 2u) | (supp & 3u) | ((supp & 0x10u) << 10u))
                        : ((w & 0xFFFu) | ((supp & 0x1Cu) << 10u));
        }
      } else {
        int pl = int((sy >> ph_sh) << 2u) + int(sx >> pw_sh);
        uint page = (c_iy >> 9u) * plane_w + (c_ix >> 9u);
        uint cpr = cell2x2 ? 32u : 64u;
        uint pnd_addr = uint(H(pb + 12 + pl)) + page * page_bytes +
                        ((((c_iy & 511u) >> cs_sh) * cpr + ((c_ix & 511u) >> cs_sh)) << 2u);
        uint w0 = v2_vw(pnd_addr);
        uint w1 = v2_vw(pnd_addr + 2u);
        hf = (w0 & 0x4000u) != 0u;
        vf = (w0 & 0x8000u) != 0u;
        rp_spr = ((w0 >> 13u) & 1u) != 0u;
        rp_scc = ((w0 >> 12u) & 1u) != 0u;
        pal = w0 & 0x7Fu;
        chr = w1 & 0x7FFFu;
      }
      int px = int(c_ix & (cs - 1u));
      int py = int(c_iy & (cs - 1u));
      if (hf) px = int(cs) - 1 - px;
      if (vf) py = int(cs) - 1 - py;
      uint base = chr << 5u;
      if (cell2x2) base += uint((py >> 3) * 2 + (px >> 3)) * cell_bytes_for(colors);
      return v2_dot(colors, opaque, caos, base, px & 7, py & 7, 8, pal, cram_mask,
                    rp_rgb, rp_direct, rp_msb, rp_dcc);
    }

    void main() {
      ${coordSetup}
      int fw = H(1);
      int fh = H(2);
      if (out_x >= fw || out_y >= fh) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
      }
      uint flags0 = uint(H(7));
      if ((flags0 & 1u) == 0u) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
      }

      int x_shift = H(6);
      int src_x = out_x << x_shift;
      uint cram_mask = uint(H(8));
      uint dbg_hide = (flags0 >> 16u) & 0xFFu;
      bool hires_x = (flags0 & 4u) != 0u;
      bool hires_rbg = (flags0 & 8u) != 0u;
      bool rbg_active = (flags0 & 32u) != 0u;
      bool rbg_perdot_x = (flags0 & 64u) != 0u;
      bool cc_add = (flags0 & 128u) != 0u;
      bool sp_cc_en = (flags0 & 256u) != 0u;
      bool sp_mixed = (flags0 & 512u) != 0u;
      bool sp_off = (flags0 & 1024u) != 0u;
      bool sp_offb = (flags0 & 2048u) != 0u;
      bool back_off = (flags0 & 4096u) != 0u;
      bool back_offb = (flags0 & 8192u) != 0u;
      bool rbg_crkte = (flags0 & 16384u) != 0u;
      bool has_fb_row1 = (flags0 & (1u << 24u)) != 0u;

      ivec4 l0 = texelFetch(uLineTable, ivec2(0, out_y), 0);
      uint back_u = uint(l0.x);
      int w0p = l0.y;
      int w1p = l0.z;
      int y_src = l0.w;

      uvec3 top_rgb = uvec3((back_u >> 16u) & 255u, (back_u >> 8u) & 255u, back_u & 255u);
      uvec3 sec_rgb = top_rgb;
      uint top_prio = 0u;
      uint sec_prio = 0u;
      uint top_attr = (back_off ? 0x40u : 0u) | (back_offb ? 0x80u : 0u);

      // Pass 1: VDP1 Sprites
      if ((dbg_hide & 1u) == 0u) {
        uint sp_wctl = uint(H(32));
        int fb_w = H(9);
        int fb_rows = H(10);
        bool use_row1_add = has_fb_row1 && cc_add && ((out_y & 1) != 0) && (y_src + 1 < fb_rows);
        int fb_y = use_row1_add ? (y_src + 1) : y_src;
        uint d0 = fetch_vdp1_safe(src_x, fb_y, fb_w, fb_rows);
        bool half_mesh = false;
        if (has_fb_row1 && !cc_add && src_x < fb_w && (y_src + 1 < fb_rows)) {
          uint d1 = fetch_vdp1_raw(src_x, y_src + 1);
          if (d0 == 0u && d1 != 0u) {
            d0 = d1;
            half_mesh = true;
          } else if (d0 != 0u && d1 == 0u) {
            half_mesh = true;
          }
        }
        ${ultra ? ULTRA_DEMESH_GLSL : ''}
        if (d0 != 0u && !line_windowed(sp_wctl, src_x, w0p, w1p)) {
          uvec3 srgb = uvec3(0u);
          uint sprio = 0u;
          uint sratio = 0u;
          bool smsb = false;
          bool sp_valid = true;
          if (sp_mixed && (d0 & 0x8000u) != 0u) {
            srgb = rgb555_to_888(d0);
            sprio = uint(H(16));
            sratio = uint(H(24));
            smsb = true;
          } else {
            int sp_type = H(12);
            int pr_sh, pr_mk, cc_sh, cc_mk;
            uint dc_mk;
            get_sprite_type(sp_type, pr_sh, pr_mk, cc_sh, cc_mk, dc_mk);
            uint d = (sp_type >= 8) ? (d0 & 0xFFu) : d0;
            uint dc = d & dc_mk;
            if (dc == 0u) {
              sp_valid = false;
            } else {
              int pr = (pr_mk != 0) ? (int(d >> uint(pr_sh)) & pr_mk) : 0;
              int cc = (cc_mk != 0) ? (int(d >> uint(cc_sh)) & cc_mk) : 0;
              sprio = uint(H(16 + pr));
              sratio = uint(H(24 + cc));
              cram_fetch(dc + uint(H(15)), cram_mask, srgb, smsb);
            }
          }
          if (sp_valid && sprio != 0u) {
            bool scc = false;
            if (sp_cc_en) {
              int cm = H(13);
              uint cn = uint(H(14));
              if (cm == 0) scc = (sprio <= cn);
              else if (cm == 1) scc = (sprio == cn);
              else if (cm == 2) scc = (sprio >= cn);
              else scc = smsb;
            }
            if (half_mesh) {
              if (scc) {
                uint inv = (32u - (sratio & 0x1Fu)) >> 1u;
                sratio = min(31u, 32u - inv);
              } else {
                scc = true;
                sratio = 16u;
              }
            }
            top_rgb = srgb;
            top_prio = sprio;
            top_attr = (sratio & 0x1Fu) | (scc ? 0x20u : 0u) | (sp_off ? 0x40u : 0u) | (sp_offb ? 0x80u : 0u);
          }
        }
      }

      // Pass 2: RBG0
      if (rbg_active) {
        uint r_wctl = uint(H(33));
        if (!line_windowed(r_wctl, src_x, w0p, w1p)) {
          int rx = (hires_x && !hires_rbg) ? (src_x >> 1) : src_x;
          int rx_shift = (hires_x && hires_rbg) ? 1 : 0;
          int r_mode = H(128);
          uint r_flags = uint(H(131));
          int sel = (r_mode == 1) ? 1 : 0;
          uint coef_val = 0u;
          bool rbg_skip = false;

          ivec4 r0a = texelFetch(uLineTable, ivec2(1, out_y), 0);
          ivec4 r0b = texelFetch(uLineTable, ivec2(2, out_y), 0);
          ivec4 r1a = texelFetch(uLineTable, ivec2(3, out_y), 0);
          ivec4 r1b = texelFetch(uLineTable, ivec2(4, out_y), 0);

          if (!rbg_perdot_x) {
            if (r_mode == 2) {
              bool p0_coef = (H(132 + 11) & 1) != 0;
              coef_val = p0_coef ? uint(r0b.w) : 0u;
              if ((coef_val >> 31u) != 0u) {
                sel = 1;
                coef_val = uint(r1b.w);
              }
            } else {
              int b_sel = 132 + sel * 40;
              if ((H(b_sel + 11) & 1) != 0) {
                coef_val = (sel == 0) ? uint(r0b.w) : uint(r1b.w);
              }
            }
          } else {
            int q_sel = (r_mode == 2) ? 0 : sel;
            int qb = 132 + q_sel * 40;
            int q_cfg = H(qb + 11);
            if ((q_cfg & 1) != 0) {
              int dkax = H(qb + 10);
              uint ka0_line = uint((q_sel == 0) ? r0b.z : r1b.z);
              uint ka_step = uint((dkax * rx) >> rx_shift);
              bool c1w = (q_cfg & 2) != 0;
              uint ca = (((ka0_line + ka_step) >> 10u) << (c1w ? 0u : 1u)) & (rbg_crkte ? 0x3FFu : 0x3FFFFu);
              uint bank_idx = rbg_crkte ? 0u : ((ca >> 16u) & 3u);
              bool bc = ((r_flags >> (3u + bank_idx)) & 1u) != 0u;
              coef_val = bc ? v2_coef_read_gpu(rbg_crkte, c1w, ca) : 0u;
            }
            if (r_mode == 2 && (coef_val >> 31u) != 0u) {
              sel = 1;
              coef_val = uint(r1b.w);
            }
          }

          int pb = 132 + sel * 40;
          ivec4 rpa = (sel == 0) ? r0a : r1a;
          ivec4 rpb = (sel == 0) ? r0b : r1b;
          int p_cfg = H(pb + 11);
          int kx = H(pb + 8);
          int ky = H(pb + 9);
          uint xp = uint(rpa.z);
          if ((p_cfg & 1) != 0) {
            if ((coef_val >> 31u) != 0u) {
              rbg_skip = true;
            } else {
              int cv = sext_bits(coef_val, 24);
              int cmode = (p_cfg >> 2) & 3;
              if (cmode == 0) { kx = cv; ky = cv; }
              else if (cmode == 1) { kx = cv; }
              else if (cmode == 2) { ky = cv; }
              else { xp = uint(cv) << 2u; }
            }
          }

          if (!rbg_skip) {
            uvec3 rp_rgb = uvec3(0u);
            bool rp_direct = false, rp_msb = false, rp_spr = false, rp_scc = false;
            uint rp_dcc = 0u;
            bool rp_hit = false;

            ${ultra ? ULTRA_RGSS_BRANCH_GLSL : LITE_RBG_BRANCH_GLSL}

            if (rp_hit) {
              uint rprio = eval_sf_prio(4, uint(H(129)), rp_spr, rp_direct, rp_dcc);
              if (rprio != 0u) {
                bool rcc = eval_sf_cc(4, (r_flags & 1u) != 0u, rp_scc, rp_msb, rp_direct, rp_dcc);
                bool roff = (r_flags & 2u) != 0u;
                bool roffb = (r_flags & 4u) != 0u;
                uint rratio = uint(H(130));
                if (top_prio == 0u || rprio > top_prio) {
                  if (top_prio != 0u) {
                    sec_rgb = top_rgb;
                    sec_prio = top_prio;
                  }
                  top_rgb = rp_rgb;
                  top_prio = rprio;
                  top_attr = (rratio & 0x1Fu) | (rcc ? 0x20u : 0u) | (roff ? 0x40u : 0u) | (roffb ? 0x80u : 0u);
                } else if (sec_prio == 0u || rprio > sec_prio) {
                  sec_rgb = rp_rgb;
                  sec_prio = rprio;
                }
              }
            }
          }
        }
      }

      // Pass 3: Active NBG0..3 layers
      int active_nbg_count = H(59);
      for (int idx = 0; idx < 4; ++idx) {
        if (idx >= active_nbg_count) break;
        int n = H(60 + idx);
        uint n_wctl = uint(H(34 + n));
        if (line_windowed(n_wctl, src_x, w0p, w1p)) continue;

        int nb = 64 + n * 16;
        uint nbits = uint(H(nb + 0));
        bool opaque = (nbits & 2u) != 0u;
        bool is_bm = (nbits & 4u) != 0u;
        bool cell2x2 = (nbits & 8u) != 0u;
        bool word1 = (nbits & 16u) != 0u;
        bool cnsm = (nbits & 32u) != 0u;
        bool n_cc_cfg = (nbits & 64u) != 0u;
        bool n_off = (nbits & 128u) != 0u;
        bool n_offb = (nbits & 256u) != 0u;
        uint colors = (nbits >> 12u) & 7u;
        uint plane_w = (nbits >> 16u) & 3u;
        uint plane_h = (nbits >> 18u) & 3u;
        uint supp = uint(H(nb + 1));
        uint page_bytes = uint(H(nb + 2));
        uint caos = uint(H(nb + 3));

        int bx = 0, ndx = 256, by = 0;
        if (n == 0) {
          ivec4 ln = texelFetch(uLineTable, ivec2(5, out_y), 0);
          bx = ln.x; ndx = ln.y; by = ln.z;
        } else if (n == 1) {
          ivec4 ln = texelFetch(uLineTable, ivec2(6, out_y), 0);
          bx = ln.x; ndx = ln.y; by = ln.z;
        } else {
          ivec4 ln = texelFetch(uLineTable, ivec2(7, out_y), 0);
          if (n == 2) { bx = ln.x; by = ln.y; }
          else { bx = ln.z; by = ln.w; }
          ndx = 256;
        }

        uint sx_raw = uint(bx + src_x * ndx) >> 8u;
        uint sy_raw = uint(by) >> 8u;

        uvec3 np_rgb = uvec3(0u);
        bool np_direct = false, np_msb = false, np_spr = false, np_scc = false;
        uint np_dcc = 0u;
        bool np_hit = false;

        if (is_bm) {
          uint bm_w = uint(H(nb + 8));
          uint bm_h = uint(H(nb + 9));
          np_spr = (nbits & 512u) != 0u;
          np_scc = (nbits & 1024u) != 0u;
          np_hit = v2_dot(colors, opaque, caos, uint(H(nb + 10)),
                          int(sx_raw & (bm_w - 1u)), int(sy_raw & (bm_h - 1u)),
                          int(bm_w), uint(H(nb + 11)), cram_mask,
                          np_rgb, np_direct, np_msb, np_dcc);
        } else {
          uint pw_sh = (plane_w == 2u) ? 10u : 9u;
          uint ph_sh = (plane_h == 2u) ? 10u : 9u;
          uint pw = 1u << pw_sh;
          uint ph = 1u << ph_sh;
          uint cs_sh = cell2x2 ? 4u : 3u;
          uint cs = 1u << cs_sh;
          uint sx = sx_raw & (pw * 2u - 1u);
          uint sy = sy_raw & (ph * 2u - 1u);
          uint ix = sx & (pw - 1u);
          uint iy = sy & (ph - 1u);
          int pl = int(((sy >> ph_sh) << 1u) + (sx >> pw_sh));
          uint page = (iy >> 9u) * plane_w + (ix >> 9u);
          uint cpr = cell2x2 ? 32u : 64u;
          uint pnd_addr = uint(H(nb + 4 + pl)) + page * page_bytes +
                          ((((iy & 511u) >> cs_sh) * cpr + ((ix & 511u) >> cs_sh)) << (word1 ? 1u : 2u));

          uint chr = 0u, pal = 0u;
          bool hf = false, vf = false;
          if (word1) {
            uint w = v2_vw(pnd_addr);
            np_spr = ((supp >> 9u) & 1u) != 0u;
            np_scc = ((supp >> 8u) & 1u) != 0u;
            pal = (colors == 0u) ? (((w >> 12u) & 0xFu) | ((supp >> 1u) & 0x70u)) : ((w >> 8u) & 0x70u);
            if (!cnsm) {
              hf = (w & 0x400u) != 0u;
              vf = (w & 0x800u) != 0u;
              chr = cell2x2 ? (((w & 0x3FFu) << 2u) | (supp & 3u) | ((supp & 0x1Cu) << 10u))
                            : ((w & 0x3FFu) | ((supp & 0x1Fu) << 10u));
            } else {
              chr = cell2x2 ? (((w & 0xFFFu) << 2u) | (supp & 3u) | ((supp & 0x10u) << 10u))
                            : ((w & 0xFFFu) | ((supp & 0x1Cu) << 10u));
            }
          } else {
            uint w0 = v2_vw(pnd_addr);
            uint w1 = v2_vw(pnd_addr + 2u);
            hf = (w0 & 0x4000u) != 0u;
            vf = (w0 & 0x8000u) != 0u;
            np_spr = ((w0 >> 13u) & 1u) != 0u;
            np_scc = ((w0 >> 12u) & 1u) != 0u;
            pal = w0 & 0x7Fu;
            chr = w1 & 0x7FFFu;
          }
          int px = int(ix & (cs - 1u));
          int py = int(iy & (cs - 1u));
          if (hf) px = int(cs) - 1 - px;
          if (vf) py = int(cs) - 1 - py;
          uint base = chr << 5u;
          if (cell2x2) base += uint((py >> 3) * 2 + (px >> 3)) * cell_bytes_for(colors);
          np_hit = v2_dot(colors, opaque, caos, base, px & 7, py & 7, 8, pal, cram_mask,
                          np_rgb, np_direct, np_msb, np_dcc);
        }

        if (np_hit) {
          uint nprio = eval_sf_prio(n, uint(H(nb + 12)), np_spr, np_direct, np_dcc);
          if (nprio != 0u) {
            bool ncc = eval_sf_cc(n, n_cc_cfg, np_scc, np_msb, np_direct, np_dcc);
            uint nratio = uint(H(nb + 13));
            if (top_prio == 0u || nprio > top_prio) {
              if (top_prio != 0u) {
                sec_rgb = top_rgb;
                sec_prio = top_prio;
              }
              top_rgb = np_rgb;
              top_prio = nprio;
              top_attr = (nratio & 0x1Fu) | (ncc ? 0x20u : 0u) | (n_off ? 0x40u : 0u) | (n_offb ? 0x80u : 0u);
            } else if (sec_prio == 0u || nprio > sec_prio) {
              sec_rgb = np_rgb;
              sec_prio = nprio;
            }
          }
        }
      }

      // Pass 4: Color calculation + Color offset A/B
      uvec3 final_rgb = top_rgb;
      if ((top_attr & 0x20u) != 0u && top_prio != 0u) {
        if (cc_add) {
          final_rgb = min(final_rgb + sec_rgb, uvec3(255u));
        } else {
          uint rat = top_attr & 0x1Fu;
          uint inv = 32u - rat;
          final_rgb = (final_rgb * inv + sec_rgb * rat) >> 5u;
        }
      }
      if ((top_attr & 0x40u) != 0u) {
        int ob = ((top_attr & 0x80u) != 0u) ? 41 : 38;
        ivec3 ov = ivec3(H(ob), H(ob + 1), H(ob + 2));
        final_rgb = uvec3(clamp(ivec3(final_rgb) + ov, ivec3(0), ivec3(255)));
      }

      fragColor = vec4(vec3(final_rgb) / 255.0, 1.0);
    }
    `;
  }

  const api = {
    VS_VDP2,
    buildFragmentSource,
  };

  if (typeof window !== 'undefined') {
    window.TwcWebGLVdp2Shaders = api;
  } else {
    self.TwcWebGLVdp2Shaders = api;
  }
})();
