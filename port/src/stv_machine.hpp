// Portable zero-heap dual-CPU Sega ST-V autonomous machine (Main + Slave SH-2,
// SCU DMA/IST/IMS, SMPC, SH7604 DMAC0/1/FRT/DIVU, VDP1/VDP2 frame producer).
// Shared identically between Native PC and Wasm targets.
#pragma once

#include "cpn8_io.hpp"
#include "sh2_core.hpp"
#include "stv_315_5881.hpp"
#include "stv_peripherals.hpp"
#include "stv_scsp.hpp"
#include "stv_vdp.hpp"
#include "twc98_coop_mod.hpp"

#include <cstddef>
#include <cstdint>

namespace twc98 {

enum MmioEventKind : uint8_t {
    MMIO_IRQ = 1u,
    MMIO_SMPC_CMD = 2u,
    MMIO_VDP1_REG_W = 3u,
    MMIO_SH_DMAC1 = 4u,
    MMIO_SCU_DMA = 5u,
    MMIO_MINIT = 6u
};

struct MmioLogEntry {
    uint64_t cycles = 0u;
    uint32_t round = 0u;
    uint32_t pc = 0u;
    uint32_t arg0 = 0u;
    uint32_t arg1 = 0u;
    uint8_t kind = 0u;
    uint8_t cpu_id = 0u;
    uint8_t width = 0u;
    uint8_t reserved = 0u;
};

struct ScuDmaChannel {
    uint32_t r = 0u;
    uint32_t w = 0u;
    uint32_t c = 0u;
    uint32_t ad = 0u;
    uint32_t en = 0u;
    uint32_t md = 0u;
};

struct ShDmaCh1 {
    uint32_t sar = 0u;
    uint32_t dar = 0u;
    uint32_t tcr = 0u;
    uint32_t chcr = 0u;
    uint8_t drcr = 0u;
};

// Active-low ST-V IOGA/SMPC input lines (0xFF = all released).
struct StvInputState {
    uint8_t port_a = 0xFFu;   // IOGA +0x01: P1 (B7=L B6=R B5=U B4=D B3=B4 B2=B3 B1=B2 B0=B1)
    uint8_t port_b = 0xFFu;   // IOGA +0x03: P2 (B7=L B6=R B5=U B4=D B3=B4 B2=B3 B1=B2 B0=B1)
    uint8_t port_c = 0xFFu;   // IOGA +0x05: System (B7=Svc B6=Test B5=2PStart B4=1PStart B3..0=Coin4..1)
    uint8_t port_e = 0xFFu;   // IOGA +0x09: P3
    uint8_t port_f = 0xFFu;   // IOGA +0x0B: P4
    uint8_t pdr1_in = 0x7Fu;  // SMPC PDR1 (+0x75) input bits
    uint8_t pdr2_in = 0x67u;  // SMPC PDR2 (+0x77) input bits (0x67 idle observed in MAME)
};

struct IogaAccessEntry {
    uint32_t pc = 0u;
    uint32_t address = 0u;
    uint8_t val = 0u;
    uint8_t cpu_id = 0u;
    uint8_t is_write = 0u;
    uint8_t width = 0u;
};

class StvMachineState final : public Sh2MemoryBus {
public:
    static constexpr uint32_t kBiosSize = 0x80000u;
    static constexpr uint32_t kBackupRamSize = 0x8000u;
    static constexpr uint32_t kSoundRamSize = 0x100000u;   // 1 MiB SCSP Sound RAM (0x05A00000..0x05AFFFFF)
    static constexpr uint32_t kScspRegsSize = 0x1000u;     // 4 KiB SCSP registers (0x05B00000..0x05B00FFF)
    static constexpr uint32_t kCartPopulatedSize = 0x01400000u;  // 20 MiB (0x02000000..0x033FFFFF)
    static constexpr uint32_t kCartFullEnd = 0x05000000u;        // 48 MiB range end (exclusive)
    static constexpr uint32_t kCartProtStart = 0x04FFFFF0u;      // Sega 315-5881 key/crypto window
    static constexpr uint64_t kFrameCycles = 450000ULL;
    static constexpr uint32_t kMaxMmioLog = 4096u;
    static constexpr uint32_t kMaxIogaLog = 256u;
    static constexpr uint32_t kMaxSnapshotBytes = 8u * 1024u * 1024u;
    static constexpr uint32_t kSnapshotMagic = 0x53435754u;  // "TWCS"
    static constexpr uint32_t kSnapshotVersion = 1u;
    static constexpr uint32_t kMaxFastRollbackBytes = 6656u * 1024u;
    static constexpr uint32_t kFastRollbackMagic = 0x52425754u;  // "TWBR"
    static constexpr uint32_t kFastRollbackVersion = 2u;

    Sh2CpuState cpus[2]{};
    SaturnStvBus inner{};
    StvVdpState vdp{};

    uint8_t bios_rom[kBiosSize]{};
    bool has_bios = false;
    uint8_t cart_rom[kCartPopulatedSize]{};
    bool has_cart = false;
    bool cart_prot_enabled = false;
    Stv3155881State crypt315{};
    uint8_t backupram[kBackupRamSize]{};
    uint8_t sound_ram[kSoundRamSize]{};
    uint8_t scsp_regs[kScspRegsSize]{};

    ScuDmaChannel scu_dma[3]{};
    uint32_t scu_ims = 0xFFFF717Cu;
    uint32_t scu_ist = 0u;
    uint32_t scu_rsel = 0u;
    bool scu_dma_irq_armed[3]{};
    uint64_t scu_dma_irq_cycle[3]{};

    ShDmaCh1 dmac1{};
    uint8_t frt_tocr[2]{};
    uint16_t frt_icr[2]{};

    uint8_t smpc_ireg[7]{};
    uint8_t smpc_oreg[32]{};
    uint8_t smpc_sr = 0x00u;
    uint8_t smpc_sf = 0x00u;
    uint8_t smpc_pdr1_out = 0x00u;
    uint8_t smpc_pdr2_out = 0x00u;
    uint8_t smpc_ddr1 = 0x00u;
    uint8_t smpc_ddr2 = 0x00u;
    uint8_t smpc_gamebank_select = 0u;
    bool smpc_peri_pending = false;
    uint64_t smpc_irq_cycle = 0u;
    bool smpc_irq_armed = false;
    uint64_t rtc_base_cycles = 0u;

    StvInputState inputs{};
    uint8_t ioga_system_output = 0x00u;
    uint8_t ioga_mode = 0x00u;
    uint8_t ioga_portg = 0xFFu;
    uint8_t ioga_billboard = 0x00u;
    uint8_t ioga_direction = 0x00u;

    uint16_t vdp2_tvstat = 0u;
    uint16_t vdp2_hcnt = 0u;
    uint16_t vdp2_vcnt = 0u;

    uint64_t next_vbi_cyc = 0u;
    uint64_t next_vbo_cyc = 0u;
    bool pending_vbi = false;
    bool pending_vbo = false;

    uint32_t rounds_done = 0u;
    uint32_t main_steps = 0u;
    uint32_t slave_steps = 0u;
    uint32_t vbi_count = 0u;
    uint32_t vbo_count = 0u;
    uint32_t sched2_hits = 0u;
    uint32_t minit_count = 0u;
    uint32_t sinit_count = 0u;
    uint32_t vdp1_vram_writes = 0u;
    uint32_t scu_dma_runs = 0u;
    uint32_t sh_dmac1_runs = 0u;
    uint32_t ioga_reads = 0u;
    uint32_t ioga_writes = 0u;
    uint32_t smpc_input_reads = 0u;
    uint32_t cart_reads = 0u;
    uint32_t soundram_reads = 0u;
    uint32_t soundram_writes = 0u;
    bool scsp_ack_armed = false;
    uint64_t scsp_ack_cycle = 0u;
    uint32_t last_write_val = 0u;
    uint32_t last_write_ea = 0u;

    bool halted = false;
    int halt_cpu = -1;
    BusAccessFault last_fault{};
    bool slave_enabled = true;               // Non-serialized unless changed in cold boot
    bool smpc_reset_master_pending = false;
    bool pending_nmi = false;

    MmioLogEntry mmio_log[kMaxMmioLog]{};
    uint32_t mmio_log_count = 0u;
    IogaAccessEntry ioga_log[kMaxIogaLog]{};
    uint32_t ioga_log_count = 0u;

    struct ScspCmdEvent {
        uint32_t seq = 0u;
        uint32_t slot = 0u;
        uint32_t w0 = 0u;
        uint32_t w1 = 0u;
    };
    static constexpr uint32_t kMaxScspEvents = 64u;
    ScspCmdEvent scsp_events[kMaxScspEvents]{};
    uint32_t scsp_event_seq = 0u;
    uint8_t dbg_hide_layers = 0u;  // Non-serialized visual debug layer mask (1=VDP1, 2=RBG0, 4=NBG, 8=HD-RBG0)
    bool fix_p1_gk_ai = false;     // Non-serialized GK AI shot-reaction & 1v1 rush-out fix (B-RE-04 / B-DEC-04)
    bool offscreen_p1_indicator = false;  // Non-serialized read-only HUD badge when P1/P2 marker hides or exits screen
    bool idle_skip_enabled = true; // Non-serialized bit-exact idle spin-loop skip optimization
    bool master_events_dirty = true; // Non-serialized flag to recompute next Master IRQ/timer threshold

    // Non-serialized SH-2 AOT 64-byte High WRAM dirty/validity bitmaps covering [0x06000000, 0x06070000).
    // 64-byte granularity cleanly separates hot SGL 3D code (e.g. 0x06063200..0x060632BF & 0x06063300..0x060633FF)
    // from adjacent mutable scratch data (0x060632C4..0x060632F3) with zero rollback snapshot bloat.
    static constexpr uint32_t kAotCodeLo = 0x06001000u;
    static constexpr uint32_t kAotCodeHi = 0x06070000u;
    static constexpr uint32_t kAotLineShift = 6u;
    static constexpr uint32_t kAotLineSize = 1u << kAotLineShift;
    static constexpr uint32_t kAotCodeRangeBytes = kAotCodeHi - kHighWramBase;  // 0x00070000 (448 KiB)
    static constexpr uint32_t kAotNumLines = kAotCodeRangeBytes >> kAotLineShift;  // 7168 lines
    static constexpr uint32_t kAotBitmapWords = kAotNumLines >> 5;                 // 224 words (896 B)
    uint32_t code_dirty_bits[kAotBitmapWords]{};
    uint32_t aot_page_valid_bits[kAotBitmapWords]{};
    bool aot_pages_initialized = false;
    bool sh2_aot_enabled = true;   // Non-serialized toggle (wasm_set_sh2_aot)
    bool fast_cpu_enabled = false; // Non-serialized fast quantum/idle-skip/HLE toggle (wasm_set_fast_cpu)

    inline void invalidate_all_aot_pages() noexcept {
        for (uint32_t i = 0u; i < kAotBitmapWords; ++i) {
            code_dirty_bits[i] = 0xFFFFFFFFu;
            aot_page_valid_bits[i] = 0u;
        }
        aot_pages_initialized = true;
    }

    inline void mark_high_wram_dirty(uint32_t h_off, uint32_t width) noexcept {
        if (__builtin_expect(h_off >= kAotCodeRangeBytes || width == 0u, 1)) {
            return;
        }
        if (__builtin_expect(width <= 4u && ((h_off & (width - 1u)) == 0u), 1)) {
            const uint32_t ln = h_off >> kAotLineShift;
            const uint32_t wi = ln >> 5;
            const uint32_t mask = 1u << (ln & 31u);
            code_dirty_bits[wi] |= mask;
            aot_page_valid_bits[wi] &= ~mask;
            return;
        }
        const uint32_t start_ln = h_off >> kAotLineShift;
        uint32_t end_off = h_off + width - 1u;
        if (end_off >= kAotCodeRangeBytes) {
            end_off = kAotCodeRangeBytes - 1u;
        }
        const uint32_t end_ln = end_off >> kAotLineShift;
        for (uint32_t ln = start_ln; ln <= end_ln; ++ln) {
            const uint32_t wi = ln >> 5;
            const uint32_t mask = 1u << (ln & 31u);
            code_dirty_bits[wi] |= mask;
            aot_page_valid_bits[wi] &= ~mask;
        }
    }

    bool revalidate_aot_page(uint32_t line_idx) noexcept;

    // Non-serialized Copa America runtime trackers (reset cleanly on mode toggle / boot)
    bool copa_mode_active = false;
    bool copa_entered_gameplay = false;
    bool copa_returned_to_title = false;
    uint8_t copa_ts_heartbeat = 0u; // >0 while Team Select 3x4 grid loop is actively running
    uint32_t copa_sp3_wind_timer = 0u;
    uint32_t copa_sp3_kick_count = 0u;
    uint32_t copa_ball_srca = 0u; // Exact VDP1 ball sprite CMDSRCA*8 when locked
    using CopaVsHookFn = void (*)(StvMachineState&);
    CopaVsHookFn copa_vs_hook = nullptr;  // Non-serialized static hook set by twc98::copa
    CopaVsHookFn copa_opp_hook = nullptr; // Non-serialized pre-load opponent hook set by twc98::copa

    // B-H5-05 (Hard Mode) & B-H6-04 (Host Match Time / Goal Limit Rules) — synced pre-Frame0 and rolled back cleanly
    bool rule_hard_mode = false;
    uint8_t rule_mode = 0u;       // 0=Default Arcade, 1=Custom Time (sec), 2=Goal Limit (first to N goals)
    uint16_t rule_value = 90u;    // Target seconds (mode 1) or target goals (mode 2)
    bool rule_goal_triggered = false;
    uint16_t rule_goal_delay = 0u;

    // 2P Same-Team Co-op Mode (Team 0 = P1 + P2 vs Team 1 = CPU) — serialized conditionally for 0-desync rollback
    CoopState coop{};

    // Real M68EC000 + Yamaha YMF292 SCSP audio state (serialized conditionally via flags & 16u).
    StvScspState scsp{};
    bool real_audio_enabled = false;
    bool m68k_enabled = false;
    bool m68k_in_reset = true;
    bool pending_sound_irq = false;
    int32_t m68k_debt = 0;
    uint64_t audio_cyc_acc = 0u;
    uint64_t audio_last_sh2_cyc = 0u;
    uint32_t m68k_pc_cached = 0u;
    uint32_t audio_pcm_produced = 0u;
    uint32_t audio_pcm_nonzero = 0u;
    static constexpr uint32_t kMaxM68kCtxBytes = 2048u;
    uint8_t m68k_ctx_blob[kMaxM68kCtxBytes]{};
    uint32_t m68k_ctx_size = 0u;

    // Non-serialized stereo 44,100 Hz PCM output ring consumed by PC host waveOut.
    static constexpr uint32_t kMaxAudioRingFrames = 8192u;
    int16_t audio_ring[kMaxAudioRingFrames * 2u]{};
    uint32_t audio_ring_r = 0u;
    uint32_t audio_ring_w = 0u;
    uint32_t audio_ring_count = 0u;

    void set_gk_ai_fix(bool enabled);
    void apply_gk_ai_fix() noexcept;
    void set_coop_mode(bool enabled);
    void set_real_audio(bool enabled);
    uint32_t consume_audio_pcm(int16_t* dst_stereo, uint32_t max_frames);
    void clear_audio_pcm();

    inline uint32_t fast_read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                              uint64_t cpu_cycles, BusAccessFault& fault);
    inline void fast_write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
                           uint64_t cpu_cycles, BusAccessFault& fault);

    void reset();
    void clear_cart_rom();
    bool load_cart_rom(const uint8_t* data, size_t len);
    void mark_cart_loaded(uint32_t len);
    void set_input_ports(uint8_t port_a, uint8_t port_b, uint8_t port_c,
                         uint8_t port_e, uint8_t port_f, uint8_t pdr1_in, uint8_t pdr2_in);
    void reset_inputs();

    bool init_from_cpn8_bytes(const uint8_t* cp_data, size_t cp_len,
                              const uint8_t* bios_data, size_t bios_len,
                              const char** out_error);
    bool init_cold_boot_bytes(const uint8_t* bios_data, size_t bios_len,
                              const uint8_t* cart_data, size_t cart_len,
                              const char** out_error);

    void seed_vdp_snapshot(const uint8_t* v1_vram, size_t v1_vram_len,
                           const uint8_t* v1_regs, size_t v1_regs_len,
                           const uint8_t* v2_vram, size_t v2_vram_len,
                           const uint8_t* v2_cram, size_t v2_cram_len,
                           const uint8_t* v2_regs, size_t v2_regs_len);

    bool run_rounds(uint32_t num_rounds);
    bool run_rounds_ex(uint32_t num_rounds, bool emit_audio_ring);
    void advance_frame_state_no_render();
    bool step_frame_no_render(uint32_t num_rounds = 10000u, bool emit_audio_ring = false);
    void prepare_render_frame();
    void finish_render_frame();
    void render_frame();

    static constexpr uint32_t kMaxRenderPktBytes = 2200u * 1024u;
    uint32_t pack_render_state(uint8_t* dst, size_t dst_capacity) const;
    bool unpack_render_state_only(const uint8_t* src, size_t len);
    bool unpack_render_state_and_render(const uint8_t* src, size_t len);
    uint32_t pack_gpu_vdp2_frame(uint8_t* dst, size_t dst_capacity,
                                 uint8_t prev_vdp2_vram[0x80000], bool force_full_vram,
                                 uint8_t prev_vdp1_vram[0x80000] = nullptr, bool force_full_v1_vram = false);

    // Zero-heap portable mutable state snapshot (excludes immutable bios_rom/cart_rom & derived frame_px).
    uint32_t save_state_bytes(uint8_t* dst, size_t dst_capacity) const;
    bool load_state_bytes(const uint8_t* src, size_t len, const char** out_error);

    // High-speed page-diff in-memory rollback snapshot (< 1 ms PC / < 2 ms Wasm; skips pixel compose & audio ring clear).
    uint32_t save_fast_rollback(uint8_t* dst, size_t dst_capacity) const;
    bool load_fast_rollback(const uint8_t* src, size_t len);

    // Fast deterministic 64-bit state hash (FNV-1a64 over CPUs, High WRAM, VDP regs, and frame counters).
    uint64_t compute_sync_hash64() const;

    void compute_digests(uint8_t high_wram_sha[32],
                         uint8_t low_wram_sha[32],
                         uint8_t vdp1_vram_sha[32],
                         uint8_t vdp2_vram_sha[32],
                         uint8_t vdp2_cram_sha[32],
                         uint8_t vdp2_regs_sha[32],
                         uint8_t frame_sha[32]) const;

    uint32_t read(int cpu_id, uint32_t ea, uint8_t width, uint32_t pc,
                  uint64_t cpu_cycles, BusAccessFault& fault) override;
    void write(int cpu_id, uint32_t ea, uint8_t width, uint32_t val, uint32_t pc,
               uint64_t cpu_cycles, BusAccessFault& fault) override;

private:
    void log_mmio(MmioEventKind kind, uint8_t cpu_id, uint32_t pc,
                  uint32_t arg0, uint32_t arg1, uint8_t width, uint64_t cyc);
    void log_ioga(uint8_t cpu_id, uint32_t pc, uint32_t addr, uint8_t val,
                  uint8_t is_write, uint8_t width);
    uint8_t read_ioga_byte(uint32_t off);
    void write_ioga_byte(uint32_t off, uint8_t bval);
    void run_sh_dmac1(int cpu_id, uint32_t pc, uint64_t cyc, BusAccessFault& fault);
    void scu_copy(int cpu_id, uint32_t& src, uint32_t& dst, uint32_t bytes,
                  uint32_t radd, uint32_t wadd, uint32_t pc, uint64_t cyc,
                  BusAccessFault& fault);
    void run_scu_dma(int cpu_id, int lvl, uint32_t pc, uint64_t cyc, BusAccessFault& fault);
    bool inject_sh2_irq(int cpu_id, uint8_t vector, uint8_t level, BusAccessFault& fault);
    void start_m68k_unlocked();
    void ensure_stage0_sound_seeded();
    void handle_smpc_pdr2_write(uint8_t prev, uint8_t cur);
    void step_audio_samples(uint32_t num_samples);
    void step_audio_tick(uint64_t sh2_cycles);

    bool emit_audio_ring_ = true;
};

}  // namespace twc98
