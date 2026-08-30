// plugin.h
//
// Metamod:Source plugin entry

#pragma once

#include <ISmmPlugin.h>
#include <playerslot.h>
#include <tier1/utlvector.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class CServerSideClient;
class INetworkGameClient;
class CCSPlayerController;
class ConCommandRef;
class CCommandContext;
class CCommand;
enum ENetworkDisconnectionReason : int;

namespace cs2bh {

enum class IdentityMode : uint8_t
{
    Player = 0,
    Bot = 1,
};

class HiderPlugin : public ISmmPlugin, public IMetamodListener
{
  public:
    // ISmmPlugin
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;

    const char* GetAuthor() override { return "XBribo(๑•.•๑)"; }
    const char* GetName() override { return "CS2-Bot-Hider"; }
    const char* GetDescription() override { return "Bot persona/steamid/ping/crosshair/avatar hider"; }
    const char* GetURL() override { return ""; }
    const char* GetLicense() override { return "AGPL-3.0"; }
    const char* GetVersion() override { return "0.4.0"; }
    const char* GetDate() override { return __DATE__; }
    const char* GetLogTag() override { return "BH"; }

    // IMetamodListener
    void OnLevelInit(char const* pMapName, char const*, char const*, char const*, bool, bool) override;
    void OnLevelShutdown() override;

    // Hook entry points
    void HookOnClientConnectedPost(
        CPlayerSlot slot, const char* pszName, uint64 xuid, const char* pszNetworkID, const char* pszAddress, bool bFakePlayer);
    void HookClientPutInServerPost(CPlayerSlot slot, char const* pszName, int type, uint64 xuid);
    void HookClientDisconnectPre(
        CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID);
    CUtlVector<INetworkGameClient*>* HookStartChangeLevelPre(const char* mapName, const char* landmark, void* changelevelState);
    void HookGameFramePost(bool simulating, bool bFirstTick, bool bLastTick);

    // ICvar::DispatchConCommand — wrap Valve population commands in one identity transaction
    void HookDispatchConCommandPre(ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args);
    void HookDispatchConCommandPost(ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args);

    // Changes the global managed-bot identity mode
    void SetIdentityMode(IdentityMode mode);
    bool IsDisguiseEnabled() const { return m_identityMode == IdentityMode::Player; }
    bool IsBotMode() const { return m_identityMode == IdentityMode::Bot; }

    // Native identity hooks must not touch level-owned objects while the engine
    // is shutting down or changing maps.
    void SetLifecycleActive();
    // Claims the one teardown restore window. Callers that lose the claim wait
    // for the restore to finish and must not touch level-owned entities.
    bool BeginLifecycleTeardown();
    void EndLifecycleTeardown();
    bool IsLifecycleActive() const
    {
        return m_LifecycleState.load(std::memory_order_acquire) == LifecycleState::Active;
    }
    bool IsLifecycleRestorationActive() const
    {
        return m_TeardownRestoreActive.load(std::memory_order_acquire);
    }

    // Toggle the display-name source: true=bot_info.json name, false=botprofile name
    void SetUseBotInfoName(bool useBotInfo) { m_useBotInfoName = useBotInfo; }

  private:
    enum class LifecycleState : uint8_t
    {
        Inactive,
        Active,
        Teardown,
    };

    void* m_hookedGameServer = nullptr;
    int m_startChangeLevelHookId = 0;
    bool m_selfDisabled = false;
    std::atomic<LifecycleState> m_LifecycleState{LifecycleState::Inactive};
    std::atomic_bool m_TeardownRestoreActive{false};
    mutable std::mutex m_LifecycleMutex;
    std::condition_variable m_LifecycleCv;
    unsigned int m_tickCounter = 0; // throttles per-tick idle-timer reset
    IdentityMode m_identityMode = IdentityMode::Player;
    bool m_fakePingEnabled = true;
    int m_fakePingMin = 20;
    int m_fakePingMax = 90;
    unsigned int m_populationCommandDepth = 0;

    // Display-name source: false=botprofile name, true=bot_info.json name
    bool m_useBotInfoName = false;
};

extern HiderPlugin g_plugin;

} // namespace cs2bh

PLUGIN_GLOBALVARS();
