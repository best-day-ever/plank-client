#pragma once

#include "decoder.h"

// This is a storage/presentation policy, not a decoder capability claim.
// The real decoded fixture must still pass exact Apple profile validation.
inline bool plankUsesPackedBt709(const DECODER_PARAMETERS* params)
{
    return params->captureSource == DecoderCaptureSource::ScreenCaptureKit &&
            params->encoderBackend == DecoderEncoderBackend::VideoToolbox &&
            params->videoFormat == VIDEO_FORMAT_H265_REXT10_444 &&
            !params->enableIdentityGbr;
}
