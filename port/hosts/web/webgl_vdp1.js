// TWC98BO WebGL2 Hardware VDP1 Renderer (Mobile-Optimized Single-Quad + Portable RGBA8 FBO Fallback).
// Rasterizes ST-V VDP1 commands (comm 0..7, cmode 0..5, mesh, gouraud, clip/outside, ccb 0..3, mon, TVMR 8/16-bit)
// directly into an R16UI (or fallback RGBA8 packed) FBO texture consumed by TwcWebGLVdp2Compositor.

(() => {
  const VS_VDP1 = `#version 300 es
    precision highp float;
    precision highp int;

    layout(location = 0) in ivec4 aP0; // x0, y0, x1, y1
    layout(location = 1) in ivec4 aP1; // srca, ctrl|(pmod<<16), colr|(primType<<16)|(v<<18), w|(h<<16)
    layout(location = 2) in ivec4 aP2; // ux0|(uy0<<16), ux1|(uy1<<16), sys_x|(sys_y<<16), g0|(g1<<16)
    layout(location = 3) in ivec4 aP3; // x2, y2, x3, y3 (for primType == 2 single-quad)

    uniform vec2 uFbSize;

    flat out ivec4 vP0;
    flat out ivec4 vP1;
    flat out ivec4 vP2;
    flat out ivec4 vP3;

    const int CORNER_X[6] = int[6](0, 1, 0, 0, 1, 1);
    const int CORNER_Y[6] = int[6](0, 0, 1, 1, 0, 1);

    void main() {
      vP0 = aP0;
      vP1 = aP1;
      vP2 = aP2;
      vP3 = aP3;

      int x0 = aP0.x;
      int y0 = aP0.y;
      int x1 = aP0.z;
      int y1 = aP0.w;
      int primType = (aP1.z >> 16) & 3;

      int cx = CORNER_X[gl_VertexID];
      int cy = CORNER_Y[gl_VertexID];

      vec2 pos;
      if (primType == 2) {
        int minX = min(min(x0, x1), min(aP3.x, aP3.z));
        int maxX = max(max(x0, x1), max(aP3.x, aP3.z)) + 1;
        int minY = min(min(y0, y1), min(aP3.y, aP3.w));
        int maxY = max(max(y0, y1), max(aP3.y, aP3.w)) + 1;
        pos = vec2(cx == 0 ? float(minX) : float(maxX), cy == 0 ? float(minY) : float(maxY));
      } else {
        int adx = abs(x1 - x0);
        int ady = abs(y1 - y0);
        if (primType == 0 || x0 == x1 || y0 == y1 || max(adx, ady) <= 24) {
          float minX = float(min(x0, x1));
          float maxX = float(max(x0, x1) + 1);
          float minY = float(min(y0, y1));
          float maxY = float(max(y0, y1) + 1);
          pos = vec2(cx == 0 ? minX : maxX, cy == 0 ? minY : maxY);
        } else {
          vec2 p0 = vec2(float(x0) + 0.5, float(y0) + 0.5);
          vec2 p1 = vec2(float(x1) + 0.5, float(y1) + 0.5);
          vec2 dir = normalize(p1 - p0);
          vec2 nrm = vec2(-dir.y, dir.x);
          vec2 endPt = (cx == 0) ? (p0 - dir * 1.5) : (p1 + dir * 1.5);
          pos = endPt + nrm * ((cy == 0) ? -2.0 : 2.0);
        }
      }

      vec2 ndc = (pos / uFbSize) * 2.0 - 1.0;
      gl_Position = vec4(ndc, 0.0, 1.0);
    }
  `;

  function buildFsVdp1(useRgba8Fbo) {
    const fbCopyDecl = useRgba8Fbo
      ? `uniform sampler2D uFbCopy;
    out vec4 outPix;
    uint read_fb_copy(int px, int py) {
      vec2 rg = texelFetch(uFbCopy, ivec2(px, py), 0).rg;
      return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8u);
    }
    void emit_pix(uint val) {
      outPix = vec4(float(val & 0xFFu) / 255.0, float((val >> 8u) & 0xFFu) / 255.0, 0.0, 1.0);
    }`
      : `uniform highp usampler2D uFbCopy;
    out uvec4 outPix;
    uint read_fb_copy(int px, int py) {
      return texelFetch(uFbCopy, ivec2(px, py), 0).r;
    }
    void emit_pix(uint val) {
      outPix = uvec4(val, 0u, 0u, 1u);
    }`;

    return `#version 300 es
    precision highp float;
    precision highp int;
    precision highp usampler2D;

    flat in ivec4 vP0;
    flat in ivec4 vP1;
    flat in ivec4 vP2;
    flat in ivec4 vP3;

    uniform highp usampler2D uVdp1Vram; // 1024 x 512 R8UI (512 KiB)
    uniform int uTvmr;
    uniform ivec2 uFbSizeI;
    ${fbCopyDecl}

    uint v1_vb(int addr) {
      int a = addr & 0x7FFFF;
      return texelFetch(uVdp1Vram, ivec2(a & 1023, a >> 10), 0).r;
    }

    uint v1_vw(int addr) {
      int a = addr & 0x7FFFE;
      uint hi = texelFetch(uVdp1Vram, ivec2(a & 1023, a >> 10), 0).r;
      uint lo = texelFetch(uVdp1Vram, ivec2((a + 1) & 1023, a >> 10), 0).r;
      return (hi << 8u) | lo;
    }

    ivec3 rgb5_of(int v) {
      return ivec3(v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F);
    }

    ivec3 mix_rgb5(ivec3 a, ivec3 b, int k, int n) {
      if (n <= 0) return a;
      return ivec3(
        a.x + ((b.x - a.x) * k) / n,
        a.y + ((b.y - a.y) * k) / n,
        a.z + ((b.z - a.z) * k) / n
      );
    }

    float cross2(vec2 a, vec2 b) {
      return a.x * b.y - a.y * b.x;
    }

    // Solve bilinear quad P = A + u*(B-A) + v*(D-A) + u*v*(A-B+C-D)
    bool solve_quad_uv(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d, out vec2 uv) {
      vec2 e = b - a;
      vec2 f = d - a;
      vec2 g = a - b + c - d;
      vec2 h = p - a;
      float k2 = cross2(g, f);
      float k1 = cross2(e, f) + cross2(h, g);
      float k0 = cross2(h, e);
      const float EPS = 0.0015;

      if (abs(k2) < 0.0001) {
        if (abs(k1) < 0.0001) return false;
        float v = -k0 / k1;
        vec2 denom = e + g * v;
        float u = (abs(denom.x) > abs(denom.y))
          ? ((h.x - f.x * v) / denom.x)
          : ((h.y - f.y * v) / denom.y);
        if (u < -EPS || u > 1.0 + EPS || v < -EPS || v > 1.0 + EPS) return false;
        uv = clamp(vec2(u, v), vec2(0.0), vec2(1.0));
        return true;
      }

      float disc = k1 * k1 - 4.0 * k0 * k2;
      if (disc < 0.0) return false;
      float s = sqrt(disc);
      float inv2k2 = 0.5 / k2;

      float v1 = (-k1 - s) * inv2k2;
      vec2 d1 = e + g * v1;
      float u1 = (abs(d1.x) > abs(d1.y))
        ? ((h.x - f.x * v1) / d1.x)
        : ((h.y - f.y * v1) / d1.y);
      if (u1 >= -EPS && u1 <= 1.0 + EPS && v1 >= -EPS && v1 <= 1.0 + EPS) {
        uv = clamp(vec2(u1, v1), vec2(0.0), vec2(1.0));
        return true;
      }

      float v2 = (-k1 + s) * inv2k2;
      vec2 d2 = e + g * v2;
      float u2 = (abs(d2.x) > abs(d2.y))
        ? ((h.x - f.x * v2) / d2.x)
        : ((h.y - f.y * v2) / d2.y);
      if (u2 >= -EPS && u2 <= 1.0 + EPS && v2 >= -EPS && v2 <= 1.0 + EPS) {
        uv = clamp(vec2(u2, v2), vec2(0.0), vec2(1.0));
        return true;
      }
      return false;
    }

    int sample_texel(int srca, int w, int h, int cmode, int colr, bool hflip, bool vflip, bool spd, bool ecd, int u, int v) {
      if (hflip) u = w - 1 - u;
      if (vflip) v = h - 1 - v;
      int i = v * w + u;
      uint raw = 0u;
      bool is_end = false;
      bool is_zero = false;
      uint out_val = 0u;

      if (cmode <= 1) {
        uint b = v1_vb(srca + (i >> 1));
        raw = ((i & 1) != 0) ? (b & 0xFu) : (b >> 4u);
        is_end = (raw == 0xFu);
        is_zero = (raw == 0u);
        if (cmode == 0) {
          out_val = (uint(colr) & 0xFFF0u) | raw;
        } else {
          out_val = v1_vw(colr * 8 + int(raw) * 2);
        }
      } else if (cmode <= 4) {
        raw = v1_vb(srca + i);
        is_end = (raw == 0xFFu);
        is_zero = (raw == 0u);
        uint m = (cmode == 2) ? 0x3Fu : ((cmode == 3) ? 0x7Fu : 0xFFu);
        out_val = (uint(colr) & (~m & 0xFFFFu)) | (raw & m);
      } else {
        raw = v1_vw(srca + i * 2);
        is_end = (raw == 0x7FFFu);
        is_zero = (raw == 0u);
        out_val = raw;
      }

      if (is_end && !ecd) return -1;
      if (is_zero && !spd) return -1;
      return int(out_val & 0xFFFFu);
    }

    void main() {
      int px = int(gl_FragCoord.x);
      int py = int(gl_FragCoord.y);

      int sys_x = vP2.z & 0xFFFF;
      int sys_y = (vP2.z >> 16) & 0xFFFF;
      if (px < 0 || py < 0 || px > sys_x || py > sys_y || px >= uFbSizeI.x || py >= uFbSizeI.y) {
        discard;
      }

      int ctrl = vP1.y & 0xFFFF;
      int pmod = (vP1.y >> 16) & 0xFFFF;
      int comm = ctrl & 0xF;
      bool clip = (pmod & 0x400) != 0;
      bool outside = (pmod & 0x200) != 0;
      if (clip) {
        int ux0 = vP2.x & 0xFFFF;
        int uy0 = (vP2.x >> 16) & 0xFFFF;
        int ux1 = vP2.y & 0xFFFF;
        int uy1 = (vP2.y >> 16) & 0xFFFF;
        bool inside = (px >= ux0 && px <= ux1 && py >= uy0 && py <= uy1);
        if (inside == outside) discard;
      }

      bool mesh = (pmod & 0x100) != 0;
      if (mesh && (((px ^ py) & 1) != 0)) {
        discard;
      }

      int primType = (vP1.z >> 16) & 3;
      int w = vP1.w & 0xFFFF;
      int h = (vP1.w >> 16) & 0xFFFF;
      bool textured = (comm < 4);
      int u = 0;
      int v = 0;
      int k = 0;
      int len = 0;

      if (primType == 2) {
        vec2 uv;
        if (!solve_quad_uv(
          vec2(float(px), float(py)),
          vec2(float(vP0.x), float(vP0.y)),
          vec2(float(vP0.z), float(vP0.w)),
          vec2(float(vP3.x), float(vP3.y)),
          vec2(float(vP3.z), float(vP3.w)),
          uv
        )) {
          discard;
        }
        if (textured) {
          u = clamp(int(floor(uv.x * float(w) * 0.9999)), 0, max(0, w - 1));
          v = clamp(int(floor(uv.y * float(h) * 0.9999)), 0, max(0, h - 1));
        }
      } else {
        int x0 = vP0.x;
        int y0 = vP0.y;
        int x1 = vP0.z;
        int y1 = vP0.w;
        int dx = x1 - x0;
        int dy = y1 - y0;
        int adx = abs(dx);
        int ady = abs(dy);
        int xi = (dx < 0) ? -1 : 1;
        int yi = (dy < 0) ? -1 : 1;

        if (primType == 0) {
          k = (px - x0) * xi;
          int i = (py - y0) * yi;
          if (k < 0 || k > adx || i < 0 || i > ady) discard;
          len = adx;
          if (textured) {
            u = (k * w) / (adx + 1);
            v = (i * h) / (ady + 1);
          }
        } else {
          len = max(adx, ady);
          if (adx >= ady) {
            k = (px - x0) * xi;
            if (k < 0 || k > adx) discard;
            int numB = 2 * k * ady - adx;
            int mBefore = (k == 0 || numB <= 0) ? 0 : ((numB - 1) / (2 * adx) + 1);
            int numA = 2 * (k + 1) * ady - adx;
            int mAfter = (k == adx || numA <= 0) ? mBefore : ((numA - 1) / (2 * adx) + 1);
            int dm = (py - y0) * yi;
            if (dm < mBefore || dm > mAfter) discard;
          } else {
            k = (py - y0) * yi;
            if (k < 0 || k > ady) discard;
            int numB = 2 * k * adx - ady;
            int mBefore = (k == 0 || numB <= 0) ? 0 : ((numB - 1) / (2 * ady) + 1);
            int numA = 2 * (k + 1) * adx - ady;
            int mAfter = (k == ady || numA <= 0) ? mBefore : ((numA - 1) / (2 * ady) + 1);
            int dm = (px - x0) * xi;
            if (dm < mBefore || dm > mAfter) discard;
          }
          v = (vP1.z >> 18) & 0x3FFF;
          if (textured) {
            u = (k * w) / (len + 1);
          }
        }
      }

      int colr = vP1.z & 0xFFFF;
      uint pix = uint(colr);
      if (textured) {
        int cmode = (pmod >> 3) & 7;
        bool spd = (pmod & 0x40) != 0;
        bool ecd = (pmod & 0x80) != 0;
        bool hflip = (ctrl & 0x10) != 0;
        bool vflip = (ctrl & 0x20) != 0;
        int s = sample_texel(vP1.x, w, h, cmode, colr, hflip, vflip, spd, ecd, u, v);
        if (s < 0) discard;
        pix = uint(s);
      }

      int ccb = pmod & 7;
      bool mon = (pmod & 0x8000) != 0;
      bool gouraud = (ccb & 4) != 0;

      if ((uTvmr & 1) != 0) {
        if (mon || gouraud || ((ccb & 3) != 0)) discard;
        emit_pix(pix & 0xFFu);
        return;
      }

      if (mon) {
        uint d = read_fb_copy(px, py);
        emit_pix(d | 0x8000u);
        return;
      }

      if (gouraud && ((pix & 0x8000u) != 0u)) {
        ivec3 g0 = rgb5_of(vP2.w & 0xFFFF);
        ivec3 g1 = rgb5_of((vP2.w >> 16) & 0xFFFF);
        ivec3 g = mix_rgb5(g0, g1, k, len);
        int r = clamp(int(pix & 0x1Fu) + g.x - 16, 0, 31);
        int gg = clamp(int((pix >> 5u) & 0x1Fu) + g.y - 16, 0, 31);
        int b = clamp(int((pix >> 10u) & 0x1Fu) + g.z - 16, 0, 31);
        pix = uint(0x8000 | (b << 10) | (gg << 5) | r);
      }

      int ccb3 = ccb & 3;
      if (ccb3 == 0) {
        emit_pix(pix);
      } else if (ccb3 == 1) {
        uint d = read_fb_copy(px, py);
        if ((d & 0x8000u) == 0u) discard;
        emit_pix(((d >> 1u) & 0x3DEFu) | 0x8000u);
      } else if (ccb3 == 2) {
        uint res = ((pix & 0x8000u) != 0u) ? (((pix >> 1u) & 0x3DEFu) | 0x8000u) : pix;
        emit_pix(res);
      } else {
        uint d = read_fb_copy(px, py);
        uint res = (((d & 0x8000u) != 0u) && ((pix & 0x8000u) != 0u))
          ? ((pix + d - ((pix ^ d) & 0x8421u)) >> 1u)
          : pix;
        emit_pix(res & 0xFFFFu);
      }
    }
    `;
  }

  function compileShader(gl, type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const info = (gl.getShaderInfoLog(sh) || 'unknown').trim().slice(0, 180);
      gl.deleteShader(sh);
      throw new Error(`VDP1 shader: ${info}`);
    }
    return sh;
  }

  function stepTo(a, b, i, n) {
    if (n <= 0) return a;
    const d = (b - a) * i * 2;
    return a + (d >= 0 ? Math.trunc((d + n) / (2 * n)) : -Math.trunc((-d + n) / (2 * n)));
  }

  function mixRgb5Packed(v0, v1, i, n) {
    if (n <= 0) return v0 & 0x7fff;
    const r0 = v0 & 0x1f, g0 = (v0 >> 5) & 0x1f, b0 = (v0 >> 10) & 0x1f;
    const r1 = v1 & 0x1f, g1 = (v1 >> 5) & 0x1f, b1 = (v1 >> 10) & 0x1f;
    const r = r0 + Math.trunc(((r1 - r0) * i) / n);
    const g = g0 + Math.trunc(((g1 - g0) * i) / n);
    const b = b0 + Math.trunc(((b1 - b0) * i) / n);
    return (r & 0x1f) | ((g & 0x1f) << 5) | ((b & 0x1f) << 10);
  }

  // Strictly convex quad with enough thickness (>= 3px edges) for 1-instance analytical bilinear UV
  function isConvexQuadForGpu(x0, y0, x1, y1, x2, y2, x3, y3) {
    const e0x = x1 - x0, e0y = y1 - y0;
    const e1x = x2 - x1, e1y = y2 - y1;
    const e2x = x3 - x2, e2y = y3 - y2;
    const e3x = x0 - x3, e3y = y0 - y3;
    const c0 = e0x * e1y - e0y * e1x;
    const c1 = e1x * e2y - e1y * e2x;
    const c2 = e2x * e3y - e2y * e3x;
    const c3 = e3x * e0y - e3y * e0x;
    const allPos = (c0 > 0 && c1 > 0 && c2 > 0 && c3 > 0);
    const allNeg = (c0 < 0 && c1 < 0 && c2 < 0 && c3 < 0);
    if (!allPos && !allNeg) return false;
    if (Math.abs(c0 + c2) < 16) return false;
    const m0 = Math.max(Math.abs(e0x), Math.abs(e0y));
    const m1 = Math.max(Math.abs(e1x), Math.abs(e1y));
    const m2 = Math.max(Math.abs(e2x), Math.abs(e2y));
    const m3 = Math.max(Math.abs(e3x), Math.abs(e3y));
    return (m0 >= 3 && m1 >= 3 && m2 >= 3 && m3 >= 3);
  }

  class TwcWebGLVdp1Renderer {
    constructor(gl, opts = {}) {
      this.gl = gl;
      this.ready = false;
      this.lastInitError = '';
      this.useRgba8Fbo = false;
      this.forceScanline = Boolean(opts.forceScanline);
      this.prog = null;
      this.vao = null;
      this.instVbo = null;
      this.fbo = null;
      this.vdp1VramTex = null;
      this.vdp1FbTex = null;
      this.vdp1FbCopyTex = null;
      this.vramShadow = new Uint8Array(524288);
      this.fbW = 0;
      this.fbH = 0;
      this.maxInstances = 8192;
      this.instBuf = new Int32Array(this.maxInstances * 16);
      this.instCount = 0;
      this._xs = new Int32Array(4);
      this._ys = new Int32Array(4);
      this._gs = new Int32Array(4);
      this.clearUintBuf = new Uint32Array(4);
      this.uLocFbSize = null;
      this.uLocFbSizeI = null;
      this.uLocTvmr = null;
      this.init();
    }

    isRgba8Fbo() {
      return this.useRgba8Fbo;
    }

    init() {
      const gl = this.gl;
      if (!gl) {
        this.lastInitError = 'no-gl';
        return;
      }
      try {
        const mkNearestTex = () => {
          const t = gl.createTexture();
          gl.bindTexture(gl.TEXTURE_2D, t);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
          return t;
        };

        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        this.vdp1VramTex = mkNearestTex();
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, 1024, 512, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, null);

        this.vdp1FbTex = mkNearestTex();
        this.vdp1FbCopyTex = mkNearestTex();
        this.fbo = gl.createFramebuffer();

        // Try R16UI FBO first; gracefully fall back to universally renderable RGBA8 packed FBO on strict mobile drivers
        this.useRgba8Fbo = false;
        this.ensureFbSize(512, 256);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
        let st = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        if (st !== gl.FRAMEBUFFER_COMPLETE) {
          this.useRgba8Fbo = true;
          this.fbW = 0;
          this.fbH = 0;
          this.ensureFbSize(512, 256);
          gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
          st = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          if (st !== gl.FRAMEBUFFER_COMPLETE) {
            throw new Error(`VDP1 FBO 0x${st.toString(16)}`);
          }
        }

        const vs = compileShader(gl, gl.VERTEX_SHADER, VS_VDP1);
        const fs = compileShader(gl, gl.FRAGMENT_SHADER, buildFsVdp1(this.useRgba8Fbo));
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          const info = (gl.getProgramInfoLog(prog) || 'VDP1 link failed').trim().slice(0, 180);
          gl.deleteShader(vs);
          gl.deleteShader(fs);
          gl.deleteProgram(prog);
          throw new Error(`VDP1 link: ${info}`);
        }
        gl.deleteShader(vs);
        gl.deleteShader(fs);

        this.prog = prog;
        this.uLocFbSize = gl.getUniformLocation(prog, 'uFbSize');
        this.uLocFbSizeI = gl.getUniformLocation(prog, 'uFbSizeI');
        this.uLocTvmr = gl.getUniformLocation(prog, 'uTvmr');
        gl.useProgram(prog);
        gl.uniform1i(gl.getUniformLocation(prog, 'uVdp1Vram'), 0);
        gl.uniform1i(gl.getUniformLocation(prog, 'uFbCopy'), 1);

        this.vao = gl.createVertexArray();
        this.instVbo = gl.createBuffer();
        gl.bindVertexArray(this.vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.instVbo);
        gl.bufferData(gl.ARRAY_BUFFER, this.instBuf.byteLength, gl.DYNAMIC_DRAW);

        const stride = 16 * 4;
        for (let loc = 0; loc < 4; loc++) {
          gl.enableVertexAttribArray(loc);
          gl.vertexAttribIPointer(loc, 4, gl.INT, stride, loc * 16);
          gl.vertexAttribDivisor(loc, 1);
        }
        gl.bindVertexArray(null);
        this.ready = true;
      } catch (err) {
        this.lastInitError = (err && err.message) ? err.message : String(err);
        console.warn('[TWC98 GPU-VDP1] Init fallback to CPU VDP1:', this.lastInitError);
        this.ready = false;
      }
    }

    ensureFbSize(w, h) {
      if (this.fbW === w && this.fbH === h) return;
      const gl = this.gl;
      this.fbW = w;
      this.fbH = h;
      if (this.useRgba8Fbo) {
        gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbCopyTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.vdp1FbTex, 0);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      } else {
        gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R16UI, w, h, 0, gl.RED_INTEGER, gl.UNSIGNED_SHORT, null);
        gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbCopyTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R16UI, w, h, 0, gl.RED_INTEGER, gl.UNSIGNED_SHORT, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.vdp1FbTex, 0);
        this.clearUintBuf[0] = 0;
        this.clearUintBuf[1] = 0;
        this.clearUintBuf[2] = 0;
        this.clearUintBuf[3] = 0;
        gl.clearBufferuiv(gl.COLOR, 0, this.clearUintBuf);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      }
    }

    ensureInstanceCap(needed) {
      if (needed <= this.maxInstances) return;
      let nextCap = this.maxInstances;
      while (nextCap < needed) nextCap *= 2;
      this.maxInstances = nextCap;
      const nextBuf = new Int32Array(nextCap * 16);
      nextBuf.set(this.instBuf);
      this.instBuf = nextBuf;
      const gl = this.gl;
      gl.bindBuffer(gl.ARRAY_BUFFER, this.instVbo);
      gl.bufferData(gl.ARRAY_BUFFER, this.instBuf.byteLength, gl.DYNAMIC_DRAW);
    }

    vramWord(addr) {
      const a = addr & 0x7fffe;
      return (this.vramShadow[a] << 8) | this.vramShadow[a + 1];
    }

    flushBatch() {
      if (this.instCount <= 0) return;
      const gl = this.gl;
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, this.instBuf, 0, this.instCount * 16);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, this.instCount);
      this.instCount = 0;
    }

    pushInstance(x0, y0, x1, y1, srca, ctrlPmod, colrPrimV, wh, uxy0, uxy1, sysXy, g01, x2 = 0, y2 = 0, x3 = 0, y3 = 0) {
      if (this.instCount >= this.maxInstances) {
        this.ensureInstanceCap(this.instCount + 1024);
      }
      const b = this.instCount * 16;
      const ib = this.instBuf;
      ib[b + 0] = x0;
      ib[b + 1] = y0;
      ib[b + 2] = x1;
      ib[b + 3] = y1;
      ib[b + 4] = srca;
      ib[b + 5] = ctrlPmod;
      ib[b + 6] = colrPrimV;
      ib[b + 7] = wh;
      ib[b + 8] = uxy0;
      ib[b + 9] = uxy1;
      ib[b + 10] = sysXy;
      ib[b + 11] = g01;
      ib[b + 12] = x2;
      ib[b + 13] = y2;
      ib[b + 14] = x3;
      ib[b + 15] = y3;
      this.instCount++;
    }

    uploadDirtyV1Vram(pktU8, pktI32 = null) {
      if (!this.ready || !pktU8 || pktU8.byteLength < 1024) return;
      const hdr = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, 256);
      if (hdr[0] !== 0x47564432) return;
      const flags0 = hdr[7] >>> 0;
      if ((flags0 & (1 << 25)) === 0) return;
      const dirtyMask = hdr[245] >>> 0;
      if (dirtyMask === 0) return;
      const v1VramOff = hdr[253] >>> 0;
      const gl = this.gl;
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.vdp1VramTex);
      if (dirtyMask === 0xffffffff) {
        this.vramShadow.set(pktU8.subarray(v1VramOff, v1VramOff + 524288));
        gl.texSubImage2D(
          gl.TEXTURE_2D, 0, 0, 0, 1024, 512, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
          pktU8, v1VramOff
        );
      } else {
        let curOff = v1VramOff;
        for (let c = 0; c < 32; c++) {
          if ((dirtyMask & (1 << c)) !== 0) {
            this.vramShadow.set(pktU8.subarray(curOff, curOff + 16384), c * 16384);
            gl.texSubImage2D(
              gl.TEXTURE_2D, 0, 0, c * 16, 1024, 16, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
              pktU8, curOff
            );
            curOff += 16384;
          }
        }
      }
      hdr[245] = 0;
    }

    renderFromPacket(pktU8, pktI32 = null) {
      if (!this.ready || !pktU8 || pktU8.byteLength < 1024) return null;
      const i32 = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, pktU8.byteLength >> 2);
      if (i32[0] !== 0x47564432) return null;
      const flags0 = i32[7] >>> 0;
      if ((flags0 & (1 << 25)) === 0) return null;

      if ((i32[245] >>> 0) !== 0) {
        this.uploadDirtyV1Vram(pktU8, i32);
      }

      const cmdCount = i32[246] | 0;
      const tvmr = i32[247] & 0xffff;
      const ewdr = (i32[247] >>> 16) & 0xffff;
      const ewX0 = (i32[248] << 16) >> 16;
      const ewY0 = i32[248] >> 16;
      const ewX1 = (i32[249] << 16) >> 16;
      const ewY1 = i32[249] >> 16;
      const fbW = (i32[250] | 0) || ((tvmr & 1) ? 1024 : 512);
      const fbH = (i32[251] | 0) || ((tvmr & 1) ? 512 : 256);
      const cmdsOffI32 = (i32[252] >>> 2);

      this.ensureFbSize(fbW, fbH);
      const gl = this.gl;

      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.viewport(0, 0, fbW, fbH);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.BLEND);
      gl.disable(gl.CULL_FACE);

      // 1. Hardware VDP1 Erase-Write region clear
      const ex0 = Math.max(0, ewX0);
      const ey0 = Math.max(0, ewY0);
      const ex1 = Math.min(fbW, ewX1);
      const ey1 = Math.min(fbH, ewY1);
      if (ex0 < ex1 && ey0 < ey1) {
        gl.enable(gl.SCISSOR_TEST);
        gl.scissor(ex0, ey0, ex1 - ex0, ey1 - ey0);
        if (this.useRgba8Fbo) {
          gl.clearColor((ewdr & 0xff) / 255.0, ((ewdr >> 8) & 0xff) / 255.0, 0.0, 1.0);
          gl.clear(gl.COLOR_BUFFER_BIT);
        } else {
          this.clearUintBuf[0] = ewdr >>> 0;
          this.clearUintBuf[1] = 0;
          this.clearUintBuf[2] = 0;
          this.clearUintBuf[3] = 0;
          gl.clearBufferuiv(gl.COLOR, 0, this.clearUintBuf);
        }
        gl.disable(gl.SCISSOR_TEST);
      }

      if (cmdCount <= 0) {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        return this.vdp1FbTex;
      }

      gl.useProgram(this.prog);
      gl.bindVertexArray(this.vao);
      gl.uniform2f(this.uLocFbSize, fbW, fbH);
      gl.uniform2i(this.uLocFbSizeI, fbW, fbH);
      gl.uniform1i(this.uLocTvmr, tvmr);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.vdp1VramTex);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbCopyTex);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.instVbo);

      const is8BitFb = (tvmr & 1) !== 0;
      this.instCount = 0;
      const xs = this._xs;
      const ys = this._ys;
      const gs = this._gs;

      for (let idx = 0; idx < cmdCount; idx++) {
        const cb = cmdsOffI32 + idx * 12;
        const ctrlPmod = i32[cb + 0];
        const ctrl = ctrlPmod & 0xffff;
        const pmod = (ctrlPmod >>> 16) & 0xffff;
        const colr = i32[cb + 1] & 0xffff;
        const srca = i32[cb + 2];
        const gAddr = i32[cb + 3];
        const wh = i32[cb + 4];
        const h = wh >> 16;
        const x01 = i32[cb + 5];
        const x0 = (x01 << 16) >> 16;
        const x1 = x01 >> 16;
        const x23 = i32[cb + 6];
        const x2 = (x23 << 16) >> 16;
        const x3 = x23 >> 16;
        const y01 = i32[cb + 7];
        const y0 = (y01 << 16) >> 16;
        const y1 = y01 >> 16;
        const y23 = i32[cb + 8];
        const y2 = (y23 << 16) >> 16;
        const y3 = y23 >> 16;
        const uxy0 = i32[cb + 9];
        const uxy1 = i32[cb + 10];
        const sysXy = i32[cb + 11];

        const comm = ctrl & 0xf;
        const ccb = pmod & 7;
        const ccb3 = ccb & 3;
        const mon = (pmod & 0x8000) !== 0;
        const gouraud = (ccb & 4) !== 0;
        const textured = comm < 4;

        const needsFbRead = !is8BitFb && (mon || ccb3 === 1 || ccb3 === 3);
        if (needsFbRead) {
          this.flushBatch();
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbCopyTex);
          gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, fbW, fbH);
        }

        let g0 = 0, g1 = 0, g2 = 0, g3 = 0;
        if (gouraud) {
          g0 = this.vramWord(gAddr + 0);
          g1 = this.vramWord(gAddr + 2);
          g2 = this.vramWord(gAddr + 4);
          g3 = this.vramWord(gAddr + 6);
        }

        if (comm <= 4) {
          const isAxisBox = !gouraud && !needsFbRead && (y0 === y1) && (y2 === y3) && (x0 === x3) && (x1 === x2);
          if (isAxisBox) {
            this.pushInstance(x0, y0, x2, y2, srca, ctrlPmod, colr, wh, uxy0, uxy1, sysXy, 0);
          } else if (!this.forceScanline && !gouraud && isConvexQuadForGpu(x0, y0, x1, y1, x2, y2, x3, y3)) {
            this.pushInstance(x0, y0, x1, y1, srca, ctrlPmod, colr | (2 << 16), wh, uxy0, uxy1, sysXy, 0, x2, y2, x3, y3);
          } else {
            const left = Math.max(Math.abs(x3 - x0), Math.abs(y3 - y0));
            const right = Math.max(Math.abs(x2 - x1), Math.abs(y2 - y1));
            const n = Math.max(left, right);
            if (n <= 2048) {
              for (let i = 0; i <= n; i++) {
                const lx = stepTo(x0, x3, i, n);
                const ly = stepTo(y0, y3, i, n);
                const rx = stepTo(x1, x2, i, n);
                const ry = stepTo(y1, y2, i, n);
                const v = textured ? Math.trunc((i * h) / (n + 1)) : 0;
                const colrPrimV = colr | (1 << 16) | ((v & 0x3fff) << 18);
                let g01 = 0;
                if (gouraud) {
                  const glVal = mixRgb5Packed(g0, g3, i, n);
                  const grVal = mixRgb5Packed(g1, g2, i, n);
                  g01 = glVal | (grVal << 16);
                }
                this.pushInstance(lx, ly, rx, ry, srca, ctrlPmod, colrPrimV, wh, uxy0, uxy1, sysXy, g01);
              }
            }
          }
        } else if (comm === 5 || comm === 7) {
          xs[0] = x0; xs[1] = x1; xs[2] = x2; xs[3] = x3;
          ys[0] = y0; ys[1] = y1; ys[2] = y2; ys[3] = y3;
          gs[0] = g0; gs[1] = g1; gs[2] = g2; gs[3] = g3;
          const colrPrimV = colr | (1 << 16);
          for (let i = 0; i < 4; i++) {
            const ni = (i + 1) & 3;
            const g01 = gouraud ? ((gs[i] & 0xffff) | ((gs[ni] & 0xffff) << 16)) : 0;
            this.pushInstance(xs[i], ys[i], xs[ni], ys[ni], srca, ctrlPmod, colrPrimV, wh, uxy0, uxy1, sysXy, g01);
          }
        } else if (comm === 6) {
          const colrPrimV = colr | (1 << 16);
          const g01 = gouraud ? ((g0 & 0xffff) | ((g1 & 0xffff) << 16)) : 0;
          this.pushInstance(x0, y0, x1, y1, srca, ctrlPmod, colrPrimV, wh, uxy0, uxy1, sysXy, g01);
        }

        if (needsFbRead) {
          this.flushBatch();
        }
      }

      this.flushBatch();
      gl.bindVertexArray(null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.activeTexture(gl.TEXTURE0);
      return this.vdp1FbTex;
    }
  }

  if (typeof window !== 'undefined') {
    window.TwcWebGLVdp1Renderer = TwcWebGLVdp1Renderer;
  } else {
    self.TwcWebGLVdp1Renderer = TwcWebGLVdp1Renderer;
  }
})();
