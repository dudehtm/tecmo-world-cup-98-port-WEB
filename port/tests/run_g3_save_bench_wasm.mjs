import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';

const g3Dir = path.resolve(process.argv[2] || 'port');
const repoRoot = path.resolve(process.argv[3] || '.');

const wasmPath = path.join(g3Dir, 'build-wasm-g4', 'twc98_g4.wasm');
const nativePath = path.join(g3Dir, 'validation_g4', 'native', 'save_bench_report.json');
const wasmOutPath = path.join(g3Dir, 'validation_g4', 'wasm', 'save_bench_wasm.json');

const tInit0 = performance.now();
const wasmBytes = fs.readFileSync(wasmPath);
const wasmMod = new WebAssembly.Module(wasmBytes);
const imports = WebAssembly.Module.imports(wasmMod);
assert.equal(imports.length, 0);

const inst = new WebAssembly.Instance(wasmMod, {});
const ex = inst.exports;
const mem = new Uint8Array(ex.memory.buffer);
const dv = new DataView(ex.memory.buffer);
const wasmMemBytes = ex.memory.buffer.byteLength;
assert.ok(wasmMemBytes <= 256 * 1024 * 1024, `Wasm linear memory ${wasmMemBytes} > 256 MiB`);

const cartBytes = fs.readFileSync(path.join(g3Dir, 'data', 'twcup98_cart_20m.bin'));
const biosBytes = fs.readFileSync(path.join(g3Dir, 'data', 'bios_euro_be.bin'));
const epCp = fs.readFileSync(path.join(g3Dir, 'data', 'endpoint_frame8175.cp'));
const visDir = path.join(repoRoot, '.local', 'experiments', '012-h4-control-trace', 'visual-sync1027b-neutral');
const v1Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-vram.bin'));
const v1Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-registers.bin'));
const v2Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-vram.bin'));
const v2Cram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-cram.bin'));
const v2Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-registers.bin'));

mem.set(cartBytes, ex.wasm_cart_ptr());
assert.equal(ex.wasm_init_cart(cartBytes.length), 1);
mem.set(epCp, ex.wasm_cp_ptr());
mem.set(biosBytes, ex.wasm_bios_ptr());
mem.set(v1Vram, ex.wasm_vdp1_vram_ptr());
mem.set(v1Regs, ex.wasm_vdp1_regs_ptr());
mem.set(v2Vram, ex.wasm_vdp2_vram_ptr());
mem.set(v2Cram, ex.wasm_vdp2_cram_ptr());
mem.set(v2Regs, ex.wasm_vdp2_regs_ptr());
assert.equal(ex.wasm_init_cpn8(epCp.length, biosBytes.length, 1), 1);
const coldInitMs = performance.now() - tInit0;

const hex32 = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');
const digestHex = (ptr, idx) => {
  const start = ptr + idx * 32;
  let out = '';
  for (let i = 0; i < 32; i++) out += mem[start + i].toString(16).padStart(2, '0');
  return out;
};

function stepRange(fStart, fEnd, renderEach) {
  for (let f = fStart; f < fEnd; f++) {
    let pa = 0xff, pb = 0xff, pc = 0xff;
    if (f >= 5 && f <= 10) pc = 0xfe;
    else if (f >= 25 && f <= 34) pc = 0xdf;
    if (f >= 60) {
      if ((f % 60) < 20) pa = 0xdf;
      else if ((f % 45) < 6) pa = 0xfe;
      pb = ((f % 40) < 20) ? 0x7e : 0xef;
    }
    ex.wasm_set_input_ports(pa, pb, pc, 0xff, 0xff, 0x7f, 0x67);
    assert.equal(ex.wasm_step_rounds(10000), 1, `Halted at f=${f}`);
    if (renderEach) ex.wasm_render_frame();
  }
}

stepRange(0, 60, false);
ex.wasm_render_frame();
ex.wasm_refresh_state();
const digPtr = ex.wasm_digests_ptr();
const cpuPtr = ex.wasm_cpu_snapshot_ptr();
const f60Hw = digestHex(digPtr, 0);
const f60Fr = digestHex(digPtr, 6);

const snapLen = ex.wasm_save_state() >>> 0;
assert.ok(snapLen > 0 && snapLen < ex.wasm_state_capacity(), `Bad snapLen=${snapLen}`);
const statePtr = ex.wasm_state_ptr();
const savedCopy = mem.slice(statePtr, statePtr + snapLen);
const snapSha = crypto.createHash('sha256').update(savedCopy).digest('hex');

// Negative corruption tests
mem[statePtr] ^= 0xff;
assert.equal(ex.wasm_load_state(snapLen), 0, 'Corrupt magic should fail');
mem.set(savedCopy, statePtr);
mem[statePtr + 1000] ^= 0x5a;
assert.equal(ex.wasm_load_state(snapLen), 0, 'Corrupt payload should fail');
mem.set(savedCopy, statePtr);
assert.equal(ex.wasm_load_state(24), 0, 'Truncated snapshot should fail');

// Advance f60..f240 reference
stepRange(60, 240, false);
ex.wasm_render_frame();
ex.wasm_refresh_state();
const f240RefHw = digestHex(digPtr, 0);
const f240RefFr = digestHex(digPtr, 6);

// Restore f=60 & verify replay bit-exact
mem.set(savedCopy, statePtr);
assert.equal(ex.wasm_load_state(snapLen), 1, 'Valid load_state failed');
ex.wasm_refresh_state();
assert.equal(digestHex(digPtr, 0), f60Hw);
assert.equal(digestHex(digPtr, 6), f60Fr);

stepRange(60, 240, false);
ex.wasm_render_frame();
ex.wasm_refresh_state();
const f240ReplayHw = digestHex(digPtr, 0);
const f240ReplayFr = digestHex(digPtr, 6);
const f240MainPc = hex32(dv.getUint32(cpuPtr + 16 * 4, true));
const f240SlavePc = hex32(dv.getUint32(cpuPtr + (28 + 16) * 4, true));
assert.equal(f240ReplayHw, f240RefHw);
assert.equal(f240ReplayFr, f240RefFr);

// Latency benchmark for save/load
let saveMsTotal = 0;
let loadMsTotal = 0;
for (let it = 0; it < 10; it++) {
  const ts0 = performance.now();
  const n = ex.wasm_save_state() >>> 0;
  const ts1 = performance.now();
  assert.equal(n, snapLen);
  assert.equal(ex.wasm_load_state(n), 1);
  const ts2 = performance.now();
  saveMsTotal += (ts1 - ts0);
  loadMsTotal += (ts2 - ts1);
}

// Warm 600-frame benchmark with render_frame
const tb0 = performance.now();
stepRange(240, 840, true);
const bench600Ms = performance.now() - tb0;
const warmFps = 600 * 1000 / bench600Ms;
const warmMsPerFrame = bench600Ms / 600;
const realtimeRatio = warmFps / 60;

ex.wasm_refresh_state();
const f840MainPc = hex32(dv.getUint32(cpuPtr + 16 * 4, true));
const f840CartReads = ex.wasm_cart_reads() >>> 0;
const f840Fr = digestHex(digPtr, 6);
const rssBytes = process.memoryUsage().rss;
assert.ok(rssBytes <= 512 * 1024 * 1024, `Node RSS ${rssBytes} > 512 MiB`);

const native = JSON.parse(fs.readFileSync(nativePath, 'utf8'));
assert.equal(snapLen, native.snapshot_bytes, 'Snapshot byte size mismatch PC vs Wasm');
assert.equal(snapSha, native.snapshot_sha256, 'Snapshot SHA-256 mismatch PC vs Wasm!');
assert.equal(f240MainPc, native.f240_main_pc);
assert.equal(f240SlavePc, native.f240_slave_pc);
assert.equal(f240ReplayHw, native.f240_high_wram_sha256);
assert.equal(f240ReplayFr, native.f240_frame_sha256);
assert.equal(f840MainPc, native.f840_main_pc);
assert.equal(f840CartReads, native.f840_cart_reads);
assert.equal(f840Fr, native.f840_frame_sha256);

const report = {
  schema: 'g3_save_bench_wasm_v1',
  status: 'PASS_SAVE_RESTORE_AND_BUDGETS_PC_WASM_BIT_EXACT',
  wasm_binary_bytes: wasmBytes.length,
  wasm_linear_memory_bytes: wasmMemBytes,
  wasm_imports: 0,
  node_rss_bytes: rssBytes,
  snapshot_bytes: snapLen,
  snapshot_sha256: snapSha,
  save_ms_avg: Number((saveMsTotal / 10).toFixed(3)),
  load_ms_avg: Number((loadMsTotal / 10).toFixed(3)),
  cold_init_ms: Number(coldInitMs.toFixed(1)),
  warm_600f_with_render_ms: Number(bench600Ms.toFixed(1)),
  warm_fps: Number(warmFps.toFixed(2)),
  warm_ms_per_frame: Number(warmMsPerFrame.toFixed(3)),
  realtime_ratio_60fps: Number(realtimeRatio.toFixed(3)),
  f240_frame_sha256: f240ReplayFr,
  f840_frame_sha256: f840Fr,
};
fs.mkdirSync(path.dirname(wasmOutPath), { recursive: true });
fs.writeFileSync(wasmOutPath, JSON.stringify(report, null, 2) + '\n', 'utf8');
console.log(JSON.stringify(report, null, 2));
