#include "core/config.h"
#include "core/log.h"
#include <fstream>
#include <iterator>
#include <string>
#include <nlohmann/json.hpp>
namespace cs2bh::config {
// Parses identity and fake-ping settings without changing their defaults.
Settings Load(const char* baseDir)
{
    Settings settings;
    std::string configPath = baseDir;
    configPath += "/addons/BotHider/config.json";
    std::ifstream configFile(configPath, std::ios::binary);
    if (configFile.is_open())
    {
        const std::string configText((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
        const nlohmann::json config = nlohmann::json::parse(configText, nullptr, false);
        if (config.is_discarded())
        {
            BH_LOG_WARN("config.json invalid: using defaults");
        }
        else if (config.is_object())
        {
            if (config.contains("identity_mode") && config["identity_mode"].is_string())
            {
                const std::string mode = config["identity_mode"].get<std::string>();
                if (mode == "bot") settings.botMode = true;
                else if (mode != "player")
                    BH_LOG_WARN("identity_mode='%s' invalid: using player", mode.c_str());
            }

            if (config.contains("fake_ping") && config["fake_ping"].is_object())
            {
                const auto& fakePing = config["fake_ping"];
                if (fakePing.contains("enabled") && fakePing["enabled"].is_boolean())
                    settings.fakePingEnabled = fakePing["enabled"].get<bool>();

                int minimum = settings.fakePingMin;
                int maximum = settings.fakePingMax;
                if (fakePing.contains("min") && fakePing["min"].is_number_integer()) minimum = fakePing["min"].get<int>();
                if (fakePing.contains("max") && fakePing["max"].is_number_integer()) maximum = fakePing["max"].get<int>();
                if (minimum >= 1 && maximum <= 999 && minimum <= maximum)
                {
                    settings.fakePingMin = minimum;
                    settings.fakePingMax = maximum;
                }
                else
                {
                    BH_LOG_WARN("ping range=%d-%d invalid: using 20-90", minimum, maximum);
                }
            }
        }
    }
    else
    {
        // Creates the documented defaults on first install
        std::ofstream defaultConfig(configPath, std::ios::trunc);
        if (defaultConfig.is_open())
        {
            defaultConfig << "{\n"
                             "    \"identity_mode\": \"player\",\n"
                             "    \"commands\": {\n"
                             "        \"require_permissions\": false,\n"
                             "        \"permission\": \"@bothider/admin\"\n"
                             "    },\n"
                             "    \"fake_ping\": {\n"
                             "        \"enabled\": true,\n"
                             "        \"min\": 20,\n"
                             "        \"max\": 90\n"
                             "    }\n"
                             "}\n";
        }
        else
        {
            BH_LOG_WARN("config.json creation failed: using defaults");
        }
    }

    return settings;
}
} // namespace cs2bh::config
