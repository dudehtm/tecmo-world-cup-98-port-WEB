// Portable zero-heap Sega ST-V VDP1 + VDP2 software frame producer.
// Derived from saturnkit runtime (https://github.com/vs-sr-dev/saturnkit,
// commit 1b345ef949e1fa258ce3f693f03c0df8acdfb40d, MIT License) and
// .local/experiments/010-output-consumer/vdp1-experiment.cpp.
#pragma once

#include <cstdint>

namespace twc98 {

struct StvVdpState {
    static constexpr uint32_t kVdp1VramSize = 0x80000u;
    static constexpr uint32_t kVdp1FbPlaneWords = 1024u * 512u;
    static constexpr uint32_t kVdp2CramSize = 0x1000u;
    static constexpr uint32_t kVdp2RegsSize = 0x200u;
    static constexpr int kMaxFrameW = 704;
    static constexpr int kMaxFrameH = 512;

    enum RegIdx {
        TVMR = 0,
        FBCR = 1,
        PTMR = 2,
        EWDR = 3,
        EWLR = 4,
        EWRR = 5,
        ENDR = 6,
        EDSR = 8,
        LOPR = 9,
        COPR = 10,
        MODR = 11
    };

    struct Nbg {
        bool on = false;
        bool opaque = false;
        bool bitmap = false;
        bool cell2x2 = false;
        bool word1 = false;
        bool cnsm = false;
        int colors = 0;
        uint16_t supp = 0u;
        int plane_w = 0;
        int plane_h = 0;
        uint32_t plane[4]{};
        uint32_t page_bytes = 0u;
        int bm_w = 0;
        int bm_h = 0;
        uint32_t bm_addr = 0u;
        uint16_t bm_pal = 0u;
        uint32_t caos = 0u;
        int32_t sx = 0;
        int32_t sy = 0;
        int32_t dx = 0;
        int32_t dy = 0;
        uint8_t prio = 0u;
        uint8_t ratio = 0u;
        bool cc = false;
        bool offset = false;
        bool offset_b = false;
        bool bm_spr = false;
        bool bm_scc = false;
        bool line = false;
        int32_t lx[kMaxFrameH]{};
        int32_t ly[kMaxFrameH]{};
        int32_t ldx[kMaxFrameH]{};
    };

    struct RotParam {
        Nbg chr{};
        uint32_t plane[16]{};
        int32_t xst = 0, yst = 0, zst = 0, dxst = 0, dyst = 0, dx = 0, dy = 0;
        int32_t m[6]{};
        int32_t px = 0, py = 0, pz = 0, cx = 0, cy = 0, cz = 0, mx = 0, my = 0, kx = 0, ky = 0;
        uint32_t ka0 = 0u;
        int32_t dkast = 0, dkax = 0;
        bool coef = false;
        bool coef1w = false;
        int coef_mode = 0;
        int over = 0;
        uint16_t over_pn = 0u;
        int32_t xsp = 0, ysp = 0, xp = 0, yp = 0, ldx = 0, ldy = 0;
        uint32_t ka = 0u;
        uint32_t line_coef = 0u;
    };

    struct Rbg {
        bool on = false;
        int mode = 0;
        bool crkte = false;
        bool perdot = false;
        bool bank_coef[4]{};
        RotParam par[2]{};
        uint8_t prio = 0u;
        uint8_t ratio = 0u;
        bool cc = false;
        bool offset = false;
        bool offset_b = false;
    };

    struct Window {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool lines = false;
        uint32_t table = 0u;
    };

    uint8_t vdp1_vram[kVdp1VramSize]{};
    uint16_t vdp1_fb[2][kVdp1FbPlaneWords]{};
    uint16_t vdp1_regs[0x10]{};
    int draw_fb = 0;
    bool change_pending = false;
    bool erase_pending = false;
    bool erase_field = false;
    int sys_x = 319;
    int sys_y = 223;
    int ux0 = 0;
    int uy0 = 0;
    int ux1 = 319;
    int uy1 = 223;
    int local_x = 0;
    int local_y = 0;
    uint64_t frame_changes = 0u;
    uint64_t draws = 0u;
    bool vdp1_unsupported = false;

    uint8_t vdp2_cram[kVdp2CramSize]{};
    uint8_t vdp2_regs[kVdp2RegsSize]{};
    uint32_t frame_px[kMaxFrameW * kMaxFrameH]{};  // 0x00RRGGBB pixels
    int frame_w = 320;
    int frame_h = 224;

    Nbg compose_nbg[4]{};
    Rbg compose_rbg{};
    Window compose_win[2]{};

    struct OffscreenBadge {
        uint8_t active = 0u;
        uint8_t slot = 0u;
        uint8_t sc = 1u;
        uint8_t is_edge = 0u;
        int16_t bx = 0;
        int16_t by = 0;
        float tip_x = 0.0f;
        float tip_y = 0.0f;
        float dir_x = 0.0f;
        float dir_y = 1.0f;
    };
    OffscreenBadge offscreen_badges[2]{};

    struct RbgLineParams {
        int32_t xsp = 0;
        int32_t ysp = 0;
        int32_t xp = 0;
        int32_t yp = 0;
        int32_t ldx = 0;
        int32_t ldy = 0;
        uint32_t ka = 0u;
        uint32_t line_coef = 0u;
        uint32_t line_coef_dot = 0u;
    };

    struct ComposePrepared {
        uint32_t cram_rgb[2048]{};
        uint8_t cram_msb[2048]{};
        uint32_t cram_mask = 0x3FFu;
        bool disp_on = false;
        bool lsmd3 = false;
        bool hires_x = false;
        bool hires_rbg = false;
        bool half_y = false;
        bool rbg_active = false;
        bool rbg_perdot_x = false;
        bool add = false;
        bool sp_cc_en = false;
        bool sp_mixed = false;
        bool sp_off = false;
        bool sp_offb = false;
        bool back_off = false;
        bool back_offb = false;
        bool bk_lines = false;
        int raw_w = 320;
        int raw_h = 224;
        int y_step = 1;
        int x_shift = 0;
        int fb_pitch = 512;
        int active_nbg_count = 0;
        int active_nbgs[4]{};
        int win_sh = 0;
        int sp_type = 0;
        int sp_ccmode = 0;
        int sp_ccnum = 0;
        int sf_prio_mode[5]{};
        int sf_cc_mode[5]{};
        int off_vals[2][3]{};
        uint32_t sp_caos = 0u;
        uint32_t bk = 0u;
        uint32_t bk_base_rgb = 0u;
        uint8_t wctl[6]{};
        uint8_t sp_prio[8]{};
        uint8_t sp_ratio[8]{};
        uint8_t sf_code_byte[5]{};
        uint8_t dbg_hide_layers = 0u;
        uint32_t back_rgb_line[kMaxFrameH]{};
        uint8_t win_y_ok[kMaxFrameH][2]{};
        int16_t win_x0[kMaxFrameH][2]{};
        int16_t win_x1[kMaxFrameH][2]{};
        RbgLineParams rbg_lines[kMaxFrameH][2]{};
    };
    ComposePrepared compose_prep{};

    struct SpriteObs {
        uint32_t srca = 0u;
        uint16_t pmod = 0u;
        int16_t cx = 0;
        int16_t cy = 0;
        uint8_t tex_w = 0u;
        uint8_t tex_h = 0u;
        int16_t draw_w = 0;
        int16_t draw_h = 0;
    };
    static constexpr int kMaxSpriteObs = 1024;
    SpriteObs v1_obs_draw[kMaxSpriteObs]{};
    SpriteObs v1_obs_disp[kMaxSpriteObs]{};
    int v1_obs_draw_count = 0;
    int v1_obs_disp_count = 0;

    struct WindVfxState {
        bool active = false;
        float cx = 0.0f;
        float cy = 0.0f;
        float radius = 16.0f;
        uint32_t frame_age = 0u;
        static constexpr int kTrailLen = 18;
        float trail_x[kTrailLen]{};
        float trail_y[kTrailLen]{};
        int trail_count = 0;
    };
    WindVfxState wind_vfx{};

    struct CopaVfxBox {
        bool active = false;
        bool hflip = false;
        int16_t min_x = 32767;
        int16_t min_y = 32767;
        int16_t max_x = -32768;
        int16_t max_y = -32768;
        uint32_t anim_age = 0u;
    };
    struct WindChargeRay {
        int16_t x0 = 0;
        int16_t y0 = 0;
        int16_t x1 = 0;
        int16_t y1 = 0;
    };
    struct CopaSpVfxState {
        static constexpr int kMaxChargeRays = 16;
        CopaVfxBox condor{};
        CopaVfxBox machu{};
        CopaVfxBox uru_flag{};
        CopaVfxBox draw_condor{};
        CopaVfxBox draw_machu{};
        CopaVfxBox draw_uru_flag{};
        int16_t uru_anchor_x = 0;
        int16_t uru_anchor_y = 0;
        uint8_t uru_anchor_ttl = 0u;
        uint8_t charge_count = 0u;
        uint8_t draw_charge_count = 0u;
        uint16_t _pad_vfx = 0u;
        uint32_t condor_age = 0u;
        uint32_t machu_age = 0u;
        uint32_t uru_age = 0u;
        uint32_t charge_age = 0u;
        int16_t condor_prev_cx = 0;
        int8_t condor_dir = 1;
        uint8_t _pad_vfx2 = 0u;
        WindChargeRay charge_rays[kMaxChargeRays]{};
        WindChargeRay draw_charge_rays[kMaxChargeRays]{};
    };
    CopaSpVfxState copa_sp_vfx{};
    bool copa_vfx_replace = false; // Replace foreign France monument (PER), Germany flag (URU) & BOL sparks only in CopaAmerica

    // Copa America Team Select 3x4 Group Labels overlay ("GRP A/B/C"), drawn only during Team Select.
    bool copa_group_badges = false;

    // B-H5-08 & B-H5-09: Copa America 6-Round + 7th All-Star Bracket & Visual Progress Screen state
    // (POD, zero heap, deterministically serialized & hashed when copa_mode_active == true).
    struct CopaMatchSlot {
        uint8_t team_a = 0xFFu;
        uint8_t team_b = 0xFFu;
        uint8_t goals_a = 0u;
        uint8_t goals_b = 0u;
        uint8_t played = 0u;    // 0 = upcoming, 1 = regulation win/draw, 2 = penalty win
        uint8_t winner = 0xFFu; // 0xFF if TBD or regulation group draw
    };
    struct CopaStandingRow {
        uint8_t team = 0xFFu;
        uint8_t played = 0u;
        uint8_t wins = 0u;
        uint8_t draws = 0u;
        uint8_t losses = 0u;
        uint8_t gf = 0u;
        uint8_t ga = 0u;
        uint8_t pts = 0u;
    };
    struct CopaBracketState {
        uint8_t initialized = 0u;
        uint8_t progress_active = 0u;     // 1 = pause SH-2 & draw Visual Progress Screen overlay
        uint8_t progress_stage = 0u;      // 0..2 = Groups MD1..3, 3 = QF, 4 = SF, 5 = Final, 6 = All-Star
        uint8_t last_shown_round = 0xFFu; // Prevents re-trigger on Continue
        uint8_t player_eliminated = 0u;   // 1 = P1 did not qualify for Top 8 QF
        uint8_t prev_input_action = 0u;   // Edge trigger tracker for Start/A button
        uint8_t next_opp = 0xFFu;         // Resolved opponent slot for progress_stage
        uint8_t live_goals_valid = 0u;
        uint8_t live_p1_goals = 0u;
        uint8_t live_p2_goals = 0u;
        uint8_t last_resolved_round = 0xFFu;
        uint16_t progress_frames = 0u;
        uint16_t _pad1 = 0u;
        uint32_t seed = 0u;
        uint8_t player_team = 0u;
        uint8_t player_group = 0u;
        uint8_t qual_mask[16]{};          // bit0=top2, bit1=best3rd
        uint8_t groups[3][4]{};
        CopaStandingRow standings[3][4]{};
        CopaMatchSlot group_matches[3][6]{}; // [matchday 0..2][group*2 + idx]
        CopaMatchSlot qf[4]{};
        CopaMatchSlot sf[2]{};
        CopaMatchSlot final_match{};
        CopaMatchSlot all_star{};
    };
    CopaBracketState copa_bracket{};

    // Adaptive 224p progressive composition for mobile/low-end hosts (skips redundant interlace half-lines).
    bool low_res_224p = false;
    bool low_res_half_width = false;

    // Hardware WebGL2 VDP1 metadata split (defaults false for 100% bit-exact PC/CLI behavior unless enabled).
    bool gpu_vdp1_enabled = false;
    static constexpr int kMaxV1GpuCmds = 2048;
    struct V1GpuCmd {
        uint16_t ctrl = 0u;
        uint16_t pmod = 0u;
        uint16_t colr = 0u;
        uint16_t _pad = 0u;
        uint32_t srca = 0u;
        uint32_t gouraud_addr = 0u;
        int16_t w = 0;
        int16_t h = 0;
        int16_t x[4]{};
        int16_t y[4]{};
        int16_t ux0 = 0;
        int16_t uy0 = 0;
        int16_t ux1 = 0;
        int16_t uy1 = 0;
        int16_t sys_x = 0;
        int16_t sys_y = 0;
    };
    static_assert(sizeof(V1GpuCmd) == 48u, "V1GpuCmd must remain compact 48 bytes");

    struct V1GpuFrame {
        bool valid = false;
        bool cpu_materialized = false;
        uint16_t count = 0u;
        uint16_t tvmr = 0u;
        uint16_t ewdr = 0u;
        int16_t ew_x0 = 0;
        int16_t ew_y0 = 0;
        int16_t ew_x1 = 0;
        int16_t ew_y1 = 0;
        int16_t fb_w = 512;
        int16_t fb_h = 256;
        V1GpuCmd cmds[kMaxV1GpuCmds]{};
    };
    V1GpuFrame v1_gpu_frames[2]{};

    // Non-serialized 3D software rasterizer scratch buffers for PER Condor & Machu Picchu SP VFX
    static constexpr int kPeruZBufMaxW = 400;
    static constexpr int kPeruZBufMaxH = 340;
    struct PeruProjVert {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
    uint16_t peru_zbuf[kPeruZBufMaxW * kPeruZBufMaxH]{};
    PeruProjVert peru_proj[2624]{};

    void init();
    void vblank_out();
    void compose_frame_prepare(const uint8_t vdp2_vram[0x80000], uint8_t dbg_hide_layers = 0u);
    void compose_frame_band(const uint8_t vdp2_vram[0x80000], int out_y_begin, int out_y_end);
    void compose_frame_post();
    void compose_frame(const uint8_t vdp2_vram[0x80000], uint8_t dbg_hide_layers = 0u);
    bool has_cpu_only_overlays() const;

    void set_gpu_vdp1(bool enabled);
    void invalidate_gpu_vdp1_frames();
    void materialize_cpu_fb(int which);
    void materialize_cpu_display_fb() { materialize_cpu_fb(draw_fb ^ 1); }
    const V1GpuFrame& vdp1_gpu_display() const { return v1_gpu_frames[draw_fb ^ 1]; }

    uint32_t vdp1_reg_read(uint32_t off, int size) const;
    void vdp1_reg_write(uint32_t off, uint32_t val, int size);
    uint32_t vdp1_fb_read(uint32_t off, int size) const;
    void vdp1_fb_write(uint32_t off, uint32_t val, int size);

    const uint16_t* vdp1_display() const { return vdp1_fb[draw_fb ^ 1]; }
    int vdp2_lines() const;

private:
    int fb_width() const { return (vdp1_regs[TVMR] & 1u) ? 1024 : 512; }
    int fb_height() const { return (vdp1_regs[TVMR] & 1u) ? 512 : 256; }
    uint16_t v1w(uint32_t a) const {
        a &= 0x7FFFEu;
        return static_cast<uint16_t>((vdp1_vram[a] << 8) | vdp1_vram[a + 1u]);
    }
    void v1_draw();
    void v1_draw_meta_only();
    void v1_erase(int which);
    void v1_record_erase(int which);
    void v1_reg_write16(uint32_t off, uint16_t v);
    uint16_t v2reg(uint32_t off) const {
        return static_cast<uint16_t>((vdp2_regs[off] << 8) | vdp2_regs[off + 1u]);
    }
};

}  // namespace twc98
