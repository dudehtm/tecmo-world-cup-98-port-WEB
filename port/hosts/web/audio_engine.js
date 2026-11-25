// TWC98BO Continuous Ring Audio Engine (AudioWorklet 44.1 kHz + jitter cushion + direct SAB ring).
// Eliminates per-frame AudioBufferSourceNode splice gaps, main-thread audio stalls, and robotic stuttering.

(() => {
  const RING_CAP_FRAMES = 16384; // power of 2 (~371 ms at 44.1 kHz)
  const SAB_HDR_BYTES = 16;      // Int32Array[4]: [0]=writeSeq, [1]=readSeq, [2]=clearSeq, [3]=underrunSamples
  const SAB_TOTAL_BYTES = SAB_HDR_BYTES + RING_CAP_FRAMES * 2 * 2;

  const WORKLET_CODE = `
    class Twc98RingProcessor extends AudioWorkletProcessor {
      constructor(options) {
        super();
        const opts = (options && options.processorOptions) || {};
        this.srcRate = 44100;
        this.outRate = opts.outRate || sampleRate || 44100;
        this.cap = ${RING_CAP_FRAMES};
        this.mask = this.cap - 1;
        const mob = Boolean(opts.isMobile);
        this.minPrime = mob ? 4410 : 3308;    // ~100 ms mobile / ~75 ms desktop cushion
        this.targetLevel = mob ? 4851 : 3675; // ~110 ms mobile / ~83 ms desktop target
        this.maxQueued = mob ? 12000 : 9555;  // ~272 ms mobile / ~216 ms desktop cap
        this.trimTarget = this.targetLevel;
        this.primed = false;
        this.frac = 0;
        this.lastL = 0;
        this.lastR = 0;
        this.fade = 0;
        this.underrunSamples = 0;

        this.sabHdr = null;
        this.sabRingI16 = null;
        this.lastClearSeq = 0;
        if (opts.sab && typeof SharedArrayBuffer !== 'undefined' && typeof Atomics !== 'undefined') {
          try {
            this.sabHdr = new Int32Array(opts.sab, 0, 4);
            this.sabRingI16 = new Int16Array(opts.sab, ${SAB_HDR_BYTES}, this.cap * 2);
            this.lastClearSeq = Atomics.load(this.sabHdr, 2);
          } catch {
            this.sabHdr = null;
            this.sabRingI16 = null;
          }
        }

        // Fallback postMessage ring when SharedArrayBuffer is unavailable in AudioWorkletGlobalScope
        this.bufL = this.sabHdr ? null : new Float32Array(this.cap);
        this.bufR = this.sabHdr ? null : new Float32Array(this.cap);
        this.r = 0;
        this.w = 0;
        this.count = 0;

        this.port.postMessage({ type: 'worklet_ready', sabActive: Boolean(this.sabHdr) });

        this.port.onmessage = (ev) => {
          const msg = ev.data;
          if (!msg) return;
          if (msg.sab && typeof SharedArrayBuffer !== 'undefined' && !this.sabHdr) {
            this.sabHdr = new Int32Array(msg.sab, 0, 4);
            this.sabRingI16 = new Int16Array(msg.sab, ${SAB_HDR_BYTES}, this.cap * 2);
            this.lastClearSeq = Atomics.load(this.sabHdr, 2);
            return;
          }
          if (msg.clear) {
            this.r = 0;
            this.w = 0;
            this.count = 0;
            this.primed = false;
            this.frac = 0;
            this.lastL = 0;
            this.lastR = 0;
            this.fade = 0;
            this.underrunSamples = 0;
            return;
          }
          if (!msg.pcm || this.sabHdr) return;
          const i16 = new Int16Array(msg.pcm);
          const frames = (i16.length / 2) | 0;
          if (frames <= 0) return;
          this.underrunSamples = 0;
          for (let i = 0; i < frames; i++) {
            this.bufL[this.w] = i16[i * 2] / 32768.0;
            this.bufR[this.w] = i16[i * 2 + 1] / 32768.0;
            this.w = (this.w + 1) & this.mask;
            if (this.count < this.cap) {
              this.count++;
            } else {
              this.r = (this.r + 1) & this.mask;
            }
          }
          if (this.count > this.maxQueued) {
            const drop = this.count - this.trimTarget;
            this.r = (this.r + drop) & this.mask;
            this.count -= drop;
          }
        };
      }

      process(_inputs, outputs) {
        const out = outputs[0];
        if (!out || out.length < 2) return true;
        const outL = out[0];
        const outR = out[1];
        const n = outL.length;

        if (this.sabHdr) {
          const curClear = Atomics.load(this.sabHdr, 2);
          if (curClear !== this.lastClearSeq) {
            this.lastClearSeq = curClear;
            Atomics.store(this.sabHdr, 1, Atomics.load(this.sabHdr, 0));
            this.primed = false;
            this.frac = 0;
            this.lastL = 0;
            this.lastR = 0;
            this.fade = 0;
            this.underrunSamples = 0;
          }

          const w = Atomics.load(this.sabHdr, 0);
          let r = Atomics.load(this.sabHdr, 1);
          let avail = (w - r) | 0;
          if (avail < 0 || avail > this.cap) {
            r = (w - this.trimTarget) | 0;
            avail = this.trimTarget;
          }

          if (!this.primed) {
            if (avail >= this.minPrime) {
              this.primed = true;
              this.fade = 0;
              this.frac = 0;
            } else {
              for (let i = 0; i < n; i++) {
                this.lastL *= 0.92;
                this.lastR *= 0.92;
                outL[i] = this.lastL;
                outR[i] = this.lastR;
              }
              Atomics.store(this.sabHdr, 1, r);
              return true;
            }
          }

          if (avail > this.maxQueued) {
            const drop = avail - this.trimTarget;
            r = (r + drop) | 0;
            avail -= drop;
            this.fade = 0;
          }

          const err = avail - this.targetLevel;
          const nudge = Math.max(-0.012, Math.min(0.012, err * 0.0000024));
          const step = (this.srcRate / this.outRate) * (1.0 + nudge);
          const ring = this.sabRingI16;
          const mask = this.mask;
          let underrunBatch = 0;

          for (let i = 0; i < n; i++) {
            if (avail < 2) {
              this.underrunSamples++;
              underrunBatch++;
              this.lastL *= 0.994;
              this.lastR *= 0.994;
              outL[i] = this.lastL;
              outR[i] = this.lastR;
              if (this.underrunSamples > 16000) {
                this.primed = false;
              }
              continue;
            }
            this.underrunSamples = 0;
            const i0 = (r & mask) * 2;
            const i1 = ((r + 1) & mask) * 2;
            const t = this.frac;
            const l0 = ring[i0] / 32768.0;
            const r0 = ring[i0 + 1] / 32768.0;
            const l1 = ring[i1] / 32768.0;
            const r1 = ring[i1 + 1] / 32768.0;
            let sL = l0 + (l1 - l0) * t;
            let sR = r0 + (r1 - r0) * t;

            if (this.fade < 1.0) {
              this.fade = Math.min(1.0, this.fade + 0.005);
              sL *= this.fade;
              sR *= this.fade;
            }

            this.lastL = sL;
            this.lastR = sR;
            outL[i] = sL;
            outR[i] = sR;

            this.frac += step;
            const adv = this.frac | 0;
            if (adv > 0) {
              this.frac -= adv;
              const take = adv < avail ? adv : avail;
              r = (r + take) | 0;
              avail -= take;
            }
          }
          Atomics.store(this.sabHdr, 1, r);
          if (underrunBatch > 0) {
            Atomics.add(this.sabHdr, 3, underrunBatch);
          }
          return true;
        }

        // Legacy postMessage fallback path
        if (!this.primed) {
          if (this.count >= this.minPrime) {
            this.primed = true;
            this.fade = 0;
            this.frac = 0;
          } else {
            for (let i = 0; i < n; i++) {
              this.lastL *= 0.92;
              this.lastR *= 0.92;
              outL[i] = this.lastL;
              outR[i] = this.lastR;
            }
            return true;
          }
        }

        const err = this.count - this.targetLevel;
        const nudge = Math.max(-0.006, Math.min(0.006, err * 0.0000018));
        const step = (this.srcRate / this.outRate) * (1.0 + nudge);

        for (let i = 0; i < n; i++) {
          if (this.count < 2) {
            this.underrunSamples++;
            this.lastL *= 0.994;
            this.lastR *= 0.994;
            outL[i] = this.lastL;
            outR[i] = this.lastR;
            if (this.underrunSamples > 16000) {
              this.primed = false;
            }
            continue;
          }
          const idx0 = this.r;
          const idx1 = (this.r + 1) & this.mask;
          const t = this.frac;
          let sL = this.bufL[idx0] + (this.bufL[idx1] - this.bufL[idx0]) * t;
          let sR = this.bufR[idx0] + (this.bufR[idx1] - this.bufR[idx0]) * t;

          if (this.fade < 1.0) {
            this.fade = Math.min(1.0, this.fade + 0.005);
            sL *= this.fade;
            sR *= this.fade;
          }

          this.lastL = sL;
          this.lastR = sR;
          outL[i] = sL;
          outR[i] = sR;

          this.frac += step;
          const adv = this.frac | 0;
          if (adv > 0) {
            this.frac -= adv;
            const take = adv < this.count ? adv : this.count;
            this.r = (this.r + take) & this.mask;
            this.count -= take;
          }
        }
        return true;
      }
    }
    registerProcessor('twc98-ring-processor', Twc98RingProcessor);
  `;

  class TwcAudioEngine {
    constructor(opts = {}) {
      this.isMobile = Boolean(opts.isMobile);
      this.onSabModeChange = (typeof opts.onSabModeChange === 'function') ? opts.onSabModeChange : null;
      this.ctx = null;
      this.gain = null;
      this.worklet = null;
      this.workletPromise = null;
      this.scriptNode = null;
      this.fbL = null;
      this.fbR = null;
      this.fbRIdx = 0;
      this.fbWIdx = 0;
      this.fbCount = 0;
      this.fbFrac = 0;
      this.fbPrimed = false;
      this.fbLastL = 0;
      this.fbLastR = 0;
      this.muted = false;
      this.volumePct = 100;
      this.fallbackVoices = 0;
      this.ready = false;
      this.backendTag = 'init';
      this.lastError = '';
      this.sab = null;
      this.sabHdr = null;
      this.useSab = false;
      if (typeof SharedArrayBuffer !== 'undefined' && typeof Atomics !== 'undefined' &&
          (typeof window === 'undefined' || window.crossOriginIsolated !== false)) {
        try {
          this.sab = new SharedArrayBuffer(SAB_TOTAL_BYTES);
          this.sabHdr = new Int32Array(this.sab, 0, 4);
          this.useSab = true;
        } catch {
          this.sab = null;
          this.sabHdr = null;
          this.useSab = false;
        }
      }
    }

    getSharedAudioSab() {
      return this.useSab ? this.sab : null;
    }

    isRunning() {
      return Boolean(this.ctx && this.ctx.state === 'running' && (this.worklet || this.scriptNode));
    }

    getStateTag() {
      const st = this.ctx ? this.ctx.state : 'idle';
      return `${st}/${this.backendTag}`;
    }

    getUnderrunCount() {
      return (this.useSab && this.sabHdr) ? (Atomics.load(this.sabHdr, 3) >>> 0) : 0;
    }

    getBufferedFrames() {
      if (this.useSab && this.sabHdr) {
        return (Atomics.load(this.sabHdr, 0) - Atomics.load(this.sabHdr, 1)) | 0;
      }
      return this.fbCount | 0;
    }

    attachScriptFallback() {
      if (!this.ctx || this.worklet || this.scriptNode || typeof this.ctx.createScriptProcessor !== 'function') return;
      const cap = RING_CAP_FRAMES;
      const mask = cap - 1;
      const sabRing = (this.useSab && this.sab) ? new Int16Array(this.sab, SAB_HDR_BYTES, cap * 2) : null;
      const sabHdr = (this.useSab && this.sabHdr) ? this.sabHdr : null;
      if (!sabHdr) {
        this.fbL = new Float32Array(cap);
        this.fbR = new Float32Array(cap);
      }
      const sp = this.ctx.createScriptProcessor(2048, 0, 2);
      const outRate = this.ctx.sampleRate || 44100;
      let lastClear = sabHdr ? Atomics.load(sabHdr, 2) : 0;
      sp.onaudioprocess = (ev) => {
        const outL = ev.outputBuffer.getChannelData(0);
        const outR = ev.outputBuffer.getChannelData(1);
        const n = outL.length;
        if (sabHdr && sabRing) {
          const curClear = Atomics.load(sabHdr, 2);
          if (curClear !== lastClear) {
            lastClear = curClear;
            Atomics.store(sabHdr, 1, Atomics.load(sabHdr, 0));
            this.fbPrimed = false;
            this.fbFrac = 0;
          }
          const w = Atomics.load(sabHdr, 0);
          let r = Atomics.load(sabHdr, 1);
          let avail = (w - r) | 0;
          if (avail < 0 || avail > cap) {
            r = (w - 3308) | 0;
            avail = 3308;
          }
          if (!this.fbPrimed) {
            if (avail >= 2940) {
              this.fbPrimed = true;
              this.fbFrac = 0;
            } else {
              outL.fill(0);
              outR.fill(0);
              Atomics.store(sabHdr, 1, r);
              return;
            }
          }
          if (avail > 7350) {
            const drop = avail - 3308;
            r = (r + drop) | 0;
            avail -= drop;
          }
          const step = (44100 / outRate) * (1.0 + Math.max(-0.006, Math.min(0.006, (avail - 3308) * 0.0000018)));
          for (let i = 0; i < n; i++) {
            if (avail < 2) {
              this.fbLastL *= 0.994;
              this.fbLastR *= 0.994;
              outL[i] = this.fbLastL;
              outR[i] = this.fbLastR;
              continue;
            }
            const i0 = (r & mask) * 2;
            const i1 = ((r + 1) & mask) * 2;
            const t = this.fbFrac;
            const l0 = sabRing[i0] / 32768.0;
            const r0 = sabRing[i0 + 1] / 32768.0;
            this.fbLastL = l0 + (sabRing[i1] / 32768.0 - l0) * t;
            this.fbLastR = r0 + (sabRing[i1 + 1] / 32768.0 - r0) * t;
            outL[i] = this.fbLastL;
            outR[i] = this.fbLastR;
            this.fbFrac += step;
            const adv = this.fbFrac | 0;
            if (adv > 0) {
              this.fbFrac -= adv;
              const take = adv < avail ? adv : avail;
              r = (r + take) | 0;
              avail -= take;
            }
          }
          Atomics.store(sabHdr, 1, r);
          return;
        }
        if (!this.fbPrimed) {
          if (this.fbCount >= 2940) {
            this.fbPrimed = true;
            this.fbFrac = 0;
          } else {
            outL.fill(0);
            outR.fill(0);
            return;
          }
        }
        const step = (44100 / outRate) * (1.0 + Math.max(-0.006, Math.min(0.006, (this.fbCount - 3308) * 0.0000018)));
        for (let i = 0; i < n; i++) {
          if (this.fbCount < 2) {
            this.fbLastL *= 0.994;
            this.fbLastR *= 0.994;
            outL[i] = this.fbLastL;
            outR[i] = this.fbLastR;
            continue;
          }
          const idx0 = this.fbRIdx;
          const idx1 = (idx0 + 1) & mask;
          const t = this.fbFrac;
          this.fbLastL = this.fbL[idx0] + (this.fbL[idx1] - this.fbL[idx0]) * t;
          this.fbLastR = this.fbR[idx0] + (this.fbR[idx1] - this.fbR[idx0]) * t;
          outL[i] = this.fbLastL;
          outR[i] = this.fbLastR;
          this.fbFrac += step;
          const adv = this.fbFrac | 0;
          if (adv > 0) {
            this.fbFrac -= adv;
            const take = adv < this.fbCount ? adv : this.fbCount;
            this.fbRIdx = (this.fbRIdx + take) & mask;
            this.fbCount -= take;
          }
        }
      };
      sp.connect(this.gain);
      this.scriptNode = sp;
      this.ready = true;
      this.backendTag = this.useSab ? 'SPN-SAB' : 'SPN-PM';
    }

    ensureStarted() {
      if (!this.ctx && (window.AudioContext || window.webkitAudioContext)) {
        const AC = window.AudioContext || window.webkitAudioContext;
        try {
          // Use OS native hardware sampleRate + balanced buffer (prevents Android AAudio underrun crackles).
          this.ctx = new AC({ latencyHint: 'balanced' });
        } catch {
          this.ctx = new AC();
        }
        this.gain = this.ctx.createGain();
        this.applyGain();
        this.gain.connect(this.ctx.destination);
      }
      if (this.ctx && this.ctx.state !== 'running' && typeof this.ctx.resume === 'function') {
        this.ctx.resume().catch(() => {});
      }
      if (this.ctx && !this.worklet && !this.workletPromise && !this.scriptNode) {
        if (this.ctx.audioWorklet) {
          const blob = new Blob([WORKLET_CODE], { type: 'text/javascript' });
          const url = URL.createObjectURL(blob);
          this.workletPromise = this.ctx.audioWorklet.addModule(url).then(() => {
            URL.revokeObjectURL(url);
            if (!this.ctx) return;
            const node = new AudioWorkletNode(this.ctx, 'twc98-ring-processor', {
              numberOfInputs: 0,
              numberOfOutputs: 1,
              outputChannelCount: [2],
              processorOptions: { outRate: this.ctx.sampleRate, sab: this.useSab ? this.sab : null, isMobile: this.isMobile },
            });
            node.port.onmessage = (ev) => {
              const m = ev.data || {};
              if (m.type === 'worklet_ready' && this.useSab && !m.sabActive) {
                this.useSab = false;
                this.sabHdr = null;
                this.backendTag = 'AW-PM';
                if (this.onSabModeChange) this.onSabModeChange(false);
              }
            };
            node.connect(this.gain);
            this.worklet = node;
            this.ready = true;
            this.backendTag = this.useSab ? 'AW-SAB' : 'AW-PM';
          }).catch((err) => {
            URL.revokeObjectURL(url);
            this.workletPromise = null;
            this.lastError = String(err && err.message ? err.message : err);
            this.attachScriptFallback();
          });
        } else {
          this.attachScriptFallback();
        }
      }
    }

    applyGain() {
      if (!this.gain || !this.ctx) return;
      const g = this.muted ? 0.0 : Math.max(0.0, Math.min(1.0, this.volumePct / 100.0));
      this.gain.gain.setTargetAtTime(g, this.ctx.currentTime, 0.015);
    }

    setVolume(pct) {
      this.volumePct = Math.max(0, Math.min(100, pct | 0));
      this.applyGain();
    }

    setMuted(m) {
      this.muted = Boolean(m);
      this.applyGain();
    }

    pushPcmBuffer(pcmArrayBuffer) {
      if (!pcmArrayBuffer || (this.useSab && this.sabHdr)) return;
      if (this.worklet) {
        this.worklet.port.postMessage({ pcm: pcmArrayBuffer }, [pcmArrayBuffer]);
        return;
      }
      if (this.scriptNode && this.fbL && this.fbR) {
        const i16 = new Int16Array(pcmArrayBuffer);
        const frames = (i16.length / 2) | 0;
        const cap = RING_CAP_FRAMES;
        const mask = cap - 1;
        for (let i = 0; i < frames; i++) {
          this.fbL[this.fbWIdx] = i16[i * 2] / 32768.0;
          this.fbR[this.fbWIdx] = i16[i * 2 + 1] / 32768.0;
          this.fbWIdx = (this.fbWIdx + 1) & mask;
          if (this.fbCount < cap) this.fbCount++;
          else this.fbRIdx = (this.fbRIdx + 1) & mask;
        }
        if (this.fbCount > 7350) {
          const drop = this.fbCount - 3308;
          this.fbRIdx = (this.fbRIdx + drop) & mask;
          this.fbCount -= drop;
        }
      }
    }

    clear() {
      if (this.useSab && this.sabHdr) {
        Atomics.add(this.sabHdr, 2, 1);
      }
      if (this.worklet && !(this.useSab && this.sabHdr)) {
        this.worklet.port.postMessage({ clear: true });
      }
      if (this.scriptNode) {
        this.fbRIdx = 0;
        this.fbWIdx = 0;
        this.fbCount = 0;
        this.fbFrac = 0;
        this.fbPrimed = false;
      }
    }

    triggerPrebootVoice(slot, w0) {
      if (this.muted || !this.ctx || this.ctx.state !== 'running' || !this.gain) return;
      const cmd = (w0 >>> 24) & 0xff;
      const p1 = (w0 >>> 16) & 0xff;
      const p2 = (w0 >>> 8) & 0xff;
      const freq = 160 + ((p1 ^ p2 ^ cmd ^ slot) & 0x3f) * 14;
      const now = this.ctx.currentTime;
      const osc = this.ctx.createOscillator();
      const g = this.ctx.createGain();
      osc.type = 'triangle';
      osc.frequency.setValueAtTime(freq, now);
      g.gain.setValueAtTime(0.001, now);
      g.gain.linearRampToValueAtTime(0.08, now + 0.01);
      g.gain.exponentialRampToValueAtTime(0.0001, now + 0.14);
      osc.connect(g);
      g.connect(this.gain);
      osc.start(now);
      osc.stop(now + 0.15);
      this.fallbackVoices++;
    }
  }

  window.TwcAudioEngine = TwcAudioEngine;
})();
