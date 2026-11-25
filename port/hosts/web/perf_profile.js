// TWC98BO Modular Performance Profile, Memory Guard & Adaptive 60/30 FPS Pacer.
// Shared between main thread (app.js) and worker (emu_worker.js via importScripts)
// so device-tier rules and adaptive cadence stay in a single clean module.

(() => {
  const VALID_FPS_MODES = { auto: 'auto', '60': '60', '30': '30' };

  function normalizeFpsMode(raw) {
    const k = String(raw || 'auto').toLowerCase().trim();
    return VALID_FPS_MODES[k] || 'auto';
  }

  function detectMobileDevice(opts = {}) {
    if (typeof opts.isMobile === 'boolean') return opts.isMobile;
    const nav = (typeof navigator !== 'undefined') ? navigator : {};
    const ua = typeof opts.userAgent === 'string' ? opts.userAgent : String(nav.userAgent || '');
    if (/Android|iPhone|iPad|iPod|Mobile/i.test(ua)) return true;
    const hasWindow = (typeof window !== 'undefined');
    const coarse = Boolean(hasWindow && window.matchMedia && window.matchMedia('(pointer: coarse)').matches);
    const touchPts = (nav.maxTouchPoints | 0);
    // Cover modern iPadOS (reports "Macintosh" UA) and large Android tablets (>1100px landscape)
    if (coarse && (touchPts > 1 || (hasWindow && window.innerWidth <= 1100))) return true;
    if (/Macintosh/i.test(ua) && touchPts > 1) return true;
    return false;
  }

  function detectDeviceProfile(opts = {}) {
    const nav = (typeof navigator !== 'undefined') ? navigator : {};
    const isMobile = detectMobileDevice(opts);
    const cores = Math.max(1, (opts.hardwareConcurrency || nav.hardwareConcurrency || 4) | 0);
    const memRaw = Number(opts.deviceMemory ?? nav.deviceMemory ?? 0);
    const deviceMemoryGb = (Number.isFinite(memRaw) && memRaw > 0) ? memRaw : (isMobile ? 4 : 8);
    const isSoftwareGpu = Boolean(opts.isSoftwareRenderer);
    const gpuVdpActive = (typeof opts.gpuVdpActive === 'boolean') ? opts.gpuVdpActive : true;

    const isLowEnd = Boolean(
      isSoftwareGpu ||
      (isMobile && (!gpuVdpActive || deviceMemoryGb <= 4 || cores <= 4))
    );
    const tier = !isMobile
      ? (isSoftwareGpu ? 'desktop-software' : 'desktop')
      : (isLowEnd ? 'mobile-lowend' : 'mobile-standard');

    return {
      isMobile,
      cores,
      deviceMemoryGb,
      isSoftwareGpu,
      isLowEnd,
      tier,
    };
  }

  // Never spawn multi-Wasm band workers on mobile (each worker allocates 128 MiB linear RAM + JIT).
  function computeBandWorkerCount(opts = {}) {
    const explicit = opts.explicitParam;
    if (explicit !== null && explicit !== undefined && explicit !== '') {
      const n = parseInt(String(explicit), 10);
      if (Number.isFinite(n)) {
        if (n <= 0) return 0;
        return Math.max(2, Math.min(4, n));
      }
    }
    if (opts.isMobile) return 0;
    const cores = Math.max(1, (opts.cores || 4) | 0);
    const memGb = Number(opts.deviceMemoryGb || 8);
    if (cores < 4 || memGb < 4) return 0;
    return cores >= 6 ? 3 : 2;
  }

  class AdaptivePacer {
    constructor(opts = {}) {
      this.mode = normalizeFpsMode(opts.mode);
      this.isMobile = Boolean(opts.isMobile);
      this.gpuVdp = (typeof opts.gpuVdp === 'boolean') ? opts.gpuVdp : true;
      this.renderEvery = (this.mode === '30') ? 2 : 1;
      this.targetVisFps = (this.mode === '30') ? 30 : 60;
      this.simEma = 8.0;
      this.sampleCount = 0;
      this.warmupFrames = 60;
      this.slowStreak = 0;
      this.fastStreak = 0;
      this.below30Streak = 0;
      this.ok30Streak = 0;
      this.below30Warning = false;
      this.autoLowResRequested = false;
    }

    setMode(mode) {
      const next = normalizeFpsMode(mode);
      this.mode = next;
      if (next === '30') {
        this.renderEvery = 2;
        this.targetVisFps = 30;
      } else if (next === '60') {
        this.renderEvery = 1;
        this.targetVisFps = 60;
      } else {
        this.renderEvery = 1;
        this.targetVisFps = 60;
      }
      this.slowStreak = 0;
      this.fastStreak = 0;
      this.below30Streak = 0;
      this.ok30Streak = 0;
      this.below30Warning = false;
      this.warmupFrames = 45;
      return this.getState();
    }

    setEnvironment(opts = {}) {
      if (typeof opts.isMobile === 'boolean') this.isMobile = opts.isMobile;
      if (typeof opts.gpuVdp === 'boolean') this.gpuVdp = opts.gpuVdp;
      if (this.gpuVdp && this.mode === 'auto' && this.renderEvery > 1) {
        this.renderEvery = 1;
        this.targetVisFps = 60;
      }
      if (!this.gpuVdp && this.isMobile && this.mode === 'auto') {
        this.autoLowResRequested = true;
      }
    }

    reset() {
      this.sampleCount = 0;
      this.warmupFrames = 60;
      this.slowStreak = 0;
      this.fastStreak = 0;
      this.below30Streak = 0;
      this.ok30Streak = 0;
      this.below30Warning = false;
      if (this.mode === 'auto') {
        this.renderEvery = 1;
        this.targetVisFps = 60;
      }
    }

    shouldRenderFrame(stepId) {
      if (this.renderEvery <= 1) return true;
      return ((stepId | 0) % this.renderEvery) === 0;
    }

    recordFrame(sample = {}) {
      const simMs = Number(sample.simMs || 0);
      const skipped = Boolean(sample.skipped);
      const simFps = Number(sample.simFps || 0);
      const visFps = Number(sample.visFps || 0);

      if (Number.isFinite(simMs) && simMs >= 0 && simMs < 250) {
        if (this.sampleCount < 5) {
          this.simEma = (this.sampleCount === 0) ? simMs : (this.simEma * 0.6 + simMs * 0.4);
        } else {
          this.simEma = this.simEma * 0.92 + simMs * 0.08;
        }
        this.sampleCount++;
      }

      if (this.warmupFrames > 0) {
        this.warmupFrames--;
        return { changed: false, state: this.getState() };
      }

      let changed = false;

      // Auto 60->30 frame-skip only helps when CPU rendering (!gpuVdp) is the bottleneck.
      // When GPU VDP1+2 is active (ren ~0.7 ms), keeping renderEvery=1 displays 100% of simulated frames.
      if (this.mode === 'auto' && !this.gpuVdp) {
        const downThresh = this.isMobile ? 11.8 : 15.4;
        const upThresh = this.isMobile ? 8.5 : 11.2;

        if (this.renderEvery === 1) {
          const isOverBudget = (this.simEma > downThresh) || (skipped && this.simEma > downThresh * 0.85);
          if (isOverBudget) {
            this.slowStreak++;
            this.fastStreak = 0;
          } else {
            this.slowStreak = Math.max(0, this.slowStreak - 1);
          }
          if (this.slowStreak >= 45) {
            this.renderEvery = 2;
            this.targetVisFps = 30;
            this.slowStreak = 0;
            this.fastStreak = 0;
            if (this.isMobile) {
              this.autoLowResRequested = true;
            }
            changed = true;
          }
        } else {
          const hasHeadroom = (this.simEma < upThresh) && !skipped && (simFps === 0 || simFps >= 57.0);
          if (hasHeadroom) {
            this.fastStreak++;
            this.slowStreak = 0;
          } else {
            this.fastStreak = 0;
          }
          if (this.fastStreak >= 240) {
            this.renderEvery = 1;
            this.targetVisFps = 60;
            this.fastStreak = 0;
            this.below30Streak = 0;
            this.ok30Streak = 0;
            if (this.below30Warning) this.below30Warning = false;
            changed = true;
          }
        }
      }

      // Sustained low-FPS / CPU-limit warning (offers 1-tap Audio Lite -33% CPU boost; never blocks gameplay)
      const estVisFps = (visFps > 0) ? visFps : (simFps > 0 ? (simFps / this.renderEvery) : 60);
      const belowMinimum = (this.renderEvery >= 2)
        ? (this.simEma > 19.5 || (simFps > 0 && simFps < 51.0) || estVisFps < 26.5)
        : (this.simEma > 17.8 && simFps > 0 && simFps < 52.0);

      if (belowMinimum) {
        this.below30Streak++;
        this.ok30Streak = 0;
        if (this.below30Streak >= 150 && !this.below30Warning) {
          this.below30Warning = true;
          changed = true;
        }
      } else if (this.below30Warning) {
        this.ok30Streak++;
        this.below30Streak = Math.max(0, this.below30Streak - 2);
        if (this.ok30Streak >= 180) {
          this.below30Warning = false;
          this.ok30Streak = 0;
          changed = true;
        }
      } else {
        this.below30Streak = Math.max(0, this.below30Streak - 1);
      }

      return { changed, state: this.getState() };
    }

    getState() {
      return {
        mode: this.mode,
        renderEvery: this.renderEvery,
        targetVisFps: this.targetVisFps,
        below30Warning: this.below30Warning,
        autoLowResRequested: this.autoLowResRequested,
        simEma: Number(this.simEma.toFixed(2)),
      };
    }
  }

  const api = {
    normalizeFpsMode,
    detectMobileDevice,
    detectDeviceProfile,
    computeBandWorkerCount,
    AdaptivePacer,
  };

  if (typeof window !== 'undefined') {
    window.TwcPerfProfile = api;
  } else {
    self.TwcPerfProfile = api;
  }
})();
