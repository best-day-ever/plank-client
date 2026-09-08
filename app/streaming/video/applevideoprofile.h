#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// Explicit Apple Main10/RExt10 YCbCr contracts, not RGB identity or HDR.
// Inspect hardware storage without transferring its pixels back to the CPU.
inline bool plankAppleVideoFrameMatches(const AVFrame* frame, int codecProfile, bool fullChroma)
{
    if (!frame || codecProfile != (fullChroma ? AV_PROFILE_HEVC_REXT : AV_PROFILE_HEVC_MAIN_10) ||
            frame->width <= 0 || frame->height <= 0 ||
            frame->width > 8192 || frame->height > 8192 ||
            (frame->width & 1) || (frame->height & 1) ||
            frame->color_range != AVCOL_RANGE_JPEG ||
            frame->colorspace != AVCOL_SPC_BT709 ||
            frame->color_primaries != AVCOL_PRI_BT709 ||
            frame->color_trc != AVCOL_TRC_IEC61966_2_1) {
        return false;
    }
    auto format = static_cast<AVPixelFormat>(frame->format);
    if (frame->hw_frames_ctx) {
        if (frame->hw_frames_ctx->size < sizeof(AVHWFramesContext) ||
                !frame->hw_frames_ctx->data) {
            return false;
        }
        format = reinterpret_cast<const AVHWFramesContext*>(
                    frame->hw_frames_ctx->data)->sw_format;
    }
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || descriptor->nb_components != 3 ||
            descriptor->log2_chroma_w != (fullChroma ? 0 : 1) || descriptor->log2_chroma_h != (fullChroma ? 0 : 1) ||
            (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL))) {
        return false;
    }
    for (int component = 0; component < 3; ++component) {
        if (descriptor->comp[component].depth != 10) {
            return false;
        }
    }
    return true;
}
