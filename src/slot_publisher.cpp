// slot_publisher.cpp
//
// See slot_shm.h

#include "slot_publisher.h"

#include <Windows.h>
#include <cstring>

namespace cs2bh
{

    namespace
    {
        SlotPublisher g_Publisher;
    }

    SlotPublisher &Publisher() { return g_Publisher; }

    SlotPublisher::~SlotPublisher() { Shutdown(); }

    // Create the page-file-backed mapping and stamp the header once
    bool SlotPublisher::Init()
    {
        if (m_pView)
            return true;

        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0, shm::kTotalSize,
                                      shm::kMappingName);
        if (!h)
            return false;

        auto *view = static_cast<unsigned char *>(
            MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, shm::kTotalSize));
        if (!view)
        {
            CloseHandle(h);
            return false;
        }

        m_hMapping = h;
        m_pView = view;

        // ReadIdx/WriteIdx start at 0
        std::memset(view, 0, shm::kTotalSize);
        *reinterpret_cast<uint32_t *>(view + shm::kOff_Magic) = shm::kMagic;
        *reinterpret_cast<uint32_t *>(view + shm::kOff_Version) = shm::kVersion;
        *reinterpret_cast<uint32_t *>(view + shm::kOff_MaxSlots) = shm::kMaxSlots;
        *reinterpret_cast<uint32_t *>(view + shm::kOff_DataGen) = 0;
        return true;
    }

    void SlotPublisher::Shutdown()
    {
        if (m_pView)
        {
            UnmapViewOfFile(m_pView);
            m_pView = nullptr;
        }
        if (m_hMapping)
        {
            CloseHandle(m_hMapping);
            m_hMapping = nullptr;
        }
    }

    // Internal pointer helpers

    unsigned char *SlotPublisher::SlotStatePtr() const
    {
        return m_pView + shm::kOff_SlotState;
    }

    uint64_t *SlotPublisher::SidPtr(int slot) const
    {
        return reinterpret_cast<uint64_t *>(
            m_pView + shm::kOff_SyntheticSid + slot * sizeof(uint64_t));
    }

    char *SlotPublisher::NamePtr(int slot) const
    {
        return reinterpret_cast<char *>(
            m_pView + shm::kOff_PersonaName + slot * shm::kNameLen);
    }

    int *SlotPublisher::PingPtr(int slot) const
    {
        return reinterpret_cast<int *>(
            m_pView + shm::kOff_CurrentPing + slot * sizeof(int));
    }

    char *SlotPublisher::CrosshairPtr(int slot) const
    {
        return reinterpret_cast<char *>(
            m_pView + shm::kOff_Crosshair + slot * shm::kCrosshairLen);
    }

    void SlotPublisher::BumpGen()
    {
        auto *gen = reinterpret_cast<volatile uint32_t *>(m_pView + shm::kOff_DataGen);
        *gen = *gen + 1;
    }

    // Data-region writers

    void SlotPublisher::PublishAdopt(int slot, uint64_t syntheticSid,
                                     const char *personaName, const char *crosshairCode)
    {
        if (!m_pView || slot < 0 || slot >= shm::kMaxSlots)
            return;
        *SidPtr(slot) = syntheticSid;
        char *dst = NamePtr(slot);
        std::memset(dst, 0, shm::kNameLen);
        if (personaName)
        {
            std::strncpy(dst, personaName, shm::kNameLen - 1);
        }
        char *cross = CrosshairPtr(slot);
        std::memset(cross, 0, shm::kCrosshairLen);
        if (crosshairCode)
        {
            std::strncpy(cross, crosshairCode, shm::kCrosshairLen - 1);
        }
        *PingPtr(slot) = 0;
        SlotStatePtr()[slot] = 1;
        BumpGen();
    }

    void SlotPublisher::PublishRelease(int slot)
    {
        if (!m_pView || slot < 0 || slot >= shm::kMaxSlots)
            return;
        SlotStatePtr()[slot] = 0;
        *SidPtr(slot) = 0;
        std::memset(NamePtr(slot), 0, shm::kNameLen);
        std::memset(CrosshairPtr(slot), 0, shm::kCrosshairLen);
        *PingPtr(slot) = 0;
        BumpGen();
    }

    void SlotPublisher::UpdatePing(int slot, int ping)
    {
        if (!m_pView || slot < 0 || slot >= shm::kMaxSlots)
            return;
        *PingPtr(slot) = ping;
        BumpGen();
    }

    void SlotPublisher::UpdateSyntheticSid(int slot, uint64_t sid)
    {
        if (!m_pView || slot < 0 || slot >= shm::kMaxSlots)
            return;
        *SidPtr(slot) = sid;
        BumpGen();
    }

    void SlotPublisher::UpdatePersonaName(int slot, const char *name)
    {
        if (!m_pView || slot < 0 || slot >= shm::kMaxSlots)
            return;
        char *dst = NamePtr(slot);
        std::memset(dst, 0, shm::kNameLen);
        if (name)
            std::strncpy(dst, name, shm::kNameLen - 1);
        BumpGen();
    }

    // CSS->C++

    void SlotPublisher::DrainCommands(const SteamIdSink &onSteamId,
                                      const PersonaSink &onPersona)
    {
        if (!m_pView)
            return;
        auto *writeIdx = reinterpret_cast<volatile uint32_t *>(m_pView + shm::kOff_WriteIdx);
        auto *readIdx = reinterpret_cast<volatile uint32_t *>(m_pView + shm::kOff_ReadIdx);
        auto *cmds = reinterpret_cast<shm::Command *>(m_pView + shm::kOff_Cmds);

        uint32_t w = *writeIdx;
        uint32_t r = *readIdx;
        // Guard against a runaway producer: process at most kCmdCount entries
        int budget = shm::kCmdCount;
        while (r != w && budget-- > 0)
        {
            const shm::Command &c = cmds[r % shm::kCmdCount];
            int slot = c.Slot;
            if (slot >= 0 && slot < shm::kMaxSlots)
            {
                if (c.Type == shm::kCmd_SetSteamId && onSteamId)
                {
                    onSteamId(slot, c.SteamId);
                }
                else if (c.Type == shm::kCmd_SetPersona && onPersona)
                {
                    char name[shm::kNameLen];
                    std::memcpy(name, c.Name, shm::kNameLen);
                    name[shm::kNameLen - 1] = '\0';
                    onPersona(slot, name);
                }
            }
            ++r;
        }
        *readIdx = r;
    }

} // namespace cs2bh
