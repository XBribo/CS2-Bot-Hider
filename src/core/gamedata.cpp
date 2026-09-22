#include "core/gameconfig.h"
#include "core/gamedata.h"
#include "core/log.h"
#include "entity_access.h"
#include "identity_hooks.h"
#include "offsets.h"
#include "core/memory_module.h"
#include "utils/platform.h"
#include <filesystem>
#include <string>
namespace cs2bh::gamedata {
namespace {
// Resolves gamedata beside bin/, relative to this plugin module.
std::string ComputeGamedataPath()
{
    std::filesystem::path path(platform::SelfModulePath());
    if (path.empty()) return "";
    for (int i = 0; i < 3; ++i)
    {
        path = path.parent_path();
        if (path.empty()) return "";
    }
    return (path / "gamedata.json").string();
}
} // namespace

// Resolves the existing gamedata entries and preserves optional-feature failures.
void Prepare()
{
    // Resolve UTIL_Remove
    // Required to destroy controllers on kick
    {
        const std::string gdPath = ComputeGamedataPath();
        nlohmann::json gamedata;
        if (!gameconfig::LoadGamedata(gdPath.c_str(), gamedata))
        {
            BH_LOG_WARN("gamedata.json not loaded at '%s' — "
                        "controller cleanup disabled\n",
                        gdPath.c_str());
        }
        else
        {
            // Override member offsets from gamedata.json (fallback kept if absent)
            offsets::LoadFromGamedata(gamedata);
            if (offsets::g_vtableSlotClientSetName < 0)
            {
                BH_LOG_WARN("CServerSideClient::SetName vtable slot missing - "
                            "name overwrite disabled\n");
            }

            modules::Initialize();
            const modules::ModuleInfo& serverModule = modules::server->Image();
            entity_access::ResolveUtilRemoveAndEntSys(gamedata, serverModule);

            identity_hooks::PrepareAll(gamedata, serverModule);
        }
    }
    if (!entity_access::UtilRemoveTarget())
    {
        BH_LOG_WARN("UTIL_Remove signature unresolved — "
                    "controller cleanup disabled\n");
    }
}
} // namespace cs2bh::gamedata
