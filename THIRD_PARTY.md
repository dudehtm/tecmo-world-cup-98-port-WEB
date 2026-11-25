# Third-Party Components & Licenses

This repository includes or adapts the following open-source components. Each retains its original license and copyright holders:

| Component | Origin | Version / Revision | License | Path |
|---|---|---|---|---|
| **Musashi M68000 Core** | Karl Stenerud (64-bit host-safe import via `saturnkit`) | `v4.60` | **MIT** (`port/third_party/musashi/README.md`, `readme.txt`) | `port/third_party/musashi/` |
| **SoftFloat 2b** | John R. Hauser (bundled with Musashi) | `Release 2b` | **SoftFloat 2b Permissive** (`port/third_party/musashi/softfloat/README.txt`) | `port/third_party/musashi/softfloat/` |
| **Sega 315-5881 Decryption/Decompression** | MAME `src/mame/sega/315-5881_crypt.cpp` & `stv.cpp` (Andreas Naive, Olivier Galibert, David Haywood) | MAME `0.289` | **BSD-3-Clause** | `port/src/stv_315_5881.hpp`, `port/src/stv_315_5881.cpp` |
| **Yamaha YMF292 (SCSP) + DSP Core** | MAME `scsp.cpp` / `scspdsp.cpp` (ElSemi, R. Belmont) & `saturnkit/runtime/scsp.cpp` | Rewritten zero-heap C++17 `struct StvScspState` | **BSD-3-Clause** (MAME) & **MIT** (`saturnkit`) | `port/src/stv_scsp.hpp`, `port/src/stv_scsp.cpp` |
| **VDP1 / VDP2 Hardware Core** | Derived from `saturnkit` runtime (`vs-sr-dev/saturnkit`, commit `1b345ef949e1fa258ce3f693f03c0df8acdfb40d`) | Extended with WebGL2/D3D11 acceleration & VFX hooks | **MIT** | `port/src/stv_vdp.hpp`, `port/src/stv_vdp.cpp` |

## Legal Disclaimer

*Tecmo World Cup '98* is a trademark of Koei Tecmo Games Co., Ltd., and *Sega ST-V / Sega Saturn* are trademarks of SEGA Corporation. This project is an independent, non-commercial research and preservation effort and is not affiliated with or endorsed by Koei Tecmo or SEGA. **No copyrighted ROMs, BIOS dumps, or game assets are included in this repository.**
