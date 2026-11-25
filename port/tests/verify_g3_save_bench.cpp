#include "sha256.hpp"
#include "stv_machine.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
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

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return std::string(buf);
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

uint64_t query_peak_rss_bytes() {
#ifdef _WIN32
    struct Pmc {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
    };
    using FnGetPmc = BOOL(WINAPI*)(HANDLE, Pmc*, DWORD);
    HMODULE h = LoadLibraryA("psapi.dll");
    if (h) {
        auto fn = reinterpret_cast<FnGetPmc>(GetProcAddress(h, "GetProcessMemoryInfo"));
        if (fn) {
            Pmc pmc{};
            pmc.cb = sizeof(pmc);
            if (fn(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                FreeLibrary(h);
                return static_cast<uint64_t>(pmc.PeakWorkingSetSize);
            }
        }
        FreeLibrary(h);
    }
#endif
    return 0u;
}

struct StateDigest {
    uint32_t rounds_done = 0u;
    uint32_t main_pc = 0u;
    uint32_t slave_pc = 0u;
    uint32_t ioga_reads = 0u;
    uint32_t cart_reads = 0u;
    uint32_t vdp1_draws = 0u;
    std::string hw, lw, v1, v2, frame_sha;
};

StateDigest snapshot_digests(twc98::StvMachineState& m) {
    m.render_frame();
    uint8_t d[7][32]{};
    m.compute_digests(d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
    StateDigest out{};
    out.rounds_done = m.rounds_done;
    out.main_pc = m.cpus[0].pc;
    out.slave_pc = m.cpus[1].pc;
    out.ioga_reads = m.ioga_reads;
    out.cart_reads = m.cart_reads;
    out.vdp1_draws = static_cast<uint32_t>(m.vdp.draws);
    out.hw = digest_to_hex(d[0]);
    out.lw = digest_to_hex(d[1]);
    out.v1 = digest_to_hex(d[2]);
    out.v2 = digest_to_hex(d[3]);
    out.frame_sha = digest_to_hex(d[6]);
    return out;
}

}  // namespace

int main() {
    const std::string g3 = "port";
    const std::string vis = ".local/experiments/012-h4-control-trace/visual-sync1027b-neutral";

    const auto t_init0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> cart, bios, ep_cp;
    std::vector<uint8_t> v1_vram, v1_regs, v2_vram, v2_cram, v2_regs;
    if (!read_file_bytes(g3 + "/data/twcup98_cart_20m.bin", cart) ||
        !read_file_bytes(g3 + "/data/bios_euro_be.bin", bios) ||
        !read_file_bytes(g3 + "/data/endpoint_frame8175.cp", ep_cp) ||
        !read_file_bytes(vis + "/control-window-start-vdp1-vram.bin", v1_vram) ||
        !read_file_bytes(vis + "/control-window-start-vdp1-registers.bin", v1_regs) ||
        !read_file_bytes(vis + "/control-window-start-vdp2-vram.bin", v2_vram) ||
        !read_file_bytes(vis + "/control-window-start-vdp2-cram.bin", v2_cram) ||
        !read_file_bytes(vis + "/control-window-start-vdp2-registers.bin", v2_regs)) {
        std::cerr << "Failed reading assets\n";
        return 1;
    }

    auto m = std::make_unique<twc98::StvMachineState>();
    m->load_cart_rom(cart.data(), cart.size());
    const char* err = nullptr;
    if (!m->init_from_cpn8_bytes(ep_cp.data(), ep_cp.size(), bios.data(), bios.size(), &err)) {
        std::cerr << "init_from_cpn8_bytes failed: " << (err ? err : "unknown") << "\n";
        return 1;
    }
    m->seed_vdp_snapshot(
        v1_vram.data(), v1_vram.size(),
        v1_regs.data(), v1_regs.size(),
        v2_vram.data(), v2_vram.size(),
        v2_cram.data(), v2_cram.size(),
        v2_regs.data(), v2_regs.size());
    const auto t_init1 = std::chrono::steady_clock::now();
    const double cold_init_ms = std::chrono::duration<double, std::milli>(t_init1 - t_init0).count();

    auto step_range = [&](uint32_t f_start, uint32_t f_end) -> bool {
        for (uint32_t f = f_start; f < f_end; ++f) {
            uint8_t pa = 0xFFu, pb = 0xFFu, pc = 0xFFu;
            if (f >= 5u && f <= 10u) pc = 0xFEu;
            else if (f >= 25u && f <= 34u) pc = 0xDFu;
            if (f >= 60u) {
                if ((f % 60u) < 20u) pa = 0xDFu;
                else if ((f % 45u) < 6u) pa = 0xFEu;
                pb = ((f % 40u) < 20u) ? 0x7Eu : 0xEFu;
            }
            m->set_input_ports(pa, pb, pc, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
            if (!m->run_rounds(10000u)) return false;
        }
        return true;
    };

    // Advance 60 frames, save snapshot
    if (!step_range(0u, 60u)) return 2;
    const StateDigest f60_ref = snapshot_digests(*m);

    std::vector<uint8_t> snap(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    const uint32_t snap_len = m->save_state_bytes(snap.data(), snap.size());
    if (snap_len == 0u || snap_len >= snap.size()) {
        std::cerr << "save_state_bytes failed len=" << snap_len << "\n";
        return 3;
    }
    uint8_t snap_sha_raw[32]{};
    twc98::sha256_bytes(snap.data(), snap_len, snap_sha_raw);
    const std::string snap_sha = digest_to_hex(snap_sha_raw);

    // Negative integrity checks on corrupted snapshots
    std::vector<uint8_t> bad(snap.begin(), snap.begin() + snap_len);
    bad[0] ^= 0xFFu;
    const char* bad_err = nullptr;
    if (m->load_state_bytes(bad.data(), bad.size(), &bad_err)) {
        std::cerr << "Corrupt magic should fail\n";
        return 4;
    }
    bad.assign(snap.begin(), snap.begin() + snap_len);
    bad[1000u] ^= 0x5Au;
    if (m->load_state_bytes(bad.data(), bad.size(), &bad_err)) {
        std::cerr << "Corrupt checksum should fail\n";
        return 4;
    }
    if (m->load_state_bytes(snap.data(), 24u, &bad_err)) {
        std::cerr << "Truncated snapshot should fail\n";
        return 4;
    }

    // Advance 180 frames cleanly -> f=240 reference
    if (!step_range(60u, 240u)) return 5;
    const StateDigest f240_ref = snapshot_digests(*m);

    // Restore f=60 snapshot and verify immediate f=60 match + bit-exact replay to f=240
    if (!m->load_state_bytes(snap.data(), snap_len, &bad_err)) {
        std::cerr << "load_state_bytes valid failed: " << (bad_err ? bad_err : "unknown") << "\n";
        return 6;
    }
    const StateDigest f60_restored = snapshot_digests(*m);
    if (f60_restored.hw != f60_ref.hw || f60_restored.lw != f60_ref.lw ||
        f60_restored.v1 != f60_ref.v1 || f60_restored.v2 != f60_ref.v2 ||
        f60_restored.frame_sha != f60_ref.frame_sha ||
        f60_restored.main_pc != f60_ref.main_pc || f60_restored.slave_pc != f60_ref.slave_pc) {
        std::cerr << "Immediate post-restore digest mismatch\n";
        return 7;
    }

    if (!step_range(60u, 240u)) return 8;
    const StateDigest f240_replay = snapshot_digests(*m);
    if (f240_replay.hw != f240_ref.hw || f240_replay.lw != f240_ref.lw ||
        f240_replay.v1 != f240_ref.v1 || f240_replay.v2 != f240_ref.v2 ||
        f240_replay.frame_sha != f240_ref.frame_sha ||
        f240_replay.main_pc != f240_ref.main_pc || f240_replay.slave_pc != f240_ref.slave_pc ||
        f240_replay.cart_reads != f240_ref.cart_reads || f240_replay.ioga_reads != f240_ref.ioga_reads) {
        std::cerr << "Post-restore replay to f240 diverged\n";
        return 9;
    }

    // Time 10 save + load iterations
    double save_ms_total = 0.0;
    double load_ms_total = 0.0;
    for (int it = 0; it < 10; ++it) {
        const auto ts0 = std::chrono::steady_clock::now();
        const uint32_t n = m->save_state_bytes(snap.data(), snap.size());
        const auto ts1 = std::chrono::steady_clock::now();
        if (n != snap_len) return 10;
        if (!m->load_state_bytes(snap.data(), n, &bad_err)) return 10;
        const auto ts2 = std::chrono::steady_clock::now();
        save_ms_total += std::chrono::duration<double, std::milli>(ts1 - ts0).count();
        load_ms_total += std::chrono::duration<double, std::milli>(ts2 - ts1).count();
    }
    const double save_ms_avg = save_ms_total / 10.0;
    const double load_ms_avg = load_ms_total / 10.0;

    // Warm 600-frame B-H4-06 throughput benchmark (includes per-frame render_frame!)
    const auto tb0 = std::chrono::steady_clock::now();
    for (uint32_t f = 240u; f < 840u; ++f) {
        uint8_t pa = ((f % 60u) < 20u) ? 0xDFu : (((f % 45u) < 6u) ? 0xFEu : 0xFFu);
        uint8_t pb = ((f % 40u) < 20u) ? 0x7Eu : 0xEFu;
        m->set_input_ports(pa, pb, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
        if (!m->run_rounds(10000u)) return 11;
        m->render_frame();
    }
    const auto tb1 = std::chrono::steady_clock::now();
    const double bench_600f_ms = std::chrono::duration<double, std::milli>(tb1 - tb0).count();
    const double bench_fps = 600.0 * 1000.0 / (bench_600f_ms > 0.0 ? bench_600f_ms : 1.0);
    const double bench_ms_per_frame = bench_600f_ms / 600.0;
    const double realtime_ratio = bench_fps / 60.0;
    const StateDigest f840_end = snapshot_digests(*m);
    const uint64_t peak_rss = query_peak_rss_bytes();

    std::filesystem::create_directories(g3 + "/validation_g4/native");
    std::ostringstream oss;
    oss << "{\n"
        << "  \"schema\": \"g3_save_bench_native_v1\",\n"
        << "  \"snapshot_bytes\": " << snap_len << ",\n"
        << "  \"snapshot_sha256\": \"" << snap_sha << "\",\n"
        << "  \"negative_corruption_checks_ok\": true,\n"
        << "  \"rewind_bit_exact_ok\": true,\n"
        << "  \"save_ms_avg\": " << save_ms_avg << ",\n"
        << "  \"load_ms_avg\": " << load_ms_avg << ",\n"
        << "  \"cold_init_ms\": " << cold_init_ms << ",\n"
        << "  \"warm_600f_with_render_ms\": " << bench_600f_ms << ",\n"
        << "  \"warm_fps\": " << bench_fps << ",\n"
        << "  \"warm_ms_per_frame\": " << bench_ms_per_frame << ",\n"
        << "  \"realtime_ratio_60fps\": " << realtime_ratio << ",\n"
        << "  \"peak_rss_bytes\": " << peak_rss << ",\n"
        << "  \"f240_main_pc\": \"" << hex32(f240_replay.main_pc) << "\",\n"
        << "  \"f240_slave_pc\": \"" << hex32(f240_replay.slave_pc) << "\",\n"
        << "  \"f240_high_wram_sha256\": \"" << f240_replay.hw << "\",\n"
        << "  \"f240_frame_sha256\": \"" << f240_replay.frame_sha << "\",\n"
        << "  \"f840_main_pc\": \"" << hex32(f840_end.main_pc) << "\",\n"
        << "  \"f840_cart_reads\": " << f840_end.cart_reads << ",\n"
        << "  \"f840_frame_sha256\": \"" << f840_end.frame_sha << "\"\n"
        << "}\n";

    std::ofstream ofs(g3 + "/validation_g4/native/save_bench_report.json");
    ofs << oss.str();

    std::printf(
        "PASS_NATIVE_SAVE_BENCH snap=%uB save=%.2fms load=%.2fms init=%.1fms 600f_render=%.1fms (%.1f fps, %.2f ms/f, %.2fx) rss=%.1fMiB snap_sha=%s f240=%s f840=%s\n",
        snap_len, save_ms_avg, load_ms_avg, cold_init_ms, bench_600f_ms,
        bench_fps, bench_ms_per_frame, realtime_ratio,
        static_cast<double>(peak_rss) / (1024.0 * 1024.0),
        snap_sha.substr(0, 12).c_str(),
        f240_replay.frame_sha.substr(0, 12).c_str(),
        f840_end.frame_sha.substr(0, 12).c_str());
    return 0;
}
