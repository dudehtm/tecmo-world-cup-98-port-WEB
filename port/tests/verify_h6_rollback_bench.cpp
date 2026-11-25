#include "sha256.hpp"
#include "stv_machine.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return sz == 0 || static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), sz));
}

std::string digest_to_hex(const uint8_t d[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64u, '0');
    for (size_t i = 0u; i < 32u; ++i) {
        out[i * 2u + 0u] = kHex[(d[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = kHex[d[i] & 0xFu];
    }
    return out;
}

struct FrameInputs {
    uint8_t port_a = 0xFFu;
    uint8_t port_b = 0xFFu;
    uint8_t port_c = 0xFFu;
};

FrameInputs true_match_input(uint32_t f) {
    FrameInputs in{};
    // Vary P1 and P2 directional + pass/shoot/tackle inputs across frames
    const uint8_t dirs[8] = {0x7Fu, 0xBFu, 0xDFu, 0xEFu, 0x5Fu, 0x9Fu, 0xAFu, 0xFFu};
    uint8_t pa = dirs[(f / 6u) & 7u];
    uint8_t pb = dirs[((f + 19u) / 5u) & 7u];
    if ((f % 11u) < 3u) pa &= static_cast<uint8_t>(~0x01u);  // P1 B1 (Pass/Switch)
    if ((f % 17u) == 4u) pa &= static_cast<uint8_t>(~0x02u); // P1 B2 (Shoot/Slide)
    if ((f % 13u) < 2u) pb &= static_cast<uint8_t>(~0x01u);  // P2 B1
    if ((f % 23u) == 7u) pb &= static_cast<uint8_t>(~0x04u); // P2 B3
    in.port_a = pa;
    in.port_b = pb;
    in.port_c = 0xFFu;
    return in;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string g4_dir = (argc >= 2) ? argv[1] : "port";
    const std::string vis_dir = ".local/experiments/012-h4-control-trace/visual-sync1027b-neutral";

    std::vector<uint8_t> cart, bios, ep_cp;
    std::vector<uint8_t> v1_vram, v1_regs, v2_vram, v2_cram, v2_regs;
    if (!read_file_bytes(g4_dir + "/data/twcup98_cart_20m.bin", cart) ||
        !read_file_bytes(g4_dir + "/data/bios_euro_be.bin", bios) ||
        !read_file_bytes(g4_dir + "/data/endpoint_frame8175.cp", ep_cp) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp1-vram.bin", v1_vram) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp1-registers.bin", v1_regs) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-vram.bin", v2_vram) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-cram.bin", v2_cram) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-registers.bin", v2_regs)) {
        std::cerr << "FAIL: missing input binaries under " << g4_dir << "\n";
        return 1;
    }

    auto init_machine = [&](twc98::StvMachineState& m) -> bool {
        const char* err = nullptr;
        if (!m.init_from_cpn8_bytes(ep_cp.data(), ep_cp.size(), bios.data(), bios.size(), &err)) {
            std::cerr << "FAIL: init_from_cpn8_bytes: " << (err ? err : "unknown") << "\n";
            return false;
        }
        if (!m.load_cart_rom(cart.data(), cart.size())) {
            std::cerr << "FAIL: load_cart_rom\n";
            return false;
        }
        m.seed_vdp_snapshot(v1_vram.data(), v1_vram.size(),
                            v1_regs.data(), v1_regs.size(),
                            v2_vram.data(), v2_vram.size(),
                            v2_cram.data(), v2_cram.size(),
                            v2_regs.data(), v2_regs.size());
        m.set_gk_ai_fix(true);
        m.set_real_audio(true);
        // Warm up 40 frames into active match play with P1/P2 start pressed early
        for (uint32_t w = 0u; w < 40u; ++w) {
            const uint8_t pc_in = (w >= 4u && w < 10u) ? 0xCFu : 0xFFu;
            m.set_input_ports(0xFFu, 0xFFu, pc_in, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
            if (!m.run_rounds(10000u)) return false;
            m.render_frame();
        }
        m.clear_audio_pcm();
        return true;
    };

    auto m_linear = std::make_unique<twc98::StvMachineState>();
    auto m_rollback = std::make_unique<twc98::StvMachineState>();
    if (!init_machine(*m_linear) || !init_machine(*m_rollback)) {
        return 1;
    }

    static constexpr uint32_t kRingSlots = 8u;
    std::vector<std::vector<uint8_t>> rb_ring(
        kRingSlots, std::vector<uint8_t>(twc98::StvMachineState::kMaxFastRollbackBytes, 0u));
    uint32_t rb_ring_len[kRingSlots]{};

    // Prime the 8 rollback slots once so page-diff operates on warmed slot pages
    for (uint32_t s = 0u; s < kRingSlots; ++s) {
        rb_ring_len[s] = m_rollback->save_fast_rollback(rb_ring[s].data(), rb_ring[s].size());
        if (rb_ring_len[s] == 0u) {
            std::cerr << "FAIL: initial save_fast_rollback returned 0\n";
            return 1;
        }
    }

    static constexpr uint32_t kTestFrames = 120u;
    double save_total_ms = 0.0, save_max_ms = 0.0;
    uint32_t save_calls = 0u;
    double load_total_ms = 0.0, load_max_ms = 0.0;
    uint32_t load_calls = 0u;
    double step_norender_total_ms = 0.0;
    uint32_t step_norender_calls = 0u;
    double step_render_total_ms = 0.0;
    uint32_t step_render_calls = 0u;
    double hash_total_ms = 0.0;
    uint32_t hash_calls = 0u;

    uint32_t linear_ring_consumed = 0u;
    uint32_t rollback_ring_consumed = 0u;
    std::vector<int16_t> pcm_LinearAll;
    std::vector<int16_t> pcm_RollbackAll;
    int16_t pcm_tmp[2048 * 2]{};

    uint32_t rollback_events = 0u;
    uint32_t resim_frames_total = 0u;

    // 1. Run linear reference machine across 120 frames
    for (uint32_t f = 0u; f < kTestFrames; ++f) {
        const FrameInputs in = true_match_input(f);
        m_linear->set_input_ports(in.port_a, in.port_b, in.port_c, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
        const auto t0 = std::chrono::steady_clock::now();
        if (!m_linear->run_rounds(10000u)) {
            std::cerr << "FAIL: m_linear halted at frame " << f << "\n";
            return 1;
        }
        m_linear->render_frame();
        const auto t1 = std::chrono::steady_clock::now();
        step_render_total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ++step_render_calls;

        const uint32_t got = m_linear->consume_audio_pcm(pcm_tmp, 2048u);
        linear_ring_consumed += got;
        for (uint32_t i = 0u; i < got * 2u; ++i) pcm_LinearAll.push_back(pcm_tmp[i]);

        const auto th0 = std::chrono::steady_clock::now();
        volatile uint64_t sh = m_linear->compute_sync_hash64();
        (void)sh;
        const auto th1 = std::chrono::steady_clock::now();
        hash_total_ms += std::chrono::duration<double, std::milli>(th1 - th0).count();
        ++hash_calls;
    }

    // 2. Run rollback machine across 120 frames, injecting mispredictions every 5 frames (depths 1, 2, 3)
    for (uint32_t f = 0u; f < kTestFrames; ++f) {
        const uint32_t slot = f % kRingSlots;
        const auto ts0 = std::chrono::steady_clock::now();
        rb_ring_len[slot] = m_rollback->save_fast_rollback(rb_ring[slot].data(), rb_ring[slot].size());
        const auto ts1 = std::chrono::steady_clock::now();
        const double sms = std::chrono::duration<double, std::milli>(ts1 - ts0).count();
        save_total_ms += sms;
        if (sms > save_max_ms) save_max_ms = sms;
        ++save_calls;

        // Every 5 frames (at f = 4, 9, 14, ...), simulate that the last `depth` frames (1..3)
        // had a mispredicted P2 input on the live path, then roll back and re-simulate with true inputs!
        const bool trigger_rb = ((f % 5u) == 4u) && (f >= 3u);
        const FrameInputs in = true_match_input(f);
        // If we are inside the misprediction window right before a rollback trigger, feed wrong P2 input first
        uint8_t live_pb = in.port_b;
        const uint32_t next_rb_f = (f / 5u) * 5u + 4u;
        const uint32_t next_depth = 1u + ((next_rb_f / 5u) % 3u); // 1, 2, or 3 frames
        if (next_rb_f < kTestFrames && f >= (next_rb_f + 1u - next_depth) && f <= next_rb_f) {
            live_pb ^= 0xF3u;  // deliberately wrong predicted P2 input
        }

        m_rollback->set_input_ports(in.port_a, live_pb, in.port_c, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
        if (!m_rollback->run_rounds_ex(10000u, /*emit_audio_ring=*/true)) {
            std::cerr << "FAIL: m_rollback halted on live step at frame " << f << "\n";
            return 1;
        }
        m_rollback->render_frame();
        const uint32_t got = m_rollback->consume_audio_pcm(pcm_tmp, 2048u);
        rollback_ring_consumed += got;
        for (uint32_t i = 0u; i < got * 2u; ++i) pcm_RollbackAll.push_back(pcm_tmp[i]);

        if (trigger_rb) {
            const uint32_t depth = next_depth;
            const uint32_t start_f = f + 1u - depth;
            const uint32_t start_slot = start_f % kRingSlots;

            const auto tl0 = std::chrono::steady_clock::now();
            if (!m_rollback->load_fast_rollback(rb_ring[start_slot].data(), rb_ring_len[start_slot])) {
                std::cerr << "FAIL: load_fast_rollback failed at frame " << f << "\n";
                return 1;
            }
            const auto tl1 = std::chrono::steady_clock::now();
            const double lms = std::chrono::duration<double, std::milli>(tl1 - tl0).count();
            load_total_ms += lms;
            if (lms > load_max_ms) load_max_ms = lms;
            ++load_calls;

            for (uint32_t rf = start_f; rf <= f; ++rf) {
                if (rf > start_f) {
                    const uint32_t rslot = rf % kRingSlots;
                    const auto rts0 = std::chrono::steady_clock::now();
                    rb_ring_len[rslot] = m_rollback->save_fast_rollback(rb_ring[rslot].data(), rb_ring[rslot].size());
                    const auto rts1 = std::chrono::steady_clock::now();
                    const double rsms = std::chrono::duration<double, std::milli>(rts1 - rts0).count();
                    save_total_ms += rsms;
                    if (rsms > save_max_ms) save_max_ms = rsms;
                    ++save_calls;
                }
                const FrameInputs true_in = true_match_input(rf);
                m_rollback->set_input_ports(true_in.port_a, true_in.port_b, true_in.port_c,
                                            0xFFu, 0xFFu, 0x7Fu, 0x67u);
                if (rf < f) {
                    const auto tr0 = std::chrono::steady_clock::now();
                    if (!m_rollback->step_frame_no_render(10000u, /*emit_audio_ring=*/false)) {
                        std::cerr << "FAIL: step_frame_no_render failed at rf=" << rf << "\n";
                        return 1;
                    }
                    const auto tr1 = std::chrono::steady_clock::now();
                    step_norender_total_ms += std::chrono::duration<double, std::milli>(tr1 - tr0).count();
                    ++step_norender_calls;
                } else {
                    // Present frame f after rollback: step with emit_audio_ring=false (audio slot already emitted) + render_frame()
                    const auto tr0 = std::chrono::steady_clock::now();
                    if (!m_rollback->run_rounds_ex(10000u, /*emit_audio_ring=*/false)) {
                        std::cerr << "FAIL: run_rounds_ex failed at rf=" << rf << "\n";
                        return 1;
                    }
                    const auto tr1 = std::chrono::steady_clock::now();
                    step_norender_total_ms += std::chrono::duration<double, std::milli>(tr1 - tr0).count();
                    ++step_norender_calls;
                    m_rollback->render_frame();
                }
                ++resim_frames_total;
            }
            ++rollback_events;
        }
    }

    // 3. Compare final states between m_linear and m_rollback
    const uint64_t sync_lin = m_linear->compute_sync_hash64();
    const uint64_t sync_rb = m_rollback->compute_sync_hash64();

    std::vector<uint8_t> snap_lin(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    std::vector<uint8_t> snap_rb(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    const uint32_t slin_len = m_linear->save_state_bytes(snap_lin.data(), snap_lin.size());
    const uint32_t srb_len = m_rollback->save_state_bytes(snap_rb.data(), snap_rb.size());

    uint8_t sha_snap_lin[32]{}, sha_snap_rb[32]{};
    twc98::sha256_bytes(snap_lin.data(), slin_len, sha_snap_lin);
    twc98::sha256_bytes(snap_rb.data(), srb_len, sha_snap_rb);

    uint8_t d_lin[7][32]{}, d_rb[7][32]{};
    m_linear->compute_digests(d_lin[0], d_lin[1], d_lin[2], d_lin[3], d_lin[4], d_lin[5], d_lin[6]);
    m_rollback->compute_digests(d_rb[0], d_rb[1], d_rb[2], d_rb[3], d_rb[4], d_rb[5], d_rb[6]);

    const std::string snap_lin_hex = digest_to_hex(sha_snap_lin);
    const std::string snap_rb_hex = digest_to_hex(sha_snap_rb);
    const std::string frame_lin_hex = digest_to_hex(d_lin[6]);
    const std::string frame_rb_hex = digest_to_hex(d_rb[6]);

    const double avg_save_ms = save_calls ? (save_total_ms / save_calls) : 0.0;
    const double avg_load_ms = load_calls ? (load_total_ms / load_calls) : 0.0;
    const double avg_step_norender_ms = step_norender_calls ? (step_norender_total_ms / step_norender_calls) : 0.0;
    const double avg_step_render_ms = step_render_calls ? (step_render_total_ms / step_render_calls) : 0.0;
    const double avg_hash_ms = hash_calls ? (hash_total_ms / hash_calls) : 0.0;
    const double rb3_total_ms = avg_load_ms + 2.0 * avg_save_ms + 3.0 * avg_step_norender_ms;

    const bool exact_sync = (sync_lin == sync_rb);
    const bool exact_snap = (slin_len == srb_len) && (snap_lin_hex == snap_rb_hex);
    const bool exact_frame = (frame_lin_hex == frame_rb_hex);
    const bool exact_audio_ring = (linear_ring_consumed == kTestFrames * 735u) &&
                                  (rollback_ring_consumed == kTestFrames * 735u) &&
                                  (m_rollback->audio_ring_count == 0u) &&
                                  (m_linear->audio_pcm_produced == m_rollback->audio_pcm_produced) &&
                                  (m_linear->audio_pcm_nonzero == m_rollback->audio_pcm_nonzero);
    const bool perf_pass = (avg_save_ms < 1.5) && (avg_load_ms < 1.5);
    const bool all_pass = exact_sync && exact_snap && exact_frame && exact_audio_ring && perf_pass;

    std::printf("H6-01 Rollback Bench: status=%s\n", all_pass ? "PASS" : "FAIL");
    std::printf("  rollback_events=%u resim_frames=%u fast_rb_bytes=%u\n",
                rollback_events, resim_frames_total, rb_ring_len[0]);
    std::printf("  save_fast_rollback: avg=%.3f ms max=%.3f ms (calls=%u)\n",
                avg_save_ms, save_max_ms, save_calls);
    std::printf("  load_fast_rollback: avg=%.3f ms max=%.3f ms (calls=%u)\n",
                avg_load_ms, load_max_ms, load_calls);
    std::printf("  step_frame_no_render: avg=%.3f ms vs step+render: avg=%.3f ms (render_saved=%.3f ms/f)\n",
                avg_step_norender_ms, avg_step_render_ms, avg_step_render_ms - avg_step_norender_ms);
    std::printf("  compute_sync_hash64: avg=%.3f ms | 3-frame rollback total=%.3f ms\n",
                avg_hash_ms, rb3_total_ms);
    std::printf("  sync_hash64: linear=0x%016llx rollback=0x%016llx match=%d\n",
                static_cast<unsigned long long>(sync_lin),
                static_cast<unsigned long long>(sync_rb),
                exact_sync ? 1 : 0);
    std::printf("  snapshot_sha256: %s (match=%d)\n", snap_rb_hex.c_str(), exact_snap ? 1 : 0);
    std::printf("  frame_px_sha256: %s (match=%d)\n", frame_rb_hex.c_str(), exact_frame ? 1 : 0);
    std::printf("  audio_ring_frames: linear=%u rollback=%u pcm_produced=%u pcm_nonzero=%u (match=%d)\n",
                linear_ring_consumed, rollback_ring_consumed,
                m_rollback->audio_pcm_produced, m_rollback->audio_pcm_nonzero,
                exact_audio_ring ? 1 : 0);

    return all_pass ? 0 : 1;
}
