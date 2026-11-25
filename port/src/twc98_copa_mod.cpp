#include "twc98_copa_mod.hpp"
#include "stv_machine.hpp"

#include <algorithm>
#ifndef TWC98_NO_STL
#include <chrono>
#endif
#include <cstring>

namespace twc98 {
namespace copa {
namespace {

static inline void write_be16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[1] = static_cast<uint8_t>(v & 0xFFu);
}

static inline void write_be32(uint8_t* dst, uint32_t v) {
    write_be16(dst, static_cast<uint16_t>((v >> 16) & 0xFFFFu));
    write_be16(dst + 2, static_cast<uint16_t>(v & 0xFFFFu));
}

static inline uint16_t read_be16(const uint8_t* src) {
    return static_cast<uint16_t>((static_cast<uint16_t>(src[0]) << 8) |
                                 static_cast<uint16_t>(src[1]));
}

static inline uint32_t read_be32(const uint8_t* src) {
    return (static_cast<uint32_t>(read_be16(src)) << 16) |
           static_cast<uint32_t>(read_be16(src + 2));
}

static inline int32_t read_be_s32(const uint8_t* src) {
    return static_cast<int32_t>(read_be32(src));
}

static uint32_t isqrt64(uint64_t x) {
    if (x == 0u) return 0u;
    uint64_t r = x;
    uint64_t b = 1ULL << 62;
    while (b > x) b >>= 2;
    uint64_t q = 0u;
    while (b != 0u) {
        if (r >= q + b) {
            r -= q + b;
            q = (q >> 1) + b;
        } else {
            q >>= 1;
        }
        b >>= 2;
    }
    return static_cast<uint32_t>(q);
}

static inline uint16_t rgb888_to_bgr555(uint8_t r, uint8_t g, uint8_t b, bool visible = true) {
    const uint16_t r5 = static_cast<uint16_t>((r >> 3) & 0x1Fu);
    const uint16_t g5 = static_cast<uint16_t>((g >> 3) & 0x1Fu);
    const uint16_t b5 = static_cast<uint16_t>((b >> 3) & 0x1Fu);
    return static_cast<uint16_t>((visible ? 0x8000u : 0x0000u) | (b5 << 10) | (g5 << 5) | r5);
}

static constexpr uint8_t kModSlots[CopaModBackup::kModSlotCount] = {1u, 5u, 6u, 7u, 11u, 13u, 14u};
static constexpr uint8_t kHiddenSlots[4] = {2u, 4u, 8u, 9u};
static constexpr uint8_t kHiddenMsgSlots[CopaModBackup::kHiddenMsgCount] = {
    2u, 4u, 8u, 9u, 16u, 17u, 18u
};
// CEL_OUT flag palette bank is 0..12. VEN shares ECU's yellow/blue/red ramps;
// palette 13 belongs to other UI data and is not loaded for the flag grid.
static constexpr uint8_t kCelPalIds[CopaModBackup::kModSlotCount] = {3u, 6u, 7u, 7u, 8u, 10u, 11u};

struct BgTmSlotMeta {
    uint32_t off;
    uint32_t size;
};
static constexpr BgTmSlotMeta kBgTmMeta[CopaModBackup::kModSlotCount] = {
    {TeamAssetOffsets::kBgTm01Off, TeamAssetOffsets::kBgTm01Size}, // 1: PER
    {TeamAssetOffsets::kBgTm05Off, TeamAssetOffsets::kBgTm05Size}, // 5: CHI
    {TeamAssetOffsets::kBgTm06Off, TeamAssetOffsets::kBgTm06Size}, // 6: ECU
    {TeamAssetOffsets::kBgTm07Off, TeamAssetOffsets::kBgTm07Size}, // 7: VEN
    {TeamAssetOffsets::kBgTm11Off, TeamAssetOffsets::kBgTm11Size}, // 11: URU
    {TeamAssetOffsets::kBgTm13Off, TeamAssetOffsets::kBgTm13Size}, // 13: PAR
    {TeamAssetOffsets::kBgTm14Off, TeamAssetOffsets::kBgTm14Size}, // 14: BOL
};

static constexpr uint8_t kActiveCopaSlots[CopaTournamentState::kActiveTeamCount] = {
    0u, 1u, 3u, 5u, 6u, 7u, 10u, 11u, 12u, 13u, 14u, 15u
};
// Gapless 3-group x 4-team Team Select screen coordinates (8px cell units; 12x4 cells/flag)
static constexpr uint8_t kCopaGroupCellX[CopaTournamentState::kTeamsPerGroup] = {0x13u, 0x20u, 0x2Du, 0x3Au};
static constexpr uint8_t kCopaGroupCellY[CopaTournamentState::kGroupCount]    = {0x24u, 0x29u, 0x2Eu};

// 400 verified pure CEL_OUT.DAT tile IDs exclusively from replaced/hidden flag slots (never overlapping
// kept teams {0, 3, 10, 12, 15} or any non-flag UI/trophy tile in MSG_OUT.DAT).
static constexpr uint16_t kSafeCelOutTilePool[CopaModBackup::kSafeCelPoolSize] = {
    317u, 335u, 551u, 552u, 554u, 555u, 556u, 557u, 559u, 560u, 562u, 563u, 564u, 565u, 566u, 577u,
    578u, 579u, 580u, 581u, 595u, 596u, 598u, 599u, 600u, 601u, 602u, 603u, 604u, 605u, 606u, 607u,
    608u, 609u, 623u, 624u, 625u, 626u, 628u, 629u, 630u, 643u, 644u, 645u, 646u, 647u, 648u, 649u,
    650u, 651u, 652u, 653u, 654u, 655u, 667u, 669u, 670u, 671u, 672u, 673u, 686u, 687u, 688u, 689u,
    690u, 691u, 692u, 694u, 695u, 696u, 697u, 698u, 699u, 700u, 701u, 702u, 703u, 705u, 720u, 721u,
    722u, 723u, 724u, 725u, 727u, 728u, 729u, 730u, 731u, 732u, 733u, 734u, 735u, 736u, 737u, 738u,
    739u, 740u, 741u, 742u, 743u, 744u, 745u, 746u, 747u, 749u, 750u, 751u, 752u, 753u, 754u, 755u,
    756u, 757u, 758u, 761u, 762u, 763u, 764u, 765u, 766u, 767u, 768u, 769u, 771u, 773u, 774u, 775u,
    776u, 777u, 778u, 779u, 780u, 781u, 782u, 783u, 784u, 785u, 786u, 787u, 788u, 789u, 790u, 791u,
    793u, 794u, 795u, 796u, 797u, 798u, 799u, 800u, 801u, 803u, 804u, 805u, 806u, 807u, 808u, 809u,
    810u, 811u, 812u, 813u, 815u, 816u, 817u, 818u, 819u, 820u, 821u, 822u, 823u, 824u, 827u, 828u,
    829u, 830u, 832u, 833u, 834u, 835u, 836u, 837u, 839u, 840u, 841u, 842u, 843u, 844u, 845u, 846u,
    847u, 848u, 849u, 851u, 852u, 853u, 854u, 855u, 856u, 857u, 859u, 861u, 864u, 865u, 866u, 867u,
    868u, 869u, 870u, 871u, 872u, 873u, 875u, 876u, 877u, 878u, 879u, 880u, 881u, 882u, 883u, 884u,
    885u, 887u, 888u, 889u, 890u, 891u, 892u, 893u, 894u, 895u, 896u, 897u, 899u, 900u, 901u, 911u,
    912u, 913u, 914u, 915u, 917u, 925u, 926u, 927u, 928u, 929u, 930u, 931u, 932u, 933u, 935u, 936u,
    937u, 938u, 939u, 940u, 941u, 942u, 943u, 953u, 954u, 955u, 956u, 957u, 959u, 960u, 961u, 968u,
    969u, 971u, 972u, 973u, 974u, 975u, 976u, 977u, 978u, 979u, 980u, 981u, 983u, 984u, 985u, 986u,
    987u, 988u, 989u, 999u, 1001u, 1002u, 1003u, 1005u, 1007u, 1008u, 1015u, 1016u, 1017u, 1019u, 1021u, 1022u,
    1023u, 1025u, 1027u, 1028u, 1029u, 1031u, 1032u, 1043u, 1044u, 1045u, 1046u, 1047u, 1049u, 1050u, 1051u, 1061u,
    1062u, 1063u, 1064u, 1065u, 1067u, 1069u, 1070u, 1071u, 1073u, 1074u, 1920u, 1921u, 1922u, 1923u, 1924u, 1925u,
    1926u, 1927u, 1928u, 1929u, 1931u, 1932u, 1933u, 1934u, 1935u, 1936u, 1937u, 1938u, 1939u, 1940u, 1941u, 1943u,
    1944u, 1945u, 1946u, 1947u, 1948u, 1949u, 1950u, 1951u, 1952u, 1953u, 1955u, 1956u, 1957u, 1958u, 1959u, 1960u,
    1961u, 1962u, 1963u, 1964u, 1965u, 1967u, 1968u, 1969u, 1970u, 1971u, 1972u, 1973u, 1974u, 1975u, 1976u, 1977u,
    1979u, 1980u, 1981u, 1982u, 1983u, 1984u, 1985u, 1986u, 1987u, 1988u, 1989u, 1990u, 1991u, 1992u, 1993u, 1994u,
};

// Complete roster of all 16 slots under Copa America mode:
//   12 active teams (even tier distribution: T0=3, T1=3, T2=3, T3=3 -> 3 groups of 4 teams)
//   4 hidden teams {2, 4, 8, 9} -> tier=0xFF, grid=0xFF, badge vis=0
static constexpr std::array<CopaTeamSpec, TeamAssetOffsets::kTeamCount> kTeamSpecs = {{
    { 0u, true,  "BRA", { 4u, 26u, 28u}, 3u   }, // Brazil (kept)
    { 1u, true,  "PER", {14u, 24u,  0u}, 1u   }, // Peru (replaces Japan): Technical Swirl Shot + Super Combination
    { 2u, false, "ENG", { 0u,  0u,  0u}, 0xFFu}, // Hidden
    { 3u, true,  "ARG", {31u,  2u,  0u}, 3u   }, // Argentina (kept)
    { 4u, false, "ITA", { 0u,  0u,  0u}, 0xFFu}, // Hidden
    { 5u, true,  "CHI", { 8u, 17u,  0u}, 2u   }, // Chile (replaces France): Spinning Drive Shot + Pressing Slide
    { 6u, true,  "ECU", { 5u, 30u,  0u}, 2u   }, // Ecuador (replaces Spain): Thunder Power Shot + Wing Burst
    { 7u, true,  "VEN", {12u, 19u,  0u}, 0u   }, // Venezuela (replaces Korea): Vinotinto Swerve Shot + Quick Finta
    { 8u, false, "NED", { 0u,  0u,  0u}, 0xFFu}, // Hidden
    { 9u, false, "NGA", { 0u,  0u,  0u}, 0xFFu}, // Hidden
    {10u, true,  "MEX", {15u, 21u,  0u}, 1u   }, // Mexico (kept)
    {11u, true,  "URU", { 1u, 20u,  0u}, 3u   }, // Uruguay (replaces Germany): Cannon Shot + Panzer Tackle (Garra Charrua)
    {12u, true,  "COL", { 9u, 27u,  0u}, 1u   }, // Colombia (kept)
    {13u, true,  "PAR", {10u, 23u,  0u}, 2u   }, // Paraguay (replaces Cameroon): Diagonal Shot + Super GK
    {14u, true,  "BOL", { 3u, 22u,  0u}, 0u   }, // Bolivia (replaces Yugoslavia): Condor Wind Shot + Super GK ONLY
    {15u, true,  "USA", {13u, 18u,  0u}, 0u   }, // USA (kept)
}};

static constexpr uint8_t kCopaTiers[TeamAssetOffsets::kTierTblBytes] = {
    3u, 1u, 0xFFu, 3u, 0xFFu, 2u, 2u, 0u,
    0xFFu, 0xFFu, 1u, 3u, 1u, 2u, 0u, 0u,
};

// ---------------------------------------------------------------------------
// Compact 5x7 uppercase font (used by BG_TM banners, HUD codes & CEL_OUT flags)
// ---------------------------------------------------------------------------
struct Glyph5x7 {
    char ch;
    uint8_t rows[7];
};
static constexpr Glyph5x7 kFont5x7[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
};

static const uint8_t* find_glyph_5x7(char c) {
    for (const auto& g : kFont5x7) {
        if (g.ch == c) return g.rows;
    }
    return nullptr;
}

static bool point_in_star5(int dx, int dy, int outer_r, int inner_r) {
    // Crisp symmetric upright 5-point star test using 10 triangular wedges in integer math
    const int d2 = dx * dx + dy * dy;
    if (d2 > outer_r * outer_r) return false;
    if (d2 <= inner_r * inner_r) return true;
    // Approximate 5-lobe polar modulation without <cmath>
    // Vertices at -90, -18, 54, 126, 198 deg -> max |proj| along 5 unit axes
    // Axes rotated by 72 deg (fixed-point *256):
    static constexpr int kx[5] = {  0,  243,  150, -150, -243};
    static constexpr int ky[5] = {-256, -79,  207,  207,  -79};
    int best_dot = 0;
    for (int i = 0; i < 5; ++i) {
        const int dot = dx * kx[i] + dy * ky[i];
        if (dot > best_dot) best_dot = dot;
    }
    // High projection toward one of the 5 tips relative to distance -> star arm
    const int perp2 = d2 * 65536 - best_dot * best_dot;
    return best_dot > 0 && perp2 <= (best_dot * best_dot) / 9;
}

// ---------------------------------------------------------------------------
// 1. Large 240x160 8bpp Flag Builder: T_FLAG01/05/06/11/13/14
// ---------------------------------------------------------------------------
struct Rgb { uint8_t r, g, b; };

static void set_ramp(uint8_t* pal, int base_idx, Rgb hi, Rgb lo) {
    for (int i = 0; i < 6; ++i) {
        const uint8_t r = static_cast<uint8_t>(hi.r + ((static_cast<int>(lo.r) - hi.r) * i) / 5);
        const uint8_t g = static_cast<uint8_t>(hi.g + ((static_cast<int>(lo.g) - hi.g) * i) / 5);
        const uint8_t b = static_cast<uint8_t>(hi.b + ((static_cast<int>(lo.b) - hi.b) * i) / 5);
        write_be16(pal + static_cast<size_t>(base_idx + i) * 2u, rgb888_to_bgr555(r, g, b));
    }
}

static void draw_oval_crest(uint8_t* pix, int w, int h, int cx, int cy,
                            uint8_t top_col, uint8_t bot_col, uint8_t condor_wings) {
    for (int y = std::max(0, cy - 17); y <= std::min(h - 1, cy + 16); ++y) {
        for (int x = std::max(0, cx - 20); x <= std::min(w - 1, cx + 20); ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const int ell = dx * dx * 256 + dy * dy * 400;
            if (ell <= 20 * 20 * 256) {
                uint8_t c = 24u;
                if (ell > 17 * 17 * 256) {
                    c = (ell > 19 * 19 * 256) ? 24u : 20u;
                } else if (ell > 14 * 14 * 256) {
                    c = (dy < -4) ? 2u : (dy > 4 ? bot_col : 20u);
                } else {
                    c = (dy < 2) ? 23u : top_col;
                    if (dx * dx + (dy + 3) * (dy + 3) <= 12) c = 19u;
                    if (dx * dx + (dy + 4) * (dy + 4) <= 5) c = 20u;
                }
                pix[y * w + x] = c;
            }
            if (condor_wings && y >= cy - 15 && y <= cy - 9 && dx >= -13 && dx <= 13) {
                const int ay = y - (cy - 12);
                const int adx = (dx < 0) ? -dx : dx;
                if (adx >= 2 && adx <= 12 && (ay >= -1 - adx / 6 && ay <= 1)) {
                    pix[y * w + x] = (ay <= 0) ? 22u : 21u;
                } else if (adx <= 1 && y >= cy - 15 && y <= cy - 11) {
                    pix[y * w + x] = 19u;
                }
            }
        }
    }
}

static void build_tflag(uint8_t slot, uint8_t* buf, size_t len) {
    if (len != TeamAssetOffsets::kTFlagSize) return;
    uint8_t* pix = buf + 20u;
    uint8_t* pal = buf + 20u + 240u * 160u;
    std::memset(pal, 0, 512u);

    // Common accent colors 19..24
    write_be16(pal + 0u * 2u,  rgb888_to_bgr555(24, 24, 32));
    write_be16(pal + 19u * 2u, rgb888_to_bgr555(248, 248, 248)); // White
    write_be16(pal + 20u * 2u, rgb888_to_bgr555(248, 204, 48));  // Bright Gold
    write_be16(pal + 21u * 2u, rgb888_to_bgr555(168, 120, 24));  // Deep Gold / Trim
    write_be16(pal + 22u * 2u, rgb888_to_bgr555(104, 72, 48));   // Condor Brown
    write_be16(pal + 23u * 2u, rgb888_to_bgr555(56, 136, 208));  // Crest sky accent
    write_be16(pal + 24u * 2u, rgb888_to_bgr555(40, 44, 56));    // Outline

    auto sheen = [](int x, int y) -> uint8_t {
        const int wave = ((x / 20 + y / 40) & 3);
        return static_cast<uint8_t>(wave == 3 ? 1 : wave);
    };

    if (slot == 14u) {
        // BOLIVIA: Crimson Red / Gold Yellow / DARK FOREST GREEN + Escudo crest
        set_ramp(pal, 1,  {228, 40, 36},  {172, 16, 16});
        set_ramp(pal, 7,  {248, 224, 24}, {192, 168, 8});
        set_ramp(pal, 13, {8, 118, 36},   {0, 56, 16}); // Verde mas oscuro!
        for (int y = 0; y < 160; ++y) {
            const uint8_t base = (y < 53) ? 1u : (y < 107) ? 7u : 13u;
            for (int x = 0; x < 240; ++x) pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
        }
        draw_oval_crest(pix, 240, 160, 120, 80, 15u, 14u, 1u);
    } else if (slot == 1u) {
        // PERU: Vertical Crimson Red | White | Crimson Red + central laurel wreath
        set_ramp(pal, 1,  {224, 24, 32},  {160, 12, 16});
        set_ramp(pal, 7,  {248, 248, 248},{208, 212, 220});
        set_ramp(pal, 13, {16, 136, 56},  {8, 88, 32});
        for (int y = 0; y < 160; ++y) {
            for (int x = 0; x < 240; ++x) {
                const uint8_t base = (x < 80 || x >= 160) ? 1u : 7u;
                pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
            }
        }
        draw_oval_crest(pix, 240, 160, 120, 80, 14u, 2u, 0u);
    } else if (slot == 5u) {
        // CHILE: Blue canton w/ white 5-point star + top-right White + bottom Crimson Red
        set_ramp(pal, 1,  {24, 64, 176},  {16, 40, 128});
        set_ramp(pal, 7,  {248, 248, 248},{208, 212, 220});
        set_ramp(pal, 13, {224, 28, 36},  {160, 16, 20});
        for (int y = 0; y < 160; ++y) {
            for (int x = 0; x < 240; ++x) {
                uint8_t base = 13u;
                if (y < 80) base = (x < 80) ? 1u : 7u;
                if (x < 80 && y < 80 && point_in_star5(x - 40, y - 40, 22, 9)) {
                    pix[y * 240 + x] = 19u;
                } else {
                    pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
                }
            }
        }
    } else if (slot == 6u) {
        // ECUADOR: 2:1:1 Gold Yellow (0..79) / Royal Blue (80..119) / Crimson Red (120..159) + crest
        set_ramp(pal, 1,  {248, 216, 24}, {196, 164, 8});
        set_ramp(pal, 7,  {16, 72, 184},  {8, 48, 128});
        set_ramp(pal, 13, {224, 32, 36},  {160, 16, 20});
        for (int y = 0; y < 160; ++y) {
            const uint8_t base = (y < 80) ? 1u : (y < 120) ? 7u : 13u;
            for (int x = 0; x < 240; ++x) pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
        }
        draw_oval_crest(pix, 240, 160, 120, 80, 9u, 14u, 1u);
    } else if (slot == 11u) {
        // URUGUAY: 9 alternating White / Sky-Blue horizontal stripes + Gold Sun of May canton
        set_ramp(pal, 1,  {248, 248, 248},{212, 216, 224});
        set_ramp(pal, 7,  {40, 128, 224}, {24, 88, 168});
        set_ramp(pal, 13, {248, 196, 24}, {192, 136, 8});
        for (int y = 0; y < 160; ++y) {
            const int stripe = (y * 9) / 160;
            const uint8_t stripe_base = ((stripe & 1) == 0) ? 1u : 7u;
            for (int x = 0; x < 240; ++x) {
                if (x < 88 && y < 89) {
                    const int dx = x - 44;
                    const int dy = y - 44;
                    const int d2 = dx * dx + dy * dy;
                    if (d2 <= 14 * 14) {
                        pix[y * 240 + x] = (d2 <= 11 * 11) ? 20u : 21u;
                    } else if (d2 <= 28 * 28 && (((dx * dx > 4 * dy * dy) || (dy * dy > 4 * dx * dx) ||
                               ((dx - dy) * (dx - dy) < d2 / 8) || ((dx + dy) * (dx + dy) < d2 / 8)))) {
                        pix[y * 240 + x] = 20u;
                    } else {
                        pix[y * 240 + x] = static_cast<uint8_t>(1u + sheen(x, y));
                    }
                } else {
                    pix[y * 240 + x] = static_cast<uint8_t>(stripe_base + sheen(x, y));
                }
            }
        }
    } else if (slot == 13u) {
        // PARAGUAY: Crimson Red / White / Royal Blue horizontal tricolor + central gold star crest
        set_ramp(pal, 1,  {224, 32, 40},  {164, 16, 24});
        set_ramp(pal, 7,  {248, 248, 248},{208, 212, 220});
        set_ramp(pal, 13, {24, 64, 176},  {12, 40, 128});
        for (int y = 0; y < 160; ++y) {
            const uint8_t base = (y < 53) ? 1u : (y < 107) ? 7u : 13u;
            for (int x = 0; x < 240; ++x) pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
        }
        draw_oval_crest(pix, 240, 160, 120, 80, 14u, 2u, 0u);
    } else if (slot == 7u) {
        // VENEZUELA: Gold Yellow / Royal Blue / Crimson Red tricolor + 8-star white arc
        set_ramp(pal, 1,  {248, 208, 24}, {192, 152, 8});
        set_ramp(pal, 7,  {16, 64, 176},  {8, 40, 124});
        set_ramp(pal, 13, {216, 24, 36},  {152, 12, 20});
        for (int y = 0; y < 160; ++y) {
            const uint8_t base = (y < 53) ? 1u : (y < 107) ? 7u : 13u;
            for (int x = 0; x < 240; ++x) pix[y * 240 + x] = static_cast<uint8_t>(base + sheen(x, y));
        }
        static constexpr int kVenStarX[8] = {64, 78, 94, 111, 129, 146, 162, 176};
        static constexpr int kVenStarY[8] = {92, 80, 71, 66,  66,  71,  80,  92};
        for (int si = 0; si < 8; ++si) {
            for (int dy = -7; dy <= 7; ++dy) {
                for (int dx = -7; dx <= 7; ++dx) {
                    if (point_in_star5(dx, dy, 6, 3)) {
                        pix[(kVenStarY[si] + dy) * 240 + (kVenStarX[si] + dx)] = 19u;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Flag + Banner Cellmap Builder: BG_TM01/05/06/11/13/14 (CEL_FLAG)
// ---------------------------------------------------------------------------
static void build_bgtm(uint8_t slot, uint8_t* buf, size_t len, const char* word) {
    if (len < 0x1000u || std::memcmp(buf, "CEL_FLAG", 8) != 0) return;

    const size_t map1_off = len - 0x2E8u; // 16x10 small flag map
    const size_t map2_off = len - 0x1A4u; // 32x6 country banner map
    const size_t pal_off  = map1_off - 96u;
    const uint32_t max_tiles = static_cast<uint32_t>((pal_off - 0x0Cu) / 32u);

    // Update pal0[0..15] while preserving arcade gold-orange banner gradient pal1 & pal2!
    uint8_t* pal0 = buf + pal_off;
    write_be16(pal0 + 0u * 2u,  0x8280u);
    write_be16(pal0 + 13u * 2u, rgb888_to_bgr555(248, 200, 48)); // Crest gold
    write_be16(pal0 + 14u * 2u, rgb888_to_bgr555(40, 44, 56));   // Outline
    write_be16(pal0 + 15u * 2u, rgb888_to_bgr555(248, 248, 248));

    // Set uniform arcade gold-to-orange gradient in pal1 & pal2 (matching native U.S.A./BRAZIL banners)
    for (int p = 1; p <= 2; ++p) {
        uint8_t* gp = pal0 + static_cast<size_t>(p) * 32u;
        write_be16(gp + 0u * 2u, 0x0000u);
        write_be16(gp + 1u * 2u, 0x8000u); // crisp black shadow/outline
        for (int idx = 2; idx < 16; ++idx) {
            const uint8_t r = 248u;
            const uint8_t g = static_cast<uint8_t>(232 - ((idx - 2) * 112) / 13);
            const uint8_t b = static_cast<uint8_t>(std::max(0, 96 - ((idx - 2) * 96) / 8));
            write_be16(gp + static_cast<size_t>(idx) * 2u, rgb888_to_bgr555(r, g, b));
        }
    }

    // Standard 3-band index semantics on canvas: 2 = band A, 6 = band B, 10 = band C, 15 = white, 13 = gold
    auto set_band = [&](int idx, Rgb l, Rgb m, Rgb d, Rgb x) {
        write_be16(pal0 + static_cast<size_t>(idx + 0) * 2u, rgb888_to_bgr555(l.r, l.g, l.b));
        write_be16(pal0 + static_cast<size_t>(idx + 1) * 2u, rgb888_to_bgr555(m.r, m.g, m.b));
        write_be16(pal0 + static_cast<size_t>(idx + 2) * 2u, rgb888_to_bgr555(d.r, d.g, d.b));
        write_be16(pal0 + static_cast<size_t>(idx + 3) * 2u, rgb888_to_bgr555(x.r, x.g, x.b));
    };

    if (slot == 14u) {
        // BOLIVIA: Red / Yellow / DARK FOREST GREEN
        set_band(1, {232,48,40},  {208,32,24},  {176,16,16},  {144,8,8});
        set_band(5, {248,232,40}, {232,208,24}, {200,176,16}, {168,144,8});
        set_band(9, {8,128,40},   {0,104,24},   {0,80,16},    {0,56,16});
    } else if (slot == 1u) {
        // PERU: Red / White / Red
        set_band(1, {232,40,48},  {208,24,32},  {176,16,24},  {144,8,16});
        set_band(5, {248,248,248},{236,240,244},{216,220,224},{196,200,208});
        set_band(9, {232,40,48},  {208,24,32},  {176,16,24},  {144,8,16});
    } else if (slot == 5u) {
        // CHILE: Blue / White / Red
        set_band(1, {32,80,200},  {24,64,176},  {16,48,144},  {8,32,112});
        set_band(5, {248,248,248},{236,240,244},{216,220,224},{196,200,208});
        set_band(9, {232,40,48},  {208,24,32},  {176,16,24},  {144,8,16});
    } else if (slot == 6u) {
        // ECUADOR: Yellow / Blue / Red
        set_band(1, {248,224,40}, {232,200,16}, {200,168,8},  {168,136,0});
        set_band(5, {24,80,200},  {16,64,168},  {8,48,136},   {8,32,104});
        set_band(9, {232,40,48},  {208,24,32},  {176,16,24},  {144,8,16});
    } else if (slot == 11u) {
        // URUGUAY: White / Sky-Blue / Sun Gold
        set_band(1, {248,248,248},{236,240,244},{216,220,224},{196,200,208});
        set_band(5, {64,160,240}, {40,128,224}, {24,104,192}, {16,80,160});
        set_band(9, {248,216,48}, {232,184,24}, {192,144,16}, {152,104,8});
    } else if (slot == 13u) {
        // PARAGUAY: Red / White / Blue
        set_band(1, {232,40,48},  {208,24,32},  {176,16,24},  {144,8,16});
        set_band(5, {248,248,248},{236,240,244},{216,220,224},{196,200,208});
        set_band(9, {32,80,200},  {24,64,176},  {16,48,144},  {8,32,112});
    } else if (slot == 7u) {
        // VENEZUELA: Yellow / Royal Blue / Red
        set_band(1, {248,216,32}, {232,192,16}, {200,160,8},  {168,128,0});
        set_band(5, {24,72,192},  {16,56,168},  {8,40,136},   {8,24,104});
        set_band(9, {224,32,40},  {196,20,28},  {168,12,20},  {136,8,16});
    }

    // Draw 128x80 flag into canvas1 and pack unique tiles starting at 0x31
    uint8_t flag_canvas[80][128]{};
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 128; ++x) {
            uint8_t c = 2u;
            if (slot == 14u || slot == 13u || slot == 7u) {
                c = (y < 27) ? 2u : (y < 53) ? 6u : 10u;
            } else if (slot == 1u) {
                c = (x < 43 || x >= 85) ? 2u : 6u;
            } else if (slot == 5u) {
                if (y < 40) c = (x < 44) ? 2u : 6u;
                else c = 10u;
                if (x < 44 && y < 40 && point_in_star5(x - 22, y - 20, 12, 5)) c = 15u;
            } else if (slot == 6u) {
                c = (y < 40) ? 2u : (y < 60) ? 6u : 10u;
            } else if (slot == 11u) {
                const int stripe = (y * 9) / 80;
                c = ((stripe & 1) == 0) ? 2u : 6u;
                if (x < 46 && y < 44) {
                    const int dx = x - 23;
                    const int dy = y - 22;
                    c = (dx * dx + dy * dy <= 11 * 11) ? 10u : 2u;
                }
            }
            flag_canvas[y][x] = c;
        }
    }
    if (slot == 7u) {
        static constexpr int kHudStarX[8] = {34, 41, 50, 59, 69, 78, 87, 94};
        static constexpr int kHudStarY[8] = {46, 40, 36, 33, 33, 36, 40, 46};
        for (int si = 0; si < 8; ++si) {
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (dx * dx + dy * dy <= 4) {
                        flag_canvas[kHudStarY[si] + dy][kHudStarX[si] + dx] = 15u;
                    }
                }
            }
        }
    }

    // Rewrite sequential tiles 1..48 (0x01..0x30, 12x4 = 96x32 px) for in-match HUD scoreboard flags.
    {
        uint8_t hud96[32][96]{};
        for (int y = 0; y < 32; ++y) {
            const int sy = (y * 80) / 32;
            for (int x = 0; x < 96; ++x) {
                const int sx = (x * 128) / 96;
                uint8_t c = flag_canvas[sy][sx];
                if (x == 0 || x == 95 || y == 0 || y == 31) c = 14u;
                hud96[y][x] = c;
            }
        }
        uint8_t letter_mask[32][96]{};
        const char* code = kTeamSpecs[slot].code;
        for (int li = 0; li < 3; ++li) {
            const uint8_t* rows = find_glyph_5x7(code[li]);
            if (!rows) continue;
            const int x0 = 12 + li * 26;
            for (int gr = 0; gr < 7; ++gr) {
                for (int gc = 0; gc < 5; ++gc) {
                    if ((rows[gr] & (1u << (4 - gc))) == 0u) continue;
                    for (int dy = 0; dy < 4; ++dy) {
                        for (int dx = 0; dx < 4; ++dx) {
                            const int py = 2 + gr * 4 + dy;
                            const int px = x0 + gc * 4 + dx;
                            if (py < 32 && px < 96) letter_mask[py][px] = 1u;
                        }
                    }
                }
            }
        }
        for (int y = 1; y < 31; ++y) {
            for (int x = 1; x < 95; ++x) {
                if (letter_mask[y][x]) {
                    hud96[y][x] = 15u;
                    continue;
                }
                bool edge = false;
                for (int oy = -1; oy <= 1 && !edge; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        if (letter_mask[y + oy][x + ox]) {
                            edge = true;
                            break;
                        }
                    }
                }
                if (edge) hud96[y][x] = 14u;
            }
        }
        for (uint32_t tr = 0u; tr < 4u; ++tr) {
            for (uint32_t tc = 0u; tc < 12u; ++tc) {
                const uint32_t tid = 1u + tr * 12u + tc;
                uint8_t* dst = buf + 0x0Cu + tid * 32u;
                for (uint32_t py = 0u; py < 8u; ++py) {
                    for (uint32_t xb = 0u; xb < 4u; ++xb) {
                        const uint8_t p0 = hud96[tr * 8u + py][tc * 8u + xb * 2u];
                        const uint8_t p1 = hud96[tr * 8u + py][tc * 8u + xb * 2u + 1u];
                        dst[py * 4u + xb] = static_cast<uint8_t>(((p0 & 0xFu) << 4) | (p1 & 0xFu));
                    }
                }
            }
        }
    }

    static constexpr uint32_t kFirstFlagTile = 0x31u;
    std::memset(buf + 0x0Cu + kFirstFlagTile * 32u, 0, (max_tiles - kFirstFlagTile) * 32u);

    uint32_t next_tile = kFirstFlagTile;
    auto alloc_tile = [&](const uint8_t tdata[32], uint32_t search_start) -> uint32_t {
        for (uint32_t cand = search_start; cand < next_tile; ++cand) {
            if (std::memcmp(buf + 0x0Cu + cand * 32u, tdata, 32u) == 0) return cand;
        }
        if (next_tile < max_tiles) {
            const uint32_t id = next_tile++;
            std::memcpy(buf + 0x0Cu + id * 32u, tdata, 32u);
            return id;
        }
        return (max_tiles > 0u) ? (max_tiles - 1u) : 0u;
    };

    uint8_t* map16x10 = buf + map1_off + 4u;
    for (int ty = 0; ty < 10; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
            uint8_t tdata[32]{};
            for (int py = 0; py < 8; ++py) {
                for (int xb = 0; xb < 4; ++xb) {
                    const uint8_t p0 = flag_canvas[ty * 8 + py][tx * 8 + xb * 2];
                    const uint8_t p1 = flag_canvas[ty * 8 + py][tx * 8 + xb * 2 + 1];
                    tdata[py * 4 + xb] = static_cast<uint8_t>(((p0 & 0xFu) << 4) | (p1 & 0xFu));
                }
            }
            const uint32_t tid = alloc_tile(tdata, kFirstFlagTile);
            write_be16(map16x10 + (ty * 16 + tx) * 2, static_cast<uint16_t>(tid & 0x0FFFu));
        }
    }

    // Draw 256x48 slanted arcade banner starting right after flag tiles
    const uint32_t name_start_tile = next_tile;
    uint8_t canvas[48 * 256]{};
    const int word_len = static_cast<int>(std::strlen(word));
    const int gw = (word_len >= 8) ? 4 : 5;
    const int step = (word_len >= 8) ? 26 : 30;
    const int w_tot = word_len * step - (step - 5 * gw);
    const int ox0 = std::max(4, (256 - w_tot - 4) / 2);

    auto draw_word = [&](int ox, int oy, bool shadow) {
        auto bit_on = [](const uint8_t* rows, int r, int c) -> bool {
            if (!rows || r < 0 || r >= 7 || c < 0 || c >= 5) return false;
            return (rows[r] & (1u << (4 - c))) != 0u;
        };
        for (int li = 0; li < word_len; ++li) {
            const uint8_t* rows = find_glyph_5x7(word[li]);
            if (!rows) continue;
            const int lx = ox + li * step;
            for (int gr = 0; gr < 7; ++gr) {
                for (int gc = 0; gc < 5; ++gc) {
                    const bool on = bit_on(rows, gr, gc);
                    // Smooth outer/inner bevels on 5x7 blocks for rounded arcade strokes
                    const bool u = bit_on(rows, gr - 1, gc);
                    const bool d = bit_on(rows, gr + 1, gc);
                    const bool l = bit_on(rows, gr, gc - 1);
                    const bool r = bit_on(rows, gr, gc + 1);
                    for (int sy = 0; sy < 5; ++sy) {
                        const int py = oy + gr * 5 + sy;
                        if (py < 0 || py >= 48) continue;
                        const int skew = (47 - py) / 6;
                        for (int sx = 0; sx < gw; ++sx) {
                            bool paint = on;
                            if (on) {
                                // Trim harsh outer corner pixel when two perpendicular sides are open
                                if (!u && !l && sy == 0 && sx == 0) paint = false;
                                if (!u && !r && sy == 0 && sx == gw - 1) paint = false;
                                if (!d && !l && sy == 4 && sx == 0) paint = false;
                                if (!d && !r && sy == 4 && sx == gw - 1) paint = false;
                            } else {
                                // Fill subtle 1px inner elbow bevel where two perpendicular strokes meet
                                if (u && l && sy == 0 && sx == 0) paint = true;
                                if (u && r && sy == 0 && sx == gw - 1) paint = true;
                                if (d && l && sy == 4 && sx == 0) paint = true;
                                if (d && r && sy == 4 && sx == gw - 1) paint = true;
                            }
                            if (!paint) continue;
                            const int px = lx + gc * gw + sx + skew;
                            if (px < 0 || px >= 256) continue;
                            if (shadow) {
                                canvas[py * 256 + px] = 1u;
                            } else {
                                const int grad = std::clamp(2 + ((py - oy) * 13) / 34, 2, 15);
                                canvas[py * 256 + px] = static_cast<uint8_t>(grad);
                            }
                        }
                    }
                }
            }
        }
    };
    draw_word(ox0 + 3, 9, true);
    draw_word(ox0 + 2, 8, true);
    draw_word(ox0, 6, false);

    uint8_t* map32x6 = buf + map2_off + 4u;
    for (int ty = 0; ty < 6; ++ty) {
        const uint16_t pal_bits = (ty < 3) ? 0x1000u : 0x2000u;
        for (int tx = 0; tx < 32; ++tx) {
            bool non_zero = false;
            uint8_t tdata[32]{};
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const uint8_t p0 = canvas[(ty * 8 + py) * 256 + (tx * 8 + px * 2)];
                    const uint8_t p1 = canvas[(ty * 8 + py) * 256 + (tx * 8 + px * 2 + 1)];
                    if (p0 != 0u || p1 != 0u) non_zero = true;
                    tdata[py * 4 + px] = static_cast<uint8_t>((p0 << 4) | (p1 & 0xFu));
                }
            }
            uint16_t cell_word = pal_bits; // tile 0 = blank
            if (non_zero) {
                const uint32_t tid = alloc_tile(tdata, name_start_tile);
                cell_word = static_cast<uint16_t>(pal_bits | (tid & 0x0FFFu));
            }
            write_be16(map32x6 + (ty * 32 + tx) * 2, cell_word);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Team Select 96x32 Flags (CEL_OUT + MSG_OUT) using exclusive palettes & pool
// ---------------------------------------------------------------------------
static void write_cel_5shade_ramp(uint8_t* pal, int base_idx, Rgb c0, Rgb c1, Rgb c2, Rgb c3, Rgb c4) {
    write_be16(pal + static_cast<size_t>(base_idx + 0) * 2u, rgb888_to_bgr555(c0.r, c0.g, c0.b));
    write_be16(pal + static_cast<size_t>(base_idx + 1) * 2u, rgb888_to_bgr555(c1.r, c1.g, c1.b));
    write_be16(pal + static_cast<size_t>(base_idx + 2) * 2u, rgb888_to_bgr555(c2.r, c2.g, c2.b));
    write_be16(pal + static_cast<size_t>(base_idx + 3) * 2u, rgb888_to_bgr555(c3.r, c3.g, c3.b));
    write_be16(pal + static_cast<size_t>(base_idx + 4) * 2u, rgb888_to_bgr555(c4.r, c4.g, c4.b));
}

static void build_team_select_flags(uint8_t* cart_rom) {
    uint32_t pool_cursor = 0u;

    for (uint32_t m_idx = 0u; m_idx < CopaModBackup::kModSlotCount; ++m_idx) {
        const uint8_t slot = kModSlots[m_idx];
        const uint8_t pal_id = kCelPalIds[m_idx];
        const char* code = kTeamSpecs[slot].code;
        uint8_t* pal = cart_rom + TeamAssetOffsets::kCelOutPalsBaseOff + static_cast<size_t>(pal_id) * 32u;

        // 1..5 = band group A, 6..10 = band group B, 11..15 = band group C
        if (slot == 14u) {
            // BOLIVIA: A=Yellow, B=Crimson Red, C=Dark Forest Green
            write_cel_5shade_ramp(pal, 1,  {255,252,224}, {248,240,144}, {232,200,64}, {216,176,24}, {200,152,8});
            write_cel_5shade_ramp(pal, 6,  {252,212,212}, {240,156,156}, {216,100,100},{192,56,56},  {168,20,20});
            write_cel_5shade_ramp(pal, 11, {176,228,184}, {104,188,116}, {48,140,64},  {16,104,32},  {0,68,20});
        } else if (slot == 1u) {
            // PERU: A=White, B=Crimson Red, C=Crimson Red
            write_cel_5shade_ramp(pal, 1,  {255,255,255}, {232,236,244}, {188,196,208},{144,152,168},{244,244,248});
            write_cel_5shade_ramp(pal, 6,  {252,216,216}, {244,160,160}, {216,96,96},  {192,48,48},  {172,16,24});
            write_cel_5shade_ramp(pal, 11, {252,216,216}, {244,160,160}, {216,96,96},  {192,48,48},  {172,16,24});
        } else if (slot == 5u) {
            // CHILE: A=White, B=Royal Blue, C=Crimson Red
            write_cel_5shade_ramp(pal, 1,  {255,255,255}, {232,236,244}, {188,196,208},{144,152,168},{244,244,248});
            write_cel_5shade_ramp(pal, 6,  {208,224,255}, {144,184,248}, {88,136,224}, {40,92,192},  {16,56,160});
            write_cel_5shade_ramp(pal, 11, {252,212,212}, {240,156,156}, {216,96,96},  {192,48,48},  {172,16,24});
        } else if (slot == 6u || slot == 7u) {
            // ECUADOR / VENEZUELA share A=Royal Blue, B=Gold Yellow, C=Crimson Red
            write_cel_5shade_ramp(pal, 1,  {208,224,255}, {144,184,248}, {88,136,224}, {40,92,192},  {16,56,160});
            write_cel_5shade_ramp(pal, 6,  {255,252,224}, {248,236,136}, {232,196,48}, {212,168,16}, {192,140,8});
            write_cel_5shade_ramp(pal, 11, {252,212,212}, {240,156,156}, {216,96,96},  {192,48,48},  {172,16,24});
        } else if (slot == 11u) {
            // URUGUAY: A=White, B=Sky Blue Celeste, C=Sun Gold
            write_cel_5shade_ramp(pal, 1,  {255,255,255}, {232,236,244}, {188,196,208},{144,152,168},{244,244,248});
            write_cel_5shade_ramp(pal, 6,  {224,244,255}, {168,216,252}, {104,176,240},{48,136,216}, {24,104,192});
            write_cel_5shade_ramp(pal, 11, {255,252,224}, {248,232,128}, {232,192,48}, {204,152,16}, {184,128,8});
        } else if (slot == 13u) {
            // PARAGUAY: A=White, B=Crimson Red, C=Royal Blue
            write_cel_5shade_ramp(pal, 1,  {255,255,255}, {232,236,244}, {188,196,208},{144,152,168},{244,244,248});
            write_cel_5shade_ramp(pal, 6,  {252,212,212}, {240,156,156}, {216,96,96},  {192,48,48},  {172,16,24});
            write_cel_5shade_ramp(pal, 11, {208,224,255}, {144,184,248}, {88,136,224}, {40,92,192},  {16,56,160});
        }

        // Rasterize 3-letter bold 5x7 code at 4x scale (20x28 each, x=8, 38, 68, y=2..29) into mask
        uint8_t mask[32][96]{};
        for (int li = 0; li < 3; ++li) {
            const uint8_t* rows = find_glyph_5x7(code[li]);
            if (!rows) continue;
            const int x0 = 8 + li * 30;
            for (int gr = 0; gr < 7; ++gr) {
                for (int gc = 0; gc < 5; ++gc) {
                    if ((rows[gr] & (1u << (4 - gc))) == 0u) continue;
                    for (int sy = 0; sy < 4; ++sy) {
                        for (int sx = 0; sx < 4; ++sx) {
                            const int py = 2 + gr * 4 + sy;
                            const int px = x0 + gc * 4 + sx;
                            if (px < 96 && py < 32) mask[py][px] = 1u;
                        }
                    }
                }
            }
        }

        uint8_t pix[32][96]{};
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 96; ++x) {
                uint8_t grp = 0u; // 0->ramp A(1..5), 1->ramp B(6..10), 2->ramp C(11..15)
                if (slot == 14u || slot == 13u || slot == 7u) {
                    grp = (y < 10) ? 1u : (y < 22 ? 0u : 2u);
                } else if (slot == 1u) {
                    grp = (x < 32 || x >= 64) ? 1u : 0u;
                } else if (slot == 5u) {
                    if (y < 16) grp = (x < 32) ? 1u : 0u;
                    else grp = 2u;
                } else if (slot == 6u) {
                    grp = (y < 16) ? 1u : (y < 24 ? 0u : 2u);
                } else if (slot == 11u) {
                    if (x < 32 && y < 16) grp = 2u;
                    else grp = (((y / 4) & 1) == 0) ? 0u : 1u;
                }
                const uint8_t base = static_cast<uint8_t>(1u + grp * 5u);
                const uint8_t bg = static_cast<uint8_t>(base + 4u);
                const uint8_t fg = static_cast<uint8_t>(base + 1u);
                const uint8_t hi = base;
                const uint8_t lo = static_cast<uint8_t>(base + 2u);
                if (!mask[y][x]) {
                    pix[y][x] = bg;
                } else {
                    const bool up = (y > 0) && mask[y - 1][x];
                    const bool dn = (y < 31) && mask[y + 1][x];
                    const bool lf = (x > 0) && mask[y][x - 1];
                    const bool rt = (x < 95) && mask[y][x + 1];
                    pix[y][x] = (!up || !lf) ? hi : ((!dn || !rt) ? lo : fg);
                }
            }
        }

        // Deduplicate & allocate tiles strictly from the non-overlapping safe pool
        uint8_t uniq_tiles[48][32]{};
        uint16_t uniq_ids[48]{};
        uint32_t uniq_count = 0u;
        uint8_t* map_out = cart_rom + TeamAssetOffsets::kMsgOutBaseOff +
                           static_cast<size_t>(slot) * TeamAssetOffsets::kMsgOutMapBytes;
        const uint16_t pal_bits = static_cast<uint16_t>(static_cast<uint16_t>(pal_id) << 12);

        for (int tr = 0; tr < 4; ++tr) {
            for (int tc = 0; tc < 12; ++tc) {
                uint8_t tile4[32]{};
                for (int y = 0; y < 8; ++y) {
                    for (int xb = 0; xb < 4; ++xb) {
                        const uint8_t p0 = pix[tr * 8 + y][tc * 8 + xb * 2];
                        const uint8_t p1 = pix[tr * 8 + y][tc * 8 + xb * 2 + 1];
                        tile4[y * 4 + xb] = static_cast<uint8_t>(((p0 & 0xFu) << 4) | (p1 & 0xFu));
                    }
                }
                uint16_t chosen_id = 0u;
                bool found = false;
                for (uint32_t u = 0; u < uniq_count; ++u) {
                    if (std::memcmp(uniq_tiles[u], tile4, 32u) == 0) {
                        chosen_id = uniq_ids[u];
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (pool_cursor < CopaModBackup::kSafeCelPoolSize) {
                        chosen_id = kSafeCelOutTilePool[pool_cursor++];
                        std::memcpy(cart_rom + TeamAssetOffsets::kCelOutTilesBaseOff +
                                        static_cast<size_t>(chosen_id) * 32u,
                                    tile4, 32u);
                    } else if (uniq_count > 0u) {
                        chosen_id = uniq_ids[uniq_count - 1u];
                    }
                    if (uniq_count < 48u) {
                        std::memcpy(uniq_tiles[uniq_count], tile4, 32u);
                        uniq_ids[uniq_count++] = chosen_id;
                    }
                }
                write_be16(map_out + (tr * 12 + tc) * 2, static_cast<uint16_t>(pal_bits | (chosen_id & 0x0FFFu)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Kits (TEXTM & TEXHM): Bolivia Darker Green + PER/CHI/ECU/URU/PAR
// ---------------------------------------------------------------------------
static void write_pal_ramp7(uint8_t* dst, Rgb hi, Rgb lo) {
    for (int i = 0; i < 7; ++i) {
        const uint8_t r = static_cast<uint8_t>(hi.r + ((static_cast<int>(lo.r) - hi.r) * i) / 6);
        const uint8_t g = static_cast<uint8_t>(hi.g + ((static_cast<int>(lo.g) - hi.g) * i) / 6);
        const uint8_t b = static_cast<uint8_t>(hi.b + ((static_cast<int>(lo.b) - hi.b) * i) / 6);
        write_be16(dst + i * 2, rgb888_to_bgr555(r, g, b));
    }
}

static void write_pal_ramp4(uint8_t* dst, Rgb hi, Rgb lo) {
    for (int i = 0; i < 4; ++i) {
        const uint8_t r = static_cast<uint8_t>(hi.r + ((static_cast<int>(lo.r) - hi.r) * i) / 3);
        const uint8_t g = static_cast<uint8_t>(hi.g + ((static_cast<int>(lo.g) - hi.g) * i) / 3);
        const uint8_t b = static_cast<uint8_t>(hi.b + ((static_cast<int>(lo.b) - hi.b) * i) / 3);
        write_be16(dst + i * 2, rgb888_to_bgr555(r, g, b));
    }
}

static void patch_kit_pal(uint8_t* p, Rgb pri_hi, Rgb pri_lo, Rgb trim_hi, Rgb trim_lo,
                          Rgb shorts_hi, Rgb shorts_lo, Rgb acc0, Rgb acc1, Rgb acc2) {
    // pal[0]
    write_pal_ramp7(p, pri_hi, pri_lo);
    write_be16(p + 7 * 2, rgb888_to_bgr555(acc0.r, acc0.g, acc0.b));
    write_pal_ramp4(p + 8 * 2, trim_hi, trim_lo);
    write_be16(p + 12 * 2, rgb888_to_bgr555(acc0.r, acc0.g, acc0.b));
    write_be16(p + 13 * 2, rgb888_to_bgr555(acc1.r, acc1.g, acc1.b));
    write_be16(p + 14 * 2, rgb888_to_bgr555(acc2.r, acc2.g, acc2.b));

    // pal[1] (+32): shorts & socks
    write_pal_ramp7(p + 32, pri_hi, pri_lo);
    write_pal_ramp7(p + 32 + 7 * 2, shorts_hi, shorts_lo);

    // pal[2] (+64): distant LOD indices 0..8 ONLY (preserve 9..15 skin/hair/boots)
    write_pal_ramp4(p + 64, pri_hi, pri_lo);
    write_be16(p + 64 + 4 * 2, rgb888_to_bgr555(shorts_hi.r, shorts_hi.g, shorts_hi.b));
    write_be16(p + 64 + 5 * 2, rgb888_to_bgr555(shorts_lo.r, shorts_lo.g, shorts_lo.b));
    write_be16(p + 64 + 6 * 2, rgb888_to_bgr555(trim_hi.r, trim_hi.g, trim_hi.b));
    write_be16(p + 64 + 7 * 2, rgb888_to_bgr555(acc0.r, acc0.g, acc0.b));
    write_be16(p + 64 + 8 * 2, rgb888_to_bgr555(acc2.r, acc2.g, acc2.b));
}

// Paints a crisp diagonal sash on torso sprites (3,4,5,18,19 and numbered backs 29..48)
// using index 14 (sash core) and 13 (sash edge) ONLY over base shirt pixels (1..7).
static void paint_diagonal_sash_tex(uint8_t* tex_base) {
    uint32_t pos = TeamAssetOffsets::kTexPixOff;
    for (uint32_t si = 0u; si < 49u; ++si) {
        const uint8_t w = tex_base[TeamAssetOffsets::kTexDescOff + si * 4u + 1u];
        const uint8_t h = tex_base[TeamAssetOffsets::kTexDescOff + si * 4u + 2u];
        const uint32_t sz = (static_cast<uint32_t>(w) * static_cast<uint32_t>(h)) / 2u;
        const bool is_torso = (si == 3u || si == 4u || si == 5u || si == 18u || si == 19u || si >= 29u);
        if (is_torso && w > 0u && h > 0u) {
            uint8_t* sp = tex_base + pos;
            for (uint32_t y = 0u; y < h; ++y) {
                for (uint32_t x = 0u; x < w; ++x) {
                    const uint32_t byte_i = (y * w + x) / 2u;
                    const bool hi_nib = ((x & 1u) == 0u);
                    const uint8_t pix = hi_nib ? (sp[byte_i] >> 4) : (sp[byte_i] & 0xFu);
                    if (pix >= 1u && pix <= 7u) {
                        // Diagonal top-right -> bottom-left (front) / reversed (backs)
                        const int u = static_cast<int>(x * 40u) / static_cast<int>(w);
                        const int v = static_cast<int>(y * 40u) / static_cast<int>(h);
                        const int dist = (si >= 29u) ? (u - v) : (u + v - 40);
                        const int ad = (dist < 0) ? -dist : dist;
                        if (ad <= 6) {
                            const uint8_t sash_idx = (ad <= 4) ? 14u : 13u;
                            if (hi_nib) sp[byte_i] = static_cast<uint8_t>((sash_idx << 4) | (sp[byte_i] & 0x0Fu));
                            else        sp[byte_i] = static_cast<uint8_t>((sp[byte_i] & 0xF0u) | sash_idx);
                        }
                    }
                }
            }
        }
        pos += sz;
    }
}

static void apply_kits_and_capa(uint8_t* cart_rom, const CopaModBackup& backup) {
    // Restore original TEXTM/TEXHM templates cleanly before patching
    for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
        const uint8_t s = kModSlots[i];
        std::memcpy(cart_rom + TeamAssetOffsets::kTexTmBaseOff + s * TeamAssetOffsets::kTexStride,
                    backup.orig_textm[i].data(), TeamAssetOffsets::kTexStride);
        std::memcpy(cart_rom + TeamAssetOffsets::kTexHmBaseOff + s * TeamAssetOffsets::kTexStride,
                    backup.orig_texhm[i].data(), TeamAssetOffsets::kTexStride);
    }

    // 1. Bolivia (Slot 14): DARKER Forest Green ("La Verde") Home / Crisp White Away
    {
        uint8_t* tm14 = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 14u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        uint8_t* hm14 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 14u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(tm14, {0, 88, 24}, {0, 24, 6}, {240, 240, 240}, {120, 120, 120},
                      {0, 64, 16}, {0, 20, 4}, {232, 184, 16}, {0, 56, 12}, {0, 32, 8});
        patch_kit_pal(hm14, {244, 244, 244}, {160, 160, 168}, {0, 72, 18}, {0, 24, 6},
                      {0, 72, 18}, {0, 20, 4}, {232, 184, 16}, {192, 24, 20}, {128, 12, 12});
    }

    // 2. Peru (Slot 1): White + Red Diagonal Sash ("La Franja Roja") Home / Red + White Sash Away
    {
        uint8_t* tm01_base = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 1u * TeamAssetOffsets::kTexStride;
        uint8_t* hm01_base = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 1u * TeamAssetOffsets::kTexStride;
        // Use clean plain white uniform pixel template from TEXHM01 for Home too, then paint sash
        std::memcpy(tm01_base + TeamAssetOffsets::kTexDescOff,
                    hm01_base + TeamAssetOffsets::kTexDescOff,
                    TeamAssetOffsets::kTexDescBytes);
        std::memcpy(tm01_base + TeamAssetOffsets::kTexPixOff,
                    hm01_base + TeamAssetOffsets::kTexPixOff,
                    TeamAssetOffsets::kTexPixBytes);
        paint_diagonal_sash_tex(tm01_base);
        paint_diagonal_sash_tex(hm01_base);

        // Home: white jersey (0..7), dark-crimson number/trim (8..11), bright red sash (12..14)
        patch_kit_pal(tm01_base + TeamAssetOffsets::kTexPalOff,
                      {248,248,248}, {160,160,168}, {200,24,32}, {120,16,24},
                      {248,248,248}, {168,168,176}, {224,24,32}, {208,16,24}, {232,32,40});
        // Away: crimson red jersey (0..7), white number/trim (8..11), white sash (12..14)
        patch_kit_pal(hm01_base + TeamAssetOffsets::kTexPalOff,
                      {224,24,32}, {120,8,16}, {248,248,248}, {168,168,176},
                      {224,24,32}, {128,16,24}, {248,248,248}, {224,224,232}, {248,248,248});
    }

    // 3. Chile (Slot 5): Crimson Red ("La Roja") + Royal Blue shorts / White Away
    {
        uint8_t* tm05 = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 5u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        uint8_t* hm05 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 5u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(tm05, {232,32,40}, {128,16,24}, {248,248,248}, {168,168,176},
                      {24,64,184}, {8,32,112}, {24,64,184}, {16,48,144}, {248,248,248});
        patch_kit_pal(hm05, {248,248,248}, {160,160,168}, {224,32,40}, {144,16,24},
                      {24,64,184}, {8,32,112}, {24,64,184}, {224,32,40}, {248,248,248});
    }

    // 4. Ecuador (Slot 6): Gold Yellow ("La Tri") + Royal Blue shorts / Navy Blue Away
    {
        uint8_t* tm06 = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 6u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        uint8_t* hm06 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 6u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(tm06, {248,216,24}, {176,136,8}, {24,64,184}, {8,32,112},
                      {24,64,184}, {8,32,112}, {224,24,32}, {24,64,184}, {248,216,24});
        patch_kit_pal(hm06, {16,56,160}, {8,24,96}, {248,216,24}, {176,136,8},
                      {16,56,160}, {8,24,96}, {248,216,24}, {224,24,32}, {248,248,248});
    }

    // 5. Uruguay (Slot 11): Sky-Blue ("La Celeste") + Black shorts / White Away
    {
        uint8_t* tm11 = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 11u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        uint8_t* hm11 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 11u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(tm11, {88,184,248}, {32,104,176}, {32,36,44}, {16,16,24},
                      {40,40,48}, {16,16,24}, {248,248,248}, {32,36,44}, {248,204,48});
        patch_kit_pal(hm11, {248,248,248}, {160,164,172}, {72,168,240}, {24,96,168},
                      {40,40,48}, {16,16,24}, {72,168,240}, {32,104,176}, {248,204,48});
    }

    // 6. Paraguay (Slot 13): Albirroja (ARG vertical stripes template recolored Red & White + Blue shorts)
    {
        uint8_t* tm13_base = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 13u * TeamAssetOffsets::kTexStride;
        const uint8_t* arg_base = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 3u * TeamAssetOffsets::kTexStride;
        std::memcpy(tm13_base + TeamAssetOffsets::kTexDescOff,
                    arg_base + TeamAssetOffsets::kTexDescOff,
                    TeamAssetOffsets::kTexDescBytes);
        std::memcpy(tm13_base + TeamAssetOffsets::kTexPixOff,
                    arg_base + TeamAssetOffsets::kTexPixOff,
                    TeamAssetOffsets::kTexPixBytes);

        // ARG indices: 0..5=White stripe, 6..10=Colored stripe (Red!), 11..14=Navy/Blue accents
        uint8_t* p = tm13_base + TeamAssetOffsets::kTexPalOff;
        write_be16(p + 0 * 2, rgb888_to_bgr555(248, 248, 248));
        write_be16(p + 1 * 2, rgb888_to_bgr555(240, 240, 240));
        write_be16(p + 2 * 2, rgb888_to_bgr555(224, 224, 224));
        write_be16(p + 3 * 2, rgb888_to_bgr555(200, 200, 208));
        write_be16(p + 4 * 2, rgb888_to_bgr555(176, 176, 184));
        write_be16(p + 5 * 2, rgb888_to_bgr555(152, 152, 160));
        write_be16(p + 6 * 2, rgb888_to_bgr555(232, 32, 40));
        write_be16(p + 7 * 2, rgb888_to_bgr555(208, 24, 32));
        write_be16(p + 8 * 2, rgb888_to_bgr555(184, 16, 24));
        write_be16(p + 9 * 2, rgb888_to_bgr555(152, 8, 16));
        write_be16(p + 10 * 2, rgb888_to_bgr555(120, 8, 16));
        write_be16(p + 11 * 2, rgb888_to_bgr555(24, 64, 184));
        write_be16(p + 12 * 2, rgb888_to_bgr555(16, 48, 144));
        write_be16(p + 13 * 2, rgb888_to_bgr555(16, 40, 128));
        write_be16(p + 14 * 2, rgb888_to_bgr555(24, 72, 192));
        // pal[1] (+32): Royal Blue shorts
        write_be16(p + 32 + 0 * 2, rgb888_to_bgr555(248, 248, 248));
        write_be16(p + 32 + 1 * 2, rgb888_to_bgr555(224, 224, 232));
        write_be16(p + 32 + 2 * 2, rgb888_to_bgr555(200, 200, 208));
        write_be16(p + 32 + 3 * 2, rgb888_to_bgr555(232, 32, 40));
        write_be16(p + 32 + 4 * 2, rgb888_to_bgr555(192, 16, 24));
        write_be16(p + 32 + 5 * 2, rgb888_to_bgr555(152, 8, 16));
        write_be16(p + 32 + 6 * 2, rgb888_to_bgr555(120, 8, 16));
        write_pal_ramp7(p + 32 + 7 * 2, {24, 64, 184}, {8, 32, 112});
        // pal[2] (+64): distant LOD 0..8
        write_be16(p + 64 + 0 * 2, rgb888_to_bgr555(232, 32, 40));
        write_be16(p + 64 + 1 * 2, rgb888_to_bgr555(248, 248, 248));
        write_be16(p + 64 + 2 * 2, rgb888_to_bgr555(24, 64, 184));
        write_be16(p + 64 + 3 * 2, rgb888_to_bgr555(8, 32, 112));
        write_be16(p + 64 + 4 * 2, rgb888_to_bgr555(184, 16, 24));
        write_be16(p + 64 + 5 * 2, rgb888_to_bgr555(216, 216, 224));
        write_be16(p + 64 + 6 * 2, rgb888_to_bgr555(144, 8, 16));
        write_be16(p + 64 + 7 * 2, rgb888_to_bgr555(24, 64, 184));
        write_be16(p + 64 + 8 * 2, rgb888_to_bgr555(120, 8, 16));

        // Away: Royal Blue jersey + white shorts/trim + red crest (distinct vs PER/CHI/VEN/ARG/USA)
        uint8_t* hm13 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 13u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(hm13, {20,60,176}, {8,28,108}, {248,248,248}, {168,168,176},
                      {248,248,248}, {160,160,168}, {224,24,32}, {248,248,248}, {224,24,32});
    }

    // 7. Venezuela (Slot 7): Vinotinto (Burgundy) Home + White shorts / White Away + Vinotinto trim
    {
        uint8_t* tm07 = cart_rom + TeamAssetOffsets::kTexTmBaseOff + 7u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        uint8_t* hm07 = cart_rom + TeamAssetOffsets::kTexHmBaseOff + 7u * TeamAssetOffsets::kTexStride + TeamAssetOffsets::kTexPalOff;
        patch_kit_pal(tm07, {112,16,42}, {56,6,20}, {244,244,244}, {176,176,184},
                      {244,244,244}, {152,152,160}, {224,184,48}, {112,16,42}, {244,244,244});
        patch_kit_pal(hm07, {244,244,244}, {164,164,174}, {112,16,42}, {56,6,20},
                      {112,16,42}, {56,6,20}, {224,184,48}, {112,16,42}, {244,244,244});
    }
}

// Per-player brown-skin masks (0=fair+light hair, 1=brown+black hair) shared by
// CAPA_TBL (+1 byte) and the master 11B/team table at 0x060758F1 (read by 0x0603EE36).
static constexpr uint8_t kBolSkins[11] = {1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};
static constexpr uint8_t kPerSkins[11] = {1u, 1u, 1u, 0u, 1u, 1u, 1u, 1u, 0u, 1u, 1u};
static constexpr uint8_t kChiSkins[11] = {0u, 0u, 1u, 0u, 1u, 0u, 0u, 1u, 0u, 0u, 1u};
static constexpr uint8_t kEcuSkins[11] = {1u, 1u, 1u, 1u, 1u, 1u, 1u, 0u, 1u, 1u, 1u};
static constexpr uint8_t kUruSkins[11] = {0u, 0u, 1u, 0u, 0u, 1u, 0u, 0u, 0u, 1u, 0u};
static constexpr uint8_t kParSkins[11] = {0u, 1u, 0u, 0u, 1u, 0u, 0u, 1u, 0u, 0u, 1u};
static constexpr uint8_t kVenSkins[11] = {1u, 1u, 0u, 1u, 1u, 0u, 1u, 1u, 1u, 0u, 1u};

static void patch_master_skin_table(uint8_t* skin_base) {
    std::memcpy(skin_base + 14u * 11u, kBolSkins, 11u);
    std::memcpy(skin_base + 1u  * 11u, kPerSkins, 11u);
    std::memcpy(skin_base + 5u  * 11u, kChiSkins, 11u);
    std::memcpy(skin_base + 6u  * 11u, kEcuSkins, 11u);
    std::memcpy(skin_base + 11u * 11u, kUruSkins, 11u);
    std::memcpy(skin_base + 13u * 11u, kParSkins, 11u);
    std::memcpy(skin_base + 7u  * 11u, kVenSkins, 11u);
}

static void patch_capa_table(uint8_t* capa_base) {
    auto set_skins = [&](uint8_t slot, const uint8_t skins[11]) {
        for (uint32_t p = 0u; p < 11u; ++p) {
            capa_base[slot * TeamAssetOffsets::kCapaTeamBytes + p * 48u + 1u] = skins[p];
        }
    };

    set_skins(14u, kBolSkins);
    set_skins(1u,  kPerSkins);
    set_skins(5u,  kChiSkins);
    set_skins(6u,  kEcuSkins);
    set_skins(11u, kUruSkins);
    set_skins(13u, kParSkins);
    set_skins(7u,  kVenSkins);

    // Ensure Super Goalkeeper CAPA_TBL flags on Bolivia (14) and Paraguay (13): mode=3, skills=15
    for (uint8_t gk_slot : {13u, 14u}) {
        uint8_t* p0 = capa_base + gk_slot * TeamAssetOffsets::kCapaTeamBytes;
        p0[2u] = 3u;
        p0[25u] = 15u;
        p0[26u] = 15u;
        p0[27u] = 15u;
        p0[28u] = 15u;
    }
}

static bool has_nonzero(const uint8_t* p, size_t n) {
    uint8_t acc = 0u;
    for (size_t i = 0; i < n; ++i) acc |= p[i];
    return acc != 0u;
}

static void patch_kit_clash_matrix(uint8_t* dst) {
    // Preserve bits 13..0 (0x3FFFu -> 0x0607656a/6b/6c attract/match parameters read by 0x06007424);
    // only update the Away-kit flip bits 15..14 (0x8000 / 0x4000) across the 16x16 national submatrix.
    auto set_flip = [&](uint32_t row, uint32_t col, uint16_t flip_bits) {
        uint8_t* cell = dst + (row * 19u + col) * 2u;
        const uint16_t preserved = static_cast<uint16_t>(read_be16(cell) & 0x3FFFu);
        write_be16(cell, static_cast<uint16_t>(((preserved != 0u) ? preserved : 0x0001u) | (flip_bits & 0xC000u)));
    };
    for (uint32_t i = 0u; i < 16u; ++i) {
        for (uint32_t j = 0u; j < 16u; ++j) {
            set_flip(i, j, (i == j) ? 0x4000u : 0x0000u);
        }
    }
    // Helper: ensure `away_slot` ALWAYS switches to Away kit (`TEXHM`) against `home_slot`,
    // regardless of slot ordering or [0x06076549] half-time flip in 0x06007424.
    auto flip_second = [&](uint32_t home_slot, uint32_t away_slot) {
        set_flip(home_slot, away_slot, 0x4000u);
        set_flip(away_slot, home_slot, 0x8000u);
    };
    // Yellow Home group: BRA(0), ECU(6), COL(12) -> flip second to Blue/Navy Away
    flip_second(0u, 6u);
    flip_second(0u, 12u);
    flip_second(12u, 6u);
    // Green Home group: MEX(10), BOL(14) -> flip BOL(14) to White Away
    flip_second(10u, 14u);
    // White / Light-striped Home group: PER(1), ARG(3), USA(15) -> flip ARG/USA to Dark Blue Away
    flip_second(1u, 3u);
    flip_second(1u, 15u);
    flip_second(3u, 15u);
    // Sky-Blue URU(11) vs ARG(3)/USA(15) -> flip ARG/USA to Dark Blue Away (preserve URU Celeste!)
    flip_second(11u, 3u);
    flip_second(11u, 15u);
    // Red / Stripes / Vinotinto group: CHI(5), VEN(7), PAR(13), PER(1)
    flip_second(5u, 7u);   // CHI(Red) vs VEN -> VEN(White)
    flip_second(5u, 13u);  // CHI(Red) vs PAR -> PAR(Royal Blue)
    flip_second(7u, 13u);  // VEN(Vinotinto) vs PAR -> PAR(Royal Blue)
    flip_second(1u, 13u);  // PER(White+Red) vs PAR -> PAR(Royal Blue)
}

static void build_ven_flag_texgm(uint8_t* dst) {
    // 48x24 4bpp texture (idx 0x00DD): Navy Blue (0xB) middle band with an arc of 8 White (0x1) stars
    static constexpr int kW = 48;
    static constexpr int kH = 24;
    static constexpr uint8_t kNavy = 0xBu;
    static constexpr uint8_t kWhite = 0x1u;
    std::memset(dst, static_cast<int>((kNavy << 4) | kNavy), TeamAssetOffsets::kTexGmVenFlagBytes);

    auto plot = [&](int x, int y, uint8_t val) {
        if (x < 0 || x >= kW || y < 0 || y >= kH) return;
        const int off = (y * kW + x) / 2;
        if ((x & 1) == 0) {
            dst[off] = static_cast<uint8_t>((dst[off] & 0x0Fu) | ((val & 0x0Fu) << 4));
        } else {
            dst[off] = static_cast<uint8_t>((dst[off] & 0xF0u) | (val & 0x0Fu));
        }
    };
    static constexpr int kStars[8][2] = {
        {10, 14}, {13, 10}, {17, 7}, {21, 6},
        {26,  6}, {30,  7}, {34, 10}, {37, 14}
    };
    for (int i = 0; i < 8; ++i) {
        const int sx = kStars[i][0];
        const int sy = kStars[i][1];
        plot(sx, sy, kWhite);
        plot(sx - 1, sy, kWhite);
        plot(sx + 1, sy, kWhite);
        plot(sx, sy - 1, kWhite);
        plot(sx, sy + 1, kWhite);
    }
}

// ---------------------------------------------------------------------------
// 7. Fase 3 (B-H5-06 & B-H5-07): 3D Copa America Trophy + Champion Ending
// ---------------------------------------------------------------------------
struct TexEnSlotMeta {
    uint8_t slot;
    uint32_t off;
    int cup_cx;
};
static constexpr TexEnSlotMeta kTexEnMeta[CopaTournamentState::kActiveTeamCount] = {
    { 0u, 0x010E394Cu, 204 }, // BRA
    { 1u, 0x010F6F60u, 201 }, // PER (ex-JPN)
    { 3u, 0x0111DB88u, 202 }, // ARG
    { 5u, 0x011447B0u, 144 }, // CHI (ex-FRA)
    { 6u, 0x01157DC4u, 204 }, // ECU (ex-ESP)
    { 7u, 0x0116B3D8u, 142 }, // VEN (ex-KOR)
    {10u, 0x011A5614u, 147 }, // MEX
    {11u, 0x011B8C28u, 197 }, // URU (ex-GER)
    {12u, 0x011CC23Cu, 199 }, // COL
    {13u, 0x011DF850u, 147 }, // PAR (ex-CMR)
    {14u, 0x011F2E64u, 143 }, // BOL (ex-YUG)
    {15u, 0x01206478u, 146 }, // USA
};

struct CelEnSlotMeta {
    uint32_t off;
    uint32_t size;
};
static constexpr CelEnSlotMeta kCelEnMeta[CopaModBackup::kModSlotCount] = {
    { 0x00FF7724u, 48744u }, // 1: PER
    { 0x0102DF44u, 55240u }, // 5: CHI
    { 0x0103B70Cu, 60328u }, // 6: ECU
    { 0x0104A2B4u, 52360u }, // 7: VEN
    { 0x01080B94u, 60104u }, // 11: URU
    { 0x0109DB44u, 55720u }, // 13: PAR
    { 0x010AB4ECu, 56072u }, // 14: BOL
};

static void patch_cup_palettes(uint8_t* dst64) {
    // Bank 4 (32B): Sterling Silver / Platinum ramp for the 3D Copa America Chalice & Stem
    static constexpr Rgb kSilverRamp[16] = {
        {  0, 128,   0}, {252, 254, 255}, {238, 242, 248}, {222, 226, 234},
        {204, 210, 220}, {184, 190, 202}, {162, 170, 184}, {140, 148, 162},
        {118, 126, 140}, { 98, 106, 120}, { 78,  86, 100}, { 62,  68,  82},
        { 48,  54,  66}, { 36,  40,  52}, { 24,  28,  38}, { 14,  16,  24}
    };
    // Bank 5 (32B): 4-tier Dark Mahogany Wood base (1..6) + Gold/Brass Oval Plaques (7..12) + Silver Trim (13..15)
    static constexpr Rgb kBaseWoodGoldRamp[16] = {
        {  0,   0,   0}, {124,  74,  44}, { 96,  52,  28}, { 72,  36,  18},
        { 52,  24,  12}, { 36,  16,   8}, { 20,   9,   5}, {252, 240, 168},
        {240, 208,  92}, {214, 174,  54}, {178, 136,  34}, {136,  96,  22},
        { 88,  58,  14}, {232, 238, 248}, {168, 176, 192}, { 96, 104, 118}
    };
    for (int i = 0; i < 16; ++i) {
        write_be16(dst64 + static_cast<size_t>(i) * 2u,
                   rgb888_to_bgr555(kSilverRamp[i].r, kSilverRamp[i].g, kSilverRamp[i].b, i != 0));
        write_be16(dst64 + 32u + static_cast<size_t>(i) * 2u,
                   rgb888_to_bgr555(kBaseWoodGoldRamp[i].r, kBaseWoodGoldRamp[i].g, kBaseWoodGoldRamp[i].b, i != 0));
    }
}

static void build_copa_3d_trophy(uint8_t* cart_rom) {
    // 1. POL_CUP.DAT (0x006358B4): Rebuild 138 vertices & 132 polygons along vertical Z (-Z top -> +Z bottom)
    //    matching the real Copa America: Silver Chalice + Fluted Silver Stem/Foot + Large 4-Tiered Wood Base.
    uint8_t* pol = cart_rom + TeamAssetOffsets::kPolCupOff;
    static constexpr int32_t kScale16 = 65536;
    // Fixed-point cos/sin * 65536 for 8 octagonal angles k=0..7 (45 deg steps):
    static constexpr int32_t kCos8[8] = {
         65536,  46341,      0, -46341, -65536, -46341,      0,  46341
    };
    static constexpr int32_t kSin8[8] = {
             0,  46341,  65536,  46341,      0, -46341, -65536, -46341
    };

    auto set_vert = [&](int idx, int32_t x_q16, int32_t y_q16, int32_t z_q16) {
        const uint32_t voff = 12u + static_cast<uint32_t>(idx) * 12u;
        write_be32(pol + voff + 0u, static_cast<uint32_t>(x_q16));
        write_be32(pol + voff + 4u, static_cast<uint32_t>(y_q16));
        write_be32(pol + voff + 8u, static_cast<uint32_t>(z_q16));
    };
    auto set_ring = [&](int base_idx, double z_units, double r_units) {
        const int32_t z16 = static_cast<int32_t>(z_units * 65536.0);
        const int64_t r16 = static_cast<int64_t>(r_units * 65536.0);
        for (int k = 0; k < 8; ++k) {
            const int32_t x16 = static_cast<int32_t>((r16 * kCos8[k]) >> 16);
            const int32_t y16 = static_cast<int32_t>((r16 * kSin8[k]) >> 16);
            set_vert(base_idx + k, x16, y16, z16);
        }
    };

    // Vertex layout (138 total: v0, 17 octagonal rings v1..v136, v137):
    //   v0:          inner chalice recessed floor (z = -9.4)
    //   v1..v48:     Silver Chalice c0..c5 (lip -> neck -> shoulder -> ornate belly -> lower urn -> collar)
    //   v49..v72:    Fluted Silver Stem & Foot s0..s2 (top collar -> narrow waist -> flared silver foot)
    //   v73..v136:   Big 4-Tiered Dark Wooden Base b0..b7 (4 real 3D stepped tiers + shelves)
    //   v137:        bottom plinth floor center (z = +32.6)
    struct CupRingSpec {
        double z;
        double r;
    };
    static constexpr CupRingSpec kRings[17] = {
        // Silver Chalice (rings 0..5 -> v1..v48)
        {  -8.6,  5.2 }, // 0: c0 Top flared rim
        {  -6.2,  3.6 }, // 1: c1 Concave silver neck
        {  -4.0,  6.7 }, // 2: c2 Upper shoulder / embossed frieze top
        {  -1.0,  7.0 }, // 3: c3 Embossed frieze bottom / wide amphora belly
        {   3.4,  4.7 }, // 4: c4 Curved lower chalice bowl
        {   6.8,  2.0 }, // 5: c5 Chalice base collar
        // Fluted Silver Stem & Pedestal Foot (rings 6..8 -> v49..v72)
        {   6.5,  2.1 }, // 6: s0 Stem top collar (tucked seamlessly into chalice base)
        {  10.5,  1.8 }, // 7: s1 Sleek narrow fluted stem waist
        {  13.7,  5.2 }, // 8: s2 Wide flared silver foot resting on wooden base
        // Large 4-Tiered Dark Mahogany Base (rings 9..16 -> v73..v136)
        {  14.2,  6.0 }, // 9:  b0 Tier 1 top
        {  17.4,  6.2 }, // 10: b1 Tier 1 bottom
        {  18.0,  7.2 }, // 11: b2 Tier 2 top (real 3D step shelf)
        {  21.2,  7.4 }, // 12: b3 Tier 2 bottom
        {  21.8,  8.4 }, // 13: b4 Tier 3 top (real 3D step shelf)
        {  25.0,  8.6 }, // 14: b5 Tier 3 bottom
        {  25.6,  9.7 }, // 15: b6 Tier 4 top (real 3D step shelf)
        {  29.2, 10.0 }, // 16: b7 Tier 4 bottom (wide monumental plinth!)
    };
    set_vert(0, 0, 0, static_cast<int32_t>(-6.6 * 65536.0));
    for (int ri = 0; ri < 17; ++ri) {
        set_ring(1 + ri * 8, kRings[ri].z, kRings[ri].r);
    }
    set_vert(137, 0, 0, static_cast<int32_t>(29.2 * 65536.0));

    auto write_poly = [&](int pi, uint16_t v0, uint16_t v1, uint16_t v2, uint16_t v3,
                          uint16_t texno, uint16_t colno, uint16_t dir) {
        const uint32_t poff = 0x0684u + static_cast<uint32_t>(pi) * 20u;
        const uint32_t aoff = 0x10D4u + static_cast<uint32_t>(pi) * 12u;
        write_be16(pol + poff + 12u, v0);
        write_be16(pol + poff + 14u, v1);
        write_be16(pol + poff + 16u, v2);
        write_be16(pol + poff + 18u, v3);

        auto vx = [&](uint16_t idx) -> int64_t { return read_be_s32(pol + 12u + idx * 12u + 0u); };
        auto vy = [&](uint16_t idx) -> int64_t { return read_be_s32(pol + 12u + idx * 12u + 4u); };
        auto vz = [&](uint16_t idx) -> int64_t { return read_be_s32(pol + 12u + idx * 12u + 8u); };
        const int64_t ax = (vx(v1) - vx(v0)) >> 4;
        const int64_t ay = (vy(v1) - vy(v0)) >> 4;
        const int64_t az = (vz(v1) - vz(v0)) >> 4;
        const int64_t bx = (vx(v2) - vx(v0)) >> 4;
        const int64_t by = (vy(v2) - vy(v0)) >> 4;
        const int64_t bz = (vz(v2) - vz(v0)) >> 4;
        const int64_t cx = (ay * bz - az * by) >> 12;
        const int64_t cy = (az * bx - ax * bz) >> 12;
        const int64_t cz = (ax * by - ay * bx) >> 12;
        const uint32_t mag = isqrt64(static_cast<uint64_t>(cx * cx + cy * cy + cz * cz));
        if (mag > 0u) {
            write_be32(pol + poff + 0u, static_cast<uint32_t>((cx * kScale16) / static_cast<int64_t>(mag)));
            write_be32(pol + poff + 4u, static_cast<uint32_t>((cy * kScale16) / static_cast<int64_t>(mag)));
            write_be32(pol + poff + 8u, static_cast<uint32_t>((cz * kScale16) / static_cast<int64_t>(mag)));
        }

        write_be16(pol + aoff + 0u, 0x0007u);
        write_be16(pol + aoff + 2u, texno);
        write_be16(pol + aoff + 4u, 0x0080u);
        write_be16(pol + aoff + 6u, colno);
        write_be16(pol + aoff + 8u, 0x0000u);
        write_be16(pol + aoff + 10u, dir);
    };

    int pi = 0;
    // Top concave cup interior (p0..p7, bank 4 silver tex 0)
    for (int k = 0; k < 8; ++k) {
        const uint16_t r0a = static_cast<uint16_t>(1 + k);
        const uint16_t r0b = static_cast<uint16_t>(1 + ((k + 1) & 7));
        write_poly(pi++, r0b, r0a, 0u, 0u, 431u + 2u, 0x0040u, 2u);
    }
    // Silver Chalice outer bands (rings 0..5 -> 5 bands x 8 = 40 polys, p8..p47, bank 4)
    for (int b = 0; b < 5; ++b) {
        const int r_top = 1 + b * 8;
        const int r_bot = 1 + (b + 1) * 8;
        for (int k = 0; k < 8; ++k) {
            const int kn = (k + 1) & 7;
            // Band 2 (c2->c3) displays the ornate silver embossed medallion frieze (tex 45..47)
            const uint16_t texno = (b == 2)
                ? static_cast<uint16_t>(431u + 45u + (k % 3))
                : static_cast<uint16_t>(431u + ((b == 0) ? 1u : 0u));
            write_poly(pi++, static_cast<uint16_t>(r_top + k), static_cast<uint16_t>(r_top + kn),
                       static_cast<uint16_t>(r_bot + kn), static_cast<uint16_t>(r_bot + k),
                       texno, 0x0040u, 2u);
        }
    }
    // Fluted Silver Stem & Pedestal Foot (rings 6..8 -> 2 bands x 8 = 16 polys, p48..p63, bank 4, tex 56)
    for (int b = 0; b < 2; ++b) {
        const int r_top = 1 + (6 + b) * 8;
        const int r_bot = 1 + (7 + b) * 8;
        for (int k = 0; k < 8; ++k) {
            const int kn = (k + 1) & 7;
            write_poly(pi++, static_cast<uint16_t>(r_top + k), static_cast<uint16_t>(r_top + kn),
                       static_cast<uint16_t>(r_bot + kn), static_cast<uint16_t>(r_bot + k),
                       487u, 0x0040u, 2u);
        }
    }
    // Big 4-Tiered Dark Wooden Base (rings 8->16 -> 4 step shelves + 4 plaque walls = 8 bands x 8 = 64 polys, bank 5)
    for (int b = 0; b < 8; ++b) {
        const int r_top = 1 + (8 + b) * 8;
        const int r_bot = 1 + (9 + b) * 8;
        const bool is_wall = (b & 1) == 1;
        const int tier = b >> 1;
        for (int k = 0; k < 8; ++k) {
            const int kn = (k + 1) & 7;
            // Shelves use polished mahogany bevel tile (tex 44); Walls use staggered gold-plaque wood panels (tex 48..55)
            const uint16_t texno = is_wall
                ? static_cast<uint16_t>(431u + 48u + ((k + tier * 2) & 7))
                : static_cast<uint16_t>(431u + 44u);
            const uint16_t dir = (is_wall && (tier & 1)) ? 18u : 2u;
            write_poly(pi++, static_cast<uint16_t>(r_top + k), static_cast<uint16_t>(r_top + kn),
                       static_cast<uint16_t>(r_bot + kn), static_cast<uint16_t>(r_bot + k),
                       texno, 0x0050u, dir);
        }
    }
    // Bottom plinth cap (p128..p131 = 4 quads closing b7 ring to v137)
    for (int q = 0; q < 4; ++q) {
        const int k0 = q * 2;
        const int k1 = (k0 + 1) & 7;
        const int k2 = (k0 + 2) & 7;
        write_poly(pi++, static_cast<uint16_t>(1 + 16 * 8 + k0),
                   static_cast<uint16_t>(1 + 16 * 8 + k1),
                   static_cast<uint16_t>(1 + 16 * 8 + k2),
                   137u, 431u + 44u, 0x0050u, 2u);
    }

    // 2. CUP_CEL.DAT (0x00636FCC): Replace soccer-ball textures & World Cup ring with Silver Chalice + Wood/Gold Base
    uint8_t* cel = cart_rom + TeamAssetOffsets::kCupCelOff;
    const uint32_t tex_cnt = read_be32(cel + 8u);
    uint32_t pos = 12u + tex_cnt * 4u;
    uint32_t tex_off[57] = {};
    uint8_t  tex_w[57]   = {};
    uint8_t  tex_h[57]   = {};
    for (uint32_t i = 0u; i < tex_cnt && i < 57u; ++i) {
        const uint8_t w = cel[12u + i * 4u + 1u];
        const uint8_t h = cel[12u + i * 4u + 2u];
        const uint8_t fmt = cel[12u + i * 4u + 3u];
        tex_off[i] = pos;
        tex_w[i] = w;
        tex_h[i] = h;
        pos += (fmt == 3u) ? ((static_cast<uint32_t>(w) * h) / 2u) : (static_cast<uint32_t>(w) * h);
    }
    auto set_tex_px = [&](uint32_t ti, int x, int y, uint8_t nib) {
        if (ti >= tex_cnt || ti >= 57u) return;
        const int w = tex_w[ti];
        const int h = tex_h[ti];
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        const uint32_t b_off = tex_off[ti] + static_cast<uint32_t>((y * w + x) / 2);
        if (b_off >= TeamAssetOffsets::kCupCelBytes) return;
        if ((x & 1) == 0) {
            cel[b_off] = static_cast<uint8_t>((cel[b_off] & 0x0Fu) | ((nib & 0xFu) << 4));
        } else {
            cel[b_off] = static_cast<uint8_t>((cel[b_off] & 0xF0u) | (nib & 0xFu));
        }
    };

    // 2a. Silver chalice smooth chrome textures (tex 0..2): glossy polished sterling highlights
    for (uint32_t ti = 0u; ti <= 2u; ++ti) {
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                uint8_t c = 4u;
                if (ti == 2u) {
                    // Recessed dark interior floor of cup
                    c = static_cast<uint8_t>(std::clamp(9 + (y / 3), 9, 15));
                } else if (ti == 1u) {
                    // Bright top rim specular lip + subtle concave neck curve
                    if (y <= 2) c = 1u;
                    else if (y <= 4) c = 3u;
                    else c = static_cast<uint8_t>(std::clamp(4 + (x < 4 || x > 11 ? 3 : 0) + (y / 5), 2, 10));
                } else {
                    // Smooth vertical mirror chrome gradient across amphora body
                    const int dist = (x > 6) ? (x - 6) : (6 - x);
                    int shade = 2 + (dist * 5) / 8 + (y > 11 ? 1 : 0);
                    if (dist <= 1 && y >= 2 && y <= 13) shade = 1;
                    c = static_cast<uint8_t>(std::clamp(shade, 1, 11));
                }
                set_tex_px(ti, x, y, c);
            }
        }
    }

    // 2b. Polished mahogany bevel wood shelf (tex 44, 16x16, bank 5 indices 1..5)
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const uint8_t c = (y <= 3) ? 1u : ((y <= 9) ? 2u : ((y <= 13) ? 3u : 4u));
            set_tex_px(44u, x, y, c);
        }
    }

    // 2c. Fluted silver stem & flared foot (tex 56, 48x200, bank 4)
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 48; ++x) {
            // 3 vertical twisted flutes across 48px + bright collar rings at top/foot
            uint8_t c = 5u;
            if (y < 14 || y >= 178) {
                c = (y < 6 || (y >= 178 && y <= 188)) ? 2u : 6u;
            } else {
                const int fx = (x + y / 4) % 16;
                const int dist = (fx > 7) ? (fx - 7) : (7 - fx);
                c = static_cast<uint8_t>(std::clamp(1 + dist, 1, 11));
            }
            set_tex_px(56u, x, y, c);
        }
    }

    // 2d. 4-Tiered Dark Mahogany Base walls with Gold Oval Winner Plaques (tex 48..55, 8 x 48x64, bank 5)
    for (uint32_t p = 0u; p < 8u; ++p) {
        const uint32_t ti = 48u + p;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 48; ++x) {
                // Dark polished mahogany background with top rim glint and bottom recess shadow
                uint8_t col = 3u;
                if (y <= 3) col = 1u;
                else if (y <= 8) col = 2u;
                else if (y >= 58) col = 5u;
                else if (y >= 52) col = 4u;
                else if (((x + y * 2) % 13) == 0) col = 4u; // subtle wood grain

                // Draw 2 gleaming gold/brass oval winner plaques centered per 48x64 panel (x=12 & x=36, y=32)
                for (int px_c : {12, 36}) {
                    const int dx = x - px_c;
                    const int dy = y - 32;
                    const int r2 = (dx * dx) * 100 / (10 * 10) + (dy * dy) * 100 / (19 * 19);
                    if (r2 <= 118) {
                        if (r2 > 96) {
                            col = 6u; // crisp dark mahogany bezel around oval plaque
                        } else if (r2 > 82) {
                            col = (dy < 0) ? 7u : 11u; // beveled bronze-gold frame
                        } else {
                            // Gold plate fill + specular top-left highlight + engraved crest/text lines
                            const bool engrave = ((dy >= -5 && dy <= -2 && dx >= -4 && dx <= 4) ||
                                                  (dy >= 3 && dy <= 5 && dx >= -5 && dx <= 5));
                            if (engrave) col = 12u;
                            else col = (dx + dy < -6) ? 7u : ((dy < 4) ? 8u : 9u);
                        }
                    }
                }
                set_tex_px(ti, x, y, col);
            }
        }
    }
}

static void build_copa_msg_rank(uint8_t* cart_rom, const uint8_t* orig_mr) {
    uint8_t* mr = cart_rom + TeamAssetOffsets::kMsgRankOff;
    struct LetterSlice {
        char ch;
        uint8_t src_slot;
        uint8_t c0;
        uint8_t width;
        bool clean_v_corner;
    };
    static constexpr LetterSlice kSlices[] = {
        {'A',  5u, 12u, 7u, false},
        {'B',  0u,  0u, 6u, false},
        {'C',  5u, 25u, 7u, false},
        {'D',  8u, 38u, 6u, false},
        {'E',  5u, 32u, 6u, false},
        {'G', 14u, 13u, 7u, false},
        {'H',  8u,  0u, 6u, false},
        {'I',  0u, 25u, 2u, false},
        {'L',  8u, 13u, 6u, false},
        {'N',  8u, 32u, 6u, false},
        {'O',  8u,  6u, 7u, false},
        {'P',  1u, 13u, 6u, false},
        {'R',  5u,  6u, 6u, false},
        {'U', 14u,  7u, 6u, false},
        {'V', 14u, 45u, 7u, true },
        {'Y', 14u,  0u, 7u, false},
        {'Z',  0u, 19u, 6u, false},
    };
    auto find_slice = [&](char c) -> const LetterSlice* {
        for (const auto& s : kSlices) {
            if (s.ch == c) return &s;
        }
        return nullptr;
    };
    auto read_orig_slot_cell = [&](uint8_t slot, int row, int col) -> uint16_t {
        const uint32_t t_off = read_be32(orig_mr + 8u + (120u + slot) * 4u) - 0x002B0000u;
        const uint32_t p_map = read_be32(orig_mr + t_off + 2u) - 0x002B0000u;
        const uint8_t w = orig_mr[p_map + 4u];
        return read_be16(orig_mr + p_map + 6u + static_cast<size_t>(row * w + col) * 2u);
    };

    struct WinnerMapSpec {
        uint8_t slot;
        const char* name;
        uint32_t dest_rel;
    };
    // Packed into non-kept country map blocks (0 BRA, 3 ARG, 10 MEX, 12 COL, 15 USA remain untouched)
    static constexpr WinnerMapSpec kTargets[CopaModBackup::kModSlotCount] = {
        {14u, "BOLIVIA",   0x3BF2u}, // Block A [0x3BF2..0x4026]
        { 1u, "PERU",      0x3BF2u}, // packed sequentially inside Block A (no overlap)
        { 7u, "VENEZUELA", 0x4312u}, // Block B [0x4312..0x4E7A]
        { 6u, "ECUADOR",   0x4312u},
        {11u, "URUGUAY",   0x4312u},
        { 5u, "CHILE",     0x4312u},
        {13u, "PARAGUAY",  0x55B6u}, // Block C [0x55B6..0x5BFE]
    };

    uint32_t next_a = 0x3BF2u;
    uint32_t next_b = 0x4312u;
    uint32_t next_c = 0x55B6u;
    for (const auto& tgt : kTargets) {
        const int len = static_cast<int>(std::strlen(tgt.name));
        uint16_t rows[7][72]{};
        int cur_w = 0;
        for (int li = 0; li < len; ++li) {
            const LetterSlice* sl = find_slice(tgt.name[li]);
            if (!sl) continue;
            for (int r = 0; r < 7; ++r) {
                for (int c = 0; c < sl->width; ++c) {
                    uint16_t cell = read_orig_slot_cell(sl->src_slot, r, sl->c0 + c);
                    if (sl->clean_v_corner && c == 0 && r >= 4) cell = 0x2000u;
                    rows[r][cur_w + c] = cell;
                }
            }
            cur_w += sl->width;
            if (li + 1 < len) {
                const LetterSlice* nxt = find_slice(tgt.name[li + 1]);
                if (nxt) {
                    bool last_nb = false, nxt_nb = false;
                    for (int r = 0; r < 7; ++r) {
                        if (rows[r][cur_w - 1] != 0x2000u) last_nb = true;
                        if (read_orig_slot_cell(nxt->src_slot, r, nxt->c0) != 0x2000u) nxt_nb = true;
                    }
                    if (last_nb && nxt_nb) {
                        for (int r = 0; r < 7; ++r) rows[r][cur_w] = 0x2000u;
                        ++cur_w;
                    }
                }
            }
        }
        const uint32_t byte_len = 6u + static_cast<uint32_t>(cur_w) * 14u;
        uint32_t d_off = 0u;
        if (tgt.dest_rel == 0x3BF2u) {
            d_off = next_a;
            next_a += byte_len;
        } else if (tgt.dest_rel == 0x4312u) {
            d_off = next_b;
            next_b += byte_len;
        } else {
            d_off = next_c;
            next_c += byte_len;
        }
        mr[d_off + 0u] = 0x00u;
        mr[d_off + 1u] = 0x00u;
        mr[d_off + 2u] = 0x2Cu;
        mr[d_off + 3u] = 0x2Bu;
        mr[d_off + 4u] = static_cast<uint8_t>(cur_w);
        mr[d_off + 5u] = 7u;
        for (int r = 0; r < 7; ++r) {
            for (int c = 0; c < cur_w; ++c) {
                write_be16(mr + d_off + 6u + static_cast<size_t>(r * cur_w + c) * 2u, rows[r][c]);
            }
        }
        // Redirect tbl[120 + slot]'s 0x001D banner pointer cleanly to dest_rel
        const uint32_t t_off = read_be32(orig_mr + 8u + (120u + tgt.slot) * 4u) - 0x002B0000u;
        write_be32(mr + t_off + 2u, 0x002B0000u + d_off);
    }
}

static uint8_t find_nearest_pal_u8(const uint8_t* pal512, uint8_t r, uint8_t g, uint8_t b, int start_idx = 1) {
    int best_i = start_idx;
    int best_d = 0x7FFFFFFF;
    for (int i = start_idx; i < 256; ++i) {
        const uint16_t c = read_be16(pal512 + static_cast<size_t>(i) * 2u);
        const int pr = static_cast<int>((c & 0x1Fu) << 3);
        const int pg = static_cast<int>(((c >> 5) & 0x1Fu) << 3);
        const int pb = static_cast<int>(((c >> 10) & 0x1Fu) << 3);
        const int dr = pr - static_cast<int>(r);
        const int dg = pg - static_cast<int>(g);
        const int db = pb - static_cast<int>(b);
        const int dd = dr * dr + dg * dg + db * db;
        if (dd < best_d) {
            best_d = dd;
            best_i = i;
        }
    }
    return static_cast<uint8_t>(best_i);
}

static void build_copa_tex_en(uint8_t* cart_rom) {
    for (const auto& meta : kTexEnMeta) {
        uint8_t* tex = cart_rom + meta.off;
        uint8_t* pal = tex + 12u;
        uint8_t* px = tex + 532u;
        const int cx = meta.cup_cx;

        auto rgb_at = [&](uint8_t idx, int& r, int& g, int& b) {
            const uint16_t c = read_be16(pal + static_cast<size_t>(idx) * 2u);
            r = static_cast<int>((c & 0x1Fu) << 3);
            g = static_cast<int>(((c >> 5) & 0x1Fu) << 3);
            b = static_cast<int>(((c >> 10) & 0x1Fu) << 3);
        };

        // 1. Cleanly remove wider golden World Cup globe edges and replace with Silver Copa America Chalice
        for (int y = 7; y < 56; ++y) {
            for (int x = cx - 13; x <= cx + 13; ++x) {
                int r, g, b;
                rgb_at(px[y * 352 + x], r, g, b);
                if ((r >= 135 && g >= 100 && b <= 80 && r > b + 45) || (y <= 34 && g >= 110 && b <= 75)) {
                    const int src_x = (x < cx) ? (x - 24) : (x + 24);
                    if (src_x >= 0 && src_x < 352) {
                        px[y * 352 + x] = px[y * 352 + src_x];
                    }
                }
            }
        }
        for (int dy = 0; dy < 47; ++dy) {
            const int y = 8 + dy;
            for (int dx = -12; dx <= 12; ++dx) {
                const int x = cx + dx;
                const int ax = (dx < 0) ? -dx : dx;
                bool has_px = false;
                uint8_t dr = 0, dg = 0, db = 0;
                if (dy <= 22) {
                    // Authentic handleless Silver Copa America Amphora: lip (0..1) -> neck (2..6) -> shoulder/belt (7..11) -> bowl (12..22)
                    int bw100 = 220;
                    if (dy <= 1) bw100 = 580;
                    else if (dy <= 5) bw100 = 400 + ((dy - 3) * (dy - 3)) * 22;
                    else if (dy <= 10) bw100 = 520 + ((dy - 5) * 280) / 5;
                    else bw100 = 220 + ((22 - dy) * 580) / 12;
                    if (ax * 100 <= bw100) {
                        has_px = true;
                        const int nx100 = ((dx + 2) * 100) / std::max(bw100 / 100, 1);
                        const int dnx = (nx100 + 25 < 0) ? -(nx100 + 25) : (nx100 + 25);
                        const int hl = std::max(0, 100 - (dnx * 115) / 100);
                        int shade = 128 + (hl * 124) / 100;
                        if (dy == 0 || dy == 1) shade = (ax >= 4) ? 236 : 84;
                        else if (dy >= 7 && dy <= 10 && ((ax + dy) % 3) == 0) shade = std::max(95, shade - 50);
                        if (ax * 100 >= bw100 - 110) shade = std::max(72, shade - 45);
                        dr = static_cast<uint8_t>(std::min(255, shade));
                        dg = static_cast<uint8_t>(std::min(255, shade + 4));
                        db = static_cast<uint8_t>(std::min(255, shade + 10));
                    }
                } else if (dy <= 28) {
                    const int sw = (dy <= 25) ? 2 : ((dy == 26) ? 3 : 5);
                    if (ax <= sw) {
                        has_px = true;
                        if (dx <= 0) { dr = 224u; dg = 230u; db = 240u; }
                        else { dr = 132u; dg = 138u; db = 150u; }
                    }
                } else {
                    // Wide 4-tier Dark Mahogany Wood base (dy=29..46) with Championship Gold plaques
                    const int tier = (dy < 33) ? 0 : ((dy < 38) ? 1 : ((dy < 42) ? 2 : 3));
                    const int pw = 6 + tier + ((tier >= 2) ? 1 : 0); // 6, 7, 9, 10 (wider than chalice!)
                    if (ax <= pw) {
                        has_px = true;
                        const bool in_mid_y = (dy == 30 || dy == 31 || dy == 34 || dy == 35 || dy == 39 || dy == 40 || dy == 43 || dy == 44);
                        const bool is_plaque = in_mid_y && ax <= pw - 1 && ((ax % 4) != 0);
                        if (is_plaque) {
                            if (dx <= 0) { dr = 242u; dg = 208u; db = 94u; }
                            else { dr = 196u; dg = 152u; db = 44u; }
                        } else {
                            if (dx <= 0) { dr = 98u; dg = 52u; db = 28u; }
                            else { dr = 62u; dg = 30u; db = 16u; }
                        }
                    }
                }
                if (has_px) {
                    px[y * 352 + x] = find_nearest_pal_u8(pal, dr, dg, db);
                }
            }
        }

        // 2. Recolor repurposed teams' champion photos strictly inside shirt/shorts bands (excluding faces/grass)
        const uint8_t s = meta.slot;
        if (s == 1u || s == 5u || s == 6u || s == 7u || s == 11u || s == 13u || s == 14u) {
            for (int y = 74; y <= 198; ++y) {
                const bool in_shirt = (y >= 74 && y <= 125) || (y >= 142 && y <= 176);
                const bool in_shorts = (y >= 124 && y <= 143) || (y >= 175 && y <= 195);
                if (!in_shirt && !in_shorts) continue;
                for (int x = 22; x < 330; ++x) {
                    int r, g, b;
                    rgb_at(px[y * 352 + x], r, g, b);
                    const int l = (r * 30 + g * 59 + b * 11) / 100;
                    const bool is_skin = (r > 110 && g > 65 && b > 35 && r > g && g > b && (r - g) < 58 && (g - b) < 50);
                    if (is_skin) continue;
                    if (s == 1u && in_shirt) { // PER: JPN blue -> crisp white + crimson diagonal sash
                        if (b > r + 28 && b > g + 12 && b > 80) {
                            const int diag = (x + y * 2) % 34;
                            if (diag >= 9 && diag < 17) {
                                px[y * 352 + x] = find_nearest_pal_u8(pal,
                                    static_cast<uint8_t>(std::min(255, (l * 145) / 100 + 40)),
                                    static_cast<uint8_t>(std::max(12, l / 5)),
                                    static_cast<uint8_t>(std::max(14, l / 5)));
                            } else {
                                const uint8_t v = static_cast<uint8_t>(std::min(248, (l * 115) / 100 + 55));
                                px[y * 352 + x] = find_nearest_pal_u8(pal, v, v, static_cast<uint8_t>(std::min(255, v + 4)));
                            }
                        }
                    } else if (s == 5u && in_shirt) { // CHI: FRA blue -> crimson red
                        if (b > r + 28 && b > g + 15 && b > 75) {
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::min(255, l + 75)),
                                static_cast<uint8_t>(std::max(14, l / 4)),
                                static_cast<uint8_t>(std::max(18, l / 4)));
                        }
                    } else if (s == 6u && in_shirt) { // ECU: ESP deep red -> gold yellow
                        if (r > 125 && r - g > 65 && r - b > 60) {
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::min(255, l + 115)),
                                static_cast<uint8_t>(std::min(245, l + 95)),
                                static_cast<uint8_t>(std::max(10, l / 5)));
                        }
                    } else if (s == 7u && in_shirt) { // VEN: KOR red -> vinotinto burgundy
                        if (r > 125 && r - g > 65 && r - b > 60) {
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::clamp(l + 15, 60, 185)),
                                static_cast<uint8_t>(std::max(14, l / 5)),
                                static_cast<uint8_t>(std::max(32, l / 3)));
                        }
                    } else if (s == 11u && in_shirt) { // URU: GER white -> sky blue celeste
                        const int rb_diff = (r > b) ? (r - b) : (b - r);
                        if (r > 145 && g > 145 && b > 145 && rb_diff < 28) {
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::max(45, l - 95)),
                                static_cast<uint8_t>(std::min(240, l - 5)),
                                static_cast<uint8_t>(std::min(255, l + 40)));
                        }
                    } else if (s == 13u) { // PAR: CMR green/red -> red-white vertical stripes + royal blue shorts
                        if (in_shirt && g > r + 14 && g > b + 16 && g > 70) {
                            if ((x % 8) < 4) {
                                px[y * 352 + x] = find_nearest_pal_u8(pal,
                                    static_cast<uint8_t>(std::min(255, l + 85)),
                                    static_cast<uint8_t>(std::max(18, l / 4)),
                                    static_cast<uint8_t>(std::max(20, l / 4)));
                            } else {
                                const uint8_t v = static_cast<uint8_t>(std::min(248, l + 90));
                                px[y * 352 + x] = find_nearest_pal_u8(pal, v, v, v);
                            }
                        } else if (in_shorts && r > 145 && r - g > 65 && r - b > 65) {
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::max(18, l / 4)),
                                static_cast<uint8_t>(std::max(40, l / 2)),
                                static_cast<uint8_t>(std::min(240, l + 85)));
                        }
                    } else if (s == 14u && in_shirt) { // BOL: YUG white+blue sleeves -> dark forest green
                        const int rb_diff = (r > b) ? (r - b) : (b - r);
                        const bool is_blue = (b > r + 24 && b > g + 8 && b > 80);
                        const bool is_white = (r > 135 && g > 135 && b > 138 && rb_diff < 35);
                        if (is_blue || is_white) {
                            const int gl = is_white ? l : std::min(220, (l * 14) / 10 + 30);
                            px[y * 352 + x] = find_nearest_pal_u8(pal,
                                static_cast<uint8_t>(std::max(8, gl / 5)),
                                static_cast<uint8_t>(std::clamp((gl * 82) / 100, 50, 195)),
                                static_cast<uint8_t>(std::max(14, gl / 4)));
                        }
                    }
                }
            }
        }
    }
}

static void build_copa_cel_en(uint8_t* cart_rom, uint8_t* canvas) {
    // Replace European sepia landmarks in 7 repurposed CEL_ENxx.DAT files (shown behind THANK YOU FOR PLAYING)
    // with a clean South American Copa America tribute emblem & country crest (preserving sec0/sec1 fonts & size).
    static constexpr const char* kCountryNames[CopaModBackup::kModSlotCount] = {
        "PERU", "CHILE", "ECUADOR", "VENEZUELA", "URUGUAY", "PARAGUAY", "BOLIVIA"
    };

    for (uint32_t m_idx = 0u; m_idx < CopaModBackup::kModSlotCount; ++m_idx) {
        uint8_t* cel = cart_rom + kCelEnMeta[m_idx].off;
        const uint32_t sz = kCelEnMeta[m_idx].size;
        const uint32_t fptr = read_be32(cel + 8u) - 0x0028C000u;
        if (fptr + 56u > sz) continue;
        const uint32_t s2_off = read_be32(cel + fptr + 16u) - 0x0028C000u;
        const uint32_t s2_tiles = (read_be32(cel + fptr + 20u) * 2u) / 32u;
        const uint32_t map_off = read_be32(cel + fptr + 48u) - 0x0028C000u;
        const uint32_t pal_off = read_be32(cel + fptr + 40u) - 0x0028C000u;
        if (s2_off != 0x49ACu || s2_tiles < 600u || map_off + 2468u > sz || pal_off + 32u > sz) continue;

        // Sort CEL_ENxx sepia palette indices [1..15] from darkest -> brightest so shading adapts seamlessly
        uint8_t ramp[15]{};
        int lum[16]{};
        for (int i = 1; i < 16; ++i) {
            const uint16_t c = read_be16(cel + pal_off + static_cast<size_t>(i) * 2u);
            const int r = static_cast<int>(c & 0x1Fu);
            const int g = static_cast<int>((c >> 5) & 0x1Fu);
            const int b = static_cast<int>((c >> 10) & 0x1Fu);
            lum[i] = r * 30 + g * 59 + b * 11;
            ramp[i - 1] = static_cast<uint8_t>(i);
        }
        std::sort(ramp, ramp + 15, [&](uint8_t a, uint8_t b) { return lum[a] < lum[b]; });
        auto shade_idx = [&](int level0_14) -> uint8_t {
            return ramp[std::clamp(level0_14, 0, 14)];
        };

        // Background stadium archways + spotlight beams (y=56..168 area clearest behind top/bottom text)
        for (int y = 0; y < 224; ++y) {
            for (int x = 0; x < 352; ++x) {
                int lv = 1;
                if (y < 8 || y >= 216 || x < 10 || x >= 342) {
                    lv = 0;
                } else {
                    const int dx1 = x - 110;
                    const int dx2 = x - 242;
                    const int ray1 = (dx1 * dx1 + (y + 20) * (y + 20)) / 900;
                    const int ray2 = (dx2 * dx2 + (y + 20) * (y + 20)) / 900;
                    lv = std::clamp(6 - std::min(ray1, ray2) / 4, 2, 5);
                    if (y >= 174 && y < 180) lv = 7;
                }
                canvas[y * 352 + x] = shade_idx(lv);
            }
        }

        // Left Championship Shield & Country Banner (x=36..176, y=62..164)
        for (int y = 64; y < 162; ++y) {
            for (int x = 38; x < 174; ++x) {
                const bool border = (x < 42 || x >= 170 || y < 68 || y >= 158);
                if (border) {
                    canvas[y * 352 + x] = shade_idx(12);
                } else {
                    // Stylised horizontal / vertical tri-band shade representing each flag texture
                    const int band = (y - 68) / 30;
                    const int lv = (band == 0) ? 4 : ((band == 1) ? 8 : 5);
                    canvas[y * 352 + x] = shade_idx(lv);
                }
            }
        }
        auto draw_str_sepia = [&](const char* txt, int x0, int y0, int scx, int scy, int fg, int sh) {
            const int n = static_cast<int>(std::strlen(txt));
            for (int i = 0; i < n; ++i) {
                const uint8_t* g = find_glyph_5x7(txt[i]);
                if (!g) continue;
                for (int gr = 0; gr < 7; ++gr) {
                    for (int gc = 0; gc < 5; ++gc) {
                        if ((g[gr] & (1u << (4 - gc))) == 0u) continue;
                        for (int sy = 0; sy < scy; ++sy) {
                            for (int sx = 0; sx < scx; ++sx) {
                                const int px = x0 + i * (5 * scx + scx) + gc * scx + sx;
                                const int py = y0 + gr * scy + sy;
                                if (px + 1 < 352 && py + 1 < 224) canvas[(py + 1) * 352 + px + 1] = shade_idx(sh);
                                if (px < 352 && py < 224) canvas[py * 352 + px] = shade_idx(fg);
                            }
                        }
                    }
                }
            }
        };
        const char* cname = kCountryNames[m_idx];
        const int clen = static_cast<int>(std::strlen(cname));
        const int cw_sc = (clen >= 8) ? 2 : 3;
        const int c_pix_w = clen * (5 * cw_sc + cw_sc) - cw_sc;
        draw_str_sepia(cname, 106 - c_pix_w / 2, 78, cw_sc, 3, 14, 1);
        draw_str_sepia("CAMPEON", 64, 114, 2, 2, 13, 1);
        draw_str_sepia("COPA AMERICA", 52, 136, 1, 2, 11, 1);

        // Right Large Sepia Silver Copa America Chalice Silhouette (cx=254, y=54..172)
        for (int dy = 0; dy < 118; ++dy) {
            const int y = 54 + dy;
            for (int dx = -36; dx <= 36; ++dx) {
                const int x = 254 + dx;
                const int ax = (dx < 0) ? -dx : dx;
                int lv = -1;
                if (dy <= 54) {
                    const int bw = 8 + ((54 - dy) * 20) / 54;
                    if (dy <= 4 && ax <= bw) {
                        lv = (dy == 0 || dy == 4 || ax >= bw - 2) ? 14 : 3;
                    } else if (ax <= bw) {
                        const int hl = std::max(0, 14 - (ax * 10) / std::max(bw, 1));
                        lv = (dx < 0) ? std::min(14, hl + 2) : std::max(5, hl - 2);
                    } else if (dy >= 12 && dy <= 40) {
                        const int ddy = dy - 26;
                        const int outer = bw + 9 - (ddy * ddy * 9) / 196;
                        if (ax >= bw + 2 && ax <= outer) lv = 11;
                    }
                } else if (dy <= 72) {
                    const int sw = (dy == 58 || dy == 68) ? 8 : 5;
                    if (ax <= sw) lv = (dx < 0) ? 13 : 8;
                } else {
                    const int pw = (dy < 86) ? 14 : ((dy < 102) ? 18 : 22);
                    if (ax <= pw) {
                        const bool plaque = (dy == 76 || dy == 77 || dy == 91 || dy == 92 || dy == 107 || dy == 108) && (ax <= pw - 4);
                        lv = plaque ? 14 : ((dx < 0) ? 7 : 4);
                    }
                }
                if (lv >= 0) canvas[y * 352 + x] = shade_idx(lv);
            }
        }

        // Deduplicate 44x28 tiles cleanly into s2_off (tile 0 = border/solid pad, s2_tiles >= 836)
        uint32_t next_tid = 0u;
        uint8_t tbuf[32]{};
        for (int tr = 0; tr < 28; ++tr) {
            for (int tc = 0; tc < 44; ++tc) {
                for (int py = 0; py < 8; ++py) {
                    for (int xb = 0; xb < 4; ++xb) {
                        const uint8_t p0 = canvas[(tr * 8 + py) * 352 + (tc * 8 + xb * 2)];
                        const uint8_t p1 = canvas[(tr * 8 + py) * 352 + (tc * 8 + xb * 2 + 1)];
                        tbuf[py * 4 + xb] = static_cast<uint8_t>(((p0 & 0xFu) << 4) | (p1 & 0xFu));
                    }
                }
                uint32_t found = s2_tiles;
                for (uint32_t c = 0u; c < next_tid; ++c) {
                    if (std::memcmp(cel + s2_off + c * 32u, tbuf, 32u) == 0) {
                        found = c;
                        break;
                    }
                }
                if (found == s2_tiles) {
                    if (next_tid < s2_tiles) {
                        found = next_tid++;
                        std::memcpy(cel + s2_off + found * 32u, tbuf, 32u);
                    } else {
                        found = 0u;
                    }
                }
                write_be16(cel + map_off + 4u + static_cast<size_t>(tr * 44 + tc) * 2u,
                           static_cast<uint16_t>(0x1000u | (found & 0x0FFFu)));
            }
        }
    }
}

static void write_copa_eprg_tables(uint8_t* sp_dst, uint8_t* tier_dst, uint8_t* grid_dst,
                                   uint8_t* p2_dst, const uint8_t* active_grid,
                                   uint8_t* hwram_or_cart_code, uint32_t code_base_bias) {
    for (uint32_t s = 0u; s < TeamAssetOffsets::kTeamCount; ++s) {
        sp_dst[s * 3u + 0u] = kTeamSpecs[s].sp[0];
        sp_dst[s * 3u + 1u] = kTeamSpecs[s].sp[1];
        sp_dst[s * 3u + 2u] = kTeamSpecs[s].sp[2];
    }
    std::memcpy(tier_dst, kCopaTiers, TeamAssetOffsets::kTierTblBytes);
    std::memcpy(grid_dst, active_grid, TeamAssetOffsets::kTeamGridBytes);
    write_be16(p2_dst, 0xE103u); // Initial cursor avoids hidden slot -> Argentina (3)

    // SP 3 (Bolivia): keep 0x0604EE28 (0x06040C40 zoom + darkening -96) active, redirect 0x0604EE40 emitter
    // literal (0x0604EFE0) from 0x0604114A to clean 40f wind/dust wait 0x06040D46 (with stack0=0 at 0x0604EE32),
    // and NOP only the 2 explosive fireball/puff JSR calls (0x0604EE6C, 0x0604EE7E).
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kSp3ArgHighWramOff + code_base_bias, 0xE200u);
    write_be32(hwram_or_cart_code + TeamAssetOffsets::kSp3EmitLitHighWramOff + code_base_bias,
               TeamAssetOffsets::kSp3CleanWaitFnAddr);
    for (int i = 0; i < 2; ++i) {
        const uint32_t off = TeamAssetOffsets::kSp3VfxCallHighWram[i] + code_base_bias;
        write_be16(hwram_or_cart_code + off, 0x0009u);
    }

    // Automatic Copa America 12-team kit-clash matrix (0x06069686)
    patch_kit_clash_matrix(hwram_or_cart_code + TeamAssetOffsets::kKitClashHighWramOff + code_base_bias);

    // Venezuela SP 19 waving flag band 0: Red (0x00DA) -> Yellow Gold (0x00DC)
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kVenSp19Band0HighWramOff + code_base_bias, 0x00DCu);

    // Bypass Euro/World slot 9->18 (RSA) & 13->17 (SCO) remap at 0x060050F8 -> rts; mov r4, r0
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kEuroRemapHighWramOff + code_base_bias + 0u, 0x000Bu);
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kEuroRemapHighWramOff + code_base_bias + 2u, 0x6043u);

    // Sterling Silver 3D Trophy & Ring Banner palettes (CRAM banks 4 & 5 at 0x0607077C)
    patch_cup_palettes(hwram_or_cart_code + TeamAssetOffsets::kCupPalHighWramOff + code_base_bias);

    // B-H5-08: 6 Tournament Rounds (0..2 Group, 3 QF, 4 SF, 5 Final) + 7th Round (6) All-Star -> Ending (7)
    static constexpr uint8_t kCopaRoundSched[TeamAssetOffsets::kRoundSchedBytes] = {
        0u, 1u, 1u, 2u, 2u, 3u, 4u, 4u
    };
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kCopaRoundEndCmpHighWramOff + code_base_bias, 0xEC07u);
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kCopaOppAllStarHighWramOff + code_base_bias,  0x8806u);
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kCopaContEndHighWramOff + code_base_bias,     0xE206u);
    write_be16(hwram_or_cart_code + TeamAssetOffsets::kCopaVsAllStarHighWramOff + code_base_bias,   0x8806u);
    std::memcpy(hwram_or_cart_code + TeamAssetOffsets::kRoundSchedHighWramOff + code_base_bias,
                kCopaRoundSched, TeamAssetOffsets::kRoundSchedBytes);
}

static void capture_classic_wram_backup(const StvMachineState& m, CopaModBackup& b) {
    if (b.wram_captured || m.copa_mode_active) return;
    const uint8_t* hw = m.inner.high_wram.data();
    const uint8_t* lw = m.inner.low_wram.data();
    const uint32_t euro_op = read_be32(hw + TeamAssetOffsets::kEuroRemapHighWramOff);
    const uint16_t p2_op   = read_be16(hw + TeamAssetOffsets::kP2CursorHighWramOff);
    const uint32_t tier_w  = read_be32(hw + TeamAssetOffsets::kTierTblHighWramOff);
    const uint32_t capa_w  = read_be32(lw + TeamAssetOffsets::kCapaTblLowWramOff);
    // Reject uninitialized zeros AND the BIOS "SEGASEGA" (0x53454741 / 0x47415345) fill pattern,
    // as well as already-patched Copa EPRG (0x000B6043), so we only ever snapshot genuine Classic WRAM.
    auto is_segasega = [](uint32_t v) -> bool {
        return v == 0u || v == 0x53454741u || v == 0x47415345u;
    };
    const bool wram_ready =
        (euro_op == 0xD3296030u) &&
        (p2_op == 0xE102u) &&
        !is_segasega(tier_w) &&
        !is_segasega(capa_w) &&
        has_nonzero(hw + TeamAssetOffsets::kTierTblHighWramOff, TeamAssetOffsets::kTierTblBytes) &&
        has_nonzero(lw + TeamAssetOffsets::kCapaTblLowWramOff, 64u);
    if (!wram_ready) return;

    std::memcpy(b.orig_capa_lwram.data(), m.inner.low_wram.data() + TeamAssetOffsets::kCapaTblLowWramOff, TeamAssetOffsets::kCapaAllBytes);
    std::memcpy(b.orig_capa_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kCapaTblHighWramOff, TeamAssetOffsets::kCapaAllBytes);
    std::memcpy(b.orig_sp_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kSpTblHighWramOff, TeamAssetOffsets::kSpTblTotalBytes);
    std::memcpy(b.orig_master_skin_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kMasterSkinHighWramOff, TeamAssetOffsets::kMasterSkinBytes);
    std::memcpy(b.orig_tiers_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kTierTblHighWramOff, TeamAssetOffsets::kTierTblBytes);
    std::memcpy(b.orig_grid_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kTeamGridHighWramOff, TeamAssetOffsets::kTeamGridBytes);
    std::memcpy(b.orig_p2_cursor_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kP2CursorHighWramOff, 2u);
    for (int i = 0; i < 2; ++i) {
        std::memcpy(b.orig_sp3_vfx_hwram.data() + i * 2, m.inner.high_wram.data() + TeamAssetOffsets::kSp3VfxCallHighWram[i], 2u);
    }
    std::memcpy(b.orig_sp3_arg_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kSp3ArgHighWramOff, 2u);
    std::memcpy(b.orig_sp3_emit_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kSp3EmitLitHighWramOff, 4u);
    std::memcpy(b.orig_kit_clash_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kKitClashHighWramOff, TeamAssetOffsets::kKitClashBytes);
    std::memcpy(b.orig_ven_band0_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kVenSp19Band0HighWramOff, 2u);
    std::memcpy(b.orig_euro_remap_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kEuroRemapHighWramOff, TeamAssetOffsets::kEuroRemapBytes);
    std::memcpy(b.orig_cup_pal_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kCupPalHighWramOff, TeamAssetOffsets::kCupPalBytes);
    std::memcpy(b.orig_round_ops_hwram.data() + 0u, m.inner.high_wram.data() + TeamAssetOffsets::kCopaRoundEndCmpHighWramOff, 2u);
    std::memcpy(b.orig_round_ops_hwram.data() + 2u, m.inner.high_wram.data() + TeamAssetOffsets::kCopaOppAllStarHighWramOff,  2u);
    std::memcpy(b.orig_round_ops_hwram.data() + 4u, m.inner.high_wram.data() + TeamAssetOffsets::kCopaContEndHighWramOff,     2u);
    std::memcpy(b.orig_round_ops_hwram.data() + 6u, m.inner.high_wram.data() + TeamAssetOffsets::kCopaVsAllStarHighWramOff,   2u);
    std::memcpy(b.orig_round_sched_hwram.data(), m.inner.high_wram.data() + TeamAssetOffsets::kRoundSchedHighWramOff, TeamAssetOffsets::kRoundSchedBytes);
    b.wram_captured = true;
}

static void capture_classic_backup(const StvMachineState& m, CopaModBackup& b) {
    if (!b.captured) {
        for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
            const uint8_t s = kModSlots[i];
            std::memcpy(b.orig_tflag[i].data(),
                        m.cart_rom + TeamAssetOffsets::kTFlagBaseOff + s * TeamAssetOffsets::kTFlagStride,
                        TeamAssetOffsets::kTFlagSize);

            std::memcpy(b.orig_bgtm[i].data(), m.cart_rom + kBgTmMeta[i].off, kBgTmMeta[i].size);

            std::memcpy(b.orig_textm[i].data(),
                        m.cart_rom + TeamAssetOffsets::kTexTmBaseOff + s * TeamAssetOffsets::kTexStride,
                        TeamAssetOffsets::kTexStride);

            std::memcpy(b.orig_texhm[i].data(),
                        m.cart_rom + TeamAssetOffsets::kTexHmBaseOff + s * TeamAssetOffsets::kTexStride,
                        TeamAssetOffsets::kTexStride);

            std::memcpy(b.orig_cel_pals.data() + i * 32u,
                        m.cart_rom + TeamAssetOffsets::kCelOutPalsBaseOff + static_cast<size_t>(kCelPalIds[i]) * 32u,
                        32u);
            std::memcpy(b.orig_msg_maps.data() + i * TeamAssetOffsets::kMsgOutMapBytes,
                        m.cart_rom + TeamAssetOffsets::kMsgOutBaseOff + s * TeamAssetOffsets::kMsgOutMapBytes,
                        TeamAssetOffsets::kMsgOutMapBytes);
        }

        for (uint32_t s = 0u; s < TeamAssetOffsets::kTeamCount; ++s) {
            b.orig_msg_pos_xy[s] = read_be16(
                m.cart_rom + TeamAssetOffsets::kMsgOutPosBaseOff +
                static_cast<size_t>(s) * TeamAssetOffsets::kMsgOutPosStride +
                TeamAssetOffsets::kMsgOutPosXyOff);
        }

        for (uint32_t i = 0u; i < CopaModBackup::kHiddenMsgCount; ++i) {
            const uint8_t s = kHiddenMsgSlots[i];
            std::memcpy(b.orig_hidden_msg_maps.data() + i * TeamAssetOffsets::kMsgOutMapBytes,
                        m.cart_rom + TeamAssetOffsets::kMsgOutBaseOff + s * TeamAssetOffsets::kMsgOutMapBytes,
                        TeamAssetOffsets::kMsgOutMapBytes);
        }

        for (uint32_t i = 0u; i < CopaModBackup::kSafeCelPoolSize; ++i) {
            std::memcpy(b.orig_cel_pool_tiles.data() + i * 32u,
                        m.cart_rom + TeamAssetOffsets::kCelOutTilesBaseOff + static_cast<size_t>(kSafeCelOutTilePool[i]) * 32u,
                        32u);
        }

        static constexpr uint32_t kM = TeamAssetOffsets::kEprgMirrorDelta;
        std::memcpy(b.orig_capa_cart.data(), m.cart_rom + TeamAssetOffsets::kCapaTblSlice0Off, TeamAssetOffsets::kCapaAllBytes);
        std::memcpy(b.orig_capa_eprg.data(), m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff, TeamAssetOffsets::kCapaAllBytes);
        std::memcpy(b.orig_capa_eprg_m.data(), m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff + kM, TeamAssetOffsets::kCapaAllBytes);
        std::memcpy(b.orig_sp_cart.data(), m.cart_rom + TeamAssetOffsets::kSpTblCartOff, TeamAssetOffsets::kSpTblTotalBytes);
        std::memcpy(b.orig_sp_cart_m.data(), m.cart_rom + TeamAssetOffsets::kSpTblCartOff + kM, TeamAssetOffsets::kSpTblTotalBytes);
        std::memcpy(b.orig_master_skin_cart.data(), m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff, TeamAssetOffsets::kMasterSkinBytes);
        std::memcpy(b.orig_master_skin_cart_m.data(), m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff + kM, TeamAssetOffsets::kMasterSkinBytes);
        std::memcpy(b.orig_tiers_cart.data(), m.cart_rom + TeamAssetOffsets::kTierTblCartOff, TeamAssetOffsets::kTierTblBytes);
        std::memcpy(b.orig_tiers_cart_m.data(), m.cart_rom + TeamAssetOffsets::kTierTblCartOff + kM, TeamAssetOffsets::kTierTblBytes);
        std::memcpy(b.orig_grid_cart.data(), m.cart_rom + TeamAssetOffsets::kTeamGridCartOff, TeamAssetOffsets::kTeamGridBytes);
        std::memcpy(b.orig_grid_cart_m.data(), m.cart_rom + TeamAssetOffsets::kTeamGridCartOff + kM, TeamAssetOffsets::kTeamGridBytes);
        std::memcpy(b.orig_p2_cursor_cart.data(), m.cart_rom + TeamAssetOffsets::kP2CursorCartOff, 2u);
        std::memcpy(b.orig_p2_cursor_cart_m.data(), m.cart_rom + TeamAssetOffsets::kP2CursorCartOff + kM, 2u);
        for (int i = 0; i < 2; ++i) {
            std::memcpy(b.orig_sp3_vfx_cart.data() + i * 2, m.cart_rom + TeamAssetOffsets::kSp3VfxCallCart[i], 2u);
            std::memcpy(b.orig_sp3_vfx_cart_m.data() + i * 2, m.cart_rom + TeamAssetOffsets::kSp3VfxCallCart[i] + kM, 2u);
        }
        std::memcpy(b.orig_sp3_arg_cart.data(), m.cart_rom + TeamAssetOffsets::kSp3ArgCartOff, 2u);
        std::memcpy(b.orig_sp3_arg_cart_m.data(), m.cart_rom + TeamAssetOffsets::kSp3ArgCartOff + kM, 2u);
        std::memcpy(b.orig_sp3_emit_cart.data(), m.cart_rom + TeamAssetOffsets::kSp3EmitLitCartOff, 4u);
        std::memcpy(b.orig_sp3_emit_cart_m.data(), m.cart_rom + TeamAssetOffsets::kSp3EmitLitCartOff + kM, 4u);
        std::memcpy(b.orig_kit_clash_cart.data(), m.cart_rom + TeamAssetOffsets::kKitClashCartOff, TeamAssetOffsets::kKitClashBytes);
        std::memcpy(b.orig_kit_clash_cart_m.data(), m.cart_rom + TeamAssetOffsets::kKitClashCartOff + kM, TeamAssetOffsets::kKitClashBytes);
        std::memcpy(b.orig_ven_band0_cart.data(), m.cart_rom + TeamAssetOffsets::kVenSp19Band0CartOff, 2u);
        std::memcpy(b.orig_ven_band0_cart_m.data(), m.cart_rom + TeamAssetOffsets::kVenSp19Band0CartOff + kM, 2u);
        std::memcpy(b.orig_texgm_ven_flag.data(), m.cart_rom + TeamAssetOffsets::kTexGmVenFlagOff, TeamAssetOffsets::kTexGmVenFlagBytes);
        std::memcpy(b.orig_euro_remap_cart.data(), m.cart_rom + TeamAssetOffsets::kEuroRemapCartOff, TeamAssetOffsets::kEuroRemapBytes);
        std::memcpy(b.orig_euro_remap_cart_m.data(), m.cart_rom + TeamAssetOffsets::kEuroRemapCartOff + kM, TeamAssetOffsets::kEuroRemapBytes);
        std::memcpy(b.orig_round_ops_cart.data() + 0u, m.cart_rom + TeamAssetOffsets::kCopaRoundEndCmpCartOff, 2u);
        std::memcpy(b.orig_round_ops_cart.data() + 2u, m.cart_rom + TeamAssetOffsets::kCopaOppAllStarCartOff,  2u);
        std::memcpy(b.orig_round_ops_cart.data() + 4u, m.cart_rom + TeamAssetOffsets::kCopaContEndCartOff,     2u);
        std::memcpy(b.orig_round_ops_cart.data() + 6u, m.cart_rom + TeamAssetOffsets::kCopaVsAllStarCartOff,   2u);
        std::memcpy(b.orig_round_ops_cart_m.data() + 0u, m.cart_rom + TeamAssetOffsets::kCopaRoundEndCmpCartOff + kM, 2u);
        std::memcpy(b.orig_round_ops_cart_m.data() + 2u, m.cart_rom + TeamAssetOffsets::kCopaOppAllStarCartOff + kM,  2u);
        std::memcpy(b.orig_round_ops_cart_m.data() + 4u, m.cart_rom + TeamAssetOffsets::kCopaContEndCartOff + kM,     2u);
        std::memcpy(b.orig_round_ops_cart_m.data() + 6u, m.cart_rom + TeamAssetOffsets::kCopaVsAllStarCartOff + kM,   2u);
        std::memcpy(b.orig_round_sched_cart.data(), m.cart_rom + TeamAssetOffsets::kRoundSchedCartOff, TeamAssetOffsets::kRoundSchedBytes);
        std::memcpy(b.orig_round_sched_cart_m.data(), m.cart_rom + TeamAssetOffsets::kRoundSchedCartOff + kM, TeamAssetOffsets::kRoundSchedBytes);

        std::memcpy(b.orig_pol_cup.data(), m.cart_rom + TeamAssetOffsets::kPolCupOff, TeamAssetOffsets::kPolCupBytes);
        std::memcpy(b.orig_cup_cel.data(), m.cart_rom + TeamAssetOffsets::kCupCelOff, TeamAssetOffsets::kCupCelBytes);
        std::memcpy(b.orig_cup_pal_cart.data(), m.cart_rom + TeamAssetOffsets::kCupPalCartOff, TeamAssetOffsets::kCupPalBytes);
        std::memcpy(b.orig_cup_pal_cart_m.data(), m.cart_rom + TeamAssetOffsets::kCupPalCartOff + kM, TeamAssetOffsets::kCupPalBytes);
        std::memcpy(b.orig_msg_rank.data(), m.cart_rom + TeamAssetOffsets::kMsgRankOff, TeamAssetOffsets::kMsgRankBytes);
        for (uint32_t i = 0u; i < CopaTournamentState::kActiveTeamCount; ++i) {
            std::memcpy(b.orig_tex_en[i].data(), m.cart_rom + kTexEnMeta[i].off, TeamAssetOffsets::kTexEnBytes);
        }
        for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
            std::memcpy(b.orig_cel_en[i].data(), m.cart_rom + kCelEnMeta[i].off, kCelEnMeta[i].size);
        }

        b.captured = true;
    }

    // Only capture Low/High WRAM originals after the BIOS/game has actually populated them;
    // capturing at Cold Boot Frame 0 would record all-zeros and later wipe live WRAM on Classic switch.
    capture_classic_wram_backup(m, b);
}

} // namespace

const std::array<CopaTeamSpec, TeamAssetOffsets::kTeamCount>& copa_team_specs() {
    return kTeamSpecs;
}

uint32_t generate_copa_tournament_seed() {
#ifndef TWC98_NO_STL
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint32_t seed = static_cast<uint32_t>(ticks ^ (ticks >> 32)) ^ 0x9E3779B9u;
    if (seed == 0u) seed = 0x54574339u;
    return seed;
#else
    static uint32_t s_wasm_seed = 0x54574339u;
    s_wasm_seed ^= s_wasm_seed << 13;
    s_wasm_seed ^= s_wasm_seed >> 17;
    s_wasm_seed ^= s_wasm_seed << 5;
    if (s_wasm_seed == 0u) s_wasm_seed = 0x9E3779B9u;
    return s_wasm_seed;
#endif
}

void build_random_copa_groups(CopaTournamentState& state, uint32_t seed) {
    uint32_t rng = (seed != 0u) ? seed : 0x54574339u;
    std::array<uint8_t, CopaTournamentState::kActiveTeamCount> shuffled = {
        kActiveCopaSlots[0], kActiveCopaSlots[1], kActiveCopaSlots[2],  kActiveCopaSlots[3],
        kActiveCopaSlots[4], kActiveCopaSlots[5], kActiveCopaSlots[6],  kActiveCopaSlots[7],
        kActiveCopaSlots[8], kActiveCopaSlots[9], kActiveCopaSlots[10], kActiveCopaSlots[11]
    };
    for (uint32_t i = CopaTournamentState::kActiveTeamCount - 1u; i > 0u; --i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const uint32_t j = rng % (i + 1u);
        std::swap(shuffled[i], shuffled[j]);
    }
    for (uint32_t g = 0u; g < CopaTournamentState::kGroupCount; ++g) {
        for (uint32_t pos = 0u; pos < CopaTournamentState::kTeamsPerGroup; ++pos) {
            state.groups[g][pos] = shuffled[g * CopaTournamentState::kTeamsPerGroup + pos];
        }
    }
    state.knockout_bracket.fill(0xFFu);
    state.initialized = true;
    state.seed = (seed != 0u) ? seed : 0x54574339u;
    state.player_team = shuffled[0];
    state.player_group = 0u;
    state.round_index = 0u;
    state.stage = CopaTournamentState::Stage::GroupStage;
}

int find_team_group(const CopaTournamentState& state, uint8_t slot) {
    if (!state.initialized) return -1;
    for (uint32_t g = 0u; g < CopaTournamentState::kGroupCount; ++g) {
        for (uint32_t pos = 0u; pos < CopaTournamentState::kTeamsPerGroup; ++pos) {
            if (state.groups[g][pos] == slot) return static_cast<int>(g);
        }
    }
    return -1;
}

bool apply_game_mode(StvMachineState& m, GameMode mode, CopaModBackup& backup, uint32_t copa_seed) {
    if (!m.has_cart) return false;
    capture_classic_backup(m, backup);

    static constexpr uint32_t kM = TeamAssetOffsets::kEprgMirrorDelta;

    if (mode == GameMode::Classic) {
        for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
            const uint8_t s = kModSlots[i];
            std::memcpy(m.cart_rom + TeamAssetOffsets::kTFlagBaseOff + s * TeamAssetOffsets::kTFlagStride,
                        backup.orig_tflag[i].data(), TeamAssetOffsets::kTFlagSize);
            std::memcpy(m.cart_rom + kBgTmMeta[i].off, backup.orig_bgtm[i].data(), kBgTmMeta[i].size);
            std::memcpy(m.cart_rom + TeamAssetOffsets::kTexTmBaseOff + s * TeamAssetOffsets::kTexStride,
                        backup.orig_textm[i].data(), TeamAssetOffsets::kTexStride);
            std::memcpy(m.cart_rom + TeamAssetOffsets::kTexHmBaseOff + s * TeamAssetOffsets::kTexStride,
                        backup.orig_texhm[i].data(), TeamAssetOffsets::kTexStride);
            std::memcpy(m.cart_rom + TeamAssetOffsets::kCelOutPalsBaseOff + static_cast<size_t>(kCelPalIds[i]) * 32u,
                        backup.orig_cel_pals.data() + i * 32u, 32u);
            std::memcpy(m.cart_rom + TeamAssetOffsets::kMsgOutBaseOff + s * TeamAssetOffsets::kMsgOutMapBytes,
                        backup.orig_msg_maps.data() + i * TeamAssetOffsets::kMsgOutMapBytes,
                        TeamAssetOffsets::kMsgOutMapBytes);
        }
        for (uint32_t s = 0u; s < TeamAssetOffsets::kTeamCount; ++s) {
            write_be16(m.cart_rom + TeamAssetOffsets::kMsgOutPosBaseOff +
                       static_cast<size_t>(s) * TeamAssetOffsets::kMsgOutPosStride +
                       TeamAssetOffsets::kMsgOutPosXyOff,
                       backup.orig_msg_pos_xy[s]);
        }
        for (uint32_t i = 0u; i < CopaModBackup::kHiddenMsgCount; ++i) {
            const uint8_t s = kHiddenMsgSlots[i];
            std::memcpy(m.cart_rom + TeamAssetOffsets::kMsgOutBaseOff + s * TeamAssetOffsets::kMsgOutMapBytes,
                        backup.orig_hidden_msg_maps.data() + i * TeamAssetOffsets::kMsgOutMapBytes,
                        TeamAssetOffsets::kMsgOutMapBytes);
        }
        for (uint32_t i = 0u; i < CopaModBackup::kSafeCelPoolSize; ++i) {
            std::memcpy(m.cart_rom + TeamAssetOffsets::kCelOutTilesBaseOff + static_cast<size_t>(kSafeCelOutTilePool[i]) * 32u,
                        backup.orig_cel_pool_tiles.data() + i * 32u, 32u);
        }
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblSlice0Off, backup.orig_capa_cart.data(), TeamAssetOffsets::kCapaAllBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff, backup.orig_capa_eprg.data(), TeamAssetOffsets::kCapaAllBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff + kM, backup.orig_capa_eprg_m.data(), TeamAssetOffsets::kCapaAllBytes);

        std::memcpy(m.cart_rom + TeamAssetOffsets::kSpTblCartOff, backup.orig_sp_cart.data(), TeamAssetOffsets::kSpTblTotalBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kSpTblCartOff + kM, backup.orig_sp_cart_m.data(), TeamAssetOffsets::kSpTblTotalBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff, backup.orig_master_skin_cart.data(), TeamAssetOffsets::kMasterSkinBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff + kM, backup.orig_master_skin_cart_m.data(), TeamAssetOffsets::kMasterSkinBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kTierTblCartOff, backup.orig_tiers_cart.data(), TeamAssetOffsets::kTierTblBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kTierTblCartOff + kM, backup.orig_tiers_cart_m.data(), TeamAssetOffsets::kTierTblBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kTeamGridCartOff, backup.orig_grid_cart.data(), TeamAssetOffsets::kTeamGridBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kTeamGridCartOff + kM, backup.orig_grid_cart_m.data(), TeamAssetOffsets::kTeamGridBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kP2CursorCartOff, backup.orig_p2_cursor_cart.data(), 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kP2CursorCartOff + kM, backup.orig_p2_cursor_cart_m.data(), 2u);
        for (int i = 0; i < 2; ++i) {
            std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3VfxCallCart[i], backup.orig_sp3_vfx_cart.data() + i * 2, 2u);
            std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3VfxCallCart[i] + kM, backup.orig_sp3_vfx_cart_m.data() + i * 2, 2u);
        }
        std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3ArgCartOff, backup.orig_sp3_arg_cart.data(), 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3ArgCartOff + kM, backup.orig_sp3_arg_cart_m.data(), 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3EmitLitCartOff, backup.orig_sp3_emit_cart.data(), 4u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kSp3EmitLitCartOff + kM, backup.orig_sp3_emit_cart_m.data(), 4u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kKitClashCartOff, backup.orig_kit_clash_cart.data(), TeamAssetOffsets::kKitClashBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kKitClashCartOff + kM, backup.orig_kit_clash_cart_m.data(), TeamAssetOffsets::kKitClashBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kVenSp19Band0CartOff, backup.orig_ven_band0_cart.data(), 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kVenSp19Band0CartOff + kM, backup.orig_ven_band0_cart_m.data(), 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kTexGmVenFlagOff, backup.orig_texgm_ven_flag.data(), TeamAssetOffsets::kTexGmVenFlagBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kEuroRemapCartOff, backup.orig_euro_remap_cart.data(), TeamAssetOffsets::kEuroRemapBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kEuroRemapCartOff + kM, backup.orig_euro_remap_cart_m.data(), TeamAssetOffsets::kEuroRemapBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaRoundEndCmpCartOff, backup.orig_round_ops_cart.data() + 0u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaOppAllStarCartOff,  backup.orig_round_ops_cart.data() + 2u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaContEndCartOff,     backup.orig_round_ops_cart.data() + 4u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaVsAllStarCartOff,   backup.orig_round_ops_cart.data() + 6u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaRoundEndCmpCartOff + kM, backup.orig_round_ops_cart_m.data() + 0u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaOppAllStarCartOff + kM,  backup.orig_round_ops_cart_m.data() + 2u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaContEndCartOff + kM,     backup.orig_round_ops_cart_m.data() + 4u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCopaVsAllStarCartOff + kM,   backup.orig_round_ops_cart_m.data() + 6u, 2u);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kRoundSchedCartOff, backup.orig_round_sched_cart.data(), TeamAssetOffsets::kRoundSchedBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kRoundSchedCartOff + kM, backup.orig_round_sched_cart_m.data(), TeamAssetOffsets::kRoundSchedBytes);

        std::memcpy(m.cart_rom + TeamAssetOffsets::kPolCupOff, backup.orig_pol_cup.data(), TeamAssetOffsets::kPolCupBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCupCelOff, backup.orig_cup_cel.data(), TeamAssetOffsets::kCupCelBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCupPalCartOff, backup.orig_cup_pal_cart.data(), TeamAssetOffsets::kCupPalBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kCupPalCartOff + kM, backup.orig_cup_pal_cart_m.data(), TeamAssetOffsets::kCupPalBytes);
        std::memcpy(m.cart_rom + TeamAssetOffsets::kMsgRankOff, backup.orig_msg_rank.data(), TeamAssetOffsets::kMsgRankBytes);
        for (uint32_t i = 0u; i < CopaTournamentState::kActiveTeamCount; ++i) {
            std::memcpy(m.cart_rom + kTexEnMeta[i].off, backup.orig_tex_en[i].data(), TeamAssetOffsets::kTexEnBytes);
        }
        for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
            std::memcpy(m.cart_rom + kCelEnMeta[i].off, backup.orig_cel_en[i].data(), kCelEnMeta[i].size);
        }

        if (backup.wram_captured) {
            std::memcpy(m.inner.low_wram.data() + TeamAssetOffsets::kCapaTblLowWramOff, backup.orig_capa_lwram.data(), TeamAssetOffsets::kCapaAllBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCapaTblHighWramOff, backup.orig_capa_hwram.data(), TeamAssetOffsets::kCapaAllBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kSpTblHighWramOff, backup.orig_sp_hwram.data(), TeamAssetOffsets::kSpTblTotalBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kMasterSkinHighWramOff, backup.orig_master_skin_hwram.data(), TeamAssetOffsets::kMasterSkinBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kTierTblHighWramOff, backup.orig_tiers_hwram.data(), TeamAssetOffsets::kTierTblBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kTeamGridHighWramOff, backup.orig_grid_hwram.data(), TeamAssetOffsets::kTeamGridBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kP2CursorHighWramOff, backup.orig_p2_cursor_hwram.data(), 2u);
            for (int i = 0; i < 2; ++i) {
                std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kSp3VfxCallHighWram[i], backup.orig_sp3_vfx_hwram.data() + i * 2, 2u);
            }
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kSp3ArgHighWramOff, backup.orig_sp3_arg_hwram.data(), 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kSp3EmitLitHighWramOff, backup.orig_sp3_emit_hwram.data(), 4u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kKitClashHighWramOff, backup.orig_kit_clash_hwram.data(), TeamAssetOffsets::kKitClashBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kVenSp19Band0HighWramOff, backup.orig_ven_band0_hwram.data(), 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kEuroRemapHighWramOff, backup.orig_euro_remap_hwram.data(), TeamAssetOffsets::kEuroRemapBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCupPalHighWramOff, backup.orig_cup_pal_hwram.data(), TeamAssetOffsets::kCupPalBytes);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCopaRoundEndCmpHighWramOff, backup.orig_round_ops_hwram.data() + 0u, 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCopaOppAllStarHighWramOff,  backup.orig_round_ops_hwram.data() + 2u, 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCopaContEndHighWramOff,     backup.orig_round_ops_hwram.data() + 4u, 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kCopaVsAllStarHighWramOff,   backup.orig_round_ops_hwram.data() + 6u, 2u);
            std::memcpy(m.inner.high_wram.data() + TeamAssetOffsets::kRoundSchedHighWramOff, backup.orig_round_sched_hwram.data(), TeamAssetOffsets::kRoundSchedBytes);
            m.invalidate_all_aot_pages();
        }

        m.copa_mode_active = false;
        m.copa_vs_hook = nullptr;
        m.copa_opp_hook = nullptr;
        m.copa_sp3_wind_timer = 0u;
        m.copa_ts_heartbeat = 0u;
        m.vdp.wind_vfx.active = false;
        m.vdp.copa_group_badges = false;
        m.vdp.copa_vfx_replace = false;
        m.vdp.copa_sp_vfx.condor.active = false;
        m.vdp.copa_sp_vfx.machu.active = false;
        m.vdp.copa_sp_vfx.uru_flag.active = false;
        m.vdp.copa_sp_vfx.charge_count = 0u;
        m.vdp.copa_bracket = StvVdpState::CopaBracketState{};
        backup.active_mode = GameMode::Classic;
        backup.tournament = CopaTournamentState{};
        on_match_rules_tick(m, backup);
        return true;
    }

    // Apply GameMode::CopaAmerica (12-team tournament drawn randomly into 3 groups of 4):
    static constexpr const char* kModBannerNames[CopaModBackup::kModSlotCount] = {
        "PERU", "CHILE", "ECUADOR", "VENEZUELA", "URUGUAY", "PARAGUAY", "BOLIVIA"
    };
    for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
        const uint8_t s = kModSlots[i];
        uint8_t* tf = m.cart_rom + TeamAssetOffsets::kTFlagBaseOff + s * TeamAssetOffsets::kTFlagStride;
        std::memcpy(tf, backup.orig_tflag[i].data(), TeamAssetOffsets::kTFlagSize);
        build_tflag(s, tf, TeamAssetOffsets::kTFlagSize);

        uint8_t* bg = m.cart_rom + kBgTmMeta[i].off;
        std::memcpy(bg, backup.orig_bgtm[i].data(), kBgTmMeta[i].size);
        build_bgtm(s, bg, kBgTmMeta[i].size, kModBannerNames[i]);
    }

    build_team_select_flags(m.cart_rom);
    // Blank hidden teams {2,4,8,9} and extra region slots {16,17,18} to transparent tile 0
    for (uint8_t s : kHiddenMsgSlots) {
        std::memset(m.cart_rom + TeamAssetOffsets::kMsgOutBaseOff + s * TeamAssetOffsets::kMsgOutMapBytes,
                    0, TeamAssetOffsets::kMsgOutMapBytes);
    }

    const uint32_t draw_seed = (copa_seed != 0u) ? copa_seed : generate_copa_tournament_seed();
    build_random_copa_groups(backup.tournament, draw_seed);
    std::memcpy(backup.active_tiers.data(), kCopaTiers, TeamAssetOffsets::kTierTblBytes);

    backup.active_grid.fill(0xFFu);
    for (uint32_t g = 0u; g < CopaTournamentState::kGroupCount; ++g) {
        for (uint32_t pos = 0u; pos < CopaTournamentState::kTeamsPerGroup; ++pos) {
            const uint8_t slot = backup.tournament.groups[g][pos];
            backup.active_grid[g * 6u + (pos + 1u)] = slot;
            if (slot < TeamAssetOffsets::kTeamCount) {
                const uint16_t xy = static_cast<uint16_t>(
                    (static_cast<uint16_t>(kCopaGroupCellX[pos]) << 8) | kCopaGroupCellY[g]);
                write_be16(m.cart_rom + TeamAssetOffsets::kMsgOutPosBaseOff +
                           static_cast<size_t>(slot) * TeamAssetOffsets::kMsgOutPosStride +
                           TeamAssetOffsets::kMsgOutPosXyOff,
                           xy);
            }
        }
    }

    // The original MSG_OUT script still draws hidden slots in slot order.
    // A transparent map erases earlier flags at the same coordinates (notably
    // hidden NED/8 at 0x3A29 overwrote VEN in the fixed verification draw).
    // Move those blank draws below the visible 56 cell rows as well as hiding them.
    for (uint8_t slot : kHiddenSlots) {
        write_be16(m.cart_rom + TeamAssetOffsets::kMsgOutPosBaseOff +
                   static_cast<size_t>(slot) * TeamAssetOffsets::kMsgOutPosStride +
                   TeamAssetOffsets::kMsgOutPosXyOff, 0x0038u);
    }

    apply_kits_and_capa(m.cart_rom, backup);
    build_ven_flag_texgm(m.cart_rom + TeamAssetOffsets::kTexGmVenFlagOff);

    // Fase 3: 3D Copa America Trophy + Winner Ending (MSG_RANK + 12 TEX_ENxx + 7 CEL_ENxx)
    std::memcpy(m.cart_rom + TeamAssetOffsets::kPolCupOff, backup.orig_pol_cup.data(), TeamAssetOffsets::kPolCupBytes);
    std::memcpy(m.cart_rom + TeamAssetOffsets::kCupCelOff, backup.orig_cup_cel.data(), TeamAssetOffsets::kCupCelBytes);
    std::memcpy(m.cart_rom + TeamAssetOffsets::kMsgRankOff, backup.orig_msg_rank.data(), TeamAssetOffsets::kMsgRankBytes);
    for (uint32_t i = 0u; i < CopaTournamentState::kActiveTeamCount; ++i) {
        std::memcpy(m.cart_rom + kTexEnMeta[i].off, backup.orig_tex_en[i].data(), TeamAssetOffsets::kTexEnBytes);
    }
    for (uint32_t i = 0u; i < CopaModBackup::kModSlotCount; ++i) {
        std::memcpy(m.cart_rom + kCelEnMeta[i].off, backup.orig_cel_en[i].data(), kCelEnMeta[i].size);
    }
    build_copa_3d_trophy(m.cart_rom);
    build_copa_msg_rank(m.cart_rom, backup.orig_msg_rank.data());
    build_copa_tex_en(m.cart_rom);
    build_copa_cel_en(m.cart_rom, backup.scratch_cel_canvas.data());

    std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblSlice0Off, backup.orig_capa_cart.data(), TeamAssetOffsets::kCapaAllBytes);
    patch_capa_table(m.cart_rom + TeamAssetOffsets::kCapaTblSlice0Off);
    std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff, backup.orig_capa_eprg.data(), TeamAssetOffsets::kCapaAllBytes);
    patch_capa_table(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff);
    std::memcpy(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff + kM, backup.orig_capa_eprg_m.data(), TeamAssetOffsets::kCapaAllBytes);
    patch_capa_table(m.cart_rom + TeamAssetOffsets::kCapaTblEprgCartOff + kM);
    if (has_nonzero(m.inner.low_wram.data() + TeamAssetOffsets::kCapaTblLowWramOff, TeamAssetOffsets::kCapaAllBytes)) {
        patch_capa_table(m.inner.low_wram.data() + TeamAssetOffsets::kCapaTblLowWramOff);
    }
    if (has_nonzero(m.inner.high_wram.data() + TeamAssetOffsets::kCapaTblHighWramOff, TeamAssetOffsets::kCapaAllBytes) ||
        has_nonzero(backup.orig_capa_hwram.data(), TeamAssetOffsets::kCapaAllBytes)) {
        patch_capa_table(m.inner.high_wram.data() + TeamAssetOffsets::kCapaTblHighWramOff);
    }

    write_copa_eprg_tables(m.cart_rom + TeamAssetOffsets::kSpTblCartOff,
                           m.cart_rom + TeamAssetOffsets::kTierTblCartOff,
                           m.cart_rom + TeamAssetOffsets::kTeamGridCartOff,
                           m.cart_rom + TeamAssetOffsets::kP2CursorCartOff,
                           backup.active_grid.data(),
                           m.cart_rom, 0x00200000u);
    write_copa_eprg_tables(m.cart_rom + TeamAssetOffsets::kSpTblCartOff + kM,
                           m.cart_rom + TeamAssetOffsets::kTierTblCartOff + kM,
                           m.cart_rom + TeamAssetOffsets::kTeamGridCartOff + kM,
                           m.cart_rom + TeamAssetOffsets::kP2CursorCartOff + kM,
                           backup.active_grid.data(),
                           m.cart_rom, 0x00300000u);
    if (has_nonzero(m.inner.high_wram.data() + TeamAssetOffsets::kSpTblHighWramOff, TeamAssetOffsets::kSpTblTotalBytes) ||
        has_nonzero(backup.orig_sp_hwram.data(), TeamAssetOffsets::kSpTblTotalBytes)) {
        write_copa_eprg_tables(m.inner.high_wram.data() + TeamAssetOffsets::kSpTblHighWramOff,
                               m.inner.high_wram.data() + TeamAssetOffsets::kTierTblHighWramOff,
                               m.inner.high_wram.data() + TeamAssetOffsets::kTeamGridHighWramOff,
                               m.inner.high_wram.data() + TeamAssetOffsets::kP2CursorHighWramOff,
                               backup.active_grid.data(),
                               m.inner.high_wram.data(), 0u);
        m.invalidate_all_aot_pages();
    }

    patch_master_skin_table(m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff);
    patch_master_skin_table(m.cart_rom + TeamAssetOffsets::kMasterSkinCartOff + kM);
    if (has_nonzero(m.inner.high_wram.data() + TeamAssetOffsets::kMasterSkinHighWramOff, TeamAssetOffsets::kMasterSkinBytes) ||
        has_nonzero(backup.orig_master_skin_hwram.data(), TeamAssetOffsets::kMasterSkinBytes)) {
        patch_master_skin_table(m.inner.high_wram.data() + TeamAssetOffsets::kMasterSkinHighWramOff);
    }

    m.copa_mode_active = true;
    m.copa_vs_hook = &on_vs_entry_hook;
    m.copa_opp_hook = &on_opponent_select_hook;
    m.copa_ts_heartbeat = 0u;
    m.vdp.copa_group_badges = false;
    m.vdp.copa_vfx_replace = true;
    m.vdp.copa_bracket = StvVdpState::CopaBracketState{};
    backup.active_mode = GameMode::CopaAmerica;
    on_match_rules_tick(m, backup);
    return true;
}

void set_match_rules(StvMachineState& m, CopaModBackup& backup, const MatchRulesConfig& rules) {
    MatchRulesConfig clean = rules;
    if (clean.rule_mode == MatchRuleMode::TimeLimit) {
        if (clean.rule_value < 30u) clean.rule_value = 30u;
        if (clean.rule_value > 300u) clean.rule_value = 300u;
    } else if (clean.rule_mode == MatchRuleMode::GoalLimit) {
        if (clean.rule_value < 1u) clean.rule_value = 1u;
        if (clean.rule_value > 15u) clean.rule_value = 15u;
    } else {
        clean.rule_mode = MatchRuleMode::Default;
        clean.rule_value = 90u;
    }
    backup.match_rules = clean;
    m.rule_hard_mode = clean.hard_mode;
    m.rule_mode = static_cast<uint8_t>(clean.rule_mode);
    m.rule_value = clean.rule_value;
    m.rule_goal_triggered = false;
    m.rule_goal_delay = 0u;
    on_match_rules_tick(m, backup);
}

void on_match_rules_tick(StvMachineState& m, CopaModBackup& backup) {
    uint8_t* hw = m.inner.high_wram.data();
    const uint16_t ai1 = read_be16(hw + TeamAssetOffsets::kAiDiff1RetHighWramOff);
    const uint16_t ai2 = read_be16(hw + TeamAssetOffsets::kAiDiff2IdxHighWramOff);
    const bool eprg_ready =
        (ai1 == 0x6063u || ai1 == 0xE01Fu) &&
        (ai2 == 0x6EEEu || ai2 == 0xEE1Fu);
    if (!eprg_ready) return;

    if (!backup.hard_patch_captured) {
        std::memcpy(backup.orig_ai_diff1.data(), hw + TeamAssetOffsets::kAiDiff1RetHighWramOff, 2u);
        std::memcpy(backup.orig_ai_diff2.data(), hw + TeamAssetOffsets::kAiDiff2IdxHighWramOff, 2u);
        std::memcpy(backup.orig_dip_diff.data(), hw + TeamAssetOffsets::kDipDifficultyHighWramOff, 2u);
        backup.hard_patch_captured = true;
    }
    if (!backup.time_patch_captured) {
        const uint16_t t1 = read_be16(hw + TeamAssetOffsets::kDipTime1pHighWramOff);
        const uint16_t t2 = read_be16(hw + TeamAssetOffsets::kDipTime2pHighWramOff);
        if (t1 != 0u || t2 != 0u) {
            std::memcpy(backup.orig_dip_time_1p.data(), hw + TeamAssetOffsets::kDipTime1pHighWramOff, 2u);
            std::memcpy(backup.orig_dip_time_2p.data(), hw + TeamAssetOffsets::kDipTime2pHighWramOff, 2u);
            backup.time_patch_captured = true;
        }
    }

    // 1. B-H5-05 Hard Mode: lock BOTH SH-2 difficulty calculators (0x0603EB84 & 0x0603F872) to max level 31
    //    and set GAME ASSIGNMENTS difficulty to 3 (VERY HARD) from Match 1.
    if (m.rule_hard_mode) {
        if (ai1 != 0xE01Fu) {
            write_be16(hw + TeamAssetOffsets::kAiDiff1RetHighWramOff, 0xE01Fu);
            m.mark_high_wram_dirty(TeamAssetOffsets::kAiDiff1RetHighWramOff, 2u);
        }
        if (ai2 != 0xEE1Fu) {
            write_be16(hw + TeamAssetOffsets::kAiDiff2IdxHighWramOff, 0xEE1Fu);
            m.mark_high_wram_dirty(TeamAssetOffsets::kAiDiff2IdxHighWramOff, 2u);
        }
        if (read_be16(hw + TeamAssetOffsets::kDipDifficultyHighWramOff) != 3u) {
            write_be16(hw + TeamAssetOffsets::kDipDifficultyHighWramOff, 3u);
        }
        backup.hard_patch_applied = true;
    } else if (backup.hard_patch_applied) {
        std::memcpy(hw + TeamAssetOffsets::kAiDiff1RetHighWramOff, backup.orig_ai_diff1.data(), 2u);
        m.mark_high_wram_dirty(TeamAssetOffsets::kAiDiff1RetHighWramOff, 2u);
        std::memcpy(hw + TeamAssetOffsets::kAiDiff2IdxHighWramOff, backup.orig_ai_diff2.data(), 2u);
        m.mark_high_wram_dirty(TeamAssetOffsets::kAiDiff2IdxHighWramOff, 2u);
        std::memcpy(hw + TeamAssetOffsets::kDipDifficultyHighWramOff, backup.orig_dip_diff.data(), 2u);
        backup.hard_patch_applied = false;
    }

    // 2. B-H6-04 Match Time & Goal Limit Rules (deterministic pre-Frame0 config):
    if (m.rule_mode == static_cast<uint8_t>(MatchRuleMode::TimeLimit)) {
        const uint16_t sec = (m.rule_value >= 30u && m.rule_value <= 300u) ? m.rule_value : 120u;
        write_be16(hw + TeamAssetOffsets::kDipTime1pHighWramOff, sec);
        write_be16(hw + TeamAssetOffsets::kDipTime2pHighWramOff, sec);
        backup.time_patch_applied = true;
        m.rule_goal_triggered = false;
        m.rule_goal_delay = 0u;
    } else if (m.rule_mode == static_cast<uint8_t>(MatchRuleMode::GoalLimit)) {
        const uint16_t target = (m.rule_value >= 1u && m.rule_value <= 15u) ? m.rule_value : 3u;
        write_be16(hw + TeamAssetOffsets::kDipTime1pHighWramOff, 99u);
        write_be16(hw + TeamAssetOffsets::kDipTime2pHighWramOff, 99u);
        backup.time_patch_applied = true;

        const uint16_t time_rem = read_be16(hw + TeamAssetOffsets::kMatchTimerHighWramOff);
        if (time_rem == 0u || time_rem > 300u) {
            m.rule_goal_triggered = false;
            m.rule_goal_delay = 0u;
        } else {
            const uint16_t p1_goals = read_be16(hw + TeamAssetOffsets::kMatchP1GoalsHighWramOff);
            const uint16_t p2_goals = read_be16(hw + TeamAssetOffsets::kMatchP2GoalsHighWramOff);
            if (p1_goals < target && p2_goals < target) {
                if (time_rem < 99u) {
                    write_be16(hw + TeamAssetOffsets::kMatchTimerHighWramOff, 99u);
                    write_be16(hw + TeamAssetOffsets::kMatchHudTimerHighWramOff, 99u);
                    write_be16(hw + TeamAssetOffsets::kMatchExtraTimerHighWramOff, 0u);
                }
                m.rule_goal_triggered = false;
                m.rule_goal_delay = 0u;
            } else {
                if (!m.rule_goal_triggered) {
                    m.rule_goal_triggered = true;
                    m.rule_goal_delay = 140u;
                } else if (m.rule_goal_delay > 0u) {
                    --m.rule_goal_delay;
                    if (time_rem < 10u) {
                        write_be16(hw + TeamAssetOffsets::kMatchTimerHighWramOff, 10u);
                        write_be16(hw + TeamAssetOffsets::kMatchHudTimerHighWramOff, 10u);
                    }
                    if (m.rule_goal_delay == 0u && time_rem > 1u) {
                        write_be16(hw + TeamAssetOffsets::kMatchTimerHighWramOff, 1u);
                        write_be16(hw + TeamAssetOffsets::kMatchHudTimerHighWramOff, 1u);
                        write_be16(hw + TeamAssetOffsets::kMatchExtraTimerHighWramOff, 0u);
                    }
                }
            }
        }
    } else {
        if (backup.time_patch_applied && backup.time_patch_captured) {
            std::memcpy(hw + TeamAssetOffsets::kDipTime1pHighWramOff, backup.orig_dip_time_1p.data(), 2u);
            std::memcpy(hw + TeamAssetOffsets::kDipTime2pHighWramOff, backup.orig_dip_time_2p.data(), 2u);
            backup.time_patch_applied = false;
        }
        m.rule_goal_triggered = false;
        m.rule_goal_delay = 0u;
    }
}

static uint32_t copa_hash_step(uint32_t s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (s != 0u) ? s : 0x54574339u;
}

static void init_copa_bracket(StvVdpState::CopaBracketState& br, const CopaTournamentState& ts) {
    br = StvVdpState::CopaBracketState{};
    br.initialized = 1u;
    br.last_shown_round = 0xFFu;
    br.last_resolved_round = 0xFFu;
    br.seed = ts.seed;
    br.player_team = ts.player_team;
    br.player_group = (ts.player_group < 3u) ? ts.player_group : 0u;
    for (int g = 0; g < 3; ++g) {
        for (int p = 0; p < 4; ++p) {
            br.groups[g][p] = ts.groups[g][p];
            br.standings[g][p].team = ts.groups[g][p];
        }
        // Round-robin 3 matchdays x 2 matches per group
        br.group_matches[0][g * 2 + 0].team_a = ts.groups[g][0];
        br.group_matches[0][g * 2 + 0].team_b = ts.groups[g][1];
        br.group_matches[0][g * 2 + 1].team_a = ts.groups[g][2];
        br.group_matches[0][g * 2 + 1].team_b = ts.groups[g][3];

        br.group_matches[1][g * 2 + 0].team_a = ts.groups[g][0];
        br.group_matches[1][g * 2 + 0].team_b = ts.groups[g][2];
        br.group_matches[1][g * 2 + 1].team_a = ts.groups[g][1];
        br.group_matches[1][g * 2 + 1].team_b = ts.groups[g][3];

        br.group_matches[2][g * 2 + 0].team_a = ts.groups[g][0];
        br.group_matches[2][g * 2 + 0].team_b = ts.groups[g][3];
        br.group_matches[2][g * 2 + 1].team_a = ts.groups[g][1];
        br.group_matches[2][g * 2 + 1].team_b = ts.groups[g][2];
    }
}

static void sim_copa_ai_match(StvVdpState::CopaMatchSlot& ms, uint32_t seed, uint32_t tag, bool allow_draw) {
    if (ms.played != 0u || ms.team_a >= TeamAssetOffsets::kTeamCount || ms.team_b >= TeamAssetOffsets::kTeamCount) return;
    const uint32_t ta = kCopaTiers[ms.team_a];
    const uint32_t tb = kCopaTiers[ms.team_b];
    uint32_t r = copa_hash_step(seed ^ (tag * 0x9E3779B9u) ^ (ms.team_a * 29u) ^ (ms.team_b * 53u));
    const uint32_t roll_a = r & 0xFFu;
    r = copa_hash_step(r);
    const uint32_t roll_b = r & 0xFFu;
    uint8_t ga = static_cast<uint8_t>((roll_a % 3u) + ((ta >= tb && (roll_a & 3u) == 0u) ? 1u : 0u));
    uint8_t gb = static_cast<uint8_t>((roll_b % 3u) + ((tb > ta && (roll_b & 3u) == 0u) ? 1u : 0u));
    if (ga > gb) {
        ms.goals_a = ga; ms.goals_b = gb; ms.played = 1u; ms.winner = ms.team_a;
    } else if (gb > ga) {
        ms.goals_a = ga; ms.goals_b = gb; ms.played = 1u; ms.winner = ms.team_b;
    } else if (allow_draw) {
        ms.goals_a = ga; ms.goals_b = gb; ms.played = 1u; ms.winner = 0xFFu;
    } else {
        ms.goals_a = ga; ms.goals_b = gb; ms.played = 2u;
        r = copa_hash_step(r);
        const bool a_pens = (ta > tb) ? ((r % 3u) != 0u) : ((r % 3u) == 0u);
        ms.winner = a_pens ? ms.team_a : ms.team_b;
    }
}

static void recompute_group_standings(StvVdpState::CopaBracketState& br) {
    for (int g = 0; g < 3; ++g) {
        for (int p = 0; p < 4; ++p) {
            br.standings[g][p] = StvVdpState::CopaStandingRow{};
            br.standings[g][p].team = br.groups[g][p];
        }
        auto add_team_res = [&](uint8_t team, uint8_t gf, uint8_t ga, bool won, bool drew) {
            for (int p = 0; p < 4; ++p) {
                if (br.standings[g][p].team == team) {
                    auto& s = br.standings[g][p];
                    ++s.played;
                    s.gf = static_cast<uint8_t>(s.gf + gf);
                    s.ga = static_cast<uint8_t>(s.ga + ga);
                    if (won) { ++s.wins; s.pts = static_cast<uint8_t>(s.pts + 3u); }
                    else if (drew) { ++s.draws; s.pts = static_cast<uint8_t>(s.pts + 1u); }
                    else { ++s.losses; }
                    break;
                }
            }
        };
        for (int md = 0; md < 3; ++md) {
            for (int m = 0; m < 2; ++m) {
                const auto& ms = br.group_matches[md][g * 2 + m];
                if (ms.played == 0u) continue;
                const bool a_won = (ms.winner == ms.team_a);
                const bool b_won = (ms.winner == ms.team_b);
                const bool drew = (ms.winner == 0xFFu);
                add_team_res(ms.team_a, ms.goals_a, ms.goals_b, a_won, drew);
                add_team_res(ms.team_b, ms.goals_b, ms.goals_a, b_won, drew);
            }
        }
        std::sort(br.standings[g], br.standings[g] + 4, [](const StvVdpState::CopaStandingRow& a, const StvVdpState::CopaStandingRow& b) {
            if (a.pts != b.pts) return a.pts > b.pts;
            const int gda = static_cast<int>(a.gf) - static_cast<int>(a.ga);
            const int gdb = static_cast<int>(b.gf) - static_cast<int>(b.ga);
            if (gda != gdb) return gda > gdb;
            if (a.gf != b.gf) return a.gf > b.gf;
            const uint8_t ta = (a.team < 16u) ? kCopaTiers[a.team] : 0u;
            const uint8_t tb = (b.team < 16u) ? kCopaTiers[b.team] : 0u;
            if (ta != tb) return ta > tb;
            return a.team < b.team;
        });
    }
    std::memset(br.qual_mask, 0, sizeof(br.qual_mask));
    for (int g = 0; g < 3; ++g) {
        if (br.standings[g][0].team < 16u) br.qual_mask[br.standings[g][0].team] = 1u;
        if (br.standings[g][1].team < 16u) br.qual_mask[br.standings[g][1].team] = 1u;
    }
    StvVdpState::CopaStandingRow thirds[3] = {
        br.standings[0][2], br.standings[1][2], br.standings[2][2]
    };
    std::sort(thirds, thirds + 3, [](const StvVdpState::CopaStandingRow& a, const StvVdpState::CopaStandingRow& b) {
        if (a.pts != b.pts) return a.pts > b.pts;
        const int gda = static_cast<int>(a.gf) - static_cast<int>(a.ga);
        const int gdb = static_cast<int>(b.gf) - static_cast<int>(b.ga);
        if (gda != gdb) return gda > gdb;
        if (a.gf != b.gf) return a.gf > b.gf;
        return a.team < b.team;
    });
    if (thirds[0].team < 16u) br.qual_mask[thirds[0].team] = 2u;
    if (thirds[1].team < 16u) br.qual_mask[thirds[1].team] = 2u;
}

static uint8_t resolve_scheduled_opponent(const StvVdpState::CopaBracketState& br, uint8_t round_idx) {
    if (round_idx < 3u) {
        const uint8_t g = (br.player_group < 3u) ? br.player_group : 0u;
        for (int m = 0; m < 2; ++m) {
            const auto& ms = br.group_matches[round_idx][g * 2 + m];
            if (ms.team_a == br.player_team) return ms.team_b;
            if (ms.team_b == br.player_team) return ms.team_a;
        }
    } else if (round_idx == 3u) {
        for (int q = 0; q < 4; ++q) {
            if (br.qf[q].team_a == br.player_team) return br.qf[q].team_b;
            if (br.qf[q].team_b == br.player_team) return br.qf[q].team_a;
        }
    } else if (round_idx == 4u) {
        for (int s = 0; s < 2; ++s) {
            if (br.sf[s].team_a == br.player_team) return br.sf[s].team_b;
            if (br.sf[s].team_b == br.player_team) return br.sf[s].team_a;
        }
    } else if (round_idx == 5u) {
        if (br.final_match.team_a == br.player_team) return br.final_match.team_b;
        if (br.final_match.team_b == br.player_team) return br.final_match.team_a;
    } else if (round_idx == 6u) {
        return 16u;
    }
    return 0xFFu;
}

static void prepare_copa_round_opponent(StvMachineState& m, bool invalidate_kit_cache) {
    if (!m.copa_mode_active) return;
    uint8_t* hw = m.inner.high_wram.data();
    if (hw[0x0792A5u] != 5u || read_be16(hw + 0x097148u) != 1u) return;

    const uint8_t cur_round = hw[0x07654Du];
    if (cur_round > 6u) return;

    auto& br = m.vdp.copa_bracket;
    const uint8_t p1 = hw[0x076552u + 1u];
    if (p1 < TeamAssetOffsets::kTeamCount && kTeamSpecs[p1].active_in_copa) {
        br.player_team = p1;
        for (uint8_t g = 0u; g < 3u; ++g) {
            for (int p = 0; p < 4; ++p) {
                if (br.groups[g][p] == p1) {
                    br.player_group = g;
                    break;
                }
            }
        }
    }

    if (br.last_resolved_round != cur_round) {
        const uint8_t p1_g = br.live_goals_valid ? br.live_p1_goals : static_cast<uint8_t>(read_be16(hw + 0x07655Au));
        const uint8_t p2_g = br.live_goals_valid ? br.live_p2_goals : static_cast<uint8_t>(read_be16(hw + 0x07655Cu));
        const uint8_t eff_p1_g = (p1_g >= p2_g) ? p1_g : static_cast<uint8_t>(p2_g + 1u);

        auto record_p1_result = [&](StvVdpState::CopaMatchSlot& ms, uint8_t g1, uint8_t g2) {
            if (ms.team_a == br.player_team) {
                ms.goals_a = g1; ms.goals_b = g2;
            } else {
                ms.goals_a = g2; ms.goals_b = g1;
            }
            ms.played = (g1 > g2) ? 1u : 2u;
            ms.winner = br.player_team;
        };

        const uint8_t start_adv = (br.last_resolved_round == 0xFFu) ? 0u : static_cast<uint8_t>(br.last_resolved_round + 1u);
        for (uint8_t step = start_adv; step <= cur_round; ++step) {
            if (step >= 1u && step <= 3u) {
                const uint8_t md = static_cast<uint8_t>(step - 1u);
                for (int idx = 0; idx < 6; ++idx) {
                    auto& ms = br.group_matches[md][idx];
                    if (ms.team_a == br.player_team || ms.team_b == br.player_team) {
                        if (step == cur_round) record_p1_result(ms, eff_p1_g, p2_g);
                        else record_p1_result(ms, 2u, 1u);
                    } else {
                        sim_copa_ai_match(ms, br.seed, 100u + md * 10u + static_cast<uint32_t>(idx), true);
                    }
                }
                recompute_group_standings(br);
                if (step == 3u) {
                    StvVdpState::CopaStandingRow thirds[3] = {
                        br.standings[0][2], br.standings[1][2], br.standings[2][2]
                    };
                    std::sort(thirds, thirds + 3, [](const StvVdpState::CopaStandingRow& a, const StvVdpState::CopaStandingRow& b) {
                        if (a.pts != b.pts) return a.pts > b.pts;
                        const int gda = static_cast<int>(a.gf) - static_cast<int>(a.ga);
                        const int gdb = static_cast<int>(b.gf) - static_cast<int>(b.ga);
                        if (gda != gdb) return gda > gdb;
                        return a.gf > b.gf;
                    });
                    br.qf[0].team_a = br.standings[0][0].team; br.qf[0].team_b = thirds[1].team;
                    br.qf[1].team_a = br.standings[1][0].team; br.qf[1].team_b = br.standings[2][1].team;
                    br.qf[2].team_a = br.standings[2][0].team; br.qf[2].team_b = thirds[0].team;
                    br.qf[3].team_a = br.standings[1][1].team; br.qf[3].team_b = br.standings[0][1].team;
                    br.player_eliminated = (resolve_scheduled_opponent(br, 3u) == 0xFFu) ? 1u : 0u;
                }
            } else if (step == 4u) {
                for (int q = 0; q < 4; ++q) {
                    if (br.qf[q].team_a == br.player_team || br.qf[q].team_b == br.player_team) {
                        if (step == cur_round) record_p1_result(br.qf[q], eff_p1_g, p2_g);
                        else record_p1_result(br.qf[q], 2u, 0u);
                    } else {
                        sim_copa_ai_match(br.qf[q], br.seed, 200u + static_cast<uint32_t>(q), false);
                    }
                }
                br.sf[0].team_a = br.qf[0].winner; br.sf[0].team_b = br.qf[1].winner;
                br.sf[1].team_a = br.qf[2].winner; br.sf[1].team_b = br.qf[3].winner;
            } else if (step == 5u) {
                for (int s = 0; s < 2; ++s) {
                    if (br.sf[s].team_a == br.player_team || br.sf[s].team_b == br.player_team) {
                        if (step == cur_round) record_p1_result(br.sf[s], eff_p1_g, p2_g);
                        else record_p1_result(br.sf[s], 2u, 1u);
                    } else {
                        sim_copa_ai_match(br.sf[s], br.seed, 300u + static_cast<uint32_t>(s), false);
                    }
                }
                br.final_match.team_a = br.sf[0].winner; br.final_match.team_b = br.sf[1].winner;
            } else if (step == 6u) {
                record_p1_result(br.final_match, (step == cur_round) ? eff_p1_g : 2u, (step == cur_round) ? p2_g : 1u);
                br.all_star.team_a = br.player_team;
                br.all_star.team_b = 16u;
            }
        }
        br.live_goals_valid = 0u;
        br.last_resolved_round = cur_round;
    }

    const uint8_t opp = resolve_scheduled_opponent(br, cur_round);
    if (opp <= 16u) {
        br.next_opp = opp;
        write_be16(hw + 0x076554u, opp);
        if (opp < 16u) hw[0x07656Eu + opp] = 1u;
        if (m.cpus[0].pc == 0x06007DAEu) {
            m.cpus[0].r[4] = opp;
        }
        if (invalidate_kit_cache) {
            // Force 0x06007E0E / 0x06006D1A to reload both teams' 3D kits, palettes, and skin tones.
            hw[0x0792BAu] = 0xFFu;
            hw[0x0792BBu] = 0xFFu;
            hw[0x0792BCu] = 0xFFu;
            hw[0x0792BDu] = 0xFFu;
        }
    }
}

void on_opponent_select_hook(StvMachineState& m) {
    // At 0x060085FE / 0x06008624 (Round 0 post-Team-Select), invalidate the 0x06007E0E kit cache
    // so both teams' 3D kits/palettes reload. At 0x06007DAE (Round 1+), 0x060081EE -> 0x06006D1A reloads unconditionally.
    prepare_copa_round_opponent(m, m.cpus[0].pc != 0x06007DAEu);
}

void on_vs_entry_hook(StvMachineState& m) {
    if (!m.copa_mode_active) return;
    uint8_t* hw = m.inner.high_wram.data();
    if (hw[0x0792A5u] != 5u || read_be16(hw + 0x097148u) != 1u) return;

    const uint8_t cur_round = hw[0x07654Du];
    if (cur_round > 6u) return;

    prepare_copa_round_opponent(m, false);

    auto& br = m.vdp.copa_bracket;
    if (br.last_shown_round == cur_round) {
        return;
    }

    br.last_shown_round = cur_round;
    br.progress_stage = cur_round;
    br.progress_frames = 0u;
    br.prev_input_action = 1u;
    br.progress_active = 1u;
}

void on_frame_tick(StvMachineState& m, CopaModBackup& backup) {
    on_match_rules_tick(m, backup);
    if (backup.active_mode != GameMode::CopaAmerica) {
        m.copa_vs_hook = nullptr;
        m.copa_opp_hook = nullptr;
        return;
    }
    m.copa_vs_hook = &on_vs_entry_hook;
    m.copa_opp_hook = &on_opponent_select_hook;

    // Capture genuine Classic WRAM originals right after BIOS decrypts EPRG if booted directly in Copa.
    if (!backup.wram_captured &&
        read_be32(m.inner.high_wram.data() + TeamAssetOffsets::kEuroRemapHighWramOff) == 0xD3296030u) {
        const bool saved_flag = m.copa_mode_active;
        m.copa_mode_active = false;
        capture_classic_wram_backup(m, backup);
        m.copa_mode_active = saved_flag;
    }

    // 1. Keep Copa America CAPA_TBL & EPRG tables coherent in Low/High WRAM after boot/reset copies.
    uint8_t* lw_capa = m.inner.low_wram.data() + TeamAssetOffsets::kCapaTblLowWramOff;
    if (has_nonzero(lw_capa, 64u)) {
        if (lw_capa[14u * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u ||
            lw_capa[1u  * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u ||
            lw_capa[7u  * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u) {
            patch_capa_table(lw_capa);
        }
    }

    uint8_t* hw = m.inner.high_wram.data();
    uint8_t* hw_capa = hw + TeamAssetOffsets::kCapaTblHighWramOff;
    if (has_nonzero(hw_capa, 64u)) {
        if (hw_capa[14u * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u ||
            hw_capa[1u  * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u ||
            hw_capa[7u  * TeamAssetOffsets::kCapaTeamBytes + 1u] != 1u) {
            patch_capa_table(hw_capa);
        }
    }
    const bool need_hw_patch =
        (hw[TeamAssetOffsets::kTierTblHighWramOff + 2u] != 0xFFu) ||
        (std::memcmp(hw + TeamAssetOffsets::kTeamGridHighWramOff,
                     backup.active_grid.data(), TeamAssetOffsets::kTeamGridBytes) != 0) ||
        (hw[TeamAssetOffsets::kSp3VfxCallHighWram[0]] != 0x00u) ||
        (hw[TeamAssetOffsets::kSp3VfxCallHighWram[0] + 1u] != 0x09u) ||
        (hw[TeamAssetOffsets::kVenSp19Band0HighWramOff + 1u] != 0xDCu) ||
        (hw[TeamAssetOffsets::kEuroRemapHighWramOff] != 0x00u) ||
        (hw[TeamAssetOffsets::kEuroRemapHighWramOff + 1u] != 0x0Bu) ||
        (read_be16(hw + TeamAssetOffsets::kCupPalHighWramOff + 6u) != rgb888_to_bgr555(222u, 226u, 234u)) ||
        (read_be16(hw + TeamAssetOffsets::kCopaRoundEndCmpHighWramOff) != 0xEC07u);
    m.vdp.copa_vfx_replace = true;
    if (need_hw_patch &&
        has_nonzero(hw + TeamAssetOffsets::kTierTblHighWramOff, TeamAssetOffsets::kTierTblBytes)) {
        write_copa_eprg_tables(hw + TeamAssetOffsets::kSpTblHighWramOff,
                               hw + TeamAssetOffsets::kTierTblHighWramOff,
                               hw + TeamAssetOffsets::kTeamGridHighWramOff,
                               hw + TeamAssetOffsets::kP2CursorHighWramOff,
                               backup.active_grid.data(),
                               hw, 0u);
        m.invalidate_all_aot_pages();
    }
    uint8_t* hw_skin = hw + TeamAssetOffsets::kMasterSkinHighWramOff;
    if (has_nonzero(hw_skin, TeamAssetOffsets::kMasterSkinBytes) &&
        (hw_skin[14u * 11u] != 1u || hw_skin[14u * 11u + 1u] != 1u ||
         hw_skin[1u  * 11u] != 1u || hw_skin[7u  * 11u] != 1u)) {
        patch_master_skin_table(hw_skin);
    }

    // 2. Hide the 4 unmentioned nations' 3D rotating badges on Team Select (0x060998A8[s].vis = 0).
    for (uint8_t s : kHiddenSlots) {
        const uint32_t off = TeamAssetOffsets::kBadgeArrayHighWramOff + static_cast<uint32_t>(s) * TeamAssetOffsets::kBadgeStride;
        if (hw[off] == 1u) {
            hw[off] = 0u;
        }
    }

    // 3. Sync P1's selected team & group into CopaTournamentState and drive Team Select group overlay.
    const uint8_t p1 = hw[0x00076552u + 1u];
    if (p1 < TeamAssetOffsets::kTeamCount && kTeamSpecs[p1].active_in_copa) {
        backup.tournament.player_team = p1;
        const int grp = find_team_group(backup.tournament, p1);
        backup.tournament.player_group = (grp >= 0) ? static_cast<uint8_t>(grp) : 0xFFu;
    }
    if (m.vdp.copa_bracket.initialized == 0u ||
        m.vdp.copa_bracket.seed != backup.tournament.seed ||
        (hw[0x0792A5u] != 5u &&
         (m.vdp.copa_bracket.last_shown_round != 0xFFu || m.vdp.copa_bracket.last_resolved_round != 0xFFu))) {
        init_copa_bracket(m.vdp.copa_bracket, backup.tournament);
    }
    if (m.vdp.copa_bracket.last_resolved_round == 0xFFu && p1 < TeamAssetOffsets::kTeamCount && kTeamSpecs[p1].active_in_copa) {
        m.vdp.copa_bracket.player_team = p1;
        const int grp = find_team_group(backup.tournament, p1);
        m.vdp.copa_bracket.player_group = (grp >= 0) ? static_cast<uint8_t>(grp) : 0u;
    }
    if (hw[0x0792A5u] == 5u && read_be16(hw + 0x097148u) == 1u && m.vdp.copa_bracket.progress_active == 0u) {
        const uint16_t rem = read_be16(hw + TeamAssetOffsets::kMatchTimerHighWramOff);
        if (rem > 0u && rem <= 300u) {
            m.vdp.copa_bracket.live_p1_goals = static_cast<uint8_t>(
                read_be16(hw + TeamAssetOffsets::kMatchP1GoalsHighWramOff) + read_be16(hw + 0x07655Eu));
            m.vdp.copa_bracket.live_p2_goals = static_cast<uint8_t>(
                read_be16(hw + TeamAssetOffsets::kMatchP2GoalsHighWramOff) + read_be16(hw + 0x076560u));
            m.vdp.copa_bracket.live_goals_valid = 1u;
        }
    }
    if (m.vdp.copa_bracket.progress_active != 0u) {
        auto& br = m.vdp.copa_bracket;
        ++br.progress_frames;
        const bool action_now = ((m.inputs.port_a & 0x07u) != 0x07u) || ((m.inputs.port_c & 0x10u) == 0u);
        if ((br.progress_frames >= 25u && action_now && br.prev_input_action == 0u) ||
            br.progress_frames >= 1200u) {
            br.progress_active = 0u;
        }
        br.prev_input_action = action_now ? 1u : 0u;
    }

    if (m.copa_ts_heartbeat > 0u) {
        m.vdp.copa_group_badges = true;
        --m.copa_ts_heartbeat;
    } else {
        m.vdp.copa_group_badges = false;
    }
}

} // namespace copa
} // namespace twc98
