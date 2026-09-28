#include <tier0/platform.h>
#include "avatar_publisher.h"
#include "avatar_publication.h"

#include <networkstringtabledefs.h>
#include <convar.h>
#include <mutex>
#include <string>
#include <optional>
#include <cstring>

namespace cs2bh::avatar {
namespace {
constexpr const char* kTableName = "ServerAvatarOverrides";
constexpr size_t kMaxPng = 16 * 1024;
constexpr unsigned char kPng[] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

INetworkStringTableContainer* serverTables = nullptr;
std::recursive_mutex publicationMutex;
Publications publications;

// Reads the current server-side value so it can be restored on release.
bool ReadBytes(INetworkStringTable* table, const char* key, Bytes& bytes)
{
    bytes.clear();
    const int index = table->FindStringIndex(key);
    if (index < 0) return true;
    const auto* data = table->GetStringUserData(index);
    if (!data || data->m_cbDataSize == 0) return true;
    if (!data->m_pRawData || data->m_cbDataSize > kMaxPng) return false;
    const auto* start = static_cast<const unsigned char*>(data->m_pRawData);
    bytes.assign(start, start + data->m_cbDataSize);
    return true;
}

// Writes and verifies one server avatar table entry.
bool WriteBytes(INetworkStringTable* table, const char* key, const Bytes& bytes)
{
    if (table->GetNumStrings() == 0)
    {
        SetStringUserDataRequest_t empty{};
        if (table->AddString(true, "__bothider_no_avatar__", &empty) != 0) return false;
    }
    // Unknown SteamIDs can fall back to index zero; keep that entry empty.
    const auto* fallback = table->GetStringUserData(0);
    if (fallback && fallback->m_cbDataSize != 0) return false;
    int index = table->FindStringIndex(key);
    if (index == 0) return false;
    if (index < 0 && bytes.empty()) return true;
    SetStringUserDataRequest_t data{ const_cast<unsigned char*>(bytes.data()), static_cast<unsigned int>(bytes.size()) };
    if (index < 0) index = table->AddString(true, key, &data);
    else table->SetStringUserData(index, &data, false);
    Bytes actual;
    return index > 0 && ReadBytes(table, key, actual) && SameBytes(actual, bytes);
}
} // namespace

// Retains only the server string-table interface; no client module is inspected.
void InitPublisher(INetworkStringTableContainer* server)
{
    std::lock_guard lock(publicationMutex);
    serverTables = server;
    publications.entries.clear();
}

// Publishes a PNG under one SteamID without overwriting another owner's entry.
int Publish(uint64_t id, const unsigned char* png, int length, uint64_t owner)
{
    if (!ThreadInMainThread()) return -8;
    if (!id || !png || length < 8 || length > static_cast<int>(kMaxPng) || std::memcmp(png, kPng, 8) != 0) return -1;
    std::lock_guard lock(publicationMutex);
    auto* table = serverTables ? serverTables->FindTable(kTableName) : nullptr;
    if (!table) return -2;
    ConVarRefAbstract reliable("sv_reliableavatardata");
    if (!reliable.IsValidRef() || !reliable.IsConVarDataAvailable()) return -6;
    const auto key = std::to_string(id);
    Bytes previous;
    if (!ReadBytes(table, key.c_str(), previous)) return -3;
    const Bytes desired(png, png + length);
    std::optional<Publication> rollback;
    if (auto it = publications.entries.find(id); it != publications.entries.end()) rollback = it->second;
    if (!rollback && publications.entries.size() >= 128) return -4;
    if (!publications.Prepare(id, desired, previous, owner)) return -7;
    if (!SameBytes(previous, desired) && !WriteBytes(table, key.c_str(), desired))
    {
        if (rollback) publications.entries[id] = std::move(*rollback);
        else publications.entries.erase(id);
        return -5;
    }
    if (!reliable.GetBool()) reliable.SetBool(true);
    if (!reliable.GetBool()) return -6;
    return SameBytes(previous, desired) ? 0 : 1;
}

// Restores this publisher's prior value without erasing a later writer's data.
int Clear(uint64_t id, uint64_t owner)
{
    if (!ThreadInMainThread()) return -8;
    std::lock_guard lock(publicationMutex);
    const auto it = publications.entries.find(id);
    if (it == publications.entries.end() || !it->second.owned) return 0;
    if (it->second.owner != owner) return -7;
    const Publication current = it->second;
    auto* table = serverTables ? serverTables->FindTable(kTableName) : nullptr;
    const auto key = std::to_string(id);
    Bytes actual;
    if (table && !ReadBytes(table, key.c_str(), actual)) return -3;
    const Bytes restored = SameBytes(actual, current.desired) ? current.restore : actual;
    if (table && !SameBytes(actual, restored) && !WriteBytes(table, key.c_str(), restored)) return -5;
    publications.entries.erase(id);
    return 1;
}

// Clears direct overrides by default, leaving slot-owned images intact.
void ClearAll(bool directOnly)
{
    std::vector<std::pair<uint64_t, uint64_t>> ids;
    {
        std::lock_guard lock(publicationMutex);
        for (const auto& [id, publication] : publications.entries)
            if (publication.owned && (!directOnly || publication.owner == 0))
                ids.emplace_back(id, publication.owner);
    }
    for (const auto& [id, owner] : ids) Clear(id, owner);
}

// Forgets entries when the game destroys the map's string tables.
void ResetPublications()
{
    std::lock_guard lock(publicationMutex);
    publications.entries.clear();
}

// Restores owned server entries before unloading.
bool ShutdownPublisher()
{
    if (!ThreadInMainThread()) return false;
    ClearAll(false);
    ResetPublications();
    std::lock_guard lock(publicationMutex);
    serverTables = nullptr;
    return true;
}

// Reports server publication state only.
const char* Status()
{
    static thread_local std::string summary;
    std::lock_guard lock(publicationMutex);
    summary = "server publication only; active=" + std::to_string(publications.entries.size());
    return summary.c_str();
}
} // namespace cs2bh::avatar
