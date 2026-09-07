#include "applevideoprofile.h"
#include "applevideo-test-frame.h"

#include <cstdio>
#include <cstring>

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #expression); return 1; \
} } while (0)

int main()
{
    int checks = 0;
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    CHECK(codec);
    auto* context = avcodec_alloc_context3(codec);
    CHECK(context);
    CHECK(avcodec_open2(context, codec, nullptr) == 0);
    auto* packet = av_packet_alloc();
    auto* frame = av_frame_alloc();
    CHECK(packet && frame);
    CHECK(av_new_packet(packet, k_AppleHEVCMain10TestFrameSize) == 0);
    std::memcpy(packet->data, k_AppleHEVCMain10TestFrame, k_AppleHEVCMain10TestFrameSize);
    CHECK(avcodec_send_packet(context, packet) == 0);
    CHECK(avcodec_send_packet(context, nullptr) == 0);
    CHECK(avcodec_receive_frame(context, frame) == 0);
    CHECK(frame->width == 3840 && frame->height == 2160);
    CHECK(frame->format == AV_PIX_FMT_YUV420P10LE);
    CHECK(plankAppleVideoFrameMatches(frame, context->profile));
    CHECK(!plankAppleVideoFrameMatches(nullptr, context->profile));
    CHECK(!plankAppleVideoFrameMatches(frame, AV_PROFILE_HEVC_MAIN));
    CHECK(!plankAppleVideoFrameMatches(frame, AV_PROFILE_HEVC_REXT));
    frame->color_range = AVCOL_RANGE_JPEG;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_UNSPECIFIED;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->color_trc = AVCOL_TRC_IEC61966_2_1;
    frame->format = AV_PIX_FMT_YUV420P;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->format = AV_PIX_FMT_YUV444P10LE;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    // Storage-description checks only: this is not a VA-API hardware test.
    frame->format = AV_PIX_FMT_VAAPI;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
    CHECK(frame->hw_frames_ctx);
    auto* hardware = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    hardware->sw_format = AV_PIX_FMT_P010LE;
    CHECK(plankAppleVideoFrameMatches(frame, context->profile));
    hardware->sw_format = AV_PIX_FMT_NV12;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    hardware->sw_format = AV_PIX_FMT_NONE;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    av_buffer_unref(&frame->hw_frames_ctx);
    frame->format = AV_PIX_FMT_YUV420P10LE;
    frame->width = 0;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->width = 3839;
    CHECK(!plankAppleVideoFrameMatches(frame, context->profile));
    frame->width = 3840;
    CHECK(plankAppleVideoFrameMatches(frame, context->profile));
    av_frame_unref(frame);
    CHECK(avcodec_receive_frame(context, frame) == AVERROR_EOF);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    std::printf("Apple Main10 fixture/color contract: %d checks passed\n", checks);
    return 0;
}
