#include "avsynccontroller.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace StationConnectAvSync {
namespace {
constexpr std::int32_t MinimumUpdateIntervalMs = 1000;
constexpr double MinimumFitDurationMs = 120000.0;
constexpr double FitWindowDurationMs = 300000.0;
constexpr std::int32_t MaximumVideoClockAgeMs = 2000;
constexpr int MaximumCorrectionStepPpm = 2;
constexpr std::int32_t BacklogUpdateIntervalMs = 100;
constexpr int BacklogTargetMs = 15;
constexpr int BacklogGainPpmPerMs = 500;
constexpr int MaximumBacklogCorrectionStepPpm = 1000;

std::mutex videoClockLock;
VideoClockSample latestVideoClock;
}

void resetVideoClock()
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    latestVideoClock = {};
}

void publishVideoClock(std::int64_t mediaTimeMs,
                       std::uint32_t presentationTicks)
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    latestVideoClock.mediaTimeMs = mediaTimeMs;
    latestVideoClock.presentationTicks = presentationTicks;
    latestVideoClock.valid = true;
}

VideoClockSample readVideoClock()
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    return latestVideoClock;
}

void AudioRateController::reset()
{
    m_AudioPoints.clear();
    m_VideoPoints.clear();
    m_FirstAudioFrames = 0;
    m_FirstVideoMediaMs = 0;
    m_FirstAudioObservation = 0;
    m_FirstVideoPresentation = 0;
    m_LastAudioObservation = 0;
    m_LastVideoPresentation = 0;
    m_CorrectionPpm = 0;
    m_Anchored = false;
}

AudioRateController::Result AudioRateController::update(
        std::uint64_t rawAudioFrames,
        int sampleRate,
        std::uint32_t audioObservationTicks,
        const VideoClockSample& videoClock)
{
    Result result {m_CorrectionPpm, false};
    if (sampleRate <= 0 || !videoClock.valid) {
        return result;
    }

    const std::int32_t videoClockAge = static_cast<std::int32_t>(
        audioObservationTicks - videoClock.presentationTicks);
    if (std::abs(videoClockAge) > MaximumVideoClockAgeMs) {
        return result;
    }

    if (!m_Anchored) {
        m_FirstAudioFrames = rawAudioFrames;
        m_FirstVideoMediaMs = videoClock.mediaTimeMs;
        m_FirstAudioObservation = audioObservationTicks;
        m_FirstVideoPresentation = videoClock.presentationTicks;
        m_LastAudioObservation = audioObservationTicks;
        m_LastVideoPresentation = videoClock.presentationTicks;
        m_AudioPoints.push_back({0.0, 0.0});
        m_VideoPoints.push_back({0.0, 0.0});
        m_Anchored = true;
        return result;
    }

    if (static_cast<std::int32_t>(audioObservationTicks -
                                  m_LastAudioObservation) <
            MinimumUpdateIntervalMs) {
        return result;
    }
    m_LastAudioObservation = audioObservationTicks;

    const double audioObservationElapsed = static_cast<std::uint32_t>(
        audioObservationTicks - m_FirstAudioObservation);
    const double audioMediaElapsed =
        static_cast<double>(rawAudioFrames - m_FirstAudioFrames) * 1000.0 /
        sampleRate;
    m_AudioPoints.push_back({audioObservationElapsed, audioMediaElapsed});

    if (videoClock.presentationTicks != m_LastVideoPresentation) {
        const double videoPresentationElapsed = static_cast<std::uint32_t>(
            videoClock.presentationTicks - m_FirstVideoPresentation);
        const double videoMediaElapsed = static_cast<double>(
            videoClock.mediaTimeMs - m_FirstVideoMediaMs);
        if (videoMediaElapsed < 0.0) {
            reset();
            return result;
        }
        m_VideoPoints.push_back({videoPresentationElapsed, videoMediaElapsed});
        m_LastVideoPresentation = videoClock.presentationTicks;
    }

    trimWindow(m_AudioPoints);
    trimWindow(m_VideoPoints);
    if (m_AudioPoints.size() < 20 || m_VideoPoints.size() < 20 ||
            m_AudioPoints.back().clockMs -
                m_AudioPoints.front().clockMs < MinimumFitDurationMs ||
            m_VideoPoints.back().clockMs -
                m_VideoPoints.front().clockMs < MinimumFitDurationMs) {
        return result;
    }

    const double audioRate = fitRate(m_AudioPoints);
    const double videoRate = fitRate(m_VideoPoints);
    if (audioRate <= 0.0 || videoRate <= 0.0) {
        return result;
    }

    const int measuredPpm = std::clamp(
        static_cast<int>(std::llround((audioRate / videoRate - 1.0) * 1000000.0)),
        -MaximumCorrectionPpm,
        MaximumCorrectionPpm);
    const int delta = std::clamp(measuredPpm - m_CorrectionPpm,
                                 -MaximumCorrectionStepPpm,
                                 MaximumCorrectionStepPpm);
    m_CorrectionPpm += delta;
    if (std::abs(m_CorrectionPpm) < 2) {
        m_CorrectionPpm = 0;
    }

    result.correctionPpm = m_CorrectionPpm;
    result.updated = true;
    return result;
}

int AudioRateController::correctionPpm() const
{
    return m_CorrectionPpm;
}

double AudioRateController::fitRate(const std::deque<ClockPoint>& points)
{
    double meanClock = 0.0;
    double meanMedia = 0.0;
    for (const ClockPoint& point : points) {
        meanClock += point.clockMs;
        meanMedia += point.mediaMs;
    }
    meanClock /= points.size();
    meanMedia /= points.size();

    double covariance = 0.0;
    double variance = 0.0;
    for (const ClockPoint& point : points) {
        const double clockDelta = point.clockMs - meanClock;
        covariance += clockDelta * (point.mediaMs - meanMedia);
        variance += clockDelta * clockDelta;
    }
    return variance == 0.0 ? 0.0 : covariance / variance;
}

void AudioRateController::trimWindow(std::deque<ClockPoint>& points)
{
    while (points.size() > 2 &&
           points.back().clockMs - points.front().clockMs >
               FitWindowDurationMs) {
        points.pop_front();
    }
}

void AudioBacklogController::reset()
{
    m_LastUpdateTicks = 0;
    m_CorrectionPpm = 0;
    m_Anchored = false;
}

AudioBacklogController::Result AudioBacklogController::update(
        int pendingAudioMs,
        std::uint32_t observationTicks)
{
    Result result {m_CorrectionPpm, false};
    if (!m_Anchored) {
        m_LastUpdateTicks = observationTicks;
        m_Anchored = true;
        return result;
    }

    if (static_cast<std::int32_t>(observationTicks - m_LastUpdateTicks) <
            BacklogUpdateIntervalMs) {
        return result;
    }
    m_LastUpdateTicks = observationTicks;

    const int targetCorrection = std::clamp(
        (std::max(pendingAudioMs, BacklogTargetMs) - BacklogTargetMs) *
            BacklogGainPpmPerMs,
        0,
        MaximumCorrectionPpm);
    const int delta = std::clamp(targetCorrection - m_CorrectionPpm,
                                 -MaximumBacklogCorrectionStepPpm,
                                 MaximumBacklogCorrectionStepPpm);
    m_CorrectionPpm += delta;
    result.correctionPpm = m_CorrectionPpm;
    result.updated = true;
    return result;
}

int AudioBacklogController::correctionPpm() const
{
    return m_CorrectionPpm;
}

} // namespace StationConnectAvSync
