// ping_display.h

#pragma once

#include <array>
#include <cstdint>

namespace cs2bh {

class PingDisplay
{
  public:
    static constexpr int kWindowTicks = 64;
    static constexpr int kWriteEveryTicks = 32;

    void RecordSample(int latencyMs);

    // Returns -1 when no value should be written this tick
    int MaybeProduce();

    void Reset();

    int LastWrittenPing() const { return m_lastWritten; }
    int CurrentAverage() const;

  private:
    std::array<int, kWindowTicks> m_samples{};
    int m_idx = 0;
    int m_sum = 0;
    int m_filled = 0;
    int m_ticksSinceWrite = 0;
    int m_lastWritten = 0;
};

class PingJitter
{
  public:
    explicit PingJitter(int baselineMs, int minimumMs = 1, int maximumMs = 999);
    int NextSample();

  private:
    int m_baseline;
    int m_minimum;
    int m_maximum;
    uint64_t m_state;
};

} // namespace cs2bh
