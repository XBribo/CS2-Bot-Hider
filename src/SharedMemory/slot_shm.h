// slot_shm.h
//
// Shared-memory wire format between BotHider (CSS and C++)

#pragma once

#include <cstdint>

namespace cs2bh::shm {

// Windows named mapping / POSIX shm name.
#if defined(_WIN32)
inline constexpr const char* kMappingName = "Local\\CS2BotHider_Slots";
#else
inline constexpr const char* kMappingName = "/CS2BotHider_Slots";
#endif

inline constexpr uint32_t kMagic = 0x44494842; // 'BHID'
inline constexpr uint32_t kVersion = 1;
inline constexpr int kMaxSlots = 64;
inline constexpr int kNameLen = 32; // persona name buffer (incl. NUL)
inline constexpr int kCmdCount = 64; // ring-buffer entries

// Data region
inline constexpr int kOffMagic = 0; // uint32
inline constexpr int kOffVersion = 4; // uint32
inline constexpr int kOffMaxSlots = 8; // uint32
inline constexpr int kOffDataGen = 12; // uint32, bumped on each write
inline constexpr int kOffSlotState = 16; // byte[64]   0=unmanaged 1=managed
inline constexpr int kOffSyntheticSid = 80; // uint64[64]
inline constexpr int kOffPersonaName = 592; // char[64][32]

// Command region
inline constexpr int kOffWriteIdx = 2640; // uint32, C# Interlocked bump
inline constexpr int kOffReadIdx = 2644; // uint32, C++ bump
inline constexpr int kOffCmds = 2648; // Command[64], 48B each
// Ends at 2648 + 64*48 = 5720

// Extra data region
inline constexpr int kCrosshairLen = 64; // crosshair code buffer
inline constexpr int kOffCurrentPing = 5720; // int32[64]  jittered ping
inline constexpr int kOffCrosshair = 5976; // char[64][64]
// Ends at 5976 + 64*64 = 10072

// Signature/hook status region
inline constexpr int kMaxSigs = 8; // capacity
inline constexpr int kSigNameLen = 32; // name buffer (incl. NUL)
inline constexpr int kSigEntrySize = 40; // char[32] name + uint64 addr
inline constexpr int kOffSigCount = 10072; // uint32, number of valid entries
inline constexpr int kOffSigEntries = 10080; // SigEntry[kMaxSigs], 8-byte aligned
// Ends at 10080 + 8*40 = 10400

// Scoreboard flair region
inline constexpr int kOffScoreboardFlair = 10400; // uint32[64]
// Ends at 10400 + 64*4 = 10656

// Base identity and native slot incarnation use the existing reserved space
inline constexpr int kOffBaseSyntheticSid = 10656; // uint64[64]
inline constexpr int kOffBasePersonaName = 11168; // char[64][32]
inline constexpr int kOffIncarnation = 13216; // uint64[64]
// Ends at 13216 + 64*8 = 13728

// Custom avatar request and native application state
inline constexpr int kAvatarMaxBytes = 16 * 1024;
inline constexpr int kOffAvatarSequence = 13728; // uint32[64], seqlock
inline constexpr int kOffAvatarLength = 13984; // uint32[64]
inline constexpr int kOffAvatarIncarnation = 14240; // uint64[64]
inline constexpr int kOffAvatarApplied = 14752; // byte[64]
inline constexpr int kOffAvatarAppliedSid = 14816; // uint64[64]
inline constexpr int kOffAvatarData = 16384; // byte[64][16 KiB]

inline constexpr int kTotalSize = kOffAvatarData + kMaxSlots * kAvatarMaxBytes;
static_assert(kOffAvatarAppliedSid + kMaxSlots * sizeof(uint64_t) <= kOffAvatarData, "Avatar metadata overlaps avatar data");
static_assert(kOffAvatarData + kMaxSlots * kAvatarMaxBytes <= kTotalSize, "Shared-memory data exceeds the mapping size");

// Command opcodes.
enum CmdType : uint8_t
{
    CmdNone = 0,
    CmdSetSteamId = 1,
    CmdSetPersona = 2,
    CmdSetIdentityMode = 3, // global mode carried in Command.SteamId (0=player 1=bot)
    CmdSetNameSource = 7, // global toggle, name source carried in Command.SteamId (1=bot_info 0=botprofile)
};

// Sentinel slot for global commands
inline constexpr uint8_t kSlotAll = 255;

// One ring-buffer command.
#pragma pack(push, 1)
struct Command
{
    uint8_t type; // CmdType
    uint8_t slot; // target slot
    uint8_t pad[6]; // align SteamId to 8
    uint64_t steamId; // payload for CmdSetSteamId
    char name[kNameLen]; // payload for CmdSetPersona
};
#pragma pack(pop)

static_assert(sizeof(Command) == 48, "Command must be 48 bytes");

// One signature/hook status entry. addr==0 means unresolved
#pragma pack(push, 1)
struct SigEntry
{
    char name[kSigNameLen]; // signature name
    uint64_t addr; // resolved address, 0 if unresolved
};
#pragma pack(pop)

static_assert(sizeof(SigEntry) == kSigEntrySize, "SigEntry must be 40 bytes");

} // namespace cs2bh::shm
