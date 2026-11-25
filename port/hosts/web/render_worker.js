// TWC98BO Parallel Render Worker:
// 1) Legacy OffscreenCanvas mode (type: 'init', ?mt=1)
// 2) Phase 2 Multi-Core CPU Band Pool mode (type: 'init_band', ?gpu_vdp=0 / Software GPU)

importScripts('webgl_presenter.js');

let ex = null;
let mem = null;
let presenter = null;
let rHdr = null;
let rSabU8 = null;
let slotStride = 0;
let renderPort = null;
let lastSeq = 0;
let lastRgbaLen = 0;
let lastW = 0;
let lastH = 0;
let visWinStart = 0;
let visWinCount = 0;
let visFps = 0;
let tickQueued = false;
let pendingPresent = false;
let forceRepaint = false;

// Phase 2: Multi-core horizontal band striping state
let bandMode = false;
let bandIdx = 0;
let bandCount = 1;
let bHdr = null;
let bSabU8 = null;
let bPktOffset = 64;
let bRgbaOffset0 = 0;
let bRgbaStride = 0;
let bPort = null;
let bLastSeq = 0;

function processBandJob() {
  if (!bandMode || !ex || !bHdr || !bSabU8) return;
  const seq = Atomics.load(bHdr, 0);
  if (seq === bLastSeq) return;
  bLastSeq = seq;

  const pktLen = Atomics.load(bHdr, 1) >>> 0;
  if (pktLen === 0) {
    const done = (Atomics.add(bHdr, 3, 1) + 1) | 0;
    if (done >= bandCount) Atomics.store(bHdr, 6, seq);
    return;
  }

  const t0 = performance.now();
  const dstPtr = ex.wasm_render_pkt_ptr() >>> 0;
  mem.set(bSabU8.subarray(bPktOffset, bPktOffset + pktLen), dstPtr);

  let ok = false;
  let w = 0;
  let h = 0;
  if (ex.wasm_unpack_render_state_only && ex.wasm_compose_band_rgba) {
    if (ex.wasm_unpack_render_state_only(pktLen) !== 0) {
      w = ex.wasm_frame_w() | 0;
      h = ex.wasm_frame_h() | 0;
      if (w > 0 && h > 0) {
        const y0 = Math.floor((bandIdx * h) / bandCount);
        const y1 = Math.floor(((bandIdx + 1) * h) / bandCount);
        ex.wasm_compose_band_rgba(y0, y1);
        const rowBytes = w * 4;
        const startByte = y0 * rowBytes;
        const byteLen = (y1 - y0) * rowBytes;
        const rgbaPtr = ex.wasm_frame_rgba_ptr() >>> 0;
        const slot = seq & 1;
        const dstOff = bRgbaOffset0 + slot * bRgbaStride + startByte;
        if (byteLen > 0) {
          bSabU8.set(mem.subarray(rgbaPtr + startByte, rgbaPtr + startByte + byteLen), dstOff);
        }
        if (bandIdx === 0) {
          Atomics.store(bHdr, 4, w);
          Atomics.store(bHdr, 5, h);
        }
        ok = true;
      }
    }
  }

  const elapsedUs = Math.max(1, Math.round((performance.now() - t0) * 1000));
  const done = (Atomics.add(bHdr, 3, 1) + 1) | 0;
  if (done >= bandCount) {
    const fw = ok ? w : (Atomics.load(bHdr, 4) | 0);
    const fh = ok ? h : (Atomics.load(bHdr, 5) | 0);
    Atomics.store(bHdr, 7, elapsedUs);
    Atomics.store(bHdr, 6, seq);
    self.postMessage({
      type: 'band_frame_ready',
      seq,
      slot: seq & 1,
      w: fw,
      h: fh,
      renderMs: Number((elapsedUs / 1000).toFixed(2)),
    });
  }
}

function requestRenderTick() {
  pendingPresent = true;
  if (tickQueued) return;
  tickQueued = true;
  queueMicrotask(() => {
    tickQueued = false;
    if (!pendingPresent) return;
    pendingPresent = false;
    processRenderTick();
  });
}

function processRenderTick() {
  if (!ex || !presenter || !rHdr || !rSabU8) return;

  const curSeq = Atomics.load(rHdr, 0);
  if (curSeq === lastSeq) {
    if (forceRepaint && lastRgbaLen > 0 && lastW > 0 && lastH > 0) {
      forceRepaint = false;
      const rgbaPtr = ex.wasm_frame_rgba_ptr() >>> 0;
      presenter.present(mem.subarray(rgbaPtr, rgbaPtr + lastRgbaLen), lastW, lastH);
    }
    return;
  }
  forceRepaint = false;

  if (lastSeq !== 0 && (curSeq - lastSeq) > 1) {
    Atomics.add(rHdr, 7, (curSeq - lastSeq - 1) | 0);
  }

  let slot = 0;
  let seqNow = curSeq;
  for (;;) {
    slot = Atomics.load(rHdr, 10);
    Atomics.store(rHdr, 11, slot);
    const seqAfter = Atomics.load(rHdr, 0);
    if (seqAfter === seqNow) break;
    seqNow = seqAfter;
  }

  const pktLen = Atomics.load(rHdr, 1 + slot) >>> 0;
  const srcOff = 64 + slot * slotStride;
  const t0 = performance.now();

  if (pktLen > 0 && pktLen <= slotStride) {
    const dstPtr = ex.wasm_render_pkt_ptr() >>> 0;
    mem.set(rSabU8.subarray(srcOff, srcOff + pktLen), dstPtr);
  }
  Atomics.store(rHdr, 11, -1);

  if (pktLen > 0 && ex.wasm_unpack_render_state_and_render(pktLen) !== 0) {
    const w = ex.wasm_frame_w() | 0;
    const h = ex.wasm_frame_h() | 0;
    const rgbaPtr = ex.wasm_frame_rgba_ptr() >>> 0;
    const byteLen = w * h * 4;
    if (byteLen > 0) {
      presenter.present(mem.subarray(rgbaPtr, rgbaPtr + byteLen), w, h);
      lastW = w;
      lastH = h;
      lastRgbaLen = byteLen;
      Atomics.store(rHdr, 8, w);
      Atomics.store(rHdr, 9, h);
    }
    const elapsedUs = Math.max(1, Math.round((performance.now() - t0) * 1000));
    Atomics.store(rHdr, 5, elapsedUs);
    Atomics.add(rHdr, 6, 1);
    Atomics.store(rHdr, 4, seqNow);

    const now = performance.now();
    if (visWinStart === 0 || (now - visWinStart) > 2500) {
      visWinStart = now;
      visWinCount = 1;
    } else {
      visWinCount++;
      const dt = now - visWinStart;
      const minWin = visFps === 0 ? 450 : 950;
      if (dt >= minWin) {
        visFps = Number(((visWinCount * 1000) / dt).toFixed(1));
        visWinStart = now;
        visWinCount = 0;
        self.postMessage({ type: 'vis_fps', visFps, visFrames: Atomics.load(rHdr, 6) >>> 0, renderMs: Number((elapsedUs / 1000).toFixed(2)) });
      }
    }
  }
  lastSeq = seqNow;
}

self.onmessage = async (ev) => {
  const msg = ev.data || {};
  try {
    if (msg.type === 'init_band') {
      const { bandIdx: idx, bandCount: cnt, wasmBytes, bandSab, bandPort: port } = msg;
      bandMode = true;
      bandIdx = idx | 0;
      bandCount = Math.max(1, cnt | 0);
      const { instance } = await WebAssembly.instantiate(wasmBytes, {});
      ex = instance.exports;
      mem = new Uint8Array(ex.memory.buffer);
      ex.wasm_machine_reset();

      bHdr = new Int32Array(bandSab, 0, 16);
      bSabU8 = new Uint8Array(bandSab);
      bPktOffset = Atomics.load(bHdr, 8) >>> 0;
      bRgbaOffset0 = Atomics.load(bHdr, 9) >>> 0;
      bRgbaStride = Atomics.load(bHdr, 10) >>> 0;
      bLastSeq = Atomics.load(bHdr, 0) | 0;

      bPort = port;
      bPort.onmessage = () => processBandJob();

      self.postMessage({ type: 'band_ready', bandIdx });
      return;
    }

    if (msg.type === 'init') {
      const { canvas, wasmBytes, renderSab, renderPort: port, isMobile, isSoftwareRenderer, isIntegratedGpu, initW, initH, filterMode, crtMask, hdMode, useGpu } = msg;
      presenter = new self.TwcWebGLPresenter(canvas, {
        isMobile: Boolean(isMobile),
        isSoftwareRenderer: Boolean(isSoftwareRenderer),
        isIntegratedGpu: Boolean(isIntegratedGpu),
        initW: initW | 0,
        initH: initH | 0,
      });
      if (typeof filterMode === 'number') presenter.setFilterMode(filterMode);
      if (typeof crtMask === 'boolean') presenter.setCrtMask(crtMask);
      if (typeof hdMode === 'boolean' && presenter.setHdMode) presenter.setHdMode(hdMode);
      if (typeof useGpu === 'boolean') presenter.setUseGpu(useGpu);

      const { instance } = await WebAssembly.instantiate(wasmBytes, {});
      ex = instance.exports;
      mem = new Uint8Array(ex.memory.buffer);
      ex.wasm_machine_reset();

      rHdr = new Int32Array(renderSab, 0, 16);
      rSabU8 = new Uint8Array(renderSab);
      slotStride = Atomics.load(rHdr, 12) >>> 0;

      renderPort = port;
      renderPort.onmessage = () => requestRenderTick();

      self.postMessage({ type: 'ready', backendName: presenter.backendName });
      requestRenderTick();
      return;
    }

    if (msg.type === 'resize') {
      if (presenter) {
        presenter.setDisplaySize(msg.w | 0, msg.h | 0);
        if (ex && lastRgbaLen > 0 && lastW > 0 && lastH > 0) {
          forceRepaint = true;
          requestRenderTick();
        }
      }
      return;
    }

    if (msg.type === 'video_prefs') {
      if (presenter) {
        if (typeof msg.filterMode === 'number') presenter.setFilterMode(msg.filterMode);
        if (typeof msg.crtMask === 'boolean') presenter.setCrtMask(msg.crtMask);
        if (typeof msg.hdMode === 'boolean' && presenter.setHdMode) presenter.setHdMode(msg.hdMode);
        if (typeof msg.useGpu === 'boolean') presenter.setUseGpu(msg.useGpu);
        if (ex && lastRgbaLen > 0 && lastW > 0 && lastH > 0) {
          forceRepaint = true;
          requestRenderTick();
        }
      }
      return;
    }

    if (msg.type === 'get_canvas_sha') {
      if (!ex || lastRgbaLen <= 0 || !self.crypto || !self.crypto.subtle) {
        self.postMessage({ type: 'canvas_sha', reqId: msg.reqId || 0, hex: null });
        return;
      }
      const rgbaPtr = ex.wasm_frame_rgba_ptr() >>> 0;
      const copy = mem.slice(rgbaPtr, rgbaPtr + lastRgbaLen);
      const dig = await self.crypto.subtle.digest('SHA-256', copy);
      const hex = Array.from(new Uint8Array(dig)).map((b) => b.toString(16).padStart(2, '0')).join('');
      self.postMessage({ type: 'canvas_sha', reqId: msg.reqId || 0, hex });
      return;
    }
  } catch (err) {
    self.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
  }
};
