#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

namespace cs2bh::HudPovPresentation
{
    // Optional local-client hook. client.dll is absent on dedicated servers.
    bool Install(const nlohmann::json &gamedata, char *errorOut, size_t errorOutLen);
    void Remove();
    const char *Status();
} // namespace cs2bh::HudPovPresentation
