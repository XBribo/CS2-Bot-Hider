#include "core/gamedata.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
#include "entity_access.h"
#include "identity_hooks.h"
#include "offsets.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include <cstdio>
#include <string>
namespace cs2bh::gamedata {
bool Prepare(char* error, size_t maxlen)
{
    const std::string gamedataPath = cs2_sdk::gameconfig::ComputePath();
    nlohmann::json gamedata;
    if (!cs2_sdk::gameconfig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        std::snprintf(error, maxlen, "gamedata load failed: %s", gamedataPath.c_str());
        BH_LOG_ERROR("%s", error);
        return false;
    }
    else
    {
        if (!offsets::LoadFromGamedata(gamedata, error, maxlen))
        {
            BH_LOG_ERROR("%s", error);
            return false;
        }
        if (offsets::g_vtableSlotClientSetName < 0)
        {
            BH_LOG_WARN("name writes disabled: SetName vtable slot missing");
        }

        cs2_sdk::modules::Initialize();
        if (!cs2_sdk::modules::server || !cs2_sdk::modules::server->Code())
        {
            std::snprintf(error, maxlen, "server code unavailable");
            BH_LOG_ERROR("%s", error);
            return false;
        }
        const cs2_sdk::modules::ModuleInfo& serverModule = cs2_sdk::modules::server->Code();
        entity_access::ResolveUtilRemoveAndEntSys(gamedata, serverModule);
        identity_hooks::PrepareAll(gamedata, serverModule);
    }
    return true;
}
} // namespace cs2bh::gamedata
