#include "videoframe.h"
#ifdef Q_OS_LINUX
#include "vaapivideoframe.h"
#endif
extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
}

#ifdef Q_OS_LINUX
QtVideoFrame::Ptr QtVideoFrame::hardware(QtVaapiVideoFrame::Ptr frame, Feedback feedback, Purpose purpose)
{
    if (!frame) return {};
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Size = frame->size();
    result->m_Hardware = std::move(frame);
    result->m_Feedback = std::move(feedback);
    result->m_Purpose = purpose;
    return result;
}
#endif

#ifdef Q_OS_MACOS
QtVideoFrame::Ptr QtVideoFrame::videoToolbox(const AVFrame* frame, AVPixelFormat expectedFormat,
                                          QString& error, Feedback feedback, Purpose purpose)
{
    error.clear();
    const auto* context = frame && frame->hw_frames_ctx ?
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
    if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !frame->data[3] || !frame->buf[0] ||
            !context || context->format != AV_PIX_FMT_VIDEOTOOLBOX || context->sw_format != expectedFormat ||
            frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384) {
        error = QStringLiteral("Qt Metal requires a retained exact-format VideoToolbox frame");
        return {};
    }
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Frame = av_frame_clone(frame);
    if (!result->m_Frame) { error = QStringLiteral("Could not retain the VideoToolbox frame"); return {}; }
    result->m_Size = QSize(frame->width, frame->height);
    result->m_Format = expectedFormat;
    result->m_Feedback = std::move(feedback);
    result->m_Purpose = purpose;
    return result;
}
#endif

QtVideoFrame::Ptr QtVideoFrame::software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error,
                                       Feedback feedback, Purpose purpose)
{
    error.clear();
    const auto* descriptor = av_pix_fmt_desc_get(expectedFormat);
    if (!frame || !descriptor || frame->format != expectedFormat ||
            frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384 ||
            frame->hw_frames_ctx || !frame->buf[0] || descriptor->nb_components != 3 ||
            !(descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) ||
            (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BE)) ||
            (descriptor->comp[0].depth != 8 && descriptor->comp[0].depth != 10)) {
        error = QStringLiteral("Qt video delivery requires a retained exact-format planar decoded frame");
        return {};
    }
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Frame = av_frame_clone(frame);
    result->m_Size = QSize(frame->width, frame->height);
    result->m_Format = expectedFormat;
    if (!result->m_Frame) {
        error = QStringLiteral("Could not retain the decoded Qt video frame"); return {};
    }
    result->m_Feedback = std::move(feedback);
    result->m_Purpose = purpose;
    return result;
}

void QtVideoFrame::presented(uint64_t renderMs) const
{
    if (!m_Presented.exchange(true) && m_Feedback) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_QueuedAt).count();
        m_Feedback(true, uint64_t(elapsed), renderMs);
    }
}

QtVideoFrame::~QtVideoFrame()
{
    if (!m_Presented.load() && m_Feedback) m_Feedback(false, 0, 0);
    av_frame_free(&m_Frame);
}
