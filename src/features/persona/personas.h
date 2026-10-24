// personas.h

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace cs2bh {

class PersonaPool
{
  public:
    PersonaPool();

    std::string PickFromRoster();

    // Mark a slot as managed
    void MarkSlotManaged(int slot, const char* name);
    void ClearSlot(int slot);
    bool IsSlotManaged(int slot) const;
    std::string GetSlotName(int slot) const;

    static constexpr int kMaxSlots = 64;

  private:
    mutable std::mutex m_mutex;
    std::array<std::string, kMaxSlots> m_slotNames;
    std::array<bool, kMaxSlots> m_slotManaged{};
    uint64_t m_rosterRngState = 0;
};

PersonaPool& Personas();

} // namespace cs2bh
