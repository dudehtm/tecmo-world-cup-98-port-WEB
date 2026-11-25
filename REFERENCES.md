# Technical References

These open-source projects, specifications, and tools were used as verified references during the research and engineering of this port:

## Core Hardware & Runtime References
- **[saturnkit](https://github.com/vs-sr-dev/saturnkit/tree/1b345ef949e1fa258ce3f693f03c0df8acdfb40d)** (MIT) — Foundation reference for VDP1/VDP2 (`stv_vdp`), SCSP (`stv_scsp`), and the 64-bit host-safe Musashi import.
- **[MAME 0.289](https://github.com/mamedev/mame/releases/tag/mame0289)** (BSD-3-Clause / GPL-2.0+):
  - `src/mame/sega/315-5881_crypt.cpp` (BSD-3-Clause) — Sega 315-5881 decryption/decompression algorithm.
  - `src/devices/sound/scsp.cpp` & `scspdsp.cpp` (BSD-3-Clause) — Yamaha YMF292 SCSP and DSP semantics.
  - `src/devices/cpu/sh/sh.cpp` & `sh7604.cpp` — Renesas SH-2 CPU and on-chip peripheral semantics.
  - `src/mame/sega/stv.cpp` — Sega ST-V memory map, IOGA/SMPC behavior, and cartridge initialization reference.
  - MAME Lua Debugger API — Used for trace capture, memory taps, and cycle-accurate verification against hardware behavior.
- **[Musashi M68000 v4.60](https://github.com/kstenerud/Musashi)** (MIT) + **SoftFloat 2b** — Motorola 68000 sound CPU core driving the SCSP.

## Related Sega Saturn / ST-V Projects Consulted
- **[saturn-recomp](https://github.com/callumjenkins/saturn-recomp)** (MIT) — Fork of `saturnkit` studied for Saturn runtime and task/continuation patterns.
- **[pc-saturnbomberman](https://github.com/callumjenkins/pc-saturnbomberman)** (MIT) — Studied for tick-level regression harness patterns.
- **[diehard-stv-decomp](https://github.com/dnb4ever/diehard-stv-decomp)** — Studied for Sega ST-V cartridge layout and patching context.

## Analysis & Build Toolchain
- **[Ghidra](https://github.com/NationalSecurityAgency/ghidra)** (`SuperH:BE:32:SH-2`) — Static binary analysis of SH-2 routines, memory maps, and VDP1/VDP2 command structures.
- **[Emscripten / LLVM](https://github.com/emscripten-core/emsdk)** — Standalone `wasm32-unknown-emscripten` compilation (`-DTWC98_NO_STL`, SIMD128, LTO, 0 host imports).
- **WebRTC (`RTCDataChannel`)** — Unreliable P2P transport for deterministic rollback netcode.
