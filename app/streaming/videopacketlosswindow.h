#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <utility>

class VideoPacketLossPeakWindow
{
public:
    static constexpr std::uint64_t kWindowMs = 10000;

    float addSample(std::uint64_t timestampMs, float packetLossPercent)
    {
        while (!m_Samples.empty() &&
               timestampMs - m_Samples.front().first >= kWindowMs) {
            m_Samples.pop_front();
        }

        m_Samples.emplace_back(timestampMs, packetLossPercent);

        float peakPacketLossPercent = packetLossPercent;
        for (const auto& sample : m_Samples) {
            peakPacketLossPercent =
                    std::max(peakPacketLossPercent, sample.second);
        }

        return peakPacketLossPercent;
    }

private:
    std::deque<std::pair<std::uint64_t, float>> m_Samples;
};
