// Evidence-scoped avatar publications. Access is serialized by the caller.
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace cs2bh::avatar {
using Bytes = std::vector<unsigned char>;

struct Publication
{
    Bytes desired;
    Bytes restore;
    bool owned = true;
    uint64_t revision = 0;
    uint64_t owner = 0;
};

inline bool SameBytes(std::span<const unsigned char> a, std::span<const unsigned char> b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

class Publications
{
  public:
    std::unordered_map<uint64_t, Publication> entries;

    bool Prepare(uint64_t id, Bytes desired, Bytes previous, uint64_t owner = 0)
    {
        auto it = entries.find(id);
        if (it != entries.end() && it->second.owned && it->second.owner != owner) return false;
        if (it == entries.end() || !it->second.owned)
        {
            entries[id] = Publication{ std::move(desired), std::move(previous), true, ++revision_ };
            entries[id].owner = owner;
        }
        else if (!SameBytes(it->second.desired, desired))
        {
            it->second.desired = std::move(desired);
            it->second.revision = ++revision_;
        }
        return true;
    }

  private:
    uint64_t revision_ = 0;
};
} // namespace cs2bh::avatar
