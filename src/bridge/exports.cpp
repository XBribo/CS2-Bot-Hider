#include "bridge.h"

#include "plugin.h"
#include "fake_client_manager.h"

#include <cstring>

#ifdef _WIN32
#define BH_EXPORT extern "C" __declspec(dllexport)
#else
#define BH_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
using namespace cs2bh;

bool Connected() { return Manager().IsReady() && Manager().HasProvider(); }

bool ValidSlot(int slot) { return Connected() && slot >= 0 && slot < bridge::kMaxSlots; }

bool Target(int slot, uint64_t incarnation)
{
    if (!ValidSlot(slot)) return false;
    const auto& state = Manager().Slot(slot);
    return state.managed && incarnation != 0 && incarnation == state.incarnation;
}

// Contain C++ exceptions.
template <typename F> int Apply(F&& operation) noexcept
{
    try
    {
        return operation() ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}

template <size_t N> bool CopyInput(char (&buffer)[N], const char* text, int length)
{
    if (!text || length < 0 || length >= static_cast<int>(N) || std::memchr(text, 0, length)) return false;
    std::memcpy(buffer, text, length);
    buffer[length] = 0;
    return true;
}

bool HasPngStructure(const unsigned char* data, size_t length)
{
    constexpr unsigned char signature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (!data || length < sizeof(signature) || std::memcmp(data, signature, sizeof(signature))) return false;
    const auto read32 = [](const unsigned char* p) {
        return (uint32_t{ p[0] } << 24) | (uint32_t{ p[1] } << 16) | (uint32_t{ p[2] } << 8) | uint32_t{ p[3] };
    };

    // Check the container only; CRCs and compressed pixels are not validated.
    bool hasImageData = false;
    size_t offset = sizeof(signature);
    while (length - offset >= 12)
    {
        const auto* chunk = data + offset;
        const uint32_t size = read32(chunk);
        if (size > length - offset - 12) return false;
        const bool isHeader = std::memcmp(chunk + 4, "IHDR", 4) == 0;
        if (offset == sizeof(signature))
        {
            if (!isHeader || size != 13 || !read32(chunk + 8) || !read32(chunk + 12)) return false;
        }
        else if (isHeader)
            return false;

        if (std::memcmp(chunk + 4, "IDAT", 4) == 0 && size != 0) hasImageData = true;
        offset += size + 12;
        if (std::memcmp(chunk + 4, "IEND", 4) == 0) return hasImageData && size == 0 && offset == length;
    }
    return false;
}
} // namespace

BH_EXPORT int BH_Attach() noexcept { return cs2bh::Manager().Attach() ? 1 : 0; }

BH_EXPORT void BH_Detach() noexcept
{
    if (cs2bh::Manager().IsReady()) cs2bh::Manager().Detach();
}

BH_EXPORT int BH_GetSlot(int slot, cs2bh::bridge::SlotSnapshot* output, int size) noexcept
{
    if (!ValidSlot(slot) || !output || size != static_cast<int>(sizeof(*output))) return 0;
    *output = cs2bh::Manager().Slot(slot);
    return 1;
}

BH_EXPORT int BH_GetSignatures(cs2bh::bridge::Signature* output, int capacity) noexcept
{
    if (!Connected() || !output || capacity < cs2bh::Manager().SignatureCount()) return 0;
    int count = cs2bh::Manager().SignatureCount();
    std::memcpy(output, cs2bh::Manager().Signatures(), count * sizeof(*output));
    return count;
}

BH_EXPORT int BH_SetSteamId(int slot, uint64_t incarnation, uint64_t steamId) noexcept
{
    return Apply([&] {
        return Target(slot, incarnation) && steamId != 0 && cs2bh::g_plugin.ApplySteamIdCommand(slot, steamId);
    });
}

BH_EXPORT int BH_SetName(int slot, uint64_t incarnation, const char* name, int length) noexcept
{
    return Apply([&] {
        char buffer[cs2bh::bridge::kNameLen];
        return Target(slot, incarnation) && length > 0 && CopyInput(buffer, name, length) &&
               cs2bh::g_plugin.ApplyPersonaNameCommand(slot, buffer);
    });
}

BH_EXPORT int BH_SetCrosshair(int slot, uint64_t incarnation, const char* code, int length) noexcept
{
    return Apply([&] {
        char buffer[cs2bh::bridge::kCrosshairLen];
        if (!Target(slot, incarnation) || !CopyInput(buffer, code, length)) return false;
        cs2bh::Manager().UpdateCrosshair(slot, buffer);
        return true;
    });
}

BH_EXPORT int BH_SetFlair(int slot, uint64_t incarnation, uint32_t flair) noexcept
{
    if (!Target(slot, incarnation) || flair > UINT16_MAX) return 0;
    cs2bh::Manager().UpdateScoreboardFlair(slot, flair);
    return 1;
}

BH_EXPORT int BH_SetAvatar(int slot, uint64_t incarnation, const unsigned char* data, int length) noexcept
{
    return Apply([&] {
        if (!Target(slot, incarnation) || length < 0 || length > cs2bh::bridge::kAvatarMaxBytes) return false;
        if (length && !HasPngStructure(data, static_cast<size_t>(length))) return false;
        cs2bh::Manager().SetAvatarRequest(slot, data, static_cast<uint32_t>(length));
        return true;
    });
}

BH_EXPORT int BH_SetMode(int mode) noexcept
{
    return Apply([&] {
        return Connected() && (mode == 0 || mode == 1) &&
               cs2bh::g_plugin.SetIdentityMode(mode == 1 ? cs2bh::IdentityMode::Bot : cs2bh::IdentityMode::Player);
    });
}

BH_EXPORT int BH_SetNameSource(int useBotInfo) noexcept
{
    if (!Connected() || (useBotInfo != 0 && useBotInfo != 1)) return 0;
    cs2bh::g_plugin.SetUseBotInfoName(useBotInfo == 1);
    return 1;
}
