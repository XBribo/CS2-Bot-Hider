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
            BH_LOG_WARN("config.json parse error; using defaults\n");
        }
        else if (config.is_object())
        {
            if (config.contains("auto_respawn") && config["auto_respawn"].is_boolean())
                settings.autoRespawn = config["auto_respawn"].get<bool>();
            if (config.contains("external_avatars") && config["external_avatars"].is_boolean())
                settings.externalAvatars = config["external_avatars"].get<bool>();
            if (config.contains("identity_mode") && config["identity_mode"].is_string())
            {
                const std::string mode = config["identity_mode"].get<std::string>();
                if (mode == "bot") settings.botMode = true;
                else if (mode != "player")
                    BH_LOG_WARN("unsupported identity_mode='%s'; using player\n", mode.c_str());
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
                    BH_LOG_WARN("invalid fake_ping range %d-%d; using 20-90\n", minimum, maximum);
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
                             "    \"auto_respawn\": false,\n"
                             "    \"external_avatars\": false,\n"
                             "    \"fake_ping\": {\n"
                             "        \"enabled\": true,\n"
                             "        \"min\": 20,\n"
                             "        \"max\": 90\n"
                             "    }\n"
                             "}\n";
        }
        else
        {
            BH_LOG_WARN("config.json missing and could not be created; using defaults\n");
        }
    }

    return settings;
}
} // namespace cs2bh::config
