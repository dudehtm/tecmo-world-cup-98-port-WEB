#include "sha256.hpp"
#include "stv_machine.hpp"
#include "twc98_net_session.hpp"

#include <algorithm>
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

uint8_t player_buttons_for_frame(uint8_t player, uint32_t f) {
    static constexpr uint8_t kDirs[8] = {0x7Fu, 0xBFu, 0xDFu, 0xEFu, 0x5Fu, 0x9Fu, 0xAFu, 0xFFu};
    if (player == 0u) {
        // P1 changes direction every 5..7 frames and presses pass/shoot/dash bursts
        uint8_t b = kDirs[((f / 5u) + (f / 19u)) & 7u];
        if ((f % 9u) < 2u) b &= static_cast<uint8_t>(~0x01u);
        if ((f % 15u) == 3u) b &= static_cast<uint8_t>(~0x02u);
        if ((f % 21u) == 6u) b &= static_cast<uint8_t>(~0x04u);
        return b;
    } else {
        // P2 changes direction every 4..6 frames and presses tackle/pass/lob bursts
        uint8_t b = kDirs[((f + 11u) / 4u + (f / 13u)) & 7u];
        if ((f % 8u) == 1u || (f % 8u) == 2u) b &= static_cast<uint8_t>(~0x01u);
        if ((f % 14u) == 5u) b &= static_cast<uint8_t>(~0x02u);
        if ((f % 19u) == 4u) b &= static_cast<uint8_t>(~0x04u);
        return b;
    }
}

struct InFlightWirePacket {
    uint8_t dst_peer = 0u;
    double arrival_ms = 0.0;
    uint32_t seq = 0u;
    uint8_t wire[twc98::NetInputPacket::kWireBytes]{};
};

// Deterministic LCG for reproducible jitter/loss across PC and Wasm
uint32_t next_rng(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
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

    auto init_peer = [&](twc98::StvMachineState& m) -> bool {
        const char* err = nullptr;
        if (!m.init_from_cpn8_bytes(ep_cp.data(), ep_cp.size(), bios.data(), bios.size(), &err)) {
            return false;
        }
        if (!m.load_cart_rom(cart.data(), cart.size())) {
            return false;
        }
        m.seed_vdp_snapshot(v1_vram.data(), v1_vram.size(),
                            v1_regs.data(), v1_regs.size(),
                            v2_vram.data(), v2_vram.size(),
                            v2_cram.data(), v2_cram.size(),
                            v2_regs.data(), v2_regs.size());
        m.set_gk_ai_fix(true);
        m.set_real_audio(true);
        for (uint32_t w = 0u; w < 40u; ++w) {
            const uint8_t pc_in = (w >= 4u && w < 10u) ? 0xCFu : 0xFFu;
            m.set_input_ports(0xFFu, 0xFFu, pc_in, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
            if (!m.run_rounds(10000u)) return false;
            m.render_frame();
        }
        m.clear_audio_pcm();
        return true;
    };

    auto m0 = std::make_unique<twc98::StvMachineState>();
    auto m1 = std::make_unique<twc98::StvMachineState>();
    if (!init_peer(*m0) || !init_peer(*m1)) {
        std::cerr << "FAIL: init_peer failed\n";
        return 1;
    }

    // Slow WAN parameters: RTT = 150 ms (75 ms one-way), Jitter = +/- 35 ms, Loss = ~3.3%
    static constexpr uint32_t kSimRttMs = 150u;
    static constexpr double kOneWayBaseMs = 75.0;
    static constexpr double kMaxJitterMs = 35.0;
    static constexpr uint32_t kMatchFrames = 300u;  // 5.0s of active 1v1 match play (300 frames per peer = 600 peer-frames)

    const uint8_t rec_delay = twc98::NetSession::recommend_input_delay_for_rtt_ms(kSimRttMs);
    twc98::NetSession net0{};
    twc98::NetSession net1{};
    net0.reset(0u, rec_delay, /*copa_mode=*/false, /*gk_ai_fix=*/true);
    net1.reset(1u, rec_delay, /*copa_mode=*/false, /*gk_ai_fix=*/true);

    std::vector<uint8_t> arena0(twc98::NetSession::kRequiredArenaBytes, 0u);
    std::vector<uint8_t> arena1(twc98::NetSession::kRequiredArenaBytes, 0u);

    std::vector<InFlightWirePacket> flight_queue;
    uint32_t rng = 0x98C0FFEEu;
    uint32_t pkt_seq = 0u;
    uint32_t dropped_packets = 0u;
    uint32_t duplicated_packets = 0u;
    uint32_t out_of_order_deliveries = 0u;
    uint32_t last_delivered_seq[2] = {0u, 0u};
    uint64_t total_wire_bytes_sent = 0u;

    auto send_over_wan = [&](uint8_t dst_peer, double now_ms, const twc98::NetInputPacket& pkt) {
        uint8_t wire[twc98::NetInputPacket::kWireBytes]{};
        twc98::pack_net_input_packet(pkt, wire);
        total_wire_bytes_sent += sizeof(wire);
        const uint32_t s = ++pkt_seq;

        // ~3.3% packet loss (1 in 30 packets dropped completely)
        if ((next_rng(rng) % 30u) == 7u) {
            ++dropped_packets;
            return;
        }

        // Jitter in [-35.0 ms, +35.0 ms]
        const double u = static_cast<double>(next_rng(rng) & 0xFFFFu) / 65535.0;
        const double jitter_ms = (u * 2.0 - 1.0) * kMaxJitterMs;
        const double arrival = std::max(now_ms + 2.0, now_ms + kOneWayBaseMs + jitter_ms);

        InFlightWirePacket wp{};
        wp.dst_peer = dst_peer;
        wp.arrival_ms = arrival;
        wp.seq = s;
        std::memcpy(wp.wire, wire, sizeof(wire));
        flight_queue.push_back(wp);

        // ~2% duplicate packet in flight
        if ((next_rng(rng) % 50u) == 13u) {
            InFlightWirePacket dup = wp;
            dup.arrival_ms += 6.0;
            flight_queue.push_back(dup);
            ++duplicated_packets;
        }
    };

    auto deliver_due_packets = [&](double now_ms) {
        // Sort ready packets by arrival_ms to model true arrival order across jittered paths
        std::stable_sort(flight_queue.begin(), flight_queue.end(),
                         [](const InFlightWirePacket& a, const InFlightWirePacket& b) {
                             return a.arrival_ms < b.arrival_ms;
                         });
        size_t i = 0u;
        while (i < flight_queue.size() && flight_queue[i].arrival_ms <= now_ms) {
            const auto& wp = flight_queue[i];
            if (wp.seq < last_delivered_seq[wp.dst_peer]) {
                ++out_of_order_deliveries;
            } else {
                last_delivered_seq[wp.dst_peer] = wp.seq;
            }
            if (wp.dst_peer == 0u) {
                net0.receive_wire_bytes(wp.wire, sizeof(wp.wire));
            } else {
                net1.receive_wire_bytes(wp.wire, sizeof(wp.wire));
            }
            ++i;
        }
        if (i > 0u) {
            flight_queue.erase(flight_queue.begin(), flight_queue.begin() + static_cast<std::ptrdiff_t>(i));
        }
    };

    int16_t pcm_sink[2048 * 2]{};
    uint32_t audio_consumed0 = 0u;
    uint32_t audio_consumed1 = 0u;

    for (uint32_t tick = 0u; tick < kMatchFrames; ++tick) {
        const double now_ms = static_cast<double>(tick) * (1000.0 / 60.0);
        deliver_due_packets(now_ms);

        // Peer 0 queues local P1 input for (cur_frame + delay) and sends 48B packet
        {
            const uint32_t tf0 = net0.cur_frame() + net0.input_delay();
            twc98::NetInputPacket pkt0{};
            net0.queue_local_input(player_buttons_for_frame(0u, tf0), 0xFFu, pkt0);
            send_over_wan(/*dst_peer=*/1u, now_ms, pkt0);
        }
        // Peer 1 queues local P2 input for (cur_frame + delay) and sends 48B packet
        {
            const uint32_t tf1 = net1.cur_frame() + net1.input_delay();
            twc98::NetInputPacket pkt1{};
            net1.queue_local_input(player_buttons_for_frame(1u, tf1), 0xFFu, pkt1);
            send_over_wan(/*dst_peer=*/0u, now_ms, pkt1);
        }

        if (!net0.step_frame(*m0, arena0.data(), arena0.size())) {
            std::cerr << "FAIL: net0 failed/stalled at tick " << tick << "\n";
            return 1;
        }
        audio_consumed0 += m0->consume_audio_pcm(pcm_sink, 2048u);

        if (!net1.step_frame(*m1, arena1.data(), arena1.size())) {
            std::cerr << "FAIL: net1 failed/stalled at tick " << tick << "\n";
            return 1;
        }
        audio_consumed1 += m1->consume_audio_pcm(pcm_sink, 2048u);
    }

    // Drain remaining in-flight packets for the last frames (up to +200 ms) and flush final rollbacks
    deliver_due_packets(static_cast<double>(kMatchFrames) * (1000.0 / 60.0) + 200.0);
    net0.flush_pending_rollback(*m0, arena0.data(), arena0.size(), /*render_final=*/true);
    net1.flush_pending_rollback(*m1, arena1.data(), arena1.size(), /*render_final=*/true);

    // Send one final ACK/sync packet each so the frame-300 sync_hash64 is cross-checked by both peers
    {
        twc98::NetInputPacket f0{}, f1{};
        net0.queue_local_input(0xFFu, 0xFFu, f0);
        net1.queue_local_input(0xFFu, 0xFFu, f1);
        net1.receive_packet(f0);
        net0.receive_packet(f1);
    }

    const uint64_t hash0 = m0->compute_sync_hash64();
    const uint64_t hash1 = m1->compute_sync_hash64();

    std::vector<uint8_t> snap0(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    std::vector<uint8_t> snap1(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    const uint32_t slen0 = m0->save_state_bytes(snap0.data(), snap0.size());
    const uint32_t slen1 = m1->save_state_bytes(snap1.data(), snap1.size());

    uint8_t sha_snap0[32]{}, sha_snap1[32]{};
    twc98::sha256_bytes(snap0.data(), slen0, sha_snap0);
    twc98::sha256_bytes(snap1.data(), slen1, sha_snap1);

    uint8_t d0[7][32]{}, d1[7][32]{};
    m0->compute_digests(d0[0], d0[1], d0[2], d0[3], d0[4], d0[5], d0[6]);
    m1->compute_digests(d1[0], d1[1], d1[2], d1[3], d1[4], d1[5], d1[6]);

    const std::string snap0_hex = digest_to_hex(sha_snap0);
    const std::string snap1_hex = digest_to_hex(sha_snap1);
    const std::string frame0_hex = digest_to_hex(d0[6]);
    const std::string frame1_hex = digest_to_hex(d1[6]);

    const double duration_sec = static_cast<double>(kMatchFrames) / 60.0;
    const double kbps_per_peer = (static_cast<double>(total_wire_bytes_sent) * 8.0 / 2.0) / (duration_sec * 1000.0);

    const bool exact_sync = (hash0 == hash1) && (net0.desync_count() == 0u) && (net1.desync_count() == 0u) &&
                            (net0.sync_checks_passed() >= 5u) && (net1.sync_checks_passed() >= 5u);
    const bool exact_snap = (slen0 == slen1) && (snap0_hex == snap1_hex);
    const bool exact_frame = (frame0_hex == frame1_hex);
    const bool zero_stalls = (net0.stall_frames() == 0u) && (net1.stall_frames() == 0u);
    const bool audio_ok = (audio_consumed0 == kMatchFrames * 735u) &&
                          (audio_consumed1 == kMatchFrames * 735u) &&
                          (m0->audio_pcm_produced == m1->audio_pcm_produced);
    const bool bw_ok = (kbps_per_peer < 25.0);
    const bool all_pass = exact_sync && exact_snap && exact_frame && zero_stalls && audio_ok && bw_ok;

    std::printf("H6-02 Slow-WAN NetSim: status=%s\n", all_pass ? "PASS" : "FAIL");
    std::printf("  wan_profile: rtt=%u ms jitter=+/-%.0f ms dropped_pkts=%u dup_pkts=%u ooo_pkts=%u input_delay=%u f\n",
                kSimRttMs, kMaxJitterMs, dropped_packets, duplicated_packets, out_of_order_deliveries, rec_delay);
    std::printf("  bandwidth: %.2f Kbps/peer (wire_pkt=%zu B)\n",
                kbps_per_peer, twc98::NetInputPacket::kWireBytes);
    std::printf("  peer0: frames=%u rollbacks=%u rb_frames=%u max_depth=%u stalls=%u sync_ok=%u desync=%u\n",
                net0.cur_frame(), net0.rollback_count(), net0.rollback_frames_total(),
                net0.max_rollback_depth(), net0.stall_frames(), net0.sync_checks_passed(), net0.desync_count());
    std::printf("  peer1: frames=%u rollbacks=%u rb_frames=%u max_depth=%u stalls=%u sync_ok=%u desync=%u\n",
                net1.cur_frame(), net1.rollback_count(), net1.rollback_frames_total(),
                net1.max_rollback_depth(), net1.stall_frames(), net1.sync_checks_passed(), net1.desync_count());
    std::printf("  sync_hash64: peer0=0x%016llx peer1=0x%016llx (match=%d)\n",
                static_cast<unsigned long long>(hash0),
                static_cast<unsigned long long>(hash1),
                exact_sync ? 1 : 0);
    std::printf("  snapshot_sha256: %s (match=%d)\n", snap0_hex.c_str(), exact_snap ? 1 : 0);
    std::printf("  frame_px_sha256: %s (match=%d)\n", frame0_hex.c_str(), exact_frame ? 1 : 0);
    std::printf("  audio_pcm: peer0_ring=%u peer1_ring=%u produced=%u (match=%d)\n",
                audio_consumed0, audio_consumed1, m0->audio_pcm_produced, audio_ok ? 1 : 0);

    return all_pass ? 0 : 1;
}
