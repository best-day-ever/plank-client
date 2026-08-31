#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

class VideoPacketLossInterval
{
public:
    std::optional<float> addCumulative(std::uint64_t sourceSymbols,
                                       std::uint64_t missingSourceSymbols)
    {
        if (missingSourceSymbols > sourceSymbols) {
            reset();
            return std::nullopt;
        }

        if (!m_Initialized || sourceSymbols < m_SourceSymbols ||
                missingSourceSymbols < m_MissingSourceSymbols) {
            m_Initialized = true;
            m_SourceSymbols = sourceSymbols;
            m_MissingSourceSymbols = missingSourceSymbols;
            return std::nullopt;
        }

        const std::uint64_t intervalSourceSymbols =
                sourceSymbols - m_SourceSymbols;
        const std::uint64_t intervalMissingSourceSymbols =
                missingSourceSymbols - m_MissingSourceSymbols;
        m_SourceSymbols = sourceSymbols;
        m_MissingSourceSymbols = missingSourceSymbols;
        if (intervalSourceSymbols == 0 ||
                intervalMissingSourceSymbols > intervalSourceSymbols) {
            return std::nullopt;
        }

        return static_cast<float>(intervalMissingSourceSymbols) * 100.0f /
                static_cast<float>(intervalSourceSymbols);
    }

    void reset()
    {
        m_Initialized = false;
        m_SourceSymbols = 0;
        m_MissingSourceSymbols = 0;
    }

private:
    bool m_Initialized = false;
    std::uint64_t m_SourceSymbols = 0;
    std::uint64_t m_MissingSourceSymbols = 0;
};

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

    void reset()
    {
        m_Samples.clear();
    }

private:
    std::deque<std::pair<std::uint64_t, float>> m_Samples;
};
