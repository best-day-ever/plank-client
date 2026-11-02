#pragma once

#include "../video/ffmpeg-renderers/renderer.h"
#ifdef Q_OS_LINUX
#include "softwarevideodevice.h"
#endif
#include "decodedvideodelivery.h"
#include <functional>

// Reuses FFmpegVideoDecoder's exact-profile fixtures, hardware-first selection,
// worker, transport and recovery. Only the presentation backend changes.
class QtDecodedVideoRendererFactory final : public IFFmpegRendererFactory
{
public:
    using OpenDelivery = std::function<QtDecodedVideoDelivery()>;
#ifdef Q_OS_MACOS
    explicit QtDecodedVideoRendererFactory(OpenDelivery openDelivery);
#else
    QtDecodedVideoRendererFactory(QtSoftwareVideoDevice::Ptr softwareDevice,
                                  QString vaapiRenderNode, OpenDelivery openDelivery);
#endif
    IFFmpegRenderer* createRenderer(const AVCodecHWConfig* hardware) override;

private:
#ifdef Q_OS_LINUX
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice;
    const QString m_VaapiRenderNode;
#endif
    const OpenDelivery m_OpenDelivery;
};
