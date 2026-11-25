# Copa América & Co-op Runtime Mods

*Nota breve en español: El mod Copa América reemplaza dinámicamente 12 selecciones en memoria sin modificar la ROM original, e incluye modelos 3D originales de autoría propia.*

## Overview

The port includes two non-destructive runtime mods implemented in C++ (`port/src/twc98_copa_mod.*` and `port/src/twc98_coop_mod.*`):

1. **Copa América Mode (12 National Teams):**
   - Patches team slots, flag palettes, text tables, kits, and match presentation dynamically in WRAM/ROM shadow buffers after BIOS boot and `EPRG` decryption.
   - Uses `CopaModBackup` with independent `captured` (ROM) and `wram_captured` (Low/High WRAM) flags so switching between *Classic '98* and *Copa América* cleanly restores original state without corrupting early BIOS boot.
   - Includes original 3D VFX models (`port/src/twc98_peru_3d_models.hpp`, authored by `@dudehtm`) rendered via `stv_vdp.cpp` and `port/hosts/web/webgl_copa_vfx.js`.
2. **Co-op Mode (`twc98_coop_mod.*`):**
   - Runtime controller hook allowing cooperative play configurations on top of the ST-V input pipeline.

## Key Implementation Files

- `port/src/twc98_copa_mod.hpp` / `port/src/twc98_copa_mod.cpp`
- `port/src/twc98_coop_mod.hpp` / `port/src/twc98_coop_mod.cpp`
- `port/src/twc98_peru_3d_models.hpp`
- `port/hosts/web/webgl_copa_vfx.js`
