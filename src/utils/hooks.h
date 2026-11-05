#pragma once

#include <khook.hpp>

#include <mutex>
#include <utility>
#include <vector>

namespace cs2bh::hooks {

template <typename CLASS, typename RETURN, typename... ARGS> class CheckedVirtual : public KHook::Virtual<CLASS, RETURN, ARGS...>
{
    using Base = KHook::Virtual<CLASS, RETURN, ARGS...>;

  public:
    using Base::Base;

    // Register an object and verify its hook ID.
    bool AddChecked(CLASS* object)
    {
        if (!object || !KHook::__exported__khook || this->_vtbl_index < 0) return false;

        void** hookAddress = *(void***)object + this->_vtbl_index;
        Base::Add(object);
        std::lock_guard guard(this->_hooks_stored);
        return this->_addr_hook_ids.find(hookAddress) != this->_addr_hook_ids.end();
    }

    bool RemoveAll()
    {
        std::vector<std::pair<KHook::HookID_t, void*>> hooks;
        {
            std::lock_guard guard(this->_hooks_stored);
            hooks.reserve(this->_hook_ids_addr.size());
            for (const auto& [id, address] : this->_hook_ids_addr)
                hooks.emplace_back(id, address);
        }

        const bool available = KHook::__exported__khook != nullptr;
        if (available)
        {
            for (const auto& [id, address] : hooks)
            {
                KHook::RemoveHook(id, false);
                std::lock_guard guard(this->_hooks_stored);
                this->_hook_ids_addr.erase(id);
                this->_addr_hook_ids.erase(address);
            }
        }

        this->ClearHooks();
        return available;
    }
};

} // namespace cs2bh::hooks
