# Tecmo World Cup '98 — Portable Sega ST-V Core, Web/PC Port & Research

**Author:** [@dudehtm](https://github.com/dudehtm)

A clean-room-engineered, portable **C++17** Sega ST-V core and dual-host runtime (**Windows Direct3D 11** + **WebAssembly SIMD128 / WebGL2**) built through reverse-engineering and optimization research on *Tecmo World Cup '98* (`twcup98`).

Includes a working **WebAssembly + WebGL2 browser host** (tuned for desktop and low-end mobile GPUs), deterministic **WebRTC P2P rollback netcode**, an **optional offline SH-2 AOT recompiler**, and the **Copa América 12-team runtime mod**.

> **Important (Bring Your Own ROM):** This repository contains **zero copyrighted ROMs, BIOS files, game assets, or ROM-derived AOT C++ output**. You must supply your own legitimate copy of `twcup98` and `stvbios` to run the port locally.

---

## Key Engineering Highlights

- **Zero-Heap, Zero-STL Portable Core (`port/src/`):**
  - The entire ST-V machine state (`StvMachineState` — Dual SH-2, VDP1/VDP2, SCSP + Musashi M68000, SCU/SMPC, Sega 315-5881 decryption) lives in a deterministic, flat memory layout with zero dynamic allocations.
  - Compiles cleanly to standalone WebAssembly (`0` host imports, `-DTWC98_NO_STL`, `-msimd128`, `-flto`).
  - Enables instant full-state snapshots (`F5`/`F8`) and deterministic rollback netcode.
- **Modular WebGL2 Renderer & Low-End Mobile Optimization (`port/hosts/web/`):**
  - Hardware-accelerated VDP1 (`webgl_vdp1.js`) and VDP2 (`webgl_vdp2.js`, `webgl_vdp2_shaders.js`) with strict GLSL ES 3.00 compliance (replaces dynamic uniform arrays with `RGBA32I` header textures, adds `R16UI -> RGBA8` FBO fallback for strict mobile drivers).
  - Dual shader pipeline (`lite` default for mobile GPUs, lazy-compiled `ultra` HD mode) + `AdaptivePacer` (`perf_profile.js`) with `60/30 FPS` adaptive pacing and single-worker mobile memory guard (`128 MiB` Wasm ceiling).
- **Optional Offline SH-2 AOT Recompiler (`tools/gen_sh2_aot.py`):**
  - Out of the box, the repository compiles with `sh2_aot_stub.cpp` and runs on the portable SH-2 interpreter (`sh2_core.cpp` + `sh2_hle_sgl.cpp`).
  - Users can run `tools/gen_sh2_aot.py` against their own ROM to generate `port/src/sh2_aot_twcup98.cpp` locally for maximum CPU throughput; the build scripts detect and link it automatically.
- **Deterministic WebRTC P2P Rollback (`twc98_net_session.*`, `rtc_net.js`):**
  - Unreliable `RTCDataChannel` input transport, input delay auto-tuning, state checksum verification, and 2-tab local (`BroadcastChannel`) / manual SDP / configurable WebSocket signaling.
- **Non-Destructive Runtime Mods (`twc98_copa_mod.*`, `twc98_coop_mod.*`):**
  - Live memory patching supporting a **Copa América** 12-team roster (with original author-created 3D models in `twc98_peru_3d_models.hpp`) and Co-op mode, cleanly toggleable without modifying ROM files.

---

## Repository Layout

```text
port/
  src/                 # Portable C++17 ST-V core (SH-2, VDP1/VDP2, SCSP, 315-5881, Rollback, Mods)
  third_party/musashi/ # Musashi M68000 v4.60 + SoftFloat 2b
  hosts/
    pc/                # Native Windows host (Win32 + Direct3D 11 + WASAPI)
    web/               # Browser host (Wasm Worker, WebGL2 VDP1/VDP2, WebAudio, WebRTC)
  tests/               # PC/Wasm parity, snapshot benchmarks, rollback simulation, asset prep
tools/
  gen_sh2_aot.py       # Offline SH-2 AOT generator (runs locally against user-supplied ROM)
docs/
  ARCHITECTURE.md      # Core architecture & PC/Web/Mobile optimization deep-dive
  MODS_COPA_AMERICA.md # Copa América runtime mod overview
```

---

## Building From Source

### Prerequisites
- **PC Native (Windows x64):** Visual Studio 2019/2022 Build Tools (`link.exe`) + `clang` / `clang++` (in `PATH`, via `CLANG_BIN`, or via `EMSDK`).
- **WebAssembly:** [Emscripten SDK](https://emscripten.org/) with `EMSDK` environment variable set (e.g. `set EMSDK=C:\emsdk`).
- **Local Web Host & Tools:** Python 3.10+ and Node.js 20+.

### 1. Build Native PC Host & Verification Suite
```cmd
port\build_native_g4.cmd
```
Produces `port\build-native-g4\twc98_pc.exe` plus parity/rollback benchmarks.

### 2. Build WebAssembly Core (`twc98_g4.wasm`)
```cmd
set EMSDK=C:\path\to\emsdk
port\build_wasm_g4.cmd
```
Produces `port\build-wasm-g4\twc98_g4.wasm` (standalone, `0` imports, SIMD128).

---

## Running Locally With Your Own ROM (BYOR)

Place your own legally obtained Sega ST-V files into `port/data/`:
- `port/data/twcup98_cart_20m.bin` — 20 MiB concatenated `twcup98` cartridge image
- `port/data/bios_euro_be.bin` — 512 KiB Sega ST-V BIOS (big-endian)

*(You can inspect `port/tests/prepare_g3_assets.py` for how the raw MAME zip sets `twcup98.zip` and `stvbios.zip` are assembled into the flat `.bin` images.)*

### Run on Windows (Native PC)
```cmd
port\build-native-g4\twc98_pc.exe --data-dir port\data
```
Headless smoke test:
```cmd
port\build-native-g4\twc98_pc.exe --data-dir port\data --smoke-frames 840 --headless --no-audio
```

### Run in Browser (WebAssembly + WebGL2)
Start the local COOP/COEP dev server (required for `SharedArrayBuffer` / WebAssembly threading):
```cmd
python port\hosts\web\serve_g4.py --port 18980
```
Then open `http://127.0.0.1:18980/hosts/web/index.html` in Chromium/Edge/Firefox.

### Optional: Generate Local SH-2 AOT Acceleration
To generate `port/src/sh2_aot_twcup98.cpp` from your own ROM for higher FPS on low-end devices:
```cmd
python tools\gen_sh2_aot.py --help
```
Once `port/src/sh2_aot_twcup98.cpp` is present locally (it is git-ignored), re-run `port\build_native_g4.cmd` or `port\build_wasm_g4.cmd` and it will be linked automatically in place of `sh2_aot_stub.cpp`.

---

## Documentation & Credits

- [Architecture & Optimizations](docs/ARCHITECTURE.md)
- [Copa América Runtime Mod](docs/MODS_COPA_AMERICA.md)
- [Technical References](REFERENCES.md)
- [Third-Party Licenses](THIRD_PARTY.md)
- [License (MIT)](LICENSE)
