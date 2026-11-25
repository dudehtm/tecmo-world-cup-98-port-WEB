// Windows x64 Interactive Desktop Host for TWC98BO G4 (Tecmo World Cup '98)
// Drives the single portable C++17 StvMachineState core with D3D11/GDI video,
// 1P+2P keyboard (P1 WASD+FGHT/R/Space) & XInput controls, WinMM waveOut SCSP audio,
// Unified Native-DPI Developer Debug Panel ([1]: EXEC/VIDEO/SYS-PAD/MEMORY),
// Pause/Step/Speed (25-200%), Multi-slot Save/Load, Deterministic Replay, and Smoke Verification.
//
// User32, Gdi32, WinMM, and XInput are loaded dynamically via LoadLibraryA ONLY when
// interactive GUI/audio/panel mode is active (!headless).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "sha256.hpp"
#include "stv_machine.hpp"
#include "twc98_copa_mod.hpp"
#include "twc98_gpu_d3d11.hpp"
#include "twc98_net_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Persistent multi-core Win32 band pool for parallel VDP2 compose_frame_band() on PC Native.
class VdpBandPool {
public:
    static constexpr int kMaxHelpers = 3; // Up to 4 total threads (main + 3 helpers)

    VdpBandPool() = default;
    ~VdpBandPool() { shutdown(); }

    void init(int requested_threads = 0) {
        if (initialized_) return;
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int cores = static_cast<int>(si.dwNumberOfProcessors);
        if (cores < 1) cores = 1;
        int total = (requested_threads > 0) ? requested_threads : std::min(4, cores);
        if (total < 1) total = 1;
        if (total > kMaxHelpers + 1) total = kMaxHelpers + 1;
        helper_count_ = total - 1;
        stop_.store(false, std::memory_order_relaxed);
        for (int i = 0; i < helper_count_; ++i) {
            slots_[i].pool = this;
            slots_[i].idx = i;
            slots_[i].ev_start = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            slots_[i].ev_done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            slots_[i].thread = CreateThread(nullptr, 0, &VdpBandPool::worker_proc, &slots_[i], 0, nullptr);
            done_handles_[i] = slots_[i].ev_done;
        }
        initialized_ = true;
    }

    void shutdown() {
        if (!initialized_) return;
        stop_.store(true, std::memory_order_release);
        for (int i = 0; i < helper_count_; ++i) {
            if (slots_[i].ev_start) SetEvent(slots_[i].ev_start);
        }
        for (int i = 0; i < helper_count_; ++i) {
            if (slots_[i].thread) {
                WaitForSingleObject(slots_[i].thread, 2000);
                CloseHandle(slots_[i].thread);
                slots_[i].thread = nullptr;
            }
            if (slots_[i].ev_start) { CloseHandle(slots_[i].ev_start); slots_[i].ev_start = nullptr; }
            if (slots_[i].ev_done) { CloseHandle(slots_[i].ev_done); slots_[i].ev_done = nullptr; }
        }
        helper_count_ = 0;
        initialized_ = false;
    }

    int thread_count() const { return helper_count_ + 1; }

    void render_frame(twc98::StvMachineState& m) {
        m.prepare_render_frame();
        if (m.vdp.compose_prep.disp_on) {
            const int h = m.vdp.frame_h;
            if (helper_count_ <= 0 || h < 64) {
                m.vdp.compose_frame_band(m.inner.vdp2_vram.data(), 0, h);
            } else {
                const int bands = helper_count_ + 1;
                cur_machine_ = &m;
                for (int i = 0; i < helper_count_; ++i) {
                    const int b_idx = i + 1;
                    slots_[i].y0 = (h * b_idx) / bands;
                    slots_[i].y1 = (h * (b_idx + 1)) / bands;
                    SetEvent(slots_[i].ev_start);
                }
                const int main_y1 = h / bands;
                m.vdp.compose_frame_band(m.inner.vdp2_vram.data(), 0, main_y1);
                WaitForMultipleObjects(static_cast<DWORD>(helper_count_), done_handles_, TRUE, INFINITE);
                cur_machine_ = nullptr;
            }
        }
        m.finish_render_frame();
    }

private:
    struct WorkerSlot {
        VdpBandPool* pool = nullptr;
        int idx = 0;
        int y0 = 0;
        int y1 = 0;
        HANDLE ev_start = nullptr;
        HANDLE ev_done = nullptr;
        HANDLE thread = nullptr;
    };

    static DWORD WINAPI worker_proc(LPVOID param) {
        auto* sl = static_cast<WorkerSlot*>(param);
        VdpBandPool* pool = sl->pool;
        for (;;) {
            WaitForSingleObject(sl->ev_start, INFINITE);
            if (pool->stop_.load(std::memory_order_acquire)) break;
            twc98::StvMachineState* m = pool->cur_machine_;
            if (m) {
                m->vdp.compose_frame_band(m->inner.vdp2_vram.data(), sl->y0, sl->y1);
            }
            SetEvent(sl->ev_done);
        }
        return 0;
    }

    WorkerSlot slots_[kMaxHelpers]{};
    HANDLE done_handles_[kMaxHelpers]{};
    twc98::StvMachineState* cur_machine_ = nullptr;
    std::atomic<bool> stop_{false};
    int helper_count_ = 0;
    bool initialized_ = false;
};

VdpBandPool g_vdp_pool;

// Dynamically loaded XInput types.
struct XInputGamepadRaw {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
struct XInputStateRaw {
    uint32_t dwPacketNumber;
    XInputGamepadRaw Gamepad;
};
using FnXInputGetState = uint32_t(WINAPI*)(uint32_t dwUserIndex, XInputStateRaw* pState);

FnXInputGetState load_xinput() {
    const char* dlls[] = {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"};
    for (const char* name : dlls) {
        HMODULE mod = LoadLibraryA(name);
        if (mod) {
            auto fn = reinterpret_cast<FnXInputGetState>(
                reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState")));
            if (fn) return fn;
        }
    }
    return nullptr;
}

template <typename FnPtr>
bool load_proc(HMODULE mod, const char* name, FnPtr& out) {
    out = reinterpret_cast<FnPtr>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
    return out != nullptr;
}

struct Win32UiApi {
    HMODULE user32 = nullptr;
    HMODULE gdi32 = nullptr;
    bool ready = false;

    LRESULT(WINAPI* def_window_proc_a)(HWND, UINT, WPARAM, LPARAM) = nullptr;
    VOID(WINAPI* post_quit_message)(int) = nullptr;
    HDC(WINAPI* begin_paint)(HWND, PAINTSTRUCT*) = nullptr;
    BOOL(WINAPI* end_paint)(HWND, const PAINTSTRUCT*) = nullptr;
    BOOL(WINAPI* get_client_rect)(HWND, RECT*) = nullptr;
    BOOL(WINAPI* adjust_window_rect)(RECT*, DWORD, BOOL) = nullptr;
    ATOM(WINAPI* register_class_a)(const WNDCLASSA*) = nullptr;
    HCURSOR(WINAPI* load_cursor_a)(HINSTANCE, LPCSTR) = nullptr;
    HWND(WINAPI* create_window_ex_a)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                     HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
    BOOL(WINAPI* destroy_window)(HWND) = nullptr;
    BOOL(WINAPI* show_window)(HWND, int) = nullptr;
    BOOL(WINAPI* update_window)(HWND) = nullptr;
    BOOL(WINAPI* peek_message_a)(MSG*, HWND, UINT, UINT, UINT) = nullptr;
    BOOL(WINAPI* translate_message)(const MSG*) = nullptr;
    LRESULT(WINAPI* dispatch_message_a)(const MSG*) = nullptr;
    HDC(WINAPI* get_dc)(HWND) = nullptr;
    int(WINAPI* release_dc)(HWND, HDC) = nullptr;
    BOOL(WINAPI* set_window_text_a)(HWND, LPCSTR) = nullptr;
    int(WINAPI* get_system_metrics)(int) = nullptr;
    LONG(WINAPI* get_window_long_a)(HWND, int) = nullptr;
    LONG(WINAPI* set_window_long_a)(HWND, int, LONG) = nullptr;
    BOOL(WINAPI* get_window_placement)(HWND, WINDOWPLACEMENT*) = nullptr;
    BOOL(WINAPI* set_window_placement)(HWND, const WINDOWPLACEMENT*) = nullptr;
    BOOL(WINAPI* set_window_pos)(HWND, HWND, int, int, int, int, UINT) = nullptr;
    HMONITOR(WINAPI* monitor_from_window)(HWND, DWORD) = nullptr;
    BOOL(WINAPI* get_monitor_info_a)(HMONITOR, LPMONITORINFO) = nullptr;
    int(WINAPI* fill_rect)(HDC, const RECT*, HBRUSH) = nullptr;
    BOOL(WINAPI* invalidate_rect)(HWND, const RECT*, BOOL) = nullptr;
    int(WINAPI* draw_text_a)(HDC, LPCSTR, int, LPRECT, UINT) = nullptr;

    int(WINAPI* set_stretch_blt_mode)(HDC, int) = nullptr;
    BOOL(WINAPI* set_brush_org_ex)(HDC, int, int, LPPOINT) = nullptr;
    HGDIOBJ(WINAPI* get_stock_object)(int) = nullptr;
    int(WINAPI* stretch_di_bits)(HDC, int, int, int, int, int, int, int, int,
                                 const VOID*, const BITMAPINFO*, UINT, DWORD) = nullptr;
    HDC(WINAPI* create_compatible_dc)(HDC) = nullptr;
    BOOL(WINAPI* delete_dc)(HDC) = nullptr;
    HBITMAP(WINAPI* create_dib_section)(HDC, const BITMAPINFO*, UINT, VOID**, HANDLE, DWORD) = nullptr;
    BOOL(WINAPI* delete_object)(HGDIOBJ) = nullptr;
    HGDIOBJ(WINAPI* select_object)(HDC, HGDIOBJ) = nullptr;
    HFONT(WINAPI* create_font_a)(int, int, int, int, int, DWORD, DWORD, DWORD,
                                 DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR) = nullptr;
    COLORREF(WINAPI* set_text_color)(HDC, COLORREF) = nullptr;
    int(WINAPI* set_bk_mode)(HDC, int) = nullptr;
    BOOL(WINAPI* bit_blt)(HDC, int, int, int, int, HDC, int, int, DWORD) = nullptr;
    BOOL(WINAPI* gdi_flush)() = nullptr;

    bool load() {
        user32 = LoadLibraryA("user32.dll");
        gdi32 = LoadLibraryA("gdi32.dll");
        if (!user32 || !gdi32) return false;
        ready =
            load_proc(user32, "DefWindowProcA", def_window_proc_a) &&
            load_proc(user32, "PostQuitMessage", post_quit_message) &&
            load_proc(user32, "BeginPaint", begin_paint) &&
            load_proc(user32, "EndPaint", end_paint) &&
            load_proc(user32, "GetClientRect", get_client_rect) &&
            load_proc(user32, "AdjustWindowRect", adjust_window_rect) &&
            load_proc(user32, "RegisterClassA", register_class_a) &&
            load_proc(user32, "LoadCursorA", load_cursor_a) &&
            load_proc(user32, "CreateWindowExA", create_window_ex_a) &&
            load_proc(user32, "DestroyWindow", destroy_window) &&
            load_proc(user32, "ShowWindow", show_window) &&
            load_proc(user32, "UpdateWindow", update_window) &&
            load_proc(user32, "PeekMessageA", peek_message_a) &&
            load_proc(user32, "TranslateMessage", translate_message) &&
            load_proc(user32, "DispatchMessageA", dispatch_message_a) &&
            load_proc(user32, "GetDC", get_dc) &&
            load_proc(user32, "ReleaseDC", release_dc) &&
            load_proc(user32, "SetWindowTextA", set_window_text_a) &&
            load_proc(user32, "GetSystemMetrics", get_system_metrics) &&
            load_proc(user32, "GetWindowLongA", get_window_long_a) &&
            load_proc(user32, "SetWindowLongA", set_window_long_a) &&
            load_proc(user32, "GetWindowPlacement", get_window_placement) &&
            load_proc(user32, "SetWindowPlacement", set_window_placement) &&
            load_proc(user32, "SetWindowPos", set_window_pos) &&
            load_proc(user32, "MonitorFromWindow", monitor_from_window) &&
            load_proc(user32, "GetMonitorInfoA", get_monitor_info_a) &&
            load_proc(user32, "FillRect", fill_rect) &&
            load_proc(user32, "InvalidateRect", invalidate_rect) &&
            load_proc(user32, "DrawTextA", draw_text_a) &&
            load_proc(gdi32, "SetStretchBltMode", set_stretch_blt_mode) &&
            load_proc(gdi32, "SetBrushOrgEx", set_brush_org_ex) &&
            load_proc(gdi32, "GetStockObject", get_stock_object) &&
            load_proc(gdi32, "StretchDIBits", stretch_di_bits) &&
            load_proc(gdi32, "CreateCompatibleDC", create_compatible_dc) &&
            load_proc(gdi32, "DeleteDC", delete_dc) &&
            load_proc(gdi32, "CreateDIBSection", create_dib_section) &&
            load_proc(gdi32, "DeleteObject", delete_object) &&
            load_proc(gdi32, "SelectObject", select_object) &&
            load_proc(gdi32, "CreateFontA", create_font_a) &&
            load_proc(gdi32, "SetTextColor", set_text_color) &&
            load_proc(gdi32, "SetBkMode", set_bk_mode) &&
            load_proc(gdi32, "BitBlt", bit_blt) &&
            load_proc(gdi32, "GdiFlush", gdi_flush);
        return ready;
    }
};

struct WinMmApi {
    HMODULE winmm = nullptr;
    bool ready = false;

    MMRESULT(WINAPI* wave_out_open)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
    MMRESULT(WINAPI* wave_out_prepare_header)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wave_out_write)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wave_out_reset)(HWAVEOUT) = nullptr;
    MMRESULT(WINAPI* wave_out_unprepare_header)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wave_out_close)(HWAVEOUT) = nullptr;
    MMRESULT(WINAPI* time_begin_period)(UINT) = nullptr;
    MMRESULT(WINAPI* time_end_period)(UINT) = nullptr;

    bool load() {
        winmm = LoadLibraryA("winmm.dll");
        if (!winmm) return false;
        ready =
            load_proc(winmm, "waveOutOpen", wave_out_open) &&
            load_proc(winmm, "waveOutPrepareHeader", wave_out_prepare_header) &&
            load_proc(winmm, "waveOutWrite", wave_out_write) &&
            load_proc(winmm, "waveOutReset", wave_out_reset) &&
            load_proc(winmm, "waveOutUnprepareHeader", wave_out_unprepare_header) &&
            load_proc(winmm, "waveOutClose", wave_out_close) &&
            load_proc(winmm, "timeBeginPeriod", time_begin_period) &&
            load_proc(winmm, "timeEndPeriod", time_end_period);
        return ready;
    }
};

Win32UiApi g_ui{};
WinMmApi g_winmm{};

// Dynamically loaded WinSock2 non-blocking UDP P2P transport for 1v1 Netplay (Puerta 2 / B-H6-02).
struct SockAddrInRaw {
    int16_t sin_family = 2; // AF_INET
    uint16_t sin_port = 0u;
    uint32_t sin_addr = 0u;
    uint8_t sin_zero[8]{};
};

struct UdpNetTransport {
    HMODULE ws2 = nullptr;
    uintptr_t sock = ~static_cast<uintptr_t>(0);
    bool active = false;
    bool has_peer = false;
    SockAddrInRaw peer_addr{};

    int(WINAPI* wsa_startup)(uint16_t, void*) = nullptr;
    int(WINAPI* wsa_cleanup)() = nullptr;
    uintptr_t(WINAPI* fn_socket)(int, int, int) = nullptr;
    int(WINAPI* fn_closesocket)(uintptr_t) = nullptr;
    int(WINAPI* fn_bind)(uintptr_t, const void*, int) = nullptr;
    int(WINAPI* fn_ioctlsocket)(uintptr_t, long, unsigned long*) = nullptr;
    int(WINAPI* fn_sendto)(uintptr_t, const char*, int, int, const void*, int) = nullptr;
    int(WINAPI* fn_recvfrom)(uintptr_t, char*, int, int, void*, int*) = nullptr;
    unsigned long(WINAPI* fn_inet_addr)(const char*) = nullptr;
    uint16_t(WINAPI* fn_htons)(uint16_t) = nullptr;

    bool init(uint16_t local_port, const std::string& peer_ep) {
        ws2 = LoadLibraryA("ws2_32.dll");
        if (!ws2) return false;
        if (!load_proc(ws2, "WSAStartup", wsa_startup) ||
            !load_proc(ws2, "WSACleanup", wsa_cleanup) ||
            !load_proc(ws2, "socket", fn_socket) ||
            !load_proc(ws2, "closesocket", fn_closesocket) ||
            !load_proc(ws2, "bind", fn_bind) ||
            !load_proc(ws2, "ioctlsocket", fn_ioctlsocket) ||
            !load_proc(ws2, "sendto", fn_sendto) ||
            !load_proc(ws2, "recvfrom", fn_recvfrom) ||
            !load_proc(ws2, "inet_addr", fn_inet_addr) ||
            !load_proc(ws2, "htons", fn_htons)) {
            return false;
        }
        uint8_t wsa_data[512]{};
        if (wsa_startup(0x0202u, wsa_data) != 0) return false;
        sock = fn_socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 17 /*IPPROTO_UDP*/);
        if (sock == ~static_cast<uintptr_t>(0)) return false;

        unsigned long nonblock = 1UL;
        fn_ioctlsocket(sock, static_cast<long>(0x8004667Eu) /*FIONBIO*/, &nonblock);

        SockAddrInRaw local_sa{};
        local_sa.sin_family = 2;
        local_sa.sin_port = fn_htons(local_port);
        local_sa.sin_addr = 0u; // INADDR_ANY
        if (fn_bind(sock, &local_sa, sizeof(local_sa)) != 0) {
            return false;
        }

        if (!peer_ep.empty()) {
            const auto colon = peer_ep.find(':');
            if (colon != std::string::npos) {
                const std::string ip = peer_ep.substr(0u, colon);
                const uint16_t pport = static_cast<uint16_t>(std::atoi(peer_ep.substr(colon + 1u).c_str()));
                peer_addr.sin_family = 2;
                peer_addr.sin_port = fn_htons(pport);
                peer_addr.sin_addr = static_cast<uint32_t>(fn_inet_addr(ip.c_str()));
                has_peer = (peer_addr.sin_addr != 0xFFFFFFFFu && pport != 0u);
            }
        }
        active = true;
        return true;
    }

    void poll_into(twc98::NetSession& net) {
        if (!active) return;
        for (int iter = 0; iter < 32; ++iter) {
            uint8_t buf[128]{};
            SockAddrInRaw from{};
            int from_len = sizeof(from);
            const int got = fn_recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, &from, &from_len);
            if (got <= 0) break;
            if (!has_peer) {
                peer_addr = from;
                has_peer = true;
            }
            if (static_cast<size_t>(got) >= twc98::NetInputPacket::kWireBytes) {
                net.receive_wire_bytes(buf, static_cast<size_t>(got));
            }
        }
    }

    void send_pkt(const twc98::NetInputPacket& pkt) {
        if (!active || !has_peer) return;
        uint8_t wire[twc98::NetInputPacket::kWireBytes]{};
        twc98::pack_net_input_packet(pkt, wire);
        fn_sendto(sock, reinterpret_cast<const char*>(wire),
                  static_cast<int>(sizeof(wire)), 0, &peer_addr, sizeof(peer_addr));
    }

    void shutdown() {
        if (active) {
            if (sock != ~static_cast<uintptr_t>(0) && fn_closesocket) fn_closesocket(sock);
            if (wsa_cleanup) wsa_cleanup();
            active = false;
        }
        if (ws2) {
            FreeLibrary(ws2);
            ws2 = nullptr;
        }
    }
};

bool read_bytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), sz));
}

bool write_bytes(const std::filesystem::path& p, const uint8_t* data, size_t len) {
    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return static_cast<bool>(ofs);
}

void write_ppm(const std::filesystem::path& p, const twc98::StvVdpState& vdp) {
    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) return;
    ofs << "P6\n" << vdp.frame_w << " " << vdp.frame_h << "\n255\n";
    for (int y = 0; y < vdp.frame_h; ++y) {
        for (int x = 0; x < vdp.frame_w; ++x) {
            const uint32_t c = vdp.frame_px[y * vdp.frame_w + x];
            const char rgb[3] = {
                static_cast<char>((c >> 16) & 0xFFu),
                static_cast<char>((c >> 8) & 0xFFu),
                static_cast<char>(c & 0xFFu)
            };
            ofs.write(rgb, 3);
        }
    }
}

uint64_t query_working_set_bytes() {
    using FnGetProcMem = BOOL(WINAPI*)(HANDLE, void*, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FnGetProcMem fn = nullptr;
    if (k32) load_proc(k32, "K32GetProcessMemoryInfo", fn);
    if (!fn) {
        HMODULE psapi = LoadLibraryA("psapi.dll");
        if (psapi) load_proc(psapi, "GetProcessMemoryInfo", fn);
    }
    if (!fn) return 0u;
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
    } pmc{};
    pmc.cb = sizeof(pmc);
    if (fn(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.PeakWorkingSetSize);
    }
    return 0u;
}

// Read-only memory peek helpers for the Dev Inspector (zero side effects on machine/replay state).
enum DbgMemRegion : int {
    MEM_HWRAM = 0, // 0x06000000 .. 0x060FFFFF
    MEM_LWRAM = 1, // 0x00200000 .. 0x002FFFFF
    MEM_SND   = 2, // 0x05A00000 .. 0x05A7FFFF
    MEM_VDP2  = 3  // 0x05E00000 .. 0x05E7FFFF
};

constexpr const char* kMemRegionNames[4] = {"HWRAM", "LWRAM", "SND-RAM", "VDP2-VR"};
constexpr uint32_t kMemRegionBases[4] = {
    twc98::kHighWramBase, twc98::kLowWramBase, 0x05A00000u, twc98::kVdp2VramBase
};
constexpr uint32_t kMemRegionSizes[4] = {
    twc98::kHighWramSize, twc98::kLowWramSize, twc98::StvMachineState::kSoundRamSize, twc98::kVdp2VramSize
};

inline uint8_t peek_region_u8(const twc98::StvMachineState& m, int region, uint32_t offset) {
    switch (region) {
        case MEM_HWRAM: return m.inner.high_wram[offset % twc98::kHighWramSize];
        case MEM_LWRAM: return m.inner.low_wram[offset % twc98::kLowWramSize];
        case MEM_SND:   return m.sound_ram[offset % twc98::StvMachineState::kSoundRamSize];
        case MEM_VDP2:  return m.inner.vdp2_vram[offset % twc98::kVdp2VramSize];
        default:        return 0u;
    }
}

inline uint16_t peek_hwram_be16(const twc98::StvMachineState& m, uint32_t stv_addr) {
    const uint32_t off = (stv_addr - twc98::kHighWramBase) % twc98::kHighWramSize;
    const uint8_t hi = m.inner.high_wram[off];
    const uint8_t lo = m.inner.high_wram[(off + 1u) % twc98::kHighWramSize];
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

inline uint32_t peek_sram_be32(const twc98::StvMachineState& m, uint32_t sram_off) {
    const uint32_t sz = twc98::StvMachineState::kSoundRamSize;
    return (static_cast<uint32_t>(m.sound_ram[sram_off % sz]) << 24) |
           (static_cast<uint32_t>(m.sound_ram[(sram_off + 1u) % sz]) << 16) |
           (static_cast<uint32_t>(m.sound_ram[(sram_off + 2u) % sz]) << 8) |
           static_cast<uint32_t>(m.sound_ram[(sram_off + 3u) % sz]);
}

enum DbgActionId : int {
    ACT_NONE = 0,
    ACT_TAB_0, ACT_TAB_1, ACT_TAB_2, ACT_TAB_3,
    ACT_DOCK_SIDE, ACT_CLOSE,
    ACT_PAUSE, ACT_STEP, ACT_QUICKMATCH,
    ACT_SPEED_DN, ACT_SPEED_UP,
    ACT_SLOT_0, ACT_SLOT_1, ACT_SLOT_2,
    ACT_SAVE, ACT_LOAD, ACT_REC, ACT_PLAY,
    ACT_TOGGLE_GPU, ACT_FULLSCREEN,
    ACT_ASPECT_0, ACT_ASPECT_1, ACT_ASPECT_2, ACT_ASPECT_3,
    ACT_FILTER_0, ACT_FILTER_1, ACT_FILTER_2, ACT_FILTER_3,
    ACT_CRT_0, ACT_CRT_1, ACT_CRT_2,
    ACT_PITCH, ACT_LAYER_VDP1, ACT_LAYER_RBG0, ACT_LAYER_NBG,
    ACT_GK_FIX, ACT_AUDIO_MUTE, ACT_XBOX_LAYOUT,
    ACT_MEM_BM_GK, ACT_MEM_BM_TEAM, ACT_MEM_BM_CODE, ACT_MEM_BM_SND, ACT_MEM_BM_BASE,
    ACT_MEM_REGION, ACT_MEM_M256, ACT_MEM_M16, ACT_MEM_P16, ACT_MEM_P256, ACT_MEM_DUMP,
    ACT_MENU_OPEN, ACT_MENU_CLOSE,
    ACT_MENU_TAB_0, ACT_MENU_TAB_1, ACT_MENU_TAB_2, ACT_MENU_TAB_3,
    ACT_MENU_CTRL_0, ACT_MENU_CTRL_1,
    ACT_VOL_DN, ACT_VOL_UP,
    ACT_PAD_DZ_DN, ACT_PAD_DZ_UP,
    ACT_KEY_RESET,
    ACT_KEY_BIND_0, ACT_KEY_BIND_1, ACT_KEY_BIND_2, ACT_KEY_BIND_3, ACT_KEY_BIND_4,
    ACT_KEY_BIND_5, ACT_KEY_BIND_6, ACT_KEY_BIND_7, ACT_KEY_BIND_8, ACT_KEY_BIND_9,
    ACT_MODE_SEL_CLASSIC, ACT_MODE_SEL_COPA, ACT_MODE_SEL_CONFIRM,
    ACT_TOGGLE_GAME_MODE, ACT_TOGGLE_HARD_MODE, ACT_CYCLE_MATCH_RULE, ACT_TOGGLE_QUICK_SKIP, ACT_OPEN_MODE_SELECT,
    ACT_RESTART_PROMPT,
    ACT_EXIT_APP
};

struct DbgHitBtn {
    int x = 0, y = 0, w = 0, h = 0;
    DbgActionId act = ACT_NONE;
};

struct DevPanelSurface {
    static constexpr int kPanelW = 416;
    static constexpr int kPanelH = 412;
    static constexpr int kMenuW = 400;
    static constexpr int kMenuH = 412;
    static constexpr int kModeSelW = 480;
    static constexpr int kModeSelH = 292;
    static constexpr int kPillW = 104;
    static constexpr int kPillH = 24;

    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bmp = nullptr;
    HFONT font_ui = nullptr;
    HFONT font_bold = nullptr;
    HFONT font_heading = nullptr;
    HFONT font_mono = nullptr;
    uint32_t* px = nullptr; // Top-down 32bpp BGRA pixels backed by GDI DIBSection
    int w = 0;
    int h = 0;
    int draw_x = 0;
    int draw_y = 0;
    std::vector<DbgHitBtn> btns;

    bool ensure(int req_w, int req_h) {
        if (mem_dc && px && w == req_w && h == req_h) return true;
        shutdown();
        if (!g_ui.ready || req_w <= 0 || req_h <= 0) return false;
        HDC scr = g_ui.get_dc(nullptr);
        mem_dc = g_ui.create_compatible_dc(scr);
        if (scr) g_ui.release_dc(nullptr, scr);
        if (!mem_dc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = req_w;
        bmi.bmiHeader.biHeight = -req_h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dib = g_ui.create_dib_section(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            shutdown();
            return false;
        }
        old_bmp = g_ui.select_object(mem_dc, dib);
        px = static_cast<uint32_t*>(bits);
        w = req_w;
        h = req_h;

        // Clean developer sans + monospace fonts (antialiasing 4=ANTIALIASED_QUALITY avoids ClearType fringes).
        font_ui = g_ui.create_font_a(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4,
                                     DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        font_bold = g_ui.create_font_a(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4,
                                        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        font_heading = g_ui.create_font_a(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4,
                                         DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        font_mono = g_ui.create_font_a(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 4,
                                       FIXED_PITCH | FF_MODERN, "Consolas");
        g_ui.set_bk_mode(mem_dc, TRANSPARENT);
        return true;
    }

    void shutdown() {
        if (mem_dc && old_bmp && g_ui.select_object) {
            g_ui.select_object(mem_dc, old_bmp);
            old_bmp = nullptr;
        }
        if (dib && g_ui.delete_object) { g_ui.delete_object(dib); dib = nullptr; }
        if (font_ui && g_ui.delete_object) { g_ui.delete_object(font_ui); font_ui = nullptr; }
        if (font_bold && g_ui.delete_object) { g_ui.delete_object(font_bold); font_bold = nullptr; }
        if (font_heading && g_ui.delete_object) { g_ui.delete_object(font_heading); font_heading = nullptr; }
        if (font_mono && g_ui.delete_object) { g_ui.delete_object(font_mono); font_mono = nullptr; }
        if (mem_dc && g_ui.delete_dc) { g_ui.delete_dc(mem_dc); mem_dc = nullptr; }
        px = nullptr;
        w = 0;
        h = 0;
    }

    void fill_rect(int rx, int ry, int rw, int rh, uint32_t argb) {
        if (!px) return;
        const int x0 = std::max(0, rx);
        const int y0 = std::max(0, ry);
        const int x1 = std::min(w, rx + rw);
        const int y1 = std::min(h, ry + rh);
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = px + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int x = x0; x < x1; ++x) row[x] = argb;
        }
    }

    void stroke_rect(int rx, int ry, int rw, int rh, uint32_t argb) {
        fill_rect(rx, ry, rw, 1, argb);
        fill_rect(rx, ry + rh - 1, rw, 1, argb);
        fill_rect(rx, ry, 1, rh, argb);
        fill_rect(rx + rw - 1, ry, 1, rh, argb);
    }

    void draw_text(int tx, int ty, int tw, int th, const char* txt, HFONT fnt, uint32_t rgb, UINT align = DT_LEFT) {
        if (!mem_dc || !txt || !g_ui.draw_text_a) return;
        HGDIOBJ prev = fnt ? g_ui.select_object(mem_dc, fnt) : nullptr;
        const COLORREF cref = RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
        g_ui.set_text_color(mem_dc, cref);
        RECT r{tx, ty, tx + tw, ty + th};
        g_ui.draw_text_a(mem_dc, txt, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (prev) g_ui.select_object(mem_dc, prev);
    }

    void add_btn(int bx, int by, int bw, int bh, const char* label,
                 DbgActionId act, bool active = false, uint32_t active_bg = 0xFA1F4E6Cu,
                 uint32_t active_bd = 0xFF38BDF8u, bool mono = false) {
        const uint32_t bg = active ? active_bg : 0xF41E293Bu;
        const uint32_t bd = active ? active_bd : 0xFF334155u;
        const uint32_t fg = active ? 0xF8FAFCu : 0xCBD5E1u;
        fill_rect(bx, by, bw, bh, bg);
        stroke_rect(bx, by, bw, bh, bd);
        if (act != ACT_NONE) btns.push_back(DbgHitBtn{bx, by, bw, bh, act});
        draw_text(bx + 4, by, bw - 8, bh, label,
                  mono ? font_mono : (active ? font_bold : font_ui), fg, DT_CENTER);
    }

    void add_disabled_btn(int bx, int by, int bw, int bh, const char* label, bool mono = false) {
        fill_rect(bx, by, bw, bh, 0xEC0E1522u);
        stroke_rect(bx, by, bw, bh, 0xFF1E293Bu);
        draw_text(bx + 4, by, bw - 8, bh, label, mono ? font_mono : font_ui, 0x64748Bu, DT_CENTER);
    }

    void add_disabled_row(int rx, int ry, int rw, int rh, const char* label, const char* status_tag = "Soon") {
        fill_rect(rx, ry, rw, rh, 0xEC0E1522u);
        stroke_rect(rx, ry, rw, rh, 0xFF1E293Bu);
        const int tag_w = 156;
        draw_text(rx + 8, ry, rw - tag_w - 20, rh, label, font_ui, 0x64748Bu, DT_LEFT);
        fill_rect(rx + rw - tag_w - 6, ry + 3, tag_w, rh - 6, 0xF0151E2Eu);
        stroke_rect(rx + rw - tag_w - 6, ry + 3, tag_w, rh - 6, 0xFF263348u);
        draw_text(rx + rw - tag_w - 4, ry + 3, tag_w - 4, rh - 6, status_tag, font_ui, 0x475569u, DT_CENTER);
    }

    void finalize_alpha() {
        if (!px) return;
        g_ui.gdi_flush();
        // GDI DrawTextA clears the top alpha byte (0x00) on pixels touched by text glyphs;
        // restore opaque alpha cleanly on glyph pixels while preserving card translucency.
        const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < total; ++i) {
            if ((px[i] & 0xFF000000u) == 0u) px[i] |= 0xFF000000u;
        }
    }
};

void vk_to_short_name(uint32_t vk, char* out, size_t cap) {
    if (!out || cap == 0u) return;
    if (vk >= 'A' && vk <= 'Z') { std::snprintf(out, cap, "%c", static_cast<char>(vk)); return; }
    if (vk >= '0' && vk <= '9') { std::snprintf(out, cap, "%c", static_cast<char>(vk)); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { std::snprintf(out, cap, "F%u", static_cast<unsigned>(vk - VK_F1 + 1u)); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { std::snprintf(out, cap, "Num%u", static_cast<unsigned>(vk - VK_NUMPAD0)); return; }
    switch (vk) {
        case VK_SPACE:   std::snprintf(out, cap, "Space"); break;
        case VK_RETURN:  std::snprintf(out, cap, "Enter"); break;
        case VK_SHIFT:   std::snprintf(out, cap, "Shift"); break;
        case VK_CONTROL: std::snprintf(out, cap, "Ctrl"); break;
        case VK_MENU:    std::snprintf(out, cap, "Alt"); break;
        case VK_TAB:     std::snprintf(out, cap, "Tab"); break;
        case VK_BACK:    std::snprintf(out, cap, "Bksp"); break;
        case VK_UP:      std::snprintf(out, cap, "Up"); break;
        case VK_DOWN:    std::snprintf(out, cap, "Down"); break;
        case VK_LEFT:    std::snprintf(out, cap, "Left"); break;
        case VK_RIGHT:   std::snprintf(out, cap, "Right"); break;
        case VK_INSERT:  std::snprintf(out, cap, "Ins"); break;
        case VK_DELETE:  std::snprintf(out, cap, "Del"); break;
        case VK_HOME:    std::snprintf(out, cap, "Home"); break;
        case VK_END:     std::snprintf(out, cap, "End"); break;
        case VK_PRIOR:   std::snprintf(out, cap, "PgUp"); break;
        case VK_NEXT:    std::snprintf(out, cap, "PgDn"); break;
        case VK_OEM_COMMA:  std::snprintf(out, cap, ","); break;
        case VK_OEM_PERIOD: std::snprintf(out, cap, "."); break;
        case VK_OEM_MINUS:  std::snprintf(out, cap, "-"); break;
        case VK_OEM_PLUS:   std::snprintf(out, cap, "+"); break;
        case VK_OEM_1:   std::snprintf(out, cap, ";"); break;
        case VK_OEM_2:   std::snprintf(out, cap, "/"); break;
        case VK_OEM_3:   std::snprintf(out, cap, "`"); break;
        case VK_OEM_4:   std::snprintf(out, cap, "["); break;
        case VK_OEM_5:   std::snprintf(out, cap, "\\"); break;
        case VK_OEM_6:   std::snprintf(out, cap, "]"); break;
        case VK_OEM_7:   std::snprintf(out, cap, "'"); break;
        default:         std::snprintf(out, cap, "0x%02X", static_cast<unsigned>(vk & 0xFFu)); break;
    }
}

// Stereo 44,100 Hz WinMM waveOut sink: streams real M68K+SCSP PCM once SG_DRV.DAT boots,
// falling back to lightweight mailbox tones only during pre-boot frames (e.g. Title).
class ScspMailboxAudio {
public:
    static constexpr uint32_t kSampleRate = 44100u;
    static constexpr uint32_t kBufCount = 8u;
    static constexpr uint32_t kBufSamples = 735u;  // Exact 44100 Hz / 60 fps frame packet (no zero-pad chops)
    static constexpr uint32_t kStageCap = 8192u;

    struct Voice {
        float phase = 0.0f;
        float freq = 220.0f;
        float env = 0.0f;
        float decay = 0.9994f;
        uint8_t cmd = 0u;
    };

    bool init(bool enable_device) {
        if (!enable_device || !g_winmm.ready) return false;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kSampleRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = kSampleRate * 4u;
        if (g_winmm.wave_out_open(&hwo_, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            hwo_ = nullptr;
            return false;
        }
        for (uint32_t i = 0; i < kBufCount; ++i) {
            hdrs_[i] = WAVEHDR{};
            hdrs_[i].lpData = reinterpret_cast<LPSTR>(pcm_[i]);
            hdrs_[i].dwBufferLength = kBufSamples * 2u * sizeof(int16_t);
            g_winmm.wave_out_prepare_header(hwo_, &hdrs_[i], sizeof(WAVEHDR));
            std::memset(pcm_[i], 0, sizeof(pcm_[i]));
            if (i < 2u) {
                queued_[i] = true;
                g_winmm.wave_out_write(hwo_, &hdrs_[i], sizeof(WAVEHDR));
            } else {
                queued_[i] = false;
            }
        }
        active_ = true;
        return true;
    }

    void trigger_event(const twc98::StvMachineState::ScspCmdEvent& ev) {
        const uint8_t cmd = static_cast<uint8_t>((ev.w0 >> 24) & 0xFFu);
        const uint8_t p1 = static_cast<uint8_t>((ev.w0 >> 16) & 0xFFu);
        const uint8_t p2 = static_cast<uint8_t>((ev.w0 >> 8) & 0xFFu);
        const size_t idx = ev.slot % 8u;
        Voice& v = voices_[idx];
        v.cmd = cmd;
        const float note_hz = 160.0f + static_cast<float>((p1 ^ p2 ^ cmd) & 0x3Fu) * 14.0f;
        v.freq = note_hz;
        v.env = (cmd == 0x05u || cmd == 0x06u) ? 0.16f : 0.24f;
        v.decay = (cmd & 1u) ? 0.9996f : 0.9992f;
        ++events_triggered_;
    }

    void pump(twc98::StvMachineState* m, bool muted, int volume_pct = 100) {
        if (!active_ || !g_winmm.ready) return;
        const int vol = muted ? 0 : std::clamp(volume_pct, 0, 100);
        const bool real_pcm = (m != nullptr && m->m68k_enabled);
        if (real_pcm) {
            int16_t tmp[2048 * 2]{};
            for (;;) {
                const uint32_t got = m->consume_audio_pcm(tmp, 2048u);
                if (got == 0u) break;
                for (uint32_t i = 0u; i < got; ++i) {
                    const uint32_t wp = stage_w_ * 2u;
                    stage_[wp] = tmp[i * 2u];
                    stage_[wp + 1u] = tmp[i * 2u + 1u];
                    stage_w_ = (stage_w_ + 1u) % kStageCap;
                    if (stage_count_ < kStageCap) ++stage_count_;
                    else stage_r_ = (stage_r_ + 1u) % kStageCap;
                }
            }
            if (stage_count_ > 4410u) {
                const uint32_t drop = stage_count_ - 2205u;
                stage_r_ = (stage_r_ + drop) % kStageCap;
                stage_count_ -= drop;
            }
        } else {
            stage_r_ = 0u;
            stage_w_ = 0u;
            stage_count_ = 0u;
            if (m != nullptr) m->clear_audio_pcm();
        }

        uint32_t in_flight = 0u;
        for (uint32_t i = 0; i < kBufCount; ++i) {
            if (queued_[i]) {
                if ((hdrs_[i].dwFlags & WHDR_DONE) != 0u) queued_[i] = false;
                else ++in_flight;
            }
        }

        for (uint32_t i = 0; i < kBufCount; ++i) {
            if (queued_[i]) continue;
            if (real_pcm) {
                if (stage_count_ < kBufSamples) {
                    break;
                }
                for (uint32_t s = 0u; s < kBufSamples; ++s) {
                    const uint32_t rp = stage_r_ * 2u;
                    if (vol == 0) {
                        pcm_[i][s * 2u] = 0;
                        pcm_[i][s * 2u + 1u] = 0;
                    } else {
                        const int32_t l = (static_cast<int32_t>(stage_[rp]) * vol) / 100;
                        const int32_t r = (static_cast<int32_t>(stage_[rp + 1u]) * vol) / 100;
                        pcm_[i][s * 2u] = static_cast<int16_t>(std::clamp(l, -32768, 32767));
                        pcm_[i][s * 2u + 1u] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
                    }
                    stage_r_ = (stage_r_ + 1u) % kStageCap;
                }
                stage_count_ -= kBufSamples;
            } else {
                if (in_flight >= 2u) break;
                fill_mailbox_stereo(pcm_[i], kBufSamples, vol);
            }
            hdrs_[i].dwFlags &= ~WHDR_DONE;
            queued_[i] = true;
            g_winmm.wave_out_write(hwo_, &hdrs_[i], sizeof(WAVEHDR));
            ++in_flight;
        }
    }

    void shutdown() {
        if (!active_ || !g_winmm.ready) return;
        g_winmm.wave_out_reset(hwo_);
        for (uint32_t i = 0; i < kBufCount; ++i) {
            queued_[i] = false;
            g_winmm.wave_out_unprepare_header(hwo_, &hdrs_[i], sizeof(WAVEHDR));
        }
        g_winmm.wave_out_close(hwo_);
        hwo_ = nullptr;
        active_ = false;
    }

    ~ScspMailboxAudio() { shutdown(); }
    bool is_active() const { return active_; }
    uint32_t events_triggered() const { return events_triggered_; }

private:
    void fill_mailbox_stereo(int16_t* dst_stereo, uint32_t count, int vol) {
        if (vol <= 0) {
            std::memset(dst_stereo, 0, count * 2u * sizeof(int16_t));
            return;
        }
        const float gain = 28000.0f * (static_cast<float>(vol) / 100.0f);
        constexpr float kTwoPi = 6.283185307f;
        for (uint32_t s = 0; s < count; ++s) {
            float mix = 0.0f;
            for (Voice& v : voices_) {
                if (v.env > 0.0005f) {
                    const float osc = std::sinf(v.phase * kTwoPi);
                    mix += osc * v.env;
                    v.phase += v.freq / static_cast<float>(kSampleRate);
                    if (v.phase >= 1.0f) v.phase -= 1.0f;
                    v.env *= v.decay;
                }
            }
            const float clamped = std::max(-0.9f, std::min(0.9f, mix));
            const int16_t v16 = static_cast<int16_t>(clamped * gain);
            dst_stereo[s * 2u] = v16;
            dst_stereo[s * 2u + 1u] = v16;
        }
    }

    HWAVEOUT hwo_ = nullptr;
    WAVEHDR hdrs_[kBufCount]{};
    int16_t pcm_[kBufCount][kBufSamples * 2u]{};
    int16_t stage_[kStageCap * 2u]{};
    uint32_t stage_r_ = 0u;
    uint32_t stage_w_ = 0u;
    uint32_t stage_count_ = 0u;
    Voice voices_[8]{};
    bool queued_[kBufCount]{};
    bool active_ = false;
    uint32_t events_triggered_ = 0u;
};

struct ReplayFramePorts {
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t pad;
};

struct DeterministicReplay {
    std::vector<uint8_t> init_snapshot;
    std::vector<ReplayFramePorts> frames;
    bool recording = false;
    bool playing = false;
    size_t play_cursor = 0u;

    bool save_to_file(const std::filesystem::path& path) const {
        if (init_snapshot.empty()) return false;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return false;
        const char magic[4] = {'T', 'W', 'C', 'R'};
        const uint32_t snap_sz = static_cast<uint32_t>(init_snapshot.size());
        const uint32_t frame_cnt = static_cast<uint32_t>(frames.size());
        ofs.write(magic, 4);
        ofs.write(reinterpret_cast<const char*>(&snap_sz), 4);
        ofs.write(reinterpret_cast<const char*>(&frame_cnt), 4);
        ofs.write(reinterpret_cast<const char*>(init_snapshot.data()), static_cast<std::streamsize>(snap_sz));
        if (frame_cnt > 0u) {
            ofs.write(reinterpret_cast<const char*>(frames.data()),
                      static_cast<std::streamsize>(frame_cnt * sizeof(ReplayFramePorts)));
        }
        return static_cast<bool>(ofs);
    }

    bool load_from_file(const std::filesystem::path& path) {
        std::vector<uint8_t> raw;
        if (!read_bytes(path, raw) || raw.size() < 12u) return false;
        if (std::memcmp(raw.data(), "TWCR", 4) != 0) return false;
        uint32_t snap_sz = 0u, frame_cnt = 0u;
        std::memcpy(&snap_sz, raw.data() + 4, 4);
        std::memcpy(&frame_cnt, raw.data() + 8, 4);
        if (12ull + snap_sz + static_cast<uint64_t>(frame_cnt) * sizeof(ReplayFramePorts) > raw.size()) return false;
        init_snapshot.assign(raw.data() + 12, raw.data() + 12 + snap_sz);
        frames.resize(frame_cnt);
        if (frame_cnt > 0u) {
            std::memcpy(frames.data(), raw.data() + 12 + snap_sz, frame_cnt * sizeof(ReplayFramePorts));
        }
        return true;
    }
};

constexpr int kSpeedPercents[4] = {25, 50, 100, 200};
constexpr int kAspectCount = 4; // 0=2X Int, 1=Fit 704:448, 2=4:3 CRT, 3=Fill
constexpr const char* kAspectNames[kAspectCount] = {"2X", "FIT", "4:3", "FILL"};

struct HostAppState {
    std::unique_ptr<twc98::StvMachineState> m;
    twc98::StvVdpState* vdp = nullptr;
    std::vector<uint32_t> scan_px;
    bool running = true;
    bool paused = false;
    bool step_one_req = false;
    int speed_idx = 2;  // 0=25%, 1=50%, 2=100%, 3=200%
    bool debug_open = false;
    int debug_tab = 0;  // 0=EXEC, 1=VIDEO, 2=SYS/PAD, 3=MEMORY
    bool debug_dock_left = false;
    bool menu_open = false;
    int menu_tab = 0;   // 0=GAME, 1=VIDEO, 2=AUDIO, 3=CONTROLS
    int ctrl_subtab = 0; // 0=1P Keyboard, 1=Xbox Controller
    bool paused_before_menu = false;
    twc98::copa::GameMode game_mode = twc98::copa::GameMode::CopaAmerica;
    twc98::copa::MatchRulesConfig match_rules{};
    bool dev_quick_skip = true;
    bool mode_select_open = false;
    bool pending_full_restart = false;
    int mode_select_cursor = 1; // 0=Classic WC98, 1=Copa America
    std::unique_ptr<twc98::copa::CopaModBackup> copa_backup;
    int audio_volume = 100;     // 0..100 step 10
    int pad_deadzone_pct = 24;  // 10..45 step 5 (default ~7849/32767)
    int rebinding_slot = -1;    // 0..9 while waiting for new P1 key
    // 0=Up, 1=Down, 2=Left, 3=Right, 4=Btn1(Pass), 5=Btn2(Shoot), 6=Btn3(Clear), 7=Btn4(Extra), 8=Start1, 9=Coin1
    uint32_t p1_key_map[10] = {'W', 'S', 'A', 'D', 'F', 'G', 'H', 'T', VK_SPACE, 'R'};

    void reset_p1_keys() {
        p1_key_map[0] = 'W'; p1_key_map[1] = 'S'; p1_key_map[2] = 'A'; p1_key_map[3] = 'D';
        p1_key_map[4] = 'F'; p1_key_map[5] = 'G'; p1_key_map[6] = 'H'; p1_key_map[7] = 'T';
        p1_key_map[8] = VK_SPACE; p1_key_map[9] = 'R';
    }
    bool is_p1_bound_vk(uint32_t vk) const {
        for (int i = 0; i < 10; ++i) {
            if (p1_key_map[i] == vk) return true;
        }
        return false;
    }
    int mem_region = MEM_HWRAM;
    uint32_t mem_offset = 0x076F40u; // Default: verified B-RE-04 GK1 actor struct
    uint8_t last_pa = 0xFFu;
    uint8_t last_pb = 0xFFu;
    uint8_t last_pc = 0xFFu;
    bool mem_dump_req = false;
    DevPanelSurface dbg_ui{};
    int slot_idx = 0;   // Scenario slot 0..2
    int aspect_mode = 0; // 0=2X Exact, 1=Fit, 2=4:3 Arcade, 3=Stretch
    bool use_gpu = true; // Active D3D11 hardware presentation (E toggles CPU-GDI A/B)
    int filter_mode = 3; // 0=Nearest, 1=Bilinear, 2=Sharp-Bi, 3=CRT-Arc
    int crt_level = 1;   // 0=Off, 1=Soft, 2=Arcade
    bool xbox_pro_layout = true; // X toggles XBOX-PRO (A/X/B + radial 8-way stick) vs DIRECT
    bool audio_muted = false;
    bool xinput_active[2]{};
    uint32_t xinput_retry[2]{};
    bool fullscreen = false;
    bool need_bg_clear = true;
    WINDOWPLACEMENT saved_placement{};
    twc98_pc_gpu::GpuD3D11Presenter gpu;
    bool save_req = false;
    bool load_req = false;
    bool rec_toggle_req = false;
    bool play_toggle_req = false;
    bool quick_match_req = false;
    bool full_restart_req = false;
    uint32_t quick_match_rem = 0u;
    double cur_fps = 60.0;
    double cur_ms = 0.0;
    uint32_t frames_run = 0u;
    uint32_t keys[256]{};
    std::filesystem::path validation_dir;
    std::string status_banner = "READY";
    DeterministicReplay replay;
};

HostAppState* g_app = nullptr;

void present_frame(HWND hwnd, HDC hdc, int cw, int ch, HostAppState& app);
void blit_hwnd(HWND hwnd, HostAppState& app);

void toggle_fullscreen(HWND hwnd, HostAppState& app) {
    if (!hwnd || !g_ui.ready) return;
    const LONG style = g_ui.get_window_long_a(hwnd, GWL_STYLE);
    if (!app.fullscreen) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HMONITOR mon = g_ui.monitor_from_window(hwnd, MONITOR_DEFAULTTOPRIMARY);
        app.saved_placement = WINDOWPLACEMENT{};
        app.saved_placement.length = sizeof(WINDOWPLACEMENT);
        if (g_ui.get_window_placement(hwnd, &app.saved_placement) &&
            g_ui.get_monitor_info_a(mon, &mi)) {
            g_ui.set_window_long_a(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            g_ui.set_window_pos(
                hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            app.fullscreen = true;
            app.need_bg_clear = true;
            app.status_banner = "FULLSCREEN";
        }
    } else {
        g_ui.set_window_long_a(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        g_ui.set_window_placement(hwnd, &app.saved_placement);
        g_ui.set_window_pos(hwnd, nullptr, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app.fullscreen = false;
        app.need_bg_clear = true;
        app.status_banner = "WINDOWED";
    }
    g_ui.invalidate_rect(hwnd, nullptr, FALSE);
}

std::filesystem::path slot_save_path(const HostAppState& app) {
    char name[64]{};
    std::snprintf(name, sizeof(name), "quicksave_slot%d.twcs", app.slot_idx);
    return app.validation_dir / name;
}

std::filesystem::path slot_replay_path(const HostAppState& app) {
    char name[64]{};
    std::snprintf(name, sizeof(name), "replay_slot%d.twcr", app.slot_idx);
    return app.validation_dir / name;
}

void set_slot_index(HostAppState& app, int next_slot) {
    app.slot_idx = ((next_slot % 3) + 3) % 3;
    if (!app.replay.recording && !app.replay.playing) {
        app.replay.init_snapshot.clear();
        app.replay.frames.clear();
    }
    app.status_banner = std::string("SLOT:") + std::to_string(app.slot_idx);
}

void open_player_menu(HostAppState& app) {
    if (!app.menu_open) {
        app.paused_before_menu = app.paused;
        app.paused = true;
    }
    app.menu_open = true;
    app.debug_open = false;
    app.rebinding_slot = -1;
    app.status_banner = "SETTINGS OPEN";
}

void close_player_menu(HostAppState& app) {
    if (app.menu_open) {
        app.paused = app.paused_before_menu;
    }
    app.menu_open = false;
    app.rebinding_slot = -1;
    app.need_bg_clear = true;
    app.status_banner = app.paused ? "PAUSED" : "RUNNING";
}

void set_host_game_mode(HostAppState& app, twc98::copa::GameMode mode, uint32_t copa_seed = 0u) {
    if (!app.m) return;
    if (!app.copa_backup) app.copa_backup = std::make_unique<twc98::copa::CopaModBackup>();
    twc98::copa::apply_game_mode(*app.m, mode, *app.copa_backup, copa_seed);
    twc98::copa::set_match_rules(*app.m, *app.copa_backup, app.match_rules);
    app.m->copa_entered_gameplay = false;
    app.m->copa_returned_to_title = false;
    app.m->copa_sp3_wind_timer = 0u;
    app.m->copa_sp3_kick_count = 0u;
    app.m->copa_ts_heartbeat = 0u;
    app.m->vdp.copa_group_badges = false;
    app.game_mode = mode;
    app.mode_select_cursor = (mode == twc98::copa::GameMode::CopaAmerica) ? 1 : 0;
    app.status_banner = std::string("MODE:") + twc98::copa::game_mode_name(mode);
}

bool boot_mode_clean(HostAppState& app,
                     const std::vector<uint8_t>& bios,
                     const std::vector<uint8_t>& cart,
                     twc98::copa::GameMode mode,
                     int boot_frames,
                     const char** err_out = nullptr) {
    if (!app.m) return false;
    const uint8_t saved_hide_layers = app.m->dbg_hide_layers;
    const bool saved_gk_fix = app.m->fix_p1_gk_ai;
    const bool saved_audio = app.m->real_audio_enabled;
    app.copa_backup.reset();
    if (!app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), err_out)) {
        return false;
    }
    app.m->dbg_hide_layers = saved_hide_layers;
    app.m->set_gk_ai_fix(saved_gk_fix);
    app.m->set_real_audio(saved_audio);
    app.m->clear_audio_pcm();
    set_host_game_mode(app, mode);
    for (int bf = 0; bf < boot_frames; ++bf) {
        app.m->set_input_ports(0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
        if (!app.m->run_rounds(10000u)) break;
        if (app.copa_backup) {
            twc98::copa::on_frame_tick(*app.m, *app.copa_backup);
        }
    }
    app.m->set_gk_ai_fix(saved_gk_fix);
    app.m->copa_entered_gameplay = false;
    app.m->copa_returned_to_title = false;
    app.m->copa_sp3_wind_timer = 0u;
    app.m->copa_sp3_kick_count = 0u;
    app.m->copa_ts_heartbeat = 0u;
    app.m->vdp.copa_group_badges = false;
    app.m->render_frame();
    return true;
}

void exec_debug_action(HWND hwnd, HostAppState& app, DbgActionId act) {
    switch (act) {
        case ACT_TAB_0: case ACT_TAB_1: case ACT_TAB_2: case ACT_TAB_3:
            app.debug_tab = static_cast<int>(act - ACT_TAB_0);
            break;
        case ACT_DOCK_SIDE:
            app.debug_dock_left = !app.debug_dock_left;
            app.status_banner = app.debug_dock_left ? "DOCK:LEFT" : "DOCK:RIGHT";
            break;
        case ACT_CLOSE:
            app.debug_open = false;
            app.need_bg_clear = true;
            app.status_banner = "DEV PANEL CLOSED";
            break;
        case ACT_PAUSE:
            app.paused = !app.paused;
            app.status_banner = app.paused ? "PAUSED" : "RUNNING";
            break;
        case ACT_STEP:
            app.step_one_req = true;
            break;
        case ACT_QUICKMATCH:
            app.quick_match_req = true;
            break;
        case ACT_SPEED_DN:
            if (app.speed_idx > 0) --app.speed_idx;
            app.status_banner = std::string("SPD:") + std::to_string(kSpeedPercents[app.speed_idx]) + "%";
            break;
        case ACT_SPEED_UP:
            if (app.speed_idx < 3) ++app.speed_idx;
            app.status_banner = std::string("SPD:") + std::to_string(kSpeedPercents[app.speed_idx]) + "%";
            break;
        case ACT_SLOT_0: case ACT_SLOT_1: case ACT_SLOT_2:
            set_slot_index(app, static_cast<int>(act - ACT_SLOT_0));
            break;
        case ACT_SAVE: app.save_req = true; break;
        case ACT_LOAD: app.load_req = true; break;
        case ACT_REC:  app.rec_toggle_req = true; break;
        case ACT_PLAY: app.play_toggle_req = true; break;
        case ACT_TOGGLE_GPU:
            if (app.use_gpu) {
                app.use_gpu = false;
                app.need_bg_clear = true;
                app.status_banner = "RENDER:CPU-GDI";
            } else {
                RECT rc{};
                if (hwnd) g_ui.get_client_rect(hwnd, &rc);
                const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
                const int ch = std::max(1, static_cast<int>(rc.bottom - rc.top));
                if (app.gpu.ready || (hwnd && app.gpu.init(hwnd, cw, ch))) {
                    app.use_gpu = true;
                    app.status_banner = std::string("RENDER:") + app.gpu.backend_name;
                } else {
                    app.use_gpu = false;
                    app.status_banner = "GPU FALLBACK:CPU";
                }
            }
            break;
        case ACT_FULLSCREEN:
            if (hwnd) toggle_fullscreen(hwnd, app);
            break;
        case ACT_ASPECT_0: case ACT_ASPECT_1: case ACT_ASPECT_2: case ACT_ASPECT_3:
            app.aspect_mode = static_cast<int>(act - ACT_ASPECT_0);
            app.need_bg_clear = true;
            app.status_banner = std::string("VIEW:") + kAspectNames[app.aspect_mode];
            break;
        case ACT_FILTER_0: case ACT_FILTER_1: case ACT_FILTER_2: case ACT_FILTER_3:
            app.filter_mode = static_cast<int>(act - ACT_FILTER_0);
            app.status_banner = std::string("FLT:") + twc98_pc_gpu::kFilterNames[app.filter_mode];
            break;
        case ACT_CRT_0: case ACT_CRT_1: case ACT_CRT_2:
            app.crt_level = static_cast<int>(act - ACT_CRT_0);
            app.status_banner = std::string("CRT:") + twc98_pc_gpu::kCrtLevelNames[app.crt_level];
            break;
        case ACT_PITCH:
            app.m->dbg_hide_layers ^= 0x08u;
            app.m->render_frame();
            app.status_banner = (app.m->dbg_hide_layers & 0x08u) ? "PITCH:HD-704" : "PITCH:STV-352";
            break;
        case ACT_LAYER_VDP1:
            app.m->dbg_hide_layers ^= 1u;
            app.m->render_frame();
            app.status_banner = (app.m->dbg_hide_layers & 1u) ? "VDP1:OFF" : "VDP1:ON";
            break;
        case ACT_LAYER_RBG0:
            app.m->dbg_hide_layers ^= 2u;
            app.m->render_frame();
            app.status_banner = (app.m->dbg_hide_layers & 2u) ? "RBG0:OFF" : "RBG0:ON";
            break;
        case ACT_LAYER_NBG:
            app.m->dbg_hide_layers ^= 4u;
            app.m->render_frame();
            app.status_banner = (app.m->dbg_hide_layers & 4u) ? "NBG:OFF" : "NBG:ON";
            break;
        case ACT_GK_FIX: {
            const bool next_gk = !app.m->fix_p1_gk_ai;
            app.m->set_gk_ai_fix(next_gk);
            app.status_banner = next_gk ? "GK:AUTO-FIX" : "GK:ORIG-98";
            break;
        }
        case ACT_AUDIO_MUTE:
            app.audio_muted = !app.audio_muted;
            app.status_banner = app.audio_muted ? "AUDIO:MUTED" : "AUDIO:ON";
            break;
        case ACT_XBOX_LAYOUT:
            app.xbox_pro_layout = !app.xbox_pro_layout;
            app.status_banner = app.xbox_pro_layout ? "PAD:XBOX-PRO" : "PAD:DIRECT";
            break;
        case ACT_MEM_BM_GK:
            app.mem_region = MEM_HWRAM;
            app.mem_offset = 0x076F40u;
            app.status_banner = "MEM:GK1 06076F40";
            break;
        case ACT_MEM_BM_TEAM:
            app.mem_region = MEM_HWRAM;
            app.mem_offset = 0x076540u;
            app.status_banner = "MEM:TEAM 06076540";
            break;
        case ACT_MEM_BM_CODE:
            app.mem_region = MEM_HWRAM;
            app.mem_offset = 0x033A00u;
            app.status_banner = "MEM:GKOP 06033A00";
            break;
        case ACT_MEM_BM_SND:
            app.mem_region = MEM_SND;
            app.mem_offset = 0x000700u;
            app.status_banner = "MEM:SND 05A00700";
            break;
        case ACT_MEM_BM_BASE:
            app.mem_region = MEM_HWRAM;
            app.mem_offset = 0x000000u;
            app.status_banner = "MEM:WRAM 06000000";
            break;
        case ACT_MEM_REGION:
            app.mem_region = (app.mem_region + 1) % 4;
            app.mem_offset %= kMemRegionSizes[app.mem_region];
            app.status_banner = std::string("REGION:") + kMemRegionNames[app.mem_region];
            break;
        case ACT_MEM_M256: {
            const uint32_t sz = kMemRegionSizes[app.mem_region];
            app.mem_offset = (app.mem_offset + sz - 256u) % sz;
            break;
        }
        case ACT_MEM_M16: {
            const uint32_t sz = kMemRegionSizes[app.mem_region];
            app.mem_offset = (app.mem_offset + sz - 16u) % sz;
            break;
        }
        case ACT_MEM_P16:
            app.mem_offset = (app.mem_offset + 16u) % kMemRegionSizes[app.mem_region];
            break;
        case ACT_MEM_P256:
            app.mem_offset = (app.mem_offset + 256u) % kMemRegionSizes[app.mem_region];
            break;
        case ACT_MEM_DUMP:
            app.mem_dump_req = true;
            break;
        case ACT_MENU_OPEN:
            open_player_menu(app);
            break;
        case ACT_MENU_CLOSE:
            close_player_menu(app);
            break;
        case ACT_MENU_TAB_0: case ACT_MENU_TAB_1: case ACT_MENU_TAB_2: case ACT_MENU_TAB_3:
            app.menu_tab = static_cast<int>(act - ACT_MENU_TAB_0);
            app.rebinding_slot = -1;
            break;
        case ACT_MENU_CTRL_0: case ACT_MENU_CTRL_1:
            app.ctrl_subtab = static_cast<int>(act - ACT_MENU_CTRL_0);
            app.rebinding_slot = -1;
            break;
        case ACT_VOL_DN:
            app.audio_volume = std::max(0, app.audio_volume - 10);
            app.status_banner = std::string("VOL:") + std::to_string(app.audio_volume) + "%";
            break;
        case ACT_VOL_UP:
            app.audio_volume = std::min(100, app.audio_volume + 10);
            if (app.audio_volume > 0 && app.audio_muted) app.audio_muted = false;
            app.status_banner = std::string("VOL:") + std::to_string(app.audio_volume) + "%";
            break;
        case ACT_PAD_DZ_DN:
            app.pad_deadzone_pct = std::max(10, app.pad_deadzone_pct - 5);
            app.status_banner = std::string("DZ:") + std::to_string(app.pad_deadzone_pct) + "%";
            break;
        case ACT_PAD_DZ_UP:
            app.pad_deadzone_pct = std::min(45, app.pad_deadzone_pct + 5);
            app.status_banner = std::string("DZ:") + std::to_string(app.pad_deadzone_pct) + "%";
            break;
        case ACT_KEY_RESET:
            app.reset_p1_keys();
            app.rebinding_slot = -1;
            app.status_banner = "P1 KEYS:DEFAULT";
            break;
        case ACT_KEY_BIND_0: case ACT_KEY_BIND_1: case ACT_KEY_BIND_2: case ACT_KEY_BIND_3: case ACT_KEY_BIND_4:
        case ACT_KEY_BIND_5: case ACT_KEY_BIND_6: case ACT_KEY_BIND_7: case ACT_KEY_BIND_8: case ACT_KEY_BIND_9:
            app.rebinding_slot = static_cast<int>(act - ACT_KEY_BIND_0);
            app.status_banner = "PRESS NEW KEY...";
            break;
        case ACT_MODE_SEL_CLASSIC:
            app.mode_select_cursor = 0;
            if (app.pending_full_restart) {
                app.pending_full_restart = false;
                app.game_mode = twc98::copa::GameMode::Classic;
                app.full_restart_req = true;
                app.status_banner = "REBOOTING...";
            } else {
                set_host_game_mode(app, twc98::copa::GameMode::Classic);
            }
            app.mode_select_open = false;
            app.need_bg_clear = true;
            break;
        case ACT_MODE_SEL_COPA:
            app.mode_select_cursor = 1;
            if (app.pending_full_restart || app.game_mode != twc98::copa::GameMode::CopaAmerica) {
                app.pending_full_restart = false;
                app.game_mode = twc98::copa::GameMode::CopaAmerica;
                app.full_restart_req = true;
                app.status_banner = "REBOOTING...";
            } else {
                set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica);
            }
            app.mode_select_open = false;
            app.need_bg_clear = true;
            break;
        case ACT_MODE_SEL_CONFIRM: {
            const auto chosen = (app.mode_select_cursor == 0)
                ? twc98::copa::GameMode::Classic
                : twc98::copa::GameMode::CopaAmerica;
            const bool need_copa_cold_boot =
                (chosen == twc98::copa::GameMode::CopaAmerica &&
                 app.game_mode != twc98::copa::GameMode::CopaAmerica);
            if (app.pending_full_restart || need_copa_cold_boot) {
                app.pending_full_restart = false;
                app.game_mode = chosen;
                app.full_restart_req = true;
                app.status_banner = "REBOOTING...";
            } else {
                set_host_game_mode(app, chosen);
            }
            app.mode_select_open = false;
            app.need_bg_clear = true;
            break;
        }
        case ACT_TOGGLE_GAME_MODE:
            if (app.game_mode == twc98::copa::GameMode::CopaAmerica) {
                set_host_game_mode(app, twc98::copa::GameMode::Classic);
            } else {
                app.game_mode = twc98::copa::GameMode::CopaAmerica;
                app.full_restart_req = true;
                app.status_banner = "REBOOTING...";
            }
            break;
        case ACT_TOGGLE_HARD_MODE:
            app.match_rules.hard_mode = !app.match_rules.hard_mode;
            if (app.m && app.copa_backup) {
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, app.match_rules);
            }
            app.status_banner = app.match_rules.hard_mode ? "HARD MODE:ON" : "HARD MODE:OFF";
            break;
        case ACT_CYCLE_MATCH_RULE: {
            using MRM = twc98::copa::MatchRuleMode;
            if (app.match_rules.rule_mode == MRM::Default) {
                app.match_rules.rule_mode = MRM::TimeLimit;
                app.match_rules.rule_value = 120u;
            } else if (app.match_rules.rule_mode == MRM::TimeLimit && app.match_rules.rule_value == 120u) {
                app.match_rules.rule_value = 180u;
            } else if (app.match_rules.rule_mode == MRM::TimeLimit) {
                app.match_rules.rule_mode = MRM::GoalLimit;
                app.match_rules.rule_value = 3u;
            } else if (app.match_rules.rule_mode == MRM::GoalLimit && app.match_rules.rule_value == 3u) {
                app.match_rules.rule_value = 5u;
            } else {
                app.match_rules.rule_mode = MRM::Default;
                app.match_rules.rule_value = 90u;
            }
            if (app.m && app.copa_backup) {
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, app.match_rules);
            }
            app.status_banner = "RULE UPDATED";
            break;
        }
        case ACT_TOGGLE_QUICK_SKIP:
            app.dev_quick_skip = !app.dev_quick_skip;
            app.status_banner = app.dev_quick_skip ? "QUICK-SKIP:ON" : "QUICK-SKIP:OFF";
            break;
        case ACT_OPEN_MODE_SELECT:
            close_player_menu(app);
            app.debug_open = false;
            app.pending_full_restart = false;
            app.mode_select_cursor = (app.game_mode == twc98::copa::GameMode::CopaAmerica) ? 1 : 0;
            app.mode_select_open = true;
            app.need_bg_clear = true;
            app.status_banner = "MODE SELECT";
            break;
        case ACT_RESTART_PROMPT:
            close_player_menu(app);
            app.debug_open = false;
            app.paused = false;
            app.pending_full_restart = true;
            app.mode_select_cursor = (app.game_mode == twc98::copa::GameMode::CopaAmerica) ? 1 : 0;
            app.mode_select_open = true;
            app.need_bg_clear = true;
            app.status_banner = "RESTART: CHOOSE MODE";
            break;
        case ACT_EXIT_APP:
            app.running = false;
            if (g_ui.ready) g_ui.post_quit_message(0);
            break;
        default:
            break;
    }
}

static void handle_pc_keydown(HWND hwnd, uint32_t vk, bool first_press) {
    g_app->keys[vk] = 1u;
    if (!first_press) return;
    if (g_app->menu_open && g_app->rebinding_slot >= 0) {
        if (vk == VK_ESCAPE) {
            g_app->rebinding_slot = -1;
            g_app->status_banner = "REBIND CANCELLED";
            return;
        }
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_SNAPSHOT ||
            vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) {
            return;
        }
        g_app->p1_key_map[g_app->rebinding_slot] = vk;
        char kn[24]{};
        vk_to_short_name(vk, kn, sizeof(kn));
        g_app->status_banner = std::string("BOUND:") + kn;
        g_app->rebinding_slot = -1;
        return;
    }
    if (g_app->mode_select_open) {
        if (vk == VK_LEFT || vk == VK_UP || vk == 'A' || vk == 'W') {
            g_app->mode_select_cursor = 0;
            return;
        }
        if (vk == VK_RIGHT || vk == VK_DOWN || vk == 'D' || vk == 'S') {
            g_app->mode_select_cursor = 1;
            return;
        }
        if (vk == VK_RETURN || vk == VK_SPACE || vk == 'F' || vk == 'G') {
            exec_debug_action(hwnd, *g_app, ACT_MODE_SEL_CONFIRM);
            return;
        }
    }
    if (vk == VK_ESCAPE) {
        if (g_app->mode_select_open) {
            if (g_app->pending_full_restart) {
                g_app->pending_full_restart = false;
                g_app->mode_select_open = false;
                g_app->need_bg_clear = true;
                g_app->status_banner = "RESTART CANCELLED";
            } else {
                exec_debug_action(hwnd, *g_app, ACT_MODE_SEL_CONFIRM);
            }
            return;
        }
        if (g_app->debug_open) exec_debug_action(hwnd, *g_app, ACT_CLOSE);
        else if (g_app->menu_open) exec_debug_action(hwnd, *g_app, ACT_MENU_CLOSE);
        else exec_debug_action(hwnd, *g_app, ACT_MENU_OPEN);
    }
    else if (vk == '1' || vk == VK_NUMPAD1) {
        if (g_app->menu_open) close_player_menu(*g_app);
        g_app->debug_open = !g_app->debug_open;
        if (!g_app->debug_open) g_app->need_bg_clear = true;
        g_app->status_banner = g_app->debug_open ? "DEV PANEL OPEN" : "DEV PANEL CLOSED";
    }
    else if (g_app->menu_open && vk == VK_PRIOR) g_app->menu_tab = (g_app->menu_tab + 3) % 4;
    else if (g_app->menu_open && vk == VK_NEXT)  g_app->menu_tab = (g_app->menu_tab + 1) % 4;
    else if (g_app->debug_open && vk == VK_PRIOR) g_app->debug_tab = (g_app->debug_tab + 3) % 4;
    else if (g_app->debug_open && vk == VK_NEXT)  g_app->debug_tab = (g_app->debug_tab + 1) % 4;
    else if (!g_app->is_p1_bound_vk(vk)) {
        if (vk == 'P') exec_debug_action(hwnd, *g_app, ACT_PAUSE);
        else if (vk == VK_OEM_PERIOD) exec_debug_action(hwnd, *g_app, ACT_STEP);
        else if (vk == '3') exec_debug_action(hwnd, *g_app, ACT_SPEED_DN);
        else if (vk == '4') exec_debug_action(hwnd, *g_app, ACT_SPEED_UP);
        else if (vk == 'V') exec_debug_action(hwnd, *g_app, static_cast<DbgActionId>(ACT_ASPECT_0 + ((g_app->aspect_mode + 1) % kAspectCount)));
        else if (vk == 'B') exec_debug_action(hwnd, *g_app, static_cast<DbgActionId>(ACT_FILTER_0 + ((g_app->filter_mode + 1) % twc98_pc_gpu::kFilterCount)));
        else if (vk == 'E') exec_debug_action(hwnd, *g_app, ACT_TOGGLE_GPU);
        else if (vk == 'Q') exec_debug_action(hwnd, *g_app, ACT_PITCH);
        else if (vk == 'X') exec_debug_action(hwnd, *g_app, ACT_XBOX_LAYOUT);
        else if (vk == VK_F1) exec_debug_action(hwnd, *g_app, ACT_GK_FIX);
        else if (vk == 'M') exec_debug_action(hwnd, *g_app, ACT_AUDIO_MUTE);
        else if (vk == VK_F11) exec_debug_action(hwnd, *g_app, ACT_FULLSCREEN);
        else if (vk == VK_F12) exec_debug_action(hwnd, *g_app, static_cast<DbgActionId>(ACT_CRT_0 + ((g_app->crt_level + 1) % twc98_pc_gpu::kCrtLevelCount)));
        else if (vk == VK_TAB) set_slot_index(*g_app, g_app->slot_idx + 1);
        else if (vk == VK_F2) exec_debug_action(hwnd, *g_app, ACT_LAYER_VDP1);
        else if (vk == VK_F3) exec_debug_action(hwnd, *g_app, ACT_LAYER_RBG0);
        else if (vk == VK_F4) exec_debug_action(hwnd, *g_app, ACT_LAYER_NBG);
        else if (vk == VK_F5) exec_debug_action(hwnd, *g_app, ACT_SAVE);
        else if (vk == VK_F7) exec_debug_action(hwnd, *g_app, ACT_QUICKMATCH);
        else if (vk == VK_F8) exec_debug_action(hwnd, *g_app, ACT_LOAD);
        else if (vk == VK_F9) exec_debug_action(hwnd, *g_app, ACT_REC);
        else if (vk == VK_F10) exec_debug_action(hwnd, *g_app, ACT_PLAY);
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ui.ready) return 0;
    if (!g_app) return g_ui.def_window_proc_a(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_app->running = false;
            g_ui.post_quit_message(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g_app->need_bg_clear = true;
            return 0;
        case WM_SYSKEYDOWN: {
            const uint32_t vk = static_cast<uint32_t>(wp & 0xFFu);
            const bool first_press = ((lp & (1LL << 30)) == 0);
            const bool alt_down = ((lp & (1LL << 29)) != 0);
            if (alt_down) {
                if (first_press && vk == VK_RETURN) {
                    toggle_fullscreen(hwnd, *g_app);
                    return 0;
                }
                return g_ui.def_window_proc_a(hwnd, msg, wp, lp);
            }
            // Non-Alt syskeys (e.g. VK_F10) must not open the Windows menu bar; route to game hotkeys.
            handle_pc_keydown(hwnd, vk, first_press);
            return 0;
        }
        case WM_SYSKEYUP: {
            if ((lp & (1LL << 29)) == 0) {
                const uint32_t vk = static_cast<uint32_t>(wp & 0xFFu);
                g_app->keys[vk] = 0u;
                return 0;
            }
            return g_ui.def_window_proc_a(hwnd, msg, wp, lp);
        }
        case WM_KEYDOWN: {
            const uint32_t vk = static_cast<uint32_t>(wp & 0xFFu);
            const bool first_press = ((lp & (1LL << 30)) == 0);
            handle_pc_keydown(hwnd, vk, first_press);
            return 0;
        }
        case WM_KEYUP: {
            const uint32_t vk = static_cast<uint32_t>(wp & 0xFFu);
            g_app->keys[vk] = 0u;
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (g_app->dbg_ui.px) {
                const int mx = static_cast<int>(static_cast<int16_t>(LOWORD(lp)));
                const int my = static_cast<int>(static_cast<int16_t>(HIWORD(lp)));
                const int lx = mx - g_app->dbg_ui.draw_x;
                const int ly = my - g_app->dbg_ui.draw_y;
                if (lx >= 0 && lx < g_app->dbg_ui.w && ly >= 0 && ly < g_app->dbg_ui.h) {
                    for (const auto& b : g_app->dbg_ui.btns) {
                        if (lx >= b.x && lx < b.x + b.w && ly >= b.y && ly < b.y + b.h) {
                            exec_debug_action(hwnd, *g_app, b.act);
                            blit_hwnd(hwnd, *g_app);
                            return 0;
                        }
                    }
                    if (g_app->debug_open || g_app->menu_open) return 0;
                }
            }
            return g_ui.def_window_proc_a(hwnd, msg, wp, lp);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = g_ui.begin_paint(hwnd, &ps);
            RECT rc{};
            g_ui.get_client_rect(hwnd, &rc);
            const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
            const int ch = std::max(1, static_cast<int>(rc.bottom - rc.top));
            g_app->need_bg_clear = true;
            present_frame(hwnd, hdc, cw, ch, *g_app);
            g_ui.end_paint(hwnd, &ps);
            return 0;
        }
        default:
            return g_ui.def_window_proc_a(hwnd, msg, wp, lp);
    }
}

void sample_live_inputs(HostAppState& app, FnXInputGetState xi,
                        uint8_t& port_a, uint8_t& port_b, uint8_t& port_c) {
    port_a = 0xFFu;
    port_b = 0xFFu;
    port_c = 0xFFu;

    // Player 1 mapped keyboard (defaults: WASD / F,G,H,T / Space=Start1 / R=Coin1) + Arrow convenience
    if (app.keys[app.p1_key_map[4]]) port_a &= static_cast<uint8_t>(~0x01u);
    if (app.keys[app.p1_key_map[5]]) port_a &= static_cast<uint8_t>(~0x02u);
    if (app.keys[app.p1_key_map[6]]) port_a &= static_cast<uint8_t>(~0x04u);
    if (app.keys[app.p1_key_map[7]]) port_a &= static_cast<uint8_t>(~0x08u);
    if (app.keys[app.p1_key_map[1]] || (!app.is_p1_bound_vk(VK_DOWN) && app.keys[VK_DOWN]))   port_a &= static_cast<uint8_t>(~0x10u);
    if (app.keys[app.p1_key_map[0]] || (!app.is_p1_bound_vk(VK_UP) && app.keys[VK_UP]))       port_a &= static_cast<uint8_t>(~0x20u);
    if (app.keys[app.p1_key_map[3]] || (!app.is_p1_bound_vk(VK_RIGHT) && app.keys[VK_RIGHT])) port_a &= static_cast<uint8_t>(~0x40u);
    if (app.keys[app.p1_key_map[2]] || (!app.is_p1_bound_vk(VK_LEFT) && app.keys[VK_LEFT]))   port_a &= static_cast<uint8_t>(~0x80u);

    // Player 2 keyboard: I/J/K/L + U/O/N/M + 6=Coin2 + 7=2PStart
    if (app.keys['U']) port_b &= static_cast<uint8_t>(~0x01u);
    if (app.keys['O']) port_b &= static_cast<uint8_t>(~0x02u);
    if (app.keys['N']) port_b &= static_cast<uint8_t>(~0x04u);
    if (app.keys['M']) port_b &= static_cast<uint8_t>(~0x08u);
    if (app.keys['K']) port_b &= static_cast<uint8_t>(~0x10u);
    if (app.keys['I']) port_b &= static_cast<uint8_t>(~0x20u);
    if (app.keys['L']) port_b &= static_cast<uint8_t>(~0x40u);
    if (app.keys['J']) port_b &= static_cast<uint8_t>(~0x80u);

    // System switches: P1 Coin/Start from P1 map, 6=Coin2, 7=2PStart, 9=Test, 0=Service
    if (app.keys[app.p1_key_map[9]]) port_c &= static_cast<uint8_t>(~0x01u);
    if (app.keys['6']) port_c &= static_cast<uint8_t>(~0x02u);
    if (app.keys[app.p1_key_map[8]]) port_c &= static_cast<uint8_t>(~0x10u);
    if (app.keys['7']) port_c &= static_cast<uint8_t>(~0x20u);
    if (app.keys['9']) port_c &= static_cast<uint8_t>(~0x40u);
    if (app.keys['0']) port_c &= static_cast<uint8_t>(~0x80u);

    if (xi) {
        const int64_t dz = (32767LL * static_cast<int64_t>(app.pad_deadzone_pct)) / 100LL;
        const int64_t kDeadzoneSq = dz * dz;
        for (uint32_t pad = 0; pad < 2; ++pad) {
            if (app.xinput_retry[pad] > 0u) {
                --app.xinput_retry[pad];
                continue;
            }
            XInputStateRaw xs{};
            if (xi(pad, &xs) == 0u) {
                app.xinput_active[pad] = true;
                uint8_t& dst = (pad == 0) ? port_a : port_b;
                const uint16_t b = xs.Gamepad.wButtons;
                const int16_t lx = xs.Gamepad.sThumbLX;
                const int16_t ly = xs.Gamepad.sThumbLY;

                if (app.xbox_pro_layout) {
                    // Xbox PC Football preset: A/LT=Btn1 (Pass/Steal), X/RT=Btn2 (Shoot/Slide),
                    // B=Btn3 (Long/Clear), Y/LB/RB=Btn4
                    if ((b & 0x1000u) || xs.Gamepad.bLeftTrigger > 80u) dst &= static_cast<uint8_t>(~0x01u);
                    if ((b & 0x4000u) || xs.Gamepad.bRightTrigger > 80u) dst &= static_cast<uint8_t>(~0x02u);
                    if (b & 0x2000u) dst &= static_cast<uint8_t>(~0x04u);
                    if (b & (0x8000u | 0x0100u | 0x0200u)) dst &= static_cast<uint8_t>(~0x08u);
                } else {
                    // Direct 1:1 Xbox pad -> ST-V buttons
                    if (b & 0x1000u) dst &= static_cast<uint8_t>(~0x01u);
                    if (b & 0x2000u) dst &= static_cast<uint8_t>(~0x02u);
                    if (b & 0x4000u) dst &= static_cast<uint8_t>(~0x04u);
                    if (b & 0x8000u) dst &= static_cast<uint8_t>(~0x08u);
                }

                // D-Pad (bitmask 0x0001=Up, 0x0002=Down, 0x0004=Left, 0x0008=Right)
                if (b & 0x0002u) dst &= static_cast<uint8_t>(~0x10u);
                if (b & 0x0001u) dst &= static_cast<uint8_t>(~0x20u);
                if (b & 0x0008u) dst &= static_cast<uint8_t>(~0x40u);
                if (b & 0x0004u) dst &= static_cast<uint8_t>(~0x80u);

                // Smooth radial deadzone + 8-way 22.5-degree octant quantization on Left Stick
                const int64_t mag2 = static_cast<int64_t>(lx) * lx + static_cast<int64_t>(ly) * ly;
                if (mag2 > kDeadzoneSq) {
                    const int32_t ax = (lx < 0) ? -static_cast<int32_t>(lx) : static_cast<int32_t>(lx);
                    const int32_t ay = (ly < 0) ? -static_cast<int32_t>(ly) : static_cast<int32_t>(ly);
                    if (ay * 12 >= ax * 5) {
                        dst &= static_cast<uint8_t>(ly > 0 ? ~0x20u : ~0x10u);
                    }
                    if (ax * 12 >= ay * 5) {
                        dst &= static_cast<uint8_t>(lx > 0 ? ~0x40u : ~0x80u);
                    }
                }

                if (b & 0x0010u) port_c &= static_cast<uint8_t>(pad == 0 ? ~0x10u : ~0x20u); // Start
                if (b & 0x0020u) port_c &= static_cast<uint8_t>(pad == 0 ? ~0x01u : ~0x02u); // Back=Coin
            } else {
                app.xinput_active[pad] = false;
                app.xinput_retry[pad] = 30u; // ~0.5s backoff so unplugged slots do not stall 60Hz loop
            }
        }
    }
}

void compose_debug_panel(HostAppState& app, int cw, int ch) {
    if (!app.debug_open || !app.m) return;
    const int pw = std::min(DevPanelSurface::kPanelW, std::max(280, cw - 20));
    const int ph = std::min(DevPanelSurface::kPanelH, std::max(240, ch - 20));
    auto& ui = app.dbg_ui;
    if (!ui.ensure(pw, ph)) return;
    ui.btns.clear();
    ui.draw_x = app.debug_dock_left ? 12 : std::max(12, cw - pw - 12);
    ui.draw_y = 12;

    // Dark developer translucent card (#111621) + crisp 1px border (#2E3C50).
    ui.fill_rect(0, 0, pw, ph, 0xEC111621u);
    ui.stroke_rect(0, 0, pw, ph, 0xFF2E3C50u);

    // Header strip (y=1..30)
    ui.fill_rect(1, 1, pw - 2, 29, 0xF5182130u);
    ui.fill_rect(1, 30, pw - 2, 1, 0xFF283548u);
    ui.fill_rect(10, 6, 36, 18, 0xFF0284C7u);
    ui.draw_text(10, 6, 36, 18, "DEV", ui.font_bold, 0xFFFFFFu, DT_CENTER);

    char hdr[96]{};
    std::snprintf(hdr, sizeof(hdr), "%.1f FPS  %.1fms  F:%u",
                  app.cur_fps, app.cur_ms, app.frames_run);
    ui.draw_text(52, 6, 194, 18, hdr, ui.font_mono, 0xCBD5E1u, DT_LEFT);

    const char* state_lbl = app.replay.recording ? "REC*"
        : (app.replay.playing ? "PLAY>" : (app.paused ? "PAUSED" : "RUN"));
    const uint32_t state_bg = app.replay.recording ? 0xFF7F1D1Du
        : (app.paused ? 0xFF78350Fu : 0xFF14532Du);
    const uint32_t state_fg = app.replay.recording ? 0xFCA5A5u
        : (app.paused ? 0xFDE68Au : 0x86EFACu);
    ui.fill_rect(pw - 118, 6, 56, 18, state_bg);
    ui.draw_text(pw - 118, 6, 56, 18, state_lbl, ui.font_bold, state_fg, DT_CENTER);

    ui.add_btn(pw - 56, 5, 24, 20, app.debug_dock_left ? ">" : "<", ACT_DOCK_SIDE, false);
    ui.add_btn(pw - 28, 5, 22, 20, "X", ACT_CLOSE, false, 0xFF7F1D1Du, 0xFFEF4444u);

    // 4 Segmented Area Tabs (y=31..58)
    ui.fill_rect(1, 31, pw - 2, 27, 0xF2141B27u);
    ui.fill_rect(1, 58, pw - 2, 1, 0xFF283548u);
    static constexpr const char* kTabNames[4] = {"1. EXEC", "2. VIDEO", "3. SYS/PAD", "4. MEMORY"};
    const int tab_w = (pw - 16) / 4;
    for (int t = 0; t < 4; ++t) {
        const int tx = 8 + t * tab_w;
        const bool active = (app.debug_tab == t);
        if (active) {
            ui.fill_rect(tx, 32, tab_w - 2, 26, 0xF81E2B3Fu);
            ui.fill_rect(tx, 56, tab_w - 2, 2, 0xFF38BDF8u);
        }
        ui.btns.push_back(DbgHitBtn{tx, 31, tab_w - 2, 27, static_cast<DbgActionId>(ACT_TAB_0 + t)});
        ui.draw_text(tx, 32, tab_w - 2, 24, kTabNames[t],
                     active ? ui.font_bold : ui.font_ui,
                     active ? 0xF8FAFCu : 0x94A3B8u, DT_CENTER);
    }

    auto draw_sec = [&](int y, const char* title) {
        ui.draw_text(12, y, pw - 24, 16, title, ui.font_bold, 0x38BDF8u, DT_LEFT);
        ui.fill_rect(12, y + 17, pw - 24, 1, 0xFF243042u);
    };

    if (app.debug_tab == 0) {
        // AREA 1: EXECUTION, SAVESTATES & REPLAY
        draw_sec(66, "EXECUTION CONTROL");
        ui.add_btn(12, 88, 128, 26, app.paused ? "Resume  [P]" : "Pause  [P]",
                   ACT_PAUSE, app.paused, 0xFA78350Fu, 0xFFF59E0Bu);
        ui.add_btn(146, 88, 114, 26, "+1 Frame  [.]", ACT_STEP, false);
        ui.add_btn(266, 88, 138, 26, "QuickMatch [F7]", ACT_QUICKMATCH, app.quick_match_rem > 0u);

        ui.draw_text(12, 122, 74, 24, "Speed:", ui.font_ui, 0x94A3B8u, DT_LEFT);
        ui.add_btn(88, 121, 56, 24, "-  [3]", ACT_SPEED_DN, false);
        char spd[32]{};
        std::snprintf(spd, sizeof(spd), "%d%%", kSpeedPercents[app.speed_idx]);
        ui.fill_rect(150, 121, 68, 24, 0xF40F172A);
        ui.stroke_rect(150, 121, 68, 24, 0xFF334155u);
        ui.draw_text(150, 121, 68, 24, spd, ui.font_mono, 0xF8FAFCu, DT_CENTER);
        ui.add_btn(224, 121, 56, 24, "+  [4]", ACT_SPEED_UP, false);
        ui.add_btn(286, 121, 118, 24, "Full Restart", ACT_RESTART_PROMPT, false, 0xFF7F1D1Du, 0xFFEF4444u);

        draw_sec(158, "SAVESTATE SLOTS  [Tab]");
        for (int s = 0; s < 3; ++s) {
            char sl[16]{};
            std::snprintf(sl, sizeof(sl), "Slot %d", s);
            ui.add_btn(12 + s * 64, 180, 58, 26, sl, static_cast<DbgActionId>(ACT_SLOT_0 + s), app.slot_idx == s);
        }
        ui.add_btn(214, 180, 92, 26, "Save  [F5]", ACT_SAVE, false);
        ui.add_btn(312, 180, 92, 26, "Load  [F8]", ACT_LOAD, false);

        draw_sec(218, "DETERMINISTIC REPLAY (.twcr)");
        ui.add_btn(12, 240, 124, 26, app.replay.recording ? "Stop Rec  [F9]" : "Record  [F9]",
                   ACT_REC, app.replay.recording, 0xFA7F1D1Du, 0xFFEF4444u);
        ui.add_btn(142, 240, 124, 26, app.replay.playing ? "Stop Play [F10]" : "Play  [F10]",
                   ACT_PLAY, app.replay.playing, 0xFA14532Du, 0xFF22C55Eu);

        char rep_stat[48]{};
        std::snprintf(rep_stat, sizeof(rep_stat), "%zu / %zu F",
                      app.replay.play_cursor, app.replay.frames.size());
        ui.fill_rect(272, 240, 132, 26, 0xF40F172A);
        ui.stroke_rect(272, 240, 132, 26, 0xFF334155u);
        ui.draw_text(272, 240, 132, 26, rep_stat, ui.font_mono, 0x93C5FDu, DT_CENTER);

        ui.fill_rect(12, 274, pw - 24, 34, 0xF00D131Du);
        ui.stroke_rect(12, 274, pw - 24, 34, 0xFF243042u);
        char rpath[96]{};
        std::snprintf(rpath, sizeof(rpath), "Target: validation_g4/replay_slot%d.twcr", app.slot_idx);
        ui.draw_text(18, 281, pw - 36, 20, rpath, ui.font_mono, 0x94A3B8u, DT_LEFT);

        draw_sec(320, "CONTROLS REFERENCE");
        ui.draw_text(12, 340, pw - 24, 18, "P1 Keys: WASD/Arrows   F/G/H/T:Btn1-4   R:Coin   Space:Start", ui.font_ui, 0xCBD5E1u, DT_LEFT);
        ui.draw_text(12, 358, pw - 24, 18, "Xbox PC: LS/D-Pad + A/X/B/Y/LB/RB   Start:Start   Back:Coin", ui.font_ui, 0x94A3B8u, DT_LEFT);
    } else if (app.debug_tab == 1) {
        // AREA 2: VIDEO, PRESENTER, FILTERS & VDP LAYERS
        draw_sec(66, "PRESENTER & VIEWPORT");
        const char* backend_tag = (app.use_gpu && app.gpu.ready) ? app.gpu.backend_name.c_str() : "CPU-GDI";
        char be_lbl[64]{};
        std::snprintf(be_lbl, sizeof(be_lbl), "Backend: %s  [E]", backend_tag);
        ui.add_btn(12, 88, 232, 26, be_lbl, ACT_TOGGLE_GPU, app.use_gpu && app.gpu.ready, 0xFA14532Du, 0xFF22C55Eu);
        ui.add_btn(250, 88, 154, 26, app.fullscreen ? "Windowed  [F11]" : "Fullscreen [F11]",
                   ACT_FULLSCREEN, app.fullscreen);

        ui.draw_text(12, 122, 80, 24, "Aspect [V]:", ui.font_ui, 0x94A3B8u, DT_LEFT);
        for (int a = 0; a < kAspectCount; ++a) {
            ui.add_btn(94 + a * 78, 121, 74, 24, kAspectNames[a],
                       static_cast<DbgActionId>(ACT_ASPECT_0 + a), app.aspect_mode == a);
        }

        draw_sec(158, "SHADER FILTER & CRT MASK");
        ui.draw_text(12, 180, 80, 24, "Filter [B]:", ui.font_ui, 0x94A3B8u, DT_LEFT);
        static constexpr const char* kShortFlt[4] = {"NEAR", "BILIN", "SHARP", "CRT-A"};
        for (int f = 0; f < 4; ++f) {
            ui.add_btn(94 + f * 78, 179, 74, 24, kShortFlt[f],
                       static_cast<DbgActionId>(ACT_FILTER_0 + f), app.filter_mode == f);
        }

        ui.draw_text(12, 214, 80, 24, "CRT [F12]:", ui.font_ui, 0x94A3B8u, DT_LEFT);
        for (int c = 0; c < 3; ++c) {
            ui.add_btn(94 + c * 104, 213, 98, 24, twc98_pc_gpu::kCrtLevelNames[c],
                       static_cast<DbgActionId>(ACT_CRT_0 + c), app.crt_level == c);
        }

        draw_sec(250, "ST-V VDP1 / VDP2 HARDWARE LAYERS");
        const bool hd_rbg = (app.m->dbg_hide_layers & 0x08u) != 0u;
        ui.add_btn(12, 272, 246, 26, hd_rbg ? "RBG0 Pitch: HD-704  [Q]" : "RBG0 Pitch: STV-352  [Q]",
                   ACT_PITCH, hd_rbg, 0xFA14532Du, 0xFF22C55Eu);
        char res_str[48]{};
        std::snprintf(res_str, sizeof(res_str), "Core: %dx%d", app.vdp->frame_w, app.vdp->frame_h);
        ui.fill_rect(264, 272, 140, 26, 0xF40F172A);
        ui.stroke_rect(264, 272, 140, 26, 0xFF334155u);
        ui.draw_text(264, 272, 140, 26, res_str, ui.font_mono, 0xCBD5E1u, DT_CENTER);

        const bool v1_on = (app.m->dbg_hide_layers & 1u) == 0u;
        const bool rbg_on = (app.m->dbg_hide_layers & 2u) == 0u;
        const bool nbg_on = (app.m->dbg_hide_layers & 4u) == 0u;
        ui.add_btn(12, 306, 126, 26, v1_on ? "VDP1: ON [F2]" : "VDP1: OFF [F2]",
                   ACT_LAYER_VDP1, v1_on, 0xFA14532Du, 0xFF22C55Eu);
        ui.add_btn(145, 306, 126, 26, rbg_on ? "RBG0: ON [F3]" : "RBG0: OFF [F3]",
                   ACT_LAYER_RBG0, rbg_on, 0xFA14532Du, 0xFF22C55Eu);
        ui.add_btn(278, 306, 126, 26, nbg_on ? "NBG: ON [F4]" : "NBG: OFF [F4]",
                   ACT_LAYER_NBG, nbg_on, 0xFA14532Du, 0xFF22C55Eu);

        ui.fill_rect(12, 344, pw - 24, 30, 0xF00D131Du);
        ui.draw_text(18, 348, pw - 36, 22, "Overlay runs native 1:1 DPI above CRT shader passes.", ui.font_ui, 0x94A3B8u, DT_LEFT);
    } else if (app.debug_tab == 2) {
        // AREA 3: SYSTEM, CPU, HARDWARE, AUDIO & CONTROLLERS
        draw_sec(66, "SH-2 CPUS & ST-V BUS");
        ui.fill_rect(12, 88, pw - 24, 62, 0xF00D131Du);
        ui.stroke_rect(12, 88, pw - 24, 62, 0xFF243042u);
        char c1[96]{}, c2[96]{}, c3[96]{};
        std::snprintf(c1, sizeof(c1), "Main  PC: %08X   Steps: %-8u  VBI: %u",
                      app.m->cpus[0].pc, app.m->main_steps, app.m->vbi_count);
        std::snprintf(c2, sizeof(c2), "Slave PC: %08X   Steps: %-8u  VBO: %u",
                      app.m->cpus[1].pc, app.m->slave_steps, app.m->vbo_count);
        std::snprintf(c3, sizeof(c3), "IOGA Rd : %-8u   Cart : %-8u  SCU-DMA: %u",
                      app.m->ioga_reads, app.m->cart_reads, app.m->scu_dma_runs);
        ui.draw_text(18, 92, pw - 36, 18, c1, ui.font_mono, 0xFDE68Au, DT_LEFT);
        ui.draw_text(18, 110, pw - 36, 18, c2, ui.font_mono, 0xFDE68Au, DT_LEFT);
        ui.draw_text(18, 128, pw - 36, 18, c3, ui.font_mono, 0x93C5FDu, DT_LEFT);

        ui.add_btn(12, 156, 212, 24, app.m->fix_p1_gk_ai ? "GK1 AI: AUTO-FIX [F1]" : "GK1 AI: ORIG-98  [F1]",
                   ACT_GK_FIX, app.m->fix_p1_gk_ai, 0xFA14532Du, 0xFF22C55Eu);
        char hw_st[64]{};
        std::snprintf(hw_st, sizeof(hw_st), "315-5881:%s  Halt:%s",
                      app.m->cart_prot_enabled ? "ON" : "OFF",
                      app.m->halted ? "YES" : "NO");
        ui.fill_rect(230, 156, 174, 24, 0xF40F172A);
        ui.stroke_rect(230, 156, 174, 24, 0xFF334155u);
        ui.draw_text(230, 156, 174, 24, hw_st, ui.font_mono, app.m->halted ? 0xFCA5A5u : 0xCBD5E1u, DT_CENTER);

        draw_sec(190, "AUDIO (M68EC000 + SCSP YMF292)");
        const uint32_t sl_mask = app.m->scsp.active_slots_mask();
        uint32_t sl_cnt = 0u;
        for (uint32_t b = 0u; b < 32u; ++b) {
            if ((sl_mask >> b) & 1u) ++sl_cnt;
        }
        const char* aud_tag = app.audio_muted ? "MUTED"
            : (app.m->m68k_enabled ? "M68K+SCSP" : (app.m->real_audio_enabled ? "PRE-BOOT" : "OFF"));
        ui.add_btn(12, 212, 166, 24, app.audio_muted ? "Audio: MUTED [M]" : "Audio: ON  [M]",
                   ACT_AUDIO_MUTE, !app.audio_muted, 0xFA14532Du, 0xFF22C55Eu);
        char aud_info[64]{};
        std::snprintf(aud_info, sizeof(aud_info), "Mode:%-9s Slots:%2u/32", aud_tag, sl_cnt);
        ui.fill_rect(184, 212, 220, 24, 0xF40F172A);
        ui.stroke_rect(184, 212, 220, 24, 0xFF334155u);
        ui.draw_text(184, 212, 220, 24, aud_info, ui.font_mono, 0x86EFACu, DT_CENTER);

        char a2[96]{};
        std::snprintf(a2, sizeof(a2), "M68K PC: %06X   Seq:%-5u  SRAM Wr:%-6u  PCM:%u",
                      app.m->m68k_pc_cached & 0xFFFFFFu, app.m->scsp_event_seq,
                      app.m->soundram_writes, app.m->audio_pcm_produced);
        ui.draw_text(14, 242, pw - 28, 20, a2, ui.font_mono, 0xCBD5E1u, DT_LEFT);

        draw_sec(272, "INPUTS & RAW ST-V PORTS");
        ui.add_btn(12, 294, 204, 24, app.xbox_pro_layout ? "Xbox Map: XBOX-PRO [X]" : "Xbox Map: DIRECT   [X]",
                   ACT_XBOX_LAYOUT, app.xbox_pro_layout, 0xFA1F4E6Cu, 0xFF38BDF8u);
        char xi_str[64]{};
        std::snprintf(xi_str, sizeof(xi_str), "XInput P1:%s  P2:%s",
                      app.xinput_active[0] ? "ON" : "--",
                      app.xinput_active[1] ? "ON" : "--");
        ui.fill_rect(222, 294, 182, 24, 0xF40F172A);
        ui.stroke_rect(222, 294, 182, 24, 0xFF334155u);
        ui.draw_text(222, 294, 182, 24, xi_str, ui.font_mono, 0xE2E8F0u, DT_CENTER);

        ui.fill_rect(12, 326, pw - 24, 44, 0xF00D131Du);
        ui.stroke_rect(12, 326, pw - 24, 44, 0xFF243042u);
        char p1[96]{}, p2[96]{};
        std::snprintf(p1, sizeof(p1), "Active-Low Ports ->  PA:%02X   PB:%02X   PC:%02X",
                      app.last_pa, app.last_pb, app.last_pc);
        std::snprintf(p2, sizeof(p2), "SMPC Reads: %-8u   IOGA Wr: %u",
                      app.m->smpc_input_reads, app.m->ioga_writes);
        ui.draw_text(18, 330, pw - 36, 18, p1, ui.font_mono, 0x38BDF8u, DT_LEFT);
        ui.draw_text(18, 348, pw - 36, 18, p2, ui.font_mono, 0x94A3B8u, DT_LEFT);
    } else {
        // AREA 4: VERIFIED RAM WATCH & LIVE MEMORY INSPECTOR (B-DBG-02)
        draw_sec(66, "VERIFIED SH-2 / SOUND BOOKMARKS");
        const bool bm_gk = (app.mem_region == MEM_HWRAM && app.mem_offset == 0x076F40u);
        const bool bm_tm = (app.mem_region == MEM_HWRAM && app.mem_offset == 0x076540u);
        const bool bm_cd = (app.mem_region == MEM_HWRAM && app.mem_offset == 0x033A00u);
        const bool bm_sn = (app.mem_region == MEM_SND && app.mem_offset == 0x000700u);
        const bool bm_bs = (app.mem_region == MEM_HWRAM && app.mem_offset == 0x000000u);
        ui.add_btn(12,  86, 74, 22, "GK1 State",  ACT_MEM_BM_GK, bm_gk);
        ui.add_btn(90,  86, 76, 22, "Team/CPU",   ACT_MEM_BM_TEAM, bm_tm);
        ui.add_btn(170, 86, 74, 22, "GK Patch",   ACT_MEM_BM_CODE, bm_cd);
        ui.add_btn(248, 86, 76, 22, "Snd Mail",   ACT_MEM_BM_SND, bm_sn);
        ui.add_btn(328, 86, 76, 22, "WorkRAM",    ACT_MEM_BM_BASE, bm_bs);

        // Live read-only values of verified B-RE-04 / SCSP addresses
        const uint16_t gk_ai = peek_hwram_be16(*app.m, 0x06076F48u);
        const uint16_t p1_act = peek_hwram_be16(*app.m, 0x0607654Au);
        const uint16_t cpu_msk = peek_hwram_be16(*app.m, 0x06077F68u);
        const uint16_t op_33a00 = peek_hwram_be16(*app.m, 0x06033A00u);
        const uint16_t op_34280 = peek_hwram_be16(*app.m, 0x06034280u);
        const uint32_t snd_cmd = peek_sram_be32(*app.m, 0x0700u);

        ui.fill_rect(12, 113, pw - 24, 40, 0xF00D131Du);
        ui.stroke_rect(12, 113, pw - 24, 40, 0xFF243042u);
        char w1[96]{}, w2[96]{};
        std::snprintf(w1, sizeof(w1), "GK[6F48]:%04X  P1Act[654A]:%04X  CpuMsk[7F68]:%04X",
                      gk_ai, p1_act, cpu_msk);
        std::snprintf(w2, sizeof(w2), "GKOp[33A00/34280]:%04X/%04X  Snd[700]:%08X",
                      op_33a00, op_34280, snd_cmd);
        ui.draw_text(18, 115, pw - 36, 18, w1, ui.font_mono, 0x86EFACu, DT_LEFT);
        ui.draw_text(18, 133, pw - 36, 18, w2, ui.font_mono, 0xFDE68Au, DT_LEFT);

        // Region + address stepper
        ui.add_btn(12, 159, 86, 24, kMemRegionNames[app.mem_region], ACT_MEM_REGION, true);
        ui.add_btn(104, 159, 48, 24, "-256", ACT_MEM_M256, false, 0, 0, true);
        ui.add_btn(156, 159, 42, 24, "-16",  ACT_MEM_M16,  false, 0, 0, true);
        char abuf[32]{};
        std::snprintf(abuf, sizeof(abuf), "%08X",
                      kMemRegionBases[app.mem_region] + app.mem_offset);
        ui.fill_rect(202, 159, 100, 24, 0xF40F172A);
        ui.stroke_rect(202, 159, 100, 24, 0xFF38BDF8u);
        ui.draw_text(202, 159, 100, 24, abuf, ui.font_mono, 0x38BDF8u, DT_CENTER);
        ui.add_btn(306, 159, 44, 24, "+16",  ACT_MEM_P16,  false, 0, 0, true);
        ui.add_btn(354, 159, 50, 24, "+256", ACT_MEM_P256, false, 0, 0, true);

        // 8 rows x 8 bytes = 64 bytes hex + ASCII viewer
        ui.fill_rect(12, 188, pw - 24, 156, 0xF20B101Au);
        ui.stroke_rect(12, 188, pw - 24, 156, 0xFF243042u);
        const uint32_t rsize = kMemRegionSizes[app.mem_region];
        const uint32_t rbase = kMemRegionBases[app.mem_region];
        for (int row = 0; row < 8; ++row) {
            const uint32_t off = (app.mem_offset + static_cast<uint32_t>(row) * 8u) % rsize;
            char line[96]{};
            int pos = std::snprintf(line, sizeof(line), "%08X: ", rbase + off);
            char ascii[9]{};
            for (int col = 0; col < 8; ++col) {
                const uint8_t v = peek_region_u8(*app.m, app.mem_region, off + static_cast<uint32_t>(col));
                pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "%02X ", v);
                ascii[col] = (v >= 32u && v <= 126u) ? static_cast<char>(v) : '.';
            }
            std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), " %s", ascii);
            ui.draw_text(18, 192 + row * 18, pw - 36, 18, line, ui.font_mono,
                         (row & 1) ? 0xCBD5E1u : 0xE2E8F0u, DT_LEFT);
        }

        ui.add_btn(12, 350, 232, 24, "Dump HWRAM + SoundRAM (.bin)", ACT_MEM_DUMP, false);
        ui.draw_text(250, 350, 154, 24, "SH-2 Big-Endian Peek", ui.font_ui, 0x94A3B8u, DT_RIGHT);
    }

    // Compact footer bar (y=ph-24..ph)
    ui.fill_rect(1, ph - 24, pw - 2, 23, 0xF5151C28u);
    ui.fill_rect(1, ph - 25, pw - 2, 1, 0xFF283548u);
    ui.draw_text(10, ph - 23, 178, 22, app.status_banner.c_str(), ui.font_mono, 0x38BDF8u, DT_LEFT);
    ui.draw_text(190, ph - 23, pw - 200, 22, "1:Hide  PgUp/Dn:Tab  Click:UI", ui.font_ui, 0x64748Bu, DT_RIGHT);
    ui.finalize_alpha();
}

void compose_corner_trigger(HostAppState& app, int cw, int ch) {
    (void)ch;
    const int pw = DevPanelSurface::kPillW;
    const int ph = DevPanelSurface::kPillH;
    auto& ui = app.dbg_ui;
    if (!ui.ensure(pw, ph)) return;
    ui.btns.clear();
    ui.draw_x = std::max(8, cw - pw - 12);
    ui.draw_y = 12;

    ui.fill_rect(0, 0, pw, ph, 0xD8111621u);
    ui.stroke_rect(0, 0, pw, ph, 0xFF334155u);
    ui.fill_rect(4, 4, 42, ph - 8, 0xFF059669u);
    ui.draw_text(4, 4, 42, ph - 8, "MENU", ui.font_bold, 0xFFFFFFu, DT_CENTER);
    ui.draw_text(50, 0, pw - 54, ph, "[Esc]", ui.font_mono, 0xCBD5E1u, DT_CENTER);
    ui.btns.push_back(DbgHitBtn{0, 0, pw, ph, ACT_MENU_OPEN});
    ui.finalize_alpha();
}

void compose_mode_select(HostAppState& app, int cw, int ch) {
    const int pw = std::min(DevPanelSurface::kModeSelW, std::max(360, cw - 24));
    const int ph = std::min(DevPanelSurface::kModeSelH, std::max(280, ch - 24));
    auto& ui = app.dbg_ui;
    if (!ui.ensure(pw, ph)) return;
    ui.btns.clear();
    ui.draw_x = std::max(0, (cw - pw) / 2);
    ui.draw_y = std::max(0, (ch - ph) / 2);

    // Quiet, compact host overlay; mode actions and game rendering stay unchanged.
    ui.fill_rect(0, 0, pw, ph, 0xF8101720u);
    ui.stroke_rect(0, 0, pw, ph, 0xFF344150u);
    ui.draw_text(24, 18, pw - 48, 28, "Select mode", ui.font_heading, 0xF1F5F9u, DT_LEFT);
    ui.draw_text(24, 49, pw - 48, 18,
                 app.pending_full_restart ? "Choose an edition for the restart."
                                          : "Two editions. The same arcade spirit.",
                 ui.font_ui, 0x94A3B8u, DT_LEFT);

    const int gap = 12;
    const int card_w = (pw - 48 - gap) / 2;
    const int card_y = 82;
    const int card_h = ph - 154;
    for (int mode = 0; mode < 2; ++mode) {
        const int cx = 24 + mode * (card_w + gap);
        const bool sel = app.mode_select_cursor == mode;
        ui.fill_rect(cx, card_y, card_w, card_h, sel ? 0xFF182D2Cu : 0xFF171F2Au);
        ui.stroke_rect(cx, card_y, card_w, card_h, sel ? 0xFF52C9AFu : 0xFF344150u);
        ui.btns.push_back(DbgHitBtn{cx, card_y, card_w, card_h,
                                   mode == 0 ? ACT_MODE_SEL_CLASSIC : ACT_MODE_SEL_COPA});
        ui.draw_text(cx + 16, card_y + 12, card_w - 32, 16,
                     mode == 0 ? "ORIGINAL" : "MOD EDITION", ui.font_mono, 0x94A3B8u, DT_LEFT);
        ui.draw_text(cx + 16, card_y + 36, card_w - 32, 22,
                     mode == 0 ? "Classic '98" : "Copa America", ui.font_bold, 0xF1F5F9u, DT_LEFT);
        ui.draw_text(cx + 16, card_y + 63, card_w - 32, 18,
                     mode == 0 ? "Official teams" : "12 national teams", ui.font_ui, 0xCBD5E1u, DT_LEFT);
        ui.draw_text(cx + 16, card_y + 82, card_w - 32, 18,
                     mode == 0 ? "Original gameplay" : "New roster & kits", ui.font_ui, 0xCBD5E1u, DT_LEFT);
        if (sel) {
            ui.fill_rect(cx + 16, card_y + card_h - 17, 5, 5, 0xFF52C9AFu);
            ui.draw_text(cx + 28, card_y + card_h - 24, card_w - 40, 18,
                         "Selected", ui.font_ui, 0x75DCC5u, DT_LEFT);
        }
    }

    ui.fill_rect(24, ph - 68, pw - 48, 1, 0xFF2B3643u);
    ui.add_btn(24, ph - 54, 156, 32,
               app.pending_full_restart ? "Restart  [Enter]" : "Play  [Enter]",
               ACT_MODE_SEL_CONFIRM, true, 0xFF235E53u, 0xFF52C9AFu);
    ui.add_btn(pw - 148, ph - 54, 124, 32,
               app.dev_quick_skip ? "Dev skip: ON" : "Dev skip: OFF",
               ACT_TOGGLE_QUICK_SKIP, false, 0xFF171F2Au, 0xFF344150u);
    ui.draw_text(24, ph - 20, pw - 48, 16,
                 app.pending_full_restart ? "Arrows: choose   /   Click: launch   /   Esc: cancel"
                                          : "Arrows: choose   /   Click or Enter: launch",
                 ui.font_ui, 0x94A3B8u, DT_CENTER);
    ui.finalize_alpha();
}

void compose_player_menu(HostAppState& app, int cw, int ch) {
    if (!app.menu_open || !app.m) return;
    const int pw = std::min(DevPanelSurface::kMenuW, std::max(280, cw - 20));
    const int ph = std::min(DevPanelSurface::kMenuH, std::max(240, ch - 20));
    auto& ui = app.dbg_ui;
    if (!ui.ensure(pw, ph)) return;
    ui.btns.clear();
    ui.draw_x = std::max(12, cw - pw - 12);
    ui.draw_y = 12;

    // Clean player Game Settings card (#101724) + emerald/slate accents.
    ui.fill_rect(0, 0, pw, ph, 0xEE101724u);
    ui.stroke_rect(0, 0, pw, ph, 0xFF2D3E55u);

    // Header strip (y=1..30)
    ui.fill_rect(1, 1, pw - 2, 29, 0xF5172233u);
    ui.fill_rect(1, 30, pw - 2, 1, 0xFF2A3A50u);
    ui.fill_rect(10, 6, 46, 18, 0xFF059669u);
    ui.draw_text(10, 6, 46, 18, "GAME", ui.font_bold, 0xFFFFFFu, DT_CENTER);
    ui.draw_text(64, 6, 184, 18, "Game Settings", ui.font_bold, 0xF8FAFCu, DT_LEFT);

    ui.fill_rect(pw - 94, 6, 56, 18, 0xFF78350Fu);
    ui.draw_text(pw - 94, 6, 56, 18, "PAUSED", ui.font_bold, 0xFDE68Au, DT_CENTER);
    ui.add_btn(pw - 32, 5, 24, 20, "X", ACT_MENU_CLOSE, false, 0xFF7F1D1Du, 0xFFEF4444u);

    // 4 Segmented Tabs (y=31..58)
    ui.fill_rect(1, 31, pw - 2, 27, 0xF2131C2Au);
    ui.fill_rect(1, 58, pw - 2, 1, 0xFF2A3A50u);
    static constexpr const char* kMenuTabs[4] = {"1. GAME", "2. VIDEO", "3. AUDIO", "4. CONTROLS"};
    const int tab_w = (pw - 16) / 4;
    for (int t = 0; t < 4; ++t) {
        const int tx = 8 + t * tab_w;
        const bool active = (app.menu_tab == t);
        if (active) {
            ui.fill_rect(tx, 32, tab_w - 2, 26, 0xF81B2C3Eu);
            ui.fill_rect(tx, 56, tab_w - 2, 2, 0xFF10B981u);
        }
        ui.btns.push_back(DbgHitBtn{tx, 31, tab_w - 2, 27, static_cast<DbgActionId>(ACT_MENU_TAB_0 + t)});
        ui.draw_text(tx, 32, tab_w - 2, 24, kMenuTabs[t],
                     active ? ui.font_bold : ui.font_ui,
                     active ? 0xF8FAFCu : 0x94A3B8u, DT_CENTER);
    }

    auto draw_sec = [&](int y, const char* title) {
        ui.draw_text(12, y, pw - 24, 16, title, ui.font_bold, 0x34D399u, DT_LEFT);
        ui.fill_rect(12, y + 17, pw - 24, 1, 0xFF233246u);
    };

    if (app.menu_tab == 0) {
        // TAB 0: GAME
        draw_sec(64, "MATCH & SESSION");
        ui.add_btn(12, 86, 184, 26, "Resume Game  [Esc]", ACT_MENU_CLOSE, true, 0xFA065F46u, 0xFF10B981u);
        ui.add_btn(204, 86, 184, 26, "Quick Match  [F7]", ACT_QUICKMATCH, app.quick_match_rem > 0u);

        ui.add_btn(12, 118, 184, 24,
                   (app.game_mode == twc98::copa::GameMode::CopaAmerica) ? "Mode: COPA AMERICA" : "Mode: CLASSIC WC98",
                   ACT_TOGGLE_GAME_MODE, app.game_mode == twc98::copa::GameMode::CopaAmerica);
        ui.add_btn(204, 118, 184, 24,
                   app.m->fix_p1_gk_ai ? "1P GK Assist: ON" : "1P GK Assist: OFF",
                   ACT_GK_FIX, app.m->fix_p1_gk_ai);

        ui.draw_text(12, 148, 66, 24, "Save Slot", ui.font_ui, 0x94A3B8u, DT_LEFT);
        for (int s = 0; s < 3; ++s) {
            char sb[8]{};
            std::snprintf(sb, sizeof(sb), "%d", s);
            ui.add_btn(82 + s * 38, 148, 34, 24, sb, static_cast<DbgActionId>(ACT_SLOT_0 + s), app.slot_idx == s);
        }
        ui.add_btn(200, 148, 90, 24, "Save [F5]", ACT_SAVE, false);
        ui.add_btn(298, 148, 90, 24, "Load [F8]", ACT_LOAD, false);

        ui.add_btn(12, 178, 184, 24, "Mode Select Screen...", ACT_OPEN_MODE_SELECT, false);
        ui.add_btn(204, 178, 184, 24,
                   app.dev_quick_skip ? "Dev Quick-Skip: ON" : "Dev Quick-Skip: OFF",
                   ACT_TOGGLE_QUICK_SKIP, app.dev_quick_skip);

        draw_sec(210, "MATCH RULES & TOOLS");
        ui.add_btn(12, 232, 184, 24,
                   app.match_rules.hard_mode ? "Hard Mode: ON (Max)" : "Hard Mode: OFF",
                   ACT_TOGGLE_HARD_MODE, app.match_rules.hard_mode);
        char rule_lbl[40]{};
        if (app.match_rules.rule_mode == twc98::copa::MatchRuleMode::TimeLimit) {
            std::snprintf(rule_lbl, sizeof(rule_lbl), "Rule: Time %us", static_cast<unsigned>(app.match_rules.rule_value));
        } else if (app.match_rules.rule_mode == twc98::copa::MatchRuleMode::GoalLimit) {
            std::snprintf(rule_lbl, sizeof(rule_lbl), "Rule: First to %uG", static_cast<unsigned>(app.match_rules.rule_value));
        } else {
            std::snprintf(rule_lbl, sizeof(rule_lbl), "Rule: Arcade Time");
        }
        ui.add_btn(204, 232, 184, 24, rule_lbl, ACT_CYCLE_MATCH_RULE,
                   app.match_rules.rule_mode != twc98::copa::MatchRuleMode::Default);
        ui.add_disabled_row(12, 262, 376, 22, "Difficulty AI Patch",
                            app.match_rules.hard_mode ? "Level 31 Locked" : "Progressive 0..31");
        ui.add_disabled_row(12, 288, 376, 22, "Tournament Format",
                            (app.game_mode == twc98::copa::GameMode::CopaAmerica) ? "Copa 12 Teams" : "Classic 16 Teams");
        ui.add_disabled_row(12, 314, 376, 22, "Online Matchplay", "Use Web Host P2P");

        ui.add_btn(12, 344, 144, 28, "Exit Game", ACT_EXIT_APP, true, 0xF87F1D1Du, 0xFFEF4444u);
        ui.draw_text(164, 344, 224, 28, "Window stays paused while open", ui.font_ui, 0x94A3B8u, DT_RIGHT);
    } else if (app.menu_tab == 1) {
        // TAB 1: VIDEO
        draw_sec(64, "DISPLAY & PICTURE");
        ui.add_btn(12, 86, 184, 26,
                   app.fullscreen ? "Fullscreen: ON [F11]" : "Windowed Mode [F11]",
                   ACT_FULLSCREEN, app.fullscreen);
        ui.add_btn(204, 86, 184, 26,
                   (app.use_gpu && app.gpu.ready) ? "Renderer: GPU D3D11" : "Renderer: CPU GDI",
                   ACT_TOGGLE_GPU, app.use_gpu && app.gpu.ready);

        ui.draw_text(12, 118, 58, 24, "Aspect", ui.font_ui, 0x94A3B8u, DT_LEFT);
        for (int a = 0; a < 4; ++a) {
            ui.add_btn(74 + a * 79, 118, 75, 24, kAspectNames[a],
                       static_cast<DbgActionId>(ACT_ASPECT_0 + a), app.aspect_mode == a);
        }

        ui.draw_text(12, 148, 58, 24, "Filter", ui.font_ui, 0x94A3B8u, DT_LEFT);
        static constexpr const char* kFltLbl[4] = {"NEAR", "BILIN", "SHARP", "CRT-A"};
        for (int f = 0; f < 4; ++f) {
            ui.add_btn(74 + f * 79, 148, 75, 24, kFltLbl[f],
                       static_cast<DbgActionId>(ACT_FILTER_0 + f), app.filter_mode == f);
        }

        ui.draw_text(12, 178, 58, 24, "CRT", ui.font_ui, 0x94A3B8u, DT_LEFT);
        static constexpr const char* kCrtLbl[3] = {"OFF", "SOFT SCAN", "ARCADE"};
        for (int c = 0; c < 3; ++c) {
            ui.add_btn(74 + c * 106, 178, 102, 24, kCrtLbl[c],
                       static_cast<DbgActionId>(ACT_CRT_0 + c), app.crt_level == c);
        }

        const bool hi_rbg = (app.m->dbg_hide_layers & 0x08u) != 0u;
        ui.draw_text(12, 208, 58, 24, "Pitch", ui.font_ui, 0x94A3B8u, DT_LEFT);
        ui.add_btn(74, 208, 314, 24,
                   hi_rbg ? "Field Pitch: HD 704x448 (Switch to 352)" : "Field Pitch: Arcade 352x224 (Switch to HD)",
                   ACT_PITCH, hi_rbg);

        draw_sec(244, "UNAVAILABLE FEATURES");
        ui.add_disabled_row(12, 266, 376, 22, "Frame Rate / V-Sync", "Fixed 60 Hz (Soon)");
        ui.add_disabled_row(12, 292, 376, 22, "Custom Shader Preset", "Built-in Only (Soon)");
    } else if (app.menu_tab == 2) {
        // TAB 2: AUDIO
        draw_sec(64, "SOUND MIXER");
        ui.add_btn(12, 88, 376, 28,
                   app.audio_muted ? "Sound Output: MUTED  [M to Unmute]" : "Sound Output: ACTIVE  [M to Mute]",
                   ACT_AUDIO_MUTE, !app.audio_muted, 0xFA065F46u, 0xFF10B981u);

        ui.draw_text(12, 124, 78, 26, "Master Vol", ui.font_ui, 0xCBD5E1u, DT_LEFT);
        ui.add_btn(96, 124, 48, 26, "-10%", ACT_VOL_DN, false, 0, 0, true);
        ui.fill_rect(150, 124, 180, 26, 0xF40B1220u);
        ui.stroke_rect(150, 124, 180, 26, 0xFF2D3E55u);
        const int eff_vol = app.audio_muted ? 0 : app.audio_volume;
        const int bar_w = (174 * eff_vol) / 100;
        if (bar_w > 0) ui.fill_rect(153, 127, bar_w, 20, app.audio_muted ? 0xFF475569u : 0xFF059669u);
        char vbuf[24]{};
        if (app.audio_muted) std::snprintf(vbuf, sizeof(vbuf), "MUTED (%d%%)", app.audio_volume);
        else std::snprintf(vbuf, sizeof(vbuf), "%d%%", app.audio_volume);
        ui.draw_text(150, 124, 180, 26, vbuf, ui.font_bold, 0xF8FAFCu, DT_CENTER);
        ui.add_btn(336, 124, 52, 26, "+10%", ACT_VOL_UP, false, 0, 0, true);

        ui.fill_rect(12, 158, 376, 24, 0xEC0D1522u);
        ui.stroke_rect(12, 158, 376, 24, 0xFF243042u);
        ui.draw_text(20, 158, 360, 24,
                     app.m->m68k_enabled ? "Engine: Real M68K + SCSP DSP (44.1 kHz Stereo)"
                                         : "Engine: Mailbox Synth (Pre-Boot Title)",
                     ui.font_ui, 0x94A3B8u, DT_LEFT);

        draw_sec(194, "UNAVAILABLE FEATURES");
        ui.add_disabled_row(12, 216, 376, 22, "Music / BGM Volume", "Shared SCSP Bus (N/A)");
        ui.add_disabled_row(12, 242, 376, 22, "Sound Effects (SFX)", "Shared SCSP Bus (N/A)");
        ui.add_disabled_row(12, 268, 376, 22, "Audio Output Device", "Default System (Soon)");
    } else {
        // TAB 3: CONTROLS (Keyboard Remap + Xbox Controller)
        ui.add_btn(12, 66, 184, 26, "1P Keyboard Remap", ACT_MENU_CTRL_0, app.ctrl_subtab == 0);
        ui.add_btn(204, 66, 184, 26, "Xbox / XInput Pad", ACT_MENU_CTRL_1, app.ctrl_subtab == 1);

        if (app.ctrl_subtab == 0) {
            if (app.rebinding_slot >= 0) {
                ui.draw_text(12, 97, 376, 18, "PRESS ANY NEW KEY TO ASSIGN  (Esc = Cancel)",
                             ui.font_bold, 0xFBBF24u, DT_LEFT);
            } else {
                ui.draw_text(12, 97, 376, 18, "Click any key button to remap  (Arrows move too)",
                             ui.font_ui, 0x94A3B8u, DT_LEFT);
            }

            static constexpr const char* kLeftLbl[5] = {"Move Up", "Move Down", "Move Left", "Move Right", "Start 1P"};
            static constexpr int kLeftIdx[5] = {0, 1, 2, 3, 8};
            static constexpr const char* kRightLbl[5] = {"Pass (B1)", "Shoot (B2)", "Clear (B3)", "Extra (B4)", "Coin 1P"};
            static constexpr int kRightIdx[5] = {4, 5, 6, 7, 9};

            for (int r = 0; r < 5; ++r) {
                const int ry = 120 + r * 28;
                const int li = kLeftIdx[r];
                const int ri = kRightIdx[r];
                char lkey[24]{}, rkey[24]{};
                if (app.rebinding_slot == li) std::snprintf(lkey, sizeof(lkey), "> KEY? <");
                else vk_to_short_name(app.p1_key_map[li], lkey, sizeof(lkey));
                if (app.rebinding_slot == ri) std::snprintf(rkey, sizeof(rkey), "> KEY? <");
                else vk_to_short_name(app.p1_key_map[ri], rkey, sizeof(rkey));

                ui.draw_text(12, ry, 74, 24, kLeftLbl[r], ui.font_ui, 0xCBD5E1u, DT_LEFT);
                ui.add_btn(88, ry, 104, 24, lkey, static_cast<DbgActionId>(ACT_KEY_BIND_0 + li),
                           app.rebinding_slot == li, 0xFA78350Fu, 0xFFF59E0Bu, true);

                ui.draw_text(204, ry, 80, 24, kRightLbl[r], ui.font_ui, 0xCBD5E1u, DT_LEFT);
                ui.add_btn(288, ry, 100, 24, rkey, static_cast<DbgActionId>(ACT_KEY_BIND_0 + ri),
                           app.rebinding_slot == ri, 0xFA78350Fu, 0xFFF59E0Bu, true);
            }

            ui.add_btn(12, 268, 160, 26, "Reset Defaults", ACT_KEY_RESET, false);
            ui.draw_text(178, 268, 210, 26, "Def: WASD + FGHT + Spc/R", ui.font_mono, 0x64748Bu, DT_RIGHT);

            draw_sec(302, "UNAVAILABLE FEATURES");
            ui.add_disabled_row(12, 324, 376, 22, "2P Keyboard Remap", "Fixed IJKL/UONM (Soon)");
            ui.add_disabled_row(12, 350, 376, 22, "Save Profile to Disk", "Session Only (Soon)");
        } else {
            draw_sec(102, "XBOX / XINPUT STATUS & TUNING");
            ui.fill_rect(12, 124, 184, 24, app.xinput_active[0] ? 0xFF14532Du : 0xF21A2230u);
            ui.stroke_rect(12, 124, 184, 24, app.xinput_active[0] ? 0xFF22C55Eu : 0xFF334155u);
            ui.draw_text(12, 124, 184, 24,
                         app.xinput_active[0] ? "P1 Pad: CONNECTED" : "P1 Pad: Not Detected",
                         ui.font_bold, app.xinput_active[0] ? 0x86EFACu : 0x94A3B8u, DT_CENTER);

            ui.fill_rect(204, 124, 184, 24, app.xinput_active[1] ? 0xFF14532Du : 0xF21A2230u);
            ui.stroke_rect(204, 124, 184, 24, app.xinput_active[1] ? 0xFF22C55Eu : 0xFF334155u);
            ui.draw_text(204, 124, 184, 24,
                         app.xinput_active[1] ? "P2 Pad: CONNECTED" : "P2 Pad: Not Detected",
                         ui.font_bold, app.xinput_active[1] ? 0x86EFACu : 0x94A3B8u, DT_CENTER);

            ui.add_btn(12, 156, 376, 26,
                       app.xbox_pro_layout ? "Layout Preset: Xbox Football Pro (Switch to Direct)"
                                           : "Layout Preset: Direct 1:1 Mapping (Switch to Pro)",
                       ACT_XBOX_LAYOUT, app.xbox_pro_layout);

            ui.draw_text(12, 190, 100, 24, "Stick Deadzone", ui.font_ui, 0xCBD5E1u, DT_LEFT);
            ui.add_btn(116, 190, 48, 24, "-5%", ACT_PAD_DZ_DN, false, 0, 0, true);
            ui.fill_rect(170, 190, 156, 24, 0xF40B1220u);
            ui.stroke_rect(170, 190, 156, 24, 0xFF2D3E55u);
            const int dz_fill = ((app.pad_deadzone_pct - 10) * 150) / 35;
            if (dz_fill > 0) ui.fill_rect(173, 193, std::min(150, dz_fill), 18, 0xFF0284C7u);
            char dzbuf[24]{};
            std::snprintf(dzbuf, sizeof(dzbuf), "%d%%", app.pad_deadzone_pct);
            ui.draw_text(170, 190, 156, 24, dzbuf, ui.font_bold, 0xF8FAFCu, DT_CENTER);
            ui.add_btn(332, 190, 56, 24, "+5%", ACT_PAD_DZ_UP, false, 0, 0, true);

            ui.fill_rect(12, 222, 376, 40, 0xEC0D1522u);
            ui.stroke_rect(12, 222, 376, 40, 0xFF243042u);
            ui.draw_text(20, 224, 360, 18,
                         app.xbox_pro_layout ? "A/LT:Pass   X/RT:Shoot   B:Clear   Y/LB/RB:Extra"
                                             : "A:Btn1(Pass)  B:Btn2(Shoot)  X:Btn3  Y:Btn4",
                         ui.font_mono, 0xCBD5E1u, DT_LEFT);
            ui.draw_text(20, 242, 360, 18, "L-Stick (8-way) / D-Pad:Move   Start:1P   Back:Coin",
                         ui.font_mono, 0x94A3B8u, DT_LEFT);

            draw_sec(272, "UNAVAILABLE FEATURES");
            ui.add_disabled_row(12, 294, 376, 22, "Controller Vibration", "Not Supported (Soon)");
            ui.add_disabled_row(12, 320, 376, 22, "Custom Pad Remapping", "Use Preset Above (Soon)");
        }
    }

    // Footer bar (y=ph-24..ph)
    ui.fill_rect(1, ph - 24, pw - 2, 23, 0xF5141D2Bu);
    ui.fill_rect(1, ph - 25, pw - 2, 1, 0xFF2A3A50u);
    ui.draw_text(10, ph - 23, 150, 22, app.status_banner.c_str(), ui.font_mono, 0x34D399u, DT_LEFT);
    ui.draw_text(162, ph - 23, pw - 172, 22, "Esc:Close  1:DevPanel  PgUp/Dn", ui.font_ui, 0x64748Bu, DT_RIGHT);
    ui.finalize_alpha();
}

void show_fatal_error(bool headless, const std::string& msg) {
    std::fprintf(stderr, "ERROR: %s\n", msg.c_str());
    if (!headless) {
        HMODULE u32 = LoadLibraryA("user32.dll");
        if (u32) {
            using FnMsgBox = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
            auto mb = reinterpret_cast<FnMsgBox>(reinterpret_cast<void*>(GetProcAddress(u32, "MessageBoxA")));
            if (mb) mb(nullptr, msg.c_str(), "TWC98BO PC - Error de inicio", MB_OK | MB_ICONERROR);
        }
    }
}

std::filesystem::path find_default_data_dir(const char* argv0) {
    std::error_code ec;
    char mod_path[MAX_PATH * 4]{};
    const DWORD mlen = GetModuleFileNameA(nullptr, mod_path, sizeof(mod_path));
    std::filesystem::path exe_path = (mlen > 0u && mlen < sizeof(mod_path))
        ? std::filesystem::path(mod_path)
        : std::filesystem::path(argv0 ? argv0 : "");
    std::filesystem::path exe_dir = std::filesystem::weakly_canonical(exe_path, ec).parent_path();
    std::filesystem::path cwd = std::filesystem::current_path(ec);

    std::vector<std::filesystem::path> candidates;
    auto push_roots = [&](std::filesystem::path base) {
        for (int depth = 0; depth <= 4 && !base.empty(); ++depth) {
            candidates.push_back(base / "data");
            candidates.push_back(base / ".local" / "agent-work" / "T147" / "G4-01" / "data");
            const auto parent = base.parent_path();
            if (parent == base) break;
            base = parent;
        }
    };
    push_roots(exe_dir);
    push_roots(cwd);

    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "twcup98_cart_20m.bin", ec)) {
            const auto canon = std::filesystem::weakly_canonical(c, ec);
            return ec ? c : canon;
        }
    }
    return std::filesystem::path("port/data");
}

void compute_present_rect(int cw, int ch, int fw, int fh, int mode,
                          int& dx, int& dy, int& dw, int& dh) {
    if (mode == 3 || fw <= 0 || fh <= 0) {
        dx = 0; dy = 0; dw = cw; dh = ch;
        return;
    }
    if (mode == 0) {
        // 2X / Integer-exact multiple centered inside client area
        const int scale = std::max(1, std::min(cw / fw, ch / fh));
        dw = fw * scale;
        dh = fh * scale;
        dx = (cw - dw) / 2;
        dy = (ch - dh) / 2;
        return;
    }
    // mode == 1 -> Fit 704:448; mode == 2 -> 4:3 Arcade CRT
    const double target_ar = (mode == 2) ? (4.0 / 3.0) : (static_cast<double>(fw) / static_cast<double>(fh));
    const double win_ar = static_cast<double>(cw) / static_cast<double>(ch);
    if (win_ar >= target_ar) {
        dh = ch;
        dw = std::max(1, static_cast<int>(static_cast<double>(ch) * target_ar + 0.5));
    } else {
        dw = cw;
        dh = std::max(1, static_cast<int>(static_cast<double>(cw) / target_ar + 0.5));
    }
    dx = (cw - dw) / 2;
    dy = (ch - dh) / 2;
}

void present_to_hdc(HDC hdc, int cw, int ch, HostAppState& app) {
    if (!hdc || !g_ui.ready || !app.vdp || app.vdp->frame_w <= 0 || app.vdp->frame_h <= 0) return;
    const int fw = app.vdp->frame_w;
    const int fh = app.vdp->frame_h;
    const uint32_t* raw_src = app.vdp->frame_px;

    int src_w = fw;
    int src_h = fh;
    const uint32_t* src = raw_src;
    if (app.crt_level > 0) {
        src_h = fh * 2;
        const size_t cnt = static_cast<size_t>(src_w * src_h);
        if (app.scan_px.size() != cnt) app.scan_px.resize(cnt);
        for (int y = 0; y < fh; ++y) {
            const uint32_t* row = raw_src + static_cast<size_t>(y * fw);
            uint32_t* even_dst = app.scan_px.data() + static_cast<size_t>((y * 2) * fw);
            uint32_t* odd_dst = even_dst + fw;
            std::memcpy(even_dst, row, static_cast<size_t>(fw) * sizeof(uint32_t));
            for (int x = 0; x < fw; ++x) {
                const uint32_t c = row[x];
                odd_dst[x] = (app.crt_level == 1)
                    ? (((c >> 1) & 0x7F7F7Fu) + ((c >> 2) & 0x3F3F3Fu))
                    : ((c >> 1) & 0x7F7F7Fu);
            }
        }
        src = app.scan_px.data();
    }

    int dx = 0, dy = 0, dw = cw, dh = ch;
    compute_present_rect(cw, ch, fw, fh, app.aspect_mode, dx, dy, dw, dh);

    if (app.need_bg_clear) {
        HBRUSH blk = reinterpret_cast<HBRUSH>(g_ui.get_stock_object(BLACK_BRUSH));
        if (blk) {
            if (dy > 0) {
                RECT top{0, 0, cw, dy};
                RECT bot{0, dy + dh, cw, ch};
                g_ui.fill_rect(hdc, &top, blk);
                g_ui.fill_rect(hdc, &bot, blk);
            }
            if (dx > 0) {
                RECT left{0, dy, dx, dy + dh};
                RECT right{dx + dw, dy, cw, dy + dh};
                g_ui.fill_rect(hdc, &left, blk);
                g_ui.fill_rect(hdc, &right, blk);
            }
        }
        app.need_bg_clear = false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = src_w;
    bmi.bmiHeader.biHeight = -src_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const bool smooth_gdi = (app.filter_mode != 0);
    g_ui.set_stretch_blt_mode(hdc, smooth_gdi ? HALFTONE : COLORONCOLOR);
    if (smooth_gdi) g_ui.set_brush_org_ex(hdc, dx, dy, nullptr);
    g_ui.stretch_di_bits(hdc, dx, dy, dw, dh,
                         0, 0, src_w, src_h,
                         src, &bmi, DIB_RGB_COLORS, SRCCOPY);

    if (app.dbg_ui.mem_dc) {
        g_ui.bit_blt(hdc, app.dbg_ui.draw_x, app.dbg_ui.draw_y,
                     app.dbg_ui.w, app.dbg_ui.h,
                     app.dbg_ui.mem_dc, 0, 0, SRCCOPY);
    }
}

void present_frame(HWND hwnd, HDC hdc, int cw, int ch, HostAppState& app) {
    if (!app.vdp || app.vdp->frame_w <= 0 || app.vdp->frame_h <= 0) return;
    if (app.mode_select_open) compose_mode_select(app, cw, ch);
    else if (app.debug_open) compose_debug_panel(app, cw, ch);
    else if (app.menu_open) compose_player_menu(app, cw, ch);
    else compose_corner_trigger(app, cw, ch);
    if (app.use_gpu && app.gpu.ready) {
        int dx = 0, dy = 0, dw = cw, dh = ch;
        compute_present_rect(cw, ch, app.vdp->frame_w, app.vdp->frame_h, app.aspect_mode, dx, dy, dw, dh);
        const uint32_t* ov_px = app.dbg_ui.px;
        if (app.gpu.present(app.vdp->frame_px, app.vdp->frame_w, app.vdp->frame_h,
                            cw, ch, dx, dy, dw, dh,
                            app.filter_mode, app.crt_level,
                            ov_px, app.dbg_ui.draw_x, app.dbg_ui.draw_y,
                            app.dbg_ui.w, app.dbg_ui.h)) {
            app.need_bg_clear = false;
            return;
        }
        // Automatic safe fallback to CPU GDI if GPU present ever fails
        app.use_gpu = false;
        app.need_bg_clear = true;
        app.status_banner = "GPU FALLBACK:CPU";
    }
    if (hdc) {
        present_to_hdc(hdc, cw, ch, app);
    } else if (hwnd && g_ui.ready) {
        HDC tmp_hdc = g_ui.get_dc(hwnd);
        if (tmp_hdc) {
            present_to_hdc(tmp_hdc, cw, ch, app);
            g_ui.release_dc(hwnd, tmp_hdc);
        }
    }
}

void blit_hwnd(HWND hwnd, HostAppState& app) {
    if (!hwnd || !g_ui.ready || !app.vdp) return;
    RECT rc{};
    g_ui.get_client_rect(hwnd, &rc);
    const int cw = std::max(1, static_cast<int>(rc.right - rc.left));
    const int ch = std::max(1, static_cast<int>(rc.bottom - rc.top));
    present_frame(hwnd, nullptr, cw, ch, app);
}

void dump_debug_panels_ppm(HostAppState& app, const std::filesystem::path& out_dir) {
    if (!g_ui.load() || !app.vdp) return;
    std::filesystem::create_directories(out_dir);
    const int cw = 1408, ch = 896;
    const int fw = app.vdp->frame_w, fh = app.vdp->frame_h;
    std::vector<uint32_t> canvas(static_cast<size_t>(cw * ch), 0xFF000000u);
    const bool prev_dbg = app.debug_open;
    const int prev_dtab = app.debug_tab;
    const bool prev_menu = app.menu_open;
    const int prev_mtab = app.menu_tab;
    const int prev_csub = app.ctrl_subtab;

    auto blend_and_save = [&](const char* fname) {
        for (int y = 0; y < ch; ++y) {
            const int sy = std::min(fh - 1, (y * fh) / ch);
            for (int x = 0; x < cw; ++x) {
                const int sx = std::min(fw - 1, (x * fw) / cw);
                canvas[static_cast<size_t>(y * cw + x)] = app.vdp->frame_px[sy * fw + sx];
            }
        }
        if (app.dbg_ui.px) {
            for (int py = 0; py < app.dbg_ui.h; ++py) {
                const int dy = app.dbg_ui.draw_y + py;
                if (dy < 0 || dy >= ch) continue;
                for (int px = 0; px < app.dbg_ui.w; ++px) {
                    const int dx = app.dbg_ui.draw_x + px;
                    if (dx < 0 || dx >= cw) continue;
                    const uint32_t fg = app.dbg_ui.px[static_cast<size_t>(py * app.dbg_ui.w + px)];
                    const uint32_t a = (fg >> 24) & 0xFFu;
                    const uint32_t bg = canvas[static_cast<size_t>(dy * cw + dx)];
                    const uint32_t r = (((fg >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * (255u - a)) / 255u;
                    const uint32_t g = (((fg >> 8) & 0xFFu) * a + ((bg >> 8) & 0xFFu) * (255u - a)) / 255u;
                    const uint32_t b = ((fg & 0xFFu) * a + (bg & 0xFFu) * (255u - a)) / 255u;
                    canvas[static_cast<size_t>(dy * cw + dx)] = (r << 16) | (g << 8) | b;
                }
            }
        }
        std::ofstream ofs(out_dir / fname, std::ios::binary);
        ofs << "P6\n" << cw << " " << ch << "\n255\n";
        for (uint32_t c : canvas) {
            const char rgb[3] = {
                static_cast<char>((c >> 16) & 0xFFu),
                static_cast<char>((c >> 8) & 0xFFu),
                static_cast<char>(c & 0xFFu)
            };
            ofs.write(rgb, 3);
        }
    };

    app.debug_open = true;
    app.menu_open = false;
    for (int t = 0; t < 4; ++t) {
        app.debug_tab = t;
        compose_debug_panel(app, cw, ch);
        char fname[64]{};
        std::snprintf(fname, sizeof(fname), "dbg_tab%d.ppm", t);
        blend_and_save(fname);
    }

    app.debug_open = false;
    app.menu_open = false;
    compose_corner_trigger(app, cw, ch);
    blend_and_save("menu_corner.ppm");

    app.menu_open = true;
    for (int t = 0; t < 3; ++t) {
        app.menu_tab = t;
        compose_player_menu(app, cw, ch);
        char fname[64]{};
        std::snprintf(fname, sizeof(fname), "menu_tab%d.ppm", t);
        blend_and_save(fname);
    }
    app.menu_tab = 3;
    app.ctrl_subtab = 0;
    compose_player_menu(app, cw, ch);
    blend_and_save("menu_tab3_kb.ppm");

    app.ctrl_subtab = 1;
    compose_player_menu(app, cw, ch);
    blend_and_save("menu_tab3_pad.ppm");

    const bool prev_msel = app.mode_select_open;
    const int prev_mode_cursor = app.mode_select_cursor;
    const bool prev_restart = app.pending_full_restart;
    app.menu_open = false;
    app.mode_select_open = true;
    app.mode_select_cursor = 1;
    compose_mode_select(app, cw, ch);
    blend_and_save("mode_select.ppm");
    app.mode_select_cursor = 0;
    compose_mode_select(app, cw, ch);
    blend_and_save("mode_select_classic.ppm");
    app.pending_full_restart = true;
    compose_mode_select(app, cw, ch);
    blend_and_save("mode_select_restart.ppm");
    app.mode_select_open = prev_msel;
    app.mode_select_cursor = prev_mode_cursor;
    app.pending_full_restart = prev_restart;

    app.debug_open = prev_dbg;
    app.debug_tab = prev_dtab;
    app.menu_open = prev_menu;
    app.menu_tab = prev_mtab;
    app.ctrl_subtab = prev_csub;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path data_dir = find_default_data_dir(argc > 0 ? argv[0] : "twc98_pc.exe");
    int smoke_frames = 0;
    bool headless = false;
    bool enable_audio = true;
    int initial_debug = 0;
    bool initial_fullscreen = false;
    bool initial_gpu = true;
    int initial_filter = 3; // 0=NEAREST, 1=BILINEAR, 2=SHARP-BI, 3=CRT-ARC
    int initial_crt = 1;    // 0=OFF, 1=SOFT, 2=ARCADE
    bool initial_hires_rbg = false;
    bool hires_rbg_explicit = false;
    bool test_gpu = false;
    int cold_boot_test_frames = 0;
    bool cold_boot_trace = false;
    bool cli_mode_explicit = false;
    twc98::copa::GameMode cli_mode = twc98::copa::GameMode::CopaAmerica;
    bool force_mode_select = false;
    bool cli_quick_skip = true;
    bool verify_copa_mod = false;
    std::filesystem::path copa_verify_dir;
    std::filesystem::path cold_boot_ppm;
    int initial_aspect = 0; // 0=2X, 1=Fit, 2=4:3, 3=Fill
    std::filesystem::path smoke_out;
    std::filesystem::path smoke_ppm;
    std::filesystem::path dump_panels_dir;
    uint16_t net_host_port = 0u;
    std::string net_join_peer;
    uint8_t net_delay_frames = 3u;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--smoke-frames" && i + 1 < argc) smoke_frames = std::atoi(argv[++i]);
        else if (arg == "--smoke-out" && i + 1 < argc) smoke_out = argv[++i];
        else if (arg == "--smoke-ppm" && i + 1 < argc) smoke_ppm = argv[++i];
        else if (arg == "--dump-debug-panels" && i + 1 < argc) { dump_panels_dir = argv[++i]; headless = true; }
        else if (arg == "--headless") headless = true;
        else if (arg == "--no-audio") enable_audio = false;
        else if (arg == "--debug" && i + 1 < argc) initial_debug = std::atoi(argv[++i]);
        else if (arg == "--fullscreen") initial_fullscreen = true;
        else if (arg == "--cpu") initial_gpu = false;
        else if (arg == "--gpu") initial_gpu = true;
        else if (arg == "--smooth") initial_filter = 1;
        else if (arg == "--scanlines") initial_crt = 1;
        else if (arg == "--crt" && i + 1 < argc) initial_crt = std::clamp(std::atoi(argv[++i]), 0, 2);
        else if (arg == "--hires-rbg") { initial_hires_rbg = true; hires_rbg_explicit = true; }
        else if (arg == "--stv-pitch") { initial_hires_rbg = false; hires_rbg_explicit = true; }
        else if (arg == "--test-gpu") test_gpu = true;
        else if (arg == "--cold-boot-test" && i + 1 < argc) { cold_boot_test_frames = std::atoi(argv[++i]); headless = true; }
        else if (arg == "--cold-boot-trace") cold_boot_trace = true;
        else if (arg == "--cold-boot-dump-ppm" && i + 1 < argc) cold_boot_ppm = argv[++i];
        else if (arg == "--mode-select") { force_mode_select = true; cli_quick_skip = false; }
        else if (arg == "--no-quick-skip") { force_mode_select = true; cli_quick_skip = false; }
        else if (arg == "--mode" && i + 1 < argc) {
            const std::string mv = argv[++i];
            cli_mode_explicit = true;
            cli_mode = (mv == "classic" || mv == "orig" || mv == "wc98")
                ? twc98::copa::GameMode::Classic
                : twc98::copa::GameMode::CopaAmerica;
        }
        else if (arg == "--verify-copa-mod") { verify_copa_mod = true; headless = true; }
        else if (arg == "--copa-verify-dir" && i + 1 < argc) copa_verify_dir = argv[++i];
        else if (arg == "--net-host" && i + 1 < argc) net_host_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--net-join" && i + 1 < argc) net_join_peer = argv[++i];
        else if (arg == "--net-delay" && i + 1 < argc) net_delay_frames = static_cast<uint8_t>(std::clamp(std::atoi(argv[++i]), 1, 6));
        else if (arg == "--filter" && i + 1 < argc) {
            const std::string fm = argv[++i];
            if (fm == "nearest") initial_filter = 0;
            else if (fm == "bilinear" || fm == "smooth") initial_filter = 1;
            else if (fm == "sharp" || fm == "sharp-bi") initial_filter = 2;
            else initial_filter = 3;
        }
        else if (arg == "--aspect" && i + 1 < argc) {
            const std::string m = argv[++i];
            if (m == "fit") initial_aspect = 1;
            else if (m == "43" || m == "4:3") initial_aspect = 2;
            else if (m == "fill" || m == "stretch") initial_aspect = 3;
            else initial_aspect = 0;
        }
    }

    if (!headless && smoke_frames == 0 && !hires_rbg_explicit) {
        // Default interactive visual preset: GPU-D3D11 + CRT-ARC + SOFT + RBG0 HD-704.
        initial_hires_rbg = true;
    }

    if (test_gpu) {
        twc98_pc_gpu::GpuD3D11Presenter probe;
        std::string report;
        const bool ok = probe.test_headless_init(report);
        std::printf("%s\n", report.c_str());
        return ok ? 0 : 5;
    }

    if (cold_boot_test_frames > 0) {
        std::vector<uint8_t> cart, bios;
        if (!read_bytes(data_dir / "twcup98_cart_20m.bin", cart) ||
            !read_bytes(data_dir / "bios_euro_be.bin", bios)) {
            std::printf("COLD_BOOT_ERR=missing_files\n");
            return 1;
        }
        auto m = std::make_unique<twc98::StvMachineState>();
        const char* cb_err = nullptr;
        if (!m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err)) {
            std::printf("COLD_BOOT_ERR=%s\n", cb_err ? cb_err : "init_fail");
            return 2;
        }
        m->set_input_ports(0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
        bool ok = true;
        uint32_t last_region = 0xFFFFFFFFu;
        int logged_transitions = 0;
        for (int f = 0; f < cold_boot_test_frames; ++f) {
            for (int sub = 0; sub < 50; ++sub) {
                if (!m->run_rounds(200u)) { ok = false; break; }
                if (cold_boot_trace) {
                    const uint32_t pc = m->cpus[0].pc;
                    const uint32_t reg = (pc >= 0x06000000u) ? (pc & 0xFFFF0000u)
                                       : (pc >= 0x00000d00u && pc <= 0x00000d22u) ? 0x00000d00u
                                       : (pc & 0xFFFFFF00u);
                    if (reg != last_region && logged_transitions < 80) {
                        std::printf("CB_TRANS f=%d.%02d PC=0x%08x PR=0x%08x R0=0x%08x R4=0x%08x\n",
                                    f, sub, pc, m->cpus[0].pr, m->cpus[0].r[0], m->cpus[0].r[4]);
                        last_region = reg;
                        ++logged_transitions;
                    }
                }
            }
            if (!ok) break;
            if (f == 0 || (f % 300) == 0 || f == cold_boot_test_frames - 1) {
                std::printf("CB f=%d ok=%d M_PC=0x%08x S_PC=0x%08x S_EN=%d vbi=%u sched2=%u prot=%d lw=0x%08x@0x%08x\n",
                            f, ok ? 1 : 0, m->cpus[0].pc, m->cpus[1].pc, m->slave_enabled ? 1 : 0,
                            m->vbi_count, m->sched2_hits, m->cart_prot_enabled ? 1 : 0,
                            m->last_write_val, m->last_write_ea);
            }
        }
        if (ok && !cold_boot_ppm.empty()) {
            m->render_frame();
            if (!cold_boot_ppm.parent_path().empty()) {
                std::filesystem::create_directories(cold_boot_ppm.parent_path());
            }
            write_ppm(cold_boot_ppm, m->vdp);
            std::printf("CB_PPM=%s w=%d h=%d\n", cold_boot_ppm.string().c_str(), m->vdp.frame_w, m->vdp.frame_h);
        }
        if (!ok) {
            std::printf("COLD_BOOT_FAULT cpu=%d reason=%s pc=0x%08x addr=0x%08x w=%u wr=%d\n",
                        m->halt_cpu, m->last_fault.reason ? m->last_fault.reason : "?",
                        m->last_fault.pc, m->last_fault.address, m->last_fault.width,
                        m->last_fault.is_write ? 1 : 0);
        }
        return ok ? 0 : 3;
    }

    const auto t_boot0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> cart, bios, ep_cp;
    std::vector<uint8_t> v1_vram, v1_regs, v2_vram, v2_cram, v2_regs;
    if (!read_bytes(data_dir / "twcup98_cart_20m.bin", cart) ||
        !read_bytes(data_dir / "bios_euro_be.bin", bios) ||
        !read_bytes(data_dir / "endpoint_frame8175.cp", ep_cp)) {
        show_fatal_error(headless, std::string("Faltan binarios core en: ") + data_dir.string() +
                                       "\nEjecuta desde la raiz o pasa --data-dir <ruta>");
        return 1;
    }

    HostAppState app{};
    app.m = std::make_unique<twc98::StvMachineState>();
    app.vdp = &app.m->vdp;
    app.debug_open = (initial_debug > 0);
    app.aspect_mode = initial_aspect;
    app.use_gpu = initial_gpu;
    app.filter_mode = initial_filter;
    app.crt_level = initial_crt;
    app.validation_dir = data_dir.parent_path() / "validation_g4";

    const char* err = nullptr;
    if (!app.m->init_from_cpn8_bytes(ep_cp.data(), ep_cp.size(), bios.data(), bios.size(), &err) ||
        !app.m->load_cart_rom(cart.data(), cart.size())) {
        show_fatal_error(headless, std::string("Machine init failed: ") + (err ? err : "unknown"));
        return 2;
    }
    if (initial_hires_rbg || !dump_panels_dir.empty()) app.m->dbg_hide_layers |= 0x08u;
    if (!headless || !dump_panels_dir.empty()) {
        app.m->set_gk_ai_fix(true);
        if (enable_audio && !headless) app.m->set_real_audio(true);
    }
    if (read_bytes(data_dir / "control-window-start-vdp1-vram.bin", v1_vram) &&
        read_bytes(data_dir / "control-window-start-vdp1-registers.bin", v1_regs) &&
        read_bytes(data_dir / "control-window-start-vdp2-vram.bin", v2_vram) &&
        read_bytes(data_dir / "control-window-start-vdp2-cram.bin", v2_cram) &&
        read_bytes(data_dir / "control-window-start-vdp2-registers.bin", v2_regs)) {
        app.m->seed_vdp_snapshot(v1_vram.data(), v1_vram.size(),
                                 v1_regs.data(), v1_regs.size(),
                                 v2_vram.data(), v2_vram.size(),
                                 v2_cram.data(), v2_cram.size(),
                                 v2_regs.data(), v2_regs.size());
    }
    app.m->render_frame();
    // Apply Mode Select / Copa America / Dev Quick-Skip policy:
    // - Regression smoke (--smoke-frames) defaults to Classic unless --mode copa passed.
    // - Interactive launch defaults to Dev Quick-Skip ON -> straight into Copa America.
    app.dev_quick_skip = cli_quick_skip;
    const twc98::copa::GameMode start_mode = cli_mode_explicit
        ? cli_mode
        : ((smoke_frames > 0) ? twc98::copa::GameMode::Classic : twc98::copa::GameMode::CopaAmerica);
    if (!verify_copa_mod && smoke_frames == 0 && dump_panels_dir.empty() &&
        start_mode == twc98::copa::GameMode::CopaAmerica) {
        const char* cb_err = nullptr;
        if (!boot_mode_clean(app, bios, cart, twc98::copa::GameMode::CopaAmerica, 820, &cb_err)) {
            show_fatal_error(headless, std::string("Copa boot failed: ") + (cb_err ? cb_err : "unknown"));
            return 2;
        }
    } else {
        set_host_game_mode(app, start_mode);
    }
    app.mode_select_open = (!headless && (force_mode_select || !app.dev_quick_skip));

    if (verify_copa_mod) {
        using Off = twc98::copa::TeamAssetOffsets;
        std::vector<uint8_t> orig_cart_slice(cart.begin(), cart.end());
        set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x13572468u);
        const auto draw_a = app.copa_backup->tournament;
        set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x24681357u);
        const auto draw_b = app.copa_backup->tournament;
        const bool random_groups_ok =
            (twc98::copa::generate_copa_tournament_seed() != 0u) &&
            draw_a.initialized && draw_b.initialized &&
            (draw_a.groups != draw_b.groups);

        const bool bol_sp_ok =
            app.m->cart_rom[Off::kSpTblSlot14CartOff + 0u] == 3u &&
            app.m->cart_rom[Off::kSpTblSlot14CartOff + 1u] == 22u &&
            app.m->cart_rom[Off::kSpTblSlot14CartOff + 2u] == 0u;
        bool bol_skin_ok = true;
        for (uint32_t p = 0u; p < 11u; ++p) {
            if (app.m->cart_rom[Off::kCapaTblSlice0Off + 14u * Off::kCapaTeamBytes + p * 48u + 1u] != 1u) {
                bol_skin_ok = false;
            }
        }
        const bool copa_gk_ok =
            app.m->cart_rom[Off::kCapaTblSlice0Off + 14u * Off::kCapaTeamBytes + 2u] == 3u &&
            app.m->cart_rom[Off::kCapaGkSlot14Off + 0u] == 15u &&
            app.m->cart_rom[Off::kCapaGkSlot14Off + 3u] == 15u;
        const bool euro_remap_ok =
            app.m->cart_rom[Off::kEuroRemapCartOff + 0u] == 0x00u &&
            app.m->cart_rom[Off::kEuroRemapCartOff + 1u] == 0x0Bu;

        auto cart_u16 = [&](uint32_t off) -> uint16_t {
            return static_cast<uint16_t>((app.m->cart_rom[off] << 8) | app.m->cart_rom[off + 1u]);
        };
        auto cart_u32 = [&](uint32_t off) -> uint32_t {
            return (static_cast<uint32_t>(cart_u16(off)) << 16) | cart_u16(off + 2u);
        };
        const bool sp3_darken_ok =
            (cart_u16(0x0024EE28u) != 0x0009u) &&
            (cart_u16(Off::kSp3ArgCartOff) == 0xE200u) &&
            (cart_u32(Off::kSp3EmitLitCartOff) == Off::kSp3CleanWaitFnAddr) &&
            (cart_u16(Off::kSp3VfxCallCart[0]) == 0x0009u) &&
            (cart_u16(Off::kSp3VfxCallCart[1]) == 0x0009u);

        const uint16_t bol_away_col0 = cart_u16(Off::kTexHm14Off + Off::kTexPalOff);
        const bool bol_away_white = ((bol_away_col0 & 0x1Fu) >= 28u) &&
                                    (((bol_away_col0 >> 5) & 0x1Fu) >= 28u) &&
                                    (((bol_away_col0 >> 10) & 0x1Fu) >= 28u);
        auto clash_of = [&](uint32_t t0, uint32_t t1) -> uint16_t {
            return cart_u16(Off::kKitClashCartOff + (t0 * 19u + t1) * 2u);
        };
        const bool kit_clash_ok = bol_away_white &&
            ((clash_of(0u, 6u) & 0xC000u) == 0x4000u) && ((clash_of(0u, 6u) & 0x3FFFu) == 0x0245u) &&
            ((clash_of(6u, 0u) & 0xC000u) == 0x8000u) && ((clash_of(6u, 0u) & 0x3FFFu) != 0u) &&
            ((clash_of(10u, 14u) & 0xC000u) == 0x4000u) && ((clash_of(14u, 10u) & 0xC000u) == 0x8000u) &&
            ((clash_of(3u, 11u) & 0xC000u) == 0x8000u) && ((clash_of(11u, 3u) & 0xC000u) == 0x4000u) &&
            ((clash_of(5u, 7u) & 0xC000u) == 0x4000u) && ((clash_of(1u, 13u) & 0xC000u) == 0x4000u);

        const bool ven_flag_ok =
            (cart_u16(Off::kVenSp19Band0CartOff) == 0x00DCu) &&
            (std::memcmp(app.m->cart_rom + Off::kTexGmVenFlagOff,
                         orig_cart_slice.data() + Off::kTexGmVenFlagOff,
                         Off::kTexGmVenFlagBytes) != 0);

        bool hidden_ok = true;
        for (uint8_t h : {2u, 4u, 8u, 9u}) {
            if (app.m->cart_rom[Off::kTierTblCartOff + h] != 0xFFu) hidden_ok = false;
        }
        const bool ven_active_ok = (app.m->cart_rom[Off::kTierTblCartOff + 7u] == 0u);
        bool added_ok = ven_active_ok;
        for (uint8_t s : {1u, 5u, 6u, 7u, 11u, 13u, 14u}) {
            const uint32_t tf_off = Off::kTFlagBaseOff + s * Off::kTFlagStride;
            if (std::memcmp(app.m->cart_rom + tf_off, orig_cart_slice.data() + tf_off, Off::kTFlagSize) == 0) {
                added_ok = false;
            }
        }
        const uint32_t bgtm_offs[7] = {
            Off::kBgTm01Off, Off::kBgTm05Off, Off::kBgTm06Off, Off::kBgTm07Off,
            Off::kBgTm11Off, Off::kBgTm13Off, Off::kBgTm14Off
        };
        // Decode the actual 4bpp HUD tile bank independently of the builder:
        // every replacement must have a closed border and visible three-letter code.
        bool hud_assets_ok = true;
        for (uint32_t i = 0u; i < 7u; ++i) {
            const uint8_t* bg = app.m->cart_rom + bgtm_offs[i];
            if (std::memcmp(bg + 0x2Cu,
                            orig_cart_slice.data() + bgtm_offs[i] + 0x2Cu,
                            48u * 32u) == 0) {
                added_ok = false;
            }
            auto pixel = [&](int x, int y) -> uint8_t {
                const int tile = 1 + (y / 8) * 12 + x / 8;
                const uint8_t packed = bg[12 + tile * 32 + (y % 8) * 4 + (x % 8) / 2];
                return (x & 1) ? (packed & 15u) : (packed >> 4);
            };
            for (int x = 0; x < 96; ++x) {
                hud_assets_ok &= pixel(x, 0) == 14u && pixel(x, 31) == 14u;
            }
            for (int y = 0; y < 32; ++y) {
                hud_assets_ok &= pixel(0, y) == 14u && pixel(95, y) == 14u;
            }
            for (int letter = 0; letter < 3; ++letter) {
                int white_pixels = 0;
                for (int y = 2; y < 30; ++y) {
                    for (int x = 12 + letter * 26; x < 32 + letter * 26; ++x) {
                        white_pixels += pixel(x, y) == 15u ? 1 : 0;
                    }
                }
                hud_assets_ok &= white_pixels >= 100;
            }
        }
        bool roster_ok = true;
        uint32_t active_count = 0u;
        uint32_t drawn_mask = 0u;
        for (const auto& spec : twc98::copa::copa_team_specs()) {
            active_count += spec.active_in_copa ? 1u : 0u;
        }
        for (const auto& group : draw_b.groups) {
            for (uint8_t slot : group) {
                if (slot >= Off::kTeamCount || (drawn_mask & (1u << slot)) != 0u) {
                    roster_ok = false;
                    continue;
                }
                drawn_mask |= 1u << slot;
                roster_ok &= twc98::copa::copa_team_specs()[slot].active_in_copa;
            }
        }
        roster_ok &= active_count == 12u;
        const uint8_t* ven_map = app.m->cart_rom + Off::kMsgOutBaseOff + 7u * Off::kMsgOutMapBytes;
        const uint8_t* ecu_map = app.m->cart_rom + Off::kMsgOutBaseOff + 6u * Off::kMsgOutMapBytes;
        for (uint32_t cell = 0u; cell < 48u; ++cell) {
            // Palette 13+ is outside the flag bank loaded by Team Select.
            roster_ok &= (ven_map[cell * 2u] >> 4) == 7u &&
                         (ecu_map[cell * 2u] >> 4) == 7u;
        }

        // Cold boot into Copa America Title -> Coin -> Start -> 3x4 Group Team Select -> VEN -> BOL Match -> SP 3 Wind VFX
        const auto vdir = copa_verify_dir.empty()
            ? std::filesystem::path("port/copa_verify")
            : copa_verify_dir;
        std::filesystem::create_directories(vdir);

        const char* cb_err = nullptr;
        bool copa_run_ok = app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err);
        if (copa_run_ok) {
            app.copa_backup.reset();
            set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x54574339u);
        }

        auto step_frames = [&](int n, uint8_t pa, uint8_t pb, uint8_t pc, bool render = true) -> bool {
            for (int f = 0; f < n; ++f) {
                app.m->set_input_ports(pa, pb, pc, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                if (!app.m->run_rounds(10000u)) return false;
                twc98::copa::on_frame_tick(*app.m, *app.copa_backup);
                if (render) app.m->render_frame();
            }
            return true;
        };

        // Fast-forward BIOS + Tecmo boot into Title screen ("INSERT COIN(S)")
        copa_run_ok &= step_frames(900, 0xFFu, 0xFFu, 0xFFu, false);
        copa_run_ok &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, true);
        write_ppm(vdir / "00_title.ppm", app.m->vdp);
        std::vector<uint8_t> title_snap(16u * 1024u * 1024u);
        const size_t title_snap_len = app.m->save_state_bytes(title_snap.data(), title_snap.size());

        // Insert Coin + 1P Start -> enter 3x4 Group Team Select
        copa_run_ok &= step_frames(8, 0xFFu, 0xFFu, 0xFEu, false);   // Coin 1
        copa_run_ok &= step_frames(24, 0xFFu, 0xFFu, 0xFFu, false);
        copa_run_ok &= step_frames(10, 0xFFu, 0xFFu, 0xEFu, false);  // 1P Start
        for (int wf = 0; wf < 180 && !app.m->copa_entered_gameplay; ++wf) {
            copa_run_ok &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
        }
        copa_run_ok &= step_frames(45, 0xFFu, 0xFFu, 0xFFu, true);
        const auto cur_sel = [&]() -> uint8_t { return app.m->inner.high_wram[0x00076552u + 1u]; };
        const bool ts_overlay_ok = app.m->vdp.copa_group_badges;
        // Same-session Euro cold boot: the NBG flag map starts at VRAM 0x7C000,
        // 64 cells/plane row, adjacent plane at +0x2000. Check displayed maps,
        // not just the generated cartridge data (rightmost flags cross the plane edge).
        bool grid_visible_ok = true;
        for (const auto& group : app.copa_backup->tournament.groups) {
            for (uint8_t slot : group) {
                if (slot == cur_sel()) continue; // The flashing 1P badge replaces this flag.
                const uint8_t* xy = app.m->cart_rom + Off::kMsgOutPosBaseOff +
                    slot * Off::kMsgOutPosStride + Off::kMsgOutPosXyOff;
                const uint8_t* map = app.m->cart_rom + Off::kMsgOutBaseOff + slot * Off::kMsgOutMapBytes;
                for (uint32_t row = 0u; row < 4u; ++row) {
                    for (uint32_t col = 0u; col < 12u; ++col) {
                        const uint32_t x = xy[0] + col;
                        const uint32_t address = 0x7C000u + (x / 64u) * 0x2000u +
                            (xy[1] + row) * 128u + (x % 64u) * 2u;
                        grid_visible_ok &= std::memcmp(app.m->inner.vdp2_vram.data() + address,
                                                       map + row * 24u + col * 2u, 2u) == 0;
                    }
                }
            }
        }
        std::printf("COPA_AFTER_START main_pc=0x%08x entered=%d p1_sel=%u ts_badges=%d\n",
                    app.m->cpus[0].pc, app.m->copa_entered_gameplay ? 1 : 0, cur_sel(), ts_overlay_ok ? 1 : 0);
        write_ppm(vdir / "01_team_select_groups.ppm", app.m->vdp);
        write_bytes(vdir / "team_select_cart_assets.bin",
                    app.m->cart_rom + Off::kCelOutTilesBaseOff,
                    Off::kMsgOutBaseOff + 19u * Off::kMsgOutMapBytes - Off::kCelOutTilesBaseOff);
        write_bytes(vdir / "team_select_vram.bin", app.m->inner.vdp2_vram.data(), twc98::kVdp2VramSize);
        write_bytes(vdir / "team_select_cram.bin", app.m->vdp.vdp2_cram, sizeof(app.m->vdp.vdp2_cram));

        auto find_cell = [&](uint8_t slot, int& out_r, int& out_c) -> bool {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    if (app.copa_backup->tournament.groups[r][c] == slot) {
                        out_r = r;
                        out_c = c;
                        return true;
                    }
                }
            }
            return false;
        };
        auto navigate_to_slot = [&](uint8_t target_slot) -> bool {
            int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
            if (!find_cell(cur_sel(), r0, c0) || !find_cell(target_slot, r1, c1)) return false;
            while (r0 < r1) {
                copa_run_ok &= step_frames(4, 0xEFu, 0xFFu, 0xFFu, false); // Down
                copa_run_ok &= step_frames(18, 0xFFu, 0xFFu, 0xFFu, false);
                ++r0;
            }
            while (r0 > r1) {
                copa_run_ok &= step_frames(4, 0xDFu, 0xFFu, 0xFFu, false); // Up
                copa_run_ok &= step_frames(18, 0xFFu, 0xFFu, 0xFFu, false);
                --r0;
            }
            while (c0 < c1) {
                copa_run_ok &= step_frames(4, 0xBFu, 0xFFu, 0xFFu, false); // Right
                copa_run_ok &= step_frames(18, 0xFFu, 0xFFu, 0xFFu, false);
                ++c0;
            }
            while (c0 > c1) {
                copa_run_ok &= step_frames(4, 0x7Fu, 0xFFu, 0xFFu, false); // Left
                copa_run_ok &= step_frames(18, 0xFFu, 0xFFu, 0xFFu, false);
                --c0;
            }
            copa_run_ok &= step_frames(12, 0xFFu, 0xFFu, 0xFFu, true);
            return cur_sel() == target_slot;
        };

        const bool ven_nav_ok = navigate_to_slot(7u);
        const uint8_t sel_ven = cur_sel();
        const int ven_grp = twc98::copa::find_team_group(app.copa_backup->tournament, 7u);
        write_ppm(vdir / "07b_team_select_ven.ppm", app.m->vdp);

        const bool bol_nav_ok = navigate_to_slot(14u);
        const uint8_t sel_bol = cur_sel();
        write_ppm(vdir / "07_team_select_bol.ppm", app.m->vdp);
        std::printf("TS_BOL v1_obs=%d capa14_skin=%u master14=%u,%u capa14_gk=%u,%u\n",
                    app.m->vdp.v1_obs_disp_count,
                    app.m->inner.low_wram[Off::kCapaTblLowWramOff + 14u * Off::kCapaTeamBytes + 1u],
                    app.m->inner.high_wram[Off::kMasterSkinHighWramOff + 14u * 11u + 0u],
                    app.m->inner.high_wram[Off::kMasterSkinHighWramOff + 14u * 11u + 1u],
                    app.m->inner.low_wram[Off::kCapaTblLowWramOff + 14u * Off::kCapaTeamBytes + 2u],
                    app.m->inner.low_wram[Off::kCapaTblLowWramOff + 14u * Off::kCapaTeamBytes + 25u]);

        const bool sel_seq_ok =
            random_groups_ok && ts_overlay_ok && ven_nav_ok && bol_nav_ok &&
            (sel_ven == 7u && ven_grp >= 0 && sel_bol == 14u);
        std::printf("COPA_SEL_SEQ ven=%u(grp=%d) bol=%u ts_badges=%d rand=%d ok=%d\n",
                    sel_ven, ven_grp, sel_bol, ts_overlay_ok ? 1 : 0, random_groups_ok ? 1 : 0, sel_seq_ok ? 1 : 0);

        // Confirm Bolivia -> Wait for B-H5-09 Progress Screen (Groups) -> capture 13_progress_groups.ppm -> dismiss -> VIEW MODE -> SUPER PLAY -> Kickoff
        copa_run_ok &= step_frames(8, 0xFEu, 0xFFu, 0xFFu, false); // Btn1 Confirm BOL(14)
        for (int pf = 0; pf < 200 && app.m->vdp.copa_bracket.progress_active == 0u; ++pf) {
            copa_run_ok &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
        }
        const bool prog_grp_opened = (app.m->vdp.copa_bracket.progress_active != 0u);
        if (prog_grp_opened) {
            copa_run_ok &= step_frames(30, 0xFFu, 0xFFu, 0xFFu, true);
            write_ppm(vdir / "13_progress_groups.ppm", app.m->vdp);
            copa_run_ok &= step_frames(8, 0xFEu, 0xFFu, 0xFFu, false); // Dismiss Progress Screen cleanly
        }
        copa_run_ok &= step_frames(210, 0xFFu, 0xFFu, 0xFFu, false);
        copa_run_ok &= step_frames(10, 0xFEu, 0xFFu, 0xFFu, false); // Btn1 Confirm VIEW MODE (NORMAL)
        copa_run_ok &= step_frames(20, 0xFFu, 0xFFu, 0xFFu, false);
        copa_run_ok &= step_frames(8, 0xFEu, 0xFFu, 0xFFu, false);  // Safety pulse if VIEW MODE opened slightly later
        copa_run_ok &= step_frames(100, 0xFFu, 0xFFu, 0xFFu, true);
        write_ppm(vdir / "08a_bolivia_superplay.ppm", app.m->vdp);

        for (int kf = 0; kf < 480 && app.m->vdp.v1_obs_disp_count <= 5; ++kf) {
            copa_run_ok &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, true);
        }
        copa_run_ok &= step_frames(60, 0xFFu, 0xFFu, 0xFFu, true);
        write_ppm(vdir / "08_bolivia_kickoff.ppm", app.m->vdp);

        const auto rd_hw16 = [&](uint32_t hw_off) -> uint16_t {
            return static_cast<uint16_t>(
                (static_cast<uint16_t>(app.m->inner.high_wram[hw_off]) << 8) |
                app.m->inner.high_wram[hw_off + 1u]);
        };
        const auto verify_live_team_consistency = [&](const char* tag, uint8_t exp_p1, uint8_t exp_round, uint8_t& out_opp) -> bool {
            const uint8_t* hw = app.m->inner.high_wram.data();
            const uint8_t act_round = hw[0x0007654Du];
            const uint8_t act_p1 = cur_sel();
            const uint8_t act_opp = hw[0x00076555u];
            out_opp = act_opp;
            const auto& specs = twc98::copa::copa_team_specs();
            const bool opp_valid = (act_opp < Off::kTeamCount) && specs[act_opp].active_in_copa && (act_opp != exp_p1);
            const uint8_t kit0 = hw[0x00076568u];
            const uint8_t kit1 = hw[0x00076569u];
            const uint16_t cache0 = rd_hw16(0x000792BAu);
            const uint16_t cache1 = rd_hw16(0x000792BCu);

            auto sp_bar_matches = [&](uint32_t bar_off, uint8_t team) -> bool {
                if (team >= Off::kTeamCount) return false;
                uint8_t exp_list[3]{};
                uint8_t exp_cnt = 0u;
                for (int k = 0; k < 3; ++k) {
                    if (specs[team].sp[k] != 0u) exp_list[exp_cnt++] = specs[team].sp[k];
                }
                if (hw[bar_off] != exp_cnt) return false;
                for (uint8_t k = 0u; k < exp_cnt; ++k) {
                    if (hw[bar_off + 1u + k] != exp_list[k]) return false;
                }
                return true;
            };

            const bool bracket_ok = (act_opp == app.m->vdp.copa_bracket.next_opp);
            const bool cache_ok = (exp_round > 0u) ||
                ((cache0 == static_cast<uint16_t>(kit0)) &&
                 (cache1 == static_cast<uint16_t>(kit1)));
            const bool kits_ok = opp_valid &&
                ((kit0 & 0x7Fu) == exp_p1) &&
                ((kit1 & 0x7Fu) == act_opp) &&
                cache_ok;
            const bool sp_p1_ok = sp_bar_matches(Off::kMatchBarP1HighWramOff, exp_p1);
            const bool sp_cpu_ok = opp_valid && sp_bar_matches(Off::kMatchBarP2HighWramOff, act_opp);
            const bool all_ok = (act_round == exp_round) && (act_p1 == exp_p1) &&
                                opp_valid && bracket_ok && kits_ok && sp_p1_ok && sp_cpu_ok;
            std::printf("COPA_TEAM_CONSISTENCY[%s] rnd=%u/%u p1=%u/%u opp=%u(brk=%u) kit=(0x%02x,0x%02x) cache=(0x%04x,0x%04x) sp_p1=%d sp_cpu=%d[%u,%u,%u vs %u,%u,%u] ok=%d\n",
                        tag, act_round, exp_round, act_p1, exp_p1, act_opp, app.m->vdp.copa_bracket.next_opp,
                        kit0, kit1, cache0, cache1, sp_p1_ok ? 1 : 0, sp_cpu_ok ? 1 : 0,
                        hw[Off::kMatchBarP2HighWramOff + 1u], hw[Off::kMatchBarP2HighWramOff + 2u], hw[Off::kMatchBarP2HighWramOff + 3u],
                        opp_valid ? specs[act_opp].sp[0] : 0u, opp_valid ? specs[act_opp].sp[1] : 0u, opp_valid ? specs[act_opp].sp[2] : 0u,
                        all_ok ? 1 : 0);
            return all_ok;
        };

        const uint8_t p1_team = cur_sel();
        const uint8_t bar_p1_sp0 = app.m->inner.high_wram[Off::kMatchBarP1HighWramOff + 0u];
        const uint8_t bar_p1_sp1 = app.m->inner.high_wram[Off::kMatchBarP1HighWramOff + 1u];
        const uint8_t bar_p1_sp2 = app.m->inner.high_wram[Off::kMatchBarP1HighWramOff + 2u];
        std::printf("COPA_MATCH_BAR p1_team=%u p1_sp=[%u,%u,%u] v1_obs=%d entered_game=%d ts_badges=%d\n",
                    p1_team, bar_p1_sp0, bar_p1_sp1, bar_p1_sp2,
                    app.m->vdp.v1_obs_disp_count, app.m->copa_entered_gameplay ? 1 : 0,
                    app.m->vdp.copa_group_badges ? 1 : 0);
        uint8_t bol_r0_opp = 0xFFu;
        bool team_consistency_ok = verify_live_team_consistency("BOL_R0", 14u, 0u, bol_r0_opp);

        // Kickoff play: wait for KICK OFF whistle, kickoff pass, sprint/dribble into attacking half, then shoot (SP 3)
        bool sp3_charge_cap = false;
        bool sp3_cap1 = false;
        bool sp3_cap2 = false;
        for (int sf = 0; sf < 260; ++sf) {
            uint8_t pa = 0xBFu; // Hold Right to dribble forward
            if (sf < 95) {
                pa = 0xFFu; // Wait for KICK OFF banner & whistle
            } else if (sf >= 95 && sf < 103) {
                pa = 0xBEu; // Right + Btn1 kickoff pass forward
            } else if (sf >= 103 && sf < 155) {
                pa = 0x9Fu; // Dribble Up+Right into open attacking lane
            } else if (sf >= 155 && sf < 166) {
                pa = 0xBCu; // Right + Btn1 + Btn2 -> 0x0500: SP Slot 0 (BOL SP 3 Wind Shoot)
            } else if (sf >= 166 && sf < 176) {
                pa = 0xBFu;
            } else if (sf >= 176 && sf < 186) {
                pa = 0xBDu; // Right + Btn2 (follow-up Shoot if needed)
            }
            copa_run_ok &= step_frames(1, pa, 0xFFu, 0xFFu, true);
            if (!sp3_charge_cap && app.m->copa_sp3_kick_count > 0u &&
                app.m->vdp.copa_sp_vfx.charge_count > 0u && app.m->vdp.wind_vfx.frame_age >= 10u) {
                sp3_charge_cap = true;
                std::printf("  WIND_CHARGE tmr=%u rays=%u age=%u wf=(%.1f,%.1f)\n",
                            app.m->copa_sp3_wind_timer,
                            static_cast<unsigned>(app.m->vdp.copa_sp_vfx.charge_count),
                            app.m->vdp.copa_sp_vfx.charge_age,
                            app.m->vdp.wind_vfx.cx, app.m->vdp.wind_vfx.cy);
                write_ppm(vdir / "09_sp3_charge_wind.ppm", app.m->vdp);
            }
            if (!sp3_cap1 && app.m->copa_sp3_kick_count > 0u && app.m->copa_sp3_wind_timer == 64u) {
                sp3_cap1 = true;
                std::printf("  WIND_CAP1 tmr=%u v1_cnt=%d wf=(%.1f,%.1f) r=%.1f\n",
                            app.m->copa_sp3_wind_timer, app.m->vdp.v1_obs_disp_count,
                            app.m->vdp.wind_vfx.cx, app.m->vdp.wind_vfx.cy, app.m->vdp.wind_vfx.radius);
                write_ppm(vdir / "09_sp3_wind_vfx.ppm", app.m->vdp);
            } else if (sp3_cap1 && !sp3_cap2 && app.m->copa_sp3_wind_timer == 46u) {
                sp3_cap2 = true;
                std::printf("  WIND_CAP2 tmr=%u wf=(%.1f,%.1f) trail=%d\n",
                            app.m->copa_sp3_wind_timer,
                            app.m->vdp.wind_vfx.cx, app.m->vdp.wind_vfx.cy,
                            app.m->vdp.wind_vfx.trail_count);
                write_ppm(vdir / "09b_sp3_wind_vfx.ppm", app.m->vdp);
            }
        }
        const bool sp3_ok = (app.m->copa_sp3_kick_count > 0u && sp3_charge_cap && sp3_cap1 && sp3_cap2);
        std::printf("COPA_SP3_CHECK kicks=%u timer_rem=%u charge=%d cap1=%d cap2=%d ok=%d\n",
                    app.m->copa_sp3_kick_count, app.m->copa_sp3_wind_timer,
                    sp3_charge_cap ? 1 : 0, sp3_cap1 ? 1 : 0, sp3_cap2 ? 1 : 0, sp3_ok ? 1 : 0);

        // Toggle back to Classic and verify 100% bit-exact restore of all cartridge bytes
        set_host_game_mode(app, twc98::copa::GameMode::Classic);
        bool classic_exact =
            (std::memcmp(app.m->cart_rom, orig_cart_slice.data(), orig_cart_slice.size()) == 0) &&
            !app.m->vdp.copa_group_badges &&
            !app.copa_backup->tournament.initialized;

        // Separate cold session: VEN must also reach a live match, not merely highlight
        // an inactive slot on Team Select. Keep the same seed so navigation is reproducible.
        bool ven_match_ok = false;
        if (copa_run_ok && classic_exact &&
            app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err)) {
            app.copa_backup.reset();
            set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x54574339u);
            ven_match_ok = step_frames(900, 0xFFu, 0xFFu, 0xFFu, false) &&
                step_frames(8, 0xFFu, 0xFFu, 0xFEu, false) &&
                step_frames(24, 0xFFu, 0xFFu, 0xFFu, false) &&
                step_frames(10, 0xFFu, 0xFFu, 0xEFu, false) &&
                step_frames(225, 0xFFu, 0xFFu, 0xFFu, true) && navigate_to_slot(7u);
            if (ven_match_ok) {
                ven_match_ok = step_frames(8, 0xFEu, 0xFFu, 0xFFu, false);
                for (int pf = 0; pf < 200 && app.m->vdp.copa_bracket.progress_active == 0u; ++pf) {
                    ven_match_ok &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
                }
                ven_match_ok &= step_frames(30, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(8, 0xFEu, 0xFFu, 0xFFu, false) &&
                    step_frames(210, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(10, 0xFEu, 0xFFu, 0xFFu, false) &&
                    step_frames(20, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(8, 0xFEu, 0xFFu, 0xFFu, false) &&
                    step_frames(400, 0xFFu, 0xFFu, 0xFFu, true);
                ven_match_ok &= cur_sel() == 7u && app.m->vdp.v1_obs_disp_count > 5 &&
                    !app.m->vdp.copa_group_badges;
                uint8_t ven_r0_opp = 0xFFu;
                team_consistency_ok &= verify_live_team_consistency("VEN_R0", 7u, 0u, ven_r0_opp);
                write_ppm(vdir / "10_venezuela_match.ppm", app.m->vdp);
            }
            bool hard_mode_ok = false;
            bool goal_limit_ok = false;
            if (ven_match_ok) {
                // Verify B-H5-05 Hard Mode (constant max AI level 31 in 0x0603EB84 & 0x0603F872 + DIP 3)
                twc98::copa::MatchRulesConfig hm_cfg{};
                hm_cfg.hard_mode = true;
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, hm_cfg);
                step_frames(5, 0xFFu, 0xFFu, 0xFFu, false);
                const uint16_t ai1_on = (static_cast<uint16_t>(app.m->inner.high_wram[0x0003EB84u]) << 8) |
                                        app.m->inner.high_wram[0x0003EB85u];
                const uint16_t ai2_on = (static_cast<uint16_t>(app.m->inner.high_wram[0x0003F872u]) << 8) |
                                        app.m->inner.high_wram[0x0003F873u];
                const uint16_t dip_on = (static_cast<uint16_t>(app.m->inner.high_wram[0x00076520u]) << 8) |
                                        app.m->inner.high_wram[0x00076521u];
                hm_cfg.hard_mode = false;
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, hm_cfg);
                const uint16_t ai1_off = (static_cast<uint16_t>(app.m->inner.high_wram[0x0003EB84u]) << 8) |
                                         app.m->inner.high_wram[0x0003EB85u];
                const uint16_t ai2_off = (static_cast<uint16_t>(app.m->inner.high_wram[0x0003F872u]) << 8) |
                                         app.m->inner.high_wram[0x0003F873u];
                hard_mode_ok = (ai1_on == 0xE01Fu && ai2_on == 0xEE1Fu && dip_on == 3u &&
                                ai1_off == 0x6063u && ai2_off == 0x6EEEu);

                // Verify B-H6-04 Goal Limit (clock pins at 99 until P1/P2 reaches 3 goals, then triggers match end)
                twc98::copa::MatchRulesConfig gl_cfg{};
                gl_cfg.rule_mode = twc98::copa::MatchRuleMode::GoalLimit;
                gl_cfg.rule_value = 3u;
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, gl_cfg);
                step_frames(70, 0xFFu, 0xFFu, 0xFFu, false);
                const uint16_t pinned_t = (static_cast<uint16_t>(app.m->inner.high_wram[0x0007654Eu]) << 8) |
                                          app.m->inner.high_wram[0x0007654Fu];
                app.m->inner.high_wram[0x0007655Au] = 0u;
                app.m->inner.high_wram[0x0007655Bu] = 3u;
                step_frames(150, 0xFFu, 0xFFu, 0xFFu, false);
                const uint16_t end_t = (static_cast<uint16_t>(app.m->inner.high_wram[0x0007654Eu]) << 8) |
                                       app.m->inner.high_wram[0x0007654Fu];
                goal_limit_ok = (pinned_t == 99u && end_t <= 1u);
                app.match_rules = twc98::copa::MatchRulesConfig{};
                twc98::copa::set_match_rules(*app.m, *app.copa_backup, app.match_rules);
            }

            // Fase 3 (B-H5-06 & B-H5-07): Verify Copa Trophy + Winner Ending assets & live Bolivia Ending
            const bool copa_trophy_ok =
                (std::memcmp(app.m->cart_rom + Off::kPolCupOff,
                             orig_cart_slice.data() + Off::kPolCupOff, Off::kPolCupBytes) != 0) &&
                (std::memcmp(app.m->cart_rom + Off::kCupCelOff,
                             orig_cart_slice.data() + Off::kCupCelOff, Off::kCupCelBytes) != 0) &&
                (cart_u16(Off::kCupPalCartOff + 6u) !=
                 static_cast<uint16_t>((orig_cart_slice[Off::kCupPalCartOff + 6u] << 8) |
                                       orig_cart_slice[Off::kCupPalCartOff + 7u])) &&
                (cart_u16(Off::kCupPalCartOff + 6u) ==
                 static_cast<uint16_t>((app.m->inner.high_wram[Off::kCupPalHighWramOff + 6u] << 8) |
                                       app.m->inner.high_wram[Off::kCupPalHighWramOff + 7u]));

            bool ending_assets_ok =
                (std::memcmp(app.m->cart_rom + Off::kMsgRankOff,
                             orig_cart_slice.data() + Off::kMsgRankOff, Off::kMsgRankBytes) != 0);
            for (uint8_t ks : {0u, 3u, 10u, 12u, 15u}) {
                if (cart_u32(Off::kMsgRankOff + 8u + (120u + ks) * 4u) !=
                    ((static_cast<uint32_t>(orig_cart_slice[Off::kMsgRankOff + 8u + (120u + ks) * 4u]) << 24) |
                     (static_cast<uint32_t>(orig_cart_slice[Off::kMsgRankOff + 9u + (120u + ks) * 4u]) << 16) |
                     (static_cast<uint32_t>(orig_cart_slice[Off::kMsgRankOff + 10u + (120u + ks) * 4u]) << 8) |
                     static_cast<uint32_t>(orig_cart_slice[Off::kMsgRankOff + 11u + (120u + ks) * 4u]))) {
                    ending_assets_ok = false;
                }
            }
            for (uint8_t ms : {1u, 5u, 6u, 7u, 11u, 13u, 14u}) {
                const uint32_t t_off = cart_u32(Off::kMsgRankOff + 8u + (120u + ms) * 4u) - 0x002B0000u;
                const uint32_t p_map = cart_u32(Off::kMsgRankOff + t_off + 2u) - 0x002B0000u;
                if (app.m->cart_rom[Off::kMsgRankOff + p_map + 5u] != 7u ||
                    app.m->cart_rom[Off::kMsgRankOff + p_map + 4u] < 20u) {
                    ending_assets_ok = false;
                }
            }
            static constexpr uint32_t kVerifyTexEn[12] = {
                0x010E394Cu, 0x010F6F60u, 0x0111DB88u, 0x011447B0u, 0x01157DC4u, 0x0116B3D8u,
                0x011A5614u, 0x011B8C28u, 0x011CC23Cu, 0x011DF850u, 0x011F2E64u, 0x01206478u
            };
            for (uint32_t off : kVerifyTexEn) {
                if (std::memcmp(app.m->cart_rom + off, orig_cart_slice.data() + off, Off::kTexEnBytes) == 0) {
                    ending_assets_ok = false;
                }
            }
            if (std::memcmp(app.m->cart_rom + 0x010AB4ECu, orig_cart_slice.data() + 0x010AB4ECu, 56072u) == 0) {
                ending_assets_ok = false;
            }

            bool bolivia_ending_ok = false;
            if (ven_match_ok && ending_assets_ok && copa_trophy_ok) {
                app.m->inner.high_wram[0x0007654Du] = 7u;
                app.m->inner.high_wram[0x00076552u + 1u] = 14u;
                app.m->inner.high_wram[0x00076556u + 1u] = 14u;
                app.m->inner.high_wram[0x0007655Au] = 0u;
                app.m->inner.high_wram[0x0007655Bu] = 4u;
                app.m->inner.high_wram[0x0007654Eu] = 0u;
                app.m->inner.high_wram[0x0007654Fu] = 1u;
                app.m->inner.high_wram[0x00076550u] = 0u;
                app.m->inner.high_wram[0x00076551u] = 0u;
                app.m->inner.high_wram[0x000970C4u] = 0u;
                app.m->inner.high_wram[0x000970C5u] = 1u;
                bool saw_champ_banner = false;
                bool saw_thankyou = false;
                int rank_seen_count = 0;
                for (int ek = 0; ek < 400; ++ek) {
                    const bool want_render = (ek >= 220);
                    step_frames(15, (ek < 75 && ek % 6 == 0) ? 0xFEu : 0xFFu, 0xFFu, 0xFFu, want_render);
                    const char* h28 = reinterpret_cast<const char*>(app.m->inner.low_wram.data() + 0x08C000u);
                    if (ek >= 220 && std::strncmp(h28, "CEL_RANK", 8) == 0 && !saw_champ_banner) {
                        ++rank_seen_count;
                        int yellow_px = 0;
                        int green_px = 0;
                        if (app.m->vdp.frame_w > 0 && app.m->vdp.frame_h >= 400) {
                            for (int y = 340; y < 430; ++y) {
                                for (int x = 150; x < 550; ++x) {
                                    const uint32_t c = app.m->vdp.frame_px[y * app.m->vdp.frame_w + x];
                                    const uint32_t r = (c >> 16) & 0xFFu;
                                    const uint32_t g = (c >> 8) & 0xFFu;
                                    const uint32_t b = c & 0xFFu;
                                    if (r > 200u && g > 200u && b < 40u) ++yellow_px;
                                }
                            }
                            for (int y = 120; y < 320; y += 2) {
                                for (int x = 80; x < 620; x += 2) {
                                    const uint32_t c = app.m->vdp.frame_px[y * app.m->vdp.frame_w + x];
                                    const uint32_t r = (c >> 16) & 0xFFu;
                                    const uint32_t g = (c >> 8) & 0xFFu;
                                    const uint32_t b = c & 0xFFu;
                                    if (g > r + 24u && g > b + 24u) ++green_px;
                                }
                            }
                        }
                        if (rank_seen_count >= 2 && yellow_px > 400 && green_px > 500) {
                            write_ppm(vdir / "11_bolivia_champion.ppm", app.m->vdp);
                            saw_champ_banner = true;
                        }
                    }
                    if (saw_champ_banner && !saw_thankyou && ek >= 240) {
                        int sepia_px = 0;
                        int ty_yellow = 0;
                        if (app.m->vdp.frame_w > 0 && app.m->vdp.frame_h >= 400) {
                            for (int y = 100; y < 320; y += 2) {
                                for (int x = 80; x < 620; x += 2) {
                                    const uint32_t c = app.m->vdp.frame_px[y * app.m->vdp.frame_w + x];
                                    const uint32_t r = (c >> 16) & 0xFFu;
                                    const uint32_t g = (c >> 8) & 0xFFu;
                                    const uint32_t b = c & 0xFFu;
                                    if (r > g && g > b && (r - b) >= 20u && (r - b) <= 80u) ++sepia_px;
                                }
                            }
                            for (int y = 340; y < 430; ++y) {
                                for (int x = 150; x < 550; ++x) {
                                    const uint32_t c = app.m->vdp.frame_px[y * app.m->vdp.frame_w + x];
                                    if (((c >> 16) & 0xFFu) > 200u && ((c >> 8) & 0xFFu) > 200u && (c & 0xFFu) < 40u) {
                                        ++ty_yellow;
                                    }
                                }
                            }
                        }
                        if (sepia_px > 12000 && ty_yellow > 500) {
                            write_ppm(vdir / "12_bolivia_thankyou.ppm", app.m->vdp);
                            saw_thankyou = true;
                            break;
                        }
                    }
                }
                bolivia_ending_ok = saw_champ_banner && saw_thankyou;
            }

            // Dedicated live SH-2 transition check: Round 0 VEN win -> real post-match 0x06007CD2/0x06007DAE
            // -> Round 1 Progress Screen -> 2nd Round Intro -> Round 1 Kickoff (kits + SP + scoreboard must all match!).
            if (ven_match_ok && app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err)) {
                app.copa_backup.reset();
                set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x54574339u);
                bool r1_boot_ok = step_frames(900, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(8, 0xFFu, 0xFFu, 0xFEu, false) &&
                    step_frames(24, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(10, 0xFFu, 0xFFu, 0xEFu, false) &&
                    step_frames(225, 0xFFu, 0xFFu, 0xFFu, false) &&
                    navigate_to_slot(7u);
                if (r1_boot_ok) {
                    step_frames(8, 0xFEu, 0xFFu, 0xFFu, false); // Confirm VEN
                    for (int pf = 0; pf < 200 && app.m->vdp.copa_bracket.progress_active == 0u; ++pf) {
                        step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
                    }
                    step_frames(30, 0xFFu, 0xFFu, 0xFFu, false); // >=25f debounce before dismissing Progress Screen
                    step_frames(8, 0xFEu, 0xFFu, 0xFFu, false); // Dismiss R0 Progress
                    step_frames(210, 0xFFu, 0xFFu, 0xFFu, false);
                    step_frames(10, 0xFEu, 0xFFu, 0xFFu, false); // VIEW MODE
                    step_frames(20, 0xFFu, 0xFFu, 0xFFu, false);
                    step_frames(8, 0xFEu, 0xFFu, 0xFFu, false);
                    step_frames(400, 0xFFu, 0xFFu, 0xFFu, false); // Reach R0 Kickoff
                    // Score 3-0 win via GoalLimit (exact path proven above) -> clean transition through Round 0 Result -> Round 1 Progress
                    twc98::copa::MatchRulesConfig r1_gl{};
                    r1_gl.rule_mode = twc98::copa::MatchRuleMode::GoalLimit;
                    r1_gl.rule_value = 3u;
                    twc98::copa::set_match_rules(*app.m, *app.copa_backup, r1_gl);
                    step_frames(70, 0xFFu, 0xFFu, 0xFFu, false);
                    app.m->inner.high_wram[0x0007655Au] = 0u;
                    app.m->inner.high_wram[0x0007655Bu] = 3u;
                    step_frames(150, 0xFFu, 0xFFu, 0xFFu, false);
                    app.match_rules = twc98::copa::MatchRulesConfig{};
                    twc98::copa::set_match_rules(*app.m, *app.copa_backup, app.match_rules);
                    for (int r1f = 0; r1f < 900; ++r1f) {
                        if (app.m->vdp.copa_bracket.progress_active != 0u &&
                            app.m->vdp.copa_bracket.progress_stage == 1u) {
                            break;
                        }
                        const uint8_t pa = ((r1f % 24) < 6) ? 0xFEu : 0xFFu;
                        step_frames(1, pa, 0xFFu, 0xFFu, false);
                    }
                    const bool r1_prog_ok = (app.m->vdp.copa_bracket.progress_active != 0u &&
                                             app.m->vdp.copa_bracket.progress_stage == 1u);
                    if (r1_prog_ok) {
                        step_frames(30, 0xFFu, 0xFFu, 0xFFu, false); // >=25f debounce before dismissing Progress Screen
                        step_frames(8, 0xFEu, 0xFFu, 0xFFu, false); // Dismiss R1 Progress
                        step_frames(210, 0xFFu, 0xFFu, 0xFFu, false);
                        step_frames(10, 0xFEu, 0xFFu, 0xFFu, false); // Confirm VIEW MODE
                        step_frames(20, 0xFFu, 0xFFu, 0xFFu, false);
                        step_frames(8, 0xFEu, 0xFFu, 0xFFu, false);
                        // Wait until Round 1 Kickoff pitch renders (past 2nd ROUND SUPER PLAY into active 90s timer)
                        for (int kf = 0; kf < 700; ++kf) {
                            step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
                            if (app.m->inner.high_wram[0x0007654Du] == 1u &&
                                rd_hw16(0x0007654Eu) >= 85u &&
                                app.m->vdp.v1_obs_disp_count > 5) {
                                break;
                            }
                        }
                        step_frames(60, 0xFFu, 0xFFu, 0xFFu, true);
                        write_ppm(vdir / "10b_venezuela_r1_kickoff.ppm", app.m->vdp);
                        uint8_t ven_r1_opp = 0xFFu;
                        team_consistency_ok &= verify_live_team_consistency("VEN_R1", 7u, 1u, ven_r1_opp);
                    } else {
                        std::printf("COPA_TEAM_CONSISTENCY[VEN_R1] failed: progress_stage=%u rnd=%u\n",
                                    app.m->vdp.copa_bracket.progress_stage, app.m->inner.high_wram[0x0007654Du]);
                        team_consistency_ok = false;
                    }
                } else {
                    team_consistency_ok = false;
                }
            } else {
                team_consistency_ok = false;
            }

            // B-H5-08 & B-H5-09: Verify 6 Tournament Rounds (0..2 Group + 3 QF + 4 SF + 5 Final) + 7th All-Star (6)
            // and Visual Group & Bracket Progress Screen across all transitions.
            bool fase4_rounds_ok = false;
            bool progress_ui_ok = prog_grp_opened;
            {
                if (app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err)) {
                    app.copa_backup.reset();
                    set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x54574339u);
                    bool boot_ok = step_frames(900, 0xFFu, 0xFFu, 0xFFu, false);
                    step_frames(1, 0xFFu, 0xFFu, 0xFFu, false); // trigger on_frame_tick
                    boot_ok &= (rd_hw16(Off::kCopaRoundEndCmpHighWramOff) == 0xEC07u) &&
                               (rd_hw16(Off::kCopaOppAllStarHighWramOff) == 0x8806u) &&
                               (rd_hw16(Off::kCopaContEndHighWramOff) == 0xE206u) &&
                               (rd_hw16(Off::kCopaVsAllStarHighWramOff) == 0x8806u);

                    uint8_t seen_grp_opps[3] = {0xFFu, 0xFFu, 0xFFu};
                    const uint8_t bol_grp = static_cast<uint8_t>(
                        twc98::copa::find_team_group(app.copa_backup->tournament, 14u));
                    bool sched_ok = boot_ok && (bol_grp < 3u);
                    if (sched_ok) {
                        uint8_t* hw = app.m->inner.high_wram.data();
                        hw[0x0792A5u] = 5u;
                        hw[0x097148u] = 0u; hw[0x097149u] = 1u;
                        hw[0x076552u] = 0u; hw[0x076553u] = 14u; // Bolivia
                        for (uint8_t rnd = 0u; rnd <= 6u && sched_ok; ++rnd) {
                            hw[0x07654Du] = rnd;
                            hw[0x07655Au] = 0u; hw[0x07655Bu] = 2u; // P1 2 goals
                            hw[0x07655Cu] = 0u; hw[0x07655Du] = 0u; // CPU 0 goals
                            hw[0x07655Eu] = 0u; hw[0x07655Fu] = 0u;
                            hw[0x076560u] = 0u; hw[0x076561u] = 0u;
                            app.m->vdp.copa_bracket.live_p1_goals = 2u;
                            app.m->vdp.copa_bracket.live_p2_goals = 0u;
                            app.m->vdp.copa_bracket.live_goals_valid = 1u;
                            twc98::copa::on_vs_entry_hook(*app.m);
                            const uint8_t opp = hw[0x076555u];
                            if (rnd < 3u) {
                                seen_grp_opps[rnd] = opp;
                                const int ogrp = twc98::copa::find_team_group(app.copa_backup->tournament, opp);
                                sched_ok &= (opp != 14u && ogrp == static_cast<int>(bol_grp));
                            } else if (rnd == 3u) {
                                // Check QF brackets are all populated and render 14_progress_knockout.ppm
                                for (int q = 0; q < 4; ++q) {
                                    sched_ok &= (app.m->vdp.copa_bracket.qf[q].team_a < 16u &&
                                                 app.m->vdp.copa_bracket.qf[q].team_b < 16u);
                                }
                                app.m->render_frame();
                                write_ppm(vdir / "14_progress_knockout.ppm", app.m->vdp);
                            } else if (rnd == 6u) {
                                sched_ok &= (opp == 16u); // 7th round = ALL-STARS
                            }
                            app.m->vdp.copa_bracket.progress_active = 0u;
                        }
                        if (sched_ok) {
                            sched_ok &= (seen_grp_opps[0] != seen_grp_opps[1] &&
                                         seen_grp_opps[0] != seen_grp_opps[2] &&
                                         seen_grp_opps[1] != seen_grp_opps[2]);
                        }
                        // Also capture Final bracket screenshot (15_progress_allstar.ppm) at round 6 after SF/Final populate
                        hw[0x07654Du] = 6u;
                        app.m->vdp.copa_bracket.progress_active = 1u;
                        app.m->render_frame();
                        write_ppm(vdir / "15_progress_allstar.ppm", app.m->vdp);
                        app.m->vdp.copa_bracket.progress_active = 0u;
                    }
                    fase4_rounds_ok = sched_ok;
                    progress_ui_ok &= sched_ok &&
                        std::filesystem::exists(vdir / "13_progress_groups.ppm") &&
                        std::filesystem::exists(vdir / "14_progress_knockout.ppm");
                }
            }

            // Separate quick check: PER (slot 1) Super Combination triggers Machu Picchu replacement VFX cleanly
            bool machu_vfx_ok = false;
            if (ven_match_ok &&
                app.m->init_cold_boot_bytes(bios.data(), bios.size(), cart.data(), cart.size(), &cb_err)) {
                app.copa_backup.reset();
                set_host_game_mode(app, twc98::copa::GameMode::CopaAmerica, 0x54574339u);
                bool per_ready = step_frames(900, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(8, 0xFFu, 0xFFu, 0xFEu, false) &&
                    step_frames(24, 0xFFu, 0xFFu, 0xFFu, false) &&
                    step_frames(10, 0xFFu, 0xFFu, 0xEFu, false) &&
                    step_frames(225, 0xFFu, 0xFFu, 0xFFu, false) && navigate_to_slot(1u);
                if (per_ready) {
                    per_ready &= step_frames(8, 0xFEu, 0xFFu, 0xFFu, false);
                    for (int pf = 0; pf < 200 && app.m->vdp.copa_bracket.progress_active == 0u; ++pf) {
                        per_ready &= step_frames(1, 0xFFu, 0xFFu, 0xFFu, false);
                    }
                    per_ready &= step_frames(30, 0xFFu, 0xFFu, 0xFFu, false) &&
                        step_frames(8, 0xFEu, 0xFFu, 0xFFu, false) &&
                        step_frames(210, 0xFFu, 0xFFu, 0xFFu, false) &&
                        step_frames(10, 0xFEu, 0xFFu, 0xFFu, false) &&
                        step_frames(20, 0xFFu, 0xFFu, 0xFFu, false) &&
                        step_frames(8, 0xFEu, 0xFFu, 0xFFu, false) &&
                        step_frames(400, 0xFFu, 0xFFu, 0xFFu, false);
                }
                if (per_ready) {
                    const uint8_t* hw = app.m->inner.high_wram.data();
                    std::printf("COPA_PER_START p1_sp=[%u,%u,%u,%u] tmr=%u\n",
                                hw[Off::kMatchBarP1HighWramOff + 0u],
                                hw[Off::kMatchBarP1HighWramOff + 1u],
                                hw[Off::kMatchBarP1HighWramOff + 2u],
                                hw[Off::kMatchBarP1HighWramOff + 3u],
                                static_cast<unsigned>((hw[Off::kMatchTimerHighWramOff] << 8) | hw[Off::kMatchTimerHighWramOff + 1u]));
                    // 1) Save kick-off snapshot so SP 24 (Heel Lift -> Condor) and SP 14/38 (Super Combination -> Machu Picchu)
                    //    can each run from clean kick-off possession.
                    std::vector<uint8_t> per_ko_snap(7200000u);
                    const uint32_t per_ko_len = app.m->save_state_bytes(per_ko_snap.data(), per_ko_snap.size());

                    bool condor_vfx_ok = false;
                    for (int sf = 0; sf < 200; ++sf) {
                        uint8_t pa = 0xBFu;
                        if (sf < 95) pa = 0xFFu;
                        else if (sf < 103) pa = 0xBEu;  // Right + Btn1 kickoff pass
                        else if (sf < 145) pa = 0x9Fu;  // Dribble forward (Up+Right)
                        else if (sf < 165) pa = 0xB9u;  // Right + Btn2 + Btn3 -> SP 24 Heel Lift -> 3D Condor
                        step_frames(1, pa, 0xFFu, 0xFFu, true);
                        if (!condor_vfx_ok && app.m->vdp.copa_sp_vfx.condor.active &&
                            app.m->vdp.copa_sp_vfx.condor.anim_age >= 5u) {
                            condor_vfx_ok = true;
                            std::printf("COPA_PER_CONDOR_CHECK sf=%d age=%u box=(%d,%d)-(%d,%d) ok=1\n",
                                        sf, app.m->vdp.copa_sp_vfx.condor.anim_age,
                                        app.m->vdp.copa_sp_vfx.condor.min_x, app.m->vdp.copa_sp_vfx.condor.min_y,
                                        app.m->vdp.copa_sp_vfx.condor.max_x, app.m->vdp.copa_sp_vfx.condor.max_y);
                            write_ppm(vdir / "10b_peru_condor.ppm", app.m->vdp);
                        }
                    }

                    // 2) Restore kick-off and trigger SP 14 / SP 38 (Super Combination -> 3D Machu Picchu)
                    //    0xBC (Right + Btn1 + Btn2) triggers SP 14 at sf=148 and chains passes until phase 4 (~sf=272..340) draws Machu Picchu 3D!
                    if (per_ko_len > 0u) {
                        const char* lerr = nullptr;
                        app.m->load_state_bytes(per_ko_snap.data(), per_ko_len, &lerr);
                        for (int sf = 0; sf < 380; ++sf) {
                            uint8_t pa = 0xBFu;
                            if (sf < 95) pa = 0xFFu;
                            else if (sf < 103) pa = 0xBEu;
                            else if (sf < 145) pa = 0x9Fu;
                            else if (sf < 162) pa = 0xBCu;
                            step_frames(1, pa, 0xFFu, 0xFFu, true);
                            if (!machu_vfx_ok && app.m->vdp.copa_sp_vfx.machu.active &&
                                app.m->vdp.copa_sp_vfx.machu.anim_age >= 8u) {
                                machu_vfx_ok = true;
                                std::printf("COPA_PER_MACHU_CHECK sf=%d age=%u box=(%d,%d)-(%d,%d) ok=1\n",
                                            sf, app.m->vdp.copa_sp_vfx.machu.anim_age,
                                            app.m->vdp.copa_sp_vfx.machu.min_x, app.m->vdp.copa_sp_vfx.machu.min_y,
                                            app.m->vdp.copa_sp_vfx.machu.max_x, app.m->vdp.copa_sp_vfx.machu.max_y);
                                write_ppm(vdir / "10c_peru_machu.ppm", app.m->vdp);
                                break;
                            }
                        }
                    }
                    if (!condor_vfx_ok) {
                        std::printf("COPA_PER_CONDOR_CHECK not triggered; ok=0\n");
                    }
                    if (!machu_vfx_ok) {
                        std::printf("COPA_PER_MACHU_CHECK not triggered; ok=0\n");
                    }
                    machu_vfx_ok = (condor_vfx_ok && machu_vfx_ok);
                }
            }

            bool attract_ok = false;
            if (title_snap_len > 0u) {
                const char* lerr = nullptr;
                if (app.m->load_state_bytes(title_snap.data(), title_snap_len, &lerr)) {
                    bool idle_ok = true;
                    // Cross full Title -> Trophy -> Attract Demo window (f = 902..3401, past f=3002 halt)
                    idle_ok &= step_frames(2199, 0xFFu, 0xFFu, 0xFFu, true);
                    uint32_t nonblack_3100 = 0u;
                    for (uint32_t px : app.m->vdp.frame_px) {
                        if ((px & 0x00FFFFFFu) != 0u) ++nonblack_3100;
                    }
                    idle_ok &= step_frames(301, 0xFFu, 0xFFu, 0xFFu, false);
                    attract_ok = idle_ok && !app.m->halted && (nonblack_3100 > 0u);
                    std::printf("COPA_ATTRACT_CHECK f=3100..3401 nonblack=%u halted=%d ok=%d\n",
                                nonblack_3100, app.m->halted ? 1 : 0, attract_ok ? 1 : 0);
                }
            }

            set_host_game_mode(app, twc98::copa::GameMode::Classic);
            classic_exact &= std::memcmp(app.m->cart_rom, orig_cart_slice.data(), orig_cart_slice.size()) == 0;
            std::printf("COPA_MOD_VERIFY bol_sp=%d bol_skin=%d copa_gk=%d euro_remap=%d hidden=%d added=%d sel_seq=%d copa_run=%d sp3=%d sp3_darken=%d kit_clash=%d ven_flag=%d classic_exact=%d hud_assets=%d roster12=%d ven_match=%d grid_visible=%d hard_mode=%d goal_limit=%d copa_trophy=%d ending_assets=%d bolivia_ending=%d fase4_rounds=%d progress_ui=%d team_consistency=%d machu_vfx=%d attract=%d\n",
                        bol_sp_ok ? 1 : 0, bol_skin_ok ? 1 : 0, copa_gk_ok ? 1 : 0, euro_remap_ok ? 1 : 0,
                        hidden_ok ? 1 : 0, added_ok ? 1 : 0, sel_seq_ok ? 1 : 0,
                        copa_run_ok ? 1 : 0, sp3_ok ? 1 : 0, sp3_darken_ok ? 1 : 0,
                        kit_clash_ok ? 1 : 0, ven_flag_ok ? 1 : 0, classic_exact ? 1 : 0,
                        hud_assets_ok ? 1 : 0, roster_ok ? 1 : 0, ven_match_ok ? 1 : 0, grid_visible_ok ? 1 : 0,
                        hard_mode_ok ? 1 : 0, goal_limit_ok ? 1 : 0,
                        copa_trophy_ok ? 1 : 0, ending_assets_ok ? 1 : 0, bolivia_ending_ok ? 1 : 0,
                        fase4_rounds_ok ? 1 : 0, progress_ui_ok ? 1 : 0, team_consistency_ok ? 1 : 0,
                        machu_vfx_ok ? 1 : 0, attract_ok ? 1 : 0);
            return (bol_sp_ok && bol_skin_ok && copa_gk_ok && euro_remap_ok &&
                    hidden_ok && added_ok && sel_seq_ok && copa_run_ok && sp3_ok &&
                    sp3_darken_ok && kit_clash_ok && ven_flag_ok && classic_exact &&
                    hud_assets_ok && roster_ok && ven_match_ok && grid_visible_ok &&
                    hard_mode_ok && goal_limit_ok &&
                    copa_trophy_ok && ending_assets_ok && bolivia_ending_ok &&
                    fase4_rounds_ok && progress_ui_ok && team_consistency_ok &&
                    machu_vfx_ok && attract_ok) ? 0 : 6;
        }
        return 6;
    }

    if (!dump_panels_dir.empty()) {
        for (int rf = 0; rf < 5; ++rf) {
            app.m->set_input_ports(0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
            app.m->run_rounds(10000u);
        }
        app.m->render_frame();
        app.frames_run = 5u;
        app.cur_fps = 60.0;
        app.cur_ms = 4.2;
        app.status_banner = "READY:GPU-D3D11-HW";
        dump_debug_panels_ppm(app, dump_panels_dir);
        app.dbg_ui.shutdown();
        return 0;
    }
    const auto t_boot1 = std::chrono::steady_clock::now();
    const double cold_init_ms = std::chrono::duration<double, std::milli>(t_boot1 - t_boot0).count();

    g_app = &app;
    HWND hwnd = nullptr;
    if (!headless) {
        if (!g_ui.load()) {
            std::fprintf(stderr, "ERROR: Failed to load user32/gdi32 dynamically\n");
            return 3;
        }
        const HINSTANCE hinst = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = g_ui.load_cursor_a(nullptr, MAKEINTRESOURCEA(32512));
        wc.lpszClassName = "TWC98BO_G4_PC_HOST";
        g_ui.register_class_a(&wc);

        // Default 2x exact (1408x896) of original 704x448 when desktop permits; fallback 1.5x Fit on smaller screens.
        const int scr_w = g_ui.get_system_metrics(SM_CXSCREEN);
        const int scr_h = g_ui.get_system_metrics(SM_CYSCREEN);
        int init_w = 1408;
        int init_h = 896;
        if (scr_w < 1460 || scr_h < 960) {
            init_w = 1056;
            init_h = 672;
            if (app.aspect_mode == 0) app.aspect_mode = 1; // FIT cleanly when 2x exceeds desktop height
        }
        RECT wr{0, 0, init_w, init_h};
        g_ui.adjust_window_rect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = g_ui.create_window_ex_a(
            0, wc.lpszClassName, "Tecmo World Cup '98 (TWC98BO PC - 1:DevPanel B:Filter F12:CRT Q:Pitch F11:FS)",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hinst, nullptr);
        if (!hwnd) {
            std::fprintf(stderr, "ERROR: CreateWindowExA failed\n");
            return 3;
        }
        if (app.use_gpu) {
            if (app.gpu.init(hwnd, init_w, init_h)) {
                app.status_banner = std::string("READY:") + app.gpu.backend_name;
            } else {
                app.use_gpu = false;
                app.status_banner = "READY:CPU-GDI";
            }
        } else {
            app.status_banner = "READY:CPU-GDI";
        }
        g_ui.show_window(hwnd, SW_SHOW);
        g_ui.update_window(hwnd);
        if (initial_fullscreen) toggle_fullscreen(hwnd, app);
        if (enable_audio) g_winmm.load();
    }
    g_vdp_pool.init();

    FnXInputGetState xinput_fn = !headless ? load_xinput() : nullptr;
    ScspMailboxAudio audio;
    audio.init(enable_audio && !headless);

    const bool net_enabled = (net_host_port != 0u) || (!net_join_peer.empty());
    UdpNetTransport udp_net{};
    twc98::NetSession net_session{};
    std::vector<uint8_t> net_rollback_arena;
    if (net_enabled) {
        const uint8_t local_player = (!net_join_peer.empty() && net_host_port == 0u) ? 1u : 0u;
        const uint16_t bind_port = (net_host_port != 0u) ? net_host_port : 0u;
        if (udp_net.init(bind_port, net_join_peer)) {
            net_session.reset(local_player, net_delay_frames,
                              app.game_mode == twc98::copa::GameMode::CopaAmerica,
                              app.m->fix_p1_gk_ai);
            net_rollback_arena.resize(twc98::NetSession::kRequiredArenaBytes, 0u);
            app.mode_select_open = false;
            app.status_banner = (local_player == 0u) ? "NET P1 HOST" : "NET P2 GUEST";
        }
    }

    std::vector<uint8_t> save_buf(twc98::StvMachineState::kMaxSnapshotBytes, 0u);
    std::vector<double> frame_times_ms;
    if (smoke_frames > 0) frame_times_ms.reserve(static_cast<size_t>(smoke_frames));

    if (!headless && g_winmm.ready) g_winmm.time_begin_period(1);
    LARGE_INTEGER qpc_freq{}, qpc_next{};
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_next);

    uint32_t last_scsp_seq = app.m->scsp_event_seq;
    bool smoke_save_restore_ok = true;
    bool smoke_replay_ok = true;
    std::string smoke_snap_sha;
    std::string smoke_end_frame_sha;
    uint32_t smoke_snap_bytes = 0u;
    std::vector<uint8_t> smoke_restore_snap;
    std::vector<ReplayFramePorts> smoke_replay_log;
    uint8_t smoke_replay_ref_fr[32]{};

    auto t_fps0 = std::chrono::steady_clock::now();
    uint32_t fps_frames = 0u;

    while (app.running) {
        if (!headless && g_ui.ready) {
            MSG msg{};
            while (g_ui.peek_message_a(&msg, nullptr, 0, 0, PM_REMOVE)) {
                g_ui.translate_message(&msg);
                g_ui.dispatch_message_a(&msg);
            }
            if (!app.running) break;
        }

        if (smoke_frames > 0 && static_cast<int>(app.frames_run) >= smoke_frames) {
            break;
        }

        if (app.save_req) {
            app.save_req = false;
            std::filesystem::create_directories(app.validation_dir);
            const uint32_t len = app.m->save_state_bytes(save_buf.data(), save_buf.size());
            const auto sp = slot_save_path(app);
            if (len > 0u && write_bytes(sp, save_buf.data(), len)) {
                uint8_t raw[32]{};
                twc98::sha256_bytes(save_buf.data(), len, raw);
                char hex[17]{};
                for (int i = 0; i < 8; ++i) std::snprintf(hex + i * 2, 3, "%02x", raw[i]);
                app.status_banner = std::string("SAVED S") + std::to_string(app.slot_idx) + " " + hex;
            }
        }
        if (app.load_req) {
            app.load_req = false;
            std::vector<uint8_t> loaded;
            const char* lerr = nullptr;
            if (read_bytes(slot_save_path(app), loaded) &&
                app.m->load_state_bytes(loaded.data(), loaded.size(), &lerr)) {
                app.m->render_frame();
                last_scsp_seq = app.m->scsp_event_seq;
                app.status_banner = std::string("LOADED S") + std::to_string(app.slot_idx);
            } else {
                app.status_banner = "NO SNAPSHOT IN SLOT";
            }
        }
        if (app.rec_toggle_req) {
            app.rec_toggle_req = false;
            if (!app.replay.recording) {
                const uint32_t len = app.m->save_state_bytes(save_buf.data(), save_buf.size());
                if (len > 0u) {
                    app.replay.playing = false;
                    app.replay.recording = true;
                    app.replay.init_snapshot.assign(save_buf.data(), save_buf.data() + len);
                    app.replay.frames.clear();
                    app.replay.play_cursor = 0u;
                    app.status_banner = "REC STARTED";
                }
            } else {
                app.replay.recording = false;
                app.replay.save_to_file(slot_replay_path(app));
                app.status_banner = "REC SAVED (" + std::to_string(app.replay.frames.size()) + "F)";
            }
        }
        if (app.play_toggle_req) {
            app.play_toggle_req = false;
            if (app.replay.playing) {
                app.replay.playing = false;
                app.status_banner = "REPLAY STOPPED";
            } else {
                app.replay.recording = false;
                if (app.replay.init_snapshot.empty() || app.replay.frames.empty()) {
                    app.replay.load_from_file(slot_replay_path(app));
                }
                const char* lerr = nullptr;
                if (!app.replay.init_snapshot.empty() && !app.replay.frames.empty() &&
                    app.m->load_state_bytes(app.replay.init_snapshot.data(), app.replay.init_snapshot.size(), &lerr)) {
                    app.m->render_frame();
                    last_scsp_seq = app.m->scsp_event_seq;
                    app.replay.playing = true;
                    app.replay.play_cursor = 0u;
                    app.status_banner = "REPLAYING";
                } else {
                    app.status_banner = "NO REPLAY DATA";
                }
            }
        }
        if (app.quick_match_req) {
            app.quick_match_req = false;
            app.quick_match_rem = 150u;
            app.status_banner = "MACRO:QUICKMATCH";
        }
        if (app.mem_dump_req) {
            app.mem_dump_req = false;
            std::filesystem::create_directories(app.validation_dir);
            const bool ok1 = write_bytes(app.validation_dir / "high_wram_dump.bin",
                                         app.m->inner.high_wram.data(), twc98::kHighWramSize);
            const bool ok2 = write_bytes(app.validation_dir / "sound_ram_dump.bin",
                                         app.m->sound_ram, twc98::StvMachineState::kSoundRamSize);
            app.status_banner = (ok1 && ok2) ? "DUMPED HWRAM+SND" : "DUMP FAILED";
        }
        if (app.full_restart_req) {
            app.full_restart_req = false;
            app.replay.recording = false;
            app.replay.playing = false;
            app.quick_match_rem = 0u;
            app.paused = false;
            app.frames_run = 0u;
            app.need_bg_clear = true;
            const char* rerr = nullptr;
            if (!boot_mode_clean(app, bios, cart, app.game_mode, 820, &rerr)) {
                app.status_banner = std::string("REBOOT FAIL:") + (rerr ? rerr : "?");
            } else {
                last_scsp_seq = app.m->scsp_event_seq;
                app.status_banner = (app.game_mode == twc98::copa::GameMode::CopaAmerica)
                    ? "REBOOT:COPA AMERICA"
                    : "REBOOT:CLASSIC WC98";
            }
        }

        const bool run_this_iter = !app.mode_select_open && (!app.paused || app.step_one_req);
        if (app.step_one_req) {
            app.paused = true;
            app.step_one_req = false;
        }

        if (run_this_iter) {
            uint8_t port_a = 0xFFu, port_b = 0xFFu, port_c = 0xFFu;
            if (smoke_frames > 0) {
                if (app.frames_run >= 120u && app.frames_run <= 125u) port_c = 0xFEu;
                else if (app.frames_run >= 145u && app.frames_run <= 150u) port_c = 0xEFu;
                else if (app.frames_run >= 220u && app.frames_run <= 225u) port_c = 0xDFu;
                if (app.frames_run >= 230u && app.frames_run <= 270u) port_a = 0xDEu;
            } else if (app.replay.playing && app.replay.play_cursor < app.replay.frames.size()) {
                const auto& rf = app.replay.frames[app.replay.play_cursor++];
                port_a = rf.a;
                port_b = rf.b;
                port_c = rf.c;
                if (app.replay.play_cursor >= app.replay.frames.size()) {
                    app.replay.playing = false;
                    app.status_banner = "REPLAY END";
                }
            } else {
                sample_live_inputs(app, xinput_fn, port_a, port_b, port_c);
                if (app.quick_match_rem > 0u) {
                    const uint32_t step = 150u - app.quick_match_rem;
                    if (step >= 4u && step <= 10u) port_c &= static_cast<uint8_t>(~0x01u);      // Coin1
                    else if (step >= 25u && step <= 32u) port_c &= static_cast<uint8_t>(~0x10u); // 1P Start
                    else if (step >= 70u && step <= 78u) port_a &= static_cast<uint8_t>(~0x01u); // P1 Btn1 Confirm
                    else if (step >= 110u && step <= 118u) port_a &= static_cast<uint8_t>(~0x01u);
                    --app.quick_match_rem;
                }
                if (app.replay.recording) {
                    app.replay.frames.push_back(ReplayFramePorts{port_a, port_b, port_c, 0u});
                }
            }
            app.last_pa = port_a;
            app.last_pb = port_b;
            app.last_pc = port_c;

            const auto tf0 = std::chrono::steady_clock::now();
            if (udp_net.active) {
                udp_net.poll_into(net_session);
                const uint8_t loc_btn = (net_session.local_player() == 0u) ? port_a : port_b;
                twc98::NetInputPacket out_pkt{};
                net_session.queue_local_input(loc_btn, port_c, out_pkt);
                udp_net.send_pkt(out_pkt);
                if (!net_session.step_frame(*app.m, net_rollback_arena.data(), net_rollback_arena.size())) {
                    if (app.m->halted) {
                        std::fprintf(stderr, "HALT CPU=%d reason=%s pc=0x%08x addr=0x%08x\n",
                                     app.m->halt_cpu,
                                     app.m->last_fault.reason ? app.m->last_fault.reason : "?",
                                     app.m->last_fault.pc, app.m->last_fault.address);
                        break;
                    }
                }
                if (app.copa_backup) {
                    twc98::copa::on_frame_tick(*app.m, *app.copa_backup);
                }
            } else {
                app.m->set_input_ports(port_a, port_b, port_c, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                if (!app.m->run_rounds(10000u)) {
                    std::fprintf(stderr, "HALT CPU=%d reason=%s pc=0x%08x addr=0x%08x\n",
                                 app.m->halt_cpu,
                                 app.m->last_fault.reason ? app.m->last_fault.reason : "?",
                                 app.m->last_fault.pc, app.m->last_fault.address);
                    break;
                }
                if (app.copa_backup) {
                    twc98::copa::on_frame_tick(*app.m, *app.copa_backup);
                }
                g_vdp_pool.render_frame(*app.m);
            }
            if (!headless && smoke_frames == 0 && app.game_mode == twc98::copa::GameMode::CopaAmerica &&
                app.m->copa_returned_to_title) {
                // Full return to initial cold-booted Title + Edition-select screen once Copa America ends
                app.m->copa_returned_to_title = false;
                app.m->copa_entered_gameplay = false;
                app.replay.recording = false;
                app.replay.playing = false;
                app.quick_match_rem = 0u;
                const char* rerr = nullptr;
                if (boot_mode_clean(app, bios, cart, app.game_mode, 820, &rerr)) {
                    last_scsp_seq = app.m->scsp_event_seq;
                }
                app.mode_select_cursor = (app.game_mode == twc98::copa::GameMode::CopaAmerica) ? 1 : 0;
                app.pending_full_restart = true;
                app.mode_select_open = true;
                app.need_bg_clear = true;
                app.status_banner = "SELECT EDITION";
            }
            const auto tf1 = std::chrono::steady_clock::now();
            app.cur_ms = std::chrono::duration<double, std::milli>(tf1 - tf0).count();
            if (smoke_frames > 0) frame_times_ms.push_back(app.cur_ms);

            const uint32_t cur_seq = app.m->scsp_event_seq;
            if (cur_seq > last_scsp_seq) {
                const uint32_t start = (cur_seq - last_scsp_seq > twc98::StvMachineState::kMaxScspEvents)
                    ? (cur_seq - twc98::StvMachineState::kMaxScspEvents + 1u)
                    : (last_scsp_seq + 1u);
                for (uint32_t s = start; s <= cur_seq; ++s) {
                    const auto& ev = app.m->scsp_events[(s - 1u) % twc98::StvMachineState::kMaxScspEvents];
                    if (ev.seq == s) audio.trigger_event(ev);
                }
                last_scsp_seq = cur_seq;
            }

            // Deterministic save-restore & replay verification during smoke
            if (smoke_frames > 0 && app.frames_run == 180u) {
                const uint32_t slen = app.m->save_state_bytes(save_buf.data(), save_buf.size());
                if (slen > 0u) {
                    smoke_snap_bytes = slen;
                    smoke_restore_snap.assign(save_buf.data(), save_buf.data() + slen);
                    uint8_t sraw[32]{};
                    twc98::sha256_bytes(save_buf.data(), slen, sraw);
                    char hex[65]{};
                    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", sraw[i]);
                    smoke_snap_sha = hex;
                } else {
                    smoke_save_restore_ok = false;
                }
            }
            if (smoke_frames > 0 && app.frames_run == 200u && !smoke_restore_snap.empty()) {
                uint8_t hw[32]{}, lw[32]{}, v1[32]{}, v2[32]{}, c2[32]{}, r2[32]{}, fr200_a[32]{};
                app.m->compute_digests(hw, lw, v1, v2, c2, r2, fr200_a);
                const char* lerr = nullptr;
                if (!app.m->load_state_bytes(smoke_restore_snap.data(), smoke_restore_snap.size(), &lerr)) {
                    smoke_save_restore_ok = false;
                } else {
                    for (int rf = 181; rf <= 200; ++rf) {
                        app.m->set_input_ports(0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                        app.m->run_rounds(10000u);
                    }
                    app.m->render_frame();
                    uint8_t fr200_b[32]{};
                    app.m->compute_digests(hw, lw, v1, v2, c2, r2, fr200_b);
                    if (std::memcmp(fr200_a, fr200_b, 32u) != 0) smoke_save_restore_ok = false;
                }
                smoke_replay_log.clear();
            }
            if (smoke_frames > 0 && app.frames_run > 200u && app.frames_run <= 260u) {
                smoke_replay_log.push_back(ReplayFramePorts{port_a, port_b, port_c, 0u});
                if (app.frames_run == 260u) {
                    uint8_t hw[32]{}, lw[32]{}, v1[32]{}, v2[32]{}, c2[32]{}, r2[32]{};
                    app.m->compute_digests(hw, lw, v1, v2, c2, r2, smoke_replay_ref_fr);
                    const uint32_t sl260 = app.m->save_state_bytes(save_buf.data(), save_buf.size());
                    std::vector<uint8_t> end260(save_buf.data(), save_buf.data() + sl260);
                    // Wait: restore f=180 -> forward 181..200 neutral -> replay 201..260 -> check exact match at f=260!
                    const char* lerr = nullptr;
                    if (!app.m->load_state_bytes(smoke_restore_snap.data(), smoke_restore_snap.size(), &lerr)) {
                        smoke_replay_ok = false;
                    } else {
                        for (int rf = 181; rf <= 200; ++rf) {
                            app.m->set_input_ports(0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                            app.m->run_rounds(10000u);
                        }
                        for (const auto& rfp : smoke_replay_log) {
                            app.m->set_input_ports(rfp.a, rfp.b, rfp.c, 0xFFu, 0xFFu, 0x7Fu, 0x67u);
                            app.m->run_rounds(10000u);
                        }
                        app.m->render_frame();
                        uint8_t fr260_b[32]{};
                        app.m->compute_digests(hw, lw, v1, v2, c2, r2, fr260_b);
                        if (std::memcmp(smoke_replay_ref_fr, fr260_b, 32u) != 0) smoke_replay_ok = false;
                    }
                    app.m->load_state_bytes(end260.data(), end260.size(), &lerr);
                }
            }

            ++app.frames_run;
            ++fps_frames;
        }

        if (!headless && hwnd) {
            blit_hwnd(hwnd, app);
            audio.pump(app.m.get(), app.audio_muted, app.audio_volume);

            const auto now = std::chrono::steady_clock::now();
            const double fps_ms = std::chrono::duration<double, std::milli>(now - t_fps0).count();
            if (fps_ms >= 350.0 && g_ui.ready) {
                app.cur_fps = static_cast<double>(fps_frames) * 1000.0 / fps_ms;
                char title[256]{};
                std::snprintf(title, sizeof(title),
                              "TWC98BO PC | %s %dx%d [%s/%s%s%s] | %.1f fps | Spd:%d%% | Menu [Esc] | Dev:%s [1] | F:%u",
                              (app.use_gpu && app.gpu.ready) ? "GPU" : "CPU",
                              app.vdp->frame_w, app.vdp->frame_h,
                              kAspectNames[app.aspect_mode],
                              twc98_pc_gpu::kFilterNames[app.filter_mode],
                              (app.crt_level > 0) ? (app.crt_level == 1 ? "+CRT1" : "+CRT2") : "",
                              (app.m->dbg_hide_layers & 0x08u) ? "+HD" : "",
                              app.cur_fps, kSpeedPercents[app.speed_idx],
                              app.debug_open ? "ON" : "OFF",
                              app.frames_run);
                g_ui.set_window_text_a(hwnd, title);
                t_fps0 = now;
                fps_frames = 0u;
            }
        }

        if (!headless && smoke_frames == 0) {
            const double target_hz = 60.0 * static_cast<double>(kSpeedPercents[app.speed_idx]) / 100.0;
            double frame_ticks = static_cast<double>(qpc_freq.QuadPart) / target_hz;
            if (udp_net.active) {
                const int32_t rift_us = net_session.recommended_usec_delta();
                if (rift_us != 0) {
                    frame_ticks += (static_cast<double>(rift_us) / 1000000.0) * static_cast<double>(qpc_freq.QuadPart);
                }
            }
            LARGE_INTEGER cur{};
            QueryPerformanceCounter(&cur);
            const double rem_ms = static_cast<double>(qpc_next.QuadPart + static_cast<LONGLONG>(frame_ticks) - cur.QuadPart) * 1000.0 / static_cast<double>(qpc_freq.QuadPart);
            if (rem_ms > 2.0) Sleep(static_cast<DWORD>(rem_ms - 1.0));
            do {
                QueryPerformanceCounter(&cur);
            } while (static_cast<double>(cur.QuadPart) < static_cast<double>(qpc_next.QuadPart) + frame_ticks);
            qpc_next.QuadPart += static_cast<LONGLONG>(frame_ticks);
            if (static_cast<double>(cur.QuadPart - qpc_next.QuadPart) > frame_ticks * 4.0) {
                qpc_next = cur;
            }
        } else {
            QueryPerformanceCounter(&qpc_next);
        }
    }

    udp_net.shutdown();
    if (!headless && g_winmm.ready) g_winmm.time_end_period(1);
    audio.shutdown();
    app.gpu.shutdown();
    app.dbg_ui.shutdown();
    if (!headless && hwnd && g_ui.ready) g_ui.destroy_window(hwnd);

    if (smoke_frames > 0) {
        if (!smoke_ppm.empty()) {
            std::filesystem::create_directories(smoke_ppm.parent_path());
            write_ppm(smoke_ppm, *app.vdp);
        }
        uint8_t hw[32]{}, lw[32]{}, v1[32]{}, v2[32]{}, c2[32]{}, r2[32]{}, fr[32]{};
        app.m->compute_digests(hw, lw, v1, v2, c2, r2, fr);
        char fhex[65]{};
        for (int i = 0; i < 32; ++i) std::snprintf(fhex + i * 2, 3, "%02x", fr[i]);
        smoke_end_frame_sha = fhex;

        std::sort(frame_times_ms.begin(), frame_times_ms.end());
        double sum_ms = 0.0;
        for (double v : frame_times_ms) sum_ms += v;
        const double avg_ms = frame_times_ms.empty() ? 0.0 : (sum_ms / static_cast<double>(frame_times_ms.size()));
        const double p50_ms = frame_times_ms.empty() ? 0.0 : frame_times_ms[frame_times_ms.size() / 2];
        const double p95_ms = frame_times_ms.empty() ? 0.0 : frame_times_ms[(frame_times_ms.size() * 95) / 100];
        const double fps = avg_ms > 0.0 ? (1000.0 / avg_ms) : 0.0;
        const uint64_t rss = query_working_set_bytes();

        const bool all_ok = smoke_save_restore_ok && smoke_replay_ok && !app.m->halted;
        std::ostringstream oss;
        oss << "{\n"
            << "  \"schema\": \"g4_pc_host_smoke_v2\",\n"
            << "  \"status\": \"" << (all_ok ? "PASS_PC_HOST_SMOKE" : "FAIL") << "\",\n"
            << "  \"frames_run\": " << app.frames_run << ",\n"
            << "  \"frame_w\": " << app.vdp->frame_w << ",\n"
            << "  \"frame_h\": " << app.vdp->frame_h << ",\n"
            << "  \"cold_init_ms\": " << cold_init_ms << ",\n"
            << "  \"avg_ms_per_frame\": " << avg_ms << ",\n"
            << "  \"p50_ms_per_frame\": " << p50_ms << ",\n"
            << "  \"p95_ms_per_frame\": " << p95_ms << ",\n"
            << "  \"sustained_fps\": " << fps << ",\n"
            << "  \"realtime_ratio_60fps\": " << (fps / 60.0) << ",\n"
            << "  \"peak_rss_bytes\": " << rss << ",\n"
            << "  \"save_restore_rewind_bit_exact_ok\": " << (smoke_save_restore_ok ? "true" : "false") << ",\n"
            << "  \"replay_roundtrip_bit_exact_ok\": " << (smoke_replay_ok ? "true" : "false") << ",\n"
            << "  \"snapshot_bytes\": " << smoke_snap_bytes << ",\n"
            << "  \"snapshot_sha256\": \"" << smoke_snap_sha << "\",\n"
            << "  \"ioga_reads\": " << app.m->ioga_reads << ",\n"
            << "  \"cart_reads\": " << app.m->cart_reads << ",\n"
            << "  \"soundram_writes\": " << app.m->soundram_writes << ",\n"
            << "  \"scsp_events\": " << app.m->scsp_event_seq << ",\n"
            << "  \"end_frame_sha256\": \"" << smoke_end_frame_sha << "\"\n"
            << "}\n";
        if (!smoke_out.empty()) {
            std::filesystem::create_directories(smoke_out.parent_path());
            std::ofstream ofs(smoke_out, std::ios::binary);
            ofs << oss.str();
        }
        std::printf("%s", oss.str().c_str());
        g_vdp_pool.shutdown();
        return all_ok ? 0 : 4;
    }

    g_vdp_pool.shutdown();
    return 0;
}
