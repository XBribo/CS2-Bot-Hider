#pragma once

#include <khook.hpp>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace cs2bh::hooks {

inline thread_local uint32_t g_callbackDepth = 0;
inline thread_local uint32_t g_dispatchDepth = 0;

// Track callback depth.
class CallbackScope
{
  public:
    CallbackScope() noexcept { ++g_callbackDepth; }
    ~CallbackScope() { --g_callbackDepth; }
};

// Track dispatch depth for safe hook removal.
inline void BeginDispatch() noexcept { ++g_dispatchDepth; }
inline void EndDispatch() noexcept
{
    if (g_dispatchDepth != 0) --g_dispatchDepth;
}

// Check synchronous removal safety on this thread.
inline bool CanRemoveHooks() noexcept { return g_callbackDepth == 0 && g_dispatchDepth == 0; }

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

    // Check remaining KHook registrations.
    bool HasRegistrations()
    {
        std::lock_guard guard(this->_hooks_stored);
        return !this->_addr_hook_ids.empty();
    }

    // Retain contexts and filters during async POST callbacks.
    bool RemoveAll(bool asynchronous = false)
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
                KHook::RemoveHook(id, asynchronous);
                if (!asynchronous)
                {
                    std::lock_guard guard(this->_hooks_stored);
                    this->_hook_ids_addr.erase(id);
                    this->_addr_hook_ids.erase(address);
                }
            }
        }

        if (!asynchronous) this->ClearHooks();
        return available;
    }
};

} // namespace cs2bh::hooks
