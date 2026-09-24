#pragma once
namespace cs2bh::config {
struct Settings
{
    bool botMode = false;
    bool autoRespawn = false;
    bool externalAvatars = false;
    bool fakePingEnabled = true;
    int fakePingMin = 20;
    int fakePingMax = 90;
};
inline Settings Current;
// Reads startup configuration and creates defaults on first install.
Settings Load(const char* baseDir);
} // namespace cs2bh::config
