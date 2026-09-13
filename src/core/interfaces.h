#pragma once
#include <cstddef>
#include <ISmmAPI.h>
#include <eiface.h>
class ICvar;
class INetworkServerService;
extern IVEngineServer* g_engine;
extern ICvar* g_icvar;
extern IServerGameClients* g_gameclients;
extern IServerGameDLL* g_server;
extern INetworkServerService* g_pNetworkServerService;
namespace cs2bh::interfaces {
// Acquires the interfaces required before hook preparation.
bool Init(SourceMM::ISmmAPI* ismm, char* error, size_t maxlen);
} // namespace cs2bh::interfaces
