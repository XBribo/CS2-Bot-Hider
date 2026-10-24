#include "core/gamedata.h"
#include "core/cs2_sdk/src/core/gameconfig.h"
#include "core/log.h"
#include "entity_access.h"
#include "identity_hooks.h"
#include "offsets.h"
#include "core/cs2_sdk/src/core/memory_module.h"
#include "core/cs2_sdk/src/utils/platform.h"
#include <filesystem>
#include <string>
namespace cs2bh::gamedata {
std::string ComputePath()
{
    std::filesystem::path path(cs2_sdk::platform::SelfModulePath());
    if (path.empty()) return "";
    for (int i = 0; i < 3; ++i)
    {
        path = path.parent_path();
        if (path.empty()) return "";
    }
    return (path / "gamedata.json").string();
}

void Prepare()
{
    const std::string gamedataPath = ComputePath();
    nlohmann::json gamedata;
    if (!cs2_sdk::gameconfig::LoadGamedata(gamedataPath.c_str(), gamedata))
    {
        BH_LOG_WARN("gamedata unavailable path='%s': identity detours and cleanup disabled", gamedataPath.c_str());
    }
    else
    {
        offsets::LoadFromGamedata(gamedata);
        if (offsets::g_vtableSlotClientSetName < 0)
        {
            BH_LOG_WARN("name writes disabled: SetName vtable slot missing");
        }

        cs2_sdk::modules::Initialize();
        const cs2_sdk::modules::ModuleInfo& serverModule = cs2_sdk::modules::server->Code();
        entity_access::ResolveUtilRemoveAndEntSys(gamedata, serverModule);
        identity_hooks::PrepareAll(gamedata, serverModule);
    }
}
} // namespace cs2bh::gamedata
