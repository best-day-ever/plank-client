#include "nvenc420profile.h"
#include "nvenc420-test-frame.h"

#include <cstdio>
#include <cstring>

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #expression); return 1; \
} } while (0)

namespace {

struct Fixture
{
    const char* name;
    AVCodecID codecId;
    const unsigned char* data;
    int size;
    bool hevcMain10;
    int profile;
    AVPixelFormat softwareFormat;
    AVPixelFormat hardwareStorage;
};

const Fixture kFixtures[] = {
    {"H.264 High 8-bit 4:2:0", AV_CODEC_ID_H264, k_Nvenc420H264High8TestFrame,
     k_Nvenc420H264High8TestFrameSize, false, AV_PROFILE_H264_HIGH, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12},
    {"HEVC Main10 4:2:0", AV_CODEC_ID_HEVC, k_Nvenc420HEVCMain10TestFrame,
     k_Nvenc420HEVCMain10TestFrameSize, true, AV_PROFILE_HEVC_MAIN_10, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE},
};

int decodeOne(const Fixture& fixture, AVCodecContext* context, AVFrame* frame)
{
    int checks = 0;
    auto* packet = av_packet_alloc();
    CHECK(packet);
    CHECK(av_new_packet(packet, fixture.size) == 0);
    std::memcpy(packet->data, fixture.data, fixture.size);
    CHECK(avcodec_send_packet(context, packet) == 0);
    CHECK(avcodec_send_packet(context, nullptr) == 0);
    CHECK(avcodec_receive_frame(context, frame) == 0);
    av_packet_free(&packet);
    return checks > 0 ? 0 : 1;
}

// Software decode plus the shared rejection function, as on every client.
int runSoftware(const Fixture& fixture)
{
    int checks = 0;
    const auto* codec = avcodec_find_decoder(fixture.codecId);
    CHECK(codec);
    auto* context = avcodec_alloc_context3(codec);
    CHECK(context);
    CHECK(avcodec_open2(context, codec, nullptr) == 0);
    auto* frame = av_frame_alloc();
    CHECK(frame);
    CHECK(decodeOne(fixture, context, frame) == 0);
    CHECK(frame->width == 1280 && frame->height == 720);
    CHECK(frame->format == fixture.softwareFormat);
    CHECK(context->profile == fixture.profile);
    CHECK(plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));

    // The other tuple, other profiles and every colour field are rejected.
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, !fixture.hevcMain10));
    CHECK(!plankNvenc420FrameMatches(nullptr, context->profile, fixture.hevcMain10));
    CHECK(!plankNvenc420FrameMatches(frame, fixture.hevcMain10 ? AV_PROFILE_HEVC_MAIN : AV_PROFILE_H264_MAIN, fixture.hevcMain10));
    CHECK(!plankNvenc420FrameMatches(frame, fixture.hevcMain10 ? AV_PROFILE_HEVC_REXT : AV_PROFILE_H264_HIGH_444_PREDICTIVE, fixture.hevcMain10));
    frame->color_range = AVCOL_RANGE_JPEG;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->color_range = AVCOL_RANGE_UNSPECIFIED;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->colorspace = AVCOL_SPC_RGB;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT2020;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->color_trc = AVCOL_TRC_IEC61966_2_1;
    CHECK(plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->format = fixture.hevcMain10 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10LE;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->format = fixture.hevcMain10 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->format = fixture.hevcMain10 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));

    // Storage-description checks only (no hardware here).
    frame->format = AV_PIX_FMT_VIDEOTOOLBOX;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    frame->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
    CHECK(frame->hw_frames_ctx);
    auto* storage = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    storage->sw_format = fixture.hardwareStorage;
    CHECK(plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    storage->sw_format = fixture.hevcMain10 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_P010LE;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    storage->sw_format = AV_PIX_FMT_NONE;
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    av_buffer_unref(&frame->hw_frames_ctx);

    av_frame_unref(frame);
    CHECK(avcodec_receive_frame(context, frame) == AVERROR_EOF);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    std::printf("%s software fixture/color contract: %d checks passed\n", fixture.name, checks);
    return 0;
}

#ifdef __APPLE__
AVPixelFormat chooseVideoToolbox(AVCodecContext*, const AVPixelFormat* formats)
{
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_VIDEOTOOLBOX) return *format;
    }
    return AV_PIX_FMT_NONE;
}

// Real hardware decode: VideoToolbox must return a hardware surface whose
// storage and colour metadata satisfy the exact contract.
int runVideoToolbox(const Fixture& fixture)
{
    int checks = 0;
    AVBufferRef* device = nullptr;
    if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) {
        std::printf("%s VideoToolbox: no hardware device, skipped\n", fixture.name);
        return 0;
    }
    const auto* codec = avcodec_find_decoder(fixture.codecId);
    CHECK(codec);
    auto* context = avcodec_alloc_context3(codec);
    CHECK(context);
    context->hw_device_ctx = av_buffer_ref(device);
    // Offer only the VideoToolbox surface; the checks below fail on software.
    context->get_format = chooseVideoToolbox;
    CHECK(avcodec_open2(context, codec, nullptr) == 0);
    auto* frame = av_frame_alloc();
    CHECK(frame);
    CHECK(decodeOne(fixture, context, frame) == 0);
    CHECK(frame->format == AV_PIX_FMT_VIDEOTOOLBOX);
    CHECK(frame->hw_frames_ctx);
    const auto* storage = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    CHECK(storage->sw_format == fixture.hardwareStorage);
    CHECK(frame->width == 1280 && frame->height == 720);
    CHECK(plankNvenc420FrameMatches(frame, context->profile, fixture.hevcMain10));
    CHECK(!plankNvenc420FrameMatches(frame, context->profile, !fixture.hevcMain10));
    av_frame_free(&frame);
    avcodec_free_context(&context);
    av_buffer_unref(&device);
    std::printf("%s VideoToolbox hardware decode (%s): %d checks passed\n",
                fixture.name, av_get_pix_fmt_name(fixture.hardwareStorage), checks);
    return 0;
}
#endif

}

int main()
{
    for (const Fixture& fixture : kFixtures) {
        if (runSoftware(fixture)) return 1;
#ifdef __APPLE__
        if (runVideoToolbox(fixture)) return 1;
#endif
    }
    return 0;
}
