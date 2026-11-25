import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';

const g4Dir = path.resolve(process.argv[2] || 'port');
const repoRoot = path.resolve(process.argv[3] || '.');

const wasmPath = path.join(repoRoot, 'port', 'build-wasm-g4', 'twc98_g4.wasm');
const outPath = path.join(g4Dir, 'validation_g4', 'wasm', 'h6_net_sim_wasm.json');

const wasmBytes = fs.readFileSync(wasmPath);
const wasmMod = new WebAssembly.Module(wasmBytes);
const imports = WebAssembly.Module.imports(wasmMod);
assert.equal(imports.length, 0, `Expected 0 Wasm imports, got ${imports.length}`);

const cartBytes = fs.readFileSync(path.join(g4Dir, 'data', 'twcup98_cart_20m.bin'));
const biosBytes = fs.readFileSync(path.join(g4Dir, 'data', 'bios_euro_be.bin'));
const epCp = fs.readFileSync(path.join(g4Dir, 'data', 'endpoint_frame8175.cp'));
const visDir = path.join(repoRoot, '.local', 'experiments', '012-h4-control-trace', 'visual-sync1027b-neutral');
const v1Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-vram.bin'));
const v1Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp1-registers.bin'));
const v2Vram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-vram.bin'));
const v2Cram = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-cram.bin'));
const v2Regs = fs.readFileSync(path.join(visDir, 'control-window-start-vdp2-registers.bin'));

function createPeerInstance() {
  const inst = new WebAssembly.Instance(wasmMod, {});
  const ex = inst.exports;
  const mem = new Uint8Array(ex.memory.buffer);

  // Note: init_from_cpn8_bytes calls reset() which clears cart_rom, so init_cpn8 FIRST, then init_cart
  mem.set(epCp, ex.wasm_cp_ptr() >>> 0);
  mem.set(biosBytes, ex.wasm_bios_ptr() >>> 0);
  mem.set(v1Vram, ex.wasm_vdp1_vram_ptr() >>> 0);
  mem.set(v1Regs, ex.wasm_vdp1_regs_ptr() >>> 0);
  mem.set(v2Vram, ex.wasm_vdp2_vram_ptr() >>> 0);
  mem.set(v2Cram, ex.wasm_vdp2_cram_ptr() >>> 0);
  mem.set(v2Regs, ex.wasm_vdp2_regs_ptr() >>> 0);
  assert.equal(ex.wasm_init_cpn8(epCp.length, biosBytes.length, 1), 1, 'wasm_init_cpn8 failed');

  mem.set(cartBytes, ex.wasm_cart_ptr() >>> 0);
  assert.equal(ex.wasm_init_cart(cartBytes.length), 1, 'wasm_init_cart failed');

  ex.wasm_set_gk_ai_fix(1);
  ex.wasm_set_real_audio(1);

  for (let w = 0; w < 40; w++) {
    const pcIn = (w >= 4 && w < 10) ? 0xcf : 0xff;
    ex.wasm_set_input_ports(0xff, 0xff, pcIn, 0xff, 0xff, 0x7f, 0x67);
    assert.equal(ex.wasm_step_rounds(10000), 1);
    ex.wasm_render_frame();
  }
  ex.wasm_clear_audio_pcm();
  return { ex, mem };
}

const p0 = createPeerInstance();
const p1 = createPeerInstance();

// ---- Phase 1: Wasm Fast Rollback & Copa Mod Smoke Check on Peer 0 before NetSim ----
{
  // Measure fast rollback save/load in Wasm (with primed state buffer)
  const firstLen = p0.ex.wasm_save_fast_rollback() >>> 0;
  assert.ok(firstLen > 0);
  let saveTotalMs = 0;
  let loadTotalMs = 0;
  for (let i = 0; i < 20; i++) {
    const t0 = performance.now();
    const n = p0.ex.wasm_save_fast_rollback() >>> 0;
    const t1 = performance.now();
    assert.equal(n, firstLen);
    assert.equal(p0.ex.wasm_load_fast_rollback(n), 1);
    const t2 = performance.now();
    saveTotalMs += (t1 - t0);
    loadTotalMs += (t2 - t1);
  }
  const fastSaveMs = saveTotalMs / 20;
  const fastLoadMs = loadTotalMs / 20;
  assert.ok(fastSaveMs < 2.0, `Wasm save_fast_rollback ${fastSaveMs.toFixed(3)} ms >= 2.0 ms`);
  assert.ok(fastLoadMs < 2.0, `Wasm load_fast_rollback ${fastLoadMs.toFixed(3)} ms >= 2.0 ms`);

  // Verify Copa America mode toggle and 100% Classic bit-exact restoration in Wasm
  const preHashLo = p0.ex.wasm_sync_hash64_lo() >>> 0;
  const preHashHi = p0.ex.wasm_sync_hash64_hi() >>> 0;
  assert.equal(p0.ex.wasm_set_game_mode(1, 0x1998ca01), 1, 'wasm_set_game_mode(CopaAmerica) failed');
  assert.equal(p0.ex.wasm_get_game_mode(), 1);
  assert.equal(p0.ex.wasm_set_game_mode(0, 0), 1, 'wasm_set_game_mode(Classic) failed');
  assert.equal(p0.ex.wasm_get_game_mode(), 0);
  assert.equal(p0.ex.wasm_sync_hash64_lo() >>> 0, preHashLo, 'Classic restore hash_lo mismatch');
  assert.equal(p0.ex.wasm_sync_hash64_hi() >>> 0, preHashHi, 'Classic restore hash_hi mismatch');

  p0.fastSaveMs = Number(fastSaveMs.toFixed(3));
  p0.fastLoadMs = Number(fastLoadMs.toFixed(3));
}

// ---- Phase 2: 300-Frame 2-Peer Slow-WAN NetSession Simulation in Wasm ----
const kDirs = [0x7f, 0xbf, 0xdf, 0xef, 0x5f, 0x9f, 0xaf, 0xff];
function playerButtonsForFrame(player, f) {
  f = f >>> 0;
  if (player === 0) {
    let b = kDirs[(Math.floor(f / 5) + Math.floor(f / 19)) & 7];
    if ((f % 9) < 2) b &= ~0x01;
    if ((f % 15) === 3) b &= ~0x02;
    if ((f % 21) === 6) b &= ~0x04;
    return b & 0xff;
  } else {
    let b = kDirs[(Math.floor((f + 11) / 4) + Math.floor(f / 13)) & 7];
    if ((f % 8) === 1 || (f % 8) === 2) b &= ~0x01;
    if ((f % 14) === 5) b &= ~0x02;
    if ((f % 19) === 4) b &= ~0x04;
    return b & 0xff;
  }
}

let rngState = 0x98c0ffee >>> 0;
function nextRng() {
  rngState = (Math.imul(rngState, 1664525) + 1013904223) >>> 0;
  return rngState;
}

const kSimRttMs = 150;
const kOneWayBaseMs = 75.0;
const kMaxJitterMs = 35.0;
const kMatchFrames = 300;
const kInputDelay = 3;

assert.equal(p0.ex.wasm_net_reset(0, kInputDelay, 0, 1), 1);
assert.equal(p1.ex.wasm_net_reset(1, kInputDelay, 0, 1), 1);

const flightQueue = [];
let pktSeq = 0;
let droppedPackets = 0;
let duplicatedPackets = 0;
let outOfOrderDeliveries = 0;
const lastDeliveredSeq = [0, 0];
let totalWireBytesSent = 0;

function sendOverWan(dstPeer, nowMs, wireBytes) {
  totalWireBytesSent += wireBytes.length;
  const s = ++pktSeq;

  if ((nextRng() % 30) === 7) {
    droppedPackets++;
    return;
  }

  const u = (nextRng() & 0xffff) / 65535.0;
  const jitterMs = (u * 2.0 - 1.0) * kMaxJitterMs;
  const arrival = Math.max(nowMs + 2.0, nowMs + kOneWayBaseMs + jitterMs);

  const wp = {
    dstPeer,
    arrivalMs: arrival,
    seq: s,
    wire: new Uint8Array(wireBytes),
  };
  flightQueue.push(wp);

  if ((nextRng() % 50) === 13) {
    flightQueue.push({
      dstPeer,
      arrivalMs: arrival + 6.0,
      seq: s,
      wire: new Uint8Array(wireBytes),
    });
    duplicatedPackets++;
  }
}

function deliverDuePackets(nowMs) {
  flightQueue.sort((a, b) => a.arrivalMs - b.arrivalMs);
  let i = 0;
  while (i < flightQueue.length && flightQueue[i].arrivalMs <= nowMs) {
    const wp = flightQueue[i];
    if (wp.seq < lastDeliveredSeq[wp.dstPeer]) {
      outOfOrderDeliveries++;
    } else {
      lastDeliveredSeq[wp.dstPeer] = wp.seq;
    }
    const target = wp.dstPeer === 0 ? p0 : p1;
    const pktPtr = target.ex.wasm_net_pkt_ptr() >>> 0;
    target.mem.set(wp.wire, pktPtr);
    target.ex.wasm_net_receive_wire_pkt(wp.wire.length);
    i++;
  }
  if (i > 0) {
    flightQueue.splice(0, i);
  }
}

let audioConsumed0 = 0;
let audioConsumed1 = 0;

const tSim0 = performance.now();
for (let tick = 0; tick < kMatchFrames; tick++) {
  const nowMs = tick * (1000.0 / 60.0);
  deliverDuePackets(nowMs);

  // Peer 0 queues local input & sends 48B wire packet
  {
    const tf0 = (p0.ex.wasm_net_cur_frame() >>> 0) + kInputDelay;
    p0.ex.wasm_net_queue_local_input(playerButtonsForFrame(0, tf0), 0xff);
    const ptr0 = p0.ex.wasm_net_pkt_ptr() >>> 0;
    const len0 = p0.ex.wasm_net_pkt_size() >>> 0;
    sendOverWan(1, nowMs, p0.mem.subarray(ptr0, ptr0 + len0));
  }

  // Peer 1 queues local input & sends 48B wire packet
  {
    const tf1 = (p1.ex.wasm_net_cur_frame() >>> 0) + kInputDelay;
    p1.ex.wasm_net_queue_local_input(playerButtonsForFrame(1, tf1), 0xff);
    const ptr1 = p1.ex.wasm_net_pkt_ptr() >>> 0;
    const len1 = p1.ex.wasm_net_pkt_size() >>> 0;
    sendOverWan(0, nowMs, p1.mem.subarray(ptr1, ptr1 + len1));
  }

  assert.equal(p0.ex.wasm_net_step_frame(), 1, `Wasm net0 stalled at tick ${tick}`);
  audioConsumed0 += p0.ex.wasm_consume_audio_pcm(2048) >>> 0;

  assert.equal(p1.ex.wasm_net_step_frame(), 1, `Wasm net1 stalled at tick ${tick}`);
  audioConsumed1 += p1.ex.wasm_consume_audio_pcm(2048) >>> 0;
}

deliverDuePackets(kMatchFrames * (1000.0 / 60.0) + 200.0);
p0.ex.wasm_net_flush_rollback(1);
p1.ex.wasm_net_flush_rollback(1);

// Exchange final sync packet so frame-300 sync_hash64 is cross-checked
{
  p0.ex.wasm_net_queue_local_input(0xff, 0xff);
  p1.ex.wasm_net_queue_local_input(0xff, 0xff);
  const len = p0.ex.wasm_net_pkt_size() >>> 0;
  const w0 = p0.mem.slice(p0.ex.wasm_net_pkt_ptr() >>> 0, (p0.ex.wasm_net_pkt_ptr() >>> 0) + len);
  const w1 = p1.mem.slice(p1.ex.wasm_net_pkt_ptr() >>> 0, (p1.ex.wasm_net_pkt_ptr() >>> 0) + len);
  p1.mem.set(w0, p1.ex.wasm_net_pkt_ptr() >>> 0);
  p1.ex.wasm_net_receive_wire_pkt(len);
  p0.mem.set(w1, p0.ex.wasm_net_pkt_ptr() >>> 0);
  p0.ex.wasm_net_receive_wire_pkt(len);
}
const simTotalMs = performance.now() - tSim0;

function getSyncHashHex(peer) {
  const lo = peer.ex.wasm_sync_hash64_lo() >>> 0;
  const hi = peer.ex.wasm_sync_hash64_hi() >>> 0;
  return '0x' + hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

function getSnapshotSha(peer) {
  const len = peer.ex.wasm_save_state() >>> 0;
  const ptr = peer.ex.wasm_state_ptr() >>> 0;
  return crypto.createHash('sha256').update(peer.mem.subarray(ptr, ptr + len)).digest('hex');
}

function getFrameSha(peer) {
  peer.ex.wasm_refresh_state();
  const digPtr = peer.ex.wasm_digests_ptr() >>> 0;
  const start = digPtr + 6 * 32;
  let out = '';
  for (let i = 0; i < 32; i++) out += peer.mem[start + i].toString(16).padStart(2, '0');
  return out;
}

const hash0 = getSyncHashHex(p0);
const hash1 = getSyncHashHex(p1);
const snap0 = getSnapshotSha(p0);
const snap1 = getSnapshotSha(p1);
const frame0 = getFrameSha(p0);
const frame1 = getFrameSha(p1);

// Assert Wasm Peer 0 == Wasm Peer 1
assert.equal(hash0, hash1, 'Wasm Peer0 vs Peer1 sync_hash64 mismatch');
assert.equal(snap0, snap1, 'Wasm Peer0 vs Peer1 snapshot_sha256 mismatch');
assert.equal(frame0, frame1, 'Wasm Peer0 vs Peer1 frame_px_sha256 mismatch');
assert.equal(p0.ex.wasm_net_desync_count(), 0);
assert.equal(p1.ex.wasm_net_desync_count(), 0);
assert.equal(p0.ex.wasm_net_stall_frames(), 0);
assert.equal(p1.ex.wasm_net_stall_frames(), 0);
assert.equal(p0.ex.wasm_net_sync_checks_passed(), 5);
assert.equal(p1.ex.wasm_net_sync_checks_passed(), 5);

// Assert Wasm == Native PC (verify_h6_net_sim.exe) BIT-EXACT PARITY!
assert.equal(hash0, '0xd7a7c0ce2979f66a', 'PC vs Wasm sync_hash64 mismatch');
assert.equal(snap0, '6d6bff26db08a22339d3f10c366a30d99660b512456244bb6dff7aebebc30187', 'PC vs Wasm snapshot_sha256 mismatch');
assert.equal(frame0, '203fb255d4c9fb06c07cbca27a184abda5f00b7e347d1e456ebcc22850d569c9', 'PC vs Wasm frame_px_sha256 mismatch');
assert.equal(audioConsumed0, 220500, 'PC vs Wasm audio ring frames mismatch');
assert.equal(audioConsumed1, 220500, 'PC vs Wasm audio ring frames mismatch');
assert.equal(p0.ex.wasm_audio_pcm_produced() >>> 0, 249900, 'PC vs Wasm audio_pcm_produced mismatch');

// ---- Phase 3: 60-Frame 2-Peer Deterministic Hard Mode + Goal Limit Verification ----
{
  const m0 = createPeerInstance();
  const m1 = createPeerInstance();
  for (const peer of [m0, m1]) {
    peer.ex.wasm_set_match_rules(1, 2, 3); // Hard Mode ON + First to 3 Goals
    assert.equal(peer.ex.wasm_get_hard_mode(), 1);
    assert.equal(peer.ex.wasm_get_rule_mode(), 2);
    assert.equal(peer.ex.wasm_get_rule_value(), 3);
  }
  assert.equal(getSyncHashHex(m0), getSyncHashHex(m1), 'Hard+Goal Rule Frame-0 hash mismatch');
  assert.equal(m0.ex.wasm_net_reset(0, 2, 0, 1), 1);
  assert.equal(m1.ex.wasm_net_reset(1, 2, 0, 1), 1);
  const wLen = m0.ex.wasm_net_pkt_size() >>> 0;
  for (let t = 0; t < 65; t++) {
    m0.ex.wasm_net_queue_local_input(playerButtonsForFrame(0, t), 0xff);
    const w0 = m0.mem.slice(m0.ex.wasm_net_pkt_ptr() >>> 0, (m0.ex.wasm_net_pkt_ptr() >>> 0) + wLen);
    m1.ex.wasm_net_queue_local_input(playerButtonsForFrame(1, t), 0xff);
    const w1 = m1.mem.slice(m1.ex.wasm_net_pkt_ptr() >>> 0, (m1.ex.wasm_net_pkt_ptr() >>> 0) + wLen);
    m1.mem.set(w0, m1.ex.wasm_net_pkt_ptr() >>> 0);
    m1.ex.wasm_net_receive_wire_pkt(wLen);
    m0.mem.set(w1, m0.ex.wasm_net_pkt_ptr() >>> 0);
    m0.ex.wasm_net_receive_wire_pkt(wLen);
    assert.equal(m0.ex.wasm_net_step_frame(), 1);
    assert.equal(m1.ex.wasm_net_step_frame(), 1);
  }
  m0.ex.wasm_net_flush_rollback(1);
  m1.ex.wasm_net_flush_rollback(1);
  assert.equal(getSyncHashHex(m0), getSyncHashHex(m1), 'Hard+Goal Rule 65f desync');
  assert.equal(m0.ex.wasm_net_desync_count(), 0);
  assert.equal(m1.ex.wasm_net_desync_count(), 0);
}

const report = {
  schema: 'h6_net_sim_wasm_v1',
  status: 'PASS_PC_WASM_NET_ROLLBACK_BIT_EXACT',
  wasm_imports: 0,
  wasm_fast_save_ms: p0.fastSaveMs,
  wasm_fast_load_ms: p0.fastLoadMs,
  copa_mod_wasm_roundtrip_ok: true,
  wan_rtt_ms: kSimRttMs,
  wan_jitter_ms: kMaxJitterMs,
  dropped_packets: droppedPackets,
  duplicated_packets: duplicatedPackets,
  out_of_order_packets: outOfOrderDeliveries,
  bandwidth_kbps_per_peer: Number(((totalWireBytesSent * 8 / 2) / ((kMatchFrames / 60) * 1000)).toFixed(2)),
  peer0_rollbacks: p0.ex.wasm_net_rollback_count() >>> 0,
  peer0_rb_frames: p0.ex.wasm_net_rollback_frames() >>> 0,
  peer1_rollbacks: p1.ex.wasm_net_rollback_count() >>> 0,
  peer1_rb_frames: p1.ex.wasm_net_rollback_frames() >>> 0,
  sync_hash64: hash0,
  snapshot_sha256: snap0,
  frame_px_sha256: frame0,
  audio_ring_frames: audioConsumed0,
  audio_pcm_produced: p0.ex.wasm_audio_pcm_produced() >>> 0,
  sim_total_ms: Number(simTotalMs.toFixed(1)),
};

fs.mkdirSync(path.dirname(outPath), { recursive: true });
fs.writeFileSync(outPath, JSON.stringify(report, null, 2) + '\n', 'utf8');
console.log(JSON.stringify(report, null, 2));
