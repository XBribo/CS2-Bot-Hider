#pragma once
namespace cs2bh::config {
struct Settings
{
    bool botMode = false;
    bool fakePingEnabled = true;
    int fakePingMin = 20;
    int fakePingMax = 90;
};
// Load startup settings; create defaults on first install.
Settings Load(const char* baseDir);
} // namespace cs2bh::config
