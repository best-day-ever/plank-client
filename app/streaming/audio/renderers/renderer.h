#pragma once

#include <Limelight.h>
#include <QtGlobal>

class IAudioRenderer
{
public:
    virtual ~IAudioRenderer() {}

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig) = 0;

    virtual void* getAudioBuffer(int* size) = 0;

    // Return false if an unrecoverable error has occurred and the renderer must be reinitialized
    virtual bool submitAudio(int bytesWritten) = 0;

    virtual int getCapabilities() = 0;

    // Return the renderer-managed queue duration, or -1 when unavailable.
    virtual int getQueuedAudioDurationMs() {
        return -1;
    }

    // Return the output device buffer duration, or -1 when unavailable.
    virtual int getDeviceBufferDurationMs() {
        return -1;
    }

    // Return the start time of the most recently submitted corrected audio
    // block, or -1 when the renderer does not maintain a corrected clock.
    virtual qint64 getSubmittedAudioMediaTimeMs() {
        return -1;
    }

    virtual int getAudioClockCorrectionPpm() {
        return 0;
    }

    virtual int getAudioBacklogCorrectionPpm() {
        return 0;
    }

    virtual quint64 getSkippedAudioBlockCount() {
        return 0;
    }
    virtual void remapChannels(POPUS_MULTISTREAM_CONFIGURATION) {
        // Use default channel mapping:
        // 0 - Front Left
        // 1 - Front Right
        // 2 - Center
        // 3 - LFE
        // 4 - Surround Left
        // 5 - Surround Right
    }

    enum class AudioFormat {
        Sint16NE,  // 16-bit signed integer (native endian)
        Float32NE, // 32-bit floating point (native endian)
    };
    virtual AudioFormat getAudioBufferFormat() = 0;

    int getAudioBufferSampleSize() {
        switch (getAudioBufferFormat()) {
        case IAudioRenderer::AudioFormat::Sint16NE:
            return sizeof(short);
        case IAudioRenderer::AudioFormat::Float32NE:
            return sizeof(float);
        default:
            Q_UNREACHABLE();
        }
    }
};
