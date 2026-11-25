// TWC98BO Modular WebGL2 Copa America Special Shot VFX Post-Pass.
// Renders Bolivia SP3 (Andean Gale Wind Vortex + Aerodynamic Wake + Pre-Shot Inflow)
// directly into the VDP2 FBO on GPU so low-end mobile devices stay in the 60 FPS WebGL2 pipeline
// with zero CPU VDP1/VDP2 software fallback.
// Lazy shader compilation: zero startup overhead until Bolivia SP3 is actually triggered.

(() => {
  const VS_WIND = `#version 300 es
    uniform vec4 uBoundsNdc; // (x0, y0, x1, y1) in clip space [-1..1]
    const vec2 UV[4] = vec2[4](
      vec2(0.0, 0.0),
      vec2(1.0, 0.0),
      vec2(0.0, 1.0),
      vec2(1.0, 1.0)
    );
    void main() {
      vec2 u = UV[gl_VertexID];
      vec2 pos = mix(uBoundsNdc.xy, uBoundsNdc.zw, u);
      gl_Position = vec4(pos, 0.0, 1.0);
    }
  `;

  const FS_WIND = `#version 300 es
    precision highp float;

    uniform vec2 uRawOverOut;  // raw_w / out_w, raw_h / out_h
    uniform vec4 uBall;        // (bx, by, rad, phase)
    uniform vec4 uLife;        // (fade_life, pre_fade, inflow_on, age_f)
    uniform int uTrailCount;
    uniform vec2 uTrail[10];

    out vec4 fragColor;

    void main() {
      vec2 p = gl_FragCoord.xy * uRawOverOut;
      vec2 b = uBall.xy;
      float rad = uBall.z;
      float phase = uBall.w;
      float fade_life = uLife.x;
      float pre_fade = uLife.y;
      bool inflow_on = uLife.z > 0.5;
      float age_f = uLife.w;

      vec3 col_acc = vec3(0.0);
      float a_acc = 0.0;

      // 1. Tapering translucent wind wake / aerodynamic tunnel behind the ball
      if (uTrailCount >= 2) {
        float best_wake = 0.0;
        float inv_segs = 1.0 / float(uTrailCount - 1);
        for (int seg = 0; seg < 9; ++seg) {
          if (seg >= uTrailCount - 1) break;
          vec2 p0 = uTrail[seg];
          vec2 p1 = uTrail[seg + 1];
          vec2 d = p1 - p0;
          float l2 = dot(d, d);
          if (l2 < 0.0625) continue;
          float s = clamp(dot(p - p0, d) / l2, 0.0, 1.0);
          vec2 closest = p0 + d * s;
          vec2 diff = p - closest;
          float t_local = (float(seg) + s) * inv_segs;
          float cone_r = max(1.5, rad * (0.92 - 0.72 * t_local));
          float dist = length(diff);
          if (dist < cone_r) {
            float inv_l = inversesqrt(l2);
            vec2 nrm = vec2(-d.y, d.x) * inv_l;
            float kf = clamp(dot(diff, nrm) / cone_r, -1.0, 1.0);
            float akf = abs(kf);
            float falloff = 1.0 - t_local;
            float edge_boost = 0.25 + 0.75 * akf * akf;
            float wave = 0.5 + 0.5 * sin(t_local * 14.0 - phase * 1.5 + kf * 2.4);
            float w_a = fade_life * falloff * falloff * (0.20 * edge_boost + 0.26 * wave * akf);
            best_wake = max(best_wake, w_a);
          }
        }
        if (best_wake > 0.004) {
          vec3 wake_col = vec3(0.910, 0.949, 1.0);
          col_acc += wake_col * best_wake;
          a_acc += best_wake;
        }
      }

      // 2. Translucent wind vortex dome + 3 swirling orbital ribbons around ball
      vec2 dp = p - b;
      float dist = length(dp);
      float nr = dist / rad;
      if (nr <= 1.22) {
        float alpha = 0.0;
        if (nr <= 1.0) {
          float nr2 = nr * nr;
          float rim = nr2 * sqrt(nr);
          float outer_soft = (nr > 0.90) ? max(0.0, 1.0 - (nr - 0.90) / 0.14) : 1.0;
          alpha = (0.06 + 0.34 * rim) * outer_soft;
        }
        vec2 u = dp / rad;
        // Precomputed cos/sin of tilts (0.45, 1.40, 2.35 rad)
        const vec3 C_ANG = vec3(0.900447, 0.169967, -0.702713);
        const vec3 S_ANG = vec3(0.434966, 0.985450,  0.711474);
        const vec3 RING_R = vec3(0.86, 0.96, 1.06);
        const vec3 ELL_INV = vec3(2.631579, 2.173913, 1.851852);
        const vec3 PH_MUL = vec3(1.15, 1.43, 1.71);
        const vec3 PH_ADD = vec3(0.0, 2.094, 4.188);
        for (int ri = 0; ri < 3; ++ri) {
          float ca = C_ANG[ri];
          float sa = S_ANG[ri];
          float rx = u.x * ca + u.y * sa;
          float ry = -u.x * sa + u.y * ca;
          float ell_y = ry * ELL_INV[ri];
          float rr = RING_R[ri];
          float ring_d = abs(length(vec2(rx, ell_y)) / rr - 1.0);
          if (ring_d < 0.22) {
            float ang = atan(ell_y, rx) + phase * PH_MUL[ri] + PH_ADD[ri];
            float arc = 0.5 + 0.5 * sin(ang * 2.0);
            if (arc > 0.28) {
              alpha += (1.0 - ring_d / 0.22) * arc * 0.44;
            }
          }
        }
        float v_a = alpha * fade_life;
        if (v_a > 0.008) {
          col_acc += vec3(0.965, 0.980, 1.0) * v_a;
          a_acc += v_a;
        }
      }

      // 3. Swirling wind & turf dust particles orbiting the wind vortex
      if (nr <= 1.55) {
        for (int pi = 0; pi < 10; ++pi) {
          float pf = float(pi);
          float p_ang = phase * (1.25 + 0.09 * pf) + pf * 1.618;
          float orbit_r = rad * (0.85 + 0.46 * sin(phase * 0.5 + pf * 0.9));
          vec2 op = b + vec2(cos(p_ang) * orbit_r, sin(p_ang) * orbit_r * 0.65);
          vec2 pd = p - op;
          float d2 = dot(pd, pd);
          if (d2 < 3.5) {
            float pa = (1.0 - d2 / 3.5) * 0.72 * fade_life;
            bool warm = (pi == 0 || pi == 3 || pi == 6 || pi == 9);
            vec3 pcol = warm ? vec3(0.722, 0.580, 0.424) : vec3(0.973, 0.988, 1.0);
            col_acc += pcol * pa;
            a_acc += pa;
          }
        }
      }

      // 4. Pre-shot converging Andean gale inflow streams & contracting rings
      if (inflow_on && pre_fade > 0.01) {
        vec2 dp_in = vec2(dp.x, dp.y / 0.82);
        float r_in = length(dp_in);
        if (r_in >= 10.0 && r_in <= 165.0) {
          float ang = atan(dp_in.y, dp_in.x);
          float phase_in = age_f * 0.45;
          // 11 swirling radial wind streams rushing inward
          float swirl_ang = ang + 0.0032 * (155.0 - r_in) + 0.12 * sin(r_in * 0.075 - phase_in * 1.5);
          float ray_wave = pow(max(0.0, cos(swirl_ang * 11.0)), 6.0);
          float radial_flow = fract(age_f * 0.085 + r_in * 0.0085 + floor((ang + 3.14159265) * 1.750704) * 0.273);
          float flow_env = sin(radial_flow * 3.14159265);
          float r_env = smoothstep(162.0, 115.0, r_in) * smoothstep(10.0, 26.0, r_in);
          float g_a = pre_fade * ray_wave * flow_env * r_env * 0.58;
          if (g_a > 0.006) {
            float t_in = clamp(1.0 - r_in / 155.0, 0.0, 1.0);
            vec3 gcol = vec3(0.824 + 0.172 * t_in, 0.933 + 0.067 * t_in, 1.0);
            col_acc += gcol * g_a;
            a_acc += g_a;
          }
        }

        // 3 contracting concentric gale arcs around shooter/ball
        vec2 dp_ring = vec2(dp.x, dp.y / 0.68);
        float r_ring = length(dp_ring);
        if (r_ring >= 8.0 && r_ring <= 112.0) {
          float ang_r = atan(dp_ring.y, dp_ring.x);
          float phase_in = age_f * 0.45;
          for (int w = 0; w < 3; ++w) {
            float cyc = fract(age_f * 0.065 + float(w) * 0.3333);
            float arc_r = 92.0 * (1.0 - cyc) + 12.0;
            float dr = abs(r_ring - arc_r);
            if (dr < 2.2) {
              float dir = (w == 1) ? -0.45 : 0.45;
              float gap = sin((ang_r + phase_in * dir) * 3.0 + phase_in + float(w) * 2.1);
              if (gap > 0.05) {
                float ra = pre_fade * sin(cyc * 3.14159265) * (1.0 - dr / 2.2) * gap * 0.46;
                col_acc += vec3(0.878, 0.957, 1.0) * ra;
                a_acc += ra;
              }
            }
          }
        }
      }

      if (a_acc <= 0.004) discard;
      float out_a = min(0.88, a_acc);
      vec3 out_rgb = clamp(col_acc / max(0.001, a_acc), 0.0, 1.0);
      fragColor = vec4(out_rgb, out_a);
    }
  `;

  function compileShader(gl, type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const info = gl.getShaderInfoLog(sh) || 'CopaVfx compile error';
      gl.deleteShader(sh);
      throw new Error(info);
    }
    return sh;
  }

  class TwcWebGLCopaVfx {
    constructor(gl) {
      this.gl = gl;
      this.prog = null;
      this.vao = null;
      this.failed = false;
      this.lastInitError = '';
      this.trailF32 = new Float32Array(20); // 10 vec2 points, reused zero-alloc
      this.uLocBounds = null;
      this.uLocRawOverOut = null;
      this.uLocBall = null;
      this.uLocLife = null;
      this.uLocTrailCount = null;
      this.uLocTrail = null;
    }

    ensureProgram() {
      if (this.prog) return true;
      if (this.failed || !this.gl) return false;
      const gl = this.gl;
      try {
        const vs = compileShader(gl, gl.VERTEX_SHADER, VS_WIND);
        const fs = compileShader(gl, gl.FRAGMENT_SHADER, FS_WIND);
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          const info = gl.getProgramInfoLog(prog) || 'CopaVfx link error';
          gl.deleteProgram(prog);
          throw new Error(info);
        }
        this.prog = prog;
        this.vao = gl.createVertexArray();
        this.uLocBounds = gl.getUniformLocation(prog, 'uBoundsNdc');
        this.uLocRawOverOut = gl.getUniformLocation(prog, 'uRawOverOut');
        this.uLocBall = gl.getUniformLocation(prog, 'uBall');
        this.uLocLife = gl.getUniformLocation(prog, 'uLife');
        this.uLocTrailCount = gl.getUniformLocation(prog, 'uTrailCount');
        this.uLocTrail = gl.getUniformLocation(prog, 'uTrail[0]');
        return true;
      } catch (err) {
        this.failed = true;
        this.lastInitError = (err && err.message) ? err.message : String(err);
        return false;
      }
    }

    drawBoliviaWind(fbo, outW, outH, i32) {
      if (!fbo || outW <= 0 || outH <= 0 || !i32) return false;
      const wFlags = i32[216] >>> 0;
      if ((wFlags & 1) === 0) return false;
      if (!this.ensureProgram()) return false;

      const rawW = Math.max(1, i32[3] | 0);
      const rawH = Math.max(1, i32[4] | 0);
      const inflowOn = (wFlags & 2) !== 0;
      const trailCount = Math.min(10, (wFlags >>> 4) & 0xF);
      const frameAge = (wFlags >>> 12) & 0xFFF;
      const chargeAge = (wFlags >>> 24) & 0xFF;
      const hasChargeRays = (i32[230] | 0) !== 0;

      const bx = (i32[217] | 0) * 0.0625;
      const by = (i32[218] | 0) * 0.0625;
      const rad = Math.max(10.0, (i32[219] | 0) * 0.0625);
      const phase = frameAge * 0.42;
      const fadeLife = (frameAge > 55) ? Math.max(0.0, 1.0 - (frameAge - 55) / 18.0) : 1.0;
      const preFade = (frameAge > 28 && !hasChargeRays) ? Math.max(0.0, 1.0 - (frameAge - 28) / 7.0) : 1.0;
      const ageF = Math.max(chargeAge, frameAge);

      // Compute tight screen-space bounding quad in raw_w x raw_h
      const padBall = rad * 1.65;
      let minX = bx - padBall;
      let maxX = bx + padBall;
      let minY = by - padBall;
      let maxY = by + padBall;

      if (inflowOn && preFade > 0.01) {
        minX = Math.min(minX, bx - 170.0);
        maxX = Math.max(maxX, bx + 170.0);
        minY = Math.min(minY, by - 142.0);
        maxY = Math.max(maxY, by + 142.0);
      }

      const tArr = this.trailF32;
      for (let i = 0; i < trailCount; ++i) {
        const packed = i32[220 + i] | 0;
        const tx = ((packed << 16) >> 16) * 0.25;
        const ty = (packed >> 16) * 0.25;
        tArr[i * 2] = tx;
        tArr[i * 2 + 1] = ty;
        minX = Math.min(minX, tx - rad);
        maxX = Math.max(maxX, tx + rad);
        minY = Math.min(minY, ty - rad);
        maxY = Math.max(maxY, ty + rad);
      }

      minX = Math.max(0.0, minX);
      minY = Math.max(0.0, minY);
      maxX = Math.min(rawW, maxX);
      maxY = Math.min(rawH, maxY);
      if (maxX <= minX || maxY <= minY) return false;

      const x0Ndc = (minX / rawW) * 2.0 - 1.0;
      const y0Ndc = (minY / rawH) * 2.0 - 1.0;
      const x1Ndc = (maxX / rawW) * 2.0 - 1.0;
      const y1Ndc = (maxY / rawH) * 2.0 - 1.0;

      const gl = this.gl;
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.viewport(0, 0, outW, outH);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.useProgram(this.prog);
      gl.bindVertexArray(this.vao);
      gl.uniform4f(this.uLocBounds, x0Ndc, y0Ndc, x1Ndc, y1Ndc);
      gl.uniform2f(this.uLocRawOverOut, rawW / outW, rawH / outH);
      gl.uniform4f(this.uLocBall, bx, by, rad, phase);
      gl.uniform4f(this.uLocLife, fadeLife, preFade, inflowOn ? 1.0 : 0.0, ageF);
      gl.uniform1i(this.uLocTrailCount, trailCount);
      if (trailCount > 0 && this.uLocTrail) {
        gl.uniform2fv(this.uLocTrail, tArr);
      }
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.disable(gl.BLEND);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return true;
    }
  }

  if (typeof window !== 'undefined') {
    window.TwcWebGLCopaVfx = TwcWebGLCopaVfx;
  } else {
    self.TwcWebGLCopaVfx = TwcWebGLCopaVfx;
  }
})();
