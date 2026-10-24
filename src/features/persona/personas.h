// personas.h

#pragma once

#include <array>
#include <mutex>
#include <string>

namespace cs2bh {

class PersonaPool
{
  public:
    // Stores a slot's display name.
    void SetSlotName(int slot, const char* name);
    void ClearSlot(int slot);
    std::string GetSlotName(int slot) const;

    static constexpr int kMaxSlots = 64;

  private:
    mutable std::mutex m_mutex;
    std::array<std::string, kMaxSlots> m_slotNames;
};

PersonaPool& Personas();

} // namespace cs2bh
