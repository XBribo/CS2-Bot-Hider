// plugin.h
//
// Metamod:Source plugin entry

#pragma once

#include <ISmmPlugin.h>
#include <playerslot.h>
#include <tier1/utlvector.h>
#include <array>

class CServerSideClient;
class INetworkGameClient;
class CCSPlayerController;
class ConCommandRef;
class CCommandContext;
class CCommand;
enum ENetworkDisconnectionReason : int;

namespace cs2bh
{

    class HiderPlugin : public ISmmPlugin, public IMetamodListener
    {
    public:
        // ISmmPlugin
        bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
        bool Unload(char *error, size_t maxlen) override;

        const char *GetAuthor() override { return "XBribo(๑•.•๑)"; }
        const char *GetName() override { return "CS2-Bot-Hider"; }
        const char *GetDescription() override { return "Bot persona/steamid/ping hider"; }
        const char *GetURL() override { return ""; }
        const char *GetLicense() override { return "GPLv3"; }
        const char *GetVersion() override { return "0.2.3"; }
        const char *GetDate() override { return __DATE__; }
        const char *GetLogTag() override { return "BOTHIDER"; }

        // IMetamodListener
        void OnLevelInit(char const *pMapName, char const *, char const *, char const *, bool, bool) override;
        void OnLevelShutdown() override;

        // Hook entry points
        void Hook_OnClientConnected_Post(CPlayerSlot slot, const char *pszName, uint64 xuid,
                                         const char *pszNetworkID, const char *pszAddress,
                                         bool bFakePlayer);
        void Hook_ClientPutInServer_Post(CPlayerSlot slot, char const *pszName, int type, uint64 xuid);
        void Hook_ClientDisconnect_Pre(CPlayerSlot slot, ENetworkDisconnectionReason reason,
                                       const char *pszName, uint64 xuid, const char *pszNetworkID);
        CPlayerSlot Hook_CreateFakeClient_Pre(const char *netname);
        CUtlVector<INetworkGameClient *> *Hook_StartChangeLevel_Pre(
            const char *mapName, const char *landmark, void *changelevelState);
        void Hook_GameFrame_Post(bool simulating, bool bFirstTick, bool bLastTick);

        // ICvar::DispatchConCommand  — restore bot identity before the engine and processes a kick
        void Hook_DispatchConCommand_Pre(ConCommandRef cmd, const CCommandContext &ctx,
                                         const CCommand &args);
        void Hook_DispatchConCommand_Post(ConCommandRef cmd, const CCommandContext &ctx,
                                          const CCommand &args);

        // CUtlString::Set resolved from tier0.dll at Load
        using CUtlStringSetFn = void (*)(void * /*CUtlString this*/, const char *);
        CUtlStringSetFn m_pUtlStringSet = nullptr;

        // Toggle disguise globally
        void SetDisguiseEnabled(bool enabled);

        void RebuildBots();

        // Toggle the display-name source: true=bot_info.json name, false=botprofile name
        void SetUseBotInfoName(bool useBotInfo) { m_bUseBotInfoName = useBotInfo; }

    private:
        void *m_pHookedGameServer = nullptr;
        int m_StartChangeLevelHookId = 0;
        bool m_bSelfDisabled = false;
        unsigned int m_TickCounter = 0; // throttles per-tick idle-timer reset
        // Master disguise switch
        bool m_bDisguiseEnabled = true;

        bool m_bRebuilding = false;

        // bot_quota captured in DispatchConCommand PRE for a bot_add, so POST can
        // set it to old+1 (bot_add's own census miscomputes the target when disguised)
        int m_QuotaBeforeAdd = 0;

        // Captured before a regular kick so POST can shrink bot_quota when managed bots were removed.
        int m_ManagedBeforeKick = 0;
        int m_QuotaBeforeKick = -1;
        bool m_AdjustQuotaAfterKick = false;

        // Display-name source: false=botprofile name, true=bot_info.json name
        bool m_bUseBotInfoName = false;
    };

    extern HiderPlugin g_Plugin;

} // namespace cs2bh

PLUGIN_GLOBALVARS();
