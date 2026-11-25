#pragma once
// Mod prototype for H5 ("Copa America" vs "Classic World Cup '98").
// Portable C++17 (no Win32 / OS dependencies) operating on StvMachineState.

#include <array>
#include <cstdint>

namespace twc98 {

class StvMachineState;

namespace copa {

enum class GameMode : uint8_t {
    Classic = 0,
    CopaAmerica = 1,
};

struct CopaTeamSpec {
    uint8_t slot = 0u;
    bool active_in_copa = false;
    char code[4]{};
    uint8_t sp[3]{};
    uint8_t tier = 0xFFu;
};

struct TeamAssetOffsets {
    static constexpr uint32_t kTeamCount = 16u;
    static constexpr uint32_t kTeamSlot = 14u; // Bolivia (La Verde) slot in Copa America

    // Cartridge ROM (0x02000000..0x033FFFFF -> cart_rom[0..0x1400000))
    static constexpr uint32_t kTFlagBaseOff = 0x00400EC0u;
    static constexpr uint32_t kTFlagStride = 0x9814u;
    static constexpr uint32_t kTFlagSize = 38932u;
    static constexpr uint32_t kTFlag14Off = kTFlagBaseOff + kTeamSlot * kTFlagStride; // 0x00485FD8
    static constexpr uint32_t kTFlag14Size = kTFlagSize;

    // BG_TM offsets/sizes per repurposed team slot {1:PER, 5:CHI, 6:ECU, 7:VEN, 11:URU, 13:PAR, 14:BOL}
    static constexpr uint32_t kBgTm01Off = 0x0063F790u;
    static constexpr uint32_t kBgTm01Size = 0x1C94u;
    static constexpr uint32_t kBgTm05Off = 0x00646E40u;
    static constexpr uint32_t kBgTm05Size = 0x1E94u;
    static constexpr uint32_t kBgTm06Off = 0x00648CD4u;
    static constexpr uint32_t kBgTm06Size = 0x1BF4u;
    static constexpr uint32_t kBgTm07Off = 0x0064A8C8u;
    static constexpr uint32_t kBgTm07Size = 0x2454u;
    static constexpr uint32_t kBgTm11Off = 0x00652A78u;
    static constexpr uint32_t kBgTm11Size = 0x2034u;
    static constexpr uint32_t kBgTm13Off = 0x00656AA0u;
    static constexpr uint32_t kBgTm13Size = 0x2254u;
    static constexpr uint32_t kBgTm14Off = 0x00658CF4u;
    static constexpr uint32_t kBgTm14Size = 0x2174u;
    static constexpr uint32_t kMaxBgTmSize = 0x2454u;

    // Team Select flag grid archive (CEL_OUT.DAT + MSG_OUT.DAT)
    static constexpr uint32_t kCelOutTilesBaseOff = 0x009C8A3Cu; // 0x009C8A30 + 0x0C header
    static constexpr uint32_t kCelOutPalsBaseOff  = 0x009DC0FCu; // pal * 32 bytes BE BGR555
    static constexpr uint32_t kCelOutPal11Off     = kCelOutPalsBaseOff + 11u * 32u;
    static constexpr uint32_t kMsgOutPosBaseOff   = 0x009DCEF4u; // slot * 6 + 2 -> (cellX<<8)|cellY BE16
    static constexpr uint32_t kMsgOutPosStride    = 6u;
    static constexpr uint32_t kMsgOutPosXyOff     = 2u;
    static constexpr uint32_t kMsgOutBaseOff      = 0x009DE060u; // slot * 96 bytes (12x4 map)
    static constexpr uint32_t kMsgOutMapBytes     = 96u;

    // 3D Player Uniform Textures (TEXTM00..15 / TEXHM00..15)
    static constexpr uint32_t kTexTmBaseOff = 0x00794468u;
    static constexpr uint32_t kTexHmBaseOff = 0x00896804u;
    static constexpr uint32_t kTexStride    = 0x0000D974u;
    static constexpr uint32_t kTexTm14Off   = kTexTmBaseOff + kTeamSlot * kTexStride; // 0x008528C0
    static constexpr uint32_t kTexHm14Off   = kTexHmBaseOff + kTeamSlot * kTexStride; // 0x00954C5C
    static constexpr uint32_t kTexPalOff    = 0x0Eu;
    static constexpr uint32_t kTexPalBytes  = 96u; // 3 x 16 x uint16 BE BGR555
    static constexpr uint32_t kTexDescOff   = 112u;
    static constexpr uint32_t kTexDescBytes = 49u * 4u;
    static constexpr uint32_t kTexPixOff    = 0x134u;
    static constexpr uint32_t kTexPixBytes  = kTexStride - kTexPixOff; // 55360u

    // Player stats & skin/GK flags: CAPA_TBL slice 0 (cart + low_wram)
    static constexpr uint32_t kCapaTblSlice0Off  = 0x009FDDACu;
    static constexpr uint32_t kCapaTblLowWramOff = 0x000F7144u;
    static constexpr uint32_t kCapaTeamBytes     = 11u * 48u; // 528u
    static constexpr uint32_t kCapaAllBytes      = kTeamCount * kCapaTeamBytes; // 8448u
    static constexpr uint32_t kCapaGkSlot14Off   = kCapaTblSlice0Off + kTeamSlot * kCapaTeamBytes + 25u;
    static constexpr uint32_t kCapaGkLowWramOff  = kCapaTblLowWramOff + kTeamSlot * kCapaTeamBytes + 25u;
    static constexpr uint32_t kCapaGkSkillBytes  = 4u;
    static constexpr uint32_t kCapaTblEprgCartOff = 0x002C0144u; // CAPA_TBL active copy in EPRG (+0x200000 / +0x300000)
    static constexpr uint32_t kCapaTblHighWramOff = 0x000C0144u; // 0x060C0144 in high_wram

    // SH-2 EPRG tables (cart_rom = high_wram + 0x200000)
    static constexpr uint32_t kSpTblCartOff        = 0x00265A24u;
    static constexpr uint32_t kSpTblHighWramOff    = 0x00065A24u;
    static constexpr uint32_t kSpTblSlot14CartOff  = kSpTblCartOff + kTeamSlot * 3u;
    static constexpr uint32_t kSpTblTotalBytes     = kTeamCount * 3u; // 48u

    // Master per-player skin/hair table read by 0x0603EE36 for BOTH Team Select preview & 3D Match
    static constexpr uint32_t kMasterSkinCartOff     = 0x002758F1u;
    static constexpr uint32_t kMasterSkinHighWramOff = 0x000758F1u;
    static constexpr uint32_t kMasterSkinBytes       = kTeamCount * 11u; // 176u

    static constexpr uint32_t kTierTblCartOff      = 0x002650ECu;
    static constexpr uint32_t kTierTblHighWramOff  = 0x000650ECu;
    static constexpr uint32_t kTierTblBytes        = kTeamCount; // 16u

    static constexpr uint32_t kTeamGridCartOff     = 0x00275B3Cu;
    static constexpr uint32_t kTeamGridHighWramOff = 0x00075B3Cu;
    static constexpr uint32_t kTeamGridBytes       = 18u; // 3x6 grid

    // P2 / default cursor initialization opcode: 0x06005906 (E102 -> E103 ARG)
    static constexpr uint32_t kP2CursorCartOff     = 0x00205906u;
    static constexpr uint32_t kP2CursorHighWramOff = 0x00005906u;

    // Euro/World slot 9->18 (RSA) and 13->17 (SCO) remap fn: 0x060050F8 (patch to rts; mov r4,r0)
    static constexpr uint32_t kEuroRemapCartOff     = 0x002050F8u;
    static constexpr uint32_t kEuroRemapHighWramOff = 0x000050F8u;
    static constexpr uint32_t kEuroRemapBytes       = 4u;

    // Cart ROM contains a duplicate EPRG image at +0x00300000 loaded on cold boot
    static constexpr uint32_t kEprgMirrorDelta = 0x00100000u;

    // 3D Team Select flag badge array in High WRAM (16 x 108B; +0 visibility byte)
    static constexpr uint32_t kBadgeArrayHighWramOff = 0x000998A8u;
    static constexpr uint32_t kBadgeStride           = 108u;

    // Active SP 3 (Bolivia Condor Wind Shot) VFX patch points in High WRAM / EPRG:
    //   0x0604EE28 (0x06040C40: darkening -96 + camera zoom 35) MUST remain active!
    //   0x0604EE32 (stack0 arg 1->0) + 0x0604EFE0 (4B lit 0x0604114A -> 0x06040D46 clean 40f wind/dust wait)
    //   0x0604EE6C (fireball 0x060440B4 -> NOP), 0x0604EE7E (explosion puffs 0x0604D1CC -> NOP)
    static constexpr uint32_t kSp3VfxCallHighWram[2] = {
        0x0004EE6Cu, 0x0004EE7Eu
    };
    static constexpr uint32_t kSp3VfxCallCart[2] = {
        0x0024EE6Cu, 0x0024EE7Eu
    };
    static constexpr uint32_t kSp3ArgHighWramOff = 0x0004EE32u;
    static constexpr uint32_t kSp3ArgCartOff     = 0x0024EE32u;
    static constexpr uint32_t kSp3EmitLitHighWramOff = 0x0004EFE0u;
    static constexpr uint32_t kSp3EmitLitCartOff     = 0x0024EFE0u;
    static constexpr uint32_t kSp3CleanWaitFnAddr    = 0x06040D46u;

    // 19x19 uint16 BE automatic kit-clash matrix read by 0x06007424 (0x8000=flip T0, 0x4000=flip T1)
    static constexpr uint32_t kKitClashCartOff     = 0x00269686u;
    static constexpr uint32_t kKitClashHighWramOff = 0x00069686u;
    static constexpr uint32_t kKitClashBytes       = 19u * 19u * 2u; // 722u

    // Venezuela SP 19 (0x0604F880) waving flag: band 0 index in SP19[0] + 48x24 4bpp texture 0x00DD in TEXGM.DAT
    static constexpr uint32_t kVenSp19Band0CartOff     = 0x00268B8Au;
    static constexpr uint32_t kVenSp19Band0HighWramOff = 0x00068B8Au;
    static constexpr uint32_t kTexGmVenFlagOff         = 0x00789508u; // TEXGM idx 0x00DD (srca 0x415E0)
    static constexpr uint32_t kTexGmVenFlagBytes       = (48u * 24u) / 2u; // 576u

    // Match runtime state & B-H5-05 / B-H6-04 patch points in High WRAM
    static constexpr uint32_t kBallStateHighWramOff       = 0x00077E68u;
    static constexpr uint32_t kActiveSpHighWramOff        = 0x000991E0u;
    static constexpr uint32_t kMatchBarP1HighWramOff      = 0x00095708u;
    static constexpr uint32_t kMatchBarP2HighWramOff      = 0x00095718u;
    static constexpr uint32_t kAiDiff1RetHighWramOff      = 0x0003EB84u; // 0x6063 -> 0xE01F (mov #31, r0)
    static constexpr uint32_t kAiDiff2IdxHighWramOff      = 0x0003F872u; // 0x6EEE -> 0xEE1F (mov #31, r14)
    static constexpr uint32_t kDipDifficultyHighWramOff   = 0x00076520u; // uint16 BE (3 = VERY HARD)
    static constexpr uint32_t kDipTime1pHighWramOff       = 0x00076522u; // uint16 BE 1P match timer (sec)
    static constexpr uint32_t kDipTime2pHighWramOff       = 0x00076528u; // uint16 BE 2P VS match timer (sec)
    static constexpr uint32_t kMatchTimerHighWramOff      = 0x0007654Eu; // uint16 BE remaining seconds
    static constexpr uint32_t kMatchExtraTimerHighWramOff = 0x00076550u; // uint16 BE stoppage seconds
    static constexpr uint32_t kMatchP1GoalsHighWramOff    = 0x0007655Au; // uint16 BE P1 goals
    static constexpr uint32_t kMatchP2GoalsHighWramOff    = 0x0007655Cu; // uint16 BE P2 goals
    static constexpr uint32_t kMatchHudTimerHighWramOff   = 0x000970C4u; // uint16 BE HUD display seconds

    // B-H5-06 & B-H5-07: 3D Copa America Trophy + Winner Ending Assets
    static constexpr uint32_t kPolCupOff                  = 0x006358B4u; // POL_CUP.DAT (5912B)
    static constexpr uint32_t kPolCupBytes                = 5912u;
    static constexpr uint32_t kCupCelOff                  = 0x00636FCCu; // CUP_CEL.DAT (25392B)
    static constexpr uint32_t kCupCelBytes                = 25392u;
    static constexpr uint32_t kCupPalCartOff              = 0x0027077Cu; // VDP1 Trophy + Ring palettes (64B)
    static constexpr uint32_t kCupPalHighWramOff          = 0x0007077Cu;
    static constexpr uint32_t kCupPalBytes                = 64u;
    static constexpr uint32_t kMsgRankOff                 = 0x009F7648u; // MSG_RANK.DAT (26144B)
    static constexpr uint32_t kMsgRankBytes               = 26144u;
    static constexpr uint32_t kTexEnBytes                 = 79380u;      // TEX_ENxx.DAT (352x224 8bpp)
    static constexpr uint32_t kMaxCelEnBytes              = 60328u;      // Max size among 7 CEL_ENxx.DAT files

    // B-H5-08: 6 Tournament Rounds (0..2 Group, 3 QF, 4 SF, 5 Final) + 7th Round (6) = All-Star -> Ending (7)
    static constexpr uint32_t kCopaRoundEndCmpCartOff     = 0x002060A2u; // EC08 -> EC07 (round >= 7 => Ending)
    static constexpr uint32_t kCopaRoundEndCmpHighWramOff = 0x000060A2u;
    static constexpr uint32_t kCopaOppAllStarCartOff      = 0x00207D02u; // 8807 -> 8806 (round == 6 => slot 16 All-Star)
    static constexpr uint32_t kCopaOppAllStarHighWramOff  = 0x00007D02u;
    static constexpr uint32_t kCopaContEndCartOff         = 0x002080C6u; // E207 -> E206
    static constexpr uint32_t kCopaContEndHighWramOff     = 0x000080C6u;
    static constexpr uint32_t kCopaVsAllStarCartOff       = 0x002529BCu; // 8807 -> 8806 (VS All-Star reveal at round 6)
    static constexpr uint32_t kCopaVsAllStarHighWramOff   = 0x000529BCu;
    static constexpr uint32_t kRoundSchedCartOff          = 0x002650FDu; // 8-byte round tier schedule
    static constexpr uint32_t kRoundSchedHighWramOff      = 0x000650FDu;
    static constexpr uint32_t kRoundSchedBytes            = 8u;
};

enum class MatchRuleMode : uint8_t {
    Default   = 0u, // Original arcade timers
    TimeLimit = 1u, // Custom timer in seconds (apply to both 1P and 2P/Online VS)
    GoalLimit = 2u, // Match ends when either side reaches target goals
};

struct MatchRulesConfig {
    bool hard_mode = false;
    MatchRuleMode rule_mode = MatchRuleMode::Default;
    uint16_t rule_value = 90u;
};

struct CopaTournamentState {
    static constexpr uint32_t kGroupCount = 3u;
    static constexpr uint32_t kTeamsPerGroup = 4u;
    static constexpr uint32_t kActiveTeamCount = 12u;
    static constexpr uint32_t kKnockoutSize = 8u; // Top 2 per group + 2 best 3rds

    enum class Stage : uint8_t {
        GroupStage = 0,
        QuarterFinals = 1,
        SemiFinals = 2,
        Final = 3,
        Champion = 4,
    };

    bool initialized = false;
    uint32_t seed = 0u;
    std::array<std::array<uint8_t, kTeamsPerGroup>, kGroupCount> groups{}; // Randomized slot IDs
    std::array<uint8_t, kKnockoutSize> knockout_bracket{}; // Reserved for future Progress Screen (0xFF = TBD)
    uint8_t player_team = 0u;   // Currently highlighted / locked P1 slot ID
    uint8_t player_group = 0u;  // 0=A, 1=B, 2=C (0xFF if inactive/hidden)
    uint8_t round_index = 0u;   // Match progression counter for future Progress Screen
    Stage stage = Stage::GroupStage;
};

struct CopaModBackup {
    static constexpr uint32_t kModSlotCount = 7u;   // {1:PER, 5:CHI, 6:ECU, 7:VEN, 11:URU, 13:PAR, 14:BOL}
    static constexpr uint32_t kHiddenMsgCount = 7u; // {2, 4, 8, 9, 16, 17, 18}
    static constexpr uint32_t kSafeCelPoolSize = 400u;

    bool captured = false;
    bool wram_captured = false;
    GameMode active_mode = GameMode::Classic;
    CopaTournamentState tournament{};
    std::array<uint8_t, TeamAssetOffsets::kTeamGridBytes> active_grid{};
    std::array<uint8_t, TeamAssetOffsets::kTierTblBytes> active_tiers{};

    std::array<uint8_t, TeamAssetOffsets::kTFlagSize> orig_tflag[kModSlotCount]{};
    std::array<uint8_t, TeamAssetOffsets::kMaxBgTmSize> orig_bgtm[kModSlotCount]{};
    std::array<uint8_t, TeamAssetOffsets::kTexStride> orig_textm[kModSlotCount]{};
    std::array<uint8_t, TeamAssetOffsets::kTexStride> orig_texhm[kModSlotCount]{};

    std::array<uint8_t, kModSlotCount * 32u> orig_cel_pals{};
    std::array<uint8_t, kSafeCelPoolSize * 32u> orig_cel_pool_tiles{}; // kSafeCelPoolSize * 32B
    std::array<uint8_t, kModSlotCount * TeamAssetOffsets::kMsgOutMapBytes> orig_msg_maps{};
    std::array<uint8_t, kHiddenMsgCount * TeamAssetOffsets::kMsgOutMapBytes> orig_hidden_msg_maps{};
    std::array<uint16_t, TeamAssetOffsets::kTeamCount> orig_msg_pos_xy{};

    std::array<uint8_t, TeamAssetOffsets::kCapaAllBytes> orig_capa_cart{};
    std::array<uint8_t, TeamAssetOffsets::kCapaAllBytes> orig_capa_lwram{};
    std::array<uint8_t, TeamAssetOffsets::kCapaAllBytes> orig_capa_eprg{};
    std::array<uint8_t, TeamAssetOffsets::kCapaAllBytes> orig_capa_eprg_m{};
    std::array<uint8_t, TeamAssetOffsets::kCapaAllBytes> orig_capa_hwram{};

    std::array<uint8_t, TeamAssetOffsets::kSpTblTotalBytes> orig_sp_cart{};
    std::array<uint8_t, TeamAssetOffsets::kSpTblTotalBytes> orig_sp_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kSpTblTotalBytes> orig_sp_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kMasterSkinBytes> orig_master_skin_cart{};
    std::array<uint8_t, TeamAssetOffsets::kMasterSkinBytes> orig_master_skin_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kMasterSkinBytes> orig_master_skin_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kTierTblBytes> orig_tiers_cart{};
    std::array<uint8_t, TeamAssetOffsets::kTierTblBytes> orig_tiers_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kTierTblBytes> orig_tiers_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kTeamGridBytes> orig_grid_cart{};
    std::array<uint8_t, TeamAssetOffsets::kTeamGridBytes> orig_grid_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kTeamGridBytes> orig_grid_hwram{};
    std::array<uint8_t, 2u> orig_p2_cursor_cart{};
    std::array<uint8_t, 2u> orig_p2_cursor_cart_m{};
    std::array<uint8_t, 2u> orig_p2_cursor_hwram{};
    std::array<uint8_t, 4u> orig_sp3_vfx_cart{};
    std::array<uint8_t, 4u> orig_sp3_vfx_cart_m{};
    std::array<uint8_t, 4u> orig_sp3_vfx_hwram{};
    std::array<uint8_t, 2u> orig_sp3_arg_cart{};
    std::array<uint8_t, 2u> orig_sp3_arg_cart_m{};
    std::array<uint8_t, 2u> orig_sp3_arg_hwram{};
    std::array<uint8_t, 4u> orig_sp3_emit_cart{};
    std::array<uint8_t, 4u> orig_sp3_emit_cart_m{};
    std::array<uint8_t, 4u> orig_sp3_emit_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kKitClashBytes> orig_kit_clash_cart{};
    std::array<uint8_t, TeamAssetOffsets::kKitClashBytes> orig_kit_clash_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kKitClashBytes> orig_kit_clash_hwram{};
    std::array<uint8_t, 2u> orig_ven_band0_cart{};
    std::array<uint8_t, 2u> orig_ven_band0_cart_m{};
    std::array<uint8_t, 2u> orig_ven_band0_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kTexGmVenFlagBytes> orig_texgm_ven_flag{};
    std::array<uint8_t, TeamAssetOffsets::kEuroRemapBytes> orig_euro_remap_cart{};
    std::array<uint8_t, TeamAssetOffsets::kEuroRemapBytes> orig_euro_remap_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kEuroRemapBytes> orig_euro_remap_hwram{};

    // B-H5-06 & B-H5-07 Trophy + Champion Ending backups
    std::array<uint8_t, TeamAssetOffsets::kPolCupBytes> orig_pol_cup{};
    std::array<uint8_t, TeamAssetOffsets::kCupCelBytes> orig_cup_cel{};
    std::array<uint8_t, TeamAssetOffsets::kCupPalBytes> orig_cup_pal_cart{};
    std::array<uint8_t, TeamAssetOffsets::kCupPalBytes> orig_cup_pal_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kCupPalBytes> orig_cup_pal_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kMsgRankBytes> orig_msg_rank{};
    std::array<uint8_t, TeamAssetOffsets::kTexEnBytes> orig_tex_en[CopaTournamentState::kActiveTeamCount]{};
    std::array<uint8_t, TeamAssetOffsets::kMaxCelEnBytes> orig_cel_en[kModSlotCount]{};
    std::array<uint8_t, 224u * 352u> scratch_cel_canvas{};

    // B-H5-08: 6-Round + 7th All-Star patch backups (4 x 2B opcodes + 8B tier schedule)
    std::array<uint8_t, 8u> orig_round_ops_cart{};
    std::array<uint8_t, 8u> orig_round_ops_cart_m{};
    std::array<uint8_t, 8u> orig_round_ops_hwram{};
    std::array<uint8_t, TeamAssetOffsets::kRoundSchedBytes> orig_round_sched_cart{};
    std::array<uint8_t, TeamAssetOffsets::kRoundSchedBytes> orig_round_sched_cart_m{};
    std::array<uint8_t, TeamAssetOffsets::kRoundSchedBytes> orig_round_sched_hwram{};

    // B-H5-05 & B-H6-04 Hard Mode / Custom Match Rules backup & active config (works in Classic & Copa)
    bool hard_patch_captured = false;
    bool hard_patch_applied = false;
    std::array<uint8_t, 2u> orig_ai_diff1{};
    std::array<uint8_t, 2u> orig_ai_diff2{};
    std::array<uint8_t, 2u> orig_dip_diff{};
    bool time_patch_captured = false;
    bool time_patch_applied = false;
    std::array<uint8_t, 2u> orig_dip_time_1p{};
    std::array<uint8_t, 2u> orig_dip_time_2p{};
    MatchRulesConfig match_rules{};
};

// Returns read-only metadata for all 16 slots in Copa America mode.
const std::array<CopaTeamSpec, TeamAssetOffsets::kTeamCount>& copa_team_specs();

// Generates a non-zero tournament draw seed for Copa America 3x4 groups.
uint32_t generate_copa_tournament_seed();

// Shuffles the 12 active Copa America teams into 3 random groups of 4 (deterministic for seed != 0).
void build_random_copa_groups(CopaTournamentState& state, uint32_t seed);

// Returns group index (0=A, 1=B, 2=C) for a slot ID, or -1 if hidden/not drawn.
int find_team_group(const CopaTournamentState& state, uint8_t slot);

// Captures pristine Classic bytes (once per machine session) and applies either
// GameMode::Classic (100% bit-exact restore) or GameMode::CopaAmerica (12 teams in 3 random groups of 4,
// Venezuela slot 7, HUD flags 1..48, Bolivia dark green + brown skin + Super GK + SP3 Wind VFX, 4 hidden teams).
bool apply_game_mode(StvMachineState& m, GameMode mode, CopaModBackup& backup, uint32_t copa_seed = 0u);

// Call once per emulated frame in CopaAmerica mode (also executes active Hard Mode / Match Rules).
void on_frame_tick(StvMachineState& m, CopaModBackup& backup);

// Invoked automatically before SP/3D-kit/XX.BIN asset loads (0x060085FE / 0x06008624 / 0x06007DAE)
// to lock the scheduled Copa America opponent before any match assets or SP tables are read.
void on_opponent_select_hook(StvMachineState& m);

// Invoked automatically right before VS screen asset load (0x060529A6) to resolve & schedule
// Copa America 3x4 group matches, 8-team knockout bracket, and 7th-round All-Star opponent.
void on_vs_entry_hook(StvMachineState& m);

// Configure B-H5-05 Hard Mode (constant max AI level 31 from match 1) and B-H6-04 Match Rules
// (extended time or first-to-N goals). Safe across both Classic '98 and Copa America.
void set_match_rules(StvMachineState& m, CopaModBackup& backup, const MatchRulesConfig& rules);

// Call once per emulated frame regardless of edition to enforce Hard Mode and custom Match Rules.
void on_match_rules_tick(StvMachineState& m, CopaModBackup& backup);

inline const char* game_mode_name(GameMode mode) {
    return (mode == GameMode::CopaAmerica) ? "COPA AMERICA" : "CLASSIC WC98";
}

} // namespace copa
} // namespace twc98
