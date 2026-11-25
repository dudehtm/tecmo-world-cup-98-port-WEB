// Standalone zero-import WebAssembly export surface wrapping StvMachineState.
// Compiled identically with the shared portable core under -DTWC98_NO_STL.
#include "stv_machine.hpp"
#include "twc98_copa_mod.hpp"
#include "twc98_net_session.hpp"

#include <cstddef>
#include <cstdint>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

extern "C" {

void* memset(void* dst, int c, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    const uint8_t b = static_cast<uint8_t>(c);
    for (size_t i = 0u; i < n; ++i) {
        p[i] = b;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0u; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0u; i < n; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0u; --i) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* a = static_cast<const uint8_t*>(s1);
    const uint8_t* b = static_cast<const uint8_t*>(s2);
    size_t i = 0u;
#if defined(__wasm_simd128__)
    for (; i + 16u <= n; i += 16u) {
        const v128_t va = wasm_v128_load(a + i);
        const v128_t vb = wasm_v128_load(b + i);
        const v128_t ne = wasm_i8x16_ne(va, vb);
        if (wasm_v128_any_true(ne)) {
            for (size_t k = 0u; k < 16u; ++k) {
                if (a[i + k] != b[i + k]) {
                    return static_cast<int>(a[i + k]) - static_cast<int>(b[i + k]);
                }
            }
        }
    }
#endif
    for (; i < n; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0u;
    if (s) {
        while (s[n] != '\0') ++n;
    }
    return n;
}

int setjmp(void*) {
    return 0;
}

void longjmp(void*, int) {
    __builtin_trap();
}

}  // extern "C"

void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}

namespace {

twc98::StvMachineState g_machine;
uint8_t g_cp_buf[78u * 1024u * 1024u];
static_assert(sizeof(g_cp_buf) >= twc98::NetSession::kRequiredArenaBytes, "g_cp_buf must fit NetSession rollback arena");
uint8_t g_bios_buf[twc98::StvMachineState::kBiosSize]{};
uint8_t g_vdp1_vram_buf[twc98::StvVdpState::kVdp1VramSize]{};
uint8_t g_vdp1_regs_buf[0x20u]{};
uint8_t g_vdp2_vram_buf[twc98::kVdp2VramSize]{};
uint8_t g_vdp2_cram_buf[twc98::StvVdpState::kVdp2CramSize]{};
uint8_t g_vdp2_regs_buf[twc98::StvVdpState::kVdp2RegsSize]{};
uint8_t g_digests_buf[7u * 32u]{};
uint32_t g_cpu_snapshot_buf[2u * 28u]{};
uint8_t g_state_buf[twc98::StvMachineState::kMaxSnapshotBytes]{};
uint32_t g_frame_rgba_buf[twc98::StvVdpState::kMaxFrameW * twc98::StvVdpState::kMaxFrameH]{};

twc98::copa::CopaModBackup g_copa_backup{};
twc98::NetSession g_net_session{};
uint8_t g_net_pkt_buf[twc98::NetInputPacket::kWireBytes]{};
int16_t g_audio_pcm_buf[4096u * 2u]{};
alignas(16) uint8_t g_render_pkt_buf[twc98::StvMachineState::kMaxRenderPktBytes]{};
alignas(16) uint8_t g_gpu_vdp2_prev_vram[twc98::kVdp2VramSize]{};
bool g_gpu_vdp2_vram_valid = false;
alignas(16) uint8_t g_gpu_vdp1_prev_vram[twc98::StvVdpState::kVdp1VramSize]{};
bool g_gpu_vdp1_vram_valid = false;

}  // namespace

#define WASM_EXPORT __attribute__((visibility("default")))

extern "C" {

WASM_EXPORT uint8_t* wasm_cp_ptr() { return g_cp_buf; }
WASM_EXPORT uint32_t wasm_cp_capacity() { return static_cast<uint32_t>(sizeof(g_cp_buf)); }
WASM_EXPORT uint8_t* wasm_bios_ptr() { return g_bios_buf; }
WASM_EXPORT uint8_t* wasm_cart_ptr() { return g_machine.cart_rom; }
WASM_EXPORT uint32_t wasm_cart_capacity() { return twc98::StvMachineState::kCartPopulatedSize; }
WASM_EXPORT uint32_t wasm_init_cart(uint32_t len) {
    g_machine.mark_cart_loaded(len);
    return g_machine.has_cart ? 1u : 0u;
}
WASM_EXPORT void wasm_clear_cart() { g_machine.clear_cart_rom(); }
WASM_EXPORT uint32_t wasm_has_cart() { return g_machine.has_cart ? 1u : 0u; }
WASM_EXPORT void wasm_machine_reset() {
    g_machine.reset();
    std::memset(&g_copa_backup, 0, sizeof(g_copa_backup));
    g_copa_backup.tournament = twc98::copa::CopaTournamentState{};
    g_copa_backup.match_rules = twc98::copa::MatchRulesConfig{};
    g_gpu_vdp2_vram_valid = false;
    g_gpu_vdp1_vram_valid = false;
}
WASM_EXPORT uint8_t* wasm_vdp1_vram_ptr() { return g_vdp1_vram_buf; }
WASM_EXPORT uint8_t* wasm_vdp1_regs_ptr() { return g_vdp1_regs_buf; }
WASM_EXPORT uint8_t* wasm_vdp2_vram_ptr() { return g_vdp2_vram_buf; }
WASM_EXPORT uint8_t* wasm_vdp2_cram_ptr() { return g_vdp2_cram_buf; }
WASM_EXPORT uint8_t* wasm_vdp2_regs_ptr() { return g_vdp2_regs_buf; }
WASM_EXPORT uint8_t* wasm_digests_ptr() { return g_digests_buf; }
WASM_EXPORT uint32_t* wasm_cpu_snapshot_ptr() { return g_cpu_snapshot_buf; }
WASM_EXPORT const twc98::MmioLogEntry* wasm_mmio_log_ptr() { return g_machine.mmio_log; }
WASM_EXPORT const uint32_t* wasm_frame_px_ptr() { return g_machine.vdp.frame_px; }

WASM_EXPORT uint8_t* wasm_state_ptr() { return g_state_buf; }
WASM_EXPORT uint32_t wasm_state_capacity() { return static_cast<uint32_t>(sizeof(g_state_buf)); }
WASM_EXPORT uint32_t wasm_save_state() {
    return g_machine.save_state_bytes(g_state_buf, sizeof(g_state_buf));
}
WASM_EXPORT uint32_t wasm_load_state(uint32_t len) {
    if (len > sizeof(g_state_buf)) return 0u;
    const char* err = nullptr;
    g_gpu_vdp2_vram_valid = false;
    g_gpu_vdp1_vram_valid = false;
    return g_machine.load_state_bytes(g_state_buf, len, &err) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_init_cpn8(uint32_t cp_len, uint32_t bios_len, uint32_t seed_vdp_flag) {
    if (cp_len > sizeof(g_cp_buf)) return 0u;
    const uint8_t* bios = (bios_len == twc98::StvMachineState::kBiosSize) ? g_bios_buf : nullptr;
    const char* err = nullptr;
    std::memset(&g_copa_backup, 0, sizeof(g_copa_backup));
    g_copa_backup.tournament = twc98::copa::CopaTournamentState{};
    g_copa_backup.match_rules = twc98::copa::MatchRulesConfig{};
    g_gpu_vdp2_vram_valid = false;
    g_gpu_vdp1_vram_valid = false;
    if (!g_machine.init_from_cpn8_bytes(g_cp_buf, cp_len, bios, bios_len, &err)) {
        return 0u;
    }
    if (seed_vdp_flag != 0u) {
        g_machine.seed_vdp_snapshot(
            g_vdp1_vram_buf, sizeof(g_vdp1_vram_buf),
            g_vdp1_regs_buf, sizeof(g_vdp1_regs_buf),
            g_vdp2_vram_buf, sizeof(g_vdp2_vram_buf),
            g_vdp2_cram_buf, sizeof(g_vdp2_cram_buf),
            g_vdp2_regs_buf, sizeof(g_vdp2_regs_buf));
    }
    return 1u;
}

WASM_EXPORT uint32_t wasm_run_single_cpu(uint32_t cpu_id, uint32_t max_steps) {
    if (cpu_id > 1u) return 0u;
    twc98::BusAccessFault fault{};
    uint32_t steps = 0u;
    while (steps < max_steps) {
        if (!twc98::step_sh2(static_cast<int>(cpu_id), g_machine.cpus[cpu_id], g_machine.inner, fault)) {
            g_machine.halted = true;
            g_machine.halt_cpu = static_cast<int>(cpu_id);
            g_machine.last_fault = fault;
            break;
        }
        ++steps;
    }
    return steps;
}

WASM_EXPORT void wasm_seed_chain13_frt_baseline() {
    auto& f = g_machine.inner.frt[1];
    f.ftcsr = 0x80u;
    f.tier = 0x01u;
    f.tcr = 0x00u;
    f.ocra = 0x0000u;
    f.ocrb = 0x0000u;
    f.frc = 0xF830u;
    f.frc_base = 0xD9F99198ULL;
    f.activate();
    g_machine.cpus[1].cycles = 0xD9F9A0E6ULL;
}

WASM_EXPORT uint32_t wasm_run_interleaved_steps(uint32_t steps_c0, uint32_t steps_c1) {
    twc98::BusAccessFault fault{};
    uint32_t rem[2] = {steps_c0, steps_c1};
    while (rem[0] > 0u || rem[1] > 0u) {
        for (int cpu_id = 0; cpu_id < 2; ++cpu_id) {
            const uint32_t n = (rem[cpu_id] < 32u) ? rem[cpu_id] : 32u;
            for (uint32_t k = 0u; k < n; ++k) {
                if (cpu_id == 1 && g_machine.cpus[1].retired == 134671u) {
                    wasm_seed_chain13_frt_baseline();
                }
                if (!twc98::step_sh2(cpu_id, g_machine.cpus[cpu_id], g_machine.inner, fault)) {
                    g_machine.halted = true;
                    g_machine.halt_cpu = cpu_id;
                    g_machine.last_fault = fault;
                    return 0u;
                }
                --rem[cpu_id];
            }
        }
    }
    return 1u;
}

WASM_EXPORT uint32_t wasm_step_rounds(uint32_t num_rounds) {
    return g_machine.run_rounds(num_rounds) ? 1u : 0u;
}

WASM_EXPORT void wasm_render_frame() {
    g_machine.render_frame();
}

WASM_EXPORT void wasm_copy_frame_rgba() {
    const int32_t w = g_machine.vdp.frame_w;
    const int32_t h = g_machine.vdp.frame_h;
    const size_t n = static_cast<size_t>((w > 0 && h > 0) ? (w * h) : 0);
    const uint32_t* src = g_machine.vdp.frame_px;
    uint32_t* dst = g_frame_rgba_buf;
    size_t i = 0u;
#if defined(__wasm_simd128__)
    const v128_t alpha_ff = wasm_i8x16_splat(-1);
    for (; i + 4u <= n; i += 4u) {
        const v128_t v = wasm_v128_load(src + i);
        const v128_t swapped = wasm_i8x16_shuffle(
            v, alpha_ff,
            2, 1, 0, 19,
            6, 5, 4, 23,
            10, 9, 8, 27,
            14, 13, 12, 31);
        wasm_v128_store(dst + i, swapped);
    }
#endif
    for (; i < n; ++i) {
        const uint32_t c = src[i];
        dst[i] = 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0x00FF00u) | ((c >> 16) & 0xFFu);
    }
}

WASM_EXPORT void wasm_render_frame_rgba() {
    g_machine.render_frame();
    wasm_copy_frame_rgba();
}

WASM_EXPORT uint8_t* wasm_frame_rgba_ptr() {
    return reinterpret_cast<uint8_t*>(g_frame_rgba_buf);
}

WASM_EXPORT uint32_t wasm_soundram_writes() { return g_machine.soundram_writes; }
WASM_EXPORT uint32_t wasm_scsp_event_seq() { return g_machine.scsp_event_seq; }
WASM_EXPORT void wasm_set_dbg_layers(uint32_t mask) { g_machine.dbg_hide_layers = static_cast<uint8_t>(mask & 0x0Fu); }
WASM_EXPORT uint32_t wasm_get_dbg_layers() { return g_machine.dbg_hide_layers; }
WASM_EXPORT void wasm_set_low_res_224p(uint32_t on) { g_machine.vdp.low_res_224p = (on != 0u); }
WASM_EXPORT uint32_t wasm_get_low_res_224p() { return g_machine.vdp.low_res_224p ? 1u : 0u; }
WASM_EXPORT void wasm_set_low_res_half_width(uint32_t on) { g_machine.vdp.low_res_half_width = (on != 0u); }
WASM_EXPORT void wasm_set_idle_skip(uint32_t on) { g_machine.idle_skip_enabled = (on != 0u); }
WASM_EXPORT uint32_t wasm_get_idle_skip() { return g_machine.idle_skip_enabled ? 1u : 0u; }
WASM_EXPORT void wasm_set_sh2_aot(uint32_t on) { g_machine.sh2_aot_enabled = (on != 0u); }
WASM_EXPORT uint32_t wasm_get_sh2_aot() { return g_machine.sh2_aot_enabled ? 1u : 0u; }
WASM_EXPORT void wasm_set_fast_cpu(uint32_t on) { g_machine.fast_cpu_enabled = (on != 0u); }
WASM_EXPORT uint32_t wasm_get_fast_cpu() { return g_machine.fast_cpu_enabled ? 1u : 0u; }
WASM_EXPORT uint8_t* wasm_render_pkt_ptr() { return g_render_pkt_buf; }
WASM_EXPORT uint32_t wasm_render_pkt_capacity() { return static_cast<uint32_t>(sizeof(g_render_pkt_buf)); }
WASM_EXPORT uint32_t wasm_pack_render_state() {
    return g_machine.pack_render_state(g_render_pkt_buf, sizeof(g_render_pkt_buf));
}
WASM_EXPORT uint32_t wasm_unpack_render_state_only(uint32_t len) {
    if (len > sizeof(g_render_pkt_buf)) return 0u;
    return g_machine.unpack_render_state_only(g_render_pkt_buf, len) ? 1u : 0u;
}
WASM_EXPORT void wasm_compose_band_rgba(uint32_t y0, uint32_t y1) {
    const int fh = g_machine.vdp.frame_h;
    const int fw = g_machine.vdp.frame_w;
    int sy0 = static_cast<int>(y0);
    int sy1 = static_cast<int>(y1);
    if (sy0 < 0) sy0 = 0;
    if (sy1 > fh) sy1 = fh;
    if (sy0 >= sy1 || fw <= 0) return;
    if (g_machine.vdp.compose_prep.disp_on) {
        g_machine.vdp.compose_frame_band(g_machine.inner.vdp2_vram.data(), sy0, sy1);
    }
    g_machine.finish_render_frame();
    const uint32_t start = static_cast<uint32_t>(sy0) * static_cast<uint32_t>(fw);
    const uint32_t end = static_cast<uint32_t>(sy1) * static_cast<uint32_t>(fw);
    const uint32_t* src = g_machine.vdp.frame_px;
    uint32_t* dst = g_frame_rgba_buf;
    uint32_t i = start;
#if defined(__wasm_simd128__)
    const v128_t alpha_ff = wasm_i8x16_splat(-1);
    for (; i + 4u <= end; i += 4u) {
        const v128_t v = wasm_v128_load(src + i);
        const v128_t swapped = wasm_i8x16_shuffle(
            v, alpha_ff,
            2, 1, 0, 19,
            6, 5, 4, 23,
            10, 9, 8, 27,
            14, 13, 12, 31);
        wasm_v128_store(dst + i, swapped);
    }
#endif
    for (; i < end; ++i) {
        const uint32_t c = src[i];
        dst[i] = 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0x00FF00u) | ((c >> 16) & 0xFFu);
    }
}
WASM_EXPORT void wasm_compose_finish_rgba() {
    g_machine.finish_render_frame();
    wasm_copy_frame_rgba();
}
WASM_EXPORT uint32_t wasm_has_cpu_only_overlays() {
    return g_machine.vdp.has_cpu_only_overlays() ? 1u : 0u;
}
WASM_EXPORT void wasm_set_gpu_vdp1(uint32_t on) {
    const bool enabled = (on != 0u);
    if (g_machine.vdp.gpu_vdp1_enabled != enabled) {
        g_machine.vdp.set_gpu_vdp1(enabled);
        g_gpu_vdp1_vram_valid = false;
    }
}
WASM_EXPORT uint32_t wasm_get_gpu_vdp1() {
    return g_machine.vdp.gpu_vdp1_enabled ? 1u : 0u;
}
WASM_EXPORT void wasm_gpu_vdp1_invalidate() {
    g_gpu_vdp1_vram_valid = false;
}
WASM_EXPORT void wasm_gpu_vdp2_invalidate() {
    g_gpu_vdp2_vram_valid = false;
    g_gpu_vdp1_vram_valid = false;
}
WASM_EXPORT uint32_t wasm_pack_gpu_vdp2(uint32_t force_full_vram) {
    const bool force_full_v2 = (force_full_vram != 0u) || !g_gpu_vdp2_vram_valid;
    const bool force_full_v1 = (force_full_vram != 0u) || !g_gpu_vdp1_vram_valid;
    const bool want_v1 = g_machine.vdp.gpu_vdp1_enabled && g_machine.vdp.vdp1_gpu_display().valid;
    const uint32_t n = g_machine.pack_gpu_vdp2_frame(
        g_render_pkt_buf, sizeof(g_render_pkt_buf),
        g_gpu_vdp2_prev_vram, force_full_v2,
        g_gpu_vdp1_prev_vram, force_full_v1);
    if (n > 0u) {
        g_gpu_vdp2_vram_valid = true;
        if (want_v1) g_gpu_vdp1_vram_valid = true;
    }
    return n;
}
WASM_EXPORT uint32_t wasm_unpack_render_state_and_render(uint32_t len) {
    if (len > sizeof(g_render_pkt_buf)) return 0u;
    if (!g_machine.unpack_render_state_and_render(g_render_pkt_buf, len)) return 0u;
    wasm_copy_frame_rgba();
    return 1u;
}
WASM_EXPORT uint32_t wasm_main_pc() { return g_machine.cpus[0].pc; }
WASM_EXPORT uint32_t wasm_slave_pc() { return g_machine.cpus[1].pc; }
WASM_EXPORT uint32_t wasm_scsp_event_slot(uint32_t seq_1based) {
    if (seq_1based == 0u || seq_1based > g_machine.scsp_event_seq) return 0u;
    const auto& ev = g_machine.scsp_events[(seq_1based - 1u) % twc98::StvMachineState::kMaxScspEvents];
    return (ev.seq == seq_1based) ? ev.slot : 0u;
}
WASM_EXPORT uint32_t wasm_scsp_event_w0(uint32_t seq_1based) {
    if (seq_1based == 0u || seq_1based > g_machine.scsp_event_seq) return 0u;
    const auto& ev = g_machine.scsp_events[(seq_1based - 1u) % twc98::StvMachineState::kMaxScspEvents];
    return (ev.seq == seq_1based) ? ev.w0 : 0u;
}
WASM_EXPORT uint32_t wasm_scsp_event_w1(uint32_t seq_1based) {
    if (seq_1based == 0u || seq_1based > g_machine.scsp_event_seq) return 0u;
    const auto& ev = g_machine.scsp_events[(seq_1based - 1u) % twc98::StvMachineState::kMaxScspEvents];
    return (ev.seq == seq_1based) ? ev.w1 : 0u;
}

WASM_EXPORT void wasm_refresh_state() {
    g_machine.compute_digests(
        g_digests_buf + 0u * 32u,
        g_digests_buf + 1u * 32u,
        g_digests_buf + 2u * 32u,
        g_digests_buf + 3u * 32u,
        g_digests_buf + 4u * 32u,
        g_digests_buf + 5u * 32u,
        g_digests_buf + 6u * 32u);
    for (int i = 0; i < 2; ++i) {
        uint32_t* dst = g_cpu_snapshot_buf + i * 28;
        const twc98::Sh2CpuState& c = g_machine.cpus[i];
        for (int r = 0; r < 16; ++r) dst[r] = c.r[r];
        dst[16] = c.pc;
        dst[17] = c.pr;
        dst[18] = c.gbr;
        dst[19] = c.vbr;
        dst[20] = c.mach;
        dst[21] = c.macl;
        dst[22] = c.sr;
        dst[23] = c.cur_pc;
        dst[24] = c.delay;
        dst[25] = static_cast<uint32_t>(c.cycles & 0xFFFFFFFFULL);
        dst[26] = static_cast<uint32_t>(c.retired & 0xFFFFFFFFULL);
        dst[27] = static_cast<uint32_t>((c.cycles >> 32) & 0xFFFFFFFFULL);
    }
}

WASM_EXPORT uint32_t wasm_rounds_done() { return g_machine.rounds_done; }
WASM_EXPORT uint32_t wasm_main_steps() { return g_machine.main_steps; }
WASM_EXPORT uint32_t wasm_slave_steps() { return g_machine.slave_steps; }
WASM_EXPORT uint32_t wasm_vbi_count() { return g_machine.vbi_count; }
WASM_EXPORT uint32_t wasm_vbo_count() { return g_machine.vbo_count; }
WASM_EXPORT uint32_t wasm_sched2_hits() { return g_machine.sched2_hits; }
WASM_EXPORT uint32_t wasm_minit_count() { return g_machine.minit_count; }
WASM_EXPORT uint32_t wasm_scu_dma_runs() { return g_machine.scu_dma_runs; }
WASM_EXPORT uint32_t wasm_sh_dmac1_runs() { return g_machine.sh_dmac1_runs; }
WASM_EXPORT uint32_t wasm_vdp1_draws() { return static_cast<uint32_t>(g_machine.vdp.draws); }
WASM_EXPORT uint32_t wasm_vdp1_frame_changes() { return static_cast<uint32_t>(g_machine.vdp.frame_changes); }
WASM_EXPORT int32_t wasm_frame_w() { return g_machine.vdp.frame_w; }
WASM_EXPORT int32_t wasm_frame_h() { return g_machine.vdp.frame_h; }
WASM_EXPORT uint32_t wasm_mmio_log_count() { return g_machine.mmio_log_count; }
WASM_EXPORT uint32_t wasm_mmio_entry_size() { return static_cast<uint32_t>(sizeof(twc98::MmioLogEntry)); }
WASM_EXPORT uint32_t wasm_ioga_reads() { return g_machine.ioga_reads; }
WASM_EXPORT uint32_t wasm_ioga_writes() { return g_machine.ioga_writes; }
WASM_EXPORT uint32_t wasm_smpc_input_reads() { return g_machine.smpc_input_reads; }
WASM_EXPORT uint32_t wasm_cart_reads() { return g_machine.cart_reads; }
WASM_EXPORT uint32_t wasm_gamebank_select() { return g_machine.smpc_gamebank_select; }
WASM_EXPORT uint32_t wasm_ioga_log_count() { return g_machine.ioga_log_count; }
WASM_EXPORT uint32_t wasm_ioga_entry_size() { return static_cast<uint32_t>(sizeof(twc98::IogaAccessEntry)); }
WASM_EXPORT uint8_t* wasm_ioga_log_ptr() { return reinterpret_cast<uint8_t*>(g_machine.ioga_log); }
WASM_EXPORT void wasm_set_input_ports(uint32_t port_a, uint32_t port_b, uint32_t port_c,
                                      uint32_t port_e, uint32_t port_f,
                                      uint32_t pdr1_in, uint32_t pdr2_in) {
    g_machine.set_input_ports(
        static_cast<uint8_t>(port_a & 0xFFu),
        static_cast<uint8_t>(port_b & 0xFFu),
        static_cast<uint8_t>(port_c & 0xFFu),
        static_cast<uint8_t>(port_e & 0xFFu),
        static_cast<uint8_t>(port_f & 0xFFu),
        static_cast<uint8_t>(pdr1_in & 0xFFu),
        static_cast<uint8_t>(pdr2_in & 0xFFu));
}
WASM_EXPORT void wasm_reset_inputs() { g_machine.reset_inputs(); }
WASM_EXPORT uint32_t wasm_bus_read(uint32_t cpu_id, uint32_t ea, uint32_t width) {
    twc98::BusAccessFault fault{};
    const uint32_t v = g_machine.read(
        static_cast<int>(cpu_id & 1u), ea, static_cast<uint8_t>(width), 0u, g_machine.cpus[0].cycles, fault);
    if (!fault.ok) {
        g_machine.halted = true;
        g_machine.halt_cpu = static_cast<int>(cpu_id & 1u);
        g_machine.last_fault = fault;
    }
    return v;
}
WASM_EXPORT uint32_t wasm_bus_write(uint32_t cpu_id, uint32_t ea, uint32_t width, uint32_t val) {
    twc98::BusAccessFault fault{};
    g_machine.write(
        static_cast<int>(cpu_id & 1u), ea, static_cast<uint8_t>(width), val, 0u, g_machine.cpus[0].cycles, fault);
    if (!fault.ok) {
        g_machine.halted = true;
        g_machine.halt_cpu = static_cast<int>(cpu_id & 1u);
        g_machine.last_fault = fault;
        return 0u;
    }
    return 1u;
}
WASM_EXPORT uint32_t wasm_halted() { return g_machine.halted ? 1u : 0u; }
WASM_EXPORT int32_t wasm_halt_cpu() { return g_machine.halt_cpu; }
WASM_EXPORT uint32_t wasm_fault_pc() { return g_machine.last_fault.pc; }
WASM_EXPORT uint32_t wasm_fault_addr() { return g_machine.last_fault.address; }
WASM_EXPORT uint32_t wasm_fault_opcode() { return g_machine.last_fault.opcode; }
WASM_EXPORT uint32_t wasm_fault_width() { return g_machine.last_fault.width; }
WASM_EXPORT uint32_t wasm_fault_is_write() { return g_machine.last_fault.is_write ? 1u : 0u; }

WASM_EXPORT uint32_t wasm_divu_reg(uint32_t cpu_id, uint32_t idx) {
    if (cpu_id > 1u) return 0u;
    const auto& d = g_machine.inner.divu[cpu_id];
    switch (idx) {
        case 0u: return d.dvsr;
        case 1u: return d.dvdnth;
        case 2u: return d.dvdntl;
        case 3u: return d.ovf;
        case 4u: return d.ovfie;
        case 5u: return d.ops;
        default: return d.written;
    }
}

WASM_EXPORT uint32_t wasm_frt_reg(uint32_t cpu_id, uint32_t idx) {
    if (cpu_id > 1u) return 0u;
    const auto& f = g_machine.inner.frt[cpu_id];
    switch (idx) {
        case 0u: return f.tier;
        case 1u: return f.ftcsr;
        case 2u: return f.frc;
        case 3u: return f.ocra;
        case 4u: return f.ocrb;
        case 5u: return f.tcr;
        default: return g_machine.frt_icr[cpu_id];
    }
}

// ---- B-RE-05: Cold Boot from BIOS + 315-5881 ----
WASM_EXPORT uint32_t wasm_init_cold_boot(uint32_t bios_len, uint32_t cart_len) {
    if (bios_len != twc98::StvMachineState::kBiosSize) return 0u;
    if (cart_len == 0u || cart_len > twc98::StvMachineState::kCartPopulatedSize) return 0u;
    const char* err = nullptr;
    std::memset(&g_copa_backup, 0, sizeof(g_copa_backup));
    g_copa_backup.tournament = twc98::copa::CopaTournamentState{};
    g_copa_backup.match_rules = twc98::copa::MatchRulesConfig{};
    g_gpu_vdp2_vram_valid = false;
    g_gpu_vdp1_vram_valid = false;
    return g_machine.init_cold_boot_bytes(g_bios_buf, bios_len, g_machine.cart_rom, cart_len, &err) ? 1u : 0u;
}

// ---- B-H5-01 & B-RE-04: Copa América Mod & GK AI Fix ----
WASM_EXPORT uint32_t wasm_set_game_mode(uint32_t mode, uint32_t seed) {
    const auto target = (mode != 0u) ? twc98::copa::GameMode::CopaAmerica : twc98::copa::GameMode::Classic;
    return twc98::copa::apply_game_mode(g_machine, target, g_copa_backup, seed) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_get_game_mode() {
    return (g_copa_backup.active_mode == twc98::copa::GameMode::CopaAmerica) ? 1u : 0u;
}

WASM_EXPORT void wasm_copa_on_frame_tick() {
    twc98::copa::on_frame_tick(g_machine, g_copa_backup);
}

WASM_EXPORT void wasm_set_match_rules(uint32_t hard_mode, uint32_t rule_mode, uint32_t rule_value) {
    twc98::copa::MatchRulesConfig cfg{};
    cfg.hard_mode = (hard_mode != 0u);
    cfg.rule_mode = static_cast<twc98::copa::MatchRuleMode>(rule_mode & 0xFFu);
    cfg.rule_value = static_cast<uint16_t>(rule_value & 0xFFFFu);
    twc98::copa::set_match_rules(g_machine, g_copa_backup, cfg);
}

WASM_EXPORT uint32_t wasm_get_hard_mode() {
    return g_machine.rule_hard_mode ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_get_rule_mode() {
    return static_cast<uint32_t>(g_machine.rule_mode);
}

WASM_EXPORT uint32_t wasm_get_rule_value() {
    return static_cast<uint32_t>(g_machine.rule_value);
}

WASM_EXPORT void wasm_copa_reset_play_flags() {
    g_machine.copa_entered_gameplay = false;
    g_machine.copa_returned_to_title = false;
    g_machine.copa_sp3_wind_timer = 0u;
    g_machine.copa_sp3_kick_count = 0u;
    g_machine.copa_ts_heartbeat = 0u;
    g_machine.vdp.copa_group_badges = false;
}

WASM_EXPORT uint32_t wasm_copa_consume_returned_to_title() {
    if (!g_machine.copa_returned_to_title) return 0u;
    g_machine.copa_returned_to_title = false;
    g_machine.copa_entered_gameplay = false;
    return 1u;
}

WASM_EXPORT uint32_t wasm_m68k_enabled() {
    return g_machine.m68k_enabled ? 1u : 0u;
}

WASM_EXPORT void wasm_set_gk_ai_fix(uint32_t enabled) {
    g_machine.set_gk_ai_fix(enabled != 0u);
}

WASM_EXPORT uint32_t wasm_get_gk_ai_fix() {
    return g_machine.fix_p1_gk_ai ? 1u : 0u;
}

WASM_EXPORT void wasm_set_coop_mode(uint32_t enabled) {
    g_machine.set_coop_mode(enabled != 0u);
}

WASM_EXPORT uint32_t wasm_get_coop_mode() {
    return g_machine.coop.enabled ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_get_coop_p1_actor() {
    return g_machine.coop.p1_actor;
}

WASM_EXPORT uint32_t wasm_get_coop_p2_actor() {
    return g_machine.coop.p2_actor;
}

WASM_EXPORT uint32_t wasm_get_coop_call_pass_mask() {
    return (g_machine.coop.call_pass_timer[0] > 0u ? 1u : 0u) |
           (g_machine.coop.call_pass_timer[1] > 0u ? 2u : 0u);
}

WASM_EXPORT void wasm_set_offscreen_indicator(uint32_t enabled) {
    g_machine.offscreen_p1_indicator = (enabled != 0u);
}

WASM_EXPORT uint32_t wasm_get_offscreen_indicator() {
    return g_machine.offscreen_p1_indicator ? 1u : 0u;
}

// ---- B-QA-01: Real M68K + SCSP Audio ----
WASM_EXPORT void wasm_set_real_audio(uint32_t enabled) {
    g_machine.set_real_audio(enabled != 0u);
}

WASM_EXPORT uint32_t wasm_get_real_audio() {
    return g_machine.real_audio_enabled ? 1u : 0u;
}

WASM_EXPORT int16_t* wasm_audio_pcm_ptr() {
    return g_audio_pcm_buf;
}

WASM_EXPORT uint32_t wasm_consume_audio_pcm(uint32_t max_frames) {
    const uint32_t cap = (max_frames > 4096u) ? 4096u : max_frames;
    return g_machine.consume_audio_pcm(g_audio_pcm_buf, cap);
}

WASM_EXPORT void wasm_clear_audio_pcm() {
    g_machine.clear_audio_pcm();
}

WASM_EXPORT uint32_t wasm_audio_ring_available() {
    return g_machine.audio_ring_count;
}

WASM_EXPORT uint32_t wasm_audio_pcm_produced() {
    return g_machine.audio_pcm_produced;
}

WASM_EXPORT uint32_t wasm_audio_pcm_nonzero() {
    return g_machine.audio_pcm_nonzero;
}

// ---- B-H6-01: Fast Rollback Engine ----
WASM_EXPORT uint32_t wasm_save_fast_rollback() {
    return g_machine.save_fast_rollback(g_state_buf, sizeof(g_state_buf));
}

WASM_EXPORT uint32_t wasm_load_fast_rollback(uint32_t len) {
    if (len > sizeof(g_state_buf)) return 0u;
    return g_machine.load_fast_rollback(g_state_buf, len) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_step_frame_no_render(uint32_t num_rounds, uint32_t emit_audio_ring) {
    return g_machine.step_frame_no_render(num_rounds, emit_audio_ring != 0u) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_sync_hash64_lo() {
    return static_cast<uint32_t>(g_machine.compute_sync_hash64() & 0xFFFFFFFFULL);
}

WASM_EXPORT uint32_t wasm_sync_hash64_hi() {
    return static_cast<uint32_t>((g_machine.compute_sync_hash64() >> 32) & 0xFFFFFFFFULL);
}

// ---- B-H6-02: Deterministic NetSession ----
static void wasm_net_post_frame_hook(twc98::StvMachineState& m, void*) {
    twc98::copa::on_frame_tick(m, g_copa_backup);
}

WASM_EXPORT uint32_t wasm_net_reset_ex(uint32_t local_player, uint32_t delay_frames, uint32_t copa_mode,
                                       uint32_t gk_fix, uint32_t coop_mode) {
    g_net_session.set_post_frame_hook(&wasm_net_post_frame_hook, nullptr);
    g_net_session.reset(
        static_cast<uint8_t>(local_player & 1u),
        static_cast<uint8_t>(delay_frames),
        copa_mode != 0u,
        gk_fix != 0u,
        coop_mode != 0u);
    return 1u;
}

WASM_EXPORT uint32_t wasm_net_reset(uint32_t local_player, uint32_t delay_frames, uint32_t copa_mode, uint32_t gk_fix) {
    return wasm_net_reset_ex(local_player, delay_frames, copa_mode, gk_fix, g_machine.coop.enabled ? 1u : 0u);
}

WASM_EXPORT uint8_t* wasm_net_pkt_ptr() {
    return g_net_pkt_buf;
}

WASM_EXPORT uint32_t wasm_net_pkt_size() {
    return static_cast<uint32_t>(twc98::NetInputPacket::kWireBytes);
}

WASM_EXPORT uint32_t wasm_net_queue_local_input(uint32_t buttons, uint32_t sys_c) {
    twc98::NetInputPacket pkt{};
    g_net_session.queue_local_input(
        static_cast<uint8_t>(buttons & 0xFFu),
        static_cast<uint8_t>(sys_c & 0xFFu),
        pkt);
    twc98::pack_net_input_packet(pkt, g_net_pkt_buf);
    return pkt.head_frame;
}

WASM_EXPORT uint32_t wasm_net_receive_wire_pkt(uint32_t len) {
    return g_net_session.receive_wire_bytes(g_net_pkt_buf, len) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_net_step_frame() {
    return g_net_session.step_frame(g_machine, g_cp_buf, sizeof(g_cp_buf), true) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_net_step_frame_ex(uint32_t render_this_frame) {
    return g_net_session.step_frame(g_machine, g_cp_buf, sizeof(g_cp_buf), render_this_frame != 0u) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_net_flush_rollback(uint32_t render_final) {
    return g_net_session.flush_pending_rollback(g_machine, g_cp_buf, sizeof(g_cp_buf), render_final != 0u) ? 1u : 0u;
}

WASM_EXPORT uint32_t wasm_net_cur_frame() { return g_net_session.cur_frame(); }
WASM_EXPORT uint32_t wasm_net_rollback_count() { return g_net_session.rollback_count(); }
WASM_EXPORT uint32_t wasm_net_rollback_frames() { return g_net_session.rollback_frames_total(); }
WASM_EXPORT uint32_t wasm_net_max_rollback_depth() { return g_net_session.max_rollback_depth(); }
WASM_EXPORT uint32_t wasm_net_stall_frames() { return g_net_session.stall_frames(); }
WASM_EXPORT uint32_t wasm_net_sync_checks_passed() { return g_net_session.sync_checks_passed(); }
WASM_EXPORT uint32_t wasm_net_desync_count() { return g_net_session.desync_count(); }
WASM_EXPORT uint32_t wasm_net_first_desync_frame() { return g_net_session.first_desync_frame(); }
WASM_EXPORT uint32_t wasm_net_first_desync_local_hash() { return g_net_session.first_desync_local_hash(); }
WASM_EXPORT uint32_t wasm_net_first_desync_remote_hash() { return g_net_session.first_desync_remote_hash(); }
WASM_EXPORT int32_t wasm_net_recommended_usec_delta() { return g_net_session.recommended_usec_delta(); }

}  // extern "C"
