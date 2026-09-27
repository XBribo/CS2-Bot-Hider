// clan_tag.h
//
// Holds an optional configured clan presentation pair.

#pragma once

#include <cstdint>
#include <string>

namespace cs2bh {

struct ClanTag
{
    std::string tag;
    uint32_t groupId = 0;
};

} // namespace cs2bh
