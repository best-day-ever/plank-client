#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// Exact Linux-host NVENC 4:2:0 contract: H.264 High 8-bit or HEVC Main10,
// 4:2:0, limited-range BT.709 matrix and primaries, sRGB transfer. Never RGB
// identity or HDR. Inspects hardware storage without reading its pixels.
inline bool plankNvenc420FrameMatches(const AVFrame* frame, int codecProfile, bool hevcMain10)
{
    if (!frame || codecProfile != (hevcMain10 ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_H264_HIGH) ||
            frame->width <= 0 || frame->height <= 0 ||
            frame->width > 8192 || frame->height > 8192 ||
            frame->color_range != AVCOL_RANGE_MPEG ||
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
            descriptor->log2_chroma_w != 1 || descriptor->log2_chroma_h != 1 ||
            (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL))) {
        return false;
    }
    const int depth = hevcMain10 ? 10 : 8;
    for (int component = 0; component < 3; ++component) {
        if (descriptor->comp[component].depth != depth) {
            return false;
        }
    }
    return true;
}
