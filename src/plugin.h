// plugin.h
//
// Metamod:Source plugin entry

#pragma once

#include <ISmmPlugin.h>
#include <khook.hpp>
#include <playerslot.h>
#include <tier1/utlvector.h>
#include "hooks.h"
#include <array>
#include <cstdint>

class CServerSideClient;
class INetworkGameClient;
class INetworkGameServer;
class ICvar;
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
    HiderPlugin();
    ~HiderPlugin() override;

    // ISmmPlugin
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;

    const char* GetAuthor() override { return "XBribo(๑•.•๑)"; }
    const char* GetName() override { return "CS2-Bot-Hider"; }
    const char* GetDescription() override { return "Bot persona/steamid/ping/crosshair/avatar hider"; }
    const char* GetURL() override { return ""; }
    const char* GetLicense() override { return "AGPL-3.0"; }
    const char* GetVersion() override { return "0.5.0"; }
    const char* GetDate() override { return __DATE__; }
    const char* GetLogTag() override { return "BH"; }

    // IMetamodListener
    void OnLevelInit(char const* mapName, char const*, char const*, char const*, bool, bool) override;
    void OnLevelShutdown() override;

    // Hook entry points
    KHook::Return<void> HookOnClientConnectedPost(IServerGameClients*,
                                                  CPlayerSlot slot,
                                                  const char* name,
                                                  uint64 xuid,
                                                  const char* networkId,
                                                  const char* address,
                                                  bool fakePlayer) noexcept;
    KHook::Return<void> HookClientPutInServerPost(IServerGameClients*, CPlayerSlot slot, char const* name, int type, uint64 xuid) noexcept;
    KHook::Return<void> HookClientDisconnectPre(IServerGameClients*,
                                                CPlayerSlot slot,
                                                ENetworkDisconnectionReason reason,
                                                const char* name,
                                                uint64 xuid,
                                                const char* networkId) noexcept;
    KHook::Return<CUtlVector<INetworkGameClient*>*>
    HookStartChangeLevelPre(INetworkGameServer*, const char* mapName, const char* landmark, void* changelevelState) noexcept;
    KHook::Return<void> HookGameFramePost(IServerGameDLL*, bool simulating, bool firstTick, bool lastTick) noexcept;

    // ICvar::DispatchConCommand — wrap Valve population commands in one identity transaction
    KHook::Return<void>
    HookDispatchConCommandPre(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept;
    KHook::Return<void>
    HookDispatchConCommandPost(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept;

    // Changes the global managed-bot identity mode
    void SetIdentityMode(IdentityMode mode);
    bool IsDisguiseEnabled() const { return m_identityMode == IdentityMode::Player; }
    bool IsBotMode() const { return m_identityMode == IdentityMode::Bot; }

    // Toggle the display-name source: true=bot_info.json name, false=botprofile name
    void SetUseBotInfoName(bool useBotInfo) { m_useBotInfoName = useBotInfo; }

  private:
    using OnClientConnectedHook =
        hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, const char*, uint64, const char*, const char*, bool>;
    using ClientPutInServerHook = hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, char const*, int, uint64>;
    using ClientDisconnectHook =
        hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*>;
    using StartChangeLevelHook =
        hooks::CheckedVirtual<INetworkGameServer, CUtlVector<INetworkGameClient*>*, const char*, const char*, void*>;
    using GameFrameHook = hooks::CheckedVirtual<IServerGameDLL, void, bool, bool, bool>;
    using DispatchConCommandHook = hooks::CheckedVirtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&>;

    bool InstallVirtualHooks();
    bool RemoveVirtualHooks();
    // Queues the deferred Metamod unload after the command hook has detached.
    void ProcessPendingUnload();

    INetworkGameServer* m_hookedGameServer = nullptr;
    OnClientConnectedHook m_onClientConnectedHook;
    ClientPutInServerHook m_clientPutInServerHook;
    ClientDisconnectHook m_clientDisconnectHook;
    StartChangeLevelHook m_startChangeLevelHook;
    GameFrameHook m_gameFrameHook;
    DispatchConCommandHook m_dispatchConCommandHook;
    bool m_selfDisabled = false;
    bool m_unloadPending = false;
    unsigned int m_tickCounter = 0; // throttles per-tick idle-timer reset
    IdentityMode m_identityMode = IdentityMode::Player;
    bool m_fakePingEnabled = true;
    int m_fakePingMin = 20;
    int m_fakePingMax = 90;

    // Display-name source: false=botprofile name, true=bot_info.json name
    bool m_useBotInfoName = false;
};

extern HiderPlugin g_plugin;

} // namespace cs2bh

PLUGIN_GLOBALVARS();
