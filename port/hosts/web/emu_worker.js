// Dedicated Web Worker for TWC98BO Wasm Core: Fixed 60.0 Hz Clock (Solo + Online Rollback).
// Eliminates rAF/setTimeout 4ms clamp drift, uses frame-no-render fallback on GC spikes to protect
// 44.1 kHz continuous audio, and keeps background tabs synchronized in WebRTC 2-Tab P2P.

importScripts('perf_profile.js');

let ex = null;
let mem = null;
let biosCopy = null;
let cartCopy = null;

let netActive = false;
let netLoopRunning = false;
let netNextAt = 0;
let netFramesRun = 0;
let netConsecutiveSkips = 0;

let soloLoopRunning = false;
let soloPaused = true;
let soloSpeedPercent = 100;
let soloNextAt = 0;
let soloStepId = 0;
let soloConsecutiveSkips = 0;
let soloInputPa = 0xff;
let soloInputPb = 0xff;
let soloInputPc = 0xff;
const soloPulseQueue = [];
let soloQuickMatchRem = 0;
let soloReplayBuf = null; // Uint8Array [a,b,c, ...]
let soloReplayCursor = 0;
let soloReplayActive = false;

let liveLocalButtons = 0xff;
let liveLocalSysC = 0xff;
let liveUsecDelta = 0;

function createDashState() {
  return { dashHeld: false, dashDir: 0, lastOutDir: 0, queue: [] };
}
const soloDashSt = createDashState();
const liveDashSt = createDashState();

function updateDashInput(st, baseButtons, wantsDash) {
  const curDir = (~baseButtons) & 0xf0;
  if (!wantsDash || curDir === 0) {
    st.dashHeld = false;
    st.dashDir = 0;
    st.queue.length = 0;
    return curDir;
  }
  const reverseTurn = st.dashHeld && ((curDir & st.dashDir) === 0);
  if (!st.dashHeld || reverseTurn) {
    st.queue.length = 0;
    if (st.lastOutDir === curDir) {
      st.queue.push(0, 0, curDir, curDir, 0, 0);
    } else {
      st.queue.push(curDir, curDir, 0, 0);
    }
    st.dashHeld = true;
    st.dashDir = curDir;
  } else {
    st.dashDir = curDir;
  }
  return curDir;
}

function applyDashQueueToButtons(st, baseButtons) {
  const curDir = (~baseButtons) & 0xf0;
  let outDir = curDir;
  if (st.queue.length > 0) {
    outDir = st.queue.shift();
  }
  st.lastOutDir = outDir;
  return (baseButtons & 0x0f) | ((~outDir) & 0xf0);
}

let debugTelemetryEnabled = false;
let telemetryFrameCounter = 0;

// F1: Direct Worker -> AudioWorklet SharedArrayBuffer ring
let audioHdr = null;
let audioRingI16 = null;
function clearSabAudio() {
  if (audioHdr) Atomics.add(audioHdr, 2, 1);
}

// F3: Parallel Render Worker (OffscreenCanvas WebGL2 + triple-buffered SharedArrayBuffer)
let mtEnabled = false;
let rHdr = null;
let rSabU8 = null;
let rSlotStride = 0;
let renderPort = null;

// Phase 3: WebGL2 GPU VDP1 + VDP2 Compositor (0.06 ms CPU pack -> GPU fragment shader)
let gpuVdpEnabled = false;
let gpuVdp1Enabled = false;
let sh2AotEnabled = true;
let fastCpuEnabled = true;
const pacer = new self.TwcPerfProfile.AdaptivePacer({ mode: 'auto', isMobile: false, gpuVdp: true });
let autoLowResApplied = false;

function syncVdp1MetaFlag() {
  if (!ex || !ex.wasm_set_gpu_vdp1) return;
  // Keep v1_draw_meta_only active for GPU VDP1 AND during 30 FPS CPU fallback (skips 50% CPU VDP1 work).
  const wantMetaOnly = (gpuVdpEnabled && gpuVdp1Enabled) || (!gpuVdpEnabled && pacer.renderEvery >= 2);
  ex.wasm_set_gpu_vdp1(wantMetaOnly ? 1 : 0);
}
const gpuPktPool = [];
function allocGpuPktBuffer(byteLen) {
  for (let i = 0; i < gpuPktPool.length; i++) {
    const b = gpuPktPool[i];
    if (b && b.byteLength >= byteLen) {
      return gpuPktPool.splice(i, 1)[0];
    }
  }
  const allocLen = (byteLen + 65535) & ~65535;
  return new ArrayBuffer(Math.max(allocLen, 768 * 1024));
}

// Phase 2: Multi-Core CPU Band Striping Pool (for ?gpu_vdp=0 / Software GPU)
let bandPoolEnabled = false;
let bandHdr = null;
let bandSabU8 = null;
let bandPorts = [];
let bandPktOffset = 64;
let bandPktMax = 0;
let bandLastSeq = 0;

// Zero-spin pacing: setTimeout for coarse sleep + Atomics.wait (never Android 4ms setTimeout(...,1) clamp)
const waitSab = (typeof SharedArrayBuffer !== 'undefined' && typeof Atomics !== 'undefined' && typeof Atomics.wait === 'function')
  ? new SharedArrayBuffer(4)
  : null;
const waitI32 = waitSab ? new Int32Array(waitSab) : null;
let vsyncF64 = null; // Optional Float64Array(1) written by main thread rAF for gentle VSync phase alignment (mobile)
let isMobileHost = false;

let paceTimer = null;
let paceTargetAt = 0;
let paceCallback = null;
let paceGen = 0;
const paceChannel = (typeof MessageChannel !== 'undefined') ? new MessageChannel() : null;
if (paceChannel) {
  paceChannel.port1.onmessage = () => {
    const fn = paceCallback;
    const target = paceTargetAt;
    const gen = paceGen;
    if (!fn) return;
    let rem = target - performance.now();
    if (rem > 0.5) {
      if (rem >= 5.0) {
        paceTimer = setTimeout(() => {
          if (paceGen !== gen || !paceCallback) return;
          paceChannel.port2.postMessage(null);
        }, Math.floor(rem - 4.0));
        return;
      }
      if (waitI32) {
        for (let iter = 0; iter < 3 && rem > 1.2; iter++) {
          const ms = Math.min(2, Math.floor(rem - 0.4));
          if (ms < 1) break;
          Atomics.wait(waitI32, 0, 0, ms);
          rem = target - performance.now();
        }
      } else if (rem > 0.8) {
        paceTimer = setTimeout(() => {
          if (paceGen !== gen || !paceCallback) return;
          paceChannel.port2.postMessage(null);
        }, 1);
        return;
      }
    }
    paceCallback = null;
    fn();
  };
}

function clearPaceScheduler() {
  paceGen++;
  paceCallback = null;
  if (paceTimer !== null) {
    clearTimeout(paceTimer);
    paceTimer = null;
  }
}

function scheduleAt(targetAt, fn) {
  clearPaceScheduler();
  const gen = paceGen;
  const rem = targetAt - performance.now();
  if (rem <= 0.5) {
    if (paceChannel) {
      paceTargetAt = targetAt;
      paceCallback = fn;
      paceChannel.port2.postMessage(null);
    } else {
      paceTimer = setTimeout(() => {
        if (paceGen === gen) fn();
      }, 0);
    }
    return;
  }
  if (!paceChannel || rem >= 5.0) {
    const sleepMs = paceChannel ? Math.floor(rem - 4.0) : Math.max(0, Math.floor(rem));
    paceTimer = setTimeout(() => {
      if (paceGen !== gen) return;
      if (paceChannel) {
        paceTargetAt = targetAt;
        paceCallback = fn;
        paceChannel.port2.postMessage(null);
      } else {
        fn();
      }
    }, sleepMs);
    return;
  }
  if (waitI32) {
    const ms = Math.min(4, Math.floor(rem - 0.3));
    if (ms >= 1) Atomics.wait(waitI32, 0, 0, ms);
  }
  paceTargetAt = targetAt;
  paceCallback = fn;
  paceChannel.port2.postMessage(null);
}

let fpsWinStart = 0;
let fpsWinCount = 0;
let realizedFps = 60.0;

function recordRealizedTick() {
  const now = performance.now();
  if (fpsWinStart === 0) fpsWinStart = now;
  fpsWinCount++;
  const dt = now - fpsWinStart;
  const minWin = fpsWinCount < 35 ? 450 : 950;
  if (dt >= minWin) {
    realizedFps = Number(((fpsWinCount * 1000) / dt).toFixed(1));
    fpsWinStart = now;
    fpsWinCount = 0;
  }
}

function resetFpsCounter() {
  fpsWinStart = performance.now();
  fpsWinCount = 0;
  realizedFps = Number((60 * (soloSpeedPercent / 100)).toFixed(1));
}

let initMode = 'cold'; // 'cold' | 'cp'
const baseSnapshots = { classic: null, copa: null };
let activeBaseSnapshot = null;
let lastScspSeq = 0;

function hex8(v) {
  return '0x' + ((v >>> 0).toString(16).padStart(8, '0'));
}

function getSyncHashHex() {
  const lo = ex.wasm_sync_hash64_lo() >>> 0;
  const hi = ex.wasm_sync_hash64_hi() >>> 0;
  return '0x' + hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

function captureSnapshot() {
  const len = ex.wasm_save_state() >>> 0;
  if (len === 0) return null;
  const ptr = ex.wasm_state_ptr() >>> 0;
  return mem.slice(ptr, ptr + len);
}

function restoreSnapshot(snap) {
  if (!snap || snap.byteLength === 0) return false;
  const ptr = ex.wasm_state_ptr() >>> 0;
  mem.set(new Uint8Array(snap), ptr);
  const ok = ex.wasm_load_state(snap.byteLength) !== 0;
  if (ok) {
    lastScspSeq = ex.wasm_scsp_event_seq() >>> 0;
    if (ex.wasm_set_sh2_aot) ex.wasm_set_sh2_aot(sh2AotEnabled ? 1 : 0);
    if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(fastCpuEnabled ? 1 : 0);
    syncVdp1MetaFlag();
  }
  return ok;
}

function collectScspEvents() {
  const curSeq = ex.wasm_scsp_event_seq() >>> 0;
  const events = [];
  if (curSeq > lastScspSeq) {
    const start = (curSeq - lastScspSeq > 32) ? (curSeq - 31) : (lastScspSeq + 1);
    for (let s = start; s <= curSeq; s++) {
      events.push({
        seq: s,
        slot: ex.wasm_scsp_event_slot(s) >>> 0,
        w0: ex.wasm_scsp_event_w0(s) >>> 0,
      });
    }
    lastScspSeq = curSeq;
  }
  return { curSeq, events };
}

function collectTelemetry(full = true, includeSyncHash = false) {
  const halted = ex.wasm_halted() !== 0;
  const copaReturnedToTitle = ex.wasm_copa_consume_returned_to_title
    ? (ex.wasm_copa_consume_returned_to_title() !== 0)
    : false;
  if (!full && !halted) {
    return {
      halted,
      copaReturnedToTitle,
      realFps: realizedFps,
      partial: true,
    };
  }
  const tel = {
    wasmLinearMemoryBytes: ex.memory.buffer.byteLength,
    mainPc: hex8(ex.wasm_main_pc() >>> 0),
    slavePc: hex8(ex.wasm_slave_pc() >>> 0),
    halted,
    haltCpu: ex.wasm_halt_cpu ? (ex.wasm_halt_cpu() | 0) : 0,
    iogaReads: ex.wasm_ioga_reads ? (ex.wasm_ioga_reads() >>> 0) : 0,
    cartReads: ex.wasm_cart_reads ? (ex.wasm_cart_reads() >>> 0) : 0,
    soundramWrites: ex.wasm_soundram_writes ? (ex.wasm_soundram_writes() >>> 0) : 0,
    dbgLayers: ex.wasm_get_dbg_layers ? (ex.wasm_get_dbg_layers() >>> 0) : 0,
    gameMode: ex.wasm_get_game_mode ? (ex.wasm_get_game_mode() >>> 0) : 0,
    gkFix: ex.wasm_get_gk_ai_fix ? (ex.wasm_get_gk_ai_fix() >>> 0) : 1,
    markerFix: ex.wasm_get_offscreen_indicator ? (ex.wasm_get_offscreen_indicator() >>> 0) : 1,
    realAudio: ex.wasm_get_real_audio ? (ex.wasm_get_real_audio() >>> 0) : 1,
    m68kEnabled: ex.wasm_m68k_enabled ? (ex.wasm_m68k_enabled() !== 0) : false,
    copaReturnedToTitle,
    realFps: realizedFps,
  };
  if (includeSyncHash) {
    tel.syncHash64 = getSyncHashHex();
  }
  return tel;
}

const rgbaPool = [];
function allocRgbaBuffer(byteLen) {
  while (rgbaPool.length > 0) {
    const buf = rgbaPool.pop();
    if (buf && buf.byteLength === byteLen) return buf;
  }
  return new ArrayBuffer(byteLen);
}

function publishRenderPacketToWorker() {
  if (!mtEnabled || !rHdr || !rSabU8 || !renderPort || !ex.wasm_pack_render_state) return false;
  const pktLen = ex.wasm_pack_render_state() >>> 0;
  if (pktLen === 0 || pktLen > rSlotStride) return false;
  const pubSlot = Atomics.load(rHdr, 10);
  const readSlot = Atomics.load(rHdr, 11);
  let freeSlot = 0;
  for (let s = 0; s < 3; s++) {
    if (s !== pubSlot && s !== readSlot) {
      freeSlot = s;
      break;
    }
  }
  const srcPtr = ex.wasm_render_pkt_ptr() >>> 0;
  const dstOff = 64 + freeSlot * rSlotStride;
  rSabU8.set(mem.subarray(srcPtr, srcPtr + pktLen), dstOff);
  Atomics.store(rHdr, 1 + freeSlot, pktLen | 0);
  Atomics.store(rHdr, 10, freeSlot);
  const seq = (Atomics.add(rHdr, 0, 1) + 1) | 0;
  renderPort.postMessage(seq);
  return true;
}

function publishRenderPacketToBandPool(allowSkipBusy = true) {
  if (!bandPoolEnabled || !bandHdr || !bandSabU8 || bandPorts.length === 0 || !ex.wasm_pack_render_state) {
    return 'fallback';
  }
  const completedSeq = Atomics.load(bandHdr, 6) | 0;
  if (completedSeq !== bandLastSeq) {
    return allowSkipBusy ? 'busy' : 'fallback';
  }
  const pktLen = ex.wasm_pack_render_state() >>> 0;
  if (pktLen === 0 || pktLen > bandPktMax) return 'fallback';
  const srcPtr = ex.wasm_render_pkt_ptr() >>> 0;
  bandSabU8.set(mem.subarray(srcPtr, srcPtr + pktLen), bandPktOffset);
  Atomics.store(bandHdr, 1, pktLen | 0);
  Atomics.store(bandHdr, 3, 0);
  const seq = (Atomics.add(bandHdr, 0, 1) + 1) | 0;
  bandLastSeq = seq;
  for (let i = 0; i < bandPorts.length; i++) {
    bandPorts[i].postMessage(seq);
  }
  return 'dispatched';
}

// alreadyComposed = true skips redundant CPU VDP composition (already done inside NetSession::step_frame)
function renderFramePayload(extra = {}, alreadyComposed = false, includePixels = true, forceFullTelemetry = false) {
  const tr0 = performance.now();
  let w = ex.wasm_frame_w() | 0;
  let h = ex.wasm_frame_h() | 0;
  let rgbaBuf = null;
  let gpuPktBuf = null;
  let gpuPktLen = 0;
  let bandDispatched = false;
  let renderMs = 0;

  if (includePixels) {
    let handled = false;
    if (gpuVdpEnabled && ex.wasm_pack_gpu_vdp2) {
      const pktLen = ex.wasm_pack_gpu_vdp2(0) >>> 0;
      if (pktLen > 0) {
        const pktPtr = ex.wasm_render_pkt_ptr() >>> 0;
        gpuPktBuf = allocGpuPktBuffer(pktLen);
        new Uint8Array(gpuPktBuf, 0, pktLen).set(mem.subarray(pktPtr, pktPtr + pktLen));
        gpuPktLen = pktLen;
        w = ex.wasm_frame_w() | 0;
        h = ex.wasm_frame_h() | 0;
        renderMs = performance.now() - tr0;
        handled = true;
      }
    }
    if (!handled && mtEnabled && publishRenderPacketToWorker()) {
      renderMs = Number((Atomics.load(rHdr, 5) / 1000).toFixed(2));
      w = (Atomics.load(rHdr, 8) | 0) || w;
      h = (Atomics.load(rHdr, 9) | 0) || h;
      handled = true;
    }
    if (!handled && bandPoolEnabled) {
      const bst = publishRenderPacketToBandPool(!forceFullTelemetry);
      if (bst === 'dispatched' || bst === 'busy') {
        bandDispatched = true;
        renderMs = Number((Atomics.load(bandHdr, 7) / 1000).toFixed(2));
        w = (Atomics.load(bandHdr, 4) | 0) || w;
        h = (Atomics.load(bandHdr, 5) | 0) || h;
        handled = true;
      }
    }
    if (!handled) {
      if (alreadyComposed && ex.wasm_copy_frame_rgba) {
        ex.wasm_copy_frame_rgba();
      } else {
        ex.wasm_render_frame_rgba();
      }
      w = ex.wasm_frame_w() | 0;
      h = ex.wasm_frame_h() | 0;
      const rgbaPtr = ex.wasm_frame_rgba_ptr() >>> 0;
      const byteLen = w * h * 4;
      rgbaBuf = allocRgbaBuffer(byteLen);
      new Uint8Array(rgbaBuf).set(mem.subarray(rgbaPtr, rgbaPtr + byteLen));
      renderMs = performance.now() - tr0;
    }
  }

  const pcmFrames = ex.wasm_consume_audio_pcm ? (ex.wasm_consume_audio_pcm(1536) >>> 0) : 0;
  let pcmCopy = null;
  let pcmFramesOut = 0;
  if (pcmFrames > 0) {
    const pcmPtr = ex.wasm_audio_pcm_ptr() >>> 0;
    if (audioHdr && audioRingI16) {
      const cap = 16384;
      const wSeq = Atomics.load(audioHdr, 0);
      const rSeq = Atomics.load(audioHdr, 1);
      const queued = (wSeq - rSeq) | 0;
      if (queued >= 0 && queued + pcmFrames <= cap - 64) {
        const src = new Int16Array(ex.memory.buffer, pcmPtr, pcmFrames * 2);
        const idx = wSeq & (cap - 1);
        const first = Math.min(pcmFrames, cap - idx);
        audioRingI16.set(src.subarray(0, first * 2), idx * 2);
        if (first < pcmFrames) {
          audioRingI16.set(src.subarray(first * 2), 0);
        }
        Atomics.store(audioHdr, 0, (wSeq + pcmFrames) | 0);
      }
    } else {
      pcmCopy = new Int16Array(ex.memory.buffer, pcmPtr, pcmFrames * 2).slice();
      pcmFramesOut = pcmFrames;
    }
  }

  const m68kOn = ex.wasm_m68k_enabled ? (ex.wasm_m68k_enabled() !== 0) : false;
  const scsp = (!m68kOn || debugTelemetryEnabled)
    ? collectScspEvents()
    : { curSeq: (ex.wasm_scsp_event_seq() >>> 0), events: [] };

  const sendFullTel = forceFullTelemetry || debugTelemetryEnabled || ((telemetryFrameCounter++ % 15) === 0);
  const includeSync = forceFullTelemetry || (debugTelemetryEnabled && sendFullTel);

  const transfers = [];
  if (rgbaBuf) transfers.push(rgbaBuf);
  if (gpuPktBuf) transfers.push(gpuPktBuf);
  if (pcmCopy) transfers.push(pcmCopy.buffer);
  if (extra.txWire) transfers.push(extra.txWire);

  return {
    payload: {
      w,
      h,
      rgba: rgbaBuf,
      gpuPkt: gpuPktBuf,
      gpuPktLen,
      bandDispatched,
      renderMs,
      pcm: pcmCopy ? pcmCopy.buffer : null,
      pcmFrames: pcmFramesOut,
      scspSeq: scsp.curSeq,
      scspEvents: scsp.events,
      telemetry: collectTelemetry(sendFullTel, includeSync),
      ...extra,
    },
    transfers,
  };
}

// Exact PC-parity cold boot for Classic or Copa America (twc98_pc.cpp:boot_mode_clean)
function runColdBootModeClean({ copaMode, copaSeed, gkFix = true, markerFix = false, realAudio, dbgLayers, bootFrames = 820, reportProgress = true }) {
  if (!biosCopy || !cartCopy) throw new Error('BIOS/Cart buffers missing');
  const key = copaMode ? 'copa' : 'classic';

  // Ensure cart/bios loaded
  mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
  if (ex.wasm_init_cart(cartCopy.byteLength) !== 1) {
    throw new Error('wasm_init_cart failed');
  }
  mem.set(biosCopy, ex.wasm_bios_ptr() >>> 0);
  if (ex.wasm_init_cold_boot(biosCopy.byteLength, cartCopy.byteLength) !== 1) {
    throw new Error('wasm_init_cold_boot failed');
  }

  if (ex.wasm_set_dbg_layers) ex.wasm_set_dbg_layers(dbgLayers & 0x0f);
  ex.wasm_set_gk_ai_fix(1);
  if (ex.wasm_set_offscreen_indicator) ex.wasm_set_offscreen_indicator(0);
  ex.wasm_set_real_audio(realAudio ? 1 : 0);
  if (ex.wasm_set_sh2_aot) ex.wasm_set_sh2_aot(sh2AotEnabled ? 1 : 0);
  // Keep Cold Boot warmup 100% bit-exact (fast_cpu=0) so Frame-0 snapshot matches across all hosts;
  // fastCpuEnabled activates immediately once gameplay starts after warmup.
  if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(0);
  if (ex.wasm_set_gpu_vdp1) ex.wasm_set_gpu_vdp1((gpuVdpEnabled && gpuVdp1Enabled) ? 1 : 0);
  if (ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();

  ex.wasm_set_game_mode(copaMode ? 1 : 0, (copaSeed >>> 0) || 0x1998ca01);
  if (ex.wasm_copa_reset_play_flags) ex.wasm_copa_reset_play_flags();

  const total = Math.max(0, bootFrames | 0);
  ex.wasm_set_input_ports(0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x67);
  for (let f = 1; f <= total; f++) {
    if (ex.wasm_step_frame_no_render(10000, 0) === 0) {
      throw new Error(`Cold Boot warmup halted at frame ${f}`);
    }
    if (ex.wasm_copa_on_frame_tick) ex.wasm_copa_on_frame_tick();
    if (reportProgress && (f === total || (f % 100) === 0)) {
      self.postMessage({ type: 'boot_progress', frame: f, total });
    }
  }

  // Capture booted Classic WRAM originals right after BIOS decrypts EPRG so future Classic restores never wipe WRAM.
  if (!copaMode) {
    ex.wasm_set_game_mode(0, (copaSeed >>> 0) || 0x1998ca01);
  }

  // Enforce GK AI fix after BIOS has finished decrypting EPRG into High WRAM before capturing the clean base snapshot.
  ex.wasm_set_gk_ai_fix(1);
  if (ex.wasm_copa_reset_play_flags) ex.wasm_copa_reset_play_flags();
  if (ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();
  lastScspSeq = ex.wasm_scsp_event_seq() >>> 0;

  const snap = captureSnapshot();
  if (snap) baseSnapshots[key] = snap;
  activeBaseSnapshot = snap;
  if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(fastCpuEnabled ? 1 : 0);
}

function stopNetLoop() {
  netLoopRunning = false;
  clearPaceScheduler();
}

function stopSoloLoop() {
  soloLoopRunning = false;
  clearPaceScheduler();
}

function nextSoloFramePorts() {
  if (soloReplayActive && soloReplayBuf && soloReplayCursor + 3 <= soloReplayBuf.length) {
    const a = soloReplayBuf[soloReplayCursor++];
    const b = soloReplayBuf[soloReplayCursor++];
    const c = soloReplayBuf[soloReplayCursor++];
    let replayFinished = false;
    if (soloReplayCursor >= soloReplayBuf.length) {
      soloReplayActive = false;
      replayFinished = true;
    }
    return { pa: a, pb: b, pc: c, replayFinished };
  }

  let pa = soloInputPa & 0xff;
  let pb = soloInputPb & 0xff;
  let pc = soloInputPc & 0xff;

  pa = applyDashQueueToButtons(soloDashSt, pa);

  for (let i = soloPulseQueue.length - 1; i >= 0; i--) {
    const p = soloPulseQueue[i];
    if (p.port === 'a') pa &= p.mask;
    else if (p.port === 'b') pb &= p.mask;
    else if (p.port === 'c') pc &= p.mask;
    p.rem -= 1;
    if (p.rem <= 0) soloPulseQueue.splice(i, 1);
  }

  if (soloQuickMatchRem > 0) {
    const step = 180 - soloQuickMatchRem;
    if (step >= 4 && step <= 10) pc &= ~0x01;
    else if (step >= 25 && step <= 32) pc &= ~0x10;
    else if (step >= 70 && step <= 78) pa &= ~0x01;
    else if (step >= 110 && step <= 118) pa &= ~0x01;
    else if (step >= 150 && step <= 158) pa &= ~0x01;
    soloQuickMatchRem--;
  }

  return { pa, pb, pc, replayFinished: false };
}

function runSoloSingleFrame(forceRender = false) {
  if (!ex || netActive || netLoopRunning) return;
  const periodMs = (1000.0 / 60.0) * (100.0 / soloSpeedPercent);
  const { pa, pb, pc, replayFinished } = nextSoloFramePorts();

  const behindMs = performance.now() - soloNextAt;
  const pacerSkips = !forceRender && soloSpeedPercent === 100 && !pacer.shouldRenderFrame(soloStepId);
  // In GPU VDP mode, skipping render only saves ~0.07 ms of packing (never catches up a slow CPU sim)
  // and would throw away visible frames (e.g. 44 sim FPS -> 8 vis FPS). Only catchup-skip in CPU render mode.
  const catchupSkip = !forceRender
    && !gpuVdpEnabled
    && soloSpeedPercent === 100
    && Boolean(ex.wasm_step_frame_no_render)
    && soloConsecutiveSkips < 2
    && behindMs > periodMs * 0.9;
  const canSkipRender = pacerSkips || catchupSkip;

  const deferCpuCompose = gpuVdpEnabled || mtEnabled || bandPoolEnabled;
  const t0 = performance.now();
  const lagMs = Math.max(0, t0 - soloNextAt);
  ex.wasm_set_input_ports(pa, pb, pc, 0xff, 0xff, 0x7f, 0x67);
  if (canSkipRender || deferCpuCompose) {
    ex.wasm_step_frame_no_render(10000, 1);
    ex.wasm_copa_on_frame_tick();
    soloConsecutiveSkips = catchupSkip ? (soloConsecutiveSkips + 1) : 0;
  } else {
    ex.wasm_step_rounds(10000);
    ex.wasm_copa_on_frame_tick();
    soloConsecutiveSkips = 0;
  }
  const simMs = performance.now() - t0;
  soloStepId++;
  recordRealizedTick();

  const pacerRes = pacer.recordFrame({ simMs, skipped: catchupSkip, simFps: realizedFps });
  if (pacerRes.changed) {
    syncVdp1MetaFlag();
    if (pacerRes.state.autoLowResRequested && !autoLowResApplied && !gpuVdpEnabled && isMobileHost) {
      autoLowResApplied = true;
      if (ex.wasm_set_low_res_224p) ex.wasm_set_low_res_224p(1);
      if (ex.wasm_set_low_res_half_width) ex.wasm_set_low_res_half_width(1);
    }
  }
  const emitPacerState = pacerRes.changed || ((soloStepId % 30) === 0);

  const { payload, transfers } = renderFramePayload({
    type: 'frame',
    stepId: soloStepId,
    advanced: true,
    skippedRender: canSkipRender,
    simMs,
    lagMs,
    usedPorts: { a: pa, b: pb, c: pc },
    replayFinished,
    txWire: null,
    netStats: null,
    pacerState: emitPacerState ? pacerRes.state : null,
  }, false, !canSkipRender);
  self.postMessage(payload, transfers);
}

function tickSoloOnce() {
  if (!ex || !soloLoopRunning || soloPaused || netActive || netLoopRunning) {
    soloLoopRunning = false;
    return;
  }
  runSoloSingleFrame(false);
  if (!soloLoopRunning || soloPaused || netActive || netLoopRunning) return;

  const periodMs = (1000.0 / 60.0) * (100.0 / soloSpeedPercent);
  // Keep simulation + 44.1 kHz audio clock at 60.0 Hz; gently nudge phase away from the rAF VSync edge
  soloNextAt += periodMs;
  const after = performance.now();
  // Only apply VSync phase nudge on mobile (keeps desktop strictly at 60.000 Hz so 44.1 kHz audio never drifts)
  if (isMobileHost && soloSpeedPercent === 100 && vsyncF64) {
    const lastRaf = vsyncF64[0];
    if (lastRaf > 0) {
      const dtRaf = after - lastRaf;
      if (dtRaf >= 0 && dtRaf < 60) {
        const phase = dtRaf % periodMs;
        // Avoid straddling the rAF boundary (< 3.0 ms after rAF or > 14.8 ms right before next rAF)
        if (phase < 3.0 || phase > 14.8) {
          soloNextAt -= 0.10;
        }
      }
    }
  }
  if (after - soloNextAt > Math.max(45, periodMs * 2.5)) {
    soloNextAt = after;
  }
  scheduleAt(soloNextAt, tickSoloOnce);
}

function tickNetStepOnce() {
  if (!ex || !netActive || !netLoopRunning) return;
  const t0 = performance.now();

  const liveBtns = applyDashQueueToButtons(liveDashSt, liveLocalButtons);
  ex.wasm_net_queue_local_input(liveBtns & 0xff, liveLocalSysC & 0xff);
  const pktPtr = ex.wasm_net_pkt_ptr() >>> 0;
  const pktLen = ex.wasm_net_pkt_size() >>> 0;
  const txWire = mem.slice(pktPtr, pktPtr + pktLen);

  const nudgeMs = Math.max(-3.5, Math.min(3.5, liveUsecDelta / 1000));
  const periodMs = (1000.0 / 60.0) + nudgeMs;
  const behindMs = t0 - netNextAt;
  const lagMs = Math.max(0, behindMs);
  const pacerNetSkip = !pacer.shouldRenderFrame(netFramesRun);
  const catchupNetSkip = !gpuVdpEnabled
    && Boolean(ex.wasm_net_step_frame_ex)
    && netConsecutiveSkips < 2
    && behindMs > (periodMs * 0.9);
  const canSkipNetRender = Boolean(ex.wasm_net_step_frame_ex) && (pacerNetSkip || catchupNetSkip);

  const deferCpuCompose = gpuVdpEnabled || mtEnabled || bandPoolEnabled;
  const advanced = ex.wasm_net_step_frame_ex
    ? (ex.wasm_net_step_frame_ex((!deferCpuCompose && !canSkipNetRender) ? 1 : 0) !== 0)
    : (ex.wasm_net_step_frame() !== 0);

  const sendPixels = advanced && !canSkipNetRender;
  if (sendPixels) {
    netConsecutiveSkips = 0;
  } else if (advanced && catchupNetSkip) {
    netConsecutiveSkips++;
  }

  if (advanced) {
    netFramesRun++;
    recordRealizedTick();
  }
  const simMs = performance.now() - t0;
  liveUsecDelta = ex.wasm_net_recommended_usec_delta() | 0;
  const netPacerRes = advanced ? pacer.recordFrame({ simMs, skipped: catchupNetSkip, simFps: realizedFps }) : null;
  if (netPacerRes && netPacerRes.changed) {
    syncVdp1MetaFlag();
  }
  const emitNetPacer = netPacerRes && (netPacerRes.changed || ((netFramesRun % 30) === 0));

  // When deferCpuCompose, wasm_net_step_frame_ex(0) skipped CPU VDP composition so GPU VDP2 or workers do it.
  const { payload, transfers } = renderFramePayload({
    type: 'frame',
    stepId: netFramesRun,
    advanced,
    skippedRender: advanced && !sendPixels,
    simMs,
    lagMs,
    txWire: txWire.buffer,
    pacerState: emitNetPacer ? netPacerRes.state : null,
    netStats: {
      curFrame: ex.wasm_net_cur_frame() >>> 0,
      rollbacks: ex.wasm_net_rollback_count() >>> 0,
      rbFrames: ex.wasm_net_rollback_frames() >>> 0,
      maxDepth: ex.wasm_net_max_rollback_depth() >>> 0,
      stalls: ex.wasm_net_stall_frames() >>> 0,
      syncOk: ex.wasm_net_sync_checks_passed() >>> 0,
      desyncs: ex.wasm_net_desync_count() >>> 0,
      usecDelta: liveUsecDelta,
      firstDesyncFrame: ex.wasm_net_first_desync_frame ? (ex.wasm_net_first_desync_frame() >>> 0) : 0,
      firstDesyncLocal: ex.wasm_net_first_desync_local_hash ? hex8(ex.wasm_net_first_desync_local_hash() >>> 0) : '00000000',
      firstDesyncRemote: ex.wasm_net_first_desync_remote_hash ? hex8(ex.wasm_net_first_desync_remote_hash() >>> 0) : '00000000',
    },
  }, /*alreadyComposed=*/!deferCpuCompose, /*includePixels=*/sendPixels);
  self.postMessage(payload, transfers);

  netNextAt += periodMs;
  const after = performance.now();
  if (after - netNextAt > 85) netNextAt = after;
  scheduleAt(netNextAt, tickNetStepOnce);
}

self.onmessage = async (ev) => {
  const msg = ev.data || {};
  try {
    if (msg.type === 'recycle_rgba') {
      if (msg.buf && msg.buf.byteLength > 0 && rgbaPool.length < 4) {
        rgbaPool.push(msg.buf);
      }
      return;
    }
    if (msg.type === 'recycle_gpu_pkt') {
      if (msg.buf && msg.buf.byteLength > 0 && gpuPktPool.length < 5) {
        gpuPktPool.push(msg.buf);
      }
      return;
    }
    if (msg.type === 'set_fps_mode') {
      const st = pacer.setMode(msg.mode || 'auto');
      syncVdp1MetaFlag();
      self.postMessage({ type: 'pacer_state', pacerState: st });
      return;
    }
    if (msg.type === 'set_gpu_vdp') {
      gpuVdpEnabled = Boolean(msg.enabled);
      if (typeof msg.gpuVdp1 === 'boolean') {
        gpuVdp1Enabled = Boolean(msg.gpuVdp1);
      }
      pacer.setEnvironment({ gpuVdp: gpuVdpEnabled });
      syncVdp1MetaFlag();
      if (ex && ex.wasm_gpu_vdp2_invalidate) ex.wasm_gpu_vdp2_invalidate();
      if (ex) {
        const { payload, transfers } = renderFramePayload({ type: 'mode_ack', pacerState: pacer.getState() }, false, true, true);
        self.postMessage(payload, transfers);
      }
      return;
    }
    if (msg.type === 'set_band_pool') {
      if (msg.bandSab && msg.bandPorts && msg.bandPorts.length > 0 && typeof SharedArrayBuffer !== 'undefined') {
        bandHdr = new Int32Array(msg.bandSab, 0, 16);
        bandSabU8 = new Uint8Array(msg.bandSab);
        bandPktOffset = Atomics.load(bandHdr, 8) >>> 0;
        bandPktMax = (Atomics.load(bandHdr, 9) >>> 0) - bandPktOffset;
        bandLastSeq = Atomics.load(bandHdr, 0) | 0;
        bandPorts = msg.bandPorts;
        bandPoolEnabled = true;
      } else {
        bandPoolEnabled = false;
        bandHdr = null;
        bandSabU8 = null;
        bandPorts = [];
      }
      return;
    }
    if (msg.type === 'set_debug_mode') {
      debugTelemetryEnabled = Boolean(msg.enabled);
      return;
    }
    if (msg.type === 'set_audio_sab') {
      if (!msg.enabled) {
        audioHdr = null;
        audioRingI16 = null;
      }
      return;
    }
    if (msg.type === 'init') {
      stopSoloLoop();
      stopNetLoop();
      netActive = false;
      const {
        wasmBytes, cart, bios, epCp,
        v1Vram, v1Regs, v2Vram, v2Cram, v2Regs,
        copaMode = true, copaSeed = 0x1998ca01, gkFix = true, markerFix = true, realAudio = true,
        hardMode = false, ruleMode = 0, ruleValue = 90,
        dbgLayers = 0x08, lowRes224p = false, lowResHalfWidth = false, warmupFrames = 820,
        idleSkip = true, audioSab = null, renderSab = null, renderPort: rPort = null, mt = false,
        gpuVdp = false, gpuVdp1 = false, sh2Aot = true, fastCpu = true, fpsMode = 'auto',
        bandSab = null, bandPorts: initBandPorts = null, vsyncSab = null, isMobile = false,
      } = msg;

      isMobileHost = Boolean(isMobile);
      vsyncF64 = (vsyncSab && typeof SharedArrayBuffer !== 'undefined') ? new Float64Array(vsyncSab) : null;
      gpuVdpEnabled = Boolean(gpuVdp);
      gpuVdp1Enabled = Boolean(gpuVdp1);
      sh2AotEnabled = Boolean(sh2Aot);
      fastCpuEnabled = Boolean(fastCpu);
      autoLowResApplied = Boolean(lowRes224p && lowResHalfWidth);
      pacer.setEnvironment({ isMobile: isMobileHost, gpuVdp: gpuVdpEnabled });
      pacer.setMode(fpsMode);
      pacer.reset();
      if (bandSab && initBandPorts && initBandPorts.length > 0 && typeof SharedArrayBuffer !== 'undefined') {
        bandHdr = new Int32Array(bandSab, 0, 16);
        bandSabU8 = new Uint8Array(bandSab);
        bandPktOffset = Atomics.load(bandHdr, 8) >>> 0;
        bandPktMax = (Atomics.load(bandHdr, 9) >>> 0) - bandPktOffset;
        bandLastSeq = Atomics.load(bandHdr, 0) | 0;
        bandPorts = initBandPorts;
        bandPoolEnabled = true;
      } else {
        bandPoolEnabled = false;
        bandHdr = null;
        bandSabU8 = null;
        bandPorts = [];
      }

      if (audioSab && typeof SharedArrayBuffer !== 'undefined') {
        audioHdr = new Int32Array(audioSab, 0, 4);
        audioRingI16 = new Int16Array(audioSab, 16, 16384 * 2);
      } else {
        audioHdr = null;
        audioRingI16 = null;
      }

      if (mt && renderSab && rPort && typeof SharedArrayBuffer !== 'undefined') {
        rHdr = new Int32Array(renderSab, 0, 16);
        rSabU8 = new Uint8Array(renderSab);
        rSlotStride = Atomics.load(rHdr, 12) >>> 0;
        renderPort = rPort;
        mtEnabled = true;
      } else {
        mtEnabled = false;
        rHdr = null;
        rSabU8 = null;
        renderPort = null;
      }

      const t0 = performance.now();
      const { instance } = await WebAssembly.instantiate(wasmBytes, {});
      ex = instance.exports;
      mem = new Uint8Array(ex.memory.buffer);
      cartCopy = new Uint8Array(cart);
      biosCopy = new Uint8Array(bios);
      if (ex.wasm_set_low_res_224p) ex.wasm_set_low_res_224p(lowRes224p ? 1 : 0);
      if (ex.wasm_set_low_res_half_width) ex.wasm_set_low_res_half_width(lowResHalfWidth ? 1 : 0);
      if (ex.wasm_set_idle_skip) ex.wasm_set_idle_skip(idleSkip ? 1 : 0);
      if (ex.wasm_set_sh2_aot) ex.wasm_set_sh2_aot(sh2AotEnabled ? 1 : 0);
      if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(fastCpuEnabled ? 1 : 0);
      syncVdp1MetaFlag();

      if (epCp && v1Vram && v1Regs && v2Vram && v2Cram && v2Regs) {
        initMode = 'cp';
        mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
        if (ex.wasm_init_cart(cartCopy.byteLength) !== 1) throw new Error('wasm_init_cart failed');
        mem.set(new Uint8Array(epCp), ex.wasm_cp_ptr() >>> 0);
        mem.set(biosCopy, ex.wasm_bios_ptr() >>> 0);
        mem.set(new Uint8Array(v1Vram), ex.wasm_vdp1_vram_ptr() >>> 0);
        mem.set(new Uint8Array(v1Regs), ex.wasm_vdp1_regs_ptr() >>> 0);
        mem.set(new Uint8Array(v2Vram), ex.wasm_vdp2_vram_ptr() >>> 0);
        mem.set(new Uint8Array(v2Cram), ex.wasm_vdp2_cram_ptr() >>> 0);
        mem.set(new Uint8Array(v2Regs), ex.wasm_vdp2_regs_ptr() >>> 0);
        if (ex.wasm_init_cpn8(epCp.byteLength, biosCopy.byteLength, 1) !== 1) {
          throw new Error('wasm_init_cpn8 failed');
        }
        if (ex.wasm_set_dbg_layers) ex.wasm_set_dbg_layers(dbgLayers & 0x0f);
        ex.wasm_set_gk_ai_fix(1);
        if (ex.wasm_set_offscreen_indicator) ex.wasm_set_offscreen_indicator(0);
        ex.wasm_set_real_audio(realAudio ? 1 : 0);
        ex.wasm_set_game_mode(copaMode ? 1 : 0, copaSeed >>> 0);
        if (ex.wasm_set_match_rules) ex.wasm_set_match_rules(hardMode ? 1 : 0, ruleMode | 0, ruleValue | 0);
        if (ex.wasm_set_coop_mode) ex.wasm_set_coop_mode(msg.coopMode ? 1 : 0);
        activeBaseSnapshot = captureSnapshot();
        lastScspSeq = ex.wasm_scsp_event_seq() >>> 0;
      } else {
        initMode = 'cold';
        runColdBootModeClean({ copaMode, copaSeed, gkFix, markerFix, realAudio, dbgLayers, bootFrames: warmupFrames, reportProgress: true });
        if (ex.wasm_set_match_rules) ex.wasm_set_match_rules(hardMode ? 1 : 0, ruleMode | 0, ruleValue | 0);
        if (ex.wasm_set_coop_mode) ex.wasm_set_coop_mode(msg.coopMode ? 1 : 0);
      }
      clearSabAudio();

      const coldInitMs = Number((performance.now() - t0).toFixed(1));
      const { payload, transfers } = renderFramePayload({
        type: 'ready',
        initMode,
        coldInitMs,
        wasmBinaryBytes: wasmBytes.byteLength,
        mtEnabled,
        sabAudio: Boolean(audioHdr),
      }, false, true, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'switch_edition_clean') {
      if (!ex) return;
      stopSoloLoop();
      stopNetLoop();
      netActive = false;
      soloInputPa = 0xff;
      soloInputPb = 0xff;
      soloInputPc = 0xff;
      soloPulseQueue.length = 0;
      soloQuickMatchRem = 0;
      soloReplayActive = false;
      soloDashSt.dashHeld = false;
      soloDashSt.dashDir = 0;
      soloDashSt.lastOutDir = 0;
      soloDashSt.queue.length = 0;
      const {
        copaMode, copaSeed = 0x1998ca01, gkFix = true, markerFix = true, realAudio = true,
        hardMode = false, ruleMode = 0, ruleValue = 90, dbgLayers = 0x08, forceReboot = false,
      } = msg;
      const key = copaMode ? 'copa' : 'classic';
      if (!forceReboot && baseSnapshots[key] && initMode === 'cold') {
        if (cartCopy) mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
        restoreSnapshot(baseSnapshots[key]);
        if (ex.wasm_set_dbg_layers) ex.wasm_set_dbg_layers(dbgLayers & 0x0f);
        ex.wasm_set_gk_ai_fix(1);
        if (ex.wasm_set_offscreen_indicator) ex.wasm_set_offscreen_indicator(0);
        ex.wasm_set_real_audio(realAudio ? 1 : 0);
        ex.wasm_set_game_mode(copaMode ? 1 : 0, copaSeed >>> 0);
        if (ex.wasm_copa_reset_play_flags) ex.wasm_copa_reset_play_flags();
      } else if (initMode === 'cold') {
        runColdBootModeClean({ copaMode, copaSeed, gkFix, markerFix, realAudio, dbgLayers, bootFrames: 820, reportProgress: false });
      } else {
        if (cartCopy) mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
        if (ex.wasm_set_offscreen_indicator) ex.wasm_set_offscreen_indicator(0);
        ex.wasm_set_game_mode(copaMode ? 1 : 0, copaSeed >>> 0);
        if (ex.wasm_copa_reset_play_flags) ex.wasm_copa_reset_play_flags();
      }
      if (ex.wasm_reset_inputs) ex.wasm_reset_inputs();
      if (ex.wasm_set_match_rules) ex.wasm_set_match_rules(hardMode ? 1 : 0, ruleMode | 0, ruleValue | 0);
      if (ex.wasm_set_coop_mode) ex.wasm_set_coop_mode(msg.coopMode ? 1 : 0);
      if (ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();
      clearSabAudio();
      const { payload, transfers } = renderFramePayload({
        type: 'edition_ready',
        reqId: msg.reqId || 0,
      }, false, true, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'set_coop_mode') {
      if (!ex || netActive || netLoopRunning) return;
      if (ex.wasm_set_coop_mode) {
        ex.wasm_set_coop_mode(msg.coopMode ? 1 : 0);
      }
    } else if (msg.type === 'set_match_rules') {
      if (!ex || netActive || netLoopRunning) return;
      if (ex.wasm_set_match_rules) {
        ex.wasm_set_match_rules(msg.hardMode ? 1 : 0, msg.ruleMode | 0, msg.ruleValue | 0);
      }
      if (ex.wasm_copa_on_frame_tick) ex.wasm_copa_on_frame_tick();
    } else if (msg.type === 'set_visual_prefs') {
      if (!ex) return;
      if (ex.wasm_set_offscreen_indicator) {
        ex.wasm_set_offscreen_indicator(0);
      }
      if (typeof msg.lowRes224p === 'boolean' && ex.wasm_set_low_res_224p) {
        ex.wasm_set_low_res_224p(msg.lowRes224p ? 1 : 0);
      }
    } else if (msg.type === 'set_mode') {
      if (!ex) return;
      if (typeof msg.copaMode === 'boolean') {
        ex.wasm_set_game_mode(msg.copaMode ? 1 : 0, (msg.seed >>> 0) || 0x1998ca01);
      }
      ex.wasm_set_gk_ai_fix(1);
      if (ex.wasm_set_offscreen_indicator) {
        ex.wasm_set_offscreen_indicator(0);
      }
      if (typeof msg.realAudio === 'boolean') {
        ex.wasm_set_real_audio(msg.realAudio ? 1 : 0);
      }
      if (typeof msg.dbgLayers === 'number' && ex.wasm_set_dbg_layers) {
        ex.wasm_set_dbg_layers(msg.dbgLayers & 0x0f);
      }
      if (typeof msg.lowRes224p === 'boolean' && ex.wasm_set_low_res_224p) {
        ex.wasm_set_low_res_224p(msg.lowRes224p ? 1 : 0);
      }
      const { payload, transfers } = renderFramePayload({ type: 'mode_ack' }, false, true, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'handshake_prep') {
      if (!ex) return;
      stopSoloLoop();
      stopNetLoop();
      netActive = false;
      if (typeof msg.idleSkip === 'boolean' && ex.wasm_set_idle_skip) ex.wasm_set_idle_skip(msg.idleSkip ? 1 : 0);
      if (typeof msg.fastCpu === 'boolean') fastCpuEnabled = msg.fastCpu;
      if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(fastCpuEnabled ? 1 : 0);
      if (typeof msg.lowRes224p === 'boolean' && ex.wasm_set_low_res_224p) ex.wasm_set_low_res_224p(msg.lowRes224p ? 1 : 0);
      const targetCopa = Boolean(msg.copaMode);
      const key = targetCopa ? 'copa' : 'classic';
      if (initMode === 'cold') {
        if (!baseSnapshots[key]) {
          runColdBootModeClean({
            copaMode: targetCopa,
            copaSeed: msg.copaSeed >>> 0,
            gkFix: Boolean(msg.gkFix),
            markerFix: msg.markerFix !== false,
            realAudio: Boolean(msg.realAudio),
            dbgLayers: msg.dbgLayers ?? 0x08,
            bootFrames: 820,
            reportProgress: false,
          });
        } else {
          if (cartCopy) mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
          if (!restoreSnapshot(baseSnapshots[key])) {
            throw new Error('Failed restoring deterministic Frame-0 snapshot');
          }
        }
      } else if (activeBaseSnapshot) {
        if (cartCopy) mem.set(cartCopy, ex.wasm_cart_ptr() >>> 0);
        if (!restoreSnapshot(activeBaseSnapshot)) {
          throw new Error('Failed restoring CP Frame-0 snapshot');
        }
      }

      if (ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();
      clearSabAudio();
      if (typeof msg.idleSkip === 'boolean' && ex.wasm_set_idle_skip) ex.wasm_set_idle_skip(msg.idleSkip ? 1 : 0);
      if (typeof msg.fastCpu === 'boolean') fastCpuEnabled = msg.fastCpu;
      if (ex.wasm_set_fast_cpu) ex.wasm_set_fast_cpu(fastCpuEnabled ? 1 : 0);
      if (typeof msg.lowRes224p === 'boolean' && ex.wasm_set_low_res_224p) ex.wasm_set_low_res_224p(msg.lowRes224p ? 1 : 0);
      if (ex.wasm_set_dbg_layers) ex.wasm_set_dbg_layers((msg.dbgLayers ?? 0x08) & 0x0f);
      ex.wasm_set_gk_ai_fix(1);
      if (ex.wasm_set_offscreen_indicator) ex.wasm_set_offscreen_indicator(0);
      ex.wasm_set_real_audio(msg.realAudio ? 1 : 0);
      ex.wasm_set_game_mode(targetCopa ? 1 : 0, (msg.copaSeed >>> 0) || 0x1998ca01);
      if (ex.wasm_set_match_rules) {
        ex.wasm_set_match_rules(msg.hardMode ? 1 : 0, msg.ruleMode | 0, msg.ruleValue | 0);
      }
      const targetCoop = Boolean(msg.coopMode);
      if (ex.wasm_set_coop_mode) {
        ex.wasm_set_coop_mode(targetCoop ? 1 : 0);
      }
      if (ex.wasm_copa_on_frame_tick) ex.wasm_copa_on_frame_tick();
      if (ex.wasm_copa_reset_play_flags) ex.wasm_copa_reset_play_flags();

      if (ex.wasm_net_reset_ex) {
        ex.wasm_net_reset_ex(
          (msg.localPlayer || 0) & 1,
          Math.max(1, Math.min(6, msg.delayFrames || 3)),
          targetCopa ? 1 : 0,
          1,
          targetCoop ? 1 : 0
        );
      } else {
        ex.wasm_net_reset(
          (msg.localPlayer || 0) & 1,
          Math.max(1, Math.min(6, msg.delayFrames || 3)),
          targetCopa ? 1 : 0,
          1
        );
      }
      liveLocalButtons = 0xff;
      liveLocalSysC = 0xff;
      liveUsecDelta = 0;
      netFramesRun = 0;
      netActive = Boolean(msg.armNet);
      const { payload, transfers } = renderFramePayload({
        type: 'handshake_ready',
        reqId: msg.reqId || 0,
        armNet: netActive,
      }, false, true, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'net_start_loop') {
      if (!ex || !netActive) return;
      stopSoloLoop();
      stopNetLoop();
      netLoopRunning = true;
      netConsecutiveSkips = 0;
      netNextAt = performance.now();
      resetFpsCounter();
      tickNetStepOnce();
    } else if (msg.type === 'net_stop') {
      stopNetLoop();
      netActive = false;
      clearSabAudio();
      self.postMessage({ type: 'net_stop_ack', reqId: msg.reqId || 0 });
    } else if (msg.type === 'solo_set_paused') {
      soloPaused = Boolean(msg.paused);
      if (soloPaused) {
        stopSoloLoop();
        if (ex && ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();
        clearSabAudio();
      } else if (ex && !netActive && !netLoopRunning && !soloLoopRunning) {
        soloLoopRunning = true;
        soloConsecutiveSkips = 0;
        soloNextAt = performance.now();
        resetFpsCounter();
        tickSoloOnce();
      }
    } else if (msg.type === 'solo_set_speed') {
      soloSpeedPercent = Math.max(25, Math.min(200, (msg.speedPercent | 0) || 100));
      soloNextAt = performance.now();
      resetFpsCounter();
    } else if (msg.type === 'solo_set_input') {
      soloInputPa = (msg.pa !== undefined) ? (msg.pa & 0xff) : 0xff;
      soloInputPb = (msg.pb !== undefined) ? (msg.pb & 0xff) : 0xff;
      soloInputPc = (msg.pc !== undefined) ? (msg.pc & 0xff) : 0xff;
      updateDashInput(soloDashSt, soloInputPa, Boolean(msg.dash));
    } else if (msg.type === 'solo_pulse_port') {
      if (!netActive && !netLoopRunning) {
        soloPulseQueue.push({ port: msg.port, mask: msg.mask & 0xff, rem: Math.max(1, msg.frames | 0) });
      }
    } else if (msg.type === 'solo_quick_match') {
      if (!netActive && !netLoopRunning) {
        soloQuickMatchRem = 180;
        if (soloPaused) {
          soloPaused = false;
          soloLoopRunning = true;
          soloConsecutiveSkips = 0;
          soloNextAt = performance.now();
          resetFpsCounter();
          tickSoloOnce();
        }
      }
    } else if (msg.type === 'solo_force_win') {
      if (!ex || netActive || netLoopRunning || !ex.wasm_bus_read || !ex.wasm_bus_write) {
        self.postMessage({ type: 'force_win_ack', ok: false, reason: 'NOT_IN_SOLO' });
        return;
      }
      const timeRem = ex.wasm_bus_read(0, 0x0607654E, 2) & 0xffff;
      if (timeRem === 0 || timeRem > 300) {
        self.postMessage({ type: 'force_win_ack', ok: false, reason: 'NO_ACTIVE_MATCH' });
        return;
      }
      const p1Goals = ex.wasm_bus_read(0, 0x06076558, 4) >>> 0;
      const p2Goals = ex.wasm_bus_read(0, 0x0607655C, 2) & 0xffff;
      const winGoals = Math.max(p1Goals, p2Goals + 1, 3);
      ex.wasm_bus_write(0, 0x06076558, 4, winGoals >>> 0);
      ex.wasm_bus_write(0, 0x0607654E, 2, 1);
      ex.wasm_bus_write(0, 0x060970C4, 2, 1);
      if (soloPaused) {
        soloPaused = false;
        soloLoopRunning = true;
        soloConsecutiveSkips = 0;
        soloNextAt = performance.now();
        resetFpsCounter();
        tickSoloOnce();
      }
      self.postMessage({ type: 'force_win_ack', ok: true, winGoals, p2Goals });
    } else if (msg.type === 'solo_set_replay') {
      if (msg.active && msg.framesBuf) {
        soloReplayBuf = new Uint8Array(msg.framesBuf);
        soloReplayCursor = 0;
        soloReplayActive = true;
      } else {
        soloReplayActive = false;
        soloReplayBuf = null;
        soloReplayCursor = 0;
      }
    } else if (msg.type === 'solo_step_one') {
      if (!ex || netActive || netLoopRunning) return;
      soloPaused = true;
      stopSoloLoop();
      runSoloSingleFrame(true);
    } else if (msg.type === 'set_live_input' || msg.type === 'net_set_input') {
      const bVal = (msg.localButtons !== undefined) ? msg.localButtons : msg.buttons;
      const cVal = (msg.localSysC !== undefined) ? msg.localSysC : msg.sysC;
      liveLocalButtons = (bVal !== undefined) ? (bVal & 0xff) : 0xff;
      liveLocalSysC = (cVal !== undefined) ? (cVal & 0xff) : 0xff;
      updateDashInput(liveDashSt, liveLocalButtons, Boolean(msg.dash));
    } else if (msg.type === 'net_rx') {
      if (!ex || !netActive || !msg.wire) return;
      const wire = new Uint8Array(msg.wire);
      const ptr = ex.wasm_net_pkt_ptr() >>> 0;
      mem.set(wire, ptr);
      ex.wasm_net_receive_wire_pkt(wire.byteLength);
    } else if (msg.type === 'step') {
      if (!ex || netActive || netLoopRunning) return;
      const t0 = performance.now();
      ex.wasm_set_input_ports(msg.pa ?? 0xff, msg.pb ?? 0xff, msg.pc ?? 0xff, 0xff, 0xff, 0x7f, 0x67);
      ex.wasm_step_rounds(10000);
      ex.wasm_copa_on_frame_tick();
      const simMs = performance.now() - t0;

      const { payload, transfers } = renderFramePayload({
        type: 'frame',
        stepId: msg.stepId || 0,
        advanced: true,
        simMs,
        txWire: null,
        netStats: null,
      }, false, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'save_snapshot') {
      if (!ex) return;
      const snap = captureSnapshot();
      const transfers = snap ? [snap.buffer] : [];
      self.postMessage({
        type: 'snapshot_saved',
        reqId: msg.reqId || 0,
        slot: msg.slot,
        purpose: msg.purpose || 'slot',
        snapshot: snap ? snap.buffer : null,
      }, transfers);
    } else if (msg.type === 'load_snapshot') {
      if (!ex || !msg.snapshot) return;
      const ok = restoreSnapshot(msg.snapshot);
      if (ex.wasm_clear_audio_pcm) ex.wasm_clear_audio_pcm();
      clearSabAudio();
      const { payload, transfers } = renderFramePayload({
        type: 'snapshot_loaded',
        reqId: msg.reqId || 0,
        slot: msg.slot,
        purpose: msg.purpose || 'slot',
        ok,
      }, false, true, true);
      self.postMessage(payload, transfers);
    } else if (msg.type === 'bench_600') {
      if (!ex) return;
      const preSnap = captureSnapshot();
      const times = [];
      const simTimes = [];
      const t0 = performance.now();
      for (let f = 1; f <= 600; f++) {
        let pc = 0xff;
        if (f >= 60 && f <= 65) pc = 0xfe;
        else if (f >= 90 && f <= 95) pc = 0xef;
        ex.wasm_set_input_ports(0xff, 0xff, pc, 0xff, 0xff, 0x7f, 0x67);
        const tf0 = performance.now();
        const ok = ex.wasm_step_rounds(10000) !== 0;
        const ts1 = performance.now();
        ex.wasm_render_frame_rgba();
        const tf1 = performance.now();
        simTimes.push(ts1 - tf0);
        times.push(tf1 - tf0);
        if (!ok) break;
      }
      const totalMs = performance.now() - t0;
      const sorted = [...times].sort((a, b) => a - b);
      const sortedSim = [...simTimes].sort((a, b) => a - b);
      const avgMs = totalMs / Math.max(1, times.length);
      const fps = 1000 / avgMs;
      const benchObj = {
        frames: times.length,
        totalMs: Number(totalMs.toFixed(2)),
        fps: Number(fps.toFixed(2)),
        realtimeRatio: Number((fps / 60).toFixed(3)),
        avgMs: Number(avgMs.toFixed(3)),
        p50Ms: Number(sorted[Math.floor(sorted.length * 0.5)].toFixed(3)),
        p95Ms: Number(sorted[Math.floor(sorted.length * 0.95)].toFixed(3)),
        simP50Ms: Number(sortedSim[Math.floor(sortedSim.length * 0.5)].toFixed(3)),
        simP95Ms: Number(sortedSim[Math.floor(sortedSim.length * 0.95)].toFixed(3)),
      };
      if (preSnap) restoreSnapshot(preSnap);
      const { payload, transfers } = renderFramePayload({
        type: 'bench_done',
        reqId: msg.reqId || 0,
        bench: benchObj,
      }, false, true, true);
      self.postMessage(payload, transfers);
    }
  } catch (err) {
    self.postMessage({ type: 'error', reqId: msg.reqId || 0, message: String(err && err.message ? err.message : err) });
  }
};
