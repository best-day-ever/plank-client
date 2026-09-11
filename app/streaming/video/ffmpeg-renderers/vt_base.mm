// Avoid the AVMediaType collision between Apple frameworks and FFmpeg.
#define AVMediaType AVMediaType_FFmpeg
#include "vt.h"
#undef AVMediaType
#import <VideoToolbox/VideoToolbox.h>

bool VTBaseRenderer::checkDecoderCapabilities(id<MTLDevice>, PDECODER_PARAMETERS params)
{
    CMVideoCodecType codec;
    if (params->videoFormat & VIDEO_FORMAT_MASK_H264) codec = kCMVideoCodecType_H264;
    else if (params->videoFormat & VIDEO_FORMAT_MASK_H265) codec = kCMVideoCodecType_HEVC;
    else return false;
    // Coarse rejection only. Shared initialization still decodes and validates
    // an exact-profile frame with hardware required. No GPU-name allowlist.
    return VTIsHardwareDecodeSupported(codec);
}
