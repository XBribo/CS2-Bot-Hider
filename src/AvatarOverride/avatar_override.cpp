#include "avatar_override.h"

#include "fake_client_manager.h"
#include "personas.h"
#include "plugin.h"
#include "slot_publisher.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <networkstringtabledefs.h>
#include <tier1/convar.h>

namespace cs2bh::avatar
{

    struct AvatarRuntimeState
    {
        uint32_t ProcessedSequence = 0xFFFFFFFFu;
        uint64_t ProcessedSteamId = 0;
        uint64_t AppliedSteamId = 0;
        bool Applied = false;
    };

    static INetworkStringTableContainer *g_pNetworkStringTables = nullptr;
    static std::array<AvatarRuntimeState, PersonaPool::kMaxSlots> g_AvatarStates{};
    static INetworkStringTable *g_pLastAvatarTable = nullptr;

    // Formats a SteamID64 key for ServerAvatarOverrides
    static void FormatAvatarSteamId(uint64_t steamId, char *buffer, size_t length)
    {
        std::snprintf(buffer, length, "%llu",
                      static_cast<unsigned long long>(steamId));
    }

    // Clears one SteamID entry from ServerAvatarOverrides
    static void ClearAvatarOverride(INetworkStringTable *table, uint64_t steamId)
    {
        if (!table || steamId == 0)
            return;
        char key[32];
        FormatAvatarSteamId(steamId, key, sizeof(key));
        int index = table->FindStringIndex(key);
        if (index <= 0)
            return;

        SetStringUserDataRequest_t empty{};
        if (!table->SetStringUserData(index, &empty, true))
        {
            META_CONPRINTF("[BOTHIDER] warning: failed to clear avatar sid=%s index=%d\n",
                           key, index);
        }
    }

    // Writes one validated PNG to ServerAvatarOverrides
    static bool SetAvatarOverride(INetworkStringTable *table, uint64_t steamId,
                                  const std::vector<unsigned char> &bytes,
                                  int &index, char *error, size_t errorLength)
    {
        if (!table || steamId == 0 || bytes.empty())
        {
            std::snprintf(error, errorLength, "invalid avatar request");
            return false;
        }
        if (table->GetNumStrings() == 0)
        {
            SetStringUserDataRequest_t empty{};
            int sentinel = table->AddString(true, "__bothider_no_avatar__", &empty);
            if (sentinel != 0)
            {
                std::snprintf(error, errorLength,
                              "failed to reserve string-table index 0");
                return false;
            }
        }

        const StringUserData_t *fallback = table->GetStringUserData(0);
        if (fallback && fallback->m_cbDataSize != 0)
        {
            std::snprintf(error, errorLength,
                          "string-table index 0 contains avatar data");
            return false;
        }

        char key[32];
        FormatAvatarSteamId(steamId, key, sizeof(key));
        SetStringUserDataRequest_t userData{};
        userData.m_pRawData = const_cast<unsigned char *>(bytes.data());
        userData.m_cbDataSize = static_cast<unsigned int>(bytes.size());

        index = table->FindStringIndex(key);
        if (index == 0)
        {
            std::snprintf(error, errorLength,
                          "refusing to use reserved string-table index 0");
            return false;
        }
        if (index < 0)
        {
            index = table->AddString(true, key, &userData);
            if (index <= 0)
            {
                std::snprintf(error, errorLength,
                              "failed to allocate a string-table entry");
                return false;
            }
            return true;
        }
        if (!table->SetStringUserData(index, &userData, true))
        {
            std::snprintf(error, errorLength,
                          "failed to update string-table user data");
            return false;
        }
        return true;
    }

    // Enables reliable server avatar data before writing overrides
    static bool EnsureReliableAvatarData()
    {
        ConVarRefAbstract reliableAvatarData("sv_reliableavatardata");
        if (!reliableAvatarData.IsValidRef() ||
            !reliableAvatarData.IsConVarDataAvailable())
        {
            META_CONPRINTF("[BOTHIDER] avatar error: sv_reliableavatardata unavailable\n");
            return false;
        }
        if (!reliableAvatarData.GetBool())
        {
            reliableAvatarData.SetBool(true);
            if (!reliableAvatarData.GetBool())
            {
                META_CONPRINTF("[BOTHIDER] avatar error: failed to enable sv_reliableavatardata\n");
                return false;
            }
        }
        return true;
    }

    // Sets the network string-table interface used for avatar overrides
    void SetStringTableContainer(INetworkStringTableContainer *container)
    {
        g_pNetworkStringTables = container;
    }

    // Resets all per-slot avatar bookkeeping and published states
    void ResetRuntime()
    {
        g_AvatarStates.fill(AvatarRuntimeState{});
        g_pLastAvatarTable = nullptr;
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
            Publisher().PublishAvatarState(slot, false, 0);
    }

    // Applies pending shared-memory avatar requests on the game thread
    void ProcessOverrides()
    {
        if (!g_pNetworkStringTables)
            return;
        INetworkStringTable *table =
            g_pNetworkStringTables->FindTable("ServerAvatarOverrides");
        if (!table)
            return;

        if (table != g_pLastAvatarTable)
        {
            g_AvatarStates.fill(AvatarRuntimeState{});
            g_pLastAvatarTable = table;
            for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
                Publisher().PublishAvatarState(slot, false, 0);
        }

        static constexpr unsigned char kPngSignature[8] =
            {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        for (int slot = 0; slot < PersonaPool::kMaxSlots; ++slot)
        {
            uint32_t sequence = 0;
            uint32_t length = 0;
            uint64_t incarnation = 0;
            if (!Publisher().ReadAvatarMetadata(slot, sequence, length, incarnation))
                continue;

            AvatarRuntimeState &state = g_AvatarStates[slot];
            bool currentRequest = Manager().IsManaged(slot) &&
                                  length > 0 &&
                                  incarnation != 0 &&
                                  incarnation == Publisher().GetIncarnation(slot);
            uint64_t steamId = currentRequest ? Manager().GetSyntheticSid(slot) : 0;
            if (!currentRequest || steamId == 0)
            {
                if (!state.Applied &&
                    state.ProcessedSequence == sequence &&
                    state.ProcessedSteamId == 0)
                {
                    continue;
                }
                if (state.Applied)
                    ClearAvatarOverride(table, state.AppliedSteamId);
                state.ProcessedSequence = sequence;
                state.ProcessedSteamId = 0;
                state.AppliedSteamId = 0;
                state.Applied = false;
                Publisher().PublishAvatarState(slot, false, 0);
                continue;
            }

            if (state.ProcessedSequence == sequence &&
                state.ProcessedSteamId == steamId)
            {
                continue;
            }

            SlotPublisher::AvatarRequest request;
            if (!Publisher().ReadAvatarRequest(slot, request) ||
                request.Sequence != sequence || request.Length != length ||
                request.Incarnation != incarnation)
            {
                continue;
            }

            if (state.Applied)
                ClearAvatarOverride(table, state.AppliedSteamId);
            state.ProcessedSequence = request.Sequence;
            state.ProcessedSteamId = steamId;
            state.AppliedSteamId = 0;
            state.Applied = false;
            Publisher().PublishAvatarState(slot, false, 0);

            if (request.Data.size() < sizeof(kPngSignature) ||
                std::memcmp(request.Data.data(), kPngSignature,
                            sizeof(kPngSignature)) != 0)
            {
                META_CONPRINTF("[BOTHIDER] avatar rejected slot=%d: invalid PNG signature\n",
                               slot);
                continue;
            }
            if (!EnsureReliableAvatarData())
                continue;

            int index = -1;
            char avatarError[128] = {0};
            if (!SetAvatarOverride(table, steamId, request.Data, index,
                                   avatarError, sizeof(avatarError)))
            {
                META_CONPRINTF("[BOTHIDER] avatar rejected slot=%d sid=%llu: %s\n",
                               slot, static_cast<unsigned long long>(steamId), avatarError);
                continue;
            }

            state.AppliedSteamId = steamId;
            state.Applied = true;
            Publisher().PublishAvatarState(slot, true, steamId);
            META_CONPRINTF("[BOTHIDER] avatar applied slot=%d sid=%llu bytes=%u index=%d\n",
                           slot, static_cast<unsigned long long>(steamId),
                           request.Length, index);
        }
    }

} // namespace cs2bh::avatar
