#pragma once
#include <cstdint>
class INetworkStringTableContainer;
namespace cs2bh::avatar {
void InitPublisher(INetworkStringTableContainer* server);
bool ShutdownPublisher();
void ResetPublications();
// owner 0 is the direct SteamID API; nonzero owners are native slot incarnations.
int Publish(uint64_t steamId, const unsigned char* png, int length, uint64_t owner = 0);
int Clear(uint64_t steamId, uint64_t owner = 0);
void ClearAll(bool directOnly = true);
const char* Status();
} // namespace cs2bh::avatar
