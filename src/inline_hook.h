// inline_hook.h

#pragma once

#if defined(_WIN32)

#include <Windows.h>
#include <cstdint>
#include <cstring>

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

    class InlineHook
    {
    public:
        // stealLen must cover whole, position-independent instructions; >=14 and <=sizeof(m_original)
        bool Install(void *target, void *detour, size_t stealLen)
        {
            if (m_active || !target || !detour || stealLen < 14 || stealLen > sizeof(m_original))
                return false;

            // Trampoline = stolen bytes + jmp back to target+stealLen
            m_trampoline = VirtualAlloc(nullptr, stealLen + 14,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!m_trampoline)
                return false;
            auto *tramp = static_cast<uint8_t *>(m_trampoline);
            std::memcpy(tramp, target, stealLen);
            WriteAbsJmp(tramp + stealLen, static_cast<uint8_t *>(target) + stealLen);

            // overwrite target with jmp to detour, nop-pad the remainder
            std::memcpy(m_original, target, stealLen);
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

            m_target = target;
            m_stealLen = stealLen;
            m_active = true;
            return true;
        }

        void Remove()
        {
            if (!m_active)
                return;
            DWORD oldProt = 0;
            if (VirtualProtect(m_target, m_stealLen, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                std::memcpy(m_target, m_original, m_stealLen);
                VirtualProtect(m_target, m_stealLen, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), m_target, m_stealLen);
            }
            if (m_trampoline)
                VirtualFree(m_trampoline, 0, MEM_RELEASE);
            m_trampoline = nullptr;
            m_active = false;
        }

        void *Trampoline() const { return m_trampoline; }
        bool Active() const { return m_active; }

    private:
        void *m_target = nullptr;
        void *m_trampoline = nullptr;
        size_t m_stealLen = 0;
        uint8_t m_original[32] = {};
        bool m_active = false;
    };
} // namespace cs2bh::hook

#endif // _WIN32
