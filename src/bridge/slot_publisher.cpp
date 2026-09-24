#include "slot_publisher.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace cs2bh {
namespace {
SlotPublisher publisher;
template <size_t N> void Copy(char (&to)[N], const char* from)
{
    std::memset(to, 0, N);
    if (from) std::strncpy(to, from, N - 1);
}
} // namespace
SlotPublisher& Publisher() { return publisher; }
bool SlotPublisher::Init()
{
    m_thread = std::this_thread::get_id();
    m_session = std::max(m_session + 1, static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    m_slots = {};
    m_avatars = {};
    m_avatarApplied = {};
    m_signatures = {};
    m_signatureCount = 0;
    m_listener = nullptr;
    m_active = true;
    return true;
}
bool SlotPublisher::Active() const { return std::this_thread::get_id() == m_thread && m_active; }
bool SlotPublisher::Listen(uint64_t session, PresentationChanged listener)
{
    if (!Active() || session != m_session) return false;
    m_listener = listener;
    return true;
}
void SlotPublisher::Shutdown()
{
    m_active = false;
    m_slots = {};
    m_avatars = {};
    m_avatarApplied = {};
    auto listener = m_listener;
    m_listener = nullptr;
    if (listener) listener(kPresentationRosterChanged, -1);
}
bool SlotPublisher::ReadSlot(int slot, PresentationSlot& out) const
{
    if (!Active() || slot < 0 || slot >= 64) return false;
    out = m_slots[slot];
    out.Session = m_session;
    return true;
}
bool SlotPublisher::Matches(int slot, uint64_t session, uint64_t incarnation) const
{
    return Active() && session == m_session && incarnation != 0 && slot >= 0 && slot < 64 && m_slots[slot].Managed &&
           m_slots[slot].Incarnation == incarnation;
}
void SlotPublisher::PublishAdopt(int slot, uint64_t sid, const char* name, const char* crosshair, uint32_t flair)
{
    if (!Active() || slot < 0 || slot >= 64) return;
    auto& s = m_slots[slot];
    s = {};
    m_avatars[slot] = {};
    m_avatarApplied[slot] = false;
    s.Session = m_session;
    s.Incarnation = ++m_nextIncarnation;
    s.BaseSteamId = s.SteamId = sid;
    s.Managed = 1;
    s.ScoreboardFlair = flair;
    Copy(s.BaseName, name);
    Copy(s.Name, name);
    Copy(s.Crosshair, crosshair);
    Notify(kPresentationRosterChanged, slot);
}
void SlotPublisher::PublishRelease(int slot)
{
    if (Active() && slot >= 0 && slot < 64 && m_slots[slot].Managed)
    {
        m_slots[slot] = {};
        m_avatars[slot] = {};
        m_avatarApplied[slot] = false;
        Notify(kPresentationRosterChanged, slot);
    }
}
void SlotPublisher::UpdateSyntheticSid(int slot, uint64_t sid)
{
    if (Active() && slot >= 0 && slot < 64) m_slots[slot].SteamId = sid;
}
void SlotPublisher::UpdatePersonaName(int slot, const char* name)
{
    if (Active() && slot >= 0 && slot < 64) Copy(m_slots[slot].Name, name);
}
void SlotPublisher::UpdatePing(int slot, int ping)
{
    if (Active() && slot >= 0 && slot < 64 && m_slots[slot].Ping != ping)
    {
        m_slots[slot].Ping = ping;
        Notify(kPresentationPingChanged, slot);
    }
}
void SlotPublisher::PublishSignature(const char* name, const void* address)
{
    if (!Active() || m_signatureCount >= 8) return;
    auto& s = m_signatures[m_signatureCount++];
    Copy(s.Name, name);
    s.Address = reinterpret_cast<uint64_t>(address);
}
bool SlotPublisher::ReadSignature(int index, PresentationSignature& out) const
{
    if (!Active() || index < 0 || index >= m_signatureCount) return false;
    out = m_signatures[index];
    return true;
}
} // namespace cs2bh

namespace cs2bh {
void SlotPublisher::UpdateBaseSyntheticSid(int slot, uint64_t sid)
{
    if (!Active() || slot < 0 || slot >= 64 || !m_slots[slot].Managed) return;
    m_slots[slot].BaseSteamId = m_slots[slot].SteamId = sid;
    Notify(kPresentationRosterChanged, slot);
}
bool SlotPublisher::SetBase(
    int slot, uint64_t session, uint64_t incarnation, uint64_t sid, const char* name, const char* crosshair, uint32_t flair)
{
    if (!Matches(slot, session, incarnation) || !sid || !name || !*name || !crosshair || !std::memchr(name, 0, 32) ||
        !std::memchr(crosshair, 0, 64) || flair > 65535)
        return false;
    auto& s = m_slots[slot];
    s.BaseSteamId = sid;
    Copy(s.BaseName, name);
    Copy(s.Crosshair, crosshair);
    s.ScoreboardFlair = flair;
    Notify(kPresentationRosterChanged, slot);
    return true;
}
uint64_t SlotPublisher::GetIncarnation(int slot) const { return Active() && slot >= 0 && slot < 64 ? m_slots[slot].Incarnation : 0; }
bool SlotPublisher::SetAvatar(int slot, uint64_t session, uint64_t incarnation, const unsigned char* data, int length)
{
    if (!Matches(slot, session, incarnation) || length < 0 || length > 16384 || (length && !data)) return false;
    auto& a = m_avatars[slot];
    std::vector<unsigned char> bytes;
    if (length) bytes.assign(data, data + length);
    a.data.swap(bytes);
    a.length = length;
    a.incarnation = incarnation;
    // A replacement bot can request the same SteamID before the next frame.
    // Never reuse its predecessor's request sequence in that case.
    a.sequence = ++m_nextAvatarSequence;
    if (a.sequence == 0) a.sequence = ++m_nextAvatarSequence;
    return true;
}
bool SlotPublisher::ReadAvatarMetadata(int slot, uint32_t& sequence, uint32_t& length, uint64_t& incarnation) const
{
    if (!Active() || slot < 0 || slot >= 64) return false;
    const auto& a = m_avatars[slot];
    sequence = a.sequence;
    length = a.length;
    incarnation = a.incarnation;
    return true;
}
bool SlotPublisher::ReadAvatarRequest(int slot, AvatarRequest& request) const
{
    if (!Active() || slot < 0 || slot >= 64) return false;
    request = m_avatars[slot];
    return true;
}
void SlotPublisher::PublishAvatarState(int slot, bool applied, uint64_t)
{
    if (Active() && slot >= 0 && slot < 64) m_avatarApplied[slot] = applied;
}
bool SlotPublisher::HasAvatar(int slot) const { return Active() && slot >= 0 && slot < 64 && m_avatarApplied[slot]; }
int SlotPublisher::AvatarSize(int slot) const { return Active() && slot >= 0 && slot < 64 ? m_avatars[slot].length : 0; }
} // namespace cs2bh
