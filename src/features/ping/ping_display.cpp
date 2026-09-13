// ping_display.cpp

#include "ping_display.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace cs2bh {

void PingDisplay::RecordSample(int latencyMs)
{
    int clamped = latencyMs;
    clamped = std::max(clamped, 0);
    clamped = std::min(clamped, 999);

    int oldest = m_samples[m_idx];
    m_samples[m_idx] = clamped;
    m_sum += clamped - oldest;
    m_idx = (m_idx + 1) % kWindowTicks;
    if (m_filled < kWindowTicks) ++m_filled;
}

int PingDisplay::CurrentAverage() const
{
    if (m_filled == 0) return 0;
    return m_sum / m_filled;
}

int PingDisplay::MaybeProduce()
{
    ++m_ticksSinceWrite;
    if (m_ticksSinceWrite < kWriteEveryTicks) return -1;
    m_ticksSinceWrite = 0;
    if (m_filled == 0) return -1;
    int avg = CurrentAverage();
    if (avg == m_lastWritten) return -1;
    m_lastWritten = avg;
    return avg;
}

void PingDisplay::Reset()
{
    m_samples.fill(0);
    m_idx = m_sum = m_filled = m_ticksSinceWrite = 0;
    m_lastWritten = 0;
}

// ─────────────────────────────────────────────────────────────────────

PingJitter::PingJitter(int baselineMs, int minimumMs, int maximumMs) : m_baseline(baselineMs), m_minimum(minimumMs), m_maximum(maximumMs)
{
    m_minimum = std::max(m_minimum, 1);
    m_maximum = std::min(m_maximum, 999);
    m_maximum = std::max(m_maximum, m_minimum);
    m_baseline = std::max(m_baseline, m_minimum);
    m_baseline = std::min(m_baseline, m_maximum);
    m_state = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
              (static_cast<uint64_t>(baselineMs) * 0x9E3779B97F4A7C15ULL) ^ 0xA0761D6478BD642FULL;
    if (m_state == 0) m_state = 0xDEADBEEFCAFEBABEULL;
}

int PingJitter::NextSample()
{
    // xorshift64*
    uint64_t x = m_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    m_state = x;
    uint64_t r = x * 0x2545F4914F6CDD1DULL;

    // +/- 10%
    int span = (m_baseline + 9) / 10;
    int delta = static_cast<int>(r % (2 * span + 1)) - span;
    int v = m_baseline + delta;
    v = std::max(v, m_minimum);
    v = std::min(v, m_maximum);
    return v;
}

} // namespace cs2bh
