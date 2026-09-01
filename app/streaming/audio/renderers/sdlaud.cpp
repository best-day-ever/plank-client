#include "sdl.h"

#include <Limelight.h>
#include <cmath>

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

SdlAudioRenderer::SdlAudioRenderer(bool enableAvSyncCorrection)
    : m_AudioStream(nullptr),
      m_AudioBuffer(nullptr),
      m_FrameSize(0),
      m_FrameDurationMs(0),
      m_BytesPerSampleFrame(0),
      m_BytesPerSecond(0),
      m_DeviceBufferDurationMs(0),
      m_SampleRate(0),
      m_ChannelCount(0),
      m_EnableAvSyncCorrection(enableAvSyncCorrection),
      m_RawAudioFrames(0),
      m_SubmittedAudioFrames(0),
      m_LastSubmittedAudioMediaTimeMs(-1),
      m_SkippedAudioBlocks(0)
#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
      , m_SwrContext(nullptr)
#endif
{
    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));

#ifdef Q_OS_LINUX
    // The qualified PLANK Linux client talks to PipeWire directly.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "pipewire", SDL_HINT_OVERRIDE);
#endif

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        SDL_assert(SDL_WasInit(SDL_INIT_AUDIO));
    }
}

bool SdlAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    SDL_AudioSpec want = {};

    want.freq = opusConfig->sampleRate;
    want.format = SDL_AUDIO_F32;
    want.channels = opusConfig->channelCount;

    m_FrameDurationMs = opusConfig->samplesPerFrame / (opusConfig->sampleRate / 1000);
    m_FrameSize = opusConfig->samplesPerFrame *
                  opusConfig->channelCount *
                  getAudioBufferSampleSize();

    m_AudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &want, nullptr, nullptr);
    if (m_AudioStream == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open audio device: %s",
                     SDL_GetError());
        return false;
    }

    SDL_AudioSpec deviceSpec = {};
    int deviceSampleFrames = 0;
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(m_AudioStream);
    if (!SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceSampleFrames)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to query audio device format: %s", SDL_GetError());
        return false;
    }

    // SDL3's device stream converts from this source format to the physical
    // device format. Queue accounting remains in source-format bytes.
    m_BytesPerSecond = want.freq * want.channels * getAudioBufferSampleSize();
    m_BytesPerSampleFrame = want.channels * getAudioBufferSampleSize();
    m_DeviceBufferDurationMs = deviceSampleFrames * 1000 / deviceSpec.freq;
    m_SampleRate = want.freq;
    m_ChannelCount = want.channels;

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
    if (m_EnableAvSyncCorrection) {
        AVChannelLayout channelLayout;
        av_channel_layout_default(&channelLayout, want.channels);
        const int allocationResult = swr_alloc_set_opts2(
            &m_SwrContext,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            want.freq,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            opusConfig->sampleRate,
            0,
            nullptr);
        if (m_SwrContext != nullptr) {
            // Keep the resampler active from the first block. Otherwise the
            // first compensation request would reinitialize it mid-stream.
            av_opt_set_int(m_SwrContext, "flags", SWR_FLAG_RESAMPLE, 0);
        }
        av_channel_layout_uninit(&channelLayout);
        if (allocationResult < 0 || m_SwrContext == nullptr ||
                swr_init(m_SwrContext) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to initialize PLANK A/V audio correction");
            swr_free(&m_SwrContext);
            m_EnableAvSyncCorrection = false;
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PLANK video-master audio correction enabled (limit %d ppm)",
                        PlankAvSync::AudioRateController::MaximumCorrectionPpm);
        }
    }
#else
    m_EnableAvSyncCorrection = false;
#endif

    m_AudioBuffer = SDL_malloc(m_FrameSize);
    if (m_AudioBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to allocate audio buffer");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Decoded audio block: %u samples (%u bytes)",
                opusConfig->samplesPerFrame,
                m_FrameSize);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PipeWire device buffer: %u samples (%u ms)",
                deviceSampleFrames,
                m_DeviceBufferDurationMs);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SDL audio driver: %s",
                SDL_GetCurrentAudioDriver());

    // Start playback
    if (!SDL_ResumeAudioStreamDevice(m_AudioStream)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to start audio stream: %s", SDL_GetError());
        return false;
    }

    return true;
}

SdlAudioRenderer::~SdlAudioRenderer()
{
    if (m_AudioStream != nullptr) {
        // Stop playback
        SDL_PauseAudioStreamDevice(m_AudioStream);
        SDL_DestroyAudioStream(m_AudioStream);
    }

    if (m_AudioBuffer != nullptr) {
        SDL_free(m_AudioBuffer);
    }

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
    swr_free(&m_SwrContext);
#endif

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));
}

void* SdlAudioRenderer::getAudioBuffer(int*)
{
    return m_AudioBuffer;
}

bool SdlAudioRenderer::submitAudio(int bytesWritten)
{
    if (bytesWritten == 0) {
        // Nothing to do
        return true;
    }

    const int inputFrames = m_BytesPerSampleFrame == 0 ?
                                0 : bytesWritten / m_BytesPerSampleFrame;

    const int pendingAudioMs = LiGetPendingAudioDuration();

    // Generic Moonlight drops decoded audio to recover latency when its input
    // queue grows. PLANK instead uses bounded resampling catch-up for
    // ordinary scheduling bursts and retains a hard emergency ceiling.
    constexpr int MaximumPlankPendingAudioMs = 100;
    if ((!m_EnableAvSyncCorrection && pendingAudioMs > 30) ||
            (m_EnableAvSyncCorrection &&
             pendingAudioMs > MaximumPlankPendingAudioMs)) {
        m_RawAudioFrames += inputFrames;
        m_SkippedAudioBlocks++;
        return true;
    }

    // Provide backpressure on the queue to ensure too many frames don't build up
    // in SDL's audio queue, but don't wait forever to avoid a deadlock if the
    // audio device fails.
    for (int i = 0; i < 100; i++) {
        // Our device may enter a permanent error status upon removal, so we need
        // to recreate the audio device to pick up the new default audio device.
        if (SDL_GetAudioStreamDevice(m_AudioStream) == 0) {
            return false;
        }

        // Only queue more samples where there is 50 ms or less in SDL's queue
        if (SDL_GetAudioStreamQueued(m_AudioStream) / m_FrameSize * m_FrameDurationMs <= 50) {
            break;
        }

        SDL_Delay(1);
    }

    const void* queuedBuffer = m_AudioBuffer;
    int queuedBytes = bytesWritten;

#if defined(HAVE_FFMPEG) && defined(Q_OS_LINUX)
    if (m_EnableAvSyncCorrection && m_SwrContext != nullptr && inputFrames > 0) {
        const Uint32 now = SDL_GetTicks();
        const int backlogAudioMs = LiGetPendingAudioDuration();
        const auto correction = m_AudioRateController.update(
            m_RawAudioFrames,
            m_SubmittedAudioFrames,
            m_SampleRate,
            now,
            PlankAvSync::readVideoClock());
        const auto backlogCorrection = m_AudioBacklogController.update(
            backlogAudioMs,
            now);
        if (correction.updated || backlogCorrection.updated) {
            constexpr int CompensationSeconds = 1;
            const int compensationDistance = m_SampleRate * CompensationSeconds;
            const int appliedCorrectionPpm =
                correction.correctionPpm + backlogCorrection.correctionPpm;
            const int sampleDelta = static_cast<int>(std::llround(
                -static_cast<double>(appliedCorrectionPpm) *
                compensationDistance / 1000000.0));
            if (swr_set_compensation(m_SwrContext,
                                     sampleDelta,
                                     compensationDistance) < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to update PLANK audio correction");
            }
            else if (correction.updated &&
                     (m_RawAudioFrames / m_SampleRate) % 10 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK A/V audio correction: ppm=%d catchup=%d applied=%d delta=%d distance=%d",
                            correction.correctionPpm,
                            backlogCorrection.correctionPpm,
                            appliedCorrectionPpm,
                            sampleDelta,
                            compensationDistance);
            }
        }

        const int outputCapacity = swr_get_out_samples(m_SwrContext, inputFrames);
        m_CorrectedAudioBuffer.resize(
            static_cast<std::size_t>(outputCapacity) * m_ChannelCount);
        const uint8_t* inputPlanes[] = {
            reinterpret_cast<const uint8_t*>(m_AudioBuffer)
        };
        uint8_t* outputPlanes[] = {
            reinterpret_cast<uint8_t*>(m_CorrectedAudioBuffer.data())
        };
        const int outputFrames = swr_convert(m_SwrContext,
                                             outputPlanes,
                                             outputCapacity,
                                             inputPlanes,
                                             inputFrames);
        if (outputFrames < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PLANK audio correction failed");
            return false;
        }
        queuedBuffer = m_CorrectedAudioBuffer.data();
        queuedBytes = outputFrames * m_BytesPerSampleFrame;
    }
#endif

    m_RawAudioFrames += inputFrames;
    if (!SDL_PutAudioStreamData(m_AudioStream, queuedBuffer, queuedBytes)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to queue audio sample: %s",
                     SDL_GetError());
    }
    else if (m_EnableAvSyncCorrection) {
        m_LastSubmittedAudioMediaTimeMs =
            m_SubmittedAudioFrames * 1000 / m_SampleRate;
        m_SubmittedAudioFrames += queuedBytes / m_BytesPerSampleFrame;
    }

    return true;
}

int SdlAudioRenderer::getCapabilities()
{
    // Direct submit can't be used because we use LiGetPendingAudioDuration()
    return CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
}

int SdlAudioRenderer::getQueuedAudioDurationMs()
{
    if (m_AudioStream == nullptr || m_BytesPerSecond == 0) {
        return -1;
    }

    return static_cast<int>(SDL_GetAudioStreamQueued(m_AudioStream) * 1000ULL /
                            m_BytesPerSecond);
}

int SdlAudioRenderer::getDeviceBufferDurationMs()
{
    return m_DeviceBufferDurationMs;
}

qint64 SdlAudioRenderer::getSubmittedAudioMediaTimeMs()
{
    return m_LastSubmittedAudioMediaTimeMs;
}

int SdlAudioRenderer::getAudioClockCorrectionPpm()
{
    return m_AudioRateController.correctionPpm();
}

int SdlAudioRenderer::getAudioBacklogCorrectionPpm()
{
    return m_AudioBacklogController.correctionPpm();
}

quint64 SdlAudioRenderer::getSkippedAudioBlockCount()
{
    return m_SkippedAudioBlocks;
}
IAudioRenderer::AudioFormat SdlAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
