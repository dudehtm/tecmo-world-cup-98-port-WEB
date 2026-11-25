#include "stv_machine.hpp"

#include "sh2_aot_twcup98.hpp"
#include "sha256.hpp"
#include "sh2_step_inl.hpp"

extern "C" {
#include "m68k.h"
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace twc98 {
#ifdef TWC98_AOT_PROFILER
void twc98_aot_prof_step(int cpu_id, uint32_t pc, uint32_t prev_pc);
#endif
namespace {

StvMachineState* g_active_m68k_machine = nullptr;
bool g_m68k_core_inited = false;

void ensure_m68k_core_init() {
    if (!g_m68k_core_inited) {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        g_m68k_core_inited = true;
    }
}

void rebind_m68k_host_pointers() {
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(nullptr);
    m68k_set_bkpt_ack_callback(nullptr);
    m68k_set_reset_instr_callback(nullptr);
    m68k_set_tas_instr_callback(nullptr);
    m68k_set_illg_instr_callback(nullptr);
    m68k_set_trap_instr_callback(nullptr);
    m68k_set_pc_changed_callback(nullptr);
    m68k_set_fc_callback(nullptr);
    m68k_set_instr_hook_callback(nullptr);
}

void switch_active_m68k(StvMachineState* target) {
    if (g_active_m68k_machine == target) return;
    ensure_m68k_core_init();
    if (g_active_m68k_machine != nullptr && g_active_m68k_machine->m68k_enabled) {
        const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
        if (sz <= StvMachineState::kMaxM68kCtxBytes) {
            g_active_m68k_machine->m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(g_active_m68k_machine->m68k_ctx_blob));
        }
    }
    g_active_m68k_machine = target;
    if (target != nullptr && target->m68k_ctx_size > 0u) {
        m68k_set_context(target->m68k_ctx_blob);
    }
}

}  // namespace
}  // namespace twc98

extern "C" {

unsigned int m68k_read_memory_8(unsigned int address) {
    auto* m = twc98::g_active_m68k_machine;
    if (!m) return 0u;
    const uint32_t a = address & 0x00FFFFFFu;
    if (a < 0x100000u) {
        return m->sound_ram[a & twc98::StvScspState::kSoundRamMask];
    }
    if (a < 0x200000u) {
        return m->scsp.read(a & 0xFFFu, 1);
    }
    return 0u;
}

unsigned int m68k_read_memory_16(unsigned int address) {
    auto* m = twc98::g_active_m68k_machine;
    if (!m) return 0u;
    const uint32_t a = address & 0x00FFFFFFu;
    if (a < 0x100000u) {
        const uint32_t a0 = a & twc98::StvScspState::kSoundRamMask;
        const uint32_t a1 = (a + 1u) & twc98::StvScspState::kSoundRamMask;
        return (static_cast<unsigned int>(m->sound_ram[a0]) << 8) |
               static_cast<unsigned int>(m->sound_ram[a1]);
    }
    if (a < 0x200000u) {
        return m->scsp.read(a & 0xFFFu, 2);
    }
    return 0u;
}

unsigned int m68k_read_memory_32(unsigned int address) {
    return (m68k_read_memory_16(address) << 16) | m68k_read_memory_16(address + 2u);
}

unsigned int m68k_read_disassembler_8(unsigned int address) { return m68k_read_memory_8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return m68k_read_memory_16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return m68k_read_memory_32(address); }

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    auto* m = twc98::g_active_m68k_machine;
    if (!m) return;
    const uint32_t a = address & 0x00FFFFFFu;
    if (a < 0x100000u) {
        m->sound_ram[a & twc98::StvScspState::kSoundRamMask] = static_cast<uint8_t>(value & 0xFFu);
        return;
    }
    if (a < 0x200000u) {
        m->scsp.write(a & 0xFFFu, value & 0xFFu, 1, m->sound_ram);
        m68k_set_irq(static_cast<unsigned int>(m->scsp.m68k_irq_level()));
    }
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
    auto* m = twc98::g_active_m68k_machine;
    if (!m) return;
    const uint32_t a = address & 0x00FFFFFFu;
    if (a < 0x100000u) {
        const uint32_t a0 = a & twc98::StvScspState::kSoundRamMask;
        const uint32_t a1 = (a + 1u) & twc98::StvScspState::kSoundRamMask;
        m->sound_ram[a0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m->sound_ram[a1] = static_cast<uint8_t>(value & 0xFFu);
        return;
    }
    if (a < 0x200000u) {
        m->scsp.write(a & 0xFFFu, value & 0xFFFFu, 2, m->sound_ram);
        m68k_set_irq(static_cast<unsigned int>(m->scsp.m68k_irq_level()));
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
    m68k_write_memory_16(address, (value >> 16) & 0xFFFFu);
    m68k_write_memory_16(address + 2u, value & 0xFFFFu);
}

}  // extern "C"

namespace twc98 {
namespace {

void wr_be(uint8_t* dst, uint32_t off, uint8_t width, uint32_t val) {
    for (uint8_t i = 0u; i < width; ++i) {
        dst[off + i] = static_cast<uint8_t>((val >> ((width - 1u - i) * 8u)) & 0xFFu);
    }
}

uint32_t rd_be(const uint8_t* src, uint32_t off, uint8_t width) {
    uint32_t out = 0u;
    for (uint8_t i = 0u; i < width; ++i) {
        out = (out << 8) | src[off + i];
    }
    return out;
}

uint8_t to_bcd(uint32_t v) {
    return static_cast<uint8_t>(((v / 10u) << 4) | (v % 10u));
}

}  // namespace

void StvMachineState::reset() {
    cpus[0] = Sh2CpuState{};
    cpus[1] = Sh2CpuState{};
    inner.reset();
    vdp.init();
    std::memset(bios_rom, 0, sizeof(bios_rom));
    has_bios = false;
    std::memset(backupram, 0, sizeof(backupram));
    std::memset(sound_ram, 0, sizeof(sound_ram));
    std::memset(scsp_regs, 0, sizeof(scsp_regs));
    for (int i = 0; i < 3; ++i) {
        scu_dma[i] = ScuDmaChannel{};
        scu_dma_irq_armed[i] = false;
        scu_dma_irq_cycle[i] = 0u;
    }
    scu_ims = 0xFFFF717Cu;
    scu_ist = 0u;
    scu_rsel = 0u;
    dmac1 = ShDmaCh1{};
    frt_tocr[0] = 0u;
    frt_tocr[1] = 0u;
    frt_icr[0] = 0u;
    frt_icr[1] = 0u;
    std::memset(smpc_ireg, 0, sizeof(smpc_ireg));
    std::memset(smpc_oreg, 0, sizeof(smpc_oreg));
    smpc_sr = 0x00u;
    smpc_sf = 0x00u;
    smpc_pdr1_out = 0x00u;
    smpc_pdr2_out = 0x00u;
    smpc_ddr1 = 0x00u;
    smpc_ddr2 = 0x00u;
    smpc_gamebank_select = 0u;
    smpc_peri_pending = false;
    smpc_irq_cycle = 0u;
    smpc_irq_armed = false;
    rtc_base_cycles = 0u;
    inputs = StvInputState{};
    ioga_system_output = 0x00u;
    ioga_mode = 0x00u;
    ioga_portg = 0xFFu;
    ioga_billboard = 0x00u;
    ioga_direction = 0x00u;
    cart_prot_enabled = false;
    crypt315.reset();
    vdp2_tvstat = 0u;
    vdp2_hcnt = 0u;
    vdp2_vcnt = 0u;
    next_vbi_cyc = 0u;
    next_vbo_cyc = 0u;
    pending_vbi = false;
    pending_vbo = false;
    rounds_done = 0u;
    main_steps = 0u;
    slave_steps = 0u;
    vbi_count = 0u;
    vbo_count = 0u;
    sched2_hits = 0u;
    minit_count = 0u;
    sinit_count = 0u;
    vdp1_vram_writes = 0u;
    scu_dma_runs = 0u;
    sh_dmac1_runs = 0u;
    ioga_reads = 0u;
    ioga_writes = 0u;
    smpc_input_reads = 0u;
    cart_reads = 0u;
    soundram_reads = 0u;
    soundram_writes = 0u;
    scsp_ack_armed = false;
    scsp_ack_cycle = 0u;
    last_write_val = 0u;
    last_write_ea = 0u;
    halted = false;
    halt_cpu = -1;
    last_fault = BusAccessFault{};
    slave_enabled = true;
    smpc_reset_master_pending = false;
    pending_nmi = false;
    mmio_log_count = 0u;
    ioga_log_count = 0u;
    scsp_event_seq = 0u;
    std::memset(scsp_events, 0, sizeof(scsp_events));
    if (g_active_m68k_machine == this) {
        g_active_m68k_machine = nullptr;
    }
    scsp.reset();
    real_audio_enabled = false;
    m68k_enabled = false;
    m68k_in_reset = true;
    pending_sound_irq = false;
    m68k_debt = 0;
    audio_cyc_acc = 0u;
    audio_last_sh2_cyc = 0u;
    m68k_pc_cached = 0u;
    audio_pcm_produced = 0u;
    audio_pcm_nonzero = 0u;
    std::memset(m68k_ctx_blob, 0, sizeof(m68k_ctx_blob));
    m68k_ctx_size = 0u;
    coop::reset_runtime_trackers(coop);
    master_events_dirty = true;
    invalidate_all_aot_pages();
    clear_audio_pcm();
}

void StvMachineState::clear_cart_rom() {
    std::memset(cart_rom, 0, sizeof(cart_rom));
    has_cart = false;
    cart_prot_enabled = false;
    crypt315.reset();
}

bool StvMachineState::load_cart_rom(const uint8_t* data, size_t len) {
    if (data == nullptr || len != kCartPopulatedSize) {
        return false;
    }
    std::memcpy(cart_rom, data, kCartPopulatedSize);
    has_cart = true;
    cart_prot_enabled = false;
    crypt315.reset();
    return true;
}

void StvMachineState::mark_cart_loaded(uint32_t len) {
    has_cart = (len == kCartPopulatedSize);
    cart_prot_enabled = false;
    crypt315.reset();
}

void StvMachineState::set_input_ports(uint8_t port_a, uint8_t port_b, uint8_t port_c,
                                      uint8_t port_e, uint8_t port_f,
                                      uint8_t pdr1_in, uint8_t pdr2_in) {
    inputs.port_a = port_a;
    inputs.port_b = port_b;
    // In 2P Same-Team Co-op, block Coin 2 (bit 1) and 2P Start (bit 5) to prevent 2P Versus Join-In.
    inputs.port_c = coop.enabled ? static_cast<uint8_t>(port_c | 0x22u) : port_c;
    inputs.port_e = port_e;
    inputs.port_f = port_f;
    inputs.pdr1_in = pdr1_in;
    inputs.pdr2_in = pdr2_in;
}

void StvMachineState::reset_inputs() {
    inputs = StvInputState{};
}

void StvMachineState::log_ioga(uint8_t cpu_id, uint32_t pc, uint32_t addr, uint8_t val,
                               uint8_t is_write, uint8_t width) {
    if (ioga_log_count >= kMaxIogaLog) return;
    IogaAccessEntry& e = ioga_log[ioga_log_count++];
    e.pc = pc;
    e.address = addr;
    e.val = val;
    e.cpu_id = cpu_id;
    e.is_write = is_write;
    e.width = width;
}

uint8_t StvMachineState::read_ioga_byte(uint32_t off) {
    if ((off & 1u) == 0u) return 0x00u;
    switch (off) {
        case 0x01u: return inputs.port_a;
        case 0x03u: return inputs.port_b;
        case 0x05u: return inputs.port_c;
        case 0x07u: return ioga_system_output;
        case 0x09u: return inputs.port_e;
        case 0x0Bu: return inputs.port_f;
        case 0x0Du: {
            if ((ioga_mode & 0x80u) != 0u) {
                const uint8_t cur = ioga_portg;
                ioga_portg = static_cast<uint8_t>((ioga_portg + 1u) & 0x0Fu);
                return cur;
            }
            return ioga_portg;
        }
        case 0x1Bu: return 0x00u;
        case 0x1Du: return ioga_mode;
        default: return 0xFFu;
    }
}

void StvMachineState::write_ioga_byte(uint32_t off, uint8_t bval) {
    if ((off & 1u) == 0u) return;
    switch (off) {
        case 0x07u: ioga_system_output = bval; return;
        case 0x09u: ioga_billboard = bval; return;
        case 0x0Du: ioga_portg = bval; return;
        case 0x1Du: ioga_mode = bval; return;
        case 0x1Fu: ioga_direction = bval; return;
        default: return;
    }
}

bool StvMachineState::init_cold_boot_bytes(const uint8_t* bios_data, size_t bios_len,
                                           const uint8_t* cart_data, size_t cart_len,
                                           const char** out_error) {
    reset();
    if (bios_data == nullptr || bios_len != kBiosSize) {
        if (out_error) *out_error = "invalid bios size for cold boot";
        return false;
    }
    if (!load_cart_rom(cart_data, cart_len)) {
        if (out_error) *out_error = "invalid cart size for cold boot";
        return false;
    }
    std::memcpy(bios_rom, bios_data, kBiosSize);
    has_bios = true;
    auto rd32_be = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    };
    cpus[0].pc = rd32_be(bios_rom);
    cpus[0].r[15] = rd32_be(bios_rom + 4u);
    cpus[0].vbr = 0u;
    cpus[0].sr = 0xF0u;
    cpus[1].pc = rd32_be(bios_rom + 8u);
    cpus[1].r[15] = rd32_be(bios_rom + 12u);
    cpus[1].vbr = 0u;
    cpus[1].sr = 0xF0u;
    slave_enabled = false;
    next_vbi_cyc = kFrameCycles - 30000ULL;
    next_vbo_cyc = kFrameCycles;
    return true;
}

bool StvMachineState::init_from_cpn8_bytes(const uint8_t* cp_data, size_t cp_len,
                                           const uint8_t* bios_data, size_t bios_len,
                                           const char** out_error) {
    reset();
    Cpn8Meta meta{};
    if (!parse_cpn8_v3_bytes(cp_data, cp_len, true, &meta, cpus, inner, out_error)) {
        return false;
    }
    inner.strict_vram_init = true;
    if (bios_data != nullptr && bios_len == kBiosSize) {
        std::memcpy(bios_rom, bios_data, kBiosSize);
        has_bios = true;
    }
    rtc_base_cycles = cpus[0].cycles;
    audio_last_sh2_cyc = cpus[0].cycles;
    next_vbi_cyc = cpus[0].cycles + 16000ULL;
    next_vbo_cyc = cpus[0].cycles + 83000ULL;
    master_events_dirty = true;
    apply_gk_ai_fix();
    coop::apply_coop_patches(*this);
    return true;
}

void StvMachineState::seed_vdp_snapshot(const uint8_t* v1_vram, size_t v1_vram_len,
                                        const uint8_t* v1_regs, size_t v1_regs_len,
                                        const uint8_t* v2_vram, size_t v2_vram_len,
                                        const uint8_t* v2_cram, size_t v2_cram_len,
                                        const uint8_t* v2_regs, size_t v2_regs_len) {
    if (v1_vram != nullptr && v1_vram_len > 0u) {
        std::memcpy(vdp.vdp1_vram, v1_vram, std::min(v1_vram_len, static_cast<size_t>(StvVdpState::kVdp1VramSize)));
    }
    if (v1_regs != nullptr && v1_regs_len >= 0x20u) {
        for (size_t i = 0u; i < 16u; ++i) {
            vdp.vdp1_regs[i] = static_cast<uint16_t>((v1_regs[i * 2u] << 8) | v1_regs[i * 2u + 1u]);
        }
        vdp.vdp1_regs[StvVdpState::EDSR] |= 0x0002u;
    }
    if (v2_vram != nullptr && v2_vram_len > 0u) {
        const size_t n = std::min(v2_vram_len, inner.vdp2_vram.size());
        std::memcpy(inner.vdp2_vram.data(), v2_vram, n);
        std::memset(inner.vdp2_written.data(), 1, n);
    }
    if (v2_cram != nullptr && v2_cram_len > 0u) {
        std::memcpy(vdp.vdp2_cram, v2_cram, std::min(v2_cram_len, static_cast<size_t>(StvVdpState::kVdp2CramSize)));
    }
    if (v2_regs != nullptr && v2_regs_len > 0u) {
        std::memcpy(vdp.vdp2_regs, v2_regs, std::min(v2_regs_len, static_cast<size_t>(StvVdpState::kVdp2RegsSize)));
    }
    vdp.invalidate_gpu_vdp1_frames();
}

void StvMachineState::log_mmio(MmioEventKind kind, uint8_t cpu_id, uint32_t pc,
                               uint32_t arg0, uint32_t arg1, uint8_t width, uint64_t cyc) {
    if (mmio_log_count >= kMaxMmioLog) return;
    MmioLogEntry& e = mmio_log[mmio_log_count++];
    e.cycles = cyc;
    e.round = rounds_done;
    e.pc = pc;
    e.arg0 = arg0;
    e.arg1 = arg1;
    e.kind = static_cast<uint8_t>(kind);
    e.cpu_id = cpu_id;
    e.width = width;
    e.reserved = 0u;
}

void StvMachineState::run_sh_dmac1(int cpu_id, uint32_t pc, uint64_t cyc, BusAccessFault& fault) {
    const uint32_t dmaor = inner.dmac[static_cast<size_t>(cpu_id)].dmaor;
    if ((dmac1.chcr & dmaor & 1u) == 0u || (dmac1.chcr & 2u) != 0u) {
        return;
    }
    const uint32_t incd = (dmac1.chcr >> 14) & 3u;
    const uint32_t incs = (dmac1.chcr >> 12) & 3u;
    const uint32_t sz = (dmac1.chcr >> 10) & 3u;
    uint32_t src = dmac1.sar;
    uint32_t dst = dmac1.dar;
    const uint32_t orig_sar = src;
    const uint32_t orig_dar = dst;
    uint32_t count = dmac1.tcr ? dmac1.tcr : 0x1000000u;
    const uint8_t unit_w = (sz == 0u) ? 1u : (sz == 1u) ? 2u : 4u;
    const uint32_t step = unit_w;
    if (sz == 3u) count &= ~3u;
    const uint32_t reps = (sz == 3u) ? (count / 4u) : count;

    for (uint32_t i = 0u; i < reps; ++i) {
        if (incs == 2u) src -= step;
        if (incd == 2u) dst -= step;
        const uint32_t v = read(cpu_id, src, unit_w, pc, cyc, fault);
        if (!fault.ok) return;
        write(cpu_id, dst, unit_w, v, pc, cyc, fault);
        if (!fault.ok) return;
        if (incs == 1u) src += step;
        if (incd == 1u) dst += step;
    }
    dmac1.sar = src;
    dmac1.dar = dst;
    dmac1.tcr = 0u;
    dmac1.chcr |= 2u;  // TE=1
    ++sh_dmac1_runs;
    log_mmio(MMIO_SH_DMAC1, static_cast<uint8_t>(cpu_id), pc, orig_sar, orig_dar, unit_w, cyc);
}

void StvMachineState::scu_copy(int cpu_id, uint32_t& src, uint32_t& dst, uint32_t bytes,
                               uint32_t radd, uint32_t wadd, uint32_t pc, uint64_t cyc,
                               BusAccessFault& fault) {
    const uint32_t cea_dst = canonicalize_stv_addr(dst);
    const bool is_bbus = (cea_dst >= 0x05A00000u && cea_dst < 0x05FE0000u);
    auto scu_read32 = [&](uint32_t addr) -> uint32_t {
        if ((addr & 3u) == 0u) return read(cpu_id, addr, 4u, pc, cyc, fault);
        if ((addr & 1u) == 0u) {
            const uint32_t hi = read(cpu_id, addr, 2u, pc, cyc, fault);
            if (!fault.ok) return 0u;
            const uint32_t lo = read(cpu_id, addr + 2u, 2u, pc, cyc, fault);
            return (hi << 16) | (lo & 0xFFFFu);
        }
        uint32_t out = 0u;
        for (uint32_t k = 0u; k < 4u; ++k) {
            const uint32_t b = read(cpu_id, addr + k, 1u, pc, cyc, fault);
            if (!fault.ok) return 0u;
            out = (out << 8) | (b & 0xFFu);
        }
        return out;
    };
    for (uint32_t i = 0u; i < bytes; i += 4u) {
        const uint32_t rem = (bytes - i < 4u) ? (bytes - i) : 4u;
        if (rem == 4u) {
            const uint32_t v = scu_read32(src);
            if (!fault.ok) return;
            if (is_bbus) {
                write(cpu_id, dst, 2u, (v >> 16) & 0xFFFFu, pc, cyc, fault);
                if (!fault.ok) return;
                dst += wadd;
                write(cpu_id, dst, 2u, v & 0xFFFFu, pc, cyc, fault);
                if (!fault.ok) return;
                dst += wadd;
            } else {
                write(cpu_id, dst, 4u, v, pc, cyc, fault);
                if (!fault.ok) return;
                dst += wadd;
            }
        } else if (rem == 2u) {
            const uint32_t v = read(cpu_id, src, 2u, pc, cyc, fault);
            if (!fault.ok) return;
            write(cpu_id, dst, 2u, v & 0xFFFFu, pc, cyc, fault);
            if (!fault.ok) return;
            dst += wadd;
        } else {
            for (uint32_t k = 0u; k < rem; ++k) {
                const uint32_t b = read(cpu_id, src + k, 1u, pc, cyc, fault);
                if (!fault.ok) return;
                write(cpu_id, dst + k, 1u, b, pc, cyc, fault);
                if (!fault.ok) return;
            }
            dst += wadd;
        }
        src += radd;
    }
}

void StvMachineState::run_scu_dma(int cpu_id, int lvl, uint32_t pc, uint64_t cyc, BusAccessFault& fault) {
    ScuDmaChannel& d = scu_dma[lvl];
    const uint32_t radd = ((d.ad >> 8) & 1u) ? 4u : 0u;
    const uint32_t wadd = (d.ad & 7u) ? (1u << (d.ad & 7u)) : 0u;
    if ((d.md >> 24) & 1u) {
        uint32_t t = d.w;
        uint32_t n = 0u;
        uint32_t first_dst = 0u;
        for (;;) {
            const uint32_t cnt = read(cpu_id, t, 4u, pc, cyc, fault);
            if (!fault.ok) return;
            uint32_t dst = read(cpu_id, t + 4u, 4u, pc, cyc, fault);
            if (!fault.ok) return;
            if (n == 0u) first_dst = dst;
            const uint32_t raw_src = read(cpu_id, t + 8u, 4u, pc, cyc, fault);
            if (!fault.ok) return;
            const bool end = (raw_src & 0x80000000u) != 0u;
            uint32_t src = raw_src & 0x07FFFFFFu;
            const uint32_t bytes = cnt ? cnt : (lvl ? 0x1000u : 0x100000u);
            scu_copy(cpu_id, src, dst, bytes, radd, wadd, pc, cyc, fault);
            if (!fault.ok) return;
            t += 12u;
            ++n;
            if (end || n > 4096u) break;
        }
        if ((d.md >> 8) & 1u) d.w = t;
        ++scu_dma_runs;
        log_mmio(MMIO_SCU_DMA, static_cast<uint8_t>(cpu_id), pc,
                 static_cast<uint32_t>(lvl) | 0x100u | (n << 16), first_dst, 4u, cyc);
    } else {
        uint32_t cnt = d.c & (lvl ? 0xFFFu : 0xFFFFFu);
        if (!cnt) cnt = lvl ? 0x1000u : 0x100000u;
        uint32_t src = d.r;
        uint32_t dst = d.w;
        const uint32_t orig_dst = dst;
        scu_copy(cpu_id, src, dst, cnt, radd, wadd, pc, cyc, fault);
        if (!fault.ok) return;
        if ((d.md >> 16) & 1u) d.r = src;
        if ((d.md >> 8) & 1u) d.w = dst;
        ++scu_dma_runs;
        log_mmio(MMIO_SCU_DMA, static_cast<uint8_t>(cpu_id), pc,
                 static_cast<uint32_t>(lvl) | (cnt << 16), orig_dst, 4u, cyc);
    }
    scu_ist |= (1u << (lvl == 0 ? 11 : lvl == 1 ? 10 : 9));
    scu_dma_irq_armed[lvl] = true;
    scu_dma_irq_cycle[lvl] = cyc + 600u;
}

uint32_t StvMachineState::read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                               uint64_t cpu_cycles, BusAccessFault& fault) {
    if (is_sh2_cache_area(ea)) {
        return 0u;
    }
    const uint32_t cea = canonicalize_stv_addr(ea);
    if (has_bios && cea + width <= kBiosSize) {
        return rd_be(bios_rom, cea, width);
    }
    if (cea >= 0x00100000u && cea <= 0x0010007Fu && width == 1u) {
        const uint32_t off = cea - 0x00100000u;
        if (off >= 0x01u && off <= 0x0Du && (off & 1u)) return smpc_ireg[(off - 1u) >> 1];
        if (off >= 0x21u && off <= 0x5Fu && (off & 1u)) return smpc_oreg[(off - 0x21u) >> 1];
        if (off == 0x61u) return smpc_sr;
        if (off == 0x63u) return smpc_sf;
        if (off == 0x75u) {
            ++smpc_input_reads;
            return static_cast<uint8_t>(
                (inputs.pdr1_in & static_cast<uint8_t>(~smpc_ddr1)) | (smpc_pdr1_out & smpc_ddr1));
        }
        if (off == 0x77u) {
            ++smpc_input_reads;
            return static_cast<uint8_t>(
                (inputs.pdr2_in & static_cast<uint8_t>(~smpc_ddr2)) | (smpc_pdr2_out & smpc_ddr2));
        }
        return 0u;
    }
    if (cea >= 0x00180000u && cea + width <= 0x00190000u) {
        uint32_t out = 0u;
        for (uint8_t i = 0u; i < width; ++i) {
            const uint32_t off = (cea + i) - 0x00180000u;
            const uint8_t b = (off & 1u) ? backupram[off >> 1] : 0u;
            out = (out << 8) | b;
        }
        return out;
    }
    if (cea >= 0x00400000u && cea <= 0x0040003Fu &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        uint32_t out = 0u;
        for (uint8_t i = 0u; i < width; ++i) {
            const uint32_t lane_addr = cea + i;
            const uint32_t off = (lane_addr - 0x00400000u) & 0x1Fu;
            const uint8_t b = read_ioga_byte(off);
            out = (out << 8) | b;
            if ((off & 1u) != 0u) {
                log_ioga(static_cast<uint8_t>(cpu_id), pc, 0x00400000u + off, b, 0u, width);
            }
        }
        ++ioga_reads;
        return out;
    }
    if (cea >= 0x02000000u && cea + width <= kCartFullEnd &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        if (!has_cart) {
            fault.ok = false;
            fault.reason = "cart_rom_not_loaded";
            fault.pc = pc;
            fault.address = ea;
            fault.width = width;
            fault.is_write = false;
            return 0u;
        }
        ++cart_reads;
        if (cea >= kCartProtStart) {
            return crypt315.abus_read(cea, width, cart_rom, kCartPopulatedSize);
        }
        if (smpc_gamebank_select != 0u) {
            return 0u;
        }
        const uint32_t rel = cea - 0x02000000u;
        if (rel + width <= kCartPopulatedSize) {
            return rd_be(cart_rom, rel, width);
        }
        return 0u;
    }
    if (cea >= 0x05A00000u && cea + width <= 0x05B00000u &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        ++soundram_reads;
        return rd_be(sound_ram, cea - 0x05A00000u, width);
    }
    if (cea >= 0x05B00000u && cea + width <= 0x05B01000u &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        const uint32_t off = cea - 0x05B00000u;
        if (real_audio_enabled) {
            return scsp.read(off, static_cast<int>(width));
        }
        return rd_be(scsp_regs, off, width);
    }
    if (cea >= 0x05C00000u && cea + width <= 0x05C80000u) {
        return rd_be(vdp.vdp1_vram, cea - 0x05C00000u, width);
    }
    if (cea >= 0x05C80000u && cea + width <= 0x05CC0000u) {
        return vdp.vdp1_fb_read(cea - 0x05C80000u, width);
    }
    if (cea >= 0x05D00000u && cea + width <= 0x05D00020u) {
        return vdp.vdp1_reg_read(cea - 0x05D00000u, width);
    }
    if (cea == 0x057FFFFCu && width == 2u) {
        return 0u;  // Saturn/ST-V MINIT / clock-ratio latch read
    }
    if (cea >= 0x05890000u && cea + width <= 0x05890040u) {
        return 0u;  // Absent CD-Block MMIO on ST-V (probed by BIOS)
    }
    if (cea >= 0x05F00000u && cea + width <= 0x05F01000u) {
        return rd_be(vdp.vdp2_cram, cea - 0x05F00000u, width);
    }
    if (cea >= 0x05F80000u && cea + width <= 0x05F80200u) {
        const uint32_t off = cea - 0x05F80000u;
        if (off == 0x04u && width == 2u) {
            const uint16_t hblank = ((cpu_cycles % 1716ULL) >= 1400ULL) ? 0x0004u : 0x0000u;
            const uint16_t odd = (vbi_count & 1u) ? 0x0002u : 0x0000u;
            return vdp2_tvstat | hblank | odd;
        }
        if (off == 0x08u && width == 2u) return vdp2_hcnt;
        if (off == 0x0Au && width == 2u) return vdp2_vcnt;
        return rd_be(vdp.vdp2_regs, off, width);
    }
    if (cea >= 0x05FE0000u && cea + width <= 0x05FE00D0u && width == 4u && (cea & 3u) == 0u) {
        const uint32_t off = cea - 0x05FE0000u;
        if (off < 0x60u) {
            const auto& d = scu_dma[off / 0x20u];
            switch (off % 0x20u) {
                case 0x00u: return d.r;
                case 0x04u: return d.w;
                case 0x08u: return d.c;
                case 0x0Cu: return d.ad;
                case 0x10u: return d.en;
                case 0x14u: return d.md;
            }
        }
        if (off >= 0x60u && off <= 0x9Cu) return 0u;  // SCU DMAEN / DSTP / DSP / T0C..T1MD
        if (off == 0xA0u) return scu_ims;
        if (off == 0xA4u) return scu_ist;
        if (off == 0xC4u) return scu_rsel;
        if (off == 0xC8u) return 4u;
    }
    if (cea == 0xFFFFFE10u && width == 1u) {
        return inner.frt[static_cast<size_t>(cpu_id)].tier & 0xFFu;
    }
    if (cea == 0xFFFFFE11u && width == 1u) {
        return inner.frt[static_cast<size_t>(cpu_id)].read_ftcsr(cpu_cycles);
    }
    if ((cea == 0xFFFFFE12u && (width == 1u || width == 2u)) || (cea == 0xFFFFFE13u && width == 1u)) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.maybe_fire(cpu_cycles);
        f.resync(cpu_cycles);
        const uint32_t frc16 = f.frc & 0xFFFFu;
        if (width == 2u) return frc16;
        return (cea == 0xFFFFFE12u) ? ((frc16 >> 8) & 0xFFu) : (frc16 & 0xFFu);
    }
    if ((cea == 0xFFFFFE14u && (width == 1u || width == 2u)) || (cea == 0xFFFFFE15u && width == 1u)) {
        const auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        const uint32_t ocr16 = ((frt_tocr[cpu_id] & 0x10u) ? f.ocrb : f.ocra) & 0xFFFFu;
        if (width == 2u) return ocr16;
        return (cea == 0xFFFFFE14u) ? ((ocr16 >> 8) & 0xFFu) : (ocr16 & 0xFFu);
    }
    if (cea == 0xFFFFFE16u && width == 1u) {
        return inner.frt[static_cast<size_t>(cpu_id)].tcr & 0x83u;
    }
    if (cea == 0xFFFFFE17u && width == 1u) {
        return (frt_tocr[cpu_id] & 0x13u) | 0xE0u;
    }
    if ((cea == 0xFFFFFE18u && (width == 1u || width == 2u)) || (cea == 0xFFFFFE19u && width == 1u)) {
        const uint32_t icr16 = frt_icr[cpu_id] & 0xFFFFu;
        if (width == 2u) return icr16;
        return (cea == 0xFFFFFE18u) ? ((icr16 >> 8) & 0xFFu) : (icr16 & 0xFFu);
    }
    if (cea == 0xFFFFFE72u && width == 1u) return dmac1.drcr;
    if (cea == 0xFFFFFF90u && width == 4u) return dmac1.sar;
    if (cea == 0xFFFFFF94u && width == 4u) return dmac1.dar;
    if (cea == 0xFFFFFF98u && width == 4u) return dmac1.tcr;
    if (cea == 0xFFFFFF9Cu && width == 4u) return dmac1.chcr;
    return inner.read(cpu_id, ea, width, pc, cpu_cycles, fault);
}

void StvMachineState::write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
                            uint64_t cpu_cycles, BusAccessFault& fault) {
    if (is_sh2_cache_area(ea)) {
        return;
    }
    master_events_dirty = true;
    const uint32_t cea = canonicalize_stv_addr(ea);
    last_write_ea = cea;
    last_write_val = val;

    if (cea >= 0x00100000u && cea <= 0x0010007Fu && width == 1u) {
        const uint32_t off = cea - 0x00100000u;
        const uint8_t bval = static_cast<uint8_t>(val & 0xFFu);
        if (off >= 0x01u && off <= 0x0Du && (off & 1u)) {
            smpc_ireg[(off - 1u) >> 1] = bval;
            if (off == 0x01u && smpc_peri_pending) {
                if ((bval & 0x80u) != 0u) {
                    for (size_t i = 0u; i < 31u; ++i) smpc_oreg[i] = 0xFFu;
                    smpc_oreg[31] = 0x10u;
                    smpc_sr = static_cast<uint8_t>(0xC0u | ((smpc_ireg[1] >> 4) & 0x0Fu));
                    smpc_peri_pending = false;
                    smpc_irq_armed = true;
                    smpc_irq_cycle = cpu_cycles + 600u;
                } else if ((bval & 0x40u) != 0u) {
                    smpc_peri_pending = false;
                    smpc_sr &= static_cast<uint8_t>(~0x20u);
                }
            }
            return;
        }
        if (off == 0x1Fu) {
            if (bval == 0x00u) {
                // MSHON
            } else if (bval == 0x02u) {
                // SSHON: release/reset Slave SH-2 from reset vector
                cpus[1].pc = rd_be(bios_rom, 0u, 4u);
                cpus[1].r[15] = rd_be(bios_rom, 4u, 4u);
                cpus[1].vbr = 0u;
                cpus[1].sr = 0xF0u;
                cpus[1].cycles = cpus[0].cycles;
                slave_enabled = true;
            } else if (bval == 0x03u) {
                // SSHOFF
                slave_enabled = false;
            } else if (bval == 0x06u) {
                handle_smpc_pdr2_write(0x10u, 0x00u);
            } else if (bval == 0x07u) {
                handle_smpc_pdr2_write(0x00u, 0x10u);
            } else if (bval == 0x0Du) {
                // SYSRES: stop slave & reset Master after SMPC command completes
                slave_enabled = false;
                smpc_reset_master_pending = true;
            } else if (bval == 0x0Eu || bval == 0x0Fu) {
                // CKCHG352 / CKCHG320: stop slave & send NMI to Master (BIOS NMI handler at 0x00004420 pops frame & resumes)
                slave_enabled = false;
                pending_nmi = true;
            } else if (bval == 0x18u) {
                // NMIREQ
                pending_nmi = true;
            } else if (bval == 0x19u || bval == 0x1Au) {
                // RESENAB (0x19) / RESDISA (0x1A): reset-button mask control (no NMI)
            } else if (bval == 0x10u) {
                static constexpr uint8_t kDaysInMonth[13] = {
                    0u, 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
                };
                const uint64_t elapsed_sec = (cpu_cycles >= rtc_base_cycles)
                    ? ((cpu_cycles - rtc_base_cycles) / 28636360ULL)
                    : 0ULL;
                uint32_t sec = 35u + static_cast<uint32_t>(elapsed_sec);
                uint32_t min = 24u + sec / 60u; sec %= 60u;
                uint32_t hr  = 14u + min / 60u; min %= 60u;
                const uint32_t day_add = hr / 24u; hr %= 24u;
                const uint32_t dow = (6u + day_add) % 7u;
                uint32_t day = 7u + day_add;
                uint32_t mon = 11u;
                uint32_t yr = 2026u;
                while (day > kDaysInMonth[mon]) {
                    day -= kDaysInMonth[mon];
                    if (++mon > 12u) { mon = 1u; ++yr; }
                }
                const uint8_t kStatus[16] = {
                    0x40u,
                    to_bcd(yr / 100u),
                    to_bcd(yr % 100u),
                    static_cast<uint8_t>((dow << 4) | mon),
                    to_bcd(day),
                    to_bcd(hr),
                    to_bcd(min),
                    to_bcd(sec),
                    0x00u, 0x01u, 0x74u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
                };
                for (size_t i = 0u; i < 16u; ++i) smpc_oreg[i] = kStatus[i];
                for (size_t i = 16u; i < 31u; ++i) smpc_oreg[i] = 0x00u;
                smpc_sr = static_cast<uint8_t>(0x40u | ((smpc_ireg[1] & 8u) ? 0x20u : 0u) | ((smpc_ireg[1] >> 4) & 0x0Fu));
                smpc_peri_pending = (smpc_ireg[1] & 8u) != 0u;
                smpc_irq_armed = true;
                smpc_irq_cycle = cpu_cycles + 600u;
            }
            smpc_oreg[31] = bval;
            smpc_sf = 0u;
            log_mmio(MMIO_SMPC_CMD, static_cast<uint8_t>(cpu_id), pc, bval, smpc_ireg[1], 1u, cpu_cycles);
            return;
        }
        if (off == 0x63u) {
            smpc_sf = bval;
            return;
        }
        if (off == 0x75u) {
            smpc_pdr1_out = bval;
            smpc_gamebank_select = static_cast<uint8_t>(bval & 0x03u);
            return;
        }
        if (off == 0x77u) {
            const uint8_t prev = smpc_pdr2_out;
            smpc_pdr2_out = bval;
            handle_smpc_pdr2_write(prev, bval);
            return;
        }
        if (off == 0x79u) {
            smpc_ddr1 = bval;
            return;
        }
        if (off == 0x7Bu) {
            smpc_ddr2 = bval;
            return;
        }
        if (off >= 0x70u) return;
    }
    if (cea >= 0x00180000u && cea + width <= 0x00190000u) {
        for (uint8_t i = 0u; i < width; ++i) {
            const uint32_t off = (cea + i) - 0x00180000u;
            if (off & 1u) {
                backupram[off >> 1] = static_cast<uint8_t>((val >> ((width - 1u - i) * 8u)) & 0xFFu);
            }
        }
        return;
    }
    if (cea >= 0x00400000u && cea <= 0x0040003Fu &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        for (uint8_t i = 0u; i < width; ++i) {
            const uint32_t lane_addr = cea + i;
            const uint32_t off = (lane_addr - 0x00400000u) & 0x1Fu;
            const uint8_t b = static_cast<uint8_t>((val >> ((width - 1u - i) * 8u)) & 0xFFu);
            write_ioga_byte(off, b);
            if ((off & 1u) != 0u) {
                log_ioga(static_cast<uint8_t>(cpu_id), pc, 0x00400000u + off, b, 1u, width);
            }
        }
        ++ioga_writes;
        return;
    }
    if (cea >= 0x02000000u && cea + width <= kCartFullEnd &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        if (cea + width > kCartProtStart) {
            crypt315.abus_write(cea, width, val);
            cart_prot_enabled = (crypt315.abus_protenable & 0x00010000u) != 0u;
            return;
        }
        return;
    }
    if (cpu_id == 0 && cea >= 0x01000000u && cea <= 0x017FFFFFu && width == 2u) {
        auto& s_frt = inner.frt[1];
        s_frt.resync(cpus[1].cycles);
        frt_icr[1] = static_cast<uint16_t>(s_frt.frc & 0xFFFFu);
        s_frt.ftcsr |= 0x80u;
        ++minit_count;
        log_mmio(MMIO_MINIT, 0u, pc, minit_count, val, 2u, cpu_cycles);
        return;
    }
    if (cpu_id == 1 && cea >= 0x01800000u && cea <= 0x01FFFFFFu && width == 2u) {
        auto& m_frt = inner.frt[0];
        m_frt.resync(cpus[0].cycles);
        frt_icr[0] = static_cast<uint16_t>(m_frt.frc & 0xFFFFu);
        m_frt.ftcsr |= 0x80u;
        ++sinit_count;
        return;
    }
    if (cea >= 0x05A00000u && cea + width <= 0x05B00000u &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        ++soundram_writes;
        const uint32_t soff = cea - 0x05A00000u;
        wr_be(sound_ram, soff, width, val);
        if (!m68k_enabled) {
            const bool is_cmd_slot = (soff >= 0x700u && soff < 0x780u && ((soff & 0xFu) == 0u) &&
                                      sound_ram[soff] != 0u);
            if (((sound_ram[0x4E0u] & 0x80u) != 0u || (sound_ram[0x4E1u] & 0x80u) != 0u || is_cmd_slot) &&
                !scsp_ack_armed) {
                scsp_ack_armed = true;
                scsp_ack_cycle = cpu_cycles + 2000u;
            }
        }
        return;
    }
    if (cea >= 0x05B00000u && cea + width <= 0x05B01000u &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        const uint32_t off = cea - 0x05B00000u;
        wr_be(scsp_regs, off, width, val);
        if (real_audio_enabled) {
            scsp.write(off, val, static_cast<int>(width), sound_ram);
            if (m68k_enabled) {
                switch_active_m68k(this);
                m68k_set_irq(static_cast<unsigned int>(scsp.m68k_irq_level()));
            }
        }
        return;
    }
    if (cea >= 0x05C00000u && cea + width <= 0x05C80000u &&
        (width == 1u || (width == 2u && (cea & 1u) == 0u) || (width == 4u && (cea & 3u) == 0u))) {
        wr_be(vdp.vdp1_vram, cea - 0x05C00000u, width, val);
        ++vdp1_vram_writes;
        return;
    }
    if (cea >= 0x05C80000u && cea + width <= 0x05CC0000u) {
        vdp.vdp1_fb_write(cea - 0x05C80000u, val, width);
        return;
    }
    if (cea >= 0x05D00000u && cea + width <= 0x05D00020u && (cea & 1u) == 0u && (width == 2u || width == 4u)) {
        vdp.vdp1_reg_write(cea - 0x05D00000u, val, width);
        if (vdp.vdp1_unsupported) {
            fault.ok = false;
            fault.reason = "unsupported_vdp1_reg_access";
            fault.pc = pc;
            fault.address = ea;
            fault.width = width;
            fault.is_write = true;
            return;
        }
        log_mmio(MMIO_VDP1_REG_W, static_cast<uint8_t>(cpu_id), pc, cea, val, width, cpu_cycles);
        return;
    }
    if (cea == 0x057FFFFCu && width == 2u) {
        return;  // Saturn/ST-V MINIT / clock-ratio latch write from BIOS SYS_INIT
    }
    if (cea >= 0x05890000u && cea + width <= 0x05890040u) {
        return;  // Absent CD-Block MMIO on ST-V (probed by BIOS)
    }
    if (cea >= 0x05F00000u && cea + width <= 0x05F01000u) {
        wr_be(vdp.vdp2_cram, cea - 0x05F00000u, width, val);
        return;
    }
    if (cea >= 0x05F80000u && cea + width <= 0x05F80200u) {
        wr_be(vdp.vdp2_regs, cea - 0x05F80000u, width, val);
        return;
    }
    if (cea >= 0x05FE0000u && cea + width <= 0x05FE00D0u && width == 4u && (cea & 3u) == 0u) {
        const uint32_t off = cea - 0x05FE0000u;
        if (off < 0x60u) {
            const int l = static_cast<int>(off / 0x20u);
            auto& d = scu_dma[l];
            switch (off % 0x20u) {
                case 0x00u: d.r = val & 0x07FFFFFFu; return;
                case 0x04u: d.w = val & 0x07FFFFFFu; return;
                case 0x08u: d.c = val; return;
                case 0x0Cu: d.ad = val; return;
                case 0x10u:
                    d.en = val & 0x100u;
                    if ((val & 0x101u) == 0x101u && (d.md & 7u) == 7u) {
                        run_scu_dma(cpu_id, l, pc, cpu_cycles, fault);
                    }
                    return;
                case 0x14u: d.md = val; return;
            }
        }
        if (off >= 0x60u && off <= 0x9Cu) { return; }  // SCU DMAEN / DSTP / DSP / T0C..T1MD
        if (off == 0xA0u) { scu_ims = val; return; }
        if (off == 0xA4u) { scu_ist &= val; return; }
        if (off == 0xA8u || off == 0xACu) { return; }  // SCU A-Bus IRQ ack / setup
        if (off == 0xB0u || off == 0xB4u || off == 0xB8u || off == 0xBCu) { return; }  // SCU A-Bus set/refresh
        if (off == 0xC4u) { scu_rsel = val; return; }  // SCU SDRAM select
    }
    if (cea == 0xFFFFFE10u && width == 1u) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.resync(cpu_cycles);
        f.tier = val & 0xFFu;
        f.activate();
        return;
    }
    if ((cea == 0xFFFFFE12u && (width == 1u || width == 2u)) || (cea == 0xFFFFFE13u && width == 1u)) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.resync(cpu_cycles);
        if (width == 2u) f.frc = val & 0xFFFFu;
        else if (cea == 0xFFFFFE12u) f.frc = (f.frc & 0x00FFu) | ((val & 0xFFu) << 8);
        else f.frc = (f.frc & 0xFF00u) | (val & 0xFFu);
        f.activate();
        return;
    }
    if ((cea == 0xFFFFFE14u && (width == 1u || width == 2u)) || (cea == 0xFFFFFE15u && width == 1u)) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.resync(cpu_cycles);
        uint32_t& ocr = (frt_tocr[cpu_id] & 0x10u) ? f.ocrb : f.ocra;
        if (width == 2u) ocr = val & 0xFFFFu;
        else if (cea == 0xFFFFFE14u) ocr = (ocr & 0x00FFu) | ((val & 0xFFu) << 8);
        else ocr = (ocr & 0xFF00u) | (val & 0xFFu);
        f.activate();
        return;
    }
    if (cea == 0xFFFFFE16u && width == 1u) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.resync(cpu_cycles);
        f.tcr = val & 0x83u;
        f.activate();
        return;
    }
    if (cea == 0xFFFFFE17u && width == 1u) {
        auto& f = inner.frt[static_cast<size_t>(cpu_id)];
        f.resync(cpu_cycles);
        frt_tocr[cpu_id] = static_cast<uint8_t>(val & 0x13u);
        f.activate();
        return;
    }
    if (cea == 0xFFFFFE72u && width == 1u) { dmac1.drcr = static_cast<uint8_t>(val & 0xFFu); return; }
    if (cea == 0xFFFFFF90u && width == 4u) { dmac1.sar = val; return; }
    if (cea == 0xFFFFFF94u && width == 4u) { dmac1.dar = val; return; }
    if (cea == 0xFFFFFF98u && width == 4u) { dmac1.tcr = val & 0x00FFFFFFu; return; }
    if (cea == 0xFFFFFF9Cu && width == 4u) {
        const uint32_t prev = dmac1.chcr;
        dmac1.chcr = (val & ~2u) | (prev & val & 2u);
        run_sh_dmac1(cpu_id, pc, cpu_cycles, fault);
        return;
    }
    if (cea == 0xFFFFFE71u || (cea >= 0xFFFFFF80u && cea <= 0xFFFFFF8Fu) || cea == 0xFFFFFFB0u) {
        inner.dmac[static_cast<size_t>(cpu_id)].write_reg(cea, width, val, pc, cpu_cycles, *this, fault);
        if (cea == 0xFFFFFFB0u && width == 4u && fault.ok) {
            run_sh_dmac1(cpu_id, pc, cpu_cycles, fault);
        }
        return;
    }
    if (cea >= kHighWramBase && cea <= 0x07FFFFFFu) {
        mark_high_wram_dirty((cea - kHighWramBase) & (kHighWramSize - 1u), width);
    }
    inner.write(cpu_id, ea, width, val, pc, cpu_cycles, fault);
}

bool StvMachineState::inject_sh2_irq(int cpu_id, uint8_t vector, uint8_t level, BusAccessFault& fault) {
    Sh2CpuState& cpu = cpus[cpu_id];
    if (cpu.delay != 0u) return false;
    const uint32_t cur_imask = (cpu.sr >> 4) & 0xFu;
    if (level <= cur_imask) return false;
    const uint32_t saved_pc = cpu.pc;
    cpu.r[15] -= 4u;
    write(cpu_id, cpu.r[15], 4u, cpu.sr, saved_pc, cpu.cycles, fault);
    if (!fault.ok) return false;
    cpu.r[15] -= 4u;
    write(cpu_id, cpu.r[15], 4u, saved_pc, saved_pc, cpu.cycles, fault);
    if (!fault.ok) return false;
    const uint32_t clamped_level = (level > 0xFu) ? 0xFu : static_cast<uint32_t>(level);
    cpu.sr = (cpu.sr & ~kShSrImaskMask) | (clamped_level << 4);
    const uint32_t new_pc = read(cpu_id, cpu.vbr + static_cast<uint32_t>(vector) * 4u, 4u,
                                 saved_pc, cpu.cycles, fault);
    if (!fault.ok) return false;
    log_mmio(MMIO_IRQ, static_cast<uint8_t>(cpu_id), saved_pc, vector, level, 4u, cpu.cycles);
    cpu.pc = new_pc;
    return true;
}

inline uint16_t fast_be16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

inline uint32_t fast_be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline void fast_store_be16(uint8_t* p, uint32_t val) {
    const uint16_t v = __builtin_bswap16(static_cast<uint16_t>(val));
    std::memcpy(p, &v, 2);
}

inline void fast_store_be32(uint8_t* p, uint32_t val) {
    const uint32_t v = __builtin_bswap32(val);
    std::memcpy(p, &v, 4);
}

inline uint32_t StvMachineState::fast_read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                                           uint64_t cpu_cycles, BusAccessFault& fault) {
    if (__builtin_expect(ea < 0x40000000u, 1)) {
        const uint32_t cea = ea & 0x07FFFFFFu;
        const uint32_t h_off = cea - kHighWramBase;
        if (__builtin_expect(h_off < kHighWramSize && ((cea & (width - 1u)) == 0u), 1)) {
            const uint8_t* p = inner.high_wram.data() + h_off;
            if (width == 2u) return fast_be16(p);
            if (width == 4u) return fast_be32(p);
            return p[0];
        }
        const uint32_t l_off = cea - kLowWramBase;
        if (l_off < kLowWramSize && ((cea & (width - 1u)) == 0u)) {
            const uint8_t* p = inner.low_wram.data() + l_off;
            if (width == 2u) return fast_be16(p);
            if (width == 4u) return fast_be32(p);
            return p[0];
        }
        const uint32_t c_rel = cea - 0x02000000u;
        if (c_rel < (kCartProtStart - 0x02000000u) && !cart_prot_enabled && has_cart &&
            smpc_gamebank_select == 0u && ((cea & (width - 1u)) == 0u)) {
            ++cart_reads;
            if (__builtin_expect(c_rel + width <= kCartPopulatedSize, 1)) {
                const uint8_t* p = cart_rom + c_rel;
                if (width == 4u) return fast_be32(p);
                if (width == 2u) return fast_be16(p);
                return p[0];
            }
            return 0u;
        }
    }
    return read(cpu_id, ea, width, pc, cpu_cycles, fault);
}

inline void StvMachineState::fast_write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val,
                                        uint32_t pc, uint64_t cpu_cycles, BusAccessFault& fault) {
    if (__builtin_expect(ea < 0x40000000u, 1)) {
        const uint32_t cea = ea & 0x07FFFFFFu;
        const uint32_t h_off = cea - kHighWramBase;
        if (__builtin_expect(h_off < kHighWramSize && ((cea & (width - 1u)) == 0u), 1)) {
            last_write_ea = cea;
            last_write_val = val;
            mark_high_wram_dirty(h_off, width);
            uint8_t* p = inner.high_wram.data() + h_off;
            if (width == 4u) {
                fast_store_be32(p, val);
                return;
            }
            if (width == 2u) {
                fast_store_be16(p, val);
                return;
            }
            p[0] = static_cast<uint8_t>(val & 0xFFu);
            return;
        }
        const uint32_t l_off = cea - kLowWramBase;
        if (l_off < kLowWramSize && ((cea & (width - 1u)) == 0u)) {
            last_write_ea = cea;
            last_write_val = val;
            uint8_t* p = inner.low_wram.data() + l_off;
            if (width == 4u) {
                fast_store_be32(p, val);
                return;
            }
            if (width == 2u) {
                fast_store_be16(p, val);
                return;
            }
            p[0] = static_cast<uint8_t>(val & 0xFFu);
            return;
        }
    }
    write(cpu_id, ea, width, val, pc, cpu_cycles, fault);
}

inline bool is_hook_pc_range(uint32_t lo, uint32_t hi, bool copa_active) {
    if (0x060053E0u >= lo && 0x060053E0u <= hi) return true;
    if (!copa_active) return false;
    if (0x0600582Au >= lo && 0x0600582Au <= hi) return true;
    if (0x06005888u >= lo && 0x06005888u <= hi) return true;
    if (hi >= 0x06005894u && lo <= 0x06005920u) return true;
    if (0x06007DAEu >= lo && 0x06007DAEu <= hi) return true;
    if (0x060085FEu >= lo && 0x060085FEu <= hi) return true;
    if (0x06008624u >= lo && 0x06008624u <= hi) return true;
    if (0x0604EDFAu >= lo && 0x0604EDFAu <= hi) return true;
    if (hi >= 0x060519C0u && lo <= 0x0605219Cu) return true;
    if (0x060529A6u >= lo && 0x060529A6u <= hi) return true;
    if (0x06052A94u >= lo && 0x06052A94u <= hi) return true;
    return false;
}

inline bool read_wram_ea(const SaturnStvBus& inner, uint32_t ea, uint8_t width, uint32_t& out) {
    if (ea >= 0x40000000u) return false;
    const uint32_t cea = ea & 0x07FFFFFFu;
    if ((cea & (width - 1u)) != 0u) return false;
    const uint8_t* base = nullptr;
    uint32_t off = 0u;
    if (cea >= kHighWramBase && cea + width <= kHighWramBase + kHighWramSize) {
        base = inner.high_wram.data();
        off = cea - kHighWramBase;
    } else if (cea >= kLowWramBase && cea + width <= kLowWramBase + kLowWramSize) {
        base = inner.low_wram.data();
        off = cea - kLowWramBase;
    } else {
        return false;
    }
    if (width == 1u) out = base[off];
    else if (width == 2u) out = fast_be16(base + off);
    else out = fast_be32(base + off);
    return true;
}

struct Poll3LoopInfo {
    uint32_t head_pc = 0u;
    uint32_t phase = 0u;
    bool is_ftcsr = false;
    bool cond = false;
};

inline bool match_poll3_loop_at(int cpu_id, uint32_t head_pc, const Sh2CpuState& cpu, const SaturnStvBus& inner,
                                bool copa_active, Poll3LoopInfo& info) {
    if (head_pc >= 0x40000000u) return false;
    const uint32_t cpc = head_pc & 0x07FFFFFFu;
    if ((cpc & 1u) != 0u || cpc < kHighWramBase || cpc + 6u > kHighWramBase + kHighWramSize) return false;
    const uint8_t* code = inner.high_wram.data() + (cpc - kHighWramBase);
    const uint16_t op0 = fast_be16(code);
    if ((op0 & 0xFF0Fu) != 0x6000u) return false;  // mov.b @Rm, r0
    const uint32_t m = (op0 >> 4) & 0xFu;
    if (m == 0u) return false;  // Rm == r0 would be overwritten by mov.b
    const uint16_t op1 = fast_be16(code + 2u);
    if ((op1 & 0xFF00u) != 0xC800u) return false;  // tst #d8, r0
    const uint16_t op2 = fast_be16(code + 4u);
    if (op2 != 0x89FCu && op2 != 0x8BFCu) return false;  // bt/bf head_pc
    if (cpu_id == 0 && is_hook_pc_range(head_pc, head_pc + 4u, copa_active)) return false;
    const uint32_t imm = op1 & 0xFFu;
    const uint32_t ea = cpu.r[m];
    if (is_sh2_cache_area(ea)) return false;
    const uint32_t cea = canonicalize_stv_addr(ea);
    bool cond = false;
    if (cea == 0xFFFFFE11u) {
        if ((imm & 0x0Eu) != 0u) return false;  // maybe_fire can alter OCFA/OCFB/OVF bits
        const uint8_t v = static_cast<uint8_t>(inner.frt[static_cast<size_t>(cpu_id)].ftcsr & 0xFFu);
        cond = ((v & imm) == 0u);
        info.is_ftcsr = true;
    } else {
        uint32_t v = 0u;
        if (!read_wram_ea(inner, ea, 1u, v)) return false;
        cond = ((v & imm) == 0u);
        info.is_ftcsr = false;
    }
    const bool loops = (op2 == 0x89FCu) ? cond : !cond;
    if (!loops) return false;
    info.head_pc = head_pc;
    info.cond = cond;
    return true;
}

inline bool match_poll3_loop_any_phase(int cpu_id, const Sh2CpuState& cpu, const SaturnStvBus& inner,
                                       bool copa_active, Poll3LoopInfo& info) {
    if (cpu.delay != 0u) return false;
    for (uint32_t ph = 0u; ph < 3u; ++ph) {
        const uint32_t head_pc = cpu.pc - ph * 2u;
        if (match_poll3_loop_at(cpu_id, head_pc, cpu, inner, copa_active, info)) {
            if (ph == 1u && ((cpu.r[0] & 0xFFu) != (inner.frt[static_cast<size_t>(cpu_id)].ftcsr & 0xFFu)) && info.is_ftcsr) {
                continue;
            }
            if (ph == 2u) {
                const bool t_set = (cpu.sr & kShSrT) != 0u;
                const uint32_t cpc = head_pc & 0x07FFFFFFu;
                const uint16_t op2 = fast_be16(inner.high_wram.data() + (cpc - kHighWramBase) + 4u);
                const bool will_take = (op2 == 0x89FCu) ? t_set : !t_set;
                if (!will_take) continue;
            }
            info.phase = ph;
            return true;
        }
    }
    return false;
}

struct MasterVblLoopInfo {
    uint32_t head_pc = 0u;
    uint32_t phase = 0u;
    uint32_t cb_pc = 0u;
    uint32_t flag_sext = 0u;
};

inline bool match_master_vbl_loop_at(uint32_t head_pc, const Sh2CpuState& cpu, const SaturnStvBus& inner,
                                     bool copa_active, MasterVblLoopInfo& info) {
    if (head_pc >= 0x40000000u) return false;
    const uint32_t cpc = head_pc & 0x07FFFFFFu;
    if ((cpc & 1u) != 0u || cpc < kHighWramBase || cpc + 12u > kHighWramBase + kHighWramSize) return false;
    const uint8_t* code = inner.high_wram.data() + (cpc - kHighWramBase);
    const uint16_t op0 = fast_be16(code);
    if ((op0 & 0xFF00u) != 0xC600u) return false;  // mov.l @(d8*4, gbr), r0
    if (fast_be16(code + 2u) != 0x400Bu) return false;  // jsr @r0
    if (fast_be16(code + 4u) != 0x0009u) return false;  // nop
    const uint16_t op3 = fast_be16(code + 6u);
    if ((op3 & 0xFF00u) != 0xC400u) return false;  // mov.b @(d8, gbr), r0
    if (fast_be16(code + 8u) != 0x4011u) return false;  // cmp/pz r0
    if (fast_be16(code + 10u) != 0x89F9u) return false; // bt head_pc
    if (is_hook_pc_range(head_pc, head_pc + 10u, copa_active)) return false;
    uint32_t cb_pc = 0u;
    if (!read_wram_ea(inner, cpu.gbr + ((op0 & 0xFFu) << 2), 4u, cb_pc)) return false;
    if (cb_pc >= 0x40000000u) return false;
    const uint32_t cb_cpc = cb_pc & 0x07FFFFFFu;
    if ((cb_cpc & 1u) != 0u || cb_cpc < kHighWramBase || cb_cpc + 4u > kHighWramBase + kHighWramSize) return false;
    const uint8_t* cb_code = inner.high_wram.data() + (cb_cpc - kHighWramBase);
    if (fast_be16(cb_code) != 0x000Bu || fast_be16(cb_code + 2u) != 0x0009u) return false;  // rts; nop
    if (is_hook_pc_range(cb_pc, cb_pc + 2u, copa_active)) return false;
    uint32_t flag_u8 = 0u;
    if (!read_wram_ea(inner, cpu.gbr + (op3 & 0xFFu), 1u, flag_u8)) return false;
    const uint32_t flag_sext = sign_extend_8(flag_u8);
    if ((flag_sext & 0x80000000u) != 0u) return false;  // cmp/pz requires >= 0
    info.head_pc = head_pc;
    info.cb_pc = cb_pc;
    info.flag_sext = flag_sext;
    return true;
}

inline bool match_master_vbl_loop_any_phase(const Sh2CpuState& cpu, const SaturnStvBus& inner,
                                            bool copa_active, MasterVblLoopInfo& info) {
    if (cpu.delay == 0u) {
        if (match_master_vbl_loop_at(cpu.pc, cpu, inner, copa_active, info)) {
            info.phase = 0u;
            return true;
        }
        if (match_master_vbl_loop_at(cpu.pc - 2u, cpu, inner, copa_active, info) && cpu.r[0] == info.cb_pc) {
            info.phase = 1u;
            return true;
        }
        if (cpu.pr >= kHighWramBase + 6u && match_master_vbl_loop_at(cpu.pr - 6u, cpu, inner, copa_active, info) && cpu.pc == info.cb_pc) {
            info.phase = 3u;
            return true;
        }
        if (match_master_vbl_loop_at(cpu.pc - 6u, cpu, inner, copa_active, info)) {
            info.phase = 5u;
            return true;
        }
        if (match_master_vbl_loop_at(cpu.pc - 8u, cpu, inner, copa_active, info) && static_cast<int32_t>(cpu.r[0]) >= 0) {
            info.phase = 6u;
            return true;
        }
        if (match_master_vbl_loop_at(cpu.pc - 10u, cpu, inner, copa_active, info) && (cpu.sr & kShSrT) != 0u) {
            info.phase = 7u;
            return true;
        }
        return false;
    }
    if (match_master_vbl_loop_at(cpu.pc - 4u, cpu, inner, copa_active, info) && cpu.delay == info.cb_pc) {
        info.phase = 2u;
        return true;
    }
    if (cpu.delay >= kHighWramBase + 6u && match_master_vbl_loop_at(cpu.delay - 6u, cpu, inner, copa_active, info) && cpu.pc == info.cb_pc + 2u) {
        info.phase = 4u;
        return true;
    }
    return false;
}

bool StvMachineState::run_rounds(uint32_t num_rounds) {
    return run_rounds_ex(num_rounds, true);
}

bool StvMachineState::run_rounds_ex(uint32_t num_rounds, bool emit_audio_ring) {
    if (halted) return false;
    const bool prev_emit = emit_audio_ring_;
    emit_audio_ring_ = emit_audio_ring;
    const Sh2OpClass* decode_lut = sh2_decode_lut();
    BusAccessFault fault{};
    const uint32_t start_vbi_count = vbi_count;
    const uint32_t target_samples = (real_audio_enabled && num_rounds > 0u)
        ? ((num_rounds == 10000u) ? 735u : ((num_rounds * 735u + 5000u) / 10000u))
        : 0u;
    uint32_t sample_acc = 0u;

    if (copa_mode_active && vdp.copa_bracket.progress_active != 0u) {
        if (real_audio_enabled && target_samples > 0u) {
            step_audio_samples(target_samples);
        }
        emit_audio_ring_ = prev_emit;
        return true;
    }

    uint64_t next_evt_cyc = 0u;
    bool any_pending_check = false;
    auto refresh_master_events = [&]() {
        uint64_t cyc = (next_vbi_cyc < next_vbo_cyc) ? next_vbi_cyc : next_vbo_cyc;
        if (smpc_irq_armed && smpc_irq_cycle < cyc) cyc = smpc_irq_cycle;
        if (!m68k_enabled && scsp_ack_armed && scsp_ack_cycle < cyc) cyc = scsp_ack_cycle;
        for (int l = 0; l < 3; ++l) {
            if (scu_dma_irq_armed[l] && scu_dma_irq_cycle[l] < cyc) cyc = scu_dma_irq_cycle[l];
        }
        next_evt_cyc = cyc;
        any_pending_check = pending_nmi ||
                            (pending_vbi && (scu_ims & 0x0001u) == 0u) ||
                            (pending_vbo && (scu_ims & 0x0002u) == 0u) ||
                            (pending_sound_irq && (scu_ims & 0x0040u) == 0u);
        master_events_dirty = false;
    };
    refresh_master_events();

    uint32_t r = 0u;
    while (r < num_rounds) {
        if (idle_skip_enabled && r + 3u <= num_rounds) {
            if (master_events_dirty) refresh_master_events();
            if (!any_pending_check && !smpc_reset_master_pending && cpus[0].cycles + 144ULL <= next_evt_cyc) {
                MasterVblLoopInfo m_info{};
                if (match_master_vbl_loop_any_phase(cpus[0], inner, copa_mode_active, m_info)) {
                    Poll3LoopInfo s_info{};
                    const bool slave_idle = !slave_enabled ||
                        (match_poll3_loop_any_phase(1, cpus[1], inner, copa_mode_active, s_info) && s_info.is_ftcsr);
                    if (slave_idle) {
                        uint32_t blocks = (num_rounds - r) / 3u;
                        const uint64_t evt_blocks64 = (next_evt_cyc - cpus[0].cycles) / 144ULL;
                        if (evt_blocks64 < blocks) blocks = static_cast<uint32_t>(evt_blocks64);
                        const bool fast_cpu_ready = fast_cpu_enabled && (sched2_hits > 0u);
                        if (real_audio_enabled && target_samples > 0u && !fast_cpu_ready) {
                            const uint32_t rounds_before_emit = (num_rounds - 1u - sample_acc) / target_samples;
                            const uint32_t aud_blocks = rounds_before_emit / 3u;
                            if (aud_blocks < blocks) blocks = aud_blocks;
                        }
                        if (blocks > 0u) {
                            const uint32_t skip_rounds = blocks * 3u;
                            const uint32_t skip_steps = blocks * 96u;
                            const uint32_t m_head = (m_info.phase == 0u) ? 0u : (8u - m_info.phase);
                            const uint32_t m_tail = m_info.phase;
                            const uint32_t m_iters = blocks * 12u - ((m_info.phase == 0u) ? 0u : 1u);
                            for (uint32_t k = 0u; k < m_head; ++k) {
                                step_sh2_with_bus(0, cpus[0], *this, decode_lut, fault);
                            }
                            cpus[0].r[0] = m_info.flag_sext;
                            cpus[0].pr = m_info.head_pc + 6u;
                            cpus[0].sr |= kShSrT;
                            cpus[0].pc = m_info.head_pc;
                            cpus[0].cur_pc = m_info.head_pc + 10u;
                            cpus[0].delay = 0u;
                            cpus[0].retired += static_cast<uint64_t>(m_iters) * 8ULL;
                            cpus[0].cycles += static_cast<uint64_t>(m_iters) * 12ULL;
                            for (uint32_t k = 0u; k < m_tail; ++k) {
                                step_sh2_with_bus(0, cpus[0], *this, decode_lut, fault);
                            }
                            main_steps += skip_steps;

                            if (slave_enabled) {
                                const uint32_t s_head = (s_info.phase == 0u) ? 0u : (3u - s_info.phase);
                                const uint32_t s_tail = s_info.phase;
                                const uint32_t s_iters = blocks * 32u - ((s_info.phase == 0u) ? 0u : 1u);
                                for (uint32_t k = 0u; k < s_head; ++k) {
                                    step_sh2_with_bus(1, cpus[1], *this, decode_lut, fault);
                                }
                                const uint64_t last_read_cyc = cpus[1].cycles + (static_cast<uint64_t>(s_iters) - 1ULL) * 5ULL;
                                inner.frt[1].maybe_fire(last_read_cyc);
                                cpus[1].r[0] = sign_extend_8(inner.frt[1].ftcsr & 0xFFu);
                                cpus[1].sr |= kShSrT;
                                cpus[1].pc = s_info.head_pc;
                                cpus[1].cur_pc = s_info.head_pc + 4u;
                                cpus[1].delay = 0u;
                                cpus[1].retired += static_cast<uint64_t>(s_iters) * 3ULL;
                                cpus[1].cycles += static_cast<uint64_t>(s_iters) * 5ULL;
                                for (uint32_t k = 0u; k < s_tail; ++k) {
                                    step_sh2_with_bus(1, cpus[1], *this, decode_lut, fault);
                                }
                                slave_steps += skip_steps;
                            } else {
                                cpus[1].cycles = cpus[0].cycles;
                            }
                            if (real_audio_enabled && target_samples > 0u) {
                                sample_acc += skip_rounds * target_samples;
                                if (fast_cpu_ready && sample_acc >= num_rounds) {
                                    const uint32_t emit = sample_acc / num_rounds;
                                    sample_acc %= num_rounds;
                                    step_audio_samples(emit);
                                }
                            }
                            rounds_done += skip_rounds;
                            r += skip_rounds;
                            continue;
                        }
                    }
                }
            }
        }

        const bool allow_large_quantum = fast_cpu_enabled && (sched2_hits > 0u) && !cart_prot_enabled &&
                                         cpus[0].pc >= kAotCodeLo && cpus[0].pc < kAotCodeHi;
        const uint32_t rem_rounds = num_rounds - r;
        const uint32_t batch_rounds = allow_large_quantum
            ? ((rem_rounds < 16u) ? rem_rounds : 16u)
            : 1u;
        const int batch_steps = static_cast<int>(batch_rounds * 32u);

        for (int cpu_id = 0; cpu_id < 2; ++cpu_id) {
            if (cpu_id == 1 && !slave_enabled) {
                cpus[1].cycles = cpus[0].cycles;
                continue;
            }
            int slot = 0;
            while (slot < batch_steps) {
                if (cpu_id == 0) {
                    if (__builtin_expect(master_events_dirty || any_pending_check || cpus[0].cycles >= next_evt_cyc, 0)) {
                        if (pending_nmi && inject_sh2_irq(0, 0x0Bu, 0x10u, fault)) {
                            pending_nmi = false;
                        }
                        if (cpus[0].cycles >= next_vbi_cyc) {
                            pending_vbi = true;
                            next_vbi_cyc += kFrameCycles;
                            vdp2_tvstat |= 0x0008u;
                        }
                        if (pending_vbi && (scu_ims & 0x0001u) == 0u &&
                            inject_sh2_irq(0, 0x40u, 0xFu, fault)) {
                            pending_vbi = false;
                            ++vbi_count;
                        }
                        if (cpus[0].cycles >= next_vbo_cyc) {
                            pending_vbo = true;
                            next_vbo_cyc += kFrameCycles;
                            vdp2_tvstat &= static_cast<uint16_t>(~0x0008u);
                            vdp.vblank_out();
                        }
                        if (pending_vbo && (scu_ims & 0x0002u) == 0u &&
                            inject_sh2_irq(0, 0x41u, 0xEu, fault)) {
                            pending_vbo = false;
                            ++vbo_count;
                        }
                        if (smpc_irq_armed && cpus[0].cycles >= smpc_irq_cycle &&
                            (scu_ims & 0x0080u) == 0u &&
                            inject_sh2_irq(0, 0x47u, 0x8u, fault)) {
                            smpc_irq_armed = false;
                        }
                        if (!m68k_enabled && scsp_ack_armed && cpus[0].cycles >= scsp_ack_cycle) {
                            sound_ram[0x4E0u] &= 0x7Fu;
                            sound_ram[0x4E1u] &= 0x7Fu;
                            for (uint32_t s = 0u; s < 8u; ++s) {
                                const uint32_t off = 0x700u + s * 0x10u;
                                if (sound_ram[off] != 0u) {
                                    const uint32_t seq = ++scsp_event_seq;
                                    ScspCmdEvent& ev = scsp_events[(seq - 1u) % kMaxScspEvents];
                                    ev.seq = seq;
                                    ev.slot = s;
                                    ev.w0 = (static_cast<uint32_t>(sound_ram[off]) << 24) |
                                            (static_cast<uint32_t>(sound_ram[off + 1u]) << 16) |
                                            (static_cast<uint32_t>(sound_ram[off + 2u]) << 8) |
                                            static_cast<uint32_t>(sound_ram[off + 3u]);
                                    ev.w1 = (static_cast<uint32_t>(sound_ram[off + 4u]) << 24) |
                                            (static_cast<uint32_t>(sound_ram[off + 5u]) << 16) |
                                            (static_cast<uint32_t>(sound_ram[off + 6u]) << 8) |
                                            static_cast<uint32_t>(sound_ram[off + 7u]);
                                }
                                sound_ram[off] = 0u;
                            }
                            scsp_ack_armed = false;
                        }
                        if (pending_sound_irq && (scu_ims & 0x0040u) == 0u &&
                            inject_sh2_irq(0, 0x46u, 0x9u, fault)) {
                            pending_sound_irq = false;
                        }
                        if (scu_dma_irq_armed[0] || scu_dma_irq_armed[1] || scu_dma_irq_armed[2]) {
                            for (int lvl = 2; lvl >= 0; --lvl) {
                                const uint32_t mask = (lvl == 0) ? 0x0800u : (lvl == 1) ? 0x0400u : 0x0200u;
                                const uint8_t vec = static_cast<uint8_t>(0x4Bu - lvl);
                                const uint8_t irl = (lvl == 0) ? 0x5u : 0x6u;
                                if (scu_dma_irq_armed[lvl] &&
                                    cpus[0].cycles >= scu_dma_irq_cycle[lvl] &&
                                    (scu_ims & mask) == 0u &&
                                    inject_sh2_irq(0, vec, irl, fault)) {
                                    scu_dma_irq_armed[lvl] = false;
                                }
                            }
                        }
                        refresh_master_events();
                    }

                    const uint32_t pc = cpus[0].pc;
                    if (__builtin_expect(pc == 0x060053E0u, 0)) {
                        ++sched2_hits;
                    }
                    if (__builtin_expect(copa_mode_active, 0)) {
                        if (!copa_entered_gameplay &&
                            ((pc >= 0x06005894u && pc <= 0x06005920u) ||
                             (pc >= 0x060519C0u && pc <= 0x06052100u))) {
                            copa_entered_gameplay = true;
                        } else if (copa_entered_gameplay &&
                                   (pc == 0x06005888u || pc == 0x0600582Au)) {
                            copa_returned_to_title = true;
                        }
                        if (pc >= 0x060519C0u && pc <= 0x0605219Cu) {
                            copa_ts_heartbeat = 4u;
                        }
                        if (pc == 0x0604EDFAu) {
                            ++copa_sp3_kick_count;
                            copa_sp3_wind_timer = 74u;
                            vdp.wind_vfx = StvVdpState::WindVfxState{};
                            vdp.wind_vfx.active = true;
                        }
                        if (__builtin_expect((pc - 0x06007DAEu) <= (0x06008624u - 0x06007DAEu), 0)) {
                            if ((pc == 0x060085FEu || pc == 0x06008624u || pc == 0x06007DAEu) && copa_opp_hook != nullptr) {
                                copa_opp_hook(*this);
                            }
                        }
                        if (__builtin_expect((pc & 0xFFFFFE00u) == 0x06052800u, 0)) {
                            if (pc == 0x060529A6u && copa_vs_hook != nullptr) {
                                copa_vs_hook(*this);
                                if (vdp.copa_bracket.progress_active != 0u) {
                                    break;
                                }
                            } else if (pc == 0x06052A94u &&
                                       inner.high_wram[0x0792A5u] == 5u &&
                                       inner.high_wram[0x097149u] == 1u &&
                                       cpus[0].r[4] >= 5u && cpus[0].r[4] <= 8u) {
                                cpus[0].r[4] += 1u;
                            }
                        }
                    }

                    if (idle_skip_enabled && !any_pending_check && !smpc_reset_master_pending &&
                        cpus[0].cycles < next_evt_cyc && slot + 8 <= batch_steps) {
                        MasterVblLoopInfo m_info{};
                        if (match_master_vbl_loop_at(cpus[0].pc, cpus[0], inner, copa_mode_active, m_info)) {
                            const uint32_t max_k_slot = static_cast<uint32_t>((batch_steps - slot) / 8);
                            const uint64_t max_k_cyc = (next_evt_cyc - cpus[0].cycles) / 12ULL;
                            const uint32_t k = (max_k_cyc < max_k_slot) ? static_cast<uint32_t>(max_k_cyc) : max_k_slot;
                            if (k > 0u) {
                                const uint32_t steps = k * 8u;
                                cpus[0].r[0] = m_info.flag_sext;
                                cpus[0].pr = cpus[0].pc + 6u;
                                cpus[0].sr |= kShSrT;
                                cpus[0].cur_pc = cpus[0].pc + 10u;
                                cpus[0].retired += static_cast<uint64_t>(steps);
                                cpus[0].cycles += static_cast<uint64_t>(k) * 12ULL;
                                main_steps += steps;
                                slot += static_cast<int>(steps);
                                continue;
                            }
                        }
                    }
                } else if (idle_skip_enabled && slot + 3 <= batch_steps) {
                    Poll3LoopInfo s_info{};
                    if (cpus[1].delay == 0u && match_poll3_loop_at(1, cpus[1].pc, cpus[1], inner, copa_mode_active, s_info)) {
                        const uint32_t k = static_cast<uint32_t>((batch_steps - slot) / 3);
                        const uint32_t steps = k * 3u;
                        if (s_info.is_ftcsr) {
                            const uint64_t last_read_cyc = cpus[1].cycles + static_cast<uint64_t>(k - 1u) * 5ULL;
                            inner.frt[1].maybe_fire(last_read_cyc);
                            cpus[1].r[0] = sign_extend_8(inner.frt[1].ftcsr & 0xFFu);
                        } else {
                            uint32_t v = 0u;
                            const uint16_t op0 = fast_be16(inner.high_wram.data() + ((cpus[1].pc & 0x07FFFFFFu) - kHighWramBase));
                            read_wram_ea(inner, cpus[1].r[(op0 >> 4) & 0xFu], 1u, v);
                            cpus[1].r[0] = sign_extend_8(v);
                        }
                        set_sr_flag(cpus[1], kShSrT, s_info.cond);
                        cpus[1].cur_pc = cpus[1].pc + 4u;
                        cpus[1].retired += static_cast<uint64_t>(steps);
                        cpus[1].cycles += static_cast<uint64_t>(k) * 5ULL;
                        slave_steps += steps;
                        slot += static_cast<int>(steps);
                        continue;
                    }
                }

                if (__builtin_expect(sh2_aot_enabled && cpus[cpu_id].delay == 0u &&
                                     cpus[cpu_id].pc >= kAotCodeLo && cpus[cpu_id].pc < kAotCodeHi &&
                                     (cpu_id != 0 || (!master_events_dirty && !any_pending_check &&
                                                      !smpc_reset_master_pending && cpus[0].cycles < next_evt_cyc)),
                                     1)) {
                    const uint32_t aot_ln = (cpus[cpu_id].pc - kHighWramBase) >> kAotLineShift;
                    const uint32_t aot_wi = aot_ln >> 5;
                    const uint32_t aot_bit = 1u << (aot_ln & 31u);
                    bool aot_ok = (aot_page_valid_bits[aot_wi] & aot_bit) != 0u;
                    if (__builtin_expect(!aot_ok, 0)) {
                        if (!aot_pages_initialized || (code_dirty_bits[aot_wi] & aot_bit) != 0u) {
                            aot_ok = revalidate_aot_page(aot_ln);
                        }
                    }
                    if (__builtin_expect(aot_ok, 1)) {
                        const uint32_t aot_steps = execute_sh2_aot_quantum(
                            cpu_id, cpus[cpu_id], *this, batch_steps - slot,
                            (cpu_id == 0) ? next_evt_cyc : 0xFFFFFFFFFFFFFFFFULL, fault);
                        if (aot_steps > 0u) {
                            if (cpu_id == 0) main_steps += aot_steps;
                            else slave_steps += aot_steps;
                            slot += static_cast<int>(aot_steps);
                        }
                        if (__builtin_expect(!fault.ok, 0)) {
                            halted = true;
                            halt_cpu = cpu_id;
                            last_fault = fault;
                            emit_audio_ring_ = prev_emit;
                            return false;
                        }
                        if (__builtin_expect(cpu_id == 0 && smpc_reset_master_pending, 0)) {
                            smpc_reset_master_pending = false;
                            cpus[0].pc = rd_be(bios_rom, 0u, 4u);
                            cpus[0].r[15] = rd_be(bios_rom, 4u, 4u);
                            cpus[0].vbr = 0u;
                            cpus[0].sr = 0xF0u;
                            cpus[0].delay = 0u;
                        }
                        if (aot_steps > 0u) {
                            continue;
                        }
                    }
                }

#ifdef TWC98_AOT_PROFILER
                twc98_aot_prof_step(cpu_id, cpus[cpu_id].pc, cpus[cpu_id].cur_pc);
#endif
                if (__builtin_expect(!step_sh2_with_bus(cpu_id, cpus[cpu_id], *this, decode_lut, fault), 0)) {
                    halted = true;
                    halt_cpu = cpu_id;
                    last_fault = fault;
                    emit_audio_ring_ = prev_emit;
                    return false;
                }
                if (__builtin_expect(cpu_id == 0 && smpc_reset_master_pending, 0)) {
                    smpc_reset_master_pending = false;
                    cpus[0].pc = rd_be(bios_rom, 0u, 4u);
                    cpus[0].r[15] = rd_be(bios_rom, 4u, 4u);
                    cpus[0].vbr = 0u;
                    cpus[0].sr = 0xF0u;
                    cpus[0].delay = 0u;
                }
                if (cpu_id == 0) ++main_steps;
                else ++slave_steps;
                ++slot;
            }
        }
        if (real_audio_enabled && target_samples > 0u) {
            sample_acc += batch_rounds * target_samples;
            if (sample_acc >= num_rounds) {
                const uint32_t emit = sample_acc / num_rounds;
                sample_acc %= num_rounds;
                step_audio_samples(emit);
            }
        }
        rounds_done += batch_rounds;
        r += batch_rounds;
        if (__builtin_expect(copa_mode_active && vdp.copa_bracket.progress_active != 0u, 0)) {
            break;
        }
    }
    emit_audio_ring_ = prev_emit;
    if (coop.enabled && vbi_count != start_vbi_count) {
        coop::on_frame_tick(*this);
    }
    return true;
}

void StvMachineState::advance_frame_state_no_render() {
    apply_gk_ai_fix();
    if (copa_mode_active && copa_sp3_wind_timer > 0u) {
        auto& wf = vdp.wind_vfx;
        wf.active = true;
        // Locate rendered soccer ball sprite cluster in VDP1 command observations:
        // In twcup98, the 3D soccer ball is rendered from 8x8 quads at srca=0x30680.
        int best_idx = -1;
        int target_x = (wf.frame_age > 0u) ? static_cast<int>(wf.cx) : (vdp.frame_w / 2);
        int target_y = (wf.frame_age > 0u) ? static_cast<int>(wf.cy) : (vdp.frame_h / 2);
        auto score_cand = [&](const StvVdpState::SpriteObs& o, bool require_ball_srca) -> int {
            if (require_ball_srca) {
                if (o.srca != 0x30680u) return -1000000;
            } else {
                if (o.tex_w != 8u || o.tex_h != 8u || o.srca >= 0x40000u) return -1000000;
            }
            const int dx = o.cx - target_x;
            const int dy = o.cy - target_y;
            return 100000 - (dx * dx + dy * dy);
        };
        int best_score = -1000000;
        for (int pass = 0; pass < 2 && best_idx < 0; ++pass) {
            const bool req_ball = (pass == 0);
            if (!req_ball && copa_ball_srca == 0x30680u) {
                // Already locked onto the true 0x30680 ball quads; don't jump to unrelated 8x8 UI/player sprites.
                break;
            }
            for (int i = 0; i < vdp.v1_obs_disp_count; ++i) {
                const int sc = score_cand(vdp.v1_obs_disp[i], req_ball);
                if (sc > best_score) {
                    best_score = sc;
                    best_idx = i;
                }
            }
        }
        if (best_idx >= 0) {
            const auto& seed = vdp.v1_obs_disp[best_idx];
            copa_ball_srca = seed.srca;
            float sum_x = 0.0f, sum_y = 0.0f;
            int cnt = 0;
            int max_draw = std::max(seed.draw_w, seed.draw_h);
            for (int i = 0; i < vdp.v1_obs_disp_count; ++i) {
                const auto& o = vdp.v1_obs_disp[i];
                if (o.srca == seed.srca &&
                    std::abs(o.cx - seed.cx) <= 24 &&
                    std::abs(o.cy - seed.cy) <= 24) {
                    sum_x += static_cast<float>(o.cx);
                    sum_y += static_cast<float>(o.cy);
                    max_draw = std::max(max_draw, static_cast<int>(std::max(o.draw_w, o.draw_h)));
                    ++cnt;
                }
            }
            wf.cx = (cnt > 0) ? (sum_x / static_cast<float>(cnt)) : static_cast<float>(seed.cx);
            wf.cy = (cnt > 0) ? (sum_y / static_cast<float>(cnt)) : static_cast<float>(seed.cy);
            wf.radius = std::clamp(static_cast<float>(max_draw) * 2.2f, 18.0f, 30.0f);
        } else if (wf.frame_age == 0u) {
            wf.cx = static_cast<float>(vdp.frame_w) * 0.5f;
            wf.cy = static_cast<float>(vdp.frame_h) * 0.48f;
            wf.radius = 16.0f;
        }
        // Shift trail buffer and record latest ball point at index 0
        for (int t = StvVdpState::WindVfxState::kTrailLen - 1; t > 0; --t) {
            wf.trail_x[t] = wf.trail_x[t - 1];
            wf.trail_y[t] = wf.trail_y[t - 1];
        }
        wf.trail_x[0] = wf.cx;
        wf.trail_y[0] = wf.cy;
        if (wf.trail_count < StvVdpState::WindVfxState::kTrailLen) ++wf.trail_count;
        ++wf.frame_age;
        --copa_sp3_wind_timer;
    } else {
        vdp.wind_vfx.active = false;
    }
    const uint16_t tvmd = static_cast<uint16_t>((static_cast<uint16_t>(vdp.vdp2_regs[0]) << 8) | vdp.vdp2_regs[1]);
    static constexpr int kWidths[8] = {320, 352, 640, 704, 320, 352, 640, 704};
    vdp.frame_w = kWidths[tvmd & 7u];
    vdp.frame_h = vdp.vdp2_lines();
}

namespace {

inline uint16_t rd_be16_u(const uint8_t* p, uint32_t off) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[off]) << 8) | p[off + 1u]);
}

inline int16_t rd_be16_s(const uint8_t* p, uint32_t off) {
    return static_cast<int16_t>(rd_be16_u(p, off));
}

inline int32_t rd_be32_s(const uint8_t* p, uint32_t off) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[off]) << 24) |
                                (static_cast<uint32_t>(p[off + 1u]) << 16) |
                                (static_cast<uint32_t>(p[off + 2u]) << 8) |
                                static_cast<uint32_t>(p[off + 3u]));
}

inline void fb_plot(uint32_t* px, int w, int h, int x, int y, uint32_t rgb) {
    if (x >= 0 && x < w && y >= 0 && y < h) {
        px[y * w + x] = rgb;
    }
}

void fb_fill_rect(uint32_t* px, int w, int h, int x0, int y0, int rw, int rh, uint32_t rgb) {
    const int x1 = std::min(w, x0 + rw);
    const int y1 = std::min(h, y0 + rh);
    const int sx = std::max(0, x0);
    const int sy = std::max(0, y0);
    for (int y = sy; y < y1; ++y) {
        uint32_t* row = px + y * w;
        for (int x = sx; x < x1; ++x) row[x] = rgb;
    }
}

void fb_draw_glyph_4x5(uint32_t* px, int w, int h, int x, int y, int sc, uint8_t ch, uint32_t rgb) {
    // Crisp 4x5 pixel glyphs: '1', '2', 'P'
    static constexpr uint8_t kGlyph1[5] = {0b0100, 0b1100, 0b0100, 0b0100, 0b1110};
    static constexpr uint8_t kGlyph2[5] = {0b1110, 0b0010, 0b0100, 0b1000, 0b1110};
    static constexpr uint8_t kGlyphP[5] = {0b1110, 0b1001, 0b1110, 0b1000, 0b1000};
    const uint8_t* g = (ch == '1') ? kGlyph1 : (ch == '2') ? kGlyph2 : kGlyphP;
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (g[r] & (1u << (3 - c))) {
                fb_fill_rect(px, w, h, x + c * sc, y + r * sc, sc, sc, rgb);
            }
        }
    }
}

void fb_draw_badge(uint32_t* px, int w, int h, int cx, int cy, int sc, int slot, uint32_t fill, uint32_t border) {
    const int bw = 13 * sc;
    const int bh = 9 * sc;
    const int x0 = cx - bw / 2;
    const int y0 = cy - bh / 2;
    fb_fill_rect(px, w, h, x0 - sc, y0 - sc, bw + 2 * sc, bh + 2 * sc, border);
    fb_fill_rect(px, w, h, x0, y0, bw, bh, fill);
    const uint8_t dchar = (slot == 0) ? '1' : '2';
    // Dark shadow + crisp white '1P' / '2P'
    fb_draw_glyph_4x5(px, w, h, x0 + 2 * sc, y0 + 2 * sc + 1, sc, dchar, border);
    fb_draw_glyph_4x5(px, w, h, x0 + 7 * sc, y0 + 2 * sc + 1, sc, 'P', border);
    fb_draw_glyph_4x5(px, w, h, x0 + 2 * sc, y0 + 2 * sc, sc, dchar, 0x00FFFFFFu);
    fb_draw_glyph_4x5(px, w, h, x0 + 7 * sc, y0 + 2 * sc, sc, 'P', 0x00FFFFFFu);
}

void fb_draw_arrow_tri(uint32_t* px, int w, int h, float tip_x, float tip_y,
                       float dir_x, float dir_y, int sc, uint32_t fill, uint32_t border) {
    const float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
    if (len < 1e-4f) return;
    const float ux = dir_x / len;
    const float uy = dir_y / len;
    const float pxn = -uy;
    const float pyn = ux;
    const float depth = 6.0f * static_cast<float>(sc);
    const float half_w = 5.0f * static_cast<float>(sc);
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({tip_x, tip_x - ux * depth + pxn * half_w, tip_x - ux * depth - pxn * half_w}))) - sc);
    const int max_x = std::min(w - 1, static_cast<int>(std::ceil(std::max({tip_x, tip_x - ux * depth + pxn * half_w, tip_x - ux * depth - pxn * half_w}))) + sc);
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({tip_y, tip_y - uy * depth + pyn * half_w, tip_y - uy * depth - pyn * half_w}))) - sc);
    const int max_y = std::min(h - 1, static_cast<int>(std::ceil(std::max({tip_y, tip_y - uy * depth + pyn * half_w, tip_y - uy * depth - pyn * half_w}))) + sc);
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float dx = static_cast<float>(x) - tip_x;
            const float dy = static_cast<float>(y) - tip_y;
            const float along = -(dx * ux + dy * uy);
            const float perp = std::fabs(dx * pxn + dy * pyn);
            if (along >= 0.0f && along <= depth && perp <= (along / depth) * half_w) {
                fb_plot(px, w, h, x, y, fill);
            } else if (along >= -static_cast<float>(sc) && along <= depth + static_cast<float>(sc) &&
                       perp <= (along / depth) * half_w + static_cast<float>(sc)) {
                fb_plot(px, w, h, x, y, border);
            }
        }
    }
}

void compute_offscreen_player_indicators(const uint8_t* hw, const uint8_t* vdp1,
                                         int frame_w, int frame_h,
                                         const CoopState& coop,
                                         StvVdpState::OffscreenBadge out_badges[2]) {
    out_badges[0] = StvVdpState::OffscreenBadge{};
    out_badges[1] = StvVdpState::OffscreenBadge{};
    if (!hw || !vdp1 || frame_w < 320 || frame_h < 200) return;

    // Guard 1: Verify live match state (all 22 players + ball active with flag 0x8000).
    for (int i = 0; i < 23; ++i) {
        const uint16_t fl = rd_be16_u(hw, 0x76F48u + static_cast<uint32_t>(i) * 0xB0u);
        if ((fl & 0x8000u) == 0u) return;
    }

    // Guard 2: Scan VDP1 command table for active scene count & native 1P/2P markers.
    int cmd_count = 0;
    int lx = 0, ly = 0;
    bool native_vis[2] = {false, false};
    uint32_t off = 0u;
    for (int i = 0; i < 700 && off + 32u <= StvVdpState::kVdp1VramSize; ++i) {
        const uint16_t ctrl = rd_be16_u(vdp1, off);
        if (ctrl & 0x8000u) break;
        ++cmd_count;
        const uint16_t cmd = ctrl & 0x000Fu;
        if (cmd == 9u || cmd == 10u) {
            lx = rd_be16_s(vdp1, off + 0x0Cu);
            ly = rd_be16_s(vdp1, off + 0x0Eu);
        } else if (cmd <= 3u) {
            const uint32_t srca = static_cast<uint32_t>(rd_be16_u(vdp1, off + 0x08u)) * 8u;
            if (srca == 0x34FE0u || srca == 0x35060u) {
                const int slot = (srca == 0x34FE0u) ? 0 : 1;
                const int sx = lx + rd_be16_s(vdp1, off + 0x0Cu);
                const int sy = ly + rd_be16_s(vdp1, off + 0x0Eu);
                if (sx >= 12 && sx <= 692 && sy >= 4 && sy <= 436) {
                    native_vis[slot] = true;
                }
            }
        }
        const uint16_t jp = (ctrl >> 12) & 3u;
        off = (jp == 1u) ? (static_cast<uint32_t>(rd_be16_u(vdp1, off + 2u)) * 8u) : (off + 32u);
    }
    if (cmd_count < 40) return;

    const float cx = static_cast<float>(rd_be32_s(hw, 0x76530u)) / 65536.0f;
    const float cz = static_cast<float>(rd_be32_s(hw, 0x76534u)) / 65536.0f;
    const float cy = static_cast<float>(rd_be32_s(hw, 0x76538u)) / 65536.0f;
    const float cd = static_cast<float>(rd_be32_s(hw, 0x7653Cu)) / 65536.0f;
    if (cd <= 40.0f || cd >= 3000.0f) return;

    constexpr float kTwoPi = 6.28318530718f;
    const float pitch_rad = static_cast<float>(rd_be16_s(hw, 0x76540u)) * (kTwoPi / 65536.0f);
    const float roll_rad = static_cast<float>(rd_be16_s(hw, 0x76544u)) * (kTwoPi / 65536.0f);
    const float sp = std::sin(pitch_rad), cp = std::cos(pitch_rad);
    const float sr = std::sin(roll_rad), cr = std::cos(roll_rad);
    float fov = static_cast<float>(rd_be32_s(hw, 0xFFC68u)) / 65536.0f;
    if (fov < 200.0f || fov > 3000.0f) fov = 1117.0f;

    const float bx = static_cast<float>(rd_be32_s(hw, 0x77E68u + 20u)) / 65536.0f;
    const float bz = static_cast<float>(rd_be32_s(hw, 0x77E68u + 24u)) / 65536.0f;

    const int sc = (frame_w >= 640 || frame_h >= 400) ? 2 : 1;
    const float sx_scale = static_cast<float>(frame_w) / 704.0f;
    const float sy_scale = static_cast<float>(frame_h) / 448.0f;
    const float min_x = 18.0f * static_cast<float>(sc);
    const float max_x = static_cast<float>(frame_w) - 18.0f * static_cast<float>(sc);
    const float min_y = 38.0f * static_cast<float>(sc);
    const float max_y = static_cast<float>(frame_h) - 14.0f * static_cast<float>(sc);

    const bool coop_live = coop.enabled && hw[0x7657Fu] == 0x00u && hw[0x76580u] == 0xFFu;
    for (int slot = 0; slot < 2; ++slot) {
        int target_idx = -1;
        if (coop_live) {
            const uint8_t coop_idx = (slot == 0) ? coop.p1_actor : coop.p2_actor;
            if (coop_idx > 10u) continue;
            // Show badge when native 1P/2P VDP1 marker is hidden/off-screen OR pulsing "Call Pass" (Btn D)
            const bool call_pulse = (coop.call_pass_timer[slot] > 0u) && (((coop.call_pass_timer[slot] >> 2) & 1u) != 0u);
            if (native_vis[slot] && !call_pulse) continue;
            target_idx = static_cast<int>(coop_idx);
        } else {
            const uint8_t team_byte = hw[0x7657Fu + static_cast<uint32_t>(slot)];
            if (team_byte == 0xFFu) continue;
            if (native_vis[slot]) continue;

            const uint8_t game_idx = hw[0x76584u + static_cast<uint32_t>(slot)];
            if (game_idx < 22u) {
                target_idx = static_cast<int>(game_idx);
            } else {
                const int team = (team_byte & 1u) ? 1 : 0;
                const int start = team ? 12 : 1;
                const int end = start + 10;
                float best_on_d2 = 1e30f, best_any_d2 = 1e30f;
                int best_on = -1, best_any = -1;
                for (int i = start; i < end; ++i) {
                    const uint32_t p_off = 0x76F48u + static_cast<uint32_t>(i) * 0xB0u;
                    const uint16_t fl = rd_be16_u(hw, p_off);
                    if ((fl & 0x8000u) == 0u) continue;
                    const float px = static_cast<float>(rd_be32_s(hw, p_off + 20u)) / 65536.0f;
                    const float pz = static_cast<float>(rd_be32_s(hw, p_off + 24u)) / 65536.0f;
                    const float ddx = px - bx, ddz = pz - bz;
                    const float d2 = ddx * ddx + ddz * ddz;
                    if ((fl & 0x2000u) == 0u && d2 < best_on_d2) {
                        best_on_d2 = d2;
                        best_on = i;
                    }
                    if (d2 < best_any_d2) {
                        best_any_d2 = d2;
                        best_any = i;
                    }
                }
                target_idx = (best_on >= 0) ? best_on : ((best_any >= 0) ? best_any : (team ? 11 : 0));
            }
        }
        if (target_idx < 0 || target_idx >= 22) continue;

        const uint32_t t_off = 0x76F48u + static_cast<uint32_t>(target_idx) * 0xB0u;
        const float px = static_cast<float>(rd_be32_s(hw, t_off + 20u)) / 65536.0f;
        const float pz = static_cast<float>(rd_be32_s(hw, t_off + 24u)) / 65536.0f;
        const float py = static_cast<float>(rd_be32_s(hw, t_off + 28u)) / 65536.0f;

        const float dx = px - cx;
        const float dz = pz - cz;
        const float dy = (py - 52.0f) - cy;
        const float rx = cr * dx - sr * dz;
        const float rz = sr * dx + cr * dz;
        const float vy = cp * rz - sp * dy;
        const float vz = sp * rz + cp * dy + cd;

        float sx_704 = 352.0f;
        float sy_448 = 224.0f;
        if (vz >= 20.0f) {
            sx_704 += fov * (rx / vz);
            sy_448 += (fov * 0.75f) * (vy / vz);
        } else {
            sx_704 += (rx >= 0.0f ? 1.0f : -1.0f) * std::min(900.0f, std::fabs(rx) * 4.0f);
            sy_448 = 748.0f;
        }

        const float fx = sx_704 * sx_scale;
        const float fy = sy_448 * sy_scale;
        auto& bd = out_badges[slot];
        bd.active = 1u;
        bd.slot = static_cast<uint8_t>(slot);
        bd.sc = static_cast<uint8_t>(sc);

        if (fx >= min_x && fx <= max_x && fy >= min_y && fy <= max_y) {
            const int bx_c = static_cast<int>(std::lround(fx));
            const int by_c = static_cast<int>(std::lround(fy - 10.0f * static_cast<float>(sc)));
            bd.is_edge = 0u;
            bd.bx = static_cast<int16_t>(bx_c);
            bd.by = static_cast<int16_t>(by_c);
            bd.tip_x = static_cast<float>(bx_c);
            bd.tip_y = static_cast<float>(by_c + 8 * sc);
            bd.dir_x = 0.0f;
            bd.dir_y = 1.0f;
        } else {
            const float mid_x = static_cast<float>(frame_w) * 0.5f;
            const float mid_y = static_cast<float>(frame_h) * 0.52f;
            const float vx = fx - mid_x;
            const float vy_fb = fy - mid_y;
            float t_hit = 1.0f;
            if (vx > 1e-3f) t_hit = std::min(t_hit, (max_x - mid_x) / vx);
            else if (vx < -1e-3f) t_hit = std::min(t_hit, (min_x - mid_x) / vx);
            if (vy_fb > 1e-3f) t_hit = std::min(t_hit, (max_y - mid_y) / vy_fb);
            else if (vy_fb < -1e-3f) t_hit = std::min(t_hit, (min_y - mid_y) / vy_fb);
            const float edge_x = mid_x + vx * t_hit;
            const float edge_y = mid_y + vy_fb * t_hit;
            const float vlen = std::sqrt(vx * vx + vy_fb * vy_fb);
            const float ux = (vlen > 1e-4f) ? (vx / vlen) : 0.0f;
            const float uy = (vlen > 1e-4f) ? (vy_fb / vlen) : 1.0f;
            bd.is_edge = 1u;
            bd.tip_x = edge_x + ux * (6.0f * static_cast<float>(sc));
            bd.tip_y = edge_y + uy * (6.0f * static_cast<float>(sc));
            bd.dir_x = ux;
            bd.dir_y = uy;
            bd.bx = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(edge_x - ux * (8.0f * static_cast<float>(sc)))),
                                                    static_cast<int>(min_x), static_cast<int>(max_x)));
            bd.by = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(edge_y - uy * (8.0f * static_cast<float>(sc)))),
                                                    static_cast<int>(min_y), static_cast<int>(max_y)));
        }
    }
}

void render_offscreen_player_indicators(uint32_t* frame_px, int frame_w, int frame_h,
                                        const StvVdpState::OffscreenBadge badges[2]) {
    if (!frame_px || frame_w < 320 || frame_h < 200) return;
    constexpr uint32_t kBorder = 0x0010141Cu;
    for (int i = 0; i < 2; ++i) {
        const auto& bd = badges[i];
        if (!bd.active) continue;
        const int slot = static_cast<int>(bd.slot);
        const int sc = static_cast<int>(bd.sc);
        const uint32_t fill = (slot == 0) ? 0x00E62424u : 0x001E7BFFu;
        if (bd.is_edge) {
            fb_draw_arrow_tri(frame_px, frame_w, frame_h, bd.tip_x, bd.tip_y, bd.dir_x, bd.dir_y, sc, fill, kBorder);
            fb_draw_badge(frame_px, frame_w, frame_h, bd.bx, bd.by, sc, slot, fill, kBorder);
        } else {
            fb_draw_badge(frame_px, frame_w, frame_h, bd.bx, bd.by, sc, slot, fill, kBorder);
            fb_draw_arrow_tri(frame_px, frame_w, frame_h, bd.tip_x, bd.tip_y, bd.dir_x, bd.dir_y, sc, fill, kBorder);
        }
    }
}

}  // namespace

bool StvMachineState::step_frame_no_render(uint32_t num_rounds, bool emit_audio_ring) {
    if (!run_rounds_ex(num_rounds, emit_audio_ring)) return false;
    advance_frame_state_no_render();
    return true;
}

void StvMachineState::prepare_render_frame() {
    advance_frame_state_no_render();
    vdp.compose_frame_prepare(inner.vdp2_vram.data(), dbg_hide_layers);
    if (offscreen_p1_indicator || coop.enabled) {
        compute_offscreen_player_indicators(
            inner.high_wram.data(), vdp.vdp1_vram, vdp.frame_w, vdp.frame_h, coop, vdp.offscreen_badges);
    } else {
        vdp.offscreen_badges[0] = StvVdpState::OffscreenBadge{};
        vdp.offscreen_badges[1] = StvVdpState::OffscreenBadge{};
    }
}

void StvMachineState::finish_render_frame() {
    if (vdp.compose_prep.disp_on) {
        vdp.compose_frame_post();
    }
    if (offscreen_p1_indicator || coop.enabled) {
        render_offscreen_player_indicators(vdp.frame_px, vdp.frame_w, vdp.frame_h, vdp.offscreen_badges);
    }
}

void StvMachineState::render_frame() {
    prepare_render_frame();
    if (vdp.compose_prep.disp_on) {
        vdp.compose_frame_band(inner.vdp2_vram.data(), 0, vdp.frame_h);
    }
    finish_render_frame();
}

uint32_t StvMachineState::pack_render_state(uint8_t* dst, size_t dst_capacity) const {
    if (!dst) return 0u;
    const uint16_t tvmd = static_cast<uint16_t>((static_cast<uint16_t>(vdp.vdp2_regs[0]) << 8) | vdp.vdp2_regs[1]);
    static constexpr int kWidths[8] = {320, 352, 640, 704, 320, 352, 640, 704};
    const int raw_w = kWidths[tvmd & 7u];
    const int raw_h = vdp.vdp2_lines();
    const bool lsmd3 = ((tvmd >> 6) & 3u) == 3u;
    const bool hires_x = (tvmd & 2u) != 0u;
    const bool half_y = vdp.low_res_224p && lsmd3 && (raw_h >= 448);
    const int x_shift = (vdp.low_res_half_width && half_y && hires_x) ? 1 : 0;
    const int out_w = raw_w >> x_shift;
    const int out_h = half_y ? (raw_h / 2) : raw_h;

    const int fb_pitch = (vdp.vdp1_regs[StvVdpState::TVMR] & 1u) ? 1024 : 512;
    const uint32_t fb_rows = static_cast<uint32_t>(std::clamp(raw_h, 1, 512));
    const uint32_t fb_bytes = static_cast<uint32_t>(fb_pitch) * fb_rows * 2u;
    const bool has_ind = offscreen_p1_indicator || coop.enabled;
    const bool has_copa_vfx = vdp.copa_vfx_replace;
    const bool has_bracket = vdp.copa_bracket.progress_active != 0u;
    const size_t wind_bytes = sizeof(StvVdpState::WindVfxState);
    const size_t badges_bytes = sizeof(StvVdpState::OffscreenBadge) * 2u;
    const size_t needed = 12u + sizeof(vdp.vdp1_regs) + StvVdpState::kVdp2CramSize + StvVdpState::kVdp2RegsSize +
                          wind_bytes +
                          (has_copa_vfx ? sizeof(vdp.copa_sp_vfx) : 0u) +
                          (has_bracket ? sizeof(vdp.copa_bracket) : 0u) +
                          (has_ind ? badges_bytes : 0u) +
                          kVdp2VramSize + fb_bytes;
    if (dst_capacity < needed) return 0u;

    StvVdpState::OffscreenBadge badges[2]{};
    if (has_ind) {
        compute_offscreen_player_indicators(inner.high_wram.data(), vdp.vdp1_vram, out_w, out_h, coop, badges);
    }

    size_t pos = 0u;
    auto push_u32 = [&](uint32_t v) {
        dst[pos + 0u] = static_cast<uint8_t>(v & 0xFFu);
        dst[pos + 1u] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        dst[pos + 2u] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        dst[pos + 3u] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        pos += 4u;
    };
    auto push_bytes = [&](const void* p, size_t n) {
        std::memcpy(dst + pos, p, n);
        pos += n;
    };
    const uint32_t flags = (has_ind ? 1u : 0u) |
                           (vdp.low_res_224p ? 2u : 0u) |
                           (vdp.low_res_half_width ? 4u : 0u) |
                           (vdp.copa_group_badges ? 8u : 0u) |
                           (has_copa_vfx ? 16u : 0u) |
                           (has_bracket ? 32u : 0u) |
                           ((static_cast<uint32_t>(dbg_hide_layers) & 0xFFu) << 8);
    push_u32(0x524E4455u);  // "URND" v2 slim render packet (no 512 KiB vdp1_vram copy)
    push_u32(flags);
    push_u32(fb_bytes);
    push_bytes(vdp.vdp1_regs, sizeof(vdp.vdp1_regs));
    push_bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    push_bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    push_bytes(&vdp.wind_vfx, wind_bytes);
    if (has_copa_vfx) {
        push_bytes(&vdp.copa_sp_vfx, sizeof(vdp.copa_sp_vfx));
    }
    if (has_bracket) {
        push_bytes(&vdp.copa_bracket, sizeof(vdp.copa_bracket));
    }
    if (has_ind) {
        push_bytes(badges, badges_bytes);
    }
    push_bytes(inner.vdp2_vram.data(), kVdp2VramSize);
    const_cast<StvVdpState&>(vdp).materialize_cpu_display_fb();
    push_bytes(vdp.vdp1_display(), fb_bytes);
    return static_cast<uint32_t>(pos);
}

bool StvMachineState::unpack_render_state_only(const uint8_t* src, size_t len) {
    if (!src || len < 12u) return false;
    size_t pos = 0u;
    auto pop_u32 = [&]() -> uint32_t {
        const uint32_t v = static_cast<uint32_t>(src[pos]) |
                           (static_cast<uint32_t>(src[pos + 1u]) << 8) |
                           (static_cast<uint32_t>(src[pos + 2u]) << 16) |
                           (static_cast<uint32_t>(src[pos + 3u]) << 24);
        pos += 4u;
        return v;
    };
    const uint32_t magic = pop_u32();
    if (magic != 0x524E4455u) return false;
    const uint32_t flags = pop_u32();
    const uint32_t fb_bytes = pop_u32();
    const bool has_ind = (flags & 1u) != 0u;
    const bool has_copa_vfx = (flags & 16u) != 0u;
    const bool has_bracket = (flags & 32u) != 0u;
    const size_t wind_bytes = sizeof(StvVdpState::WindVfxState);
    const size_t badges_bytes = sizeof(StvVdpState::OffscreenBadge) * 2u;
    if (fb_bytes > StvVdpState::kVdp1FbPlaneWords * 2u) return false;
    const size_t needed = 12u + sizeof(vdp.vdp1_regs) + StvVdpState::kVdp2CramSize + StvVdpState::kVdp2RegsSize +
                          wind_bytes +
                          (has_copa_vfx ? sizeof(vdp.copa_sp_vfx) : 0u) +
                          (has_bracket ? sizeof(vdp.copa_bracket) : 0u) +
                          (has_ind ? badges_bytes : 0u) +
                          kVdp2VramSize + fb_bytes;
    if (len < needed) return false;

    auto pop_bytes = [&](void* p, size_t n) {
        std::memcpy(p, src + pos, n);
        pos += n;
    };
    pop_bytes(vdp.vdp1_regs, sizeof(vdp.vdp1_regs));
    pop_bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    pop_bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    pop_bytes(&vdp.wind_vfx, wind_bytes);
    vdp.copa_vfx_replace = has_copa_vfx;
    if (has_copa_vfx) {
        pop_bytes(&vdp.copa_sp_vfx, sizeof(vdp.copa_sp_vfx));
    } else {
        vdp.copa_sp_vfx = StvVdpState::CopaSpVfxState{};
    }
    if (has_bracket) {
        pop_bytes(&vdp.copa_bracket, sizeof(vdp.copa_bracket));
    } else {
        vdp.copa_bracket.progress_active = 0u;
    }
    if (has_ind) {
        pop_bytes(vdp.offscreen_badges, badges_bytes);
    } else {
        vdp.offscreen_badges[0] = StvVdpState::OffscreenBadge{};
        vdp.offscreen_badges[1] = StvVdpState::OffscreenBadge{};
    }
    pop_bytes(inner.vdp2_vram.data(), kVdp2VramSize);
    pop_bytes(vdp.vdp1_fb[0], fb_bytes);
    vdp.draw_fb = 1;  // vdp1_display() reads vdp1_fb[draw_fb ^ 1] = vdp1_fb[0]
    vdp.invalidate_gpu_vdp1_frames();
    offscreen_p1_indicator = has_ind;
    vdp.low_res_224p = (flags & 2u) != 0u;
    vdp.low_res_half_width = (flags & 4u) != 0u;
    vdp.copa_group_badges = (flags & 8u) != 0u;
    dbg_hide_layers = static_cast<uint8_t>((flags >> 8) & 0xFFu);

    vdp.compose_frame_prepare(inner.vdp2_vram.data(), dbg_hide_layers);
    return true;
}

bool StvMachineState::unpack_render_state_and_render(const uint8_t* src, size_t len) {
    if (!unpack_render_state_only(src, len)) return false;
    if (vdp.compose_prep.disp_on) {
        vdp.compose_frame_band(inner.vdp2_vram.data(), 0, vdp.frame_h);
        vdp.compose_frame_post();
    }
    if (offscreen_p1_indicator) {
        render_offscreen_player_indicators(vdp.frame_px, vdp.frame_w, vdp.frame_h, vdp.offscreen_badges);
    }
    return true;
}

uint32_t StvMachineState::pack_gpu_vdp2_frame(uint8_t* dst, size_t dst_capacity,
                                              uint8_t prev_vdp2_vram[0x80000], bool force_full_vram,
                                              uint8_t prev_vdp1_vram[0x80000], bool force_full_v1_vram) {
    if (!dst || dst_capacity < 1024u) return 0u;
    if (vdp.has_cpu_only_overlays()) {
        vdp.materialize_cpu_display_fb();
        return 0u;
    }

    prepare_render_frame();
    const auto& prep = vdp.compose_prep;
    if (prep.rbg_perdot_x && vdp.compose_rbg.crkte) {
        vdp.materialize_cpu_display_fb();
        return 0u;
    }
    const int fw = vdp.frame_w;
    const int fh = vdp.frame_h;
    const int fb_w_packed = std::min(prep.fb_pitch, std::max(1, fw << prep.x_shift));
    const int fb_rows = std::clamp(prep.raw_h, 1, 512);

    const auto& disp_gpu = vdp.vdp1_gpu_display();
    const bool use_gpu_vdp1 = vdp.gpu_vdp1_enabled && disp_gpu.valid;
    if (!use_gpu_vdp1) {
        vdp.materialize_cpu_display_fb();
    }

    // 32 chunks x 16 KiB = 512 KiB VDP2 VRAM dirty tracking
    uint32_t dirty_mask = 0u;
    uint32_t dirty_count = 0u;
    const uint8_t* const cur_v2 = inner.vdp2_vram.data();
    if (force_full_vram || !prev_vdp2_vram) {
        dirty_mask = 0xFFFFFFFFu;
        dirty_count = 32u;
        if (prev_vdp2_vram) std::memcpy(prev_vdp2_vram, cur_v2, kVdp2VramSize);
    } else {
        for (uint32_t c = 0u; c < 32u; ++c) {
            const uint32_t off = c * 16384u;
            if (std::memcmp(cur_v2 + off, prev_vdp2_vram + off, 16384u) != 0) {
                dirty_mask |= (1u << c);
                ++dirty_count;
                std::memcpy(prev_vdp2_vram + off, cur_v2 + off, 16384u);
            }
        }
    }

    // 32 chunks x 16 KiB = 512 KiB VDP1 VRAM dirty tracking (when use_gpu_vdp1 is active)
    uint32_t v1_dirty_mask = 0u;
    uint32_t v1_dirty_count = 0u;
    const uint8_t* const cur_v1 = vdp.vdp1_vram;
    if (use_gpu_vdp1) {
        if (force_full_v1_vram || !prev_vdp1_vram) {
            v1_dirty_mask = 0xFFFFFFFFu;
            v1_dirty_count = 32u;
            if (prev_vdp1_vram) std::memcpy(prev_vdp1_vram, cur_v1, StvVdpState::kVdp1VramSize);
        } else {
            for (uint32_t c = 0u; c < 32u; ++c) {
                const uint32_t off = c * 16384u;
                if (std::memcmp(cur_v1 + off, prev_vdp1_vram + off, 16384u) != 0) {
                    v1_dirty_mask |= (1u << c);
                    ++v1_dirty_count;
                    std::memcpy(prev_vdp1_vram + off, cur_v1 + off, 16384u);
                }
            }
        }
    }

    const uint32_t hdr_bytes = 256u * 4u;
    const uint32_t line_bytes = static_cast<uint32_t>(fh) * 8u * 4u * 4u; // 8 RGBA32I texels/row
    const uint32_t cram_bytes = 2048u * 4u;
    const uint32_t fb_bytes = use_gpu_vdp1
        ? 0u
        : (static_cast<uint32_t>(fb_w_packed) * static_cast<uint32_t>(fb_rows) * 2u);
    const uint32_t v2_bytes = dirty_count * 16384u;
    const uint32_t v1_cmds_bytes = use_gpu_vdp1
        ? (static_cast<uint32_t>(disp_gpu.count) * static_cast<uint32_t>(sizeof(StvVdpState::V1GpuCmd)))
        : 0u;
    const uint32_t v1_vram_bytes = use_gpu_vdp1 ? (v1_dirty_count * 16384u) : 0u;

    const uint32_t line_off = hdr_bytes;
    const uint32_t cram_off = line_off + line_bytes;
    const uint32_t fb_off = cram_off + cram_bytes;
    const uint32_t v2_off = (fb_off + fb_bytes + 3u) & ~3u;
    const uint32_t v1_cmds_off = (v2_off + v2_bytes + 3u) & ~3u;
    const uint32_t v1_vram_off = (v1_cmds_off + v1_cmds_bytes + 3u) & ~3u;
    const uint32_t total_bytes = v1_vram_off + v1_vram_bytes;
    if (dst_capacity < total_bytes) return 0u;

    int32_t* const hdr = reinterpret_cast<int32_t*>(dst);
    std::memset(hdr, 0, hdr_bytes);

    const uint32_t fb_h_max = static_cast<uint32_t>((vdp.vdp1_regs[StvVdpState::TVMR] & 1u) ? 512 : 256);
    const uint32_t flags0 =
        (prep.disp_on ? 1u : 0u) |
        (prep.lsmd3 ? 2u : 0u) |
        (prep.hires_x ? 4u : 0u) |
        (prep.hires_rbg ? 8u : 0u) |
        (prep.half_y ? 16u : 0u) |
        (prep.rbg_active ? 32u : 0u) |
        (prep.rbg_perdot_x ? 64u : 0u) |
        (prep.add ? 128u : 0u) |
        (prep.sp_cc_en ? 256u : 0u) |
        (prep.sp_mixed ? 512u : 0u) |
        (prep.sp_off ? 1024u : 0u) |
        (prep.sp_offb ? 2048u : 0u) |
        (prep.back_off ? 4096u : 0u) |
        (prep.back_offb ? 8192u : 0u) |
        (vdp.compose_rbg.crkte ? 16384u : 0u) |
        (vdp.compose_rbg.perdot ? 32768u : 0u) |
        (static_cast<uint32_t>(prep.dbg_hide_layers) << 16) |
        ((prep.half_y && fb_rows <= static_cast<int>(fb_h_max)) ? (1u << 24) : 0u) |
        (use_gpu_vdp1 ? (1u << 25) : 0u);

    hdr[0] = 0x47564432; // "GVD2"
    hdr[1] = fw;
    hdr[2] = fh;
    hdr[3] = prep.raw_w;
    hdr[4] = prep.raw_h;
    hdr[5] = prep.y_step;
    hdr[6] = prep.x_shift;
    hdr[7] = static_cast<int32_t>(flags0);
    hdr[8] = static_cast<int32_t>(prep.cram_mask);
    hdr[9] = fb_w_packed;
    hdr[10] = fb_rows;
    hdr[11] = static_cast<int32_t>(dirty_mask);
    hdr[12] = prep.sp_type;
    hdr[13] = prep.sp_ccmode;
    hdr[14] = prep.sp_ccnum;
    hdr[15] = static_cast<int32_t>(prep.sp_caos);
    for (int i = 0; i < 8; ++i) {
        hdr[16 + i] = static_cast<int32_t>(prep.sp_prio[i]);
        hdr[24 + i] = static_cast<int32_t>(prep.sp_ratio[i]);
    }
    for (int i = 0; i < 6; ++i) hdr[32 + i] = static_cast<int32_t>(prep.wctl[i]);
    for (int b = 0; b < 2; ++b) {
        for (int c = 0; c < 3; ++c) hdr[38 + b * 3 + c] = prep.off_vals[b][c];
    }
    for (int l = 0; l < 5; ++l) {
        hdr[44 + l] = prep.sf_prio_mode[l];
        hdr[49 + l] = prep.sf_cc_mode[l];
        hdr[54 + l] = static_cast<int32_t>(prep.sf_code_byte[l]);
    }
    hdr[59] = prep.active_nbg_count;
    for (int i = 0; i < 4; ++i) hdr[60 + i] = prep.active_nbgs[i];

    auto pack_nbg_bits = [](const StvVdpState::Nbg& s) -> int32_t {
        const uint32_t bits =
            (s.on ? 1u : 0u) |
            (s.opaque ? 2u : 0u) |
            (s.bitmap ? 4u : 0u) |
            (s.cell2x2 ? 8u : 0u) |
            (s.word1 ? 16u : 0u) |
            (s.cnsm ? 32u : 0u) |
            (s.cc ? 64u : 0u) |
            (s.offset ? 128u : 0u) |
            (s.offset_b ? 256u : 0u) |
            (s.bm_spr ? 512u : 0u) |
            (s.bm_scc ? 1024u : 0u) |
            (static_cast<uint32_t>(s.colors & 7) << 12) |
            (static_cast<uint32_t>(s.plane_w & 3) << 16) |
            (static_cast<uint32_t>(s.plane_h & 3) << 18);
        return static_cast<int32_t>(bits);
    };

    for (int n = 0; n < 4; ++n) {
        const auto& s = vdp.compose_nbg[n];
        const int b = 64 + n * 16;
        hdr[b + 0] = pack_nbg_bits(s);
        hdr[b + 1] = static_cast<int32_t>(s.supp);
        hdr[b + 2] = static_cast<int32_t>(s.page_bytes);
        hdr[b + 3] = static_cast<int32_t>(s.caos);
        for (int p = 0; p < 4; ++p) hdr[b + 4 + p] = static_cast<int32_t>(s.plane[p]);
        hdr[b + 8] = s.bm_w;
        hdr[b + 9] = s.bm_h;
        hdr[b + 10] = static_cast<int32_t>(s.bm_addr);
        hdr[b + 11] = static_cast<int32_t>(s.bm_pal);
        hdr[b + 12] = static_cast<int32_t>(s.prio);
        hdr[b + 13] = static_cast<int32_t>(s.ratio);
        hdr[b + 14] = s.dx;
        hdr[b + 15] = 0;
    }

    const auto& rbg = vdp.compose_rbg;
    hdr[128] = rbg.mode;
    hdr[129] = static_cast<int32_t>(rbg.prio);
    hdr[130] = static_cast<int32_t>(rbg.ratio);
    hdr[131] = static_cast<int32_t>(
        (rbg.cc ? 1u : 0u) |
        (rbg.offset ? 2u : 0u) |
        (rbg.offset_b ? 4u : 0u) |
        (rbg.bank_coef[0] ? 8u : 0u) |
        (rbg.bank_coef[1] ? 16u : 0u) |
        (rbg.bank_coef[2] ? 32u : 0u) |
        (rbg.bank_coef[3] ? 64u : 0u));

    for (int pi = 0; pi < 2; ++pi) {
        const auto& rp = rbg.par[pi];
        const int b = 132 + pi * 40;
        hdr[b + 0] = pack_nbg_bits(rp.chr);
        hdr[b + 1] = static_cast<int32_t>(rp.chr.supp);
        hdr[b + 2] = static_cast<int32_t>(rp.chr.page_bytes);
        hdr[b + 3] = static_cast<int32_t>(rp.chr.caos);
        hdr[b + 4] = rp.chr.bm_w;
        hdr[b + 5] = rp.chr.bm_h;
        hdr[b + 6] = static_cast<int32_t>(rp.chr.bm_addr);
        hdr[b + 7] = static_cast<int32_t>(rp.chr.bm_pal);
        hdr[b + 8] = rp.kx;
        hdr[b + 9] = rp.ky;
        hdr[b + 10] = rp.dkax;
        hdr[b + 11] = static_cast<int32_t>(
            (rp.coef ? 1u : 0u) |
            (rp.coef1w ? 2u : 0u) |
            (static_cast<uint32_t>(rp.coef_mode & 3) << 2) |
            (static_cast<uint32_t>(rp.over & 3) << 4) |
            (static_cast<uint32_t>(rp.over_pn) << 8));
        for (int pl = 0; pl < 16; ++pl) hdr[b + 12 + pl] = static_cast<int32_t>(rp.plane[pl]);
    }

    // Pack Bolivia SP3 Wind VFX state in hdr[216..230] for zero-fallback WebGL2 GPU overlay.
    if (vdp.wind_vfx.active) {
        const auto& wf = vdp.wind_vfx;
        const bool inflow_on = vdp.copa_vfx_replace &&
            (vdp.copa_sp_vfx.charge_count > 0u || wf.frame_age <= 34u);
        float hub_x = wf.cx;
        float hub_y = wf.cy;
        if (hub_x <= 0.0f && hub_y <= 0.0f && vdp.copa_sp_vfx.charge_count > 0u) {
            float sx_sum = 0.0f, sy_sum = 0.0f;
            for (int i = 0; i < static_cast<int>(vdp.copa_sp_vfx.charge_count); ++i) {
                const auto& r = vdp.copa_sp_vfx.charge_rays[i];
                sx_sum += (static_cast<float>(r.x0) + static_cast<float>(r.x1)) * 0.5f;
                sy_sum += (static_cast<float>(r.y0) + static_cast<float>(r.y1)) * 0.5f;
            }
            hub_x = sx_sum / static_cast<float>(vdp.copa_sp_vfx.charge_count);
            hub_y = sy_sum / static_cast<float>(vdp.copa_sp_vfx.charge_count);
        }
        const int src_tc = std::clamp(wf.trail_count, 0, StvVdpState::WindVfxState::kTrailLen);
        const int packed_tc = std::min(10, src_tc);
        const uint32_t age_c = std::min<uint32_t>(255u, std::max<uint32_t>(vdp.copa_sp_vfx.charge_age, wf.frame_age));
        const uint32_t wflags =
            1u |
            (inflow_on ? 2u : 0u) |
            (static_cast<uint32_t>(packed_tc & 0xF) << 4) |
            ((wf.frame_age & 0xFFFu) << 12) |
            ((age_c & 0xFFu) << 24);
        hdr[216] = static_cast<int32_t>(wflags);
        hdr[217] = static_cast<int32_t>(std::lround(hub_x * 16.0f));
        hdr[218] = static_cast<int32_t>(std::lround(hub_y * 16.0f));
        hdr[219] = static_cast<int32_t>(std::lround(std::max(10.0f, wf.radius) * 16.0f));
        for (int i = 0; i < packed_tc; ++i) {
            const int si = (packed_tc > 1 && src_tc > packed_tc)
                ? ((i * (src_tc - 1)) / (packed_tc - 1))
                : i;
            const int32_t tx4 = static_cast<int32_t>(std::lround(wf.trail_x[si] * 4.0f));
            const int32_t ty4 = static_cast<int32_t>(std::lround(wf.trail_y[si] * 4.0f));
            hdr[220 + i] = (tx4 & 0xFFFF) | (ty4 << 16);
        }
        hdr[230] = (vdp.copa_sp_vfx.charge_count > 0u) ? 1 : 0;
    }

    hdr[240] = static_cast<int32_t>(line_off);
    hdr[241] = static_cast<int32_t>(cram_off);
    hdr[242] = static_cast<int32_t>(fb_off);
    hdr[243] = static_cast<int32_t>(v2_off);
    hdr[244] = static_cast<int32_t>(total_bytes);
    if (use_gpu_vdp1) {
        hdr[245] = static_cast<int32_t>(v1_dirty_mask);
        hdr[246] = static_cast<int32_t>(disp_gpu.count);
        hdr[247] = static_cast<int32_t>(disp_gpu.tvmr) | (static_cast<int32_t>(disp_gpu.ewdr) << 16);
        hdr[248] = (static_cast<int32_t>(disp_gpu.ew_x0) & 0xFFFF) | (static_cast<int32_t>(disp_gpu.ew_y0) << 16);
        hdr[249] = (static_cast<int32_t>(disp_gpu.ew_x1) & 0xFFFF) | (static_cast<int32_t>(disp_gpu.ew_y1) << 16);
        hdr[250] = static_cast<int32_t>(disp_gpu.fb_w);
        hdr[251] = static_cast<int32_t>(disp_gpu.fb_h);
        hdr[252] = static_cast<int32_t>(v1_cmds_off);
        hdr[253] = static_cast<int32_t>(v1_vram_off);
    }

    // 1. Per-line table: 8 RGBA32I texels (32 ints) per output row
    int32_t* lt = reinterpret_cast<int32_t*>(dst + line_off);
    for (int out_y = 0; out_y < fh; ++out_y) {
        const int y = out_y * prep.y_step;
        const int32_t w0p = prep.win_y_ok[out_y][0]
            ? ((static_cast<int32_t>(prep.win_x0[out_y][0]) & 0xFFFF) | (static_cast<int32_t>(prep.win_x1[out_y][0]) << 16))
            : -1;
        const int32_t w1p = prep.win_y_ok[out_y][1]
            ? ((static_cast<int32_t>(prep.win_x0[out_y][1]) & 0xFFFF) | (static_cast<int32_t>(prep.win_x1[out_y][1]) << 16))
            : -1;
        lt[0] = static_cast<int32_t>(prep.back_rgb_line[out_y]);
        lt[1] = w0p;
        lt[2] = w1p;
        lt[3] = y;

        const auto& r0 = prep.rbg_lines[out_y][0];
        const auto& r1 = prep.rbg_lines[out_y][1];
        lt[4] = r0.xsp;
        lt[5] = r0.ysp;
        lt[6] = r0.xp;
        lt[7] = r0.yp;
        lt[8] = r0.ldx;
        lt[9] = r0.ldy;
        lt[10] = static_cast<int32_t>(r0.ka);
        lt[11] = static_cast<int32_t>(rbg.perdot ? r0.line_coef_dot : r0.line_coef);

        lt[12] = r1.xsp;
        lt[13] = r1.ysp;
        lt[14] = r1.xp;
        lt[15] = r1.yp;
        lt[16] = r1.ldx;
        lt[17] = r1.ldy;
        lt[18] = static_cast<int32_t>(r1.ka);
        lt[19] = static_cast<int32_t>((rbg.mode == 2 || !rbg.perdot) ? r1.line_coef : r1.line_coef_dot);

        const auto& n0 = vdp.compose_nbg[0];
        const auto& n1 = vdp.compose_nbg[1];
        const auto& n2 = vdp.compose_nbg[2];
        const auto& n3 = vdp.compose_nbg[3];
        lt[20] = n0.line ? n0.lx[y] : n0.sx;
        lt[21] = n0.line ? n0.ldx[y] : n0.dx;
        lt[22] = n0.line ? n0.ly[y] : (n0.sy + y * n0.dy);
        lt[23] = 0;
        lt[24] = n1.line ? n1.lx[y] : n1.sx;
        lt[25] = n1.line ? n1.ldx[y] : n1.dx;
        lt[26] = n1.line ? n1.ly[y] : (n1.sy + y * n1.dy);
        lt[27] = 0;
        lt[28] = n2.sx;
        lt[29] = n2.sy + y * n2.dy;
        lt[30] = n3.sx;
        lt[31] = n3.sy + y * n3.dy;
        lt += 32;
    }

    // 2. CRAM RGBA8 LUT (2048 x 4 bytes, A = msb ? 255 : 0)
    uint8_t* cram_dst = dst + cram_off;
    for (uint32_t i = 0u; i < 2048u; ++i) {
        const uint32_t rgb = prep.cram_rgb[i];
        cram_dst[i * 4u + 0u] = static_cast<uint8_t>((rgb >> 16) & 0xFFu);
        cram_dst[i * 4u + 1u] = static_cast<uint8_t>((rgb >> 8) & 0xFFu);
        cram_dst[i * 4u + 2u] = static_cast<uint8_t>(rgb & 0xFFu);
        cram_dst[i * 4u + 3u] = prep.cram_msb[i] ? 255u : 0u;
    }

    // 3. Cropped VDP1 FB (fb_w_packed x fb_rows uint16_t) when CPU VDP1 mode is active
    if (!use_gpu_vdp1) {
        const uint16_t* const fb_src = vdp.vdp1_display();
        uint16_t* fb_dst = reinterpret_cast<uint16_t*>(dst + fb_off);
        const size_t row_copy_bytes = static_cast<size_t>(fb_w_packed) * 2u;
        for (int y = 0; y < fb_rows; ++y) {
            std::memcpy(fb_dst + static_cast<size_t>(y) * static_cast<size_t>(fb_w_packed),
                        fb_src + static_cast<size_t>(y) * static_cast<size_t>(prep.fb_pitch),
                        row_copy_bytes);
        }
    }

    // 4. Dirty VDP2 VRAM 16 KiB chunks
    uint8_t* v2_dst = dst + v2_off;
    for (uint32_t c = 0u; c < 32u; ++c) {
        if (dirty_mask & (1u << c)) {
            std::memcpy(v2_dst, cur_v2 + c * 16384u, 16384u);
            v2_dst += 16384u;
        }
    }

    // 5. GPU VDP1 command stream & dirty VDP1 VRAM 16 KiB chunks
    if (use_gpu_vdp1) {
        if (v1_cmds_bytes > 0u) {
            std::memcpy(dst + v1_cmds_off, disp_gpu.cmds, v1_cmds_bytes);
        }
        uint8_t* v1_dst = dst + v1_vram_off;
        for (uint32_t c = 0u; c < 32u; ++c) {
            if (v1_dirty_mask & (1u << c)) {
                std::memcpy(v1_dst, cur_v1 + c * 16384u, 16384u);
                v1_dst += 16384u;
            }
        }
    }

    return total_bytes;
}

void StvMachineState::compute_digests(uint8_t high_wram_sha[32],
                                      uint8_t low_wram_sha[32],
                                      uint8_t vdp1_vram_sha[32],
                                      uint8_t vdp2_vram_sha[32],
                                      uint8_t vdp2_cram_sha[32],
                                      uint8_t vdp2_regs_sha[32],
                                      uint8_t frame_sha[32]) const {
    sha256_bytes(inner.high_wram.data(), inner.high_wram.size(), high_wram_sha);
    sha256_bytes(inner.low_wram.data(), inner.low_wram.size(), low_wram_sha);
    sha256_bytes(vdp.vdp1_vram, StvVdpState::kVdp1VramSize, vdp1_vram_sha);
    sha256_bytes(inner.vdp2_vram.data(), inner.vdp2_vram.size(), vdp2_vram_sha);
    sha256_bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize, vdp2_cram_sha);
    sha256_bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize, vdp2_regs_sha);
    const size_t px_bytes = static_cast<size_t>(vdp.frame_w * vdp.frame_h) * sizeof(uint32_t);
    sha256_bytes(reinterpret_cast<const uint8_t*>(vdp.frame_px), px_bytes, frame_sha);
}

namespace {

uint64_t snapshot_fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0u; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

void write_le16_raw(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void write_le32_raw(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void write_le64_raw(uint8_t* dst, uint64_t v) {
    write_le32_raw(dst, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    write_le32_raw(dst + 4u, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

uint16_t read_le16_raw(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

uint32_t read_le32_raw(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

uint64_t read_le64_raw(const uint8_t* src) {
    const uint64_t lo = static_cast<uint64_t>(read_le32_raw(src));
    const uint64_t hi = static_cast<uint64_t>(read_le32_raw(src + 4u));
    return lo | (hi << 32);
}

class SnapWriter {
public:
    SnapWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }

    void u8(uint8_t v) {
        if (!ensure(1u)) return;
        buf_[pos_++] = v;
    }
    void b(bool v) { u8(v ? 1u : 0u); }
    void u16(uint16_t v) {
        if (!ensure(2u)) return;
        write_le16_raw(buf_ + pos_, v);
        pos_ += 2u;
    }
    void u32(uint32_t v) {
        if (!ensure(4u)) return;
        write_le32_raw(buf_ + pos_, v);
        pos_ += 4u;
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        if (!ensure(8u)) return;
        write_le64_raw(buf_ + pos_, v);
        pos_ += 8u;
    }
    void bytes(const uint8_t* src, size_t n) {
        if (!ensure(n)) return;
        if (n > 0u) std::memcpy(buf_ + pos_, src, n);
        pos_ += n;
    }
    void u16_arr(const uint16_t* src, size_t count) {
        if (!ensure(count * 2u)) return;
        for (size_t i = 0u; i < count; ++i) {
            write_le16_raw(buf_ + pos_, src[i]);
            pos_ += 2u;
        }
    }

private:
    bool ensure(size_t n) {
        if (!ok_ || pos_ > cap_ || n > (cap_ - pos_)) {
            ok_ = false;
            return false;
        }
        return true;
    }
    uint8_t* buf_ = nullptr;
    size_t cap_ = 0u;
    size_t pos_ = 0u;
    bool ok_ = true;
};

class SnapReader {
public:
    SnapReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }

    uint8_t u8() {
        if (!ensure(1u)) return 0u;
        return buf_[pos_++];
    }
    bool b() { return u8() != 0u; }
    uint16_t u16() {
        if (!ensure(2u)) return 0u;
        const uint16_t v = read_le16_raw(buf_ + pos_);
        pos_ += 2u;
        return v;
    }
    uint32_t u32() {
        if (!ensure(4u)) return 0u;
        const uint32_t v = read_le32_raw(buf_ + pos_);
        pos_ += 4u;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64() {
        if (!ensure(8u)) return 0u;
        const uint64_t v = read_le64_raw(buf_ + pos_);
        pos_ += 8u;
        return v;
    }
    void bytes(uint8_t* dst, size_t n) {
        if (!ensure(n)) return;
        if (n > 0u) std::memcpy(dst, buf_ + pos_, n);
        pos_ += n;
    }
    void u16_arr(uint16_t* dst, size_t count) {
        if (!ensure(count * 2u)) return;
        for (size_t i = 0u; i < count; ++i) {
            dst[i] = read_le16_raw(buf_ + pos_);
            pos_ += 2u;
        }
    }

private:
    bool ensure(size_t n) {
        if (!ok_ || pos_ > len_ || n > (len_ - pos_)) {
            ok_ = false;
            return false;
        }
        return true;
    }
    const uint8_t* buf_ = nullptr;
    size_t len_ = 0u;
    size_t pos_ = 0u;
    bool ok_ = true;
};

constexpr size_t kSnapHeaderBytes = 24u;

}  // namespace

uint32_t StvMachineState::save_state_bytes(uint8_t* dst, size_t dst_capacity) const {
    if (dst == nullptr || dst_capacity < kSnapHeaderBytes) return 0u;

    SnapWriter w(dst + kSnapHeaderBytes, dst_capacity - kSnapHeaderBytes);
    for (int i = 0; i < 2; ++i) {
        const auto& c = cpus[i];
        for (int r = 0; r < 16; ++r) w.u32(c.r[r]);
        w.u32(c.pc);
        w.u32(c.pr);
        w.u32(c.gbr);
        w.u32(c.vbr);
        w.u32(c.mach);
        w.u32(c.macl);
        w.u32(c.sr);
        w.u32(c.delay);
        w.u32(c.cur_pc);
        w.u32(c.imask_meta);
        w.u32(c.context_loaded);
        w.u64(c.retired);
        w.u64(c.cycles);
    }

    w.bytes(inner.high_wram.data(), inner.high_wram.size());
    w.bytes(inner.low_wram.data(), inner.low_wram.size());
    w.bytes(inner.vdp2_vram.data(), inner.vdp2_vram.size());
    w.bytes(inner.vdp2_written.data(), inner.vdp2_written.size());
    w.b(inner.strict_vram_init);
    for (int i = 0; i < 2; ++i) {
        const auto& d = inner.divu[i];
        w.u32(d.dvsr);
        w.u32(d.dvdnth);
        w.u32(d.dvdntl);
        w.u32(d.ovf);
        w.u32(d.ovfie);
        w.u32(d.ops);
        w.u32(d.written);

        const auto& f = inner.frt[i];
        w.u32(f.ftcsr);
        w.u32(f.tier);
        w.u32(f.tcr);
        w.u32(f.ocra);
        w.u32(f.ocrb);
        w.u32(f.frc);
        w.u64(f.frc_base);
        w.b(f.has_next_event);
        w.u64(f.next_event_cycle);

        const auto& dm = inner.dmac[i];
        w.u32(dm.drcr0);
        w.u32(dm.sar0);
        w.u32(dm.dar0);
        w.u32(dm.tcr0);
        w.u32(dm.chcr0);
        w.u32(dm.dmaor);
        w.u32(dm.dma_irq0);
        w.u32(dm.transfers_completed);

        w.u8(inner.ccr[i]);
    }

    const_cast<StvVdpState&>(vdp).materialize_cpu_fb(0);
    const_cast<StvVdpState&>(vdp).materialize_cpu_fb(1);
    w.bytes(vdp.vdp1_vram, StvVdpState::kVdp1VramSize);
    w.u16_arr(vdp.vdp1_fb[0], StvVdpState::kVdp1FbPlaneWords);
    w.u16_arr(vdp.vdp1_fb[1], StvVdpState::kVdp1FbPlaneWords);
    w.u16_arr(vdp.vdp1_regs, 0x10u);
    w.i32(vdp.draw_fb);
    w.b(vdp.change_pending);
    w.b(vdp.erase_pending);
    w.b(vdp.erase_field);
    w.i32(vdp.sys_x);
    w.i32(vdp.sys_y);
    w.i32(vdp.ux0);
    w.i32(vdp.uy0);
    w.i32(vdp.ux1);
    w.i32(vdp.uy1);
    w.i32(vdp.local_x);
    w.i32(vdp.local_y);
    w.u64(vdp.frame_changes);
    w.u64(vdp.draws);
    w.b(vdp.vdp1_unsupported);
    w.bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    w.bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    w.i32(vdp.frame_w);
    w.i32(vdp.frame_h);

    w.bytes(backupram, kBackupRamSize);
    w.bytes(sound_ram, kSoundRamSize);
    w.bytes(scsp_regs, kScspRegsSize);

    for (int i = 0; i < 3; ++i) {
        w.u32(scu_dma[i].r);
        w.u32(scu_dma[i].w);
        w.u32(scu_dma[i].c);
        w.u32(scu_dma[i].ad);
        w.u32(scu_dma[i].en);
        w.u32(scu_dma[i].md);
    }
    w.u32(scu_ims);
    w.u32(scu_ist);
    for (int i = 0; i < 3; ++i) {
        w.b(scu_dma_irq_armed[i]);
        w.u64(scu_dma_irq_cycle[i]);
    }
    w.u32(dmac1.sar);
    w.u32(dmac1.dar);
    w.u32(dmac1.tcr);
    w.u32(dmac1.chcr);
    w.u8(dmac1.drcr);
    for (int i = 0; i < 2; ++i) {
        w.u8(frt_tocr[i]);
        w.u16(frt_icr[i]);
    }

    w.bytes(smpc_ireg, 7u);
    w.bytes(smpc_oreg, 32u);
    w.u8(smpc_sr);
    w.u8(smpc_sf);
    w.u8(smpc_pdr1_out);
    w.u8(smpc_pdr2_out);
    w.u8(smpc_ddr1);
    w.u8(smpc_ddr2);
    w.u8(smpc_gamebank_select);
    w.b(smpc_peri_pending);
    w.u64(smpc_irq_cycle);
    w.b(smpc_irq_armed);
    w.u64(rtc_base_cycles);

    w.u8(inputs.port_a);
    w.u8(inputs.port_b);
    w.u8(inputs.port_c);
    w.u8(inputs.port_e);
    w.u8(inputs.port_f);
    w.u8(inputs.pdr1_in);
    w.u8(inputs.pdr2_in);
    w.u8(ioga_system_output);
    w.u8(ioga_mode);
    w.u8(ioga_portg);
    w.u8(ioga_billboard);
    w.u8(ioga_direction);

    w.u16(vdp2_tvstat);
    w.u16(vdp2_hcnt);
    w.u16(vdp2_vcnt);
    w.u64(next_vbi_cyc);
    w.u64(next_vbo_cyc);
    w.b(pending_vbi);
    w.b(pending_vbo);

    w.u32(rounds_done);
    w.u32(main_steps);
    w.u32(slave_steps);
    w.u32(vbi_count);
    w.u32(vbo_count);
    w.u32(sched2_hits);
    w.u32(minit_count);
    w.u32(sinit_count);
    w.u32(vdp1_vram_writes);
    w.u32(scu_dma_runs);
    w.u32(sh_dmac1_runs);
    w.u32(ioga_reads);
    w.u32(ioga_writes);
    w.u32(smpc_input_reads);
    w.u32(cart_reads);
    w.u32(soundram_reads);
    w.u32(soundram_writes);
    w.b(scsp_ack_armed);
    w.u64(scsp_ack_cycle);
    w.u32(last_write_val);
    w.u32(last_write_ea);

    w.b(halted);
    w.i32(halt_cpu);
    w.u32(last_fault.pc);
    w.u32(last_fault.address);
    w.u16(last_fault.opcode);
    w.u8(last_fault.is_write);
    w.u8(last_fault.width);

    const uint32_t mmio_n = (mmio_log_count <= kMaxMmioLog) ? mmio_log_count : kMaxMmioLog;
    w.u32(mmio_n);
    for (uint32_t i = 0u; i < mmio_n; ++i) {
        const auto& e = mmio_log[i];
        w.u64(e.cycles);
        w.u32(e.round);
        w.u32(e.pc);
        w.u32(e.arg0);
        w.u32(e.arg1);
        w.u8(e.kind);
        w.u8(e.cpu_id);
        w.u8(e.width);
        w.u8(e.reserved);
    }

    const uint32_t ioga_n = (ioga_log_count <= kMaxIogaLog) ? ioga_log_count : kMaxIogaLog;
    w.u32(ioga_n);
    for (uint32_t i = 0u; i < ioga_n; ++i) {
        const auto& e = ioga_log[i];
        w.u32(e.pc);
        w.u32(e.address);
        w.u8(e.val);
        w.u8(e.cpu_id);
        w.u8(e.is_write);
        w.u8(e.width);
    }

    const bool crypt_dirty =
        cart_prot_enabled || crypt315.prot_cur_address != 0u || crypt315.subkey != 0u ||
        crypt315.enc_ready || crypt315.first_read || crypt315.abus_protenable != 0u ||
        crypt315.abus_protkey != 0u || crypt315.a_bus[0] != 0u || crypt315.a_bus[1] != 0u ||
        crypt315.a_bus[2] != 0u || crypt315.a_bus[3] != 0u;
    if (crypt_dirty) {
        w.u32(crypt315.key);
        w.u16(crypt315.subkey);
        w.u32(crypt315.prot_cur_address);
        w.u32(crypt315.dec_header);
        w.u16(crypt315.dec_hist);
        w.b(crypt315.enc_ready);
        w.b(crypt315.first_read);
        w.i32(crypt315.buffer_pos);
        w.i32(crypt315.line_buffer_pos);
        w.i32(crypt315.line_buffer_size);
        w.i32(crypt315.buffer_bit);
        w.i32(crypt315.buffer_bit2);
        w.i32(crypt315.block_pos);
        w.i32(crypt315.block_size);
        w.i32(crypt315.block_numlines);
        w.i32(crypt315.done_compression);
        w.u16(crypt315.buffer2a);
        w.bytes(crypt315.buffer2, 2u);
        w.u8(crypt315.line_cur);
        w.bytes(crypt315.buffer, Stv3155881State::kBufferSize);
        w.bytes(crypt315.line_buf[0], Stv3155881State::kLineSize);
        w.bytes(crypt315.line_buf[1], Stv3155881State::kLineSize);
        for (int i = 0; i < 4; ++i) w.u32(crypt315.a_bus[i]);
        w.u32(crypt315.abus_protenable);
        w.u32(crypt315.abus_protkey);
    }

    const bool audio_dirty = real_audio_enabled || m68k_enabled;
    if (audio_dirty) {
        w.b(real_audio_enabled);
        w.b(m68k_enabled);
        w.b(m68k_in_reset);
        w.b(pending_sound_irq);
        w.i32(m68k_debt);
        w.u64(audio_cyc_acc);
        w.u64(audio_last_sh2_cyc);
        w.u32(m68k_pc_cached);
        w.u32(audio_pcm_produced);
        w.u32(audio_pcm_nonzero);

        for (int i = 0; i < 32; ++i) {
            const auto& s = scsp.slots[i];
            w.u16_arr(s.r, 16u);
            w.b(s.active);
            w.b(s.keyed);
            w.b(s.backwards);
            w.u64(static_cast<uint64_t>(s.pos));
            w.i32(s.eg_state);
            w.i32(s.eg_vol);
            w.i32(s.ar);
            w.i32(s.d1r);
            w.i32(s.d2r);
            w.i32(s.rr);
            w.i32(s.dl);
            w.u32(s.plfo.phase);
            w.u32(s.plfo.step);
            w.u8(s.plfo.table_idx);
            w.u8(s.plfo.scale_idx);
            w.u32(s.alfo.phase);
            w.u32(s.alfo.step);
            w.u8(s.alfo.table_idx);
            w.u8(s.alfo.scale_idx);
        }
        w.u16_arr(scsp.com, 0x18u);
        for (int i = 0; i < 3; ++i) {
            w.i32(scsp.timers[i].ctl);
            w.i32(scsp.timers[i].counter);
            w.i32(scsp.timers[i].reload);
        }
        w.u32(scsp.scieb);
        w.u32(scsp.scipd);
        for (int i = 0; i < 3; ++i) w.u32(scsp.scilv[i]);
        w.u32(scsp.mcieb);
        w.u32(scsp.mcipd);
        w.b(scsp.main_line);
        w.u32(scsp.samples);
        for (int i = 0; i < 64; ++i) w.i32(scsp.stack[i]);
        w.u32(scsp.stack_pos);
        for (int i = 0; i < 64; ++i) w.u16(static_cast<uint16_t>(scsp.dsp.coef[i]));
        w.u16_arr(scsp.dsp.madrs, 32u);
        w.u16_arr(scsp.dsp.mpro, 128u * 4u);
        for (int i = 0; i < 128; ++i) w.i32(scsp.dsp.temp[i]);
        for (int i = 0; i < 32; ++i) w.i32(scsp.dsp.mems[i]);
        for (int i = 0; i < 16; ++i) w.i32(scsp.dsp.mixs[i]);
        for (int i = 0; i < 2; ++i) w.u16(static_cast<uint16_t>(scsp.dsp.exts[i]));
        for (int i = 0; i < 16; ++i) w.u16(static_cast<uint16_t>(scsp.dsp.efreg[i]));
        w.u32(scsp.dsp.dec);
        w.i32(scsp.dsp.last_step);
        w.u32(scsp.noise);
        w.u32(scsp.rbl);
        w.u32(scsp.rbp);
        w.i32(scsp.cpu_level);

        uint8_t ctx_tmp[kMaxM68kCtxBytes]{};
        uint32_t ctx_len = m68k_ctx_size;
        if (m68k_enabled) {
            if (g_active_m68k_machine == this) {
                const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
                if (sz <= kMaxM68kCtxBytes) {
                    ctx_len = static_cast<uint32_t>(m68k_get_context(ctx_tmp));
                }
            } else if (ctx_len > 0u && ctx_len <= kMaxM68kCtxBytes) {
                std::memcpy(ctx_tmp, m68k_ctx_blob, ctx_len);
            }
        } else {
            ctx_len = 0u;
        }
        w.u32(ctx_len);
        if (ctx_len > 0u) w.bytes(ctx_tmp, ctx_len);
    }

    const bool rule_dirty = rule_hard_mode || rule_mode != 0u || rule_goal_triggered || rule_goal_delay != 0u;
    if (rule_dirty) {
        w.b(rule_hard_mode);
        w.u8(rule_mode);
        w.u16(rule_value);
        w.b(rule_goal_triggered);
        w.u16(rule_goal_delay);
    }

    const bool copa_dirty = copa_mode_active;
    if (copa_dirty) {
        w.b(copa_mode_active);
        w.b(copa_entered_gameplay);
        w.b(copa_returned_to_title);
        w.u8(copa_ts_heartbeat);
        w.u32(copa_sp3_wind_timer);
        w.u32(copa_sp3_kick_count);
        w.u32(copa_ball_srca);
        w.bytes(reinterpret_cast<const uint8_t*>(&vdp.copa_sp_vfx), sizeof(vdp.copa_sp_vfx));
        w.bytes(reinterpret_cast<const uint8_t*>(&vdp.copa_bracket), sizeof(vdp.copa_bracket));
    }

    const bool coop_dirty = coop.enabled;
    if (coop_dirty) {
        w.b(coop.enabled);
        w.u8(coop.p1_actor);
        w.u8(coop.p2_actor);
        w.u8(coop.last_passer_slot);
        w.u8(coop.prev_ball_owner);
        w.u8(coop.p1_switch_cd);
        w.u8(coop.p2_switch_cd);
        w.u8(coop.p1_auto_cd);
        w.u8(coop.p2_auto_cd);
        w.u8(coop.prev_pa_btn);
        w.u8(coop.prev_pb_btn);
        w.u8(coop.call_pass_timer[0]);
        w.u8(coop.call_pass_timer[1]);
    }

    if (!w.ok()) return 0u;
    const uint32_t payload_bytes = static_cast<uint32_t>(w.pos());
    const uint32_t flags =
        (has_bios ? 1u : 0u) | (has_cart ? 2u : 0u) | (cart_prot_enabled ? 4u : 0u) |
        (crypt_dirty ? 8u : 0u) | (audio_dirty ? 16u : 0u) | (rule_dirty ? 32u : 0u) |
        (copa_dirty ? 64u : 0u) | (coop_dirty ? 128u : 0u);
    const uint64_t hash = snapshot_fnv1a64(dst + kSnapHeaderBytes, payload_bytes);
    write_le32_raw(dst + 0u, kSnapshotMagic);
    write_le32_raw(dst + 4u, kSnapshotVersion);
    write_le32_raw(dst + 8u, flags);
    write_le32_raw(dst + 12u, payload_bytes);
    write_le64_raw(dst + 16u, hash);
    return kSnapHeaderBytes + payload_bytes;
}

bool StvMachineState::load_state_bytes(const uint8_t* src, size_t len, const char** out_error) {
    auto fail = [&](const char* msg) -> bool {
        if (out_error) *out_error = msg;
        return false;
    };
    if (src == nullptr || len < kSnapHeaderBytes) {
        return fail("snapshot_too_short");
    }
    const uint32_t magic = read_le32_raw(src + 0u);
    const uint32_t version = read_le32_raw(src + 4u);
    const uint32_t flags = read_le32_raw(src + 8u);
    const uint32_t payload_bytes = read_le32_raw(src + 12u);
    const uint64_t expected_hash = read_le64_raw(src + 16u);

    if (magic != kSnapshotMagic) return fail("snapshot_bad_magic");
    if (version != kSnapshotVersion) return fail("snapshot_bad_version");
    if (static_cast<size_t>(payload_bytes) != (len - kSnapHeaderBytes)) {
        return fail("snapshot_bad_length");
    }
    const uint64_t actual_hash = snapshot_fnv1a64(src + kSnapHeaderBytes, payload_bytes);
    if (actual_hash != expected_hash) return fail("snapshot_checksum_mismatch");
    if ((flags & 1u) != 0u && !has_bios) return fail("snapshot_requires_bios");
    if ((flags & 2u) != 0u && !has_cart) return fail("snapshot_requires_cart");

    SnapReader r(src + kSnapHeaderBytes, payload_bytes);
    cart_prot_enabled = (flags & 4u) != 0u;

    for (int i = 0; i < 2; ++i) {
        auto& c = cpus[i];
        for (int reg = 0; reg < 16; ++reg) c.r[reg] = r.u32();
        c.pc = r.u32();
        c.pr = r.u32();
        c.gbr = r.u32();
        c.vbr = r.u32();
        c.mach = r.u32();
        c.macl = r.u32();
        c.sr = r.u32();
        c.delay = r.u32();
        c.cur_pc = r.u32();
        c.imask_meta = r.u32();
        c.context_loaded = r.u32();
        c.retired = r.u64();
        c.cycles = r.u64();
    }

    r.bytes(inner.high_wram.data(), inner.high_wram.size());
    invalidate_all_aot_pages();
    r.bytes(inner.low_wram.data(), inner.low_wram.size());
    r.bytes(inner.vdp2_vram.data(), inner.vdp2_vram.size());
    r.bytes(inner.vdp2_written.data(), inner.vdp2_written.size());
    inner.strict_vram_init = r.b();
    for (int i = 0; i < 2; ++i) {
        auto& d = inner.divu[i];
        d.dvsr = r.u32();
        d.dvdnth = r.u32();
        d.dvdntl = r.u32();
        d.ovf = r.u32();
        d.ovfie = r.u32();
        d.ops = r.u32();
        d.written = r.u32();

        auto& f = inner.frt[i];
        f.ftcsr = r.u32();
        f.tier = r.u32();
        f.tcr = r.u32();
        f.ocra = r.u32();
        f.ocrb = r.u32();
        f.frc = r.u32();
        f.frc_base = r.u64();
        f.has_next_event = r.b();
        f.next_event_cycle = r.u64();

        auto& dm = inner.dmac[i];
        dm.drcr0 = r.u32();
        dm.sar0 = r.u32();
        dm.dar0 = r.u32();
        dm.tcr0 = r.u32();
        dm.chcr0 = r.u32();
        dm.dmaor = r.u32();
        dm.dma_irq0 = r.u32();
        dm.transfers_completed = r.u32();

        inner.ccr[i] = r.u8();
    }

    r.bytes(vdp.vdp1_vram, StvVdpState::kVdp1VramSize);
    r.u16_arr(vdp.vdp1_fb[0], StvVdpState::kVdp1FbPlaneWords);
    r.u16_arr(vdp.vdp1_fb[1], StvVdpState::kVdp1FbPlaneWords);
    r.u16_arr(vdp.vdp1_regs, 0x10u);
    vdp.draw_fb = r.i32();
    vdp.invalidate_gpu_vdp1_frames();
    vdp.change_pending = r.b();
    vdp.erase_pending = r.b();
    vdp.erase_field = r.b();
    vdp.sys_x = r.i32();
    vdp.sys_y = r.i32();
    vdp.ux0 = r.i32();
    vdp.uy0 = r.i32();
    vdp.ux1 = r.i32();
    vdp.uy1 = r.i32();
    vdp.local_x = r.i32();
    vdp.local_y = r.i32();
    vdp.frame_changes = r.u64();
    vdp.draws = r.u64();
    vdp.vdp1_unsupported = r.b();
    r.bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    r.bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    vdp.frame_w = r.i32();
    vdp.frame_h = r.i32();

    r.bytes(backupram, kBackupRamSize);
    r.bytes(sound_ram, kSoundRamSize);
    r.bytes(scsp_regs, kScspRegsSize);

    for (int i = 0; i < 3; ++i) {
        scu_dma[i].r = r.u32();
        scu_dma[i].w = r.u32();
        scu_dma[i].c = r.u32();
        scu_dma[i].ad = r.u32();
        scu_dma[i].en = r.u32();
        scu_dma[i].md = r.u32();
    }
    scu_ims = r.u32();
    scu_ist = r.u32();
    for (int i = 0; i < 3; ++i) {
        scu_dma_irq_armed[i] = r.b();
        scu_dma_irq_cycle[i] = r.u64();
    }
    dmac1.sar = r.u32();
    dmac1.dar = r.u32();
    dmac1.tcr = r.u32();
    dmac1.chcr = r.u32();
    dmac1.drcr = r.u8();
    for (int i = 0; i < 2; ++i) {
        frt_tocr[i] = r.u8();
        frt_icr[i] = r.u16();
    }

    r.bytes(smpc_ireg, 7u);
    r.bytes(smpc_oreg, 32u);
    smpc_sr = r.u8();
    smpc_sf = r.u8();
    smpc_pdr1_out = r.u8();
    smpc_pdr2_out = r.u8();
    smpc_ddr1 = r.u8();
    smpc_ddr2 = r.u8();
    smpc_gamebank_select = r.u8();
    smpc_peri_pending = r.b();
    smpc_irq_cycle = r.u64();
    smpc_irq_armed = r.b();
    rtc_base_cycles = r.u64();

    inputs.port_a = r.u8();
    inputs.port_b = r.u8();
    inputs.port_c = r.u8();
    inputs.port_e = r.u8();
    inputs.port_f = r.u8();
    inputs.pdr1_in = r.u8();
    inputs.pdr2_in = r.u8();
    ioga_system_output = r.u8();
    ioga_mode = r.u8();
    ioga_portg = r.u8();
    ioga_billboard = r.u8();
    ioga_direction = r.u8();

    vdp2_tvstat = r.u16();
    vdp2_hcnt = r.u16();
    vdp2_vcnt = r.u16();
    next_vbi_cyc = r.u64();
    next_vbo_cyc = r.u64();
    pending_vbi = r.b();
    pending_vbo = r.b();

    rounds_done = r.u32();
    main_steps = r.u32();
    slave_steps = r.u32();
    vbi_count = r.u32();
    vbo_count = r.u32();
    sched2_hits = r.u32();
    minit_count = r.u32();
    sinit_count = r.u32();
    vdp1_vram_writes = r.u32();
    scu_dma_runs = r.u32();
    sh_dmac1_runs = r.u32();
    ioga_reads = r.u32();
    ioga_writes = r.u32();
    smpc_input_reads = r.u32();
    cart_reads = r.u32();
    soundram_reads = r.u32();
    soundram_writes = r.u32();
    scsp_ack_armed = r.b();
    scsp_ack_cycle = r.u64();
    last_write_val = r.u32();
    last_write_ea = r.u32();

    halted = r.b();
    halt_cpu = r.i32();
    last_fault.ok = !halted;
    last_fault.reason = halted ? "halted_in_snapshot" : nullptr;
    last_fault.pc = r.u32();
    last_fault.address = r.u32();
    last_fault.opcode = r.u16();
    last_fault.is_write = r.u8();
    last_fault.width = r.u8();

    const uint32_t mmio_n = r.u32();
    if (!r.ok() || mmio_n > kMaxMmioLog) return fail("snapshot_bad_mmio_count");
    mmio_log_count = mmio_n;
    for (uint32_t i = 0u; i < mmio_n; ++i) {
        auto& e = mmio_log[i];
        e.cycles = r.u64();
        e.round = r.u32();
        e.pc = r.u32();
        e.arg0 = r.u32();
        e.arg1 = r.u32();
        e.kind = r.u8();
        e.cpu_id = r.u8();
        e.width = r.u8();
        e.reserved = r.u8();
    }

    const uint32_t ioga_n = r.u32();
    if (!r.ok() || ioga_n > kMaxIogaLog) return fail("snapshot_bad_ioga_count");
    ioga_log_count = ioga_n;
    for (uint32_t i = 0u; i < ioga_n; ++i) {
        auto& e = ioga_log[i];
        e.pc = r.u32();
        e.address = r.u32();
        e.val = r.u8();
        e.cpu_id = r.u8();
        e.is_write = r.u8();
        e.width = r.u8();
    }

    crypt315.reset();
    if ((flags & 8u) != 0u) {
        crypt315.key = r.u32();
        crypt315.subkey = r.u16();
        crypt315.prot_cur_address = r.u32();
        crypt315.dec_header = r.u32();
        crypt315.dec_hist = r.u16();
        crypt315.enc_ready = r.b();
        crypt315.first_read = r.b();
        crypt315.buffer_pos = r.i32();
        crypt315.line_buffer_pos = r.i32();
        crypt315.line_buffer_size = r.i32();
        crypt315.buffer_bit = r.i32();
        crypt315.buffer_bit2 = r.i32();
        crypt315.block_pos = r.i32();
        crypt315.block_size = r.i32();
        crypt315.block_numlines = r.i32();
        crypt315.done_compression = r.i32();
        crypt315.buffer2a = r.u16();
        r.bytes(crypt315.buffer2, 2u);
        crypt315.line_cur = r.u8() & 1u;
        r.bytes(crypt315.buffer, Stv3155881State::kBufferSize);
        r.bytes(crypt315.line_buf[0], Stv3155881State::kLineSize);
        r.bytes(crypt315.line_buf[1], Stv3155881State::kLineSize);
        for (int i = 0; i < 4; ++i) crypt315.a_bus[i] = r.u32();
        crypt315.abus_protenable = r.u32();
        crypt315.abus_protkey = r.u32();
    }

    if (g_active_m68k_machine != nullptr && g_active_m68k_machine != this) {
        switch_active_m68k(nullptr);
    } else if (g_active_m68k_machine == this) {
        g_active_m68k_machine = nullptr;
    }

    if (flags & 16u) {
        real_audio_enabled = r.b();
        m68k_enabled = r.b();
        m68k_in_reset = r.b();
        pending_sound_irq = r.b();
        m68k_debt = r.i32();
        audio_cyc_acc = r.u64();
        audio_last_sh2_cyc = r.u64();
        m68k_pc_cached = r.u32();
        audio_pcm_produced = r.u32();
        audio_pcm_nonzero = r.u32();

        scsp.reset();
        for (int i = 0; i < 32; ++i) {
            auto& s = scsp.slots[i];
            r.u16_arr(s.r, 16u);
            s.active = r.b();
            s.keyed = r.b();
            s.backwards = r.b();
            s.pos = static_cast<int64_t>(r.u64());
            s.eg_state = r.i32();
            s.eg_vol = r.i32();
            s.ar = r.i32();
            s.d1r = r.i32();
            s.d2r = r.i32();
            s.rr = r.i32();
            s.dl = r.i32();
            s.plfo.phase = r.u32();
            s.plfo.step = r.u32();
            s.plfo.table_idx = r.u8();
            s.plfo.scale_idx = r.u8();
            s.alfo.phase = r.u32();
            s.alfo.step = r.u32();
            s.alfo.table_idx = r.u8();
            s.alfo.scale_idx = r.u8();
        }
        r.u16_arr(scsp.com, 0x18u);
        for (int i = 0; i < 3; ++i) {
            scsp.timers[i].ctl = r.i32();
            scsp.timers[i].counter = r.i32();
            scsp.timers[i].reload = r.i32();
        }
        scsp.scieb = r.u32();
        scsp.scipd = r.u32();
        for (int i = 0; i < 3; ++i) scsp.scilv[i] = r.u32();
        scsp.mcieb = r.u32();
        scsp.mcipd = r.u32();
        scsp.main_line = r.b();
        scsp.samples = r.u32();
        for (int i = 0; i < 64; ++i) scsp.stack[i] = r.i32();
        scsp.stack_pos = r.u32();
        for (int i = 0; i < 64; ++i) scsp.dsp.coef[i] = static_cast<int16_t>(r.u16());
        r.u16_arr(scsp.dsp.madrs, 32u);
        r.u16_arr(scsp.dsp.mpro, 128u * 4u);
        for (int i = 0; i < 128; ++i) scsp.dsp.temp[i] = r.i32();
        for (int i = 0; i < 32; ++i) scsp.dsp.mems[i] = r.i32();
        for (int i = 0; i < 16; ++i) scsp.dsp.mixs[i] = r.i32();
        for (int i = 0; i < 2; ++i) scsp.dsp.exts[i] = static_cast<int16_t>(r.u16());
        for (int i = 0; i < 16; ++i) scsp.dsp.efreg[i] = static_cast<int16_t>(r.u16());
        scsp.dsp.dec = r.u32();
        scsp.dsp.last_step = r.i32();
        scsp.noise = r.u32();
        scsp.rbl = r.u32();
        scsp.rbp = r.u32();
        scsp.cpu_level = r.i32();

        const uint32_t ctx_len = r.u32();
        if (ctx_len > kMaxM68kCtxBytes) {
            return fail("snapshot_bad_m68k_ctx");
        }
        std::memset(m68k_ctx_blob, 0, sizeof(m68k_ctx_blob));
        m68k_ctx_size = ctx_len;
        if (ctx_len > 0u) {
            r.bytes(m68k_ctx_blob, ctx_len);
            if (m68k_enabled) {
                ensure_m68k_core_init();
                if (g_active_m68k_machine != nullptr && g_active_m68k_machine != this && g_active_m68k_machine->m68k_enabled) {
                    const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
                    if (sz <= kMaxM68kCtxBytes) {
                        g_active_m68k_machine->m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(g_active_m68k_machine->m68k_ctx_blob));
                    }
                }
                g_active_m68k_machine = this;
                m68k_set_context(m68k_ctx_blob);
                rebind_m68k_host_pointers();
                m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(m68k_ctx_blob));
            }
        }
    } else {
        if (g_active_m68k_machine == this) {
            g_active_m68k_machine = nullptr;
        }
        scsp.reset();
        m68k_enabled = false;
        m68k_in_reset = true;
        pending_sound_irq = false;
        m68k_debt = 0;
        audio_cyc_acc = 0u;
        audio_last_sh2_cyc = cpus[0].cycles;
        m68k_pc_cached = 0u;
        audio_pcm_produced = 0u;
        audio_pcm_nonzero = 0u;
        std::memset(m68k_ctx_blob, 0, sizeof(m68k_ctx_blob));
        m68k_ctx_size = 0u;
    }

    if ((flags & 32u) != 0u) {
        rule_hard_mode = r.b();
        rule_mode = r.u8();
        rule_value = r.u16();
        rule_goal_triggered = r.b();
        rule_goal_delay = r.u16();
    } else {
        rule_hard_mode = false;
        rule_mode = 0u;
        rule_value = 90u;
        rule_goal_triggered = false;
        rule_goal_delay = 0u;
    }

    if ((flags & 64u) != 0u) {
        copa_mode_active = r.b();
        copa_entered_gameplay = r.b();
        copa_returned_to_title = r.b();
        copa_ts_heartbeat = r.u8();
        copa_sp3_wind_timer = r.u32();
        copa_sp3_kick_count = r.u32();
        copa_ball_srca = r.u32();
        vdp.copa_vfx_replace = copa_mode_active;
        r.bytes(reinterpret_cast<uint8_t*>(&vdp.copa_sp_vfx), sizeof(vdp.copa_sp_vfx));
        r.bytes(reinterpret_cast<uint8_t*>(&vdp.copa_bracket), sizeof(vdp.copa_bracket));
    } else {
        copa_mode_active = false;
        copa_entered_gameplay = false;
        copa_returned_to_title = false;
        copa_ts_heartbeat = 0u;
        copa_sp3_wind_timer = 0u;
        copa_sp3_kick_count = 0u;
        copa_ball_srca = 0u;
        copa_vs_hook = nullptr;
        copa_opp_hook = nullptr;
        vdp.copa_vfx_replace = false;
        vdp.copa_group_badges = false;
        vdp.wind_vfx.active = false;
        vdp.copa_sp_vfx = StvVdpState::CopaSpVfxState{};
        vdp.copa_bracket = StvVdpState::CopaBracketState{};
    }

    if ((flags & 128u) != 0u) {
        coop.enabled = r.b();
        coop.p1_actor = r.u8();
        coop.p2_actor = r.u8();
        coop.last_passer_slot = r.u8();
        coop.prev_ball_owner = r.u8();
        coop.p1_switch_cd = r.u8();
        coop.p2_switch_cd = r.u8();
        coop.p1_auto_cd = r.u8();
        coop.p2_auto_cd = r.u8();
        coop.prev_pa_btn = r.u8();
        coop.prev_pb_btn = r.u8();
        coop.call_pass_timer[0] = r.u8();
        coop.call_pass_timer[1] = r.u8();
    } else {
        coop = CoopState{};
    }

    clear_audio_pcm();

    if (!r.ok() || r.pos() != payload_bytes) {
        return fail("snapshot_trailing_or_truncated");
    }

    apply_gk_ai_fix();
    coop::apply_coop_patches(*this);
    master_events_dirty = true;
    if (real_audio_enabled) {
        ensure_stage0_sound_seeded();
        if (!m68k_enabled &&
            (sound_ram[4] | sound_ram[5] | sound_ram[6] | sound_ram[7]) != 0u &&
            (smpc_pdr2_out & 0x10u) == 0u) {
            m68k_in_reset = false;
            start_m68k_unlocked();
        }
    }
    render_frame();
    return true;
}

namespace {

inline void copy_pages_diff(uint8_t* dst, const uint8_t* src, size_t n) {
#if defined(__wasm__)
    // In WebAssembly (-mbulk-memory), std::memcpy compiles to the single native `memory.copy`
    // instruction, whereas std::memcmp is a scalar Wasm loop over every word.
    std::memcpy(dst, src, n);
#else
    static constexpr size_t kPage = 4096u;
    size_t off = 0u;
    for (; off + kPage <= n; off += kPage) {
        if (std::memcmp(dst + off, src + off, kPage) != 0) {
            std::memcpy(dst + off, src + off, kPage);
        }
    }
    if (off < n && std::memcmp(dst + off, src + off, n - off) != 0) {
        std::memcpy(dst + off, src + off, n - off);
    }
#endif
}

static constexpr size_t kFastRbHeaderBytes = 16u;
static constexpr size_t kFastSoundRamBytes = StvScspState::kSoundRamMask + 1u;  // 512 KiB physical SCSP RAM
static constexpr size_t kFastVdp1FbStdBytes = 512u * 256u * sizeof(uint16_t);   // 256 KiB per plane (TVMR bit0 == 0)
static constexpr size_t kFastRbMinLargeBytes =
    kHighWramSize +
    kLowWramSize +
    kVdp2VramSize +
    StvVdpState::kVdp1VramSize +
    StvMachineState::kBackupRamSize +
    kFastSoundRamBytes;

}  // namespace

uint32_t StvMachineState::save_fast_rollback(uint8_t* dst, size_t dst_capacity) const {
    const bool fb_hi = (vdp.vdp1_regs[StvVdpState::TVMR] & 1u) != 0u;
    const bool has_v2w = inner.strict_vram_init;
    const bool skip_v1_fb = vdp.gpu_vdp1_enabled;
    const size_t fb_plane_bytes = skip_v1_fb
        ? 0u
        : (fb_hi ? (StvVdpState::kVdp1FbPlaneWords * sizeof(uint16_t)) : kFastVdp1FbStdBytes);
    const size_t large_bytes =
        kHighWramSize +
        kLowWramSize +
        kVdp2VramSize +
        (has_v2w ? kVdp2VramSize : 0u) +
        StvVdpState::kVdp1VramSize +
        fb_plane_bytes * 2u +
        kBackupRamSize +
        kFastSoundRamBytes;

    if (dst == nullptr || dst_capacity < (kFastRbHeaderBytes + large_bytes + 16384u)) {
        return 0u;
    }

    size_t off = kFastRbHeaderBytes;
    auto save_large = [&](const void* src_ptr, size_t sz) {
        copy_pages_diff(dst + off, static_cast<const uint8_t*>(src_ptr), sz);
        off += sz;
    };
    save_large(inner.high_wram.data(), inner.high_wram.size());
    save_large(inner.low_wram.data(), inner.low_wram.size());
    save_large(inner.vdp2_vram.data(), inner.vdp2_vram.size());
    if (has_v2w) {
        save_large(inner.vdp2_written.data(), inner.vdp2_written.size());
    }
    save_large(vdp.vdp1_vram, StvVdpState::kVdp1VramSize);
    if (!skip_v1_fb) {
        save_large(vdp.vdp1_fb[0], fb_plane_bytes);
        save_large(vdp.vdp1_fb[1], fb_plane_bytes);
    }
    save_large(backupram, kBackupRamSize);
    save_large(sound_ram, kFastSoundRamBytes);

    SnapWriter w(dst + off, dst_capacity - off);
    w.bytes(reinterpret_cast<const uint8_t*>(cpus), sizeof(cpus));
    w.b(inner.strict_vram_init);
    w.bytes(reinterpret_cast<const uint8_t*>(inner.divu.data()), sizeof(inner.divu));
    w.bytes(reinterpret_cast<const uint8_t*>(inner.frt.data()), sizeof(inner.frt));
    w.bytes(reinterpret_cast<const uint8_t*>(inner.dmac.data()), sizeof(inner.dmac));
    w.bytes(inner.ccr.data(), sizeof(inner.ccr));

    w.bytes(reinterpret_cast<const uint8_t*>(vdp.vdp1_regs), sizeof(vdp.vdp1_regs));
    w.i32(vdp.draw_fb);
    w.b(vdp.change_pending);
    w.b(vdp.erase_pending);
    w.b(vdp.erase_field);
    w.i32(vdp.sys_x);
    w.i32(vdp.sys_y);
    w.i32(vdp.ux0);
    w.i32(vdp.uy0);
    w.i32(vdp.ux1);
    w.i32(vdp.uy1);
    w.i32(vdp.local_x);
    w.i32(vdp.local_y);
    w.u64(vdp.frame_changes);
    w.u64(vdp.draws);
    w.b(vdp.vdp1_unsupported);
    w.bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    w.bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    w.i32(vdp.frame_w);
    w.i32(vdp.frame_h);

    const int obs_draw_n = std::clamp(vdp.v1_obs_draw_count, 0, StvVdpState::kMaxSpriteObs);
    const int obs_disp_n = std::clamp(vdp.v1_obs_disp_count, 0, StvVdpState::kMaxSpriteObs);
    w.i32(obs_draw_n);
    w.i32(obs_disp_n);
    if (obs_draw_n > 0) {
        w.bytes(reinterpret_cast<const uint8_t*>(vdp.v1_obs_draw),
                static_cast<size_t>(obs_draw_n) * sizeof(StvVdpState::SpriteObs));
    }
    if (obs_disp_n > 0) {
        w.bytes(reinterpret_cast<const uint8_t*>(vdp.v1_obs_disp),
                static_cast<size_t>(obs_disp_n) * sizeof(StvVdpState::SpriteObs));
    }
    w.bytes(reinterpret_cast<const uint8_t*>(&vdp.wind_vfx), sizeof(vdp.wind_vfx));
    w.b(vdp.copa_group_badges);

    w.b(has_bios);
    w.b(has_cart);
    w.b(cart_prot_enabled);
    const bool crypt_dirty =
        cart_prot_enabled || crypt315.prot_cur_address != 0u || crypt315.subkey != 0u ||
        crypt315.enc_ready || crypt315.first_read || crypt315.abus_protenable != 0u ||
        crypt315.abus_protkey != 0u || crypt315.a_bus[0] != 0u || crypt315.a_bus[1] != 0u ||
        crypt315.a_bus[2] != 0u || crypt315.a_bus[3] != 0u;
    w.b(crypt_dirty);
    if (crypt_dirty) {
        w.bytes(reinterpret_cast<const uint8_t*>(&crypt315), sizeof(crypt315));
    }

    w.bytes(scsp_regs, kScspRegsSize);
    w.bytes(reinterpret_cast<const uint8_t*>(scu_dma), sizeof(scu_dma));
    w.u32(scu_ims);
    w.u32(scu_ist);
    w.u32(scu_rsel);
    w.bytes(reinterpret_cast<const uint8_t*>(scu_dma_irq_armed), sizeof(scu_dma_irq_armed));
    w.bytes(reinterpret_cast<const uint8_t*>(scu_dma_irq_cycle), sizeof(scu_dma_irq_cycle));
    w.bytes(reinterpret_cast<const uint8_t*>(&dmac1), sizeof(dmac1));
    w.bytes(frt_tocr, sizeof(frt_tocr));
    w.bytes(reinterpret_cast<const uint8_t*>(frt_icr), sizeof(frt_icr));

    w.bytes(smpc_ireg, sizeof(smpc_ireg));
    w.bytes(smpc_oreg, sizeof(smpc_oreg));
    w.u8(smpc_sr);
    w.u8(smpc_sf);
    w.u8(smpc_pdr1_out);
    w.u8(smpc_pdr2_out);
    w.u8(smpc_ddr1);
    w.u8(smpc_ddr2);
    w.u8(smpc_gamebank_select);
    w.b(smpc_peri_pending);
    w.u64(smpc_irq_cycle);
    w.b(smpc_irq_armed);
    w.u64(rtc_base_cycles);

    w.bytes(reinterpret_cast<const uint8_t*>(&inputs), sizeof(inputs));
    w.u8(ioga_system_output);
    w.u8(ioga_mode);
    w.u8(ioga_portg);
    w.u8(ioga_billboard);
    w.u8(ioga_direction);

    w.u16(vdp2_tvstat);
    w.u16(vdp2_hcnt);
    w.u16(vdp2_vcnt);
    w.u64(next_vbi_cyc);
    w.u64(next_vbo_cyc);
    w.b(pending_vbi);
    w.b(pending_vbo);

    w.u32(rounds_done);
    w.u32(main_steps);
    w.u32(slave_steps);
    w.u32(vbi_count);
    w.u32(vbo_count);
    w.u32(sched2_hits);
    w.u32(minit_count);
    w.u32(sinit_count);
    w.u32(vdp1_vram_writes);
    w.u32(scu_dma_runs);
    w.u32(sh_dmac1_runs);
    w.u32(ioga_reads);
    w.u32(ioga_writes);
    w.u32(smpc_input_reads);
    w.u32(cart_reads);
    w.u32(soundram_reads);
    w.u32(soundram_writes);
    w.b(scsp_ack_armed);
    w.u64(scsp_ack_cycle);
    w.u32(last_write_val);
    w.u32(last_write_ea);

    w.b(halted);
    w.i32(halt_cpu);
    w.u32(last_fault.pc);
    w.u32(last_fault.address);
    w.u16(last_fault.opcode);
    w.u8(last_fault.is_write);
    w.u8(last_fault.width);
    w.b(slave_enabled);
    w.b(smpc_reset_master_pending);
    w.b(pending_nmi);

    const uint32_t mmio_n = (mmio_log_count <= kMaxMmioLog) ? mmio_log_count : kMaxMmioLog;
    w.u32(mmio_n);
    if (mmio_n > 0u) {
        w.bytes(reinterpret_cast<const uint8_t*>(mmio_log), static_cast<size_t>(mmio_n) * sizeof(MmioLogEntry));
    }
    const uint32_t ioga_n = (ioga_log_count <= kMaxIogaLog) ? ioga_log_count : kMaxIogaLog;
    w.u32(ioga_n);
    if (ioga_n > 0u) {
        w.bytes(reinterpret_cast<const uint8_t*>(ioga_log), static_cast<size_t>(ioga_n) * sizeof(IogaAccessEntry));
    }

    w.u32(scsp_event_seq);
    w.bytes(reinterpret_cast<const uint8_t*>(scsp_events), sizeof(scsp_events));
    w.u8(dbg_hide_layers);
    w.b(fix_p1_gk_ai);

    w.b(copa_mode_active);
    w.b(copa_entered_gameplay);
    w.b(copa_returned_to_title);
    w.u8(copa_ts_heartbeat);
    w.u32(copa_sp3_wind_timer);
    w.u32(copa_sp3_kick_count);
    w.u32(copa_ball_srca);
    if (copa_mode_active) {
        w.bytes(reinterpret_cast<const uint8_t*>(&vdp.copa_sp_vfx), sizeof(vdp.copa_sp_vfx));
        w.bytes(reinterpret_cast<const uint8_t*>(&vdp.copa_bracket), sizeof(vdp.copa_bracket));
    }

    const bool audio_dirty = real_audio_enabled || m68k_enabled;
    w.b(audio_dirty);
    if (audio_dirty) {
        w.b(real_audio_enabled);
        w.b(m68k_enabled);
        w.b(m68k_in_reset);
        w.b(pending_sound_irq);
        w.i32(m68k_debt);
        w.u64(audio_cyc_acc);
        w.u64(audio_last_sh2_cyc);
        w.u32(m68k_pc_cached);
        w.u32(audio_pcm_produced);
        w.u32(audio_pcm_nonzero);
        w.bytes(reinterpret_cast<const uint8_t*>(&scsp), sizeof(scsp));

        uint8_t ctx_tmp[kMaxM68kCtxBytes]{};
        uint32_t ctx_len = m68k_ctx_size;
        if (m68k_enabled) {
            if (g_active_m68k_machine == this) {
                const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
                if (sz <= kMaxM68kCtxBytes) {
                    ctx_len = static_cast<uint32_t>(m68k_get_context(ctx_tmp));
                }
            } else if (ctx_len > 0u && ctx_len <= kMaxM68kCtxBytes) {
                std::memcpy(ctx_tmp, m68k_ctx_blob, ctx_len);
            }
        } else {
            ctx_len = 0u;
        }
        w.u32(ctx_len);
        if (ctx_len > 0u) w.bytes(ctx_tmp, ctx_len);
    }

    const bool rule_dirty = rule_hard_mode || rule_mode != 0u || rule_goal_triggered || rule_goal_delay != 0u;
    if (rule_dirty) {
        w.b(rule_hard_mode);
        w.u8(rule_mode);
        w.u16(rule_value);
        w.b(rule_goal_triggered);
        w.u16(rule_goal_delay);
    }

    const bool coop_dirty = coop.enabled;
    if (coop_dirty) {
        w.b(coop.enabled);
        w.u8(coop.p1_actor);
        w.u8(coop.p2_actor);
        w.u8(coop.last_passer_slot);
        w.u8(coop.prev_ball_owner);
        w.u8(coop.p1_switch_cd);
        w.u8(coop.p2_switch_cd);
        w.u8(coop.p1_auto_cd);
        w.u8(coop.p2_auto_cd);
        w.u8(coop.prev_pa_btn);
        w.u8(coop.prev_pb_btn);
        w.u8(coop.call_pass_timer[0]);
        w.u8(coop.call_pass_timer[1]);
    }

    if (!w.ok()) return 0u;
    const uint32_t total_bytes = static_cast<uint32_t>(off + w.pos());
    const uint32_t layout_flags = (fb_hi ? 0x100u : 0u) | (has_v2w ? 0x200u : 0u) |
                                  (rule_dirty ? 0x400u : 0u) | (coop_dirty ? 0x800u : 0u) |
                                  (skip_v1_fb ? 0x1000u : 0u);
    write_le32_raw(dst + 0u, kFastRollbackMagic);
    write_le32_raw(dst + 4u, kFastRollbackVersion | layout_flags);
    write_le32_raw(dst + 8u, total_bytes);
    write_le32_raw(dst + 12u, rounds_done);
    return total_bytes;
}

bool StvMachineState::load_fast_rollback(const uint8_t* src, size_t len) {
    if (src == nullptr || len < (kFastRbHeaderBytes + kFastRbMinLargeBytes)) {
        return false;
    }
    const uint32_t magic = read_le32_raw(src + 0u);
    const uint32_t ver_word = read_le32_raw(src + 4u);
    const uint32_t version = ver_word & 0xFFu;
    const bool fb_hi = (ver_word & 0x100u) != 0u;
    const bool has_v2w = (ver_word & 0x200u) != 0u;
    const bool skip_v1_fb = (ver_word & 0x1000u) != 0u;
    const uint32_t total_bytes = read_le32_raw(src + 8u);
    if (magic != kFastRollbackMagic || version != kFastRollbackVersion ||
        static_cast<size_t>(total_bytes) != len) {
        return false;
    }

    const size_t fb_plane_bytes = skip_v1_fb
        ? 0u
        : (fb_hi ? (StvVdpState::kVdp1FbPlaneWords * sizeof(uint16_t)) : kFastVdp1FbStdBytes);
    const size_t large_bytes =
        kHighWramSize +
        kLowWramSize +
        kVdp2VramSize +
        (has_v2w ? kVdp2VramSize : 0u) +
        StvVdpState::kVdp1VramSize +
        fb_plane_bytes * 2u +
        kBackupRamSize +
        kFastSoundRamBytes;
    if (len < (kFastRbHeaderBytes + large_bytes)) {
        return false;
    }

    size_t off = kFastRbHeaderBytes;
    auto load_large = [&](void* dst_ptr, size_t sz) {
        copy_pages_diff(static_cast<uint8_t*>(dst_ptr), src + off, sz);
        off += sz;
    };
    load_large(inner.high_wram.data(), inner.high_wram.size());
    invalidate_all_aot_pages();
    load_large(inner.low_wram.data(), inner.low_wram.size());
    load_large(inner.vdp2_vram.data(), inner.vdp2_vram.size());
    if (has_v2w) {
        load_large(inner.vdp2_written.data(), inner.vdp2_written.size());
    }
    load_large(vdp.vdp1_vram, StvVdpState::kVdp1VramSize);
    if (!skip_v1_fb) {
        load_large(vdp.vdp1_fb[0], fb_plane_bytes);
        load_large(vdp.vdp1_fb[1], fb_plane_bytes);
    }
    load_large(backupram, kBackupRamSize);
    load_large(sound_ram, kFastSoundRamBytes);

    SnapReader r(src + off, len - off);
    r.bytes(reinterpret_cast<uint8_t*>(cpus), sizeof(cpus));
    inner.strict_vram_init = r.b();
    r.bytes(reinterpret_cast<uint8_t*>(inner.divu.data()), sizeof(inner.divu));
    r.bytes(reinterpret_cast<uint8_t*>(inner.frt.data()), sizeof(inner.frt));
    r.bytes(reinterpret_cast<uint8_t*>(inner.dmac.data()), sizeof(inner.dmac));
    r.bytes(inner.ccr.data(), sizeof(inner.ccr));

    r.bytes(reinterpret_cast<uint8_t*>(vdp.vdp1_regs), sizeof(vdp.vdp1_regs));
    vdp.draw_fb = r.i32();
    vdp.change_pending = r.b();
    vdp.erase_pending = r.b();
    vdp.erase_field = r.b();
    vdp.sys_x = r.i32();
    vdp.sys_y = r.i32();
    vdp.ux0 = r.i32();
    vdp.uy0 = r.i32();
    vdp.ux1 = r.i32();
    vdp.uy1 = r.i32();
    vdp.local_x = r.i32();
    vdp.local_y = r.i32();
    vdp.frame_changes = r.u64();
    vdp.draws = r.u64();
    vdp.vdp1_unsupported = r.b();
    r.bytes(vdp.vdp2_cram, StvVdpState::kVdp2CramSize);
    r.bytes(vdp.vdp2_regs, StvVdpState::kVdp2RegsSize);
    vdp.frame_w = r.i32();
    vdp.frame_h = r.i32();

    const int obs_draw_n = std::clamp(r.i32(), 0, StvVdpState::kMaxSpriteObs);
    const int obs_disp_n = std::clamp(r.i32(), 0, StvVdpState::kMaxSpriteObs);
    vdp.v1_obs_draw_count = obs_draw_n;
    vdp.v1_obs_disp_count = obs_disp_n;
    if (obs_draw_n > 0) {
        r.bytes(reinterpret_cast<uint8_t*>(vdp.v1_obs_draw),
                static_cast<size_t>(obs_draw_n) * sizeof(StvVdpState::SpriteObs));
    }
    if (obs_disp_n > 0) {
        r.bytes(reinterpret_cast<uint8_t*>(vdp.v1_obs_disp),
                static_cast<size_t>(obs_disp_n) * sizeof(StvVdpState::SpriteObs));
    }
    r.bytes(reinterpret_cast<uint8_t*>(&vdp.wind_vfx), sizeof(vdp.wind_vfx));
    vdp.copa_group_badges = r.b();

    (void)r.b();  // has_bios (immutable host ROM)
    (void)r.b();  // has_cart (immutable host ROM)
    cart_prot_enabled = r.b();
    const bool crypt_dirty = r.b();
    if (crypt_dirty) {
        r.bytes(reinterpret_cast<uint8_t*>(&crypt315), sizeof(crypt315));
    } else {
        crypt315.reset();
    }

    r.bytes(scsp_regs, kScspRegsSize);
    r.bytes(reinterpret_cast<uint8_t*>(scu_dma), sizeof(scu_dma));
    scu_ims = r.u32();
    scu_ist = r.u32();
    scu_rsel = r.u32();
    r.bytes(reinterpret_cast<uint8_t*>(scu_dma_irq_armed), sizeof(scu_dma_irq_armed));
    r.bytes(reinterpret_cast<uint8_t*>(scu_dma_irq_cycle), sizeof(scu_dma_irq_cycle));
    r.bytes(reinterpret_cast<uint8_t*>(&dmac1), sizeof(dmac1));
    r.bytes(frt_tocr, sizeof(frt_tocr));
    r.bytes(reinterpret_cast<uint8_t*>(frt_icr), sizeof(frt_icr));

    r.bytes(smpc_ireg, sizeof(smpc_ireg));
    r.bytes(smpc_oreg, sizeof(smpc_oreg));
    smpc_sr = r.u8();
    smpc_sf = r.u8();
    smpc_pdr1_out = r.u8();
    smpc_pdr2_out = r.u8();
    smpc_ddr1 = r.u8();
    smpc_ddr2 = r.u8();
    smpc_gamebank_select = r.u8();
    smpc_peri_pending = r.b();
    smpc_irq_cycle = r.u64();
    smpc_irq_armed = r.b();
    rtc_base_cycles = r.u64();

    r.bytes(reinterpret_cast<uint8_t*>(&inputs), sizeof(inputs));
    ioga_system_output = r.u8();
    ioga_mode = r.u8();
    ioga_portg = r.u8();
    ioga_billboard = r.u8();
    ioga_direction = r.u8();

    vdp2_tvstat = r.u16();
    vdp2_hcnt = r.u16();
    vdp2_vcnt = r.u16();
    next_vbi_cyc = r.u64();
    next_vbo_cyc = r.u64();
    pending_vbi = r.b();
    pending_vbo = r.b();

    rounds_done = r.u32();
    main_steps = r.u32();
    slave_steps = r.u32();
    vbi_count = r.u32();
    vbo_count = r.u32();
    sched2_hits = r.u32();
    minit_count = r.u32();
    sinit_count = r.u32();
    vdp1_vram_writes = r.u32();
    scu_dma_runs = r.u32();
    sh_dmac1_runs = r.u32();
    ioga_reads = r.u32();
    ioga_writes = r.u32();
    smpc_input_reads = r.u32();
    cart_reads = r.u32();
    soundram_reads = r.u32();
    soundram_writes = r.u32();
    scsp_ack_armed = r.b();
    scsp_ack_cycle = r.u64();
    last_write_val = r.u32();
    last_write_ea = r.u32();

    halted = r.b();
    halt_cpu = r.i32();
    last_fault.ok = !halted;
    last_fault.reason = halted ? "halted_in_snapshot" : nullptr;
    last_fault.pc = r.u32();
    last_fault.address = r.u32();
    last_fault.opcode = r.u16();
    last_fault.is_write = r.u8();
    last_fault.width = r.u8();
    slave_enabled = r.b();
    smpc_reset_master_pending = r.b();
    pending_nmi = r.b();

    const uint32_t mmio_n = r.u32();
    if (!r.ok() || mmio_n > kMaxMmioLog) return false;
    mmio_log_count = mmio_n;
    if (mmio_n > 0u) {
        r.bytes(reinterpret_cast<uint8_t*>(mmio_log), static_cast<size_t>(mmio_n) * sizeof(MmioLogEntry));
    }
    const uint32_t ioga_n = r.u32();
    if (!r.ok() || ioga_n > kMaxIogaLog) return false;
    ioga_log_count = ioga_n;
    if (ioga_n > 0u) {
        r.bytes(reinterpret_cast<uint8_t*>(ioga_log), static_cast<size_t>(ioga_n) * sizeof(IogaAccessEntry));
    }

    scsp_event_seq = r.u32();
    r.bytes(reinterpret_cast<uint8_t*>(scsp_events), sizeof(scsp_events));
    dbg_hide_layers = r.u8();
    fix_p1_gk_ai = r.b();

    copa_mode_active = r.b();
    copa_entered_gameplay = r.b();
    copa_returned_to_title = r.b();
    copa_ts_heartbeat = r.u8();
    copa_sp3_wind_timer = r.u32();
    copa_sp3_kick_count = r.u32();
    copa_ball_srca = r.u32();
    vdp.copa_vfx_replace = copa_mode_active;
    if (copa_mode_active) {
        r.bytes(reinterpret_cast<uint8_t*>(&vdp.copa_sp_vfx), sizeof(vdp.copa_sp_vfx));
        r.bytes(reinterpret_cast<uint8_t*>(&vdp.copa_bracket), sizeof(vdp.copa_bracket));
    } else {
        copa_mode_active = false;
        copa_entered_gameplay = false;
        copa_returned_to_title = false;
        copa_ts_heartbeat = 0u;
        copa_sp3_wind_timer = 0u;
        copa_sp3_kick_count = 0u;
        copa_ball_srca = 0u;
        copa_vs_hook = nullptr;
        copa_opp_hook = nullptr;
        vdp.copa_vfx_replace = false;
        vdp.copa_group_badges = false;
        vdp.wind_vfx.active = false;
        vdp.copa_sp_vfx = StvVdpState::CopaSpVfxState{};
        vdp.copa_bracket = StvVdpState::CopaBracketState{};
    }

    const bool audio_dirty = r.b();
    if (audio_dirty) {
        real_audio_enabled = r.b();
        m68k_enabled = r.b();
        m68k_in_reset = r.b();
        pending_sound_irq = r.b();
        m68k_debt = r.i32();
        audio_cyc_acc = r.u64();
        audio_last_sh2_cyc = r.u64();
        m68k_pc_cached = r.u32();
        audio_pcm_produced = r.u32();
        audio_pcm_nonzero = r.u32();
        r.bytes(reinterpret_cast<uint8_t*>(&scsp), sizeof(scsp));

        const uint32_t ctx_len = r.u32();
        if (ctx_len > kMaxM68kCtxBytes) return false;
        m68k_ctx_size = ctx_len;
        if (ctx_len > 0u) {
            r.bytes(m68k_ctx_blob, ctx_len);
            if (m68k_enabled) {
                ensure_m68k_core_init();
                if (g_active_m68k_machine != nullptr && g_active_m68k_machine != this && g_active_m68k_machine->m68k_enabled) {
                    const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
                    if (sz <= kMaxM68kCtxBytes) {
                        g_active_m68k_machine->m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(g_active_m68k_machine->m68k_ctx_blob));
                    }
                }
                g_active_m68k_machine = this;
                m68k_set_context(m68k_ctx_blob);
                rebind_m68k_host_pointers();
                m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(m68k_ctx_blob));
            }
        }
    } else {
        if (g_active_m68k_machine == this) {
            g_active_m68k_machine = nullptr;
        }
        scsp.reset();
        m68k_enabled = false;
        m68k_in_reset = true;
        pending_sound_irq = false;
        m68k_debt = 0;
        audio_cyc_acc = 0u;
        audio_last_sh2_cyc = cpus[0].cycles;
        m68k_pc_cached = 0u;
        audio_pcm_produced = 0u;
        audio_pcm_nonzero = 0u;
        std::memset(m68k_ctx_blob, 0, sizeof(m68k_ctx_blob));
        m68k_ctx_size = 0u;
    }

    if ((ver_word & 0x400u) != 0u) {
        rule_hard_mode = r.b();
        rule_mode = r.u8();
        rule_value = r.u16();
        rule_goal_triggered = r.b();
        rule_goal_delay = r.u16();
    } else {
        rule_hard_mode = false;
        rule_mode = 0u;
        rule_value = 90u;
        rule_goal_triggered = false;
        rule_goal_delay = 0u;
    }

    if ((ver_word & 0x800u) != 0u) {
        coop.enabled = r.b();
        coop.p1_actor = r.u8();
        coop.p2_actor = r.u8();
        coop.last_passer_slot = r.u8();
        coop.prev_ball_owner = r.u8();
        coop.p1_switch_cd = r.u8();
        coop.p2_switch_cd = r.u8();
        coop.p1_auto_cd = r.u8();
        coop.p2_auto_cd = r.u8();
        coop.prev_pa_btn = r.u8();
        coop.prev_pb_btn = r.u8();
        coop.call_pass_timer[0] = r.u8();
        coop.call_pass_timer[1] = r.u8();
    } else {
        coop = CoopState{};
    }

    master_events_dirty = true;
    return r.ok() && ((off + r.pos()) == len);
}

uint64_t StvMachineState::compute_sync_hash64() const {
    uint64_t h = 14695981039346656037ULL;
    auto mix64 = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    auto mix32 = [&](uint32_t v) {
        mix64(static_cast<uint64_t>(v));
    };

    for (int i = 0; i < 2; ++i) {
        const auto& c = cpus[i];
        for (int reg = 0; reg < 16; reg += 2) {
            mix64(static_cast<uint64_t>(c.r[reg]) | (static_cast<uint64_t>(c.r[reg + 1]) << 32));
        }
        mix64(static_cast<uint64_t>(c.pc) | (static_cast<uint64_t>(c.pr) << 32));
        mix64(static_cast<uint64_t>(c.gbr) | (static_cast<uint64_t>(c.vbr) << 32));
        mix64(static_cast<uint64_t>(c.mach) | (static_cast<uint64_t>(c.macl) << 32));
        mix64(static_cast<uint64_t>(c.sr) | (static_cast<uint64_t>(c.delay) << 32));
        mix64(c.retired);
        mix64(c.cycles);
    }

    const uint8_t* hw = inner.high_wram.data();
    const size_t hw_sz = inner.high_wram.size();
    for (size_t off = 0u; off < hw_sz; off += 8u) {
        mix64(read_le64_raw(hw + off));
    }

    for (int i = 0; i < 16; i += 2) {
        mix32(static_cast<uint32_t>(vdp.vdp1_regs[i]) | (static_cast<uint32_t>(vdp.vdp1_regs[i + 1]) << 16));
    }
    for (size_t off = 0u; off < StvVdpState::kVdp2RegsSize; off += 8u) {
        mix64(read_le64_raw(vdp.vdp2_regs + off));
    }

    mix64(static_cast<uint64_t>(rounds_done) | (static_cast<uint64_t>(vbi_count) << 32));
    mix64(static_cast<uint64_t>(vbo_count) | (static_cast<uint64_t>(scu_ist) << 32));
    mix32(static_cast<uint32_t>(inputs.port_a) |
          (static_cast<uint32_t>(inputs.port_b) << 8) |
          (static_cast<uint32_t>(inputs.port_c) << 16) |
          (static_cast<uint32_t>(copa_mode_active ? 1u : 0u) << 24) |
          (static_cast<uint32_t>(rule_hard_mode ? 1u : 0u) << 25));
    if (rule_hard_mode || rule_mode != 0u) {
        mix32(static_cast<uint32_t>(rule_mode) |
              (static_cast<uint32_t>(rule_value) << 8) |
              (static_cast<uint32_t>(rule_goal_triggered ? 1u : 0u) << 24));
        mix32(static_cast<uint32_t>(rule_goal_delay));
    }
    if (copa_mode_active) {
        const auto* cb = reinterpret_cast<const uint8_t*>(&vdp.copa_bracket);
        const size_t cb_sz = sizeof(vdp.copa_bracket);
        for (size_t off = 0u; off + 8u <= cb_sz; off += 8u) {
            mix64(read_le64_raw(cb + off));
        }
        for (size_t off = cb_sz & ~size_t{7u}; off < cb_sz; ++off) {
            mix32(cb[off]);
        }
    }
    if (coop.enabled) {
        mix32(static_cast<uint32_t>(coop.p1_actor) |
              (static_cast<uint32_t>(coop.p2_actor) << 8) |
              (static_cast<uint32_t>(coop.last_passer_slot) << 16) |
              (static_cast<uint32_t>(coop.prev_ball_owner) << 24));
        mix32(static_cast<uint32_t>(coop.p1_switch_cd) |
              (static_cast<uint32_t>(coop.p2_switch_cd) << 8) |
              (static_cast<uint32_t>(coop.p1_auto_cd) << 16) |
              (static_cast<uint32_t>(coop.p2_auto_cd) << 24));
        mix32(static_cast<uint32_t>(coop.prev_pa_btn) |
              (static_cast<uint32_t>(coop.prev_pb_btn) << 8) |
              (static_cast<uint32_t>(coop.call_pass_timer[0]) << 16) |
              (static_cast<uint32_t>(coop.call_pass_timer[1]) << 24));
    }
    return h;
}

bool StvMachineState::revalidate_aot_page(uint32_t line_idx) noexcept {
    if (line_idx >= kAotNumLines) return false;
    if (__builtin_expect(!aot_pages_initialized, 0)) {
        invalidate_all_aot_pages();
    }
    const uint32_t wi = line_idx >> 5;
    const uint32_t mask = 1u << (line_idx & 31u);
    if ((code_dirty_bits[wi] & mask) == 0u) {
        return (aot_page_valid_bits[wi] & mask) != 0u;
    }
    code_dirty_bits[wi] &= ~mask;
    const uint32_t expected = sh2_aot_expected_line_hash(line_idx);
    if (expected == 0u) {
        aot_page_valid_bits[wi] &= ~mask;
        return false;
    }
    const uint8_t* p = inner.high_wram.data() + (line_idx << kAotLineShift);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < kAotLineSize; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    if (h == 0u) h = 1u;
    if (h == expected) {
        aot_page_valid_bits[wi] |= mask;
        return true;
    }
    aot_page_valid_bits[wi] &= ~mask;
    return false;
}

void StvMachineState::apply_gk_ai_fix() noexcept {
    auto rd16 = [this](uint32_t ea) -> uint16_t {
        const uint32_t off = ea - kHighWramBase;
        return static_cast<uint16_t>((static_cast<uint16_t>(inner.high_wram[off]) << 8) |
                                     inner.high_wram[off + 1u]);
    };
    auto wr16 = [this](uint32_t ea, uint16_t val) {
        const uint32_t off = ea - kHighWramBase;
        const uint8_t hi = static_cast<uint8_t>((val >> 8) & 0xFFu);
        const uint8_t lo = static_cast<uint8_t>(val & 0xFFu);
        if (inner.high_wram[off] == hi && inner.high_wram[off + 1u] == lo) {
            return;
        }
        inner.high_wram[off] = hi;
        inner.high_wram[off + 1u] = lo;
        mark_high_wram_dirty(off, 2u);
    };

    // Guard: ensure twcup98 EPRG match code is decrypted & resident in High WRAM (safe during BIOS cold boot).
    if (rd16(0x06035D28u) != 0xD334u || rd16(0x06032DDEu) != 0x2FE6u || rd16(0x06032F00u) != 0xA001u) {
        return;
    }

    // 0. Always restore the 11 GetTeamPossessionRelation (0x060237D2) call sites if any stale snapshot
    //    holds 0xE000 (mov #0, r0). Note: 0x060237D2 returns 0=own-team, 1=opponent, 2=ball-carrier,
    //    NOT IsHumanTeam! Forcing r0=0 blinded GK shot evaluation (0x0603597E) and dive calculators.
    static constexpr uint32_t kGkPossessionCheckAddrs[11] = {
        0x06033A00u, 0x06034280u, 0x060342DEu, 0x060346EAu, 0x06034870u, 0x06035C12u,
        0x06035C76u, 0x06035DB8u, 0x06035EF6u, 0x0603642Cu, 0x060367B2u,
    };
    for (uint32_t ea : kGkPossessionCheckAddrs) {
        if (rd16(ea) == 0xE000u && rd16(ea + 2u) == 0x8801u) {
            wr16(ea, 0x600Du);
        }
    }

    // 1. 0x06035D2E inside 0x06035CF2 (after jsr 0x06027F2C IsCpuTeam):
    //    Original 0x600E (exts.b r0, r0) skips automatic 1v1 rush-out for human teams.
    //    Fix 0xE001 (mov #1, r0) enables identical automatic 1v1 rush-out AI for human & CPU GKs.
    if ((rd16(0x06035D2Eu) == 0x600Eu || rd16(0x06035D2Eu) == 0xE001u) && rd16(0x06035D30u) == 0x8801u) {
        wr16(0x06035D2Eu, fix_p1_gk_ai ? 0xE001u : 0x600Eu);
    }

    // 2. 0x06032F04 inside 0x06032DDE (called by 0x0603597E before dive evaluation 0x060320C0/0x06032F28):
    //    Original 0x6E93 (mov r9, r14) returns 1 (stay in 1A:01) when lateral shuffle 3 is not chosen,
    //    causing 0x0603597E at 0x06035A08 to skip 1A:0B dive / 1A:1C reflex save on normal shots!
    //    Fix 0x0009 (nop) preserves r14=0 so 0x0603597E evaluates dive/reflex saves normally.
    if (rd16(0x06032F04u) == 0x6E93u || rd16(0x06032F04u) == 0x0009u) {
        wr16(0x06032F04u, fix_p1_gk_ai ? 0x0009u : 0x6E93u);
    }

    // 3. 0x06035A84..0x06035A86 inside 0x06035A70 (1A:02 "kj_ball_run" evaluator):
    //    Original bt/s 0x06035AA0; mov #0, r6 (0x8D0C, 0xE600) keeps GK stuck in 1A:02 during ball_action==1
    //    after an opponent pass is received. nop; nop exits 1A:02 immediately back to 1A:01 ready stance.
    if (rd16(0x06035A82u) == 0x8801u &&
        ((rd16(0x06035A84u) == 0x8D0Cu && rd16(0x06035A86u) == 0xE600u) ||
         (rd16(0x06035A84u) == 0x0009u && rd16(0x06035A86u) == 0x0009u))) {
        wr16(0x06035A84u, fix_p1_gk_ai ? 0x0009u : 0x8D0Cu);
        wr16(0x06035A86u, fix_p1_gk_ai ? 0x0009u : 0xE600u);
    }

    // 4. 0x060305AC inside 0x060303C4 (1A:0F GK catch-and-stand-up recovery):
    //    After jsr 0x06033DC0(actor, 1) (returns 1 when dist_sq <= 100 and |angle_diff| <= 0x1000),
    //    original cmp/eq #1, r0; bt 0x0603059E (0x89F7) loops FOREVER when |GK.X| <= 1472.0 (where
    //    0x06030594 sets target = pos so 0x06033DC0 returns 1 immediately), softlocking GK and team input.
    //    Fix 0x8BF7 (bf 0x0603059E) waits while NOT arrived (r0 != 1) and exits once arrived (r0 == 1).
    if (rd16(0x060305AAu) == 0x8801u &&
        (rd16(0x060305ACu) == 0x89F7u || rd16(0x060305ACu) == 0x8BF7u)) {
        wr16(0x060305ACu, fix_p1_gk_ai ? 0x8BF7u : 0x89F7u);
    }

    // 5. 0x0602FEB8 inside 0x0602FE60 (identical inverted-branch wait loop on jsr 0x06033DC0(actor, 1)):
    //    Original cmp/eq #1, r0; bt 0x0602FEAC (0x89F8) loops forever once arrived; fix 0x8BF8 (bf 0x0602FEAC).
    if (rd16(0x0602FEB6u) == 0x8801u &&
        (rd16(0x0602FEB8u) == 0x89F8u || rd16(0x0602FEB8u) == 0x8BF8u)) {
        wr16(0x0602FEB8u, fix_p1_gk_ai ? 0x8BF8u : 0x89F8u);
    }
}

void StvMachineState::set_gk_ai_fix(bool enabled) {
    fix_p1_gk_ai = enabled;
    apply_gk_ai_fix();
}

void StvMachineState::set_coop_mode(bool enabled) {
    coop::set_coop_mode(*this, enabled);
}

void StvMachineState::clear_audio_pcm() {
    audio_ring_r = 0u;
    audio_ring_w = 0u;
    audio_ring_count = 0u;
}

uint32_t StvMachineState::consume_audio_pcm(int16_t* dst_stereo, uint32_t max_frames) {
    if (dst_stereo == nullptr || max_frames == 0u || audio_ring_count == 0u) return 0u;
    const uint32_t n = (max_frames < audio_ring_count) ? max_frames : audio_ring_count;
    for (uint32_t i = 0u; i < n; ++i) {
        const uint32_t rpos = audio_ring_r * 2u;
        dst_stereo[i * 2u] = audio_ring[rpos];
        dst_stereo[i * 2u + 1u] = audio_ring[rpos + 1u];
        audio_ring_r = (audio_ring_r + 1u) % kMaxAudioRingFrames;
    }
    audio_ring_count -= n;
    return n;
}

void StvMachineState::start_m68k_unlocked() {
    ensure_m68k_core_init();
    if (g_active_m68k_machine != nullptr && g_active_m68k_machine != this && g_active_m68k_machine->m68k_enabled) {
        const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
        if (sz <= kMaxM68kCtxBytes) {
            g_active_m68k_machine->m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(g_active_m68k_machine->m68k_ctx_blob));
        }
    }
    g_active_m68k_machine = this;
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_irq(static_cast<unsigned int>(scsp.m68k_irq_level()));
    m68k_debt = 0;
    m68k_enabled = true;
    scsp_ack_armed = false;
    m68k_pc_cached = static_cast<uint32_t>(m68k_get_reg(nullptr, M68K_REG_PC));
    const uint32_t sz = static_cast<uint32_t>(m68k_get_context(nullptr));
    if (sz <= kMaxM68kCtxBytes) {
        m68k_ctx_size = static_cast<uint32_t>(m68k_get_context(m68k_ctx_blob));
    }
}

void StvMachineState::ensure_stage0_sound_seeded() {
    // endpoint_frame8175.cp omits sound_ram because Title->Match 1 audio init (0x06009884(stage=0))
    // ran prior to frame 8175. If sound_ram vectors [4..7] are empty and cart_rom is loaded,
    // seed SG_DRV.DAT + SE_00.BIN + SE_00.SEQ + Area Map ($A000/$0500) cleanly so M68K/SCSP
    // runs from frame 0 onward instead of waiting until Match 2 / VS screen.
    if (!has_cart || (sound_ram[4] | sound_ram[5] | sound_ram[6] | sound_ram[7]) != 0u) {
        return;
    }
    static constexpr uint32_t kSgDrvRomOff = 0x00DC9664u;
    static constexpr uint32_t kSgDrvSize = 0x00005D70u;
    static constexpr uint32_t kSe00BinRomOff = 0x00DCFDE8u;
    static constexpr uint32_t kSe00BinSize = 0x00071866u;
    static constexpr uint32_t kSe00SeqRomOff = 0x00DCF3D4u;
    static constexpr uint32_t kSe00SeqSize = 0x00000A14u;
    if (kSgDrvRomOff + kSgDrvSize > kCartPopulatedSize ||
        kSe00BinRomOff + kSe00BinSize > kCartPopulatedSize ||
        kSe00SeqRomOff + kSe00SeqSize > kCartPopulatedSize ||
        0x0007C866u + kSe00SeqSize > kSoundRamSize) {
        return;
    }
    std::memcpy(sound_ram + 0x000000u, cart_rom + kSgDrvRomOff, kSgDrvSize);
    std::memcpy(sound_ram + 0x00B000u, cart_rom + kSe00BinRomOff, kSe00BinSize);
    std::memcpy(sound_ram + 0x07C866u, cart_rom + kSe00SeqRomOff, kSe00SeqSize);
    static constexpr uint8_t kAreaMapA000[20] = {
        0x00u, 0x00u, 0xB0u, 0x00u, 0x00u, 0x07u, 0x18u, 0x66u,
        0x10u, 0x07u, 0xC8u, 0x66u, 0x00u, 0x00u, 0x0Au, 0x14u,
        0xFFu, 0xFFu, 0x00u, 0x00u,
    };
    static constexpr uint8_t kAreaMap0500[24] = {
        0x00u, 0x00u, 0xB0u, 0x00u, 0x80u, 0x07u, 0x18u, 0x66u,
        0x10u, 0x07u, 0xC8u, 0x66u, 0x80u, 0x00u, 0x0Au, 0x14u,
        0xFFu, 0xFFu, 0x00u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u,
    };
    std::memcpy(sound_ram + 0x00A000u, kAreaMapA000, sizeof(kAreaMapA000));
    std::memcpy(sound_ram + 0x000500u, kAreaMap0500, sizeof(kAreaMap0500));
    sound_ram[0x04E1u] = 0x80u;  // SG_DRV command handshake enable (written at 0x0605D508)
    sound_ram[0x0740u] = 0x0Du;  // Match 0x0605D4EA init state
    if (scsp_regs[0x400u] == 0u && scsp_regs[0x401u] == 0u) {
        scsp_regs[0x400u] = 0x02u;
        scsp_regs[0x401u] = 0x0Fu;  // MVOL = 15 (DAC18B=0, MEM4MB=1)
        scsp.write(0x400u, 0x020Fu, 2, sound_ram);
    }
    smpc_pdr2_out &= static_cast<uint8_t>(~0x10u);
}

void StvMachineState::set_real_audio(bool enabled) {
    if (real_audio_enabled == enabled) return;
    real_audio_enabled = enabled;
    audio_last_sh2_cyc = cpus[0].cycles;
    audio_cyc_acc = 0u;
    clear_audio_pcm();
    if (!enabled) {
        return;
    }
    // Sync initial SCSP words from SH-2 scsp_regs[] if any were written before enabling.
    for (uint32_t off = 0u; off < 0x1000u; off += 2u) {
        const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(scsp_regs[off]) << 8) | scsp_regs[off + 1u]);
        if (v != 0u) {
            scsp.write(off, v, 2, sound_ram);
        }
    }
    ensure_stage0_sound_seeded();
    if ((smpc_pdr2_out & 0x10u) == 0u &&
        (sound_ram[4] | sound_ram[5] | sound_ram[6] | sound_ram[7]) != 0u &&
        !m68k_enabled) {
        m68k_in_reset = false;
        start_m68k_unlocked();
    }
}

void StvMachineState::handle_smpc_pdr2_write(uint8_t prev, uint8_t cur) {
    (void)prev;
    if ((cur & 0x10u) != 0u) {
        m68k_in_reset = true;
        m68k_enabled = false;
        m68k_debt = 0;
        if (g_active_m68k_machine == this) {
            g_active_m68k_machine = nullptr;
        }
        return;
    }
    m68k_in_reset = false;
    const bool has_vectors = (sound_ram[4] | sound_ram[5] | sound_ram[6] | sound_ram[7]) != 0u;
    if (real_audio_enabled && has_vectors && !m68k_enabled) {
        start_m68k_unlocked();
    }
}

void StvMachineState::step_audio_samples(uint32_t num_samples) {
    if (!real_audio_enabled || num_samples == 0u) return;
    if (m68k_enabled) {
        switch_active_m68k(this);
    }
    for (uint32_t s = 0u; s < num_samples; ++s) {
        if (m68k_enabled) {
            m68k_debt += 256;
            if (m68k_debt > 0) {
                const int ran = m68k_execute(m68k_debt);
                m68k_debt -= ran;
            }
            m68k_set_irq(static_cast<unsigned int>(scsp.m68k_irq_level()));
        }
        int16_t lr[2]{};
        bool main_rise = false;
        scsp.step_sample(sound_ram, lr, main_rise);
        if (main_rise) {
            pending_sound_irq = true;
            master_events_dirty = true;
            scu_ist |= 0x0040u;
        }
        if (emit_audio_ring_) {
            const uint32_t wpos = audio_ring_w * 2u;
            audio_ring[wpos] = lr[0];
            audio_ring[wpos + 1u] = lr[1];
            audio_ring_w = (audio_ring_w + 1u) % kMaxAudioRingFrames;
            if (audio_ring_count < kMaxAudioRingFrames) {
                ++audio_ring_count;
            } else {
                audio_ring_r = (audio_ring_r + 1u) % kMaxAudioRingFrames;
            }
        }
        ++audio_pcm_produced;
        if (lr[0] != 0 || lr[1] != 0) ++audio_pcm_nonzero;
    }
    if (m68k_enabled) {
        m68k_pc_cached = static_cast<uint32_t>(m68k_get_reg(nullptr, M68K_REG_PC));
    }
}

void StvMachineState::step_audio_tick(uint64_t sh2_cycles) {
    if (!real_audio_enabled) {
        audio_last_sh2_cyc = sh2_cycles;
        return;
    }
    if (sh2_cycles <= audio_last_sh2_cyc) {
        audio_last_sh2_cyc = sh2_cycles;
        return;
    }
    const uint64_t delta = sh2_cycles - audio_last_sh2_cyc;
    audio_last_sh2_cyc = sh2_cycles;
    audio_cyc_acc += delta * 49ULL;
    if (audio_cyc_acc < 30000ULL) return;
    const uint32_t samples = static_cast<uint32_t>(audio_cyc_acc / 30000ULL);
    audio_cyc_acc %= 30000ULL;
    step_audio_samples(samples);
}

}  // namespace twc98
