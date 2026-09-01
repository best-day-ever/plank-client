#pragma once

#include <cstdint>
#include <deque>

namespace PlankAvSync {

struct VideoClockSample
{
    std::int64_t mediaTimeMs = 0;
    std::uint32_t presentationTicks = 0;
    bool valid = false;
};

void resetVideoClock();

void publishVideoClock(std::int64_t mediaTimeMs,
                       std::uint32_t presentationTicks);

VideoClockSample readVideoClock();

class AudioRateController
{
public:
    static constexpr int MaximumCorrectionPpm = 250;

    struct Result
    {
        int correctionPpm = 0;
        bool updated = false;
    };

    void reset();

    Result update(std::uint64_t rawAudioFrames,
                  std::uint64_t submittedAudioFrames,
                  int sampleRate,
                  std::uint32_t audioObservationTicks,
                  const VideoClockSample& videoClock);

    int correctionPpm() const;

private:
    struct ClockPoint
    {
        double clockMs;
        double mediaMs;
    };

    static double fitRate(const std::deque<ClockPoint>& points);
    static void trimWindow(std::deque<ClockPoint>& points);

    std::deque<ClockPoint> m_AudioPoints;
    std::deque<ClockPoint> m_VideoPoints;
    std::uint64_t m_FirstAudioFrames = 0;
    std::uint64_t m_FirstSubmittedAudioFrames = 0;
    std::int64_t m_FirstVideoMediaMs = 0;
    std::uint32_t m_FirstAudioObservation = 0;
    std::uint32_t m_FirstVideoPresentation = 0;
    std::uint32_t m_LastAudioObservation = 0;
    std::uint32_t m_LastVideoPresentation = 0;
    int m_CorrectionPpm = 0;
    bool m_Anchored = false;
};

class AudioBacklogController
{
public:
    static constexpr int MaximumCorrectionPpm = 10000;

    struct Result
    {
        int correctionPpm = 0;
        bool updated = false;
    };

    void reset();

    Result update(int pendingAudioMs, std::uint32_t observationTicks);

    int correctionPpm() const;

private:
    std::uint32_t m_LastUpdateTicks = 0;
    int m_CorrectionPpm = 0;
    bool m_Anchored = false;
};

} // namespace PlankAvSync
