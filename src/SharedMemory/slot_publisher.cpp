// slot_publisher.cpp
//
// See slot_shm.h

#include "slot_publisher.h"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <atomic>
#include <cstring>
#include <utility>

namespace cs2bh {

namespace {
SlotPublisher g_publisher;
}

SlotPublisher& Publisher() { return g_publisher; }

SlotPublisher::~SlotPublisher() { Shutdown(); }

// Create the page-file-backed mapping and stamp the header once
bool SlotPublisher::Init()
{
    if (m_view) return true;

#if defined(_WIN32)
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, shm::kTotalSize, shm::kMappingName);
    if (!h) return false;

    auto* view = static_cast<unsigned char*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, shm::kTotalSize));
    if (!view)
    {
        CloseHandle(h);
        return false;
    }

    m_mappingHandle = h;
#else
    int fd = shm_open(shm::kMappingName, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return false;

    if (ftruncate(fd, shm::kTotalSize) != 0)
    {
        close(fd);
        shm_unlink(shm::kMappingName);
        return false;
    }

    auto* view = static_cast<unsigned char*>(mmap(nullptr, shm::kTotalSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    if (view == MAP_FAILED) return false;

    m_mappingHandle = reinterpret_cast<void*>(1);
#endif
    m_view = view;

    // ReadIdx/WriteIdx start at 0
    std::memset(view, 0, shm::kTotalSize);
    *reinterpret_cast<uint32_t*>(view + shm::kOffMagic) = shm::kMagic;
    *reinterpret_cast<uint32_t*>(view + shm::kOffVersion) = shm::kVersion;
    *reinterpret_cast<uint32_t*>(view + shm::kOffMaxSlots) = shm::kMaxSlots;
    *reinterpret_cast<uint32_t*>(view + shm::kOffDataGen) = 0;
    m_nextIncarnation = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return true;
}

void SlotPublisher::Shutdown()
{
    if (m_view)
    {
#if defined(_WIN32)
        UnmapViewOfFile(m_view);
#else
        munmap(m_view, shm::kTotalSize);
        shm_unlink(shm::kMappingName);
#endif
        m_view = nullptr;
    }
    if (m_mappingHandle)
    {
#if defined(_WIN32)
        CloseHandle(m_mappingHandle);
#endif
        m_mappingHandle = nullptr;
    }
}

// Internal pointer helpers

unsigned char* SlotPublisher::SlotStatePtr() const { return m_view + shm::kOffSlotState; }

uint64_t* SlotPublisher::SidPtr(int slot) const
{
    return reinterpret_cast<uint64_t*>(m_view + shm::kOffSyntheticSid + slot * sizeof(uint64_t));
}

char* SlotPublisher::NamePtr(int slot) const { return reinterpret_cast<char*>(m_view + shm::kOffPersonaName + slot * shm::kNameLen); }

// Returns the base SteamID field for one slot
uint64_t* SlotPublisher::BaseSidPtr(int slot) const
{
    return reinterpret_cast<uint64_t*>(m_view + shm::kOffBaseSyntheticSid + slot * sizeof(uint64_t));
}

// Returns the base persona name field for one slot
char* SlotPublisher::BaseNamePtr(int slot) const
{
    return reinterpret_cast<char*>(m_view + shm::kOffBasePersonaName + slot * shm::kNameLen);
}

// Returns the native incarnation field for one slot
uint64_t* SlotPublisher::IncarnationPtr(int slot) const
{
    return reinterpret_cast<uint64_t*>(m_view + shm::kOffIncarnation + slot * sizeof(uint64_t));
}

int* SlotPublisher::PingPtr(int slot) const { return reinterpret_cast<int*>(m_view + shm::kOffCurrentPing + slot * sizeof(int)); }

char* SlotPublisher::CrosshairPtr(int slot) const
{
    return reinterpret_cast<char*>(m_view + shm::kOffCrosshair + slot * shm::kCrosshairLen);
}

uint32_t* SlotPublisher::ScoreboardFlairPtr(int slot) const
{
    return reinterpret_cast<uint32_t*>(m_view + shm::kOffScoreboardFlair + slot * sizeof(uint32_t));
}

// Returns the native avatar application flag for one slot
unsigned char* SlotPublisher::AvatarAppliedPtr(int slot) const { return m_view + shm::kOffAvatarApplied + slot; }

// Returns the applied avatar SteamID field for one slot
uint64_t* SlotPublisher::AvatarAppliedSidPtr(int slot) const
{
    return reinterpret_cast<uint64_t*>(m_view + shm::kOffAvatarAppliedSid + slot * sizeof(uint64_t));
}

// Returns a non-zero identity for one native managed-slot lifetime
uint64_t SlotPublisher::NextIncarnation()
{
    ++m_nextIncarnation;
    if (m_nextIncarnation == 0) ++m_nextIncarnation;
    return m_nextIncarnation;
}

void SlotPublisher::BumpGen()
{
    auto* gen = reinterpret_cast<volatile uint32_t*>(m_view + shm::kOffDataGen);
    *gen = *gen + 1;
}

// Data-region writers

void SlotPublisher::PublishAdopt(
    int slot, uint64_t syntheticSid, const char* personaName, const char* crosshairCode, uint32_t scoreboardFlair)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    *SidPtr(slot) = syntheticSid;
    *BaseSidPtr(slot) = syntheticSid;
    char* dst = NamePtr(slot);
    std::memset(dst, 0, shm::kNameLen);
    if (personaName)
    {
        std::strncpy(dst, personaName, shm::kNameLen - 1);
    }
    char* baseName = BaseNamePtr(slot);
    std::memset(baseName, 0, shm::kNameLen);
    if (personaName) std::strncpy(baseName, personaName, shm::kNameLen - 1);
    char* cross = CrosshairPtr(slot);
    std::memset(cross, 0, shm::kCrosshairLen);
    if (crosshairCode)
    {
        std::strncpy(cross, crosshairCode, shm::kCrosshairLen - 1);
    }
    *ScoreboardFlairPtr(slot) = scoreboardFlair;
    *AvatarAppliedPtr(slot) = 0;
    *AvatarAppliedSidPtr(slot) = 0;
    *PingPtr(slot) = 0;
    *IncarnationPtr(slot) = NextIncarnation();
    SlotStatePtr()[slot] = 1;
    BumpGen();
}

void SlotPublisher::PublishRelease(int slot)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    SlotStatePtr()[slot] = 0;
    *SidPtr(slot) = 0;
    *BaseSidPtr(slot) = 0;
    std::memset(NamePtr(slot), 0, shm::kNameLen);
    std::memset(BaseNamePtr(slot), 0, shm::kNameLen);
    std::memset(CrosshairPtr(slot), 0, shm::kCrosshairLen);
    *ScoreboardFlairPtr(slot) = 0;
    *AvatarAppliedPtr(slot) = 0;
    *AvatarAppliedSidPtr(slot) = 0;
    *PingPtr(slot) = 0;
    *IncarnationPtr(slot) = 0;
    BumpGen();
}

void SlotPublisher::UpdatePing(int slot, int ping)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    *PingPtr(slot) = ping;
    BumpGen();
}

// Updates the native persona SteamID and its current published value
void SlotPublisher::UpdateBaseSyntheticSid(int slot, uint64_t sid)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    *BaseSidPtr(slot) = sid;
    *SidPtr(slot) = sid;
    BumpGen();
}

void SlotPublisher::UpdateSyntheticSid(int slot, uint64_t sid)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    *SidPtr(slot) = sid;
    BumpGen();
}

void SlotPublisher::UpdatePersonaName(int slot, const char* name)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    char* dst = NamePtr(slot);
    std::memset(dst, 0, shm::kNameLen);
    if (name) std::strncpy(dst, name, shm::kNameLen - 1);
    BumpGen();
}

// Returns the current native managed-slot incarnation
uint64_t SlotPublisher::GetIncarnation(int slot) const
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return 0;
    return *IncarnationPtr(slot);
}

// Reads stable avatar metadata without copying PNG content
bool SlotPublisher::ReadAvatarMetadata(int slot, uint32_t& sequence, uint32_t& length, uint64_t& incarnation) const
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return false;

    auto* sequencePtr = reinterpret_cast<volatile uint32_t*>(m_view + shm::kOffAvatarSequence + slot * sizeof(uint32_t));
    uint32_t before = *sequencePtr;
    if ((before & 1u) != 0) return false;
    std::atomic_thread_fence(std::memory_order_acquire);

    uint32_t candidateLength = *reinterpret_cast<uint32_t*>(m_view + shm::kOffAvatarLength + slot * sizeof(uint32_t));
    uint64_t candidateIncarnation = *reinterpret_cast<uint64_t*>(m_view + shm::kOffAvatarIncarnation + slot * sizeof(uint64_t));
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t after = *sequencePtr;
    if (before != after || (after & 1u) != 0 || candidateLength > static_cast<uint32_t>(shm::kAvatarMaxBytes))
    {
        return false;
    }

    sequence = after;
    length = candidateLength;
    incarnation = candidateIncarnation;
    return true;
}

// Reads one stable avatar request from the C# writer
bool SlotPublisher::ReadAvatarRequest(int slot, AvatarRequest& request) const
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return false;

    auto* sequence = reinterpret_cast<volatile uint32_t*>(m_view + shm::kOffAvatarSequence + slot * sizeof(uint32_t));
    uint32_t before = *sequence;
    if ((before & 1u) != 0) return false;

    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t length = *reinterpret_cast<uint32_t*>(m_view + shm::kOffAvatarLength + slot * sizeof(uint32_t));
    uint64_t incarnation = *reinterpret_cast<uint64_t*>(m_view + shm::kOffAvatarIncarnation + slot * sizeof(uint64_t));
    if (length > static_cast<uint32_t>(shm::kAvatarMaxBytes)) return false;

    std::vector<unsigned char> data(length);
    if (length > 0)
    {
        std::memcpy(data.data(), m_view + shm::kOffAvatarData + slot * shm::kAvatarMaxBytes, length);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t after = *sequence;
    if (before != after || (after & 1u) != 0) return false;

    request.sequence = after;
    request.length = length;
    request.incarnation = incarnation;
    request.data = std::move(data);
    return true;
}

// Publishes whether the native avatar override is currently active
void SlotPublisher::PublishAvatarState(int slot, bool applied, uint64_t steamId)
{
    if (!m_view || slot < 0 || slot >= shm::kMaxSlots) return;
    *AvatarAppliedSidPtr(slot) = applied ? steamId : 0;
    *AvatarAppliedPtr(slot) = applied ? 1 : 0;
    BumpGen();
}

// Append a signature status entry at the current count slot
void SlotPublisher::PublishSignature(const char* name, const void* addr)
{
    if (!m_view || !name) return;
    auto* count = reinterpret_cast<uint32_t*>(m_view + shm::kOffSigCount);
    if (*count >= static_cast<uint32_t>(shm::kMaxSigs)) return;
    auto* entry = reinterpret_cast<shm::SigEntry*>(m_view + shm::kOffSigEntries + (*count) * shm::kSigEntrySize);
    std::memset(entry->name, 0, shm::kSigNameLen);
    std::strncpy(entry->name, name, shm::kSigNameLen - 1);
    entry->addr = reinterpret_cast<uint64_t>(addr);
    ++(*count);
    BumpGen();
}

// CSS->C++

void SlotPublisher::DrainCommands(const SteamIdSink& onSteamId,
                                  const PersonaSink& onPersona,
                                  const IdentityModeSink& onIdentityMode,
                                  const NameSourceSink& onNameSource)
{
    if (!m_view) return;
    auto* writeIdx = reinterpret_cast<volatile uint32_t*>(m_view + shm::kOffWriteIdx);
    auto* readIdx = reinterpret_cast<volatile uint32_t*>(m_view + shm::kOffReadIdx);
    auto* cmds = reinterpret_cast<shm::Command*>(m_view + shm::kOffCmds);

    uint32_t w = *writeIdx;
    uint32_t r = *readIdx;
    // Guard against a runaway producer: process at most kCmdCount entries
    int budget = shm::kCmdCount;
    while (r != w && budget-- > 0)
    {
        const shm::Command& c = cmds[r % shm::kCmdCount];
        // Global commands (no per-slot target)
        if (c.type == shm::CmdSetIdentityMode && onIdentityMode)
        {
            onIdentityMode(c.steamId != 0);
            ++r;
            continue;
        }
        if (c.type == shm::CmdSetNameSource && onNameSource)
        {
            onNameSource(c.steamId != 0);
            ++r;
            continue;
        }
        int slot = c.slot;
        if (slot >= 0 && slot < shm::kMaxSlots)
        {
            if (c.type == shm::CmdSetSteamId && onSteamId)
            {
                onSteamId(slot, c.steamId);
            }
            else if (c.type == shm::CmdSetPersona && onPersona)
            {
                char name[shm::kNameLen];
                std::memcpy(name, c.name, shm::kNameLen);
                name[shm::kNameLen - 1] = '\0';
                onPersona(slot, name);
            }
        }
        ++r;
    }
    *readIdx = r;
}

} // namespace cs2bh
