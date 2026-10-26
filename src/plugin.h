// Metamod plugin entry.

#pragma once

#include <ISmmPlugin.h>
#include <engine2/isource2server.h>
#include <khook.hpp>
#include <playerslot.h>
#include "hooks.h"
#include <array>
#include <cstdint>

class CServerSideClient;
class CServerChangelevelState;
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

    // Plugin author.
    const char* GetAuthor() override;
    // Plugin name.
    const char* GetName() override;
    // Plugin description.
    const char* GetDescription() override;
    // Project URL.
    const char* GetURL() override;
    // Plugin license.
    const char* GetLicense() override;
    // Build version.
    const char* GetVersion() override;
    // Build timestamp.
    const char* GetDate() override;
    // Log tag.
    const char* GetLogTag() override;

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
    KHook::Return<CServerChangelevelState*>
    HookStartChangeLevelPre(INetworkGameServer*, const char* mapName, const char* landmark, void* changelevelState) noexcept;
    KHook::Return<void> HookGameFramePost(IServerGameDLL*, bool simulating, bool firstTick, bool lastTick) noexcept;

    // Wrap Valve population commands in an identity transaction.
    KHook::Return<void>
    HookDispatchConCommandPre(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept;
    KHook::Return<void>
    HookDispatchConCommandPost(ICvar*, ConCommandRef command, const CCommandContext&, const CCommand& arguments) noexcept;

    // Set the global identity mode.
    void SetIdentityMode(IdentityMode mode);
    bool IsDisguiseEnabled() const { return m_identityMode == IdentityMode::Player; }
    bool IsBotMode() const { return m_identityMode == IdentityMode::Bot; }

    // Name source: true=bot_info.json, false=botprofile.
    void SetUseBotInfoName(bool useBotInfo) { m_useBotInfoName = useBotInfo; }

  private:
    using OnClientConnectedHook =
        hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, const char*, uint64, const char*, const char*, bool>;
    using ClientPutInServerHook = hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, char const*, int, uint64>;
    using ClientDisconnectHook =
        hooks::CheckedVirtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*>;
    using StartChangeLevelHook =
        hooks::CheckedVirtual<INetworkGameServer, CServerChangelevelState*, const char*, const char*, void*>;
    using GameFrameHook = hooks::CheckedVirtual<IServerGameDLL, void, bool, bool, bool>;
    using DispatchConCommandHook = hooks::CheckedVirtual<ICvar, void, ConCommandRef, const CCommandContext&, const CCommand&>;

    bool InstallVirtualHooks();
    bool RemoveVirtualHooks();
    // Queue unload after the command hook detaches.
    void ProcessPendingUnload();
    // Dispatch shared-memory commands.
    void DrainBridgeCommands();
    // Apply a collision-free SteamID.
    void ApplySteamIdCommand(int slot, uint64_t steamId);
    // Update and publish a managed client's name.
    void ApplyPersonaNameCommand(int slot, const char* name);

    INetworkGameServer* m_hookedGameServer = nullptr;
    OnClientConnectedHook m_onClientConnectedHook;
    ClientPutInServerHook m_clientPutInServerHook;
    ClientDisconnectHook m_clientDisconnectHook;
    StartChangeLevelHook m_startChangeLevelHook;
    GameFrameHook m_gameFrameHook;
    DispatchConCommandHook m_dispatchConCommandHook;
    bool m_unloadPending = false;
    unsigned int m_tickCounter = 0; // throttles per-tick idle-timer reset
    IdentityMode m_identityMode = IdentityMode::Player;

    // Name source: false=botprofile, true=bot_info.json.
    bool m_useBotInfoName = false;
};

extern HiderPlugin g_plugin;

} // namespace cs2bh

PLUGIN_GLOBALVARS();
