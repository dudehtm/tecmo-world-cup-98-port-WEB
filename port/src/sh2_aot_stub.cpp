// Fallback stub used when port/src/sh2_aot_twcup98.cpp has not been generated
// locally from the user's own ROM via tools/gen_sh2_aot.py.
// The core automatically runs on the portable SH-2 interpreter in this mode.
#include "sh2_aot_twcup98.hpp"

namespace twc98 {

uint32_t sh2_aot_expected_line_hash(uint32_t /*line_idx*/) noexcept {
    return 0u;
}

uint32_t execute_sh2_aot_quantum(int /*cpu_id*/, Sh2CpuState& /*cpu*/, StvMachineState& /*m*/,
                                 int /*max_steps*/, uint64_t /*cycle_limit*/, BusAccessFault& /*fault*/) {
    return 0u;
}

}  // namespace twc98
