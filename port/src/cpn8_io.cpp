// CPN8 v3 checkpoint loader/serializer implementation.
#include "cpn8_io.hpp"

#include <cstring>
#ifndef TWC98_NO_STL
#include <fstream>
#endif

namespace twc98 {
namespace {

inline constexpr uint32_t kCpn8Magic = 0x384E5043u;   // "CPN8"
inline constexpr uint32_t kCpf3Format = 0x33465043u;  // "CPF3"
inline constexpr uint32_t kHeaderSize = 616u;
inline constexpr uint32_t kMinSize = kHeaderSize + kHighWramSize + kLowWramSize + 8u;

uint32_t rd_le32(const uint8_t* p, size_t off) {
    return static_cast<uint32_t>(p[off]) |
           (static_cast<uint32_t>(p[off + 1u]) << 8) |
           (static_cast<uint32_t>(p[off + 2u]) << 16) |
           (static_cast<uint32_t>(p[off + 3u]) << 24);
}

#ifndef TWC98_NO_STL
void wr_le32(uint8_t* p, size_t off, uint32_t v) {
    p[off] = static_cast<uint8_t>(v & 0xFFu);
    p[off + 1u] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[off + 2u] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[off + 3u] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}
#endif

void unpack_cpu(const uint8_t* raw, size_t cpu_off, size_t adapter_off, size_t minit_frt_off,
                Sh2CpuState& cpu, Sh7604Divu& divu, Sh7604Frt& frt) {
    for (size_t i = 0; i < 16u; ++i) {
        cpu.r[i] = rd_le32(raw, cpu_off + i * 4u);
    }
    cpu.pc = rd_le32(raw, cpu_off + 16u * 4u);
    cpu.pr = rd_le32(raw, cpu_off + 17u * 4u);
    cpu.gbr = rd_le32(raw, cpu_off + 18u * 4u);
    cpu.vbr = rd_le32(raw, cpu_off + 19u * 4u);
    cpu.mach = rd_le32(raw, cpu_off + 20u * 4u);
    cpu.macl = rd_le32(raw, cpu_off + 21u * 4u);
    const uint32_t m = rd_le32(raw, cpu_off + 22u * 4u) & 1u;
    const uint32_t q = rd_le32(raw, cpu_off + 23u * 4u) & 1u;
    const uint32_t imask = rd_le32(raw, cpu_off + 24u * 4u) & 0xFu;
    const uint32_t s = rd_le32(raw, cpu_off + 25u * 4u) & 1u;
    const uint32_t t = rd_le32(raw, cpu_off + 26u * 4u) & 1u;
    cpu.sr = (m << 9) | (q << 8) | (imask << 4) | (s << 1) | t;
    cpu.delay = rd_le32(raw, cpu_off + 28u * 4u);
    cpu.cur_pc = rd_le32(raw, cpu_off + 29u * 4u);

    cpu.imask_meta = rd_le32(raw, adapter_off + 0u);
    cpu.context_loaded = rd_le32(raw, adapter_off + 4u);
    cpu.retired = rd_le32(raw, adapter_off + 8u);
    cpu.cycles = 0u;

    divu.dvsr = rd_le32(raw, adapter_off + 12u);
    divu.dvdnth = rd_le32(raw, adapter_off + 16u);
    divu.dvdntl = rd_le32(raw, adapter_off + 20u);
    divu.written = rd_le32(raw, adapter_off + 24u);

    frt.ftcsr = rd_le32(raw, minit_frt_off + 0u) & 0xFFu;
    frt.tier = rd_le32(raw, minit_frt_off + 4u) & 0xFFu;
    frt.tcr = rd_le32(raw, minit_frt_off + 8u) & 0xFFu;
}

#ifndef TWC98_NO_STL
void pack_cpu(uint8_t* raw, size_t cpu_off, size_t adapter_off, size_t minit_frt_off,
              const Sh2CpuState& cpu, const Sh7604Divu& divu, const Sh7604Frt& frt) {
    for (size_t i = 0; i < 16u; ++i) {
        wr_le32(raw, cpu_off + i * 4u, cpu.r[i]);
    }
    wr_le32(raw, cpu_off + 16u * 4u, cpu.pc);
    wr_le32(raw, cpu_off + 17u * 4u, cpu.pr);
    wr_le32(raw, cpu_off + 18u * 4u, cpu.gbr);
    wr_le32(raw, cpu_off + 19u * 4u, cpu.vbr);
    wr_le32(raw, cpu_off + 20u * 4u, cpu.mach);
    wr_le32(raw, cpu_off + 21u * 4u, cpu.macl);
    wr_le32(raw, cpu_off + 22u * 4u, (cpu.sr >> 9) & 1u);
    wr_le32(raw, cpu_off + 23u * 4u, (cpu.sr >> 8) & 1u);
    wr_le32(raw, cpu_off + 24u * 4u, (cpu.sr >> 4) & 0xFu);
    wr_le32(raw, cpu_off + 25u * 4u, (cpu.sr >> 1) & 1u);
    wr_le32(raw, cpu_off + 26u * 4u, cpu.sr & 1u);
    wr_le32(raw, cpu_off + 28u * 4u, cpu.delay);
    wr_le32(raw, cpu_off + 29u * 4u, cpu.cur_pc);

    wr_le32(raw, adapter_off + 0u, (cpu.sr >> 4) & 0xFu);
    wr_le32(raw, adapter_off + 4u, cpu.context_loaded);
    wr_le32(raw, adapter_off + 8u, static_cast<uint32_t>(cpu.retired));
    wr_le32(raw, adapter_off + 12u, divu.dvsr);
    wr_le32(raw, adapter_off + 16u, divu.dvdnth);
    wr_le32(raw, adapter_off + 20u, divu.dvdntl);
    wr_le32(raw, adapter_off + 24u, divu.written);

    wr_le32(raw, minit_frt_off + 0u, frt.ftcsr & 0xFFu);
    wr_le32(raw, minit_frt_off + 4u, frt.tier & 0xFFu);
    wr_le32(raw, minit_frt_off + 8u, frt.tcr & 0xFFu);
}
#endif

}  // namespace

uint32_t fnv1a_cpn8(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 16u; i < len; ++i) {
        h ^= static_cast<uint32_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

bool parse_cpn8_v3_bytes(const uint8_t* r, size_t len, bool verify_fnv,
                         Cpn8Meta* out_meta, Sh2CpuState cpu[2], SaturnStvBus& bus,
                         const char** out_error) {
    auto fail = [&](const char* msg) -> bool {
        if (out_error) *out_error = msg;
        return false;
    };
    if (!r || len < kMinSize) {
        return fail("Checkpoint smaller than v3 minimum");
    }
    const uint32_t magic = rd_le32(r, 0u);
    const uint32_t version = rd_le32(r, 4u);
    const uint32_t total_len = rd_le32(r, 8u);
    const uint32_t stored_fnv = rd_le32(r, 12u);
    const uint32_t fmt = rd_le32(r, 16u);
    const uint32_t schema = rd_le32(r, 20u);
    if (magic != kCpn8Magic || fmt != kCpf3Format || version != 3u || schema != 3u) {
        return fail("Invalid CPN8/CPF3 v3 header");
    }
    if (verify_fnv) {
        if (total_len != len) {
            return fail("CPN8 total length mismatch");
        }
        if (fnv1a_cpn8(r, len) != stored_fnv) {
            return fail("CPN8 FNV-1a checksum mismatch");
        }
    }

    if (out_meta) {
        out_meta->active_cpu = rd_le32(r, 596u);
        out_meta->completed_in_slice = rd_le32(r, 600u);
        out_meta->remaining = rd_le32(r, 604u);
        out_meta->mode = rd_le32(r, 612u);
        out_meta->event_count = rd_le32(r, kHeaderSize + kHighWramSize + kLowWramSize);
        out_meta->global_seq = rd_le32(r, kHeaderSize + kHighWramSize + kLowWramSize + 4u);
    }

    unpack_cpu(r, 88u, 328u, 384u + 15u * 4u, cpu[0], bus.divu[0], bus.frt[0]);
    unpack_cpu(r, 208u, 356u, 384u + 21u * 4u, cpu[1], bus.divu[1], bus.frt[1]);
    std::memcpy(bus.high_wram.data(), r + kHeaderSize, kHighWramSize);
    std::memcpy(bus.low_wram.data(), r + kHeaderSize + kHighWramSize, kLowWramSize);
    if (out_error) *out_error = nullptr;
    return true;
}

#ifndef TWC98_NO_STL
bool load_cpn8_file(const std::string& path, Cpn8Snapshot& snap,
                    Sh2CpuState cpu[2], SaturnStvBus& bus, std::string& error) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        error = "Failed to open checkpoint: " + path;
        return false;
    }
    const std::streamsize sz = ifs.tellg();
    if (sz < static_cast<std::streamsize>(kMinSize)) {
        error = "Checkpoint smaller than v3 minimum";
        return false;
    }
    snap.raw.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(snap.raw.data()), sz)) {
        error = "Failed reading checkpoint bytes";
        return false;
    }

    Cpn8Meta meta{};
    const char* c_err = nullptr;
    if (!parse_cpn8_v3_bytes(snap.raw.data(), snap.raw.size(), true, &meta, cpu, bus, &c_err)) {
        error = c_err ? c_err : "CPN8 parse failed";
        return false;
    }
    snap.active_cpu = meta.active_cpu;
    snap.completed_in_slice = meta.completed_in_slice;
    snap.remaining = meta.remaining;
    snap.mode = meta.mode;
    snap.event_count = meta.event_count;
    snap.global_seq = meta.global_seq;
    return true;
}

bool save_cpn8_updated(const std::string& path, Cpn8Snapshot& snap,
                       const Sh2CpuState cpu[2], const SaturnStvBus& bus,
                       uint32_t new_mode, uint32_t new_events, std::string& error) {
    if (snap.raw.size() < kMinSize) {
        error = "Invalid base Cpn8Snapshot";
        return false;
    }
    uint8_t* w = snap.raw.data();
    pack_cpu(w, 88u, 328u, 384u + 15u * 4u, cpu[0], bus.divu[0], bus.frt[0]);
    pack_cpu(w, 208u, 356u, 384u + 21u * 4u, cpu[1], bus.divu[1], bus.frt[1]);

    const uint64_t total_ret = cpu[0].retired + cpu[1].retired;
    const uint32_t pos = static_cast<uint32_t>(total_ret % 64u);
    const uint32_t active = (pos < 32u) ? 0u : 1u;
    const uint32_t comp = pos % 32u;
    const uint32_t rem = 32u - comp;
    wr_le32(w, 592u, 3u);  // PAUSED_CLEAN wire phase
    wr_le32(w, 596u, active);
    wr_le32(w, 600u, comp);
    wr_le32(w, 604u, rem);
    wr_le32(w, 608u, 1u);
    wr_le32(w, 612u, new_mode);

    std::memcpy(w + kHeaderSize, bus.high_wram.data(), kHighWramSize);
    std::memcpy(w + kHeaderSize + kHighWramSize, bus.low_wram.data(), kLowWramSize);
    wr_le32(w, kHeaderSize + kHighWramSize + kLowWramSize, snap.event_count);
    wr_le32(w, kHeaderSize + kHighWramSize + kLowWramSize + 4u, new_events);

    const uint32_t fnv = fnv1a_cpn8(w, snap.raw.size());
    wr_le32(w, 12u, fnv);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs || !ofs.write(reinterpret_cast<const char*>(w), static_cast<std::streamsize>(snap.raw.size()))) {
        error = "Failed writing output checkpoint: " + path;
        return false;
    }
    return true;
}
#endif

}  // namespace twc98
