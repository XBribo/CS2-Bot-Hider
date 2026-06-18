// inline_hook.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace cs2bh::hook
{
    // 14-byte RIP-relative absolute jump: FF 25 00000000 <abs64>
    inline void WriteAbsJmp(uint8_t *at, const void *dest)
    {
        at[0] = 0xFF;
        at[1] = 0x25;
        *reinterpret_cast<uint32_t *>(at + 2) = 0; // disp32 = 0
        *reinterpret_cast<uint64_t *>(at + 6) = reinterpret_cast<uint64_t>(dest);
    }

#if !defined(_WIN32)
    inline size_t PageSize()
    {
        static size_t s_pageSize = [] {
            long value = sysconf(_SC_PAGESIZE);
            return value > 0 ? static_cast<size_t>(value) : static_cast<size_t>(4096);
        }();
        return s_pageSize;
    }

    inline void *PageStart(void *addr)
    {
        auto page = PageSize();
        auto value = reinterpret_cast<uintptr_t>(addr);
        return reinterpret_cast<void *>(value & ~(static_cast<uintptr_t>(page) - 1));
    }

    inline size_t PageSpan(void *addr, size_t len)
    {
        auto page = PageSize();
        auto start = reinterpret_cast<uintptr_t>(PageStart(addr));
        auto end = reinterpret_cast<uintptr_t>(addr) + len;
        return ((end - start + page - 1) / page) * page;
    }
#endif

    class InlineHook
    {
    public:
        // stealLen must cover whole, position-independent instructions; >=14 and <=sizeof(m_original)
        bool Install(void *target, void *detour, size_t stealLen)
        {
            if (m_active || !target || !detour || stealLen < 14 || stealLen > sizeof(m_original))
                return false;

            // Trampoline = stolen bytes + jmp back to target+stealLen
#if defined(_WIN32)
            m_trampoline = VirtualAlloc(nullptr, stealLen + 14,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!m_trampoline)
                return false;
#else
            m_trampolineSize = stealLen + 14;
            m_trampoline = mmap(nullptr, m_trampolineSize,
                                PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m_trampoline == MAP_FAILED)
            {
                m_trampoline = nullptr;
                m_trampolineSize = 0;
                return false;
            }
#endif
            auto *tramp = static_cast<uint8_t *>(m_trampoline);
            std::memcpy(tramp, target, stealLen);
            WriteAbsJmp(tramp + stealLen, static_cast<uint8_t *>(target) + stealLen);

            // overwrite target with jmp to detour, nop-pad the remainder
            std::memcpy(m_original, target, stealLen);
#if defined(_WIN32)
            DWORD oldProt = 0;
            if (!VirtualProtect(target, stealLen, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                VirtualFree(m_trampoline, 0, MEM_RELEASE);
                m_trampoline = nullptr;
                return false;
            }
            auto *t = static_cast<uint8_t *>(target);
            WriteAbsJmp(t, detour);
            for (size_t i = 14; i < stealLen; ++i)
                t[i] = 0x90;
            VirtualProtect(target, stealLen, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), target, stealLen);
#else
            void *pageStart = PageStart(target);
            size_t pageSpan = PageSpan(target, stealLen);
            if (mprotect(pageStart, pageSpan, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            {
                munmap(m_trampoline, m_trampolineSize);
                m_trampoline = nullptr;
                m_trampolineSize = 0;
                return false;
            }
            auto *t = static_cast<uint8_t *>(target);
            WriteAbsJmp(t, detour);
            for (size_t i = 14; i < stealLen; ++i)
                t[i] = 0x90;
            __builtin___clear_cache(reinterpret_cast<char *>(target),
                                    reinterpret_cast<char *>(target) + stealLen);
            mprotect(pageStart, pageSpan, PROT_READ | PROT_EXEC);
#endif

            m_target = target;
            m_stealLen = stealLen;
            m_active = true;
            return true;
        }

        void Remove()
        {
            if (!m_active)
                return;
#if defined(_WIN32)
            DWORD oldProt = 0;
            if (VirtualProtect(m_target, m_stealLen, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                std::memcpy(m_target, m_original, m_stealLen);
                VirtualProtect(m_target, m_stealLen, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), m_target, m_stealLen);
            }
            if (m_trampoline)
                VirtualFree(m_trampoline, 0, MEM_RELEASE);
#else
            void *pageStart = PageStart(m_target);
            size_t pageSpan = PageSpan(m_target, m_stealLen);
            if (mprotect(pageStart, pageSpan, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
            {
                std::memcpy(m_target, m_original, m_stealLen);
                __builtin___clear_cache(reinterpret_cast<char *>(m_target),
                                        reinterpret_cast<char *>(m_target) + m_stealLen);
                mprotect(pageStart, pageSpan, PROT_READ | PROT_EXEC);
            }
            if (m_trampoline)
                munmap(m_trampoline, m_trampolineSize);
#endif
            m_trampoline = nullptr;
            m_trampolineSize = 0;
            m_active = false;
        }

        void *Trampoline() const { return m_trampoline; }
        // Resolved target address, or nullptr if not installed
        void *Target() const { return m_target; }
        bool Active() const { return m_active; }

    private:
        void *m_target = nullptr;
        void *m_trampoline = nullptr;
        size_t m_trampolineSize = 0;
        size_t m_stealLen = 0;
        uint8_t m_original[32] = {};
        bool m_active = false;
    };
} // namespace cs2bh::hook
