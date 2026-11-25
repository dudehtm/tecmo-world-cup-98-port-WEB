# Architecture & Optimization Notes

This document summarizes the engineering design behind the portable Sega ST-V core and the optimizations developed to run *Tecmo World Cup '98* smoothly on both native Windows PCs and browser/mobile targets.

## 1. Zero-Heap Deterministic ST-V Core (`port/src/`)

All hardware state resides inside `StvMachineState` (`stv_machine.hpp`) with fixed-size arrays and no runtime heap allocations:
- **Dual Hitachi SH-2 (`sh2_core.*`, `sh2_step_inl.hpp`):** Cycle-budgeted master/slave execution with explicit SH-2 fixed-width arithmetic and delay-slot fidelity.
- **Native SGL 3D HLE (`sh2_hle_sgl.*`):** Dedicated C++17 fast path for the two hottest Sega SGL matrix×vertex + `DIVU` projection loops (`Slave 0x060631DE..0x06063274` and `Master 0x0605CD86..0x0605CE1C`), guarded by 64-byte line-hash checks and non-aliasing validation. *(Note: a `PolyCull` `0x06063332` HLE was benchmarked and discarded because register spills made it slower than the compiled SH-2 path.)*
- **Optional Static AOT Hook (`sh2_aot_twcup98.hpp`):** `StvMachineState` calls two lightweight entry points (`sh2_aot_expected_line_hash` and `execute_sh2_aot_quantum`) with `FNV-1a32` 64-byte High WRAM line-hash validation to guard against self-modifying code and runtime mods (`mark_high_wram_dirty` / `invalidate_all_aot_pages`). When `sh2_aot_twcup98.cpp` is absent, `sh2_aot_stub.cpp` returns `0` and execution transparently falls back to the interpreter.
- **`fast_cpu` Quantum Batching (`stv_machine.cpp`):** Runtime flag (`wasm_set_fast_cpu`, `?fast_cpu=0/1`) that batches `idle_skip` audio stepping and raises the Master↔Slave quantum to 16 rounds (512 steps) **only after `sched2_hits > 0`** (post-BIOS/EPRG init). Delivers `3.3×–3.8×` faster 3D match simulation while keeping Cold Boot Frame-0 snapshots and rollback bit-exact.
- **Sega 315-5881 (`stv_315_5881.*`):** Clean zero-heap C++17 port of MAME's BSD-3-Clause decryption/decompression device.
- **VDP1 & VDP2 (`stv_vdp.*`):** Software rasterizer plus structured command/metadata export (`v1_draw_meta_only`) for GPU acceleration (Direct3D 11 on PC, WebGL2 on Web).
- **SCSP + Musashi M68000 (`stv_scsp.*`, `port/third_party/musashi/`):** Global-free Yamaha YMF292 sound processor + DSP + M68000 core so audio state serializes cleanly inside snapshots.
- **Snapshots (`cpn8_io.*`):** Fast deterministic full-machine save/restore used by save-states and rollback netcode.

## 2. WebAssembly & Browser Host Optimizations (`port/hosts/web/`)

Running a dual-SH-2 + VDP1/VDP2 + 68000/SCSP arcade system at 60 Hz inside a browser—including budget mobile devices—required several targeted optimizations:

1. **Standalone Zero-Import Wasm (`wasm_port.cpp`):**
   - Compiled with `-DTWC98_NO_STL -O3 -flto -msimd128 -mbulk-memory -fno-exceptions -fno-rtti`.
   - Single fixed `128 MiB` linear memory with `0` JavaScript imports, eliminating JS↔Wasm bridge overhead.
2. **Dedicated Simulation Worker (`emu_worker.js`):**
   - Fixed `60 Hz` `MessageChannel` pacer (`scheduleAt`) with catch-up frames (`wasm_step_frame_no_render` / `wasm_net_step_frame_ex`) so UI/DOM repaints never stall emulation or audio.
   - Streams PCM buffers (`ArrayBuffer` transfer) directly to `audio_engine.js` (`AudioWorklet` / WebAudio ring buffer, with ~`100–110 ms` mobile buffer headroom and native `sampleRate` + `latencyHint: 'balanced'` on desktop).
3. **Modular WebGL2 VDP1/VDP2 & VFX Pipeline (`webgl_vdp1.js`, `webgl_vdp2.js`, `webgl_vdp2_shaders.js`, `webgl_copa_vfx.js`, `webgl_presenter.js`):**
   - **VDP1 GPU dirty-chunk streaming:** Packs VDP1 command lists + dirty VRAM chunks (~`78 KiB/frame`, `-88.5%` traffic) and uploads with zero-allocation `Int32Array` `srcOffset` calls (`bufferSubData`/`texSubImage2D`).
   - **GLSL ES 3.00 strictness for mobile drivers:** Uses `std140` UBO with `RGBA32I` integer texture fallback (`uHdrTex`) instead of dynamic uniform arrays (`uHdr[60]`), avoiding shader compiler failures on mobile Adreno/Mali GPUs.
   - **FBO format fallback:** Automatically falls back from `R16UI` render targets to `RGBA8` when strict OpenGL ES 3.0 drivers reject integer color attachments.
   - **Lite vs. Ultra shaders:** Compiles the lightweight `lite` shader by default (`448p` full resolution on WebGL2 mobile, `54–58 FPS` measured on budget Android) and lazily compiles the `ultra` 2× HD De-Mesh shader only when HD mode is enabled.
   - **GPU VFX Post-Pass (`webgl_copa_vfx.js`):** Renders special-shot wind VFX (`hdr[216..230]`) directly on the VDP2 FBO with lazy shader compilation and bounded quads instead of falling back to CPU rendering.
4. **Mobile Memory, Thermal & Touch Guard (`perf_profile.js`, `app.js`, `index.html`):**
   - Shared between main thread and worker (`importScripts`), with unified mobile/iPadOS detection.
   - Disables multi-worker band rendering (`render_worker.js`) on mobile devices so only a single `128 MiB` Wasm instance exists (preventing mobile tab OOM/freezes).
   - `AdaptivePacer` dynamically scales rendering cadence (`60 FPS` ↔ `30 FPS` with `renderEvery=2`) in CPU fallback mode.
   - Removes expensive `backdrop-filter` blur layers over the WebGL canvas on mobile (restorable via `?touch_blur=1`) and tunes the floating touch joystick sprint threshold (`1.12×` engage / `0.92×` release hysteresis) to prevent accidental sprints.

## 3. Native PC Host (`port/hosts/pc/`)

- **Direct3D 11 (`twc98_gpu_d3d11.hpp`):** Hardware-accelerated presentation and HD scaling.
- **Win32 + XInput/RawInput + WASAPI (`twc98_pc.cpp`):** Low-latency input polling, headless automated smoke testing (`--headless --smoke-frames`), and deterministic benchmark hooks.

## 4. Deterministic Rollback Netcode (`twc98_net_session.*`, `rtc_net.js`)

- Runs over unreliable WebRTC `RTCDataChannel` with compact input frames, configurable or auto-tuned input delay, state rollback on misprediction, and periodic FNV/SHA checksums for desync detection.
- Supports **2-tab local testing** (`BroadcastChannel`), **Manual copy/paste SDP**, and an optional generic **WebSocket signaling server**.
