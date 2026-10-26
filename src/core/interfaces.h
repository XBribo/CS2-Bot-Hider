#pragma once
#include <cstddef>
#include <ISmmAPI.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
class ICvar;
class INetworkServerService;
class ISchemaSystem;
extern IVEngineServer* g_engine;
extern ICvar* g_icvar;
extern IServerGameClients* g_gameclients;
extern IServerGameDLL* g_server;
extern INetworkServerService* g_pNetworkServerService;
extern ISchemaSystem* g_schemaSystem;
namespace cs2bh::interfaces {
// Acquire interfaces for hook preparation.
bool Init(SourceMM::ISmmAPI* ismm, char* error, size_t maxlen);
} // namespace cs2bh::interfaces
