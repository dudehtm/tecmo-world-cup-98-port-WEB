import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';

const g3Dir = path.resolve(process.argv[2] || 'port');
const repoRoot = path.resolve(process.argv[3] || '.');

const wasmPath = path.join(g3Dir, 'build-wasm-g4', 'twc98_g4.wasm');
const nativeReportPath = path.join(g3Dir, 'validation_g4', 'native', 'g3_report.json');
const wasmOutPath = path.join(g3Dir, 'validation_g4', 'wasm', 'g3_wasm_parity.json');

const wasmBytes = fs.readFileSync(wasmPath);
const wasmMod = new WebAssembly.Module(wasmBytes);
const imports = WebAssembly.Module.imports(wasmMod);
assert.equal(imports.length, 0, `Expected zero Wasm imports, got ${JSON.stringify(imports)}`);

const inst = new WebAssembly.Instance(wasmMod, {});
const ex = inst.exports;
const mem = new Uint8Array(ex.memory.buffer);
const dv = new DataView(ex.memory.buffer);

const wasmMemBytes = ex.memory.buffer.byteLength;
assert.ok(wasmMemBytes <= 256 * 1024 * 1024, `Wasm memory ${wasmMemBytes} exceeds 256 MiB`);

const hex32 = (v) => '0x' + (v >>> 0).toString(16).padStart(8, '0');
const hex8 = (v) => '0x' + (v & 0xff).toString(16).padStart(2, '0');
const digestHex = (ptr, idx) => {
  const start = ptr + idx * 32;
  let out = '';
  for (let i = 0; i < 32; i++) out += mem[start + i].toString(16).padStart(2, '0');
  return out;
};

// Load ROM / BIOS / CP / VDP fixtures
const cartBytes = fs.readFileSync(path.join(g3Dir, 'data', 'twcup98_cart_20m.bin'));
const biosBytes = fs.readFileSync(path.join(g3Dir, 'data', 'bios_euro_be.bin'));
const epCp = fs.readFileSync(path.join(g3Dir, 'data', 'endpoint_frame8175.cp'));
const visCp = fs.readFileSync(path.join(g3Dir, 'data', 'visual_sync1027b_f8175.cp'));
const visDir = path.join(repoRoot, '.local', 'experiments', '012-h4-control-trace', 'visual-sync1027b-neutral');
const v1Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-vram.bin'));
const v1Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-registers.bin'));
const v2Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-vram.bin'));
const v2Cram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-cram.bin'));
const v2Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-registers.bin'));

// ---- Phase A: Cart ROM Unit & 17-sample verification in Wasm ----
ex.wasm_clear_cart();
ex.wasm_machine_reset();
ex.wasm_bus_read(0, 0x02200000, 1);
assert.equal(ex.wasm_halted(), 1, 'Expected halt on cart read when no cart loaded');

assert.ok(cartBytes.length <= ex.wasm_cart_capacity(), 'Cart buffer too small');
mem.set(cartBytes, ex.wasm_cart_ptr());
assert.equal(ex.wasm_init_cart(cartBytes.length), 1, 'wasm_init_cart failed');
ex.wasm_machine_reset();

const kCartSamples = [
  [0x02000000, 'cart-02000000.bin'],
  [0x02000100, 'cart-02000100.bin'],
  [0x021fffc0, 'cart-021fffc0.bin'],
  [0x02200000, 'cart-02200000.bin'],
  [0x022fffc0, 'cart-022fffc0.bin'],
  [0x02300000, 'cart-02300000.bin'],
  [0x023fffc0, 'cart-023fffc0.bin'],
  [0x02400000, 'cart-02400000.bin'],
  [0x027fffc0, 'cart-027fffc0.bin'],
  [0x02800000, 'cart-02800000.bin'],
  [0x02bfffc0, 'cart-02bfffc0.bin'],
  [0x02c00000, 'cart-02c00000.bin'],
  [0x02ffffc0, 'cart-02ffffc0.bin'],
  [0x03000000, 'cart-03000000.bin'],
  [0x033fffc0, 'cart-033fffc0.bin'],
  [0x03400000, 'cart-03400000.bin'],
  [0x04ffffb0, 'cart-04ffffb0.bin'],
];

const bankDir = path.join(repoRoot, '.local', 'experiments', '003-memory-map', 'bank-selected');
for (const [addr, fname] of kCartSamples) {
  const expected = fs.readFileSync(path.join(bankDir, fname));
  for (const mirror of [0, 0x20000000]) {
    for (let i = 0; i < expected.length; i++) {
      assert.equal(ex.wasm_bus_read(0, (addr + mirror + i) >>> 0, 1) >>> 0, expected[i]);
    }
    for (let i = 0; i < expected.length; i += 2) {
      const exp16 = (expected[i] << 8) | expected[i + 1];
      assert.equal(ex.wasm_bus_read(0, (addr + mirror + i) >>> 0, 2) >>> 0, exp16);
    }
    for (let i = 0; i < expected.length; i += 4) {
      const exp32 = ((expected[i] << 24) | (expected[i + 1] << 16) | (expected[i + 2] << 8) | expected[i + 3]) >>> 0;
      assert.equal(ex.wasm_bus_read(0, (addr + mirror + i) >>> 0, 4) >>> 0, exp32);
    }
  }
}

// Bank select check in Wasm
const bank0Word = ex.wasm_bus_read(0, 0x02200000, 4) >>> 0;
ex.wasm_bus_write(0, 0x00100075, 1, 0x01);
assert.equal(ex.wasm_gamebank_select(), 1);
assert.equal(ex.wasm_bus_read(0, 0x02200000, 4) >>> 0, 0);
ex.wasm_bus_write(0, 0x00100075, 1, 0x00);
assert.equal(ex.wasm_gamebank_select(), 0);
assert.equal(ex.wasm_bus_read(0, 0x02200000, 4) >>> 0, bank0Word);

// 315-5881 A-Bus protection window check in Wasm (matches verify_g3_subtasks1_3.cpp B-RE-05)
ex.wasm_machine_reset();
assert.equal(ex.wasm_bus_write(0, 0x04fffff1, 1, 0x00), 1);
assert.equal(ex.wasm_bus_write(0, 0x04fffff8, 2, 0x0ad2), 1);
assert.equal(ex.wasm_bus_write(0, 0x04fffffa, 2, 0x0069), 1);
assert.equal(ex.wasm_bus_write(0, 0x04fffffc, 2, 0x1212), 1);
assert.equal(ex.wasm_bus_write(0, 0x04fffff1, 1, 0x01), 1);
const decW0 = ex.wasm_bus_read(0, 0x24fffffc, 4) >>> 0;
assert.equal(ex.wasm_bus_write(0, 0x24fffff1, 1, 0x00), 1);
const regW0 = ex.wasm_bus_read(0, 0x04fffff8, 2) >>> 0;
assert.equal(ex.wasm_halted(), 0);
assert.notEqual(decW0, 0);
assert.equal(regW0, 0x0ad2);

// ---- Phase B: IOGA + SMPC Unit Tests in Wasm ----
ex.wasm_machine_reset();
ex.wasm_set_input_ports(0xdf, 0xbe, 0xee, 0xf7, 0xfe, 0x7f, 0x67);
assert.equal(ex.wasm_bus_read(0, 0x00400001, 1), 0xdf);
assert.equal(ex.wasm_bus_read(0, 0x20400021, 1), 0xdf);
assert.equal(ex.wasm_bus_read(0, 0x00400003, 1), 0xbe);
assert.equal(ex.wasm_bus_read(0, 0x00400005, 1), 0xee);
assert.equal(ex.wasm_bus_read(0, 0x00400009, 1), 0xf7);
assert.equal(ex.wasm_bus_read(0, 0x0040000b, 1), 0xfe);
assert.equal(ex.wasm_bus_read(0, 0x20400000, 4) >>> 0, 0x00df00be);
assert.equal(ex.wasm_bus_read(0, 0x00100075, 1), 0x7f);
assert.equal(ex.wasm_bus_read(0, 0x20100077, 1), 0x67);
ex.wasm_bus_write(0, 0x0010007b, 1, 0x18);
ex.wasm_bus_write(0, 0x00100077, 1, 0x10);
assert.equal(ex.wasm_bus_read(0, 0x00100077, 1), 0x77);

// ---- Phase C & D: Run Scenarios and Assert Bit-Exact Parity with Native PC ----
function runWasmScenario(name, cpBytes, seedVdp, pa, pb, pc, frames) {
  assert.ok(cpBytes.length <= ex.wasm_cp_capacity());
  mem.set(cpBytes, ex.wasm_cp_ptr());
  mem.set(biosBytes, ex.wasm_bios_ptr());
  if (seedVdp) {
    mem.set(v1Vram, ex.wasm_vdp1_vram_ptr());
    mem.set(v1Regs, ex.wasm_vdp1_regs_ptr());
    mem.set(v2Vram, ex.wasm_vdp2_vram_ptr());
    mem.set(v2Cram, ex.wasm_vdp2_cram_ptr());
    mem.set(v2Regs, ex.wasm_vdp2_regs_ptr());
  }
  assert.equal(ex.wasm_init_cpn8(cpBytes.length, biosBytes.length, seedVdp ? 1 : 0), 1);
  ex.wasm_set_input_ports(pa, pb, pc, 0xff, 0xff, 0x7f, 0x67);

  let ok = true;
  for (let f = 0; f < frames; f++) {
    if (ex.wasm_step_rounds(10000) === 0) {
      ok = false;
      break;
    }
    ex.wasm_render_frame();
  }
  ex.wasm_refresh_state();
  const digPtr = ex.wasm_digests_ptr();
  const cpuPtr = ex.wasm_cpu_snapshot_ptr();
  const mainPc = dv.getUint32(cpuPtr + 16 * 4, true);
  const slavePc = dv.getUint32(cpuPtr + (28 + 16) * 4, true);

  const iogaPtr = ex.wasm_ioga_log_ptr();
  const iogaStride = ex.wasm_ioga_entry_size();
  const nLog = Math.min(ex.wasm_ioga_log_count(), 16);
  const firstIoga = [];
  for (let i = 0; i < nLog; i++) {
    const off = iogaPtr + i * iogaStride;
    firstIoga.push({
      cpu: mem[off + 9],
      pc: hex32(dv.getUint32(off + 0, true)),
      addr: hex32(dv.getUint32(off + 4, true)),
      val: hex8(mem[off + 8]),
      wr: mem[off + 10],
    });
  }

  return {
    name,
    completed: ok,
    halted: ex.wasm_halted() !== 0,
    rounds_done: ex.wasm_rounds_done(),
    main_steps: ex.wasm_main_steps(),
    slave_steps: ex.wasm_slave_steps(),
    main_pc: hex32(mainPc),
    slave_pc: hex32(slavePc),
    ioga_reads: ex.wasm_ioga_reads(),
    ioga_writes: ex.wasm_ioga_writes(),
    smpc_input_reads: ex.wasm_smpc_input_reads(),
    cart_reads: ex.wasm_cart_reads(),
    vbi_count: ex.wasm_vbi_count(),
    vbo_count: ex.wasm_vbo_count(),
    vdp1_draws: ex.wasm_vdp1_draws(),
    mmio_log_count: ex.wasm_mmio_log_count(),
    high_wram_sha256: digestHex(digPtr, 0),
    low_wram_sha256: digestHex(digPtr, 1),
    vdp1_vram_sha256: digestHex(digPtr, 2),
    vdp2_vram_sha256: digestHex(digPtr, 3),
    frame_sha256: digestHex(digPtr, 6),
    first_ioga_reads: firstIoga,
  };
}

const native = JSON.parse(fs.readFileSync(nativeReportPath, 'utf8'));

function assertParity(w, n, checkIogaList) {
  for (const k of [
    'completed', 'halted', 'rounds_done', 'main_steps', 'slave_steps',
    'main_pc', 'slave_pc', 'ioga_reads', 'smpc_input_reads', 'cart_reads',
    'vbi_count', 'vbo_count', 'high_wram_sha256', 'low_wram_sha256', 'frame_sha256',
  ]) {
    assert.equal(w[k], n[k], `Parity mismatch in ${w.name}.${k}: wasm=${w[k]} native=${n[k]}`);
  }
  if (checkIogaList) {
    for (const k of ['ioga_writes', 'vdp1_draws', 'vdp1_vram_sha256', 'vdp2_vram_sha256']) {
      assert.equal(w[k], n[k], `Parity mismatch in ${w.name}.${k}: wasm=${w[k]} native=${n[k]}`);
    }
    assert.deepEqual(w.first_ioga_reads, n.first_ioga_reads, `first_ioga_reads mismatch in ${w.name}`);
  }
}

const g2Wasm = runWasmScenario('g2_regression_ep8175', epCp, false, 0xff, 0xff, 0xff, 12);
assertParity(g2Wasm, native.g2_regression, false);
assert.equal(g2Wasm.mmio_log_count, native.g2_regression.mmio_log_count);

const specs = [
  ['ep8175_neutral', epCp, 0xff, 0xff, 0xff, 12],
  ['ep8175_p1_up', epCp, 0xdf, 0xff, 0xff, 12],
  ['ep8175_p1_left', epCp, 0x7f, 0xff, 0xff, 12],
  ['ep8175_p2_act', epCp, 0xff, 0xbe, 0xff, 12],
  ['ep8175_coin_start', epCp, 0xff, 0xff, 0xee, 12],
  ['vis8175_neutral_2f', visCp, 0xff, 0xff, 0xff, 2],
  ['vis8175_p1_up_2f', visCp, 0xdf, 0xff, 0xff, 2],
];

const wasmScenarios = [];
for (let i = 0; i < specs.length; i++) {
  const [name, cp, pa, pb, pc, frames] = specs[i];
  const w = runWasmScenario(name, cp, true, pa, pb, pc, frames);
  assertParity(w, native.scenarios[i], true);
  wasmScenarios.push(w);
}

const report = {
  schema: 'g3_wasm_parity_v1',
  wasm_binary_bytes: wasmBytes.length,
  wasm_linear_memory_bytes: wasmMemBytes,
  wasm_imports: imports.length,
  cart_17_samples_ok: true,
  prot_guard_ok: true,
  unit_ioga_smpc_ok: true,
  pc_wasm_bit_exact_scenarios: wasmScenarios.length + 1,
  g2_regression: g2Wasm,
  scenarios: wasmScenarios,
};

fs.mkdirSync(path.dirname(wasmOutPath), { recursive: true });
fs.writeFileSync(wasmOutPath, JSON.stringify(report, null, 2) + '\n', 'utf8');

console.log(JSON.stringify({
  status: 'PASS_PC_WASM_BIT_EXACT',
  wasm_binary_bytes: wasmBytes.length,
  wasm_linear_memory_bytes: wasmMemBytes,
  wasm_imports: 0,
  pc_wasm_bit_exact_scenarios: report.pc_wasm_bit_exact_scenarios,
  g2_frame_sha256: g2Wasm.frame_sha256,
  ep8175_neutral_frame_sha256: wasmScenarios[0].frame_sha256,
  ep8175_p1_up_frame_sha256: wasmScenarios[1].frame_sha256,
  ep8175_p1_left_frame_sha256: wasmScenarios[2].frame_sha256,
  vis8175_neutral_hw_sha256: wasmScenarios[5].high_wram_sha256,
  vis8175_p1_up_hw_sha256: wasmScenarios[6].high_wram_sha256,
}, null, 2));
