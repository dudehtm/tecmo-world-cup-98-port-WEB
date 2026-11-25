// CPN8 v3 checkpoint loader/serializer for TWC98BO G2 reconstruction.
#pragma once

#include "sh2_core.hpp"
#include "stv_peripherals.hpp"

#include <cstddef>
#include <cstdint>

#ifndef TWC98_NO_STL
#include <string>
#include <vector>
#endif

namespace twc98 {

struct Cpn8Meta {
    uint32_t mode = 0u;
    uint32_t active_cpu = 0u;
    uint32_t completed_in_slice = 0u;
    uint32_t remaining = 0u;
    uint32_t event_count = 0u;
    uint32_t global_seq = 0u;
};

uint32_t fnv1a_cpn8(const uint8_t* data, size_t len);

// Zero-heap CPN8/CPF3 v3 byte parser shared by Native PC and standalone Wasm.
// If verify_fnv is false, data may be truncated to header+WRAM+8-byte trailer (2,097,776 bytes).
bool parse_cpn8_v3_bytes(const uint8_t* data, size_t len, bool verify_fnv,
                         Cpn8Meta* out_meta, Sh2CpuState cpu[2], SaturnStvBus& bus,
                         const char** out_error);

#ifndef TWC98_NO_STL
struct Cpn8Snapshot {
    std::vector<uint8_t> raw;
    uint32_t mode = 0u;
    uint32_t active_cpu = 0u;
    uint32_t completed_in_slice = 0u;
    uint32_t remaining = 0u;
    uint32_t event_count = 0u;
    uint32_t global_seq = 0u;
};

bool load_cpn8_file(const std::string& path, Cpn8Snapshot& snap,
                    Sh2CpuState cpu[2], SaturnStvBus& bus, std::string& error);

bool save_cpn8_updated(const std::string& path, Cpn8Snapshot& snap,
                       const Sh2CpuState cpu[2], const SaturnStvBus& bus,
                       uint32_t new_mode, uint32_t new_events, std::string& error);
#endif

}  // namespace twc98
