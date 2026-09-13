#pragma once

#include <khook.hpp>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace cs2bh::hooks {

inline thread_local uint32_t g_callbackDepth = 0;
inline thread_local uint32_t g_dispatchDepth = 0;

// Tracks execution inside a KHook callback.
class CallbackScope
{
  public:
    CallbackScope() noexcept { ++g_callbackDepth; }
    ~CallbackScope() { --g_callbackDepth; }
};

// Prevents synchronous removal while DispatchConCommand is still executing.
inline void BeginDispatch() noexcept { ++g_dispatchDepth; }
inline void EndDispatch() noexcept
{
    if (g_dispatchDepth != 0) --g_dispatchDepth;
}

// Returns whether KHook detours can be removed synchronously on this thread.
inline bool CanRemoveHooks() noexcept { return g_callbackDepth == 0 && g_dispatchDepth == 0; }

template <typename CLASS, typename RETURN, typename... ARGS> class CheckedVirtual : public KHook::Virtual<CLASS, RETURN, ARGS...>
{
    using Base = KHook::Virtual<CLASS, RETURN, ARGS...>;

  public:
    using Base::Base;

    // Adds one object and verifies that KHook assigned a hook ID.
    bool AddChecked(CLASS* object)
    {
        if (!object || !KHook::__exported__khook || this->_vtbl_index < 0) return false;

        void** hookAddress = *(void***)object + this->_vtbl_index;
        Base::Add(object);
        std::lock_guard guard(this->_hooks_stored);
        return this->_addr_hook_ids.find(hookAddress) != this->_addr_hook_ids.end();
    }

    // Reports registrations not yet acknowledged as removed by KHook.
    bool HasRegistrations()
    {
        std::lock_guard guard(this->_hooks_stored);
        return !this->_addr_hook_ids.empty();
    }

    // Async removal keeps contexts and object filters alive for in-flight POST callbacks.
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
