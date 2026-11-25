// TWC98BO WebGL2 GPU Presenter (paridad visual B-QA-03 con twc98_gpu_d3d11.hpp).
// Filtros: Nearest, Bilinear, Sharp-Bilinear y CRT-Arcade + Máscara CRT configurable.

(() => {
  const VS_SRC = `#version 300 es
    out vec2 vUv;
    const vec2 POS[4] = vec2[4](vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(-1.0,1.0), vec2(1.0,1.0));
    const vec2 UVS[4] = vec2[4](vec2(0.0,1.0), vec2(1.0,1.0), vec2(0.0,0.0), vec2(1.0,0.0));
    void main() {
      vUv = UVS[gl_VertexID];
      gl_Position = vec4(POS[gl_VertexID], 0.0, 1.0);
    }
  `;

  const FS_SRC = `#version 300 es
    precision highp float;
    in vec2 vUv;
    out vec4 fragColor;
    uniform sampler2D uTex;
    uniform vec2 uTexSize;
    uniform vec2 uOutSize;
    uniform int uFilterMode; // 0=Nearest, 1=Bilinear, 2=Sharp-Bi, 3=CRT-Arcade, 4=Suave-xBR
    uniform int uCrtMask;
    uniform int uHdBoost; // 1=CAS-style bounded edge sharpening (Modo HD)

    vec3 sampleSharp(vec2 uv) {
      vec2 texel = uv * uTexSize;
      vec2 scale = max(vec2(1.0), uOutSize / uTexSize);
      vec2 f = fract(texel - 0.5);
      vec2 i = floor(texel - 0.5);
      vec2 w = clamp((f - 0.5) * scale + 0.5, 0.0, 1.0);
      vec2 finalUv = (i + w + 0.5) / uTexSize;
      return texture(uTex, clamp(finalUv, vec2(0.0), vec2(1.0))).rgb;
    }

    vec3 sampleXbrLite(vec2 uv) {
      vec2 invTex = 1.0 / max(vec2(1.0), uTexSize);
      vec2 tc = floor(uv * uTexSize);
      vec2 d = (uv * uTexSize) - (tc + 0.5); // strictly [-0.5, 0.5] around texel tc

      vec3 c  = texture(uTex, clamp((tc + vec2( 0.5,  0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 t  = texture(uTex, clamp((tc + vec2( 0.5, -0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 b  = texture(uTex, clamp((tc + vec2( 0.5,  1.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 l  = texture(uTex, clamp((tc + vec2(-0.5,  0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 r  = texture(uTex, clamp((tc + vec2( 1.5,  0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 tl = texture(uTex, clamp((tc + vec2(-0.5, -0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 tr = texture(uTex, clamp((tc + vec2( 1.5, -0.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 bl = texture(uTex, clamp((tc + vec2(-0.5,  1.5)) * invTex, 0.0, 1.0)).rgb;
      vec3 br = texture(uTex, clamp((tc + vec2( 1.5,  1.5)) * invTex, 0.0, 1.0)).rgb;

      // 1. Anti-checkerboard & 15-bit debanding on smooth/mesh regions
      vec3 avgCross = (t + b + l + r) * 0.25;
      float edgeMag = max(max(length(t - c), length(b - c)), max(length(l - c), length(r - c)));
      float meshLike = step(length(t - b) + length(l - r), 0.16) * step(0.02, length(c - avgCross));
      float smoothDeband = (1.0 - smoothstep(0.06, 0.22, edgeMag)) * 0.26;
      float meshDeband = meshLike * (1.0 - smoothstep(0.18, 0.45, edgeMag)) * 0.44;
      vec3 centerClean = mix(c, avgCross, clamp(smoothDeband + meshDeband, 0.0, 0.52));

      // 2. Quadrant-local bilateral smoothing + xBR-style diagonal bevel
      vec3 hNbr = (d.x < 0.0) ? l : r;
      vec3 vNbr = (d.y < 0.0) ? t : b;
      vec3 dNbr = (d.x < 0.0) ? ((d.y < 0.0) ? tl : bl) : ((d.y < 0.0) ? tr : br);

      float wh = abs(d.x) * exp(-10.0 * length(c - hNbr));
      float wv = abs(d.y) * exp(-10.0 * length(c - vNbr));
      float wd = (abs(d.x) * abs(d.y)) * exp(-10.0 * length(c - dNbr));
      float wSum = 1.0 + wh + wv + wd;
      vec3 bilat = (centerClean + hNbr * wh + vNbr * wv + dNbr * wd) / wSum;

      // Diagonal corner bevel when hNbr & vNbr share contour across corner
      float sameCorner = exp(-16.0 * length(hNbr - vNbr));
      float diffCenter = smoothstep(0.06, 0.24, length(c - hNbr));
      vec2 scale = clamp(uOutSize * invTex, vec2(1.0), vec2(10.0));
      float aa = 0.85 / max(scale.x, scale.y);
      float bevel = smoothstep(-aa, aa, abs(d.x) + abs(d.y) - 0.5) * sameCorner * diffCenter;
      vec3 outCol = mix(bilat, (hNbr + vNbr) * 0.5, clamp(bevel * 0.92, 0.0, 0.92));

      vec3 mn = min(min(min(t, b), min(l, r)), min(min(tl, tr), min(bl, br)));
      vec3 mx = max(max(max(t, b), max(l, r)), max(max(tl, tr), max(bl, br)));
      return clamp(outCol, min(mn, c), max(mx, c));
    }

    float lumaOf(vec3 c) {
      return dot(c, vec3(0.299, 0.587, 0.114));
    }

    // Ultra HD (Modo HD): FSR-inspired EASU directional AA + FXAA 3.11 + RCAS + 15-bit debanding
    vec3 applyUltraHdPipeline(vec2 uv, vec3 baseCol) {
      vec2 invTex = 1.0 / max(vec2(1.0), uTexSize);
      vec3 tl = texture(uTex, clamp(uv + vec2(-invTex.x, -invTex.y), 0.0, 1.0)).rgb;
      vec3 t  = texture(uTex, clamp(uv + vec2(      0.0, -invTex.y), 0.0, 1.0)).rgb;
      vec3 tr = texture(uTex, clamp(uv + vec2( invTex.x, -invTex.y), 0.0, 1.0)).rgb;
      vec3 l  = texture(uTex, clamp(uv + vec2(-invTex.x,       0.0), 0.0, 1.0)).rgb;
      vec3 r  = texture(uTex, clamp(uv + vec2( invTex.x,       0.0), 0.0, 1.0)).rgb;
      vec3 bl = texture(uTex, clamp(uv + vec2(-invTex.x,  invTex.y), 0.0, 1.0)).rgb;
      vec3 b  = texture(uTex, clamp(uv + vec2(      0.0,  invTex.y), 0.0, 1.0)).rgb;
      vec3 br = texture(uTex, clamp(uv + vec2( invTex.x,  invTex.y), 0.0, 1.0)).rgb;

      float lC  = lumaOf(baseCol);
      float lTL = lumaOf(tl), lT = lumaOf(t), lTR = lumaOf(tr);
      float lL  = lumaOf(l),                  lR  = lumaOf(r);
      float lBL = lumaOf(bl), lB = lumaOf(b), lBR = lumaOf(br);

      float lMin = min(lC, min(min(lT, lB), min(lL, lR)));
      float lMax = max(lC, max(max(lT, lB), max(lL, lR)));
      float lRange = lMax - lMin;

      vec3 col = baseCol;

      // 1. 15-bit (RGB555) -> 24-bit Debanding on smooth/gradient regions
      if (lRange < 0.065) {
        vec3 avg4 = (t + b + l + r) * 0.25;
        float debandW = (1.0 - smoothstep(0.015, 0.065, lRange)) * 0.35;
        col = mix(col, avg4, debandW);
        // Subtle 2x2 ordered sub-LSB dither to hide 8-bit quantization
        float dither = (mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y) * 2.0, 4.0) - 1.5) * (0.35 / 255.0);
        col = clamp(col + dither, 0.0, 1.0);
      }

      // 2. EASU + FXAA 3.11 directional edge smoothing on high-contrast diagonal edges
      if (lRange >= 0.045) {
        vec2 dir;
        dir.x = -((lTL + lTR) - (lBL + lBR));
        dir.y =  ((lTL + lBL) - (lTR + lBR));
        float dirReduce = max((lTL + lTR + lBL + lBR) * 0.03125, 0.0078125);
        float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
        dir = clamp(dir * rcpDirMin, vec2(-4.0), vec2(4.0)) * invTex;

        vec3 rgbA = 0.5 * (
          texture(uTex, clamp(uv + dir * (1.0 / 3.0 - 0.5), 0.0, 1.0)).rgb +
          texture(uTex, clamp(uv + dir * (2.0 / 3.0 - 0.5), 0.0, 1.0)).rgb
        );
        vec3 rgbB = rgbA * 0.5 + 0.25 * (
          texture(uTex, clamp(uv + dir * -0.5, 0.0, 1.0)).rgb +
          texture(uTex, clamp(uv + dir *  0.5, 0.0, 1.0)).rgb
        );
        float lB2 = lumaOf(rgbB);
        vec3 aaCol = (lB2 < lMin || lB2 > lMax) ? rgbA : rgbB;
        float aaBlend = smoothstep(0.045, 0.22, lRange) * 0.72;
        col = mix(col, aaCol, aaBlend);
      }

      // 3. AMD RCAS (Robust Contrast-Adaptive Sharpening, zero ringing)
      vec3 mn4 = min(col, min(min(t, b), min(l, r)));
      vec3 mx4 = max(col, max(max(t, b), max(l, r)));
      vec3 amp = clamp(min(mn4, 1.0 - mx4) / max(mx4, vec3(0.0001)), 0.0, 1.0);
      vec3 wRcas = sqrt(amp) * ((uFilterMode == 4) ? -0.10 : -0.15);
      vec3 sharp = (col + (t + b + l + r) * wRcas) / max(vec3(0.25), 1.0 + 4.0 * wRcas);
      return clamp(sharp, mn4, mx4);
    }

    void main() {
      vec3 col;
      if (uFilterMode == 0) {
        vec2 nearestUv = (floor(vUv * uTexSize) + 0.5) / uTexSize;
        col = texture(uTex, nearestUv).rgb;
      } else if (uFilterMode == 1) {
        col = texture(uTex, vUv).rgb;
      } else if (uFilterMode == 4) {
        col = sampleXbrLite(vUv);
      } else {
        col = sampleSharp(vUv);
      }

      if (uHdBoost != 0 && uFilterMode != 0) {
        col = applyUltraHdPipeline(vUv, col);
      }

      if (uFilterMode == 3 && uCrtMask != 0) {
        // Scanline horizontal sutil adaptada a 448p / 224p + máscara RGB ligera
        float yLines = uTexSize.y >= 360.0 ? 224.0 : uTexSize.y;
        float scanPhase = sin(vUv.y * yLines * 6.2831853);
        float scanMul = 0.90 + 0.10 * scanPhase;
        float px = mod(floor(gl_FragCoord.x), 3.0);
        vec3 grille = (px < 0.5) ? vec3(1.03, 0.98, 0.98)
                    : ((px < 1.5) ? vec3(0.98, 1.03, 0.98) : vec3(0.98, 0.98, 1.03));
        col *= scanMul * grille;
      }
      fragColor = vec4(col, 1.0);
    }
  `;

  function compileShader(gl, type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const info = gl.getShaderInfoLog(sh);
      gl.deleteShader(sh);
      throw new Error(`WebGL shader error: ${info}`);
    }
    return sh;
  }

  class TwcWebGLPresenter {
    constructor(canvas, opts = {}) {
      this.canvas = canvas;
      this.gl = null;
      this.ctx2d = null;
      this.imgData = null;
      this.prog = null;
      this.tex = null;
      this.vao = null;
      this.texW = 0;
      this.texH = 0;
      this.filterMode = 3; // 3 = CRT-Arcade default like PC
      this.crtMask = true;
      this.hdMode = false;
      this.useGpu = true;
      this.backendName = 'CPU-2D';
      const hasWindow = (typeof window !== 'undefined');
      this.isMobile = (typeof opts.isMobile === 'boolean') ? opts.isMobile : (
        (hasWindow && window.TwcPerfProfile && typeof window.TwcPerfProfile.detectMobileDevice === 'function')
          ? window.TwcPerfProfile.detectMobileDevice()
          : (hasWindow && (
            /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent || '') ||
            (window.matchMedia && window.matchMedia('(pointer: coarse)').matches && window.innerWidth <= 1100)
          ))
      );
      this.filterMode = this.isMobile ? 1 : 3; // 1 = fast Linear on mobile, 3 = CRT-Arcade on desktop
      this.crtMask = !this.isMobile;
      this.isSoftwareRenderer = Boolean(opts.isSoftwareRenderer);
      this.isIntegratedGpu = Boolean(opts.isIntegratedGpu);
      this.maxDpr = (this.isSoftwareRenderer || this.isMobile) ? 1.0 : (this.isIntegratedGpu ? 1.5 : 2);
      this.cachedW = (opts.initW | 0) || 704;
      this.cachedH = (opts.initH | 0) || 448;
      this.dirtySize = hasWindow && (typeof canvas.getBoundingClientRect === 'function');
      this.glRendererStr = '';
      this.lastInitError = '';
      this.vdp2InitError = '';
      this.vdp1InitError = '';
      this.initContext(opts);
      this.observeResize();
    }

    observeResize() {
      const hasWindow = (typeof window !== 'undefined');
      if (!hasWindow || typeof this.canvas.getBoundingClientRect !== 'function') return;
      const markDirty = () => { this.dirtySize = true; };
      if (typeof ResizeObserver !== 'undefined') {
        this.resizeObs = new ResizeObserver(markDirty);
        this.resizeObs.observe(this.canvas);
      }
      window.addEventListener('resize', markDirty, { passive: true });
    }

    setDisplaySize(w, h) {
      const minW = (this.isMobile || this.isSoftwareRenderer) ? 352 : 704;
      const minH = (this.isMobile || this.isSoftwareRenderer) ? 224 : 448;
      this.cachedW = Math.max(minW, w | 0);
      this.cachedH = Math.max(minH, h | 0);
      this.dirtySize = false;
    }

    invalidateSize() {
      if (typeof window !== 'undefined' && typeof this.canvas.getBoundingClientRect === 'function') {
        this.dirtySize = true;
      }
    }

    initContext(opts = {}) {
      try {
        const gl = this.canvas.getContext('webgl2', {
          alpha: false,
          antialias: false,
          desynchronized: false,
          powerPreference: 'high-performance',
        });
        if (!gl) throw new Error('No WebGL2');
        try {
          const dbgExt = gl.getExtension('WEBGL_debug_renderer_info');
          const rendererStr = dbgExt ? String(gl.getParameter(dbgExt.UNMASKED_RENDERER_WEBGL) || '') : '';
          this.glRendererStr = rendererStr;
          if (/SwiftShader|llvmpipe|Microsoft Basic Render|Software/i.test(rendererStr)) {
            this.isSoftwareRenderer = true;
            this.maxDpr = 1;
          } else if (!this.isMobile && /Intel|UHD Graphics|Iris|HD Graphics|Radeon.*Graphics|Vega|Apple GPU/i.test(rendererStr) && !/NVIDIA|GeForce|RTX|GTX|Radeon RX|Quadro|Arc\s*A/i.test(rendererStr)) {
            this.isIntegratedGpu = true;
            if (!this.hdMode) this.maxDpr = 1.5;
          }
        } catch {}
        const vs = compileShader(gl, gl.VERTEX_SHADER, VS_SRC);
        const fs = compileShader(gl, gl.FRAGMENT_SHADER, FS_SRC);
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          throw new Error(gl.getProgramInfoLog(prog) || 'WebGL link error');
        }
        gl.deleteShader(vs);
        gl.deleteShader(fs);

        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

        this.gl = gl;
        this.prog = prog;
        this.tex = tex;
        this.vao = gl.createVertexArray();
        this.uLocTex = gl.getUniformLocation(prog, 'uTex');
        this.uLocTexSize = gl.getUniformLocation(prog, 'uTexSize');
        this.uLocOutSize = gl.getUniformLocation(prog, 'uOutSize');
        this.uLocFilter = gl.getUniformLocation(prog, 'uFilterMode');
        this.uLocCrt = gl.getUniformLocation(prog, 'uCrtMask');
        this.uLocHdBoost = gl.getUniformLocation(prog, 'uHdBoost');
        this.backendName = 'WebGL2';
        this.vdp2Comp = null;
        this.vdp1Ren = null;
        this.copaVfx = null;
        this.lastPresentedTex = null;
        this.lastPresentedW = 0;
        this.lastPresentedH = 0;
        const Vdp2Ctor = (typeof window !== 'undefined' ? window.TwcWebGLVdp2Compositor : self.TwcWebGLVdp2Compositor);
        if (Vdp2Ctor && (!this.isSoftwareRenderer || opts.allowSoftwareVdp2)) {
          const comp = new Vdp2Ctor(gl);
          if (comp.ready) {
            this.vdp2Comp = comp;
          } else {
            this.vdp2InitError = comp.lastInitError || 'vdp2-not-ready';
          }
        } else if (this.isSoftwareRenderer && !opts.allowSoftwareVdp2) {
          this.vdp2InitError = 'sw-gpu';
        }
        const Vdp1Ctor = (typeof window !== 'undefined' ? window.TwcWebGLVdp1Renderer : self.TwcWebGLVdp1Renderer);
        if (Vdp1Ctor && this.vdp2Comp && (!this.isSoftwareRenderer || opts.allowSoftwareVdp2)) {
          const v1 = new Vdp1Ctor(gl, { forceScanline: Boolean(opts.forceScanlineVdp1) });
          if (v1.ready) {
            this.vdp1Ren = v1;
          } else {
            this.vdp1InitError = v1.lastInitError || 'vdp1-not-ready';
          }
        }
        const CopaVfxCtor = (typeof window !== 'undefined' ? window.TwcWebGLCopaVfx : self.TwcWebGLCopaVfx);
        if (CopaVfxCtor && this.vdp2Comp) {
          this.copaVfx = new CopaVfxCtor(gl);
        }
      } catch (err) {
        this.lastInitError = (err && err.message) ? err.message : String(err);
        this.gl = null;
        this.vdp2Comp = null;
        this.vdp1Ren = null;
        this.useGpu = false;
        this.ctx2d = this.canvas.getContext('2d', { alpha: false });
        this.backendName = 'CPU-2D';
      }
    }

    setFilterMode(mode) {
      this.filterMode = Math.max(0, Math.min(4, mode | 0));
    }

    setCrtMask(enabled) {
      this.crtMask = Boolean(enabled);
    }

    setHdMode(enabled) {
      this.hdMode = Boolean(enabled);
      const isDedicated = !this.isMobile && !this.isIntegratedGpu && !this.isSoftwareRenderer;
      this.maxDpr = this.isSoftwareRenderer ? 1 : (this.hdMode ? (isDedicated ? 3.0 : (this.isMobile ? 1.5 : 2.0)) : (this.isMobile ? 1.0 : (this.isIntegratedGpu ? 1.5 : 2)));
      if (this.vdp2Comp && typeof this.vdp2Comp.setQuality === 'function') {
        const ultraScale = (this.hdMode && isDedicated) ? 2 : 1;
        this.vdp2Comp.setQuality(ultraScale, this.hdMode);
      }
      this.invalidateSize();
    }

    getUltraScale() {
      return (this.vdp2Comp && this.vdp2Comp.ultraScale) ? this.vdp2Comp.ultraScale : 1;
    }

    resizeCanvasToDisplay() {
      if (this.dirtySize && typeof this.canvas.getBoundingClientRect === 'function') {
        const rect = this.canvas.getBoundingClientRect();
        const dpr = Math.min(this.maxDpr, (typeof window !== 'undefined' ? window.devicePixelRatio : 1) || 1);
        const minW = (this.isMobile || this.isSoftwareRenderer) ? 352 : 704;
        const minH = (this.isMobile || this.isSoftwareRenderer) ? 224 : 448;
        this.cachedW = Math.max(minW, Math.round(rect.width * dpr));
        this.cachedH = Math.max(minH, Math.round(rect.height * dpr));
        this.dirtySize = false;
      }
      if (this.canvas.width !== this.cachedW || this.canvas.height !== this.cachedH) {
        this.canvas.width = this.cachedW;
        this.canvas.height = this.cachedH;
      }
    }

    setUseGpu(enabled) {
      this.useGpu = Boolean(enabled);
    }

    hasGpuVdp2() {
      return Boolean(this.gl && this.vdp2Comp && this.vdp2Comp.ready);
    }

    hasGpuVdp1() {
      return Boolean(this.gl && this.vdp2Comp && this.vdp2Comp.ready && this.vdp1Ren && this.vdp1Ren.ready);
    }

    getGpuDiagTag() {
      const clean = (s) => String(s || '').replace(/\s+/g, '_').slice(0, 40);
      const hdrTag = (this.vdp2Comp && this.vdp2Comp.ready) ? (this.vdp2Comp.useUbo ? '+ubo' : '+tex') : '';
      if (this.hasGpuVdp1()) {
        const fbTag = (typeof this.vdp1Ren.isRgba8Fbo === 'function' && this.vdp1Ren.isRgba8Fbo()) ? 'v1:rgba8' : 'ok';
        return `${fbTag}${hdrTag}`;
      }
      if (this.hasGpuVdp2()) {
        return `v1err:${clean(this.vdp1InitError || 'off')}${hdrTag}`;
      }
      const err = clean(this.vdp2InitError || this.lastInitError || 'no-gl');
      const ren = clean(this.glRendererStr);
      return ren ? `${err}(${ren.slice(0, 22)})` : err;
    }

    drawScreenTexture(srcTex, srcW, srcH) {
      if (!this.gl || !srcTex || srcW <= 0 || srcH <= 0) return;
      const gl = this.gl;
      this.resizeCanvasToDisplay();
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, this.canvas.width, this.canvas.height);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, srcTex);

      const effMode = this.useGpu ? this.filterMode : 0;
      const effCrt = (this.useGpu && this.crtMask) ? 1 : 0;
      const texFilter = (effMode === 0) ? gl.NEAREST : gl.LINEAR;
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, texFilter);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, texFilter);

      gl.useProgram(this.prog);
      gl.bindVertexArray(this.vao);
      if (this.uLocTex) gl.uniform1i(this.uLocTex, 0);
      gl.uniform2f(this.uLocTexSize, srcW, srcH);
      gl.uniform2f(this.uLocOutSize, this.canvas.width, this.canvas.height);
      gl.uniform1i(this.uLocFilter, effMode);
      gl.uniform1i(this.uLocCrt, effCrt);
      if (this.uLocHdBoost) gl.uniform1i(this.uLocHdBoost, (this.useGpu && this.hdMode) ? 1 : 0);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      this.lastPresentedTex = srcTex;
      this.lastPresentedW = srcW;
      this.lastPresentedH = srcH;
    }

    uploadGpuVdp2Vram(pktU8, pktI32 = null) {
      if (!pktU8 || pktU8.byteLength < 1024) return;
      const i32 = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, pktU8.byteLength >> 2);
      if (this.hasGpuVdp2()) {
        this.vdp2Comp.uploadDirtyVram(pktU8, i32);
      }
      if (this.hasGpuVdp1()) {
        this.vdp1Ren.uploadDirtyV1Vram(pktU8, i32);
      }
    }

    presentGpuVdp2(pktU8, pktI32 = null) {
      if (!this.hasGpuVdp2() || !pktU8 || pktU8.byteLength < 1024) return false;
      const i32 = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, pktU8.byteLength >> 2);
      let extV1Tex = null;
      let extV1IsRgba8 = false;
      if (this.hasGpuVdp1()) {
        if (i32[0] === 0x47564432 && ((i32[7] >>> 0) & (1 << 25)) !== 0) {
          extV1Tex = this.vdp1Ren.renderFromPacket(pktU8, i32);
          if (!extV1Tex) return false;
          extV1IsRgba8 = typeof this.vdp1Ren.isRgba8Fbo === 'function' && this.vdp1Ren.isRgba8Fbo();
        }
      }
      const out = this.vdp2Comp.compose(pktU8, extV1Tex, extV1IsRgba8, i32);
      if (!out) return false;
      if (this.copaVfx && ((i32[216] & 1) !== 0)) {
        this.copaVfx.drawBoliviaWind(this.vdp2Comp.fbo, out.w, out.h, i32);
      }
      this.drawScreenTexture(out.tex, out.w, out.h);
      return true;
    }

    repaintLast() {
      if (this.gl && this.lastPresentedTex && this.lastPresentedW > 0 && this.lastPresentedH > 0) {
        this.drawScreenTexture(this.lastPresentedTex, this.lastPresentedW, this.lastPresentedH);
        return true;
      }
      return false;
    }

    readGpuVdp2Rgba() {
      return this.hasGpuVdp2() ? this.vdp2Comp.readPixelsRgba() : null;
    }

    present(rgbaU8, srcW, srcH) {
      if (!rgbaU8 || srcW <= 0 || srcH <= 0) return;

      if (this.gl) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.tex);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        if (this.texW !== srcW || this.texH !== srcH) {
          this.texW = srcW;
          this.texH = srcH;
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, srcW, srcH, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgbaU8);
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, srcW, srcH, gl.RGBA, gl.UNSIGNED_BYTE, rgbaU8);
        }
        this.drawScreenTexture(this.tex, srcW, srcH);
        return;
      }

      if (!this.ctx2d) this.ctx2d = this.canvas.getContext('2d', { alpha: false });
      if (!this.ctx2d) return;
      if (this.canvas.width !== srcW || this.canvas.height !== srcH) {
        this.canvas.width = srcW;
        this.canvas.height = srcH;
      }
      if (!this.imgData || this.imgData.width !== srcW || this.imgData.height !== srcH) {
        this.imgData = this.ctx2d.createImageData(srcW, srcH);
      }
      this.imgData.data.set(rgbaU8);
      this.ctx2d.putImageData(this.imgData, 0, 0);
    }
  }

  if (typeof window !== 'undefined') {
    window.TwcWebGLPresenter = TwcWebGLPresenter;
  } else {
    self.TwcWebGLPresenter = TwcWebGLPresenter;
  }
})();
