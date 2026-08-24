#pragma once

#include "renderer.h"
#include "streaming/avsynccontroller.h"
#include "SDL_compat.h"
#include <vector>

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
struct SwrContext;
#endif

class SdlAudioRenderer : public IAudioRenderer
{
public:
    explicit SdlAudioRenderer(bool enableAvSyncCorrection = false);

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual int getCapabilities();

    virtual int getQueuedAudioDurationMs() override;

    virtual int getDeviceBufferDurationMs() override;

    virtual qint64 getSubmittedAudioMediaTimeMs() override;

    virtual int getAudioClockCorrectionPpm() override;

    virtual int getAudioBacklogCorrectionPpm() override;

    virtual quint64 getSkippedAudioBlockCount() override;
    virtual AudioFormat getAudioBufferFormat();

private:
    SDL_AudioDeviceID m_AudioDevice;
    void* m_AudioBuffer;
    int m_FrameSize;
    int m_BytesPerSampleFrame;
    int m_BytesPerSecond;
    int m_DeviceBufferDurationMs;
    int m_SampleRate;
    int m_ChannelCount;
    bool m_EnableAvSyncCorrection;
    quint64 m_RawAudioFrames;
    quint64 m_SubmittedAudioFrames;
    qint64 m_LastSubmittedAudioMediaTimeMs;
    quint64 m_SkippedAudioBlocks;
    StationConnectAvSync::AudioRateController m_AudioRateController;
    StationConnectAvSync::AudioBacklogController m_AudioBacklogController;

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
    SwrContext* m_SwrContext;
    std::vector<float> m_CorrectedAudioBuffer;
#endif
};
