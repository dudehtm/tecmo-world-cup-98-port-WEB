// TWC98BO WebGL2 Hardware GPU VDP2 Compositor (Modular Lite + Lazy Ultra).
// Consumes TwcWebGLVdp2Shaders so low-end OpenGL ES 3.0 mobile GPUs only compile the
// lightweight 'lite' shader (with RGBA32I header texture, zero dynamic uniform arrays),
// while 'ultra' (Ultra HD 2x + RGSS + De-Mesh + Scale2x) compiles lazily when Modo HD is enabled.

(() => {
  function compileShader(gl, type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const info = (gl.getShaderInfoLog(sh) || 'unknown').trim().slice(0, 180);
      gl.deleteShader(sh);
      throw new Error(`VDP2 shader: ${info}`);
    }
    return sh;
  }

  class TwcWebGLVdp2Compositor {
    constructor(gl) {
      this.gl = gl;
      this.ready = false;
      this.lastInitError = '';
      this.programs = Object.create(null); // key: `${ultra ? 'u' : 'l'}_${vdp1Rgba8 ? '8' : '16'}`
      this.failedVariants = Object.create(null);
      this.vao = null;
      this.fbo = null;
      this.outTex = null;
      this.outW = 0;
      this.outH = 0;
      this.vdp2VramTex = null;
      this.cramTex = null;
      this.vdp1FbTex = null;
      this.fbW = 0;
      this.fbH = 0;
      this.lineTex = null;
      this.lineH = 0;
      this.hdrTex = null;
      this.hdrUbo = null;
      this.useUbo = true;
      this.ultraScale = 1;
      this.deMesh = false;
      this.init();
    }

    getShaderBuilder() {
      return (typeof window !== 'undefined' ? window.TwcWebGLVdp2Shaders : self.TwcWebGLVdp2Shaders) || null;
    }

    compileVariant(ultra, vdp1Rgba8) {
      const key = `${ultra ? 'u' : 'l'}_${vdp1Rgba8 ? '8' : '16'}_${this.useUbo ? 'ubo' : 'tex'}`;
      if (this.programs[key]) return this.programs[key];
      if (this.failedVariants[key]) return null;
      const builder = this.getShaderBuilder();
      if (!builder) throw new Error('Missing TwcWebGLVdp2Shaders');
      const gl = this.gl;
      try {
        const vs = compileShader(gl, gl.VERTEX_SHADER, builder.VS_VDP2);
        const fs = compileShader(gl, gl.FRAGMENT_SHADER, builder.buildFragmentSource({ ultra, vdp1Rgba8, useUbo: this.useUbo }));
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          const info = (gl.getProgramInfoLog(prog) || 'VDP2 link failed').trim().slice(0, 180);
          gl.deleteShader(vs);
          gl.deleteShader(fs);
          gl.deleteProgram(prog);
          throw new Error(`VDP2 link: ${info}`);
        }
        gl.deleteShader(vs);
        gl.deleteShader(fs);

        gl.useProgram(prog);
        gl.uniform1i(gl.getUniformLocation(prog, 'uVdp2Vram'), 0);
        gl.uniform1i(gl.getUniformLocation(prog, 'uCramLut'), 1);
        gl.uniform1i(gl.getUniformLocation(prog, 'uVdp1Fb'), 2);
        gl.uniform1i(gl.getUniformLocation(prog, 'uLineTable'), 3);
        if (this.useUbo) {
          const blkIdx = gl.getUniformBlockIndex(prog, 'Vdp2Hdr');
          if (blkIdx === gl.INVALID_INDEX) {
            gl.deleteProgram(prog);
            throw new Error('VDP2 UBO block missing');
          }
          gl.uniformBlockBinding(prog, blkIdx, 0);
        } else {
          gl.uniform1i(gl.getUniformLocation(prog, 'uHdrTex'), 4);
        }

        const entry = {
          prog,
          ultra,
          vdp1Rgba8,
          uLocUltraScale: ultra ? gl.getUniformLocation(prog, 'uUltraScale') : null,
          uLocDeMesh: ultra ? gl.getUniformLocation(prog, 'uDeMesh') : null,
        };
        if (entry.uLocUltraScale) gl.uniform1i(entry.uLocUltraScale, 1);
        if (entry.uLocDeMesh) gl.uniform1i(entry.uLocDeMesh, 0);
        this.programs[key] = entry;
        return entry;
      } catch (err) {
        this.failedVariants[key] = (err && err.message) ? err.message : String(err);
        if (!ultra && !vdp1Rgba8) throw err;
        console.warn(`[TWC98 GPU-VDP2] Variant ${key} fallback:`, this.failedVariants[key]);
        return null;
      }
    }

    setQuality(ultraScale, deMesh) {
      this.ultraScale = (ultraScale >= 2) ? 2 : 1;
      this.deMesh = Boolean(deMesh);
    }

    init() {
      const gl = this.gl;
      if (!gl) {
        this.lastInitError = 'no-gl';
        return;
      }
      try {
        // Compile ONLY the lightweight mobile-compatible 'lite' shader at startup;
        // prefer UBO (L0 scalar cache on mobile GPUs), fall back cleanly to RGBA32I texture if rejected.
        try {
          this.compileVariant(false, false);
        } catch (uboErr) {
          console.warn('[TWC98 GPU-VDP2] UBO fallback to uHdrTex:', (uboErr && uboErr.message) || uboErr);
          this.useUbo = false;
          this.compileVariant(false, false);
        }

        this.vao = gl.createVertexArray();

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

        // VDP2 VRAM: 1024 x 512 R8UI (512 KiB)
        this.vdp2VramTex = mkNearestTex();
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, 1024, 512, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, null);

        // CRAM LUT: 2048 x 1 RGBA8 (8 KiB)
        this.cramTex = mkNearestTex();
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2048, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);

        // Header transport: 1 KiB std140 UBO (zero texture fetches) or 64x1 RGBA32I fallback
        if (this.useUbo) {
          this.hdrUbo = gl.createBuffer();
          gl.bindBuffer(gl.UNIFORM_BUFFER, this.hdrUbo);
          gl.bufferData(gl.UNIFORM_BUFFER, 1024, gl.DYNAMIC_DRAW);
          gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, this.hdrUbo);
          gl.bindBuffer(gl.UNIFORM_BUFFER, null);
        } else {
          this.hdrTex = mkNearestTex();
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32I, 64, 1, 0, gl.RGBA_INTEGER, gl.INT, null);
        }

        this.vdp1FbTex = mkNearestTex();
        this.lineTex = mkNearestTex();

        // Output FBO texture
        this.fbo = gl.createFramebuffer();
        this.outTex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.outTex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

        // Verify FBO completeness with initial 704x448 allocation
        this.ensureOutSize(704, 448);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
        const st = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        if (st !== gl.FRAMEBUFFER_COMPLETE) {
          throw new Error(`VDP2 FBO 0x${st.toString(16)}`);
        }
        this.ready = true;
      } catch (err) {
        this.lastInitError = (err && err.message) ? err.message : String(err);
        console.warn('[TWC98 GPU-VDP2] Init fallback to CPU:', this.lastInitError);
        this.ready = false;
      }
    }

    ensureOutSize(w, h) {
      if (this.outW === w && this.outH === h) return;
      const gl = this.gl;
      this.outW = w;
      this.outH = h;
      gl.bindTexture(gl.TEXTURE_2D, this.outTex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.outTex, 0);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    uploadDirtyVram(pktU8, pktI32 = null) {
      if (!this.ready || !pktU8 || pktU8.byteLength < 1024) return;
      const hdr = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, 256);
      if (hdr[0] !== 0x47564432) return;
      const dirtyMask = hdr[11] >>> 0;
      if (dirtyMask === 0) return;
      const v2Off = hdr[243] >>> 0;
      const gl = this.gl;
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.vdp2VramTex);
      if (dirtyMask === 0xFFFFFFFF) {
        gl.texSubImage2D(
          gl.TEXTURE_2D, 0, 0, 0, 1024, 512, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
          pktU8, v2Off
        );
      } else {
        let curOff = v2Off;
        for (let c = 0; c < 32; c++) {
          if ((dirtyMask & (1 << c)) !== 0) {
            gl.texSubImage2D(
              gl.TEXTURE_2D, 0, 0, c * 16, 1024, 16, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
              pktU8, curOff
            );
            curOff += 16384;
          }
        }
      }
      hdr[11] = 0;
    }

    compose(pktU8, extVdp1FbTex = null, extVdp1IsRgba8 = false, pktI32 = null) {
      if (!this.ready || !pktU8 || pktU8.byteLength < 1024) return null;
      const i32 = pktI32 || new Int32Array(pktU8.buffer, pktU8.byteOffset, pktU8.byteLength >> 2);
      if (i32[0] !== 0x47564432) return null;

      const fw = i32[1] | 0;
      const fh = i32[2] | 0;
      const fbW = i32[9] | 0;
      const fbH = i32[10] | 0;
      const lineOff = i32[240] >>> 0;
      const cramOff = i32[241] >>> 0;
      const fbOff = i32[242] >>> 0;

      if (fw <= 0 || fh <= 0 || fbW <= 0 || fbH <= 0) return null;

      const gl = this.gl;
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);

      const useRgba8Vdp1 = Boolean(extVdp1FbTex && extVdp1IsRgba8);
      const wantUltra = (this.ultraScale >= 2) || this.deMesh;
      let progEntry = wantUltra ? this.compileVariant(true, useRgba8Vdp1) : null;
      if (!progEntry) {
        progEntry = this.compileVariant(false, useRgba8Vdp1);
      }
      if (!progEntry) return null;

      // 1. Upload only dirty 16-row chunks (16 KiB each) of VDP2 VRAM if not yet uploaded
      if ((i32[11] >>> 0) !== 0) {
        this.uploadDirtyVram(pktU8, i32);
      }

      // 2. Upload CRAM LUT (8 KiB) via zero-alloc WebGL2 srcOffset
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.cramTex);
      gl.texSubImage2D(
        gl.TEXTURE_2D, 0, 0, 0, 2048, 1, gl.RGBA, gl.UNSIGNED_BYTE,
        pktU8, cramOff
      );

      // 3. Upload cropped VDP1 Framebuffer (R16UI) when not provided by hardware VDP1 FBO
      const activeVdp1Tex = extVdp1FbTex || this.vdp1FbTex;
      if (!extVdp1FbTex) {
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, this.vdp1FbTex);
        const fbData = new Uint16Array(pktU8.buffer, pktU8.byteOffset + fbOff, fbW * fbH);
        if (this.fbW !== fbW || this.fbH !== fbH) {
          this.fbW = fbW;
          this.fbH = fbH;
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.R16UI, fbW, fbH, 0, gl.RED_INTEGER, gl.UNSIGNED_SHORT, fbData);
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, fbW, fbH, gl.RED_INTEGER, gl.UNSIGNED_SHORT, fbData);
        }
      }

      // 4. Upload per-line table (8 x fh RGBA32I) via zero-alloc WebGL2 srcOffset
      gl.activeTexture(gl.TEXTURE3);
      gl.bindTexture(gl.TEXTURE_2D, this.lineTex);
      const lineOffI32 = lineOff >>> 2;
      if (this.lineH !== fh) {
        this.lineH = fh;
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32I, 8, fh, 0, gl.RGBA_INTEGER, gl.INT, i32, lineOffI32);
      } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 8, fh, gl.RGBA_INTEGER, gl.INT, i32, lineOffI32);
      }

      // 5. Upload 256-int header (1 KiB UBO fast-path or 64x1 RGBA32I texture fallback)
      if (this.useUbo) {
        gl.bindBuffer(gl.UNIFORM_BUFFER, this.hdrUbo);
        gl.bufferSubData(gl.UNIFORM_BUFFER, 0, i32, 0, 256);
        gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, this.hdrUbo);
      } else {
        gl.activeTexture(gl.TEXTURE4);
        gl.bindTexture(gl.TEXTURE_2D, this.hdrTex);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 64, 1, gl.RGBA_INTEGER, gl.INT, i32, 0);
      }

      // 6. Render full VDP2 composition into FBO outTex (1x lite or Ultra HD 2x)
      const sc = progEntry.ultra ? this.ultraScale : 1;
      const outW = fw * sc;
      const outH = fh * sc;
      this.ensureOutSize(outW, outH);
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.viewport(0, 0, outW, outH);
      gl.useProgram(progEntry.prog);
      gl.bindVertexArray(this.vao);
      if (progEntry.uLocUltraScale) gl.uniform1i(progEntry.uLocUltraScale, sc);
      if (progEntry.uLocDeMesh) gl.uniform1i(progEntry.uLocDeMesh, this.deMesh ? 1 : 0);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.vdp2VramTex);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.cramTex);
      gl.activeTexture(gl.TEXTURE2);
      gl.bindTexture(gl.TEXTURE_2D, activeVdp1Tex);
      gl.activeTexture(gl.TEXTURE3);
      gl.bindTexture(gl.TEXTURE_2D, this.lineTex);
      if (!this.useUbo) {
        gl.activeTexture(gl.TEXTURE4);
        gl.bindTexture(gl.TEXTURE_2D, this.hdrTex);
      }
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.activeTexture(gl.TEXTURE0);

      return { tex: this.outTex, w: outW, h: outH };
    }

    readPixelsRgba() {
      if (!this.ready || this.outW <= 0 || this.outH <= 0) return null;
      const gl = this.gl;
      const out = new Uint8Array(this.outW * this.outH * 4);
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.readPixels(0, 0, this.outW, this.outH, gl.RGBA, gl.UNSIGNED_BYTE, out);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return out;
    }
  }

  if (typeof window !== 'undefined') {
    window.TwcWebGLVdp2Compositor = TwcWebGLVdp2Compositor;
  } else {
    self.TwcWebGLVdp2Compositor = TwcWebGLVdp2Compositor;
  }
})();
