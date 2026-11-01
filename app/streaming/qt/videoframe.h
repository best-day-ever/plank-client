#pragma once

#include <QSize>
#include <QString>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class QtVaapiVideoFrame;

// Immutable retained decode result. One payload type crosses the coalescing
// mailbox for either decoder path; no queued pixel copies or raw AVFrame borrows.
class QtVideoFrame final
{
public:
    using Ptr = std::shared_ptr<const QtVideoFrame>;
    enum class Purpose { Live, DecoderQualification };
    // At most one completion, from the primary output after frameSwapped.
    // Destruction without presentation reports replacement/discard. The
    // callback must be thread-safe and must not retain this frame.
    using Feedback = std::function<void(bool presented, uint64_t queuedMs, uint64_t renderMs)>;
    static Ptr hardware(std::shared_ptr<const QtVaapiVideoFrame> frame, Feedback feedback = {}, Purpose purpose = Purpose::Live);
#ifdef Q_OS_MACOS
    static Ptr videoToolbox(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error,
                           Feedback feedback = {}, Purpose purpose = Purpose::Live);
#endif
    static Ptr software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error,
                        Feedback feedback = {}, Purpose purpose = Purpose::Live);
    ~QtVideoFrame();
    QtVideoFrame(const QtVideoFrame&) = delete;
    QtVideoFrame& operator=(const QtVideoFrame&) = delete;

    QSize size() const { return m_Size; }
    std::shared_ptr<const QtVaapiVideoFrame> hardwareFrame() const { return m_Hardware; }
    const AVFrame* softwareFrame() const { return m_Frame && !m_Frame->hw_frames_ctx ? m_Frame : nullptr; }
    const AVFrame* decodedFrame() const { return m_Frame; }
    AVPixelFormat softwareFormat() const { return m_Format; }
    bool isQualification() const { return m_Purpose == Purpose::DecoderQualification; }
    void presented(uint64_t renderMs) const;

private:
    QtVideoFrame() = default;
    std::shared_ptr<const QtVaapiVideoFrame> m_Hardware;
    AVFrame* m_Frame = nullptr;
    QSize m_Size;
    AVPixelFormat m_Format = AV_PIX_FMT_NONE;
    Purpose m_Purpose = Purpose::Live;
    Feedback m_Feedback;
    const std::chrono::steady_clock::time_point m_QueuedAt = std::chrono::steady_clock::now();
    mutable std::atomic<bool> m_Presented{false};
};
