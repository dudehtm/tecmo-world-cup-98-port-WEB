#include "sha256.hpp"
#include "stv_machine.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        err = "Failed to open file: " + path;
        return false;
    }
    const std::streamsize sz = ifs.tellg();
    if (sz < 0) {
        err = "Failed to stat file: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (sz > 0 && !ifs.read(reinterpret_cast<char*>(out.data()), sz)) {
        err = "Failed to read file: " + path;
        return false;
    }
    return true;
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return std::string(buf);
}

std::string hex8(uint8_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", static_cast<unsigned>(v));
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

struct CartSampleSpec {
    uint32_t addr;
    const char* filename;
};

constexpr CartSampleSpec kCartSamples[17] = {
    {0x02000000u, "cart-02000000.bin"},
    {0x02000100u, "cart-02000100.bin"},
    {0x021FFFC0u, "cart-021fffc0.bin"},
    {0x02200000u, "cart-02200000.bin"},
    {0x022FFFC0u, "cart-022fffc0.bin"},
    {0x02300000u, "cart-02300000.bin"},
    {0x023FFFC0u, "cart-023fffc0.bin"},
    {0x02400000u, "cart-02400000.bin"},
    {0x027FFFC0u, "cart-027fffc0.bin"},
    {0x02800000u, "cart-02800000.bin"},
    {0x02BFFFC0u, "cart-02bfffc0.bin"},
    {0x02C00000u, "cart-02c00000.bin"},
    {0x02FFFFC0u, "cart-02ffffc0.bin"},
    {0x03000000u, "cart-03000000.bin"},
    {0x033FFFC0u, "cart-033fffc0.bin"},
    {0x03400000u, "cart-03400000.bin"},
    {0x04FFFFB0u, "cart-04ffffb0.bin"},
};

struct ScenarioSpec {
    const char* name;
    uint8_t port_a;
    uint8_t port_b;
    uint8_t port_c;
};

constexpr ScenarioSpec kScenarios[5] = {
    {"neutral",    0xFFu, 0xFFu, 0xFFu},
    {"p1_up",      0xDFu, 0xFFu, 0xFFu},
    {"p1_left",    0x7Fu, 0xFFu, 0xFFu},
    {"p2_act",     0xFFu, 0xBEu, 0xFFu},
    {"coin_start", 0xFFu, 0xFFu, 0xEEu},
};

struct ScenarioResult {
    std::string name;
    bool completed = false;
    bool halted = false;
    std::string fault_reason;
    uint32_t fault_pc = 0u;
    uint32_t fault_addr = 0u;
    uint32_t fault_opcode = 0u;
    uint32_t rounds_done = 0u;
    uint32_t main_steps = 0u;
    uint32_t slave_steps = 0u;
    uint32_t main_pc = 0u;
    uint32_t slave_pc = 0u;
    uint32_t ioga_reads = 0u;
    uint32_t ioga_writes = 0u;
    uint32_t smpc_input_reads = 0u;
    uint32_t cart_reads = 0u;
    uint32_t vbi_count = 0u;
    uint32_t vbo_count = 0u;
    uint32_t sched2_hits = 0u;
    uint32_t vdp1_draws = 0u;
    uint32_t mmio_log_count = 0u;
    std::string high_wram_sha;
    std::string low_wram_sha;
    std::string vdp1_vram_sha;
    std::string vdp2_vram_sha;
    std::string frame_sha;
    std::vector<twc98::IogaAccessEntry> first_ioga;
};

ScenarioResult run_cp_scenario(
    const std::string& label,
    const std::vector<uint8_t>& cp_bytes,
    const std::vector<uint8_t>& bios_bytes,
    const std::vector<uint8_t>& cart_bytes,
    const std::vector<uint8_t>* v1_vram,
    const std::vector<uint8_t>* v1_regs,
    const std::vector<uint8_t>* v2_vram,
    const std::vector<uint8_t>* v2_cram,
    const std::vector<uint8_t>* v2_regs,
    uint8_t port_a, uint8_t port_b, uint8_t port_c,
    uint32_t num_frames) {
    ScenarioResult res;
    res.name = label;
    auto m = std::make_unique<twc98::StvMachineState>();
    if (!cart_bytes.empty()) {
        if (!m->load_cart_rom(cart_bytes.data(), cart_bytes.size())) {
            res.fault_reason = "cart_load_failed";
            return res;
        }
    }
    const char* err = nullptr;
    if (!m->init_from_cpn8_bytes(cp_bytes.data(), cp_bytes.size(), bios_bytes.data(), bios_bytes.size(), &err)) {
        res.fault_reason = err ? err : "init_failed";
        return res;
    }
    if (v1_vram && v1_regs && v2_vram && v2_cram && v2_regs) {
        m->seed_vdp_snapshot(
            v1_vram->data(), v1_vram->size(),
            v1_regs->data(), v1_regs->size(),
            v2_vram->data(), v2_vram->size(),
            v2_cram->data(), v2_cram->size(),
            v2_regs->data(), v2_regs->size());
    }
    m->set_input_ports(port_a, port_b, port_c, 0xFFu, 0xFFu, 0x7Fu, 0x67u);

    bool ok = true;
    for (uint32_t f = 0u; f < num_frames; ++f) {
        if (!m->run_rounds(10000u)) {
            ok = false;
            break;
        }
        m->render_frame();
    }

    res.completed = ok;
    res.halted = m->halted;
    if (m->halted) {
        if (m->last_fault.reason) res.fault_reason = m->last_fault.reason;
        res.fault_pc = m->last_fault.pc;
        res.fault_addr = m->last_fault.address;
        res.fault_opcode = m->last_fault.opcode;
    }
    res.rounds_done = m->rounds_done;
    res.main_steps = m->main_steps;
    res.slave_steps = m->slave_steps;
    res.main_pc = m->cpus[0].pc;
    res.slave_pc = m->cpus[1].pc;
    res.ioga_reads = m->ioga_reads;
    res.ioga_writes = m->ioga_writes;
    res.smpc_input_reads = m->smpc_input_reads;
    res.cart_reads = m->cart_reads;
    res.vbi_count = m->vbi_count;
    res.vbo_count = m->vbo_count;
    res.sched2_hits = m->sched2_hits;
    res.vdp1_draws = static_cast<uint32_t>(m->vdp.draws);
    res.mmio_log_count = m->mmio_log_count;

    uint8_t hw[32], lw[32], v1[32], v2[32], c2[32], r2[32], fr[32];
    m->compute_digests(hw, lw, v1, v2, c2, r2, fr);
    res.high_wram_sha = digest_to_hex(hw);
    res.low_wram_sha = digest_to_hex(lw);
    res.vdp1_vram_sha = digest_to_hex(v1);
    res.vdp2_vram_sha = digest_to_hex(v2);
    res.frame_sha = digest_to_hex(fr);

    const uint32_t n_log = (m->ioga_log_count < 16u) ? m->ioga_log_count : 16u;
    for (uint32_t i = 0u; i < n_log; ++i) {
        res.first_ioga.push_back(m->ioga_log[i]);
    }
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string g3_dir = (argc >= 2) ? argv[1] : "port";
    const std::string repo_root = (argc >= 3) ? argv[2] : ".";
    std::string err;

    std::vector<uint8_t> cart_bytes, bios_bytes, vis_cp, ep_cp;
    if (!read_file_bytes(g3_dir + "/data/twcup98_cart_20m.bin", cart_bytes, err) ||
        !read_file_bytes(g3_dir + "/data/bios_euro_be.bin", bios_bytes, err) ||
        !read_file_bytes(g3_dir + "/data/visual_sync1027b_f8175.cp", vis_cp, err) ||
        !read_file_bytes(g3_dir + "/data/endpoint_frame8175.cp", ep_cp, err)) {
        std::cerr << err << "\n";
        return 1;
    }

    const std::string vis_dir = repo_root + "/.local/experiments/012-h4-control-trace/visual-sync1027b-neutral";
    std::vector<uint8_t> v1_vram, v1_regs, v2_vram, v2_cram, v2_regs;
    if (!read_file_bytes(vis_dir + "/control-window-start-vdp1-vram.bin", v1_vram, err) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp1-registers.bin", v1_regs, err) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-vram.bin", v2_vram, err) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-cram.bin", v2_cram, err) ||
        !read_file_bytes(vis_dir + "/control-window-start-vdp2-registers.bin", v2_regs, err)) {
        std::cerr << err << "\n";
        return 1;
    }

    // ---- Phase A: Subtask 3 Cartridge ROM (0x02000000..0x04FFFFFF + 0x22000000 mirror) ----
    auto m = std::make_unique<twc98::StvMachineState>();
    m->reset();
    twc98::BusAccessFault fault{};
    (void)m->read(0, 0x02200000u, 1u, 0x06000100u, 0u, fault);
    if (fault.ok || !fault.reason || std::string(fault.reason) != "cart_rom_not_loaded") {
        std::cerr << "Expected cart_rom_not_loaded fault when no cart loaded\n";
        return 1;
    }
    if (!m->load_cart_rom(cart_bytes.data(), cart_bytes.size())) {
        std::cerr << "load_cart_rom failed\n";
        return 1;
    }

    const std::string bank_dir = repo_root + "/.local/experiments/003-memory-map/bank-selected";
    uint32_t total_sample_bytes = 0u;
    for (const auto& s : kCartSamples) {
        std::vector<uint8_t> expected;
        if (!read_file_bytes(bank_dir + "/" + s.filename, expected, err)) {
            std::cerr << err << "\n";
            return 1;
        }
        for (uint32_t mirror : {0x00000000u, 0x20000000u}) {
            // Check 8-bit, 16-bit BE, and 32-bit BE reads across all 64 bytes
            for (size_t i = 0; i < expected.size(); ++i) {
                twc98::BusAccessFault rf{};
                const uint32_t b = m->read(0, s.addr + mirror + static_cast<uint32_t>(i), 1u, 0u, 0u, rf);
                if (!rf.ok || b != expected[i]) {
                    std::cerr << "Cart 8-bit mismatch at " << hex32(s.addr + mirror + static_cast<uint32_t>(i)) << "\n";
                    return 1;
                }
            }
            for (size_t i = 0; i < expected.size(); i += 2u) {
                twc98::BusAccessFault rf{};
                const uint32_t exp16 = (static_cast<uint32_t>(expected[i]) << 8) | expected[i + 1u];
                const uint32_t w = m->read(0, s.addr + mirror + static_cast<uint32_t>(i), 2u, 0u, 0u, rf);
                if (!rf.ok || w != exp16) {
                    std::cerr << "Cart 16-bit mismatch at " << hex32(s.addr + mirror + static_cast<uint32_t>(i)) << "\n";
                    return 1;
                }
            }
            for (size_t i = 0; i < expected.size(); i += 4u) {
                twc98::BusAccessFault rf{};
                const uint32_t exp32 = (static_cast<uint32_t>(expected[i]) << 24) |
                                       (static_cast<uint32_t>(expected[i + 1u]) << 16) |
                                       (static_cast<uint32_t>(expected[i + 2u]) << 8) |
                                       expected[i + 3u];
                const uint32_t d = m->read(0, s.addr + mirror + static_cast<uint32_t>(i), 4u, 0u, 0u, rf);
                if (!rf.ok || d != exp32) {
                    std::cerr << "Cart 32-bit mismatch at " << hex32(s.addr + mirror + static_cast<uint32_t>(i)) << "\n";
                    return 1;
                }
            }
        }
        total_sample_bytes += static_cast<uint32_t>(expected.size());
    }

    // Check 0x02231E10 leaf bytes "634f43118900644b000b6043"
    constexpr uint8_t kExpectedLeaf[12] = {0x63, 0x4f, 0x43, 0x11, 0x89, 0x00, 0x64, 0x4b, 0x00, 0x0b, 0x60, 0x43};
    for (uint32_t i = 0; i < 12u; ++i) {
        twc98::BusAccessFault rf{};
        const uint32_t b = m->read(0, 0x02231E10u + i, 1u, 0u, 0u, rf);
        if (!rf.ok || b != kExpectedLeaf[i]) {
            std::cerr << "Cart function byte mismatch at 0x02231E10 + " << i << "\n";
            return 1;
        }
    }

    // Check SMPC PDR1 stv_select_game(data & 3)
    {
        twc98::BusAccessFault bf{};
        const uint32_t bank0_w = m->read(0, 0x02200000u, 4u, 0u, 0u, bf);
        m->write(0, 0x00100075u, 1u, 0x01u, 0u, 0u, bf);
        const uint32_t bank1_w = m->read(0, 0x02200000u, 4u, 0u, 0u, bf);
        m->write(0, 0x00100075u, 1u, 0x00u, 0u, 0u, bf);
        const uint32_t bank0_back = m->read(0, 0x02200000u, 4u, 0u, 0u, bf);
        if (!bf.ok || m->smpc_gamebank_select != 0u || bank0_w == 0u || bank1_w != 0u || bank0_back != bank0_w) {
            std::cerr << "SMPC gamebank_select failure\n";
            return 1;
        }
    }

    // Check Sega 315-5881 A-Bus protection window on 0x04FFFFF0..FF (cached & uncached mirrors)
    {
        twc98::BusAccessFault pf{};
        m->write(0, 0x04FFFFF1u, 1u, 0x00u, 0x06009764u, 0u, pf);
        m->write(0, 0x04FFFFF8u, 2u, 0x0AD2u, 0x06009770u, 0u, pf);
        m->write(0, 0x04FFFFFAu, 2u, 0x0069u, 0x06009778u, 0u, pf);
        m->write(0, 0x04FFFFFCu, 2u, 0x1212u, 0x06009780u, 0u, pf);
        m->write(0, 0x04FFFFF1u, 1u, 0x01u, 0x06009788u, 0u, pf);
        const uint32_t dec_w0 = m->read(0, 0x24FFFFFCu, 4u, 0x06009790u, 0u, pf);
        m->write(0, 0x24FFFFF1u, 1u, 0x00u, 0x06009818u, 0u, pf);
        const uint32_t reg_w0 = m->read(0, 0x04FFFFF8u, 2u, 0x06009820u, 0u, pf);
        if (!pf.ok || m->cart_prot_enabled || m->crypt315.subkey != 0x1212u || dec_w0 == 0u || reg_w0 != 0x0AD2u) {
            std::cerr << "315-5881 protection window check failed (ok=" << pf.ok
                      << " subkey=" << hex32(m->crypt315.subkey)
                      << " dec_w0=" << hex32(dec_w0)
                      << " reg_w0=" << hex32(reg_w0) << ")\n";
            return 1;
        }
        m->crypt315.reset();
        m->cart_prot_enabled = false;
    }

    // ---- Phase B: Subtask 2 IOGA + SMPC Unit Checks ----
    m->reset();
    m->set_input_ports(0xDFu, 0xBEu, 0xEEu, 0xF7u, 0xFEu, 0x7Fu, 0x67u);
    {
        twc98::BusAccessFault inf{};
        const uint32_t pa = m->read(0, 0x00400001u, 1u, 0u, 0u, inf);
        const uint32_t pa_m = m->read(0, 0x20400021u, 1u, 0u, 0u, inf);
        const uint32_t pb = m->read(0, 0x00400003u, 1u, 0u, 0u, inf);
        const uint32_t pc_in = m->read(0, 0x00400005u, 1u, 0u, 0u, inf);
        const uint32_t pe = m->read(0, 0x00400009u, 1u, 0u, 0u, inf);
        const uint32_t pf_in = m->read(0, 0x0040000Bu, 1u, 0u, 0u, inf);
        const uint32_t w4 = m->read(0, 0x20400000u, 4u, 0u, 0u, inf);
        const uint32_t com = m->read(0, 0x0040001Bu, 1u, 0u, 0u, inf);
        m->write(0, 0x00400007u, 1u, 0x5Au, 0u, 0u, inf);
        const uint32_t pd = m->read(0, 0x00400007u, 1u, 0u, 0u, inf);
        const uint32_t pdr1 = m->read(0, 0x00100075u, 1u, 0u, 0u, inf);
        const uint32_t pdr2 = m->read(0, 0x20100077u, 1u, 0u, 0u, inf);
        if (!inf.ok || pa != 0xDFu || pa_m != 0xDFu || pb != 0xBEu || pc_in != 0xEEu ||
            pe != 0xF7u || pf_in != 0xFEu || w4 != 0x00DF00BEu || com != 0x00u || pd != 0x5Au ||
            pdr1 != 0x7Fu || pdr2 != 0x67u) {
            std::cerr << "IOGA/SMPC unit check mismatch\n";
            return 1;
        }
        m->write(0, 0x0010007Bu, 1u, 0x18u, 0u, 0u, inf);
        m->write(0, 0x00100077u, 1u, 0x10u, 0u, 0u, inf);
        const uint32_t pdr2_drv = m->read(0, 0x00100077u, 1u, 0u, 0u, inf);
        if (!inf.ok || pdr2_drv != 0x77u) {
            std::cerr << "SMPC DDR2/PDR2 masking mismatch\n";
            return 1;
        }
    }

    // ---- Phase C: Live Control Integration (EP8175 12 frames + VIS8175 2 frames) ----
    std::vector<ScenarioResult> scenarios;
    for (const auto& sc : kScenarios) {
        scenarios.push_back(run_cp_scenario(
            std::string("ep8175_") + sc.name, ep_cp, bios_bytes, cart_bytes,
            &v1_vram, &v1_regs, &v2_vram, &v2_cram, &v2_regs,
            sc.port_a, sc.port_b, sc.port_c, 12u));
    }
    scenarios.push_back(run_cp_scenario(
        "vis8175_neutral_2f", vis_cp, bios_bytes, cart_bytes,
        &v1_vram, &v1_regs, &v2_vram, &v2_cram, &v2_regs,
        0xFFu, 0xFFu, 0xFFu, 2u));
    scenarios.push_back(run_cp_scenario(
        "vis8175_p1_up_2f", vis_cp, bios_bytes, cart_bytes,
        &v1_vram, &v1_regs, &v2_vram, &v2_cram, &v2_regs,
        0xDFu, 0xFFu, 0xFFu, 2u));

    // ---- Phase D: G2 Regression (endpoint_frame8175.cp with real cart + neutral inputs, 12 frames) ----
    ScenarioResult g2_reg = run_cp_scenario(
        "g2_regression_ep8175", ep_cp, bios_bytes, cart_bytes,
        nullptr, nullptr, nullptr, nullptr, nullptr,
        0xFFu, 0xFFu, 0xFFu, 12u);

    // Compute cart 20m digest for manifest check
    const std::string cart20m_sha = twc98::sha256_hex(cart_bytes.data(), cart_bytes.size());

    // Emit JSON report
    std::ostringstream js;
    js << "{\n";
    js << "  \"schema\": \"g3_native_report_v1\",\n";
    js << "  \"cart_20m_sha256\": \"" << cart20m_sha << "\",\n";
    js << "  \"cart_samples_verified\": 17,\n";
    js << "  \"cart_sample_bytes_verified\": " << total_sample_bytes << ",\n";
    js << "  \"prot_guard_ok\": true,\n";
    js << "  \"unit_ioga_smpc_ok\": true,\n";
    js << "  \"g2_regression\": {\n";
    js << "    \"completed\": " << (g2_reg.completed ? "true" : "false") << ",\n";
    js << "    \"halted\": " << (g2_reg.halted ? "true" : "false") << ",\n";
    js << "    \"fault_reason\": \"" << g2_reg.fault_reason << "\",\n";
    js << "    \"rounds_done\": " << g2_reg.rounds_done << ",\n";
    js << "    \"main_steps\": " << g2_reg.main_steps << ",\n";
    js << "    \"slave_steps\": " << g2_reg.slave_steps << ",\n";
    js << "    \"main_pc\": \"" << hex32(g2_reg.main_pc) << "\",\n";
    js << "    \"slave_pc\": \"" << hex32(g2_reg.slave_pc) << "\",\n";
    js << "    \"vbi_count\": " << g2_reg.vbi_count << ",\n";
    js << "    \"vbo_count\": " << g2_reg.vbo_count << ",\n";
    js << "    \"sched2_hits\": " << g2_reg.sched2_hits << ",\n";
    js << "    \"ioga_reads\": " << g2_reg.ioga_reads << ",\n";
    js << "    \"smpc_input_reads\": " << g2_reg.smpc_input_reads << ",\n";
    js << "    \"cart_reads\": " << g2_reg.cart_reads << ",\n";
    js << "    \"mmio_log_count\": " << g2_reg.mmio_log_count << ",\n";
    js << "    \"high_wram_sha256\": \"" << g2_reg.high_wram_sha << "\",\n";
    js << "    \"low_wram_sha256\": \"" << g2_reg.low_wram_sha << "\",\n";
    js << "    \"frame_sha256\": \"" << g2_reg.frame_sha << "\"\n";
    js << "  },\n";
    js << "  \"scenarios\": [\n";
    for (size_t i = 0; i < scenarios.size(); ++i) {
        const auto& r = scenarios[i];
        js << "    {\n";
        js << "      \"name\": \"" << r.name << "\",\n";
        js << "      \"completed\": " << (r.completed ? "true" : "false") << ",\n";
        js << "      \"halted\": " << (r.halted ? "true" : "false") << ",\n";
        js << "      \"fault_reason\": \"" << r.fault_reason << "\",\n";
        js << "      \"fault_pc\": \"" << hex32(r.fault_pc) << "\",\n";
        js << "      \"fault_addr\": \"" << hex32(r.fault_addr) << "\",\n";
        js << "      \"fault_opcode\": \"" << hex32(r.fault_opcode) << "\",\n";
        js << "      \"rounds_done\": " << r.rounds_done << ",\n";
        js << "      \"main_steps\": " << r.main_steps << ",\n";
        js << "      \"slave_steps\": " << r.slave_steps << ",\n";
        js << "      \"main_pc\": \"" << hex32(r.main_pc) << "\",\n";
        js << "      \"slave_pc\": \"" << hex32(r.slave_pc) << "\",\n";
        js << "      \"ioga_reads\": " << r.ioga_reads << ",\n";
        js << "      \"ioga_writes\": " << r.ioga_writes << ",\n";
        js << "      \"smpc_input_reads\": " << r.smpc_input_reads << ",\n";
        js << "      \"cart_reads\": " << r.cart_reads << ",\n";
        js << "      \"vbi_count\": " << r.vbi_count << ",\n";
        js << "      \"vbo_count\": " << r.vbo_count << ",\n";
        js << "      \"vdp1_draws\": " << r.vdp1_draws << ",\n";
        js << "      \"high_wram_sha256\": \"" << r.high_wram_sha << "\",\n";
        js << "      \"low_wram_sha256\": \"" << r.low_wram_sha << "\",\n";
        js << "      \"vdp1_vram_sha256\": \"" << r.vdp1_vram_sha << "\",\n";
        js << "      \"vdp2_vram_sha256\": \"" << r.vdp2_vram_sha << "\",\n";
        js << "      \"frame_sha256\": \"" << r.frame_sha << "\",\n";
        js << "      \"first_ioga_reads\": [";
        for (size_t k = 0; k < r.first_ioga.size(); ++k) {
            const auto& e = r.first_ioga[k];
            js << "{\"cpu\":" << static_cast<unsigned>(e.cpu_id)
               << ",\"pc\":\"" << hex32(e.pc)
               << "\",\"addr\":\"" << hex32(e.address)
               << "\",\"val\":\"" << hex8(e.val)
               << "\",\"wr\":" << static_cast<unsigned>(e.is_write) << "}";
            if (k + 1u < r.first_ioga.size()) js << ",";
        }
        js << "]\n";
        js << "    }" << (i + 1u < scenarios.size() ? "," : "") << "\n";
    }
    js << "  ]\n";
    js << "}\n";

    const std::string out_path = g3_dir + "/validation_g4/native/g3_report.json";
    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs || !ofs.write(js.str().data(), static_cast<std::streamsize>(js.str().size()))) {
        std::cerr << "Failed writing " << out_path << "\n";
        return 1;
    }

    // Hard assertions: G2 regression + Subtask 2 causal input divergence
    if (!g2_reg.completed || g2_reg.halted || g2_reg.vbi_count != 12u || g2_reg.vbo_count != 12u ||
        g2_reg.sched2_hits != 12u || g2_reg.mmio_log_count != 1139u) {
        std::cerr << "G2 regression assertion failed\n";
        return 1;
    }
    const auto& ep_neu = scenarios[0];
    const auto& ep_up = scenarios[1];
    const auto& ep_left = scenarios[2];
    const auto& ep_p2 = scenarios[3];
    const auto& ep_coin = scenarios[4];
    const auto& vis_neu = scenarios[5];
    const auto& vis_up = scenarios[6];
    for (const auto& r : scenarios) {
        if (!r.completed || r.halted || r.ioga_reads == 0u) {
            std::cerr << "Scenario did not complete cleanly: " << r.name << "\n";
            return 1;
        }
    }
    if (ep_up.high_wram_sha == ep_neu.high_wram_sha ||
        ep_left.high_wram_sha == ep_neu.high_wram_sha ||
        ep_p2.high_wram_sha == ep_neu.high_wram_sha ||
        ep_coin.high_wram_sha == ep_neu.high_wram_sha ||
        ep_up.frame_sha == ep_neu.frame_sha ||
        ep_left.frame_sha == ep_neu.frame_sha ||
        vis_up.high_wram_sha == vis_neu.high_wram_sha) {
        std::cerr << "Input causal divergence assertion failed\n";
        return 1;
    }

    std::cout << js.str();
    return 0;
}
