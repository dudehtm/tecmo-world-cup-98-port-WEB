# Architecture & Optimization Notes

This document summarizes the engineering design behind the portable Sega ST-V core and the optimizations developed to run *Tecmo World Cup '98* smoothly on both native Windows PCs and browser/mobile targets.

## 1. Zero-Heap Deterministic ST-V Core (`port/src/`)

All hardware state resides inside `StvMachineState` (`stv_machine.hpp`) with fixed-size arrays and no runtime heap allocations:
- **Dual Hitachi SH-2 (`sh2_core.*`, `sh2_step_inl.hpp`, `sh2_hle_sgl.cpp`):** Cycle-budgeted master/slave execution with explicit SH-2 fixed-width arithmetic, delay-slot fidelity, and optional HLE fast paths for hot SGL math routines.
- **Optional Static AOT Hook (`sh2_aot_twcup98.hpp`):** `StvMachineState` calls two lightweight entry points (`sh2_aot_expected_line_hash` and `execute_sh2_aot_quantum`) with 64-byte High WRAM line-hash validation to guard against self-modifying code. When `sh2_aot_twcup98.cpp` is absent, `sh2_aot_stub.cpp` returns `0` and execution transparently falls back to the interpreter.
- **Sega 315-5881 (`stv_315_5881.*`):** Clean zero-heap C++17 port of MAME's BSD-3-Clause decryption/decompression device.
- **VDP1 & VDP2 (`stv_vdp.*`):** Software rasterizer plus structured command/metadata export for GPU acceleration (Direct3D 11 on PC, WebGL2 on Web).
- **SCSP + Musashi M68000 (`stv_scsp.*`, `port/third_party/musashi/`):** Global-free Yamaha YMF292 sound processor + DSP + M68000 core so audio state serializes cleanly inside snapshots.
- **Snapshots (`cpn8_io.*`):** Fast deterministic full-machine save/restore used by save-states and rollback netcode.

## 2. WebAssembly & Browser Host Optimizations (`port/hosts/web/`)

Running a dual-SH-2 + VDP1/VDP2 + 68000/SCSP arcade system at 60 Hz inside a browser—including budget mobile devices—required several targeted optimizations:

1. **Standalone Zero-Import Wasm (`wasm_port.cpp`):**
   - Compiled with `-DTWC98_NO_STL -O3 -flto -msimd128 -mbulk-memory -fno-exceptions -fno-rtti`.
   - Single fixed `128 MiB` linear memory with `0` JavaScript imports, eliminating JS↔Wasm bridge overhead.
2. **Dedicated Simulation Worker (`emu_worker.js`):**
   - Fixed `60 Hz` `MessageChannel` pacer (`scheduleAt`) with catch-up frames (`wasm_step_frame_no_render` / `wasm_net_step_frame_ex`) so UI/DOM repaints never stall emulation or audio.
   - Streams PCM buffers (`ArrayBuffer` transfer) directly to `audio_engine.js` (`AudioWorklet` / WebAudio ring buffer).
3. **Modular WebGL2 VDP1/VDP2 Pipeline (`webgl_vdp1.js`, `webgl_vdp2.js`, `webgl_vdp2_shaders.js`, `webgl_presenter.js`):**
   - **GLSL ES 3.00 strictness for mobile drivers:** Replaced dynamic uniform arrays (`uHdr[60]`) with an `RGBA32I` integer texture (`uHdrTex`) to avoid shader compiler failures on mobile Adreno/Mali GPUs.
   - **FBO format fallback:** Automatically falls back from `R16UI` render targets to `RGBA8` when strict OpenGL ES 3.0 drivers reject integer color attachments.
   - **Lite vs. Ultra shaders:** Compiles the lightweight `lite` shader by default and lazily compiles the `ultra` HD shader only when HD mode is explicitly enabled.
4. **Mobile Memory & Thermal Guard (`perf_profile.js`):**
   - Shared between main thread and worker (`importScripts`).
   - Disables multi-worker band rendering (`render_worker.js`) on mobile devices so only a single `128 MiB` Wasm instance exists (preventing mobile tab OOM/freezes).
   - `AdaptivePacer` dynamically scales rendering cadence (`60 FPS` ↔ `30 FPS` with `renderEvery=2` and `v1_draw_meta_only` skipping CPU VDP1 rasterization on skipped frames) while keeping simulation at full speed.

## 3. Native PC Host (`port/hosts/pc/`)

- **Direct3D 11 (`twc98_gpu_d3d11.hpp`):** Hardware-accelerated presentation and HD scaling.
- **Win32 + XInput/RawInput + WASAPI (`twc98_pc.cpp`):** Low-latency input polling, headless automated smoke testing (`--headless --smoke-frames`), and deterministic benchmark hooks.

## 4. Deterministic Rollback Netcode (`twc98_net_session.*`, `rtc_net.js`)

- Runs over unreliable WebRTC `RTCDataChannel` with compact input frames, configurable or auto-tuned input delay, state rollback on misprediction, and periodic FNV/SHA checksums for desync detection.
- Supports **2-tab local testing** (`BroadcastChannel`), **Manual copy/paste SDP**, and an optional generic **WebSocket signaling server**.
