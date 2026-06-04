// slot_shm.h
//
// Shared-memory wire format between BotHider (CSS and C++)

#pragma once

#include <cstdint>

namespace cs2bh::shm
{

    // Named mapping in the Local session namespace
    inline constexpr const char *kMappingName = "Local\\CS2BotHider_Slots";

    inline constexpr uint32_t kMagic = 0x44494842; // 'BHID'
    inline constexpr uint32_t kVersion = 1;
    inline constexpr int kMaxSlots = 64;
    inline constexpr int kNameLen = 32;  // persona name buffer (incl. NUL)
    inline constexpr int kCmdCount = 64; // ring-buffer entries

    // Data region (C++ writes / C# reads)
    inline constexpr int kOff_Magic = 0;         // uint32
    inline constexpr int kOff_Version = 4;       // uint32
    inline constexpr int kOff_MaxSlots = 8;      // uint32
    inline constexpr int kOff_DataGen = 12;      // uint32, bumped on each write
    inline constexpr int kOff_SlotState = 16;    // byte[64]   0=unmanaged 1=managed
    inline constexpr int kOff_SyntheticSid = 80; // uint64[64]
    inline constexpr int kOff_PersonaName = 592; // char[64][32]

    // Command region (C# writes / C++ reads)
    inline constexpr int kOff_WriteIdx = 2640; // uint32, C# Interlocked bump
    inline constexpr int kOff_ReadIdx = 2644;  // uint32, C++ bump
    inline constexpr int kOff_Cmds = 2648;     // Command[64], 48B each
    // Ends at 2648 + 64*48 = 5720

    // Extra data region (C++ writes / C# reads)
    inline constexpr int kCrosshairLen = 64;      // crosshair code buffer
    inline constexpr int kOff_CurrentPing = 5720; // int32[64]  jittered ping
    inline constexpr int kOff_Crosshair = 5976;   // char[64][64]
    // Ends at 5976 + 64*64 = 10072

    inline constexpr int kTotalSize = 16384; // 4 pages

    // Command opcodes.
    enum CmdType : uint8_t
    {
        kCmd_None = 0,
        kCmd_SetSteamId = 1,
        kCmd_SetPersona = 2,
        kCmd_SetDisguise = 3, // global toggle, on/off carried in Command.SteamId
    };

    // Sentinel slot for global (non-per-slot) commands
    inline constexpr uint8_t kSlot_All = 255;

// One ring-buffer command. Fixed 48 bytes, covers both opcodes
#pragma pack(push, 1)
    struct Command
    {
        uint8_t Type;        // CmdType
        uint8_t Slot;        // target slot
        uint8_t Pad[6];      // align SteamId to 8
        uint64_t SteamId;    // payload for kCmd_SetSteamId
        char Name[kNameLen]; // payload for kCmd_SetPersona
    };
#pragma pack(pop)

    static_assert(sizeof(Command) == 48, "Command must be 48 bytes");

} // namespace cs2bh::shm
