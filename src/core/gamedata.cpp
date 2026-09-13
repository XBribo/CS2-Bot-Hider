#include "core/gamedata.h"
#include "core/interfaces.h"
#include "core/log.h"
#include "entity_access.h"
#include "identity_hooks.h"
#include "version_targets.h"
#include "sig_scan.h"
#include <string>
namespace cs2bh::gamedata {
// Resolves the existing gamedata entries and preserves optional-feature failures.
void Prepare(const char* baseDir)
{
    // Resolve UTIL_Remove
    // Required to destroy controllers on kick
    {
        std::string gdPath = baseDir;
        gdPath += "/addons/BotHider/gamedata.json";
        nlohmann::json gamedata;
        if (!sig::LoadGamedata(gdPath.c_str(), gamedata))
        {
            BH_LOG_WARN("[BOTHIDER] warning: gamedata.json not loaded at '%s' — "
                        "controller cleanup disabled\n",
                        gdPath.c_str());
        }
        else
        {
            // Override member offsets from gamedata.json (fallback kept if absent)
            entity_access::LoadMemberOffsets(gamedata);
            if (targets::g_vtableSlotClientSetName < 0)
            {
                BH_LOG_WARN("[BOTHIDER] warning: CServerSideClient::SetName vtable slot missing - "
                            "name overwrite disabled\n");
            }

            sig::ModuleInfo serverModule = sig::ModuleFromInterfacePtr(g_gameclients);
            if (!serverModule) serverModule = sig::ModuleFromName(targets::kServerModuleName);
            entity_access::ResolveUtilRemoveAndEntSys(gamedata, serverModule);

            identity_hooks::PrepareAll(gamedata, serverModule);
        }
    }
    if (!entity_access::UtilRemoveTarget())
    {
        BH_LOG_WARN("[BOTHIDER] warning: UTIL_Remove signature unresolved — "
                    "controller cleanup disabled\n");
    }
}
} // namespace cs2bh::gamedata
