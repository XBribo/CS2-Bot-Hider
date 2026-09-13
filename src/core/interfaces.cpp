#include "core/interfaces.h"
#include "plugin.h"
#include <ISmmAPI.h>
#include <icvar.h>
#include <eiface.h>
#include <iserver.h>
#include <interfaces/interfaces.h>
IVEngineServer* g_engine = nullptr;
ICvar* g_icvar = nullptr;
IServerGameClients* g_gameclients = nullptr;
IServerGameDLL* g_server = nullptr;
namespace cs2bh::interfaces {
// Resolves required engine and server interfaces through Metamod.
bool Init(ISmmAPI* ismm, char* error, size_t maxlen)
{
    GET_V_IFACE_CURRENT(GetEngineFactory, g_engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_icvar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
    GET_V_IFACE_ANY(GetServerFactory, g_server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
    GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

    return true;
}
} // namespace cs2bh::interfaces
