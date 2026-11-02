#include "metalvideonode.h"
#include "../video/ffmpeg-renderers/vt_colors.h"
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <QFile>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <array>
extern "C" {
#include <libavutil/pixdesc.h>
}

namespace {
struct Vertex { simd_float4 position; simd_float2 uv; };

// Match the actual CV surface to the negotiated FFmpeg storage. Do not trust
// metadata alone or reinterpret an 8-bit/4:2:0 surface as the requested profile.
OSType cvFormat(AVPixelFormat format, bool full)
{
    switch (format) {
    case AV_PIX_FMT_NV12: return full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    case AV_PIX_FMT_NV16: return full ? kCVPixelFormatType_422YpCbCr8BiPlanarFullRange : kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange;
    case AV_PIX_FMT_NV24: return full ? kCVPixelFormatType_444YpCbCr8BiPlanarFullRange : kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange;
    case AV_PIX_FMT_P010LE: return full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    case AV_PIX_FMT_P210LE: return full ? kCVPixelFormatType_422YpCbCr10BiPlanarFullRange : kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange;
    case AV_PIX_FMT_P410LE: return full ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange : kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange;
    default: return 0;
    }
}

class MetalVideoNode final : public QSGRenderNode
{
public:
    MetalVideoNode(QQuickWindow* window, std::shared_ptr<QtVideoStatus> status)
        : m_Window(window), m_Status(std::move(status)) {}
    ~MetalVideoNode() override { releaseResources(); }
    bool hasStatus(const std::shared_ptr<QtVideoStatus>& status) const { return status == m_Status; }
    void synchronize(QtVideoFrame::Ptr frame, QtSessionGeometry geometry)
    {
        m_Frame = std::move(frame);
        m_Geometry = geometry;
        markDirty(DirtyMaterial);
    }
    QRectF rect() const override { return QRectF(QPointF(), m_Geometry.logicalSize); }
    RenderingFlags flags() const override { return BoundedRectRendering; }
    StateFlags changedStates() const override { return ViewportState | ScissorState | CullState; }

    void prepare() override
    {
        @autoreleasepool {
            if (!m_Frame || m_Status->error != QtVideoStatus::None || !m_Geometry.valid() ||
                    m_Frame->size() != m_Geometry.streamSize) return;
            auto* rif = m_Window->rendererInterface();
            if (rif->graphicsApi() != QSGRendererInterface::Metal) { fail(QtVideoStatus::WrongBackend); return; }
            if (!m_Device && !initialize((id<MTLDevice>)rif->getResource(m_Window, QSGRendererInterface::DeviceResource))) {
                fail(QtVideoStatus::ImportFailed); return;
            }
            if (m_Frame != m_Imported && !importFrame()) { fail(QtVideoStatus::ImportFailed); return; }
            m_Reduce = m_Geometry.sourcePixels.width() > 8.0 * m_Geometry.destinationPixels.width();
            if (m_Reduce && !reduceHorizontal()) fail(QtVideoStatus::PrecisionOrDrawFailed);
        }
    }

    void render(const RenderState*) override
    {
        @autoreleasepool {
            if (!m_Frame || m_Imported != m_Frame || m_Status->error != QtVideoStatus::None ||
                    !m_Geometry.valid() || m_Frame->size() != m_Geometry.streamSize) return;
            const auto started = std::chrono::steady_clock::now();
            auto encoder = (id<MTLRenderCommandEncoder>)m_Window->rendererInterface()->getResource(
                m_Window, QSGRendererInterface::CommandEncoderResource);
            if (!encoder) { fail(QtVideoStatus::WrongBackend); return; }
            const auto src = m_Geometry.sourcePixels;
            const auto dst = m_Geometry.destinationPixels;
            const auto size = m_Geometry.drawableSize;
            const float x0 = -1 + 2 * dst.left() / size.width(), x1 = -1 + 2 * dst.right() / size.width();
            const float y0 = 1 - 2 * dst.top() / size.height(), y1 = 1 - 2 * dst.bottom() / size.height();
            const float u0 = m_Reduce ? 0 : src.left() / m_Frame->size().width();
            const float u1 = m_Reduce ? 1 : src.right() / m_Frame->size().width();
            const float v0 = src.top() / m_Frame->size().height(), v1 = src.bottom() / m_Frame->size().height();
            const Vertex vertices[] = {{{x0,y0,0,1},{u0,v0}},{{x0,y1,0,1},{u0,v1}},
                                       {{x1,y0,0,1},{u1,v0}},{{x1,y1,0,1},{u1,v1}}};
            [encoder setViewport:MTLViewport{0,0,double(size.width()),double(size.height()),0,1}];
            [encoder setScissorRect:MTLScissorRect{0,0,NSUInteger(size.width()),NSUInteger(size.height())}];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setRenderPipelineState:m_Pipelines[m_PlaneCount - 2]];
            [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
            [encoder setFragmentBytes:&m_Color length:sizeof(m_Color) atIndex:0];
            for (int p = 0; p < m_PlaneCount; ++p)
                [encoder setFragmentTexture:m_Reduce ? m_Reduced[p] : m_Textures[p] atIndex:p];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            // Exercise the exact import/shader path before accepting a decoder,
            // then cover its fixture. Startup must not flash the test pattern.
            if (m_Frame->isQualification()) {
                [encoder setRenderPipelineState:m_ClearPipeline];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            ++m_Status->draws;
            QtVideoFrame::Ptr retired;
            {
                std::lock_guard<std::mutex> lock(m_Status->presentationMutex);
                retired = std::move(m_Status->drawnFrame);
                m_Status->drawnFrame = m_Frame;
                m_Status->drawStarted = started;
            }
        }
    }

    void releaseResources() override
    {
        // RenderControl completes the previous submission before its next
        // frame/invalidate. No decoder surface is returned while the GPU reads it.
        for (auto& cv : m_Cv) { if (cv) CFRelease(cv); cv = nullptr; }
        for (auto& t : m_Textures) { [t release]; t = nil; }
        for (auto& t : m_Reduced) { [t release]; t = nil; }
        for (auto& p : m_Pipelines) { [p release]; p = nil; }
        [m_ClearPipeline release]; m_ClearPipeline = nil;
        [m_Reduction release]; m_Reduction = nil;
        if (m_Cache) CFRelease(m_Cache);
        m_Cache = nullptr;
        [m_Device release]; m_Device = nil;
        m_Imported.reset();
    }
private:
    bool initialize(id<MTLDevice> device)
    {
        if (!device) return false;
        m_Device = [device retain];
        QFile source(QStringLiteral(":/qt-video/video.metal"));
        if (!source.open(QIODevice::ReadOnly)) return false;
        const auto bytes = source.readAll() + "\nfragment float4 qt_video_clear() { return float4(0,0,0,1); }\n";
        auto code = [[[NSString alloc] initWithBytes:bytes.constData() length:bytes.size() encoding:NSUTF8StringEncoding] autorelease];
        NSError* error = nil;
        auto library = [[device newLibraryWithSource:code options:nil error:&error] autorelease];
        if (!library) { qWarning("Qt Metal shader: %s", error.description.UTF8String); return false; }
        const std::array<NSString*,3> names = {@"ps_draw_biplanar", @"ps_draw_triplanar", @"qt_video_clear"};
        for (int index = 0; index < 3; ++index) {
            auto desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
            desc.vertexFunction = [[library newFunctionWithName:@"vs_draw"] autorelease];
            desc.fragmentFunction = [[library newFunctionWithName:names[index]] autorelease];
            desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGB10A2Unorm;
            auto pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
            if (!pipeline) { qWarning("Qt Metal pipeline: %s", error.description.UTF8String); return false; }
            if (index < 2) m_Pipelines[index] = pipeline; else m_ClearPipeline = pipeline;
        }
        auto function = [[library newFunctionWithName:@"cs_reduce_horizontal"] autorelease];
        m_Reduction = [device newComputePipelineStateWithFunction:function error:&error];
        return m_Reduction && CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &m_Cache) == kCVReturnSuccess;
    }

    bool importFrame()
    {
        const auto* frame = m_Frame->decodedFrame();
        const auto format = m_Frame->softwareFormat();
        const auto* desc = av_pix_fmt_desc_get(format);
        if (!frame || !desc || desc->nb_components != 3 || (desc->comp[0].depth != 8 && desc->comp[0].depth != 10)) return false;
        m_PlaneCount = av_pix_fmt_count_planes(format);
        if (m_PlaneCount != 2 && m_PlaneCount != 3) return false;
        PlankVTMatrix matrix;
        switch (frame->colorspace) {
        case AVCOL_SPC_RGB: matrix = PlankVTMatrix::IdentityGbr; break;
        case AVCOL_SPC_BT709: matrix = PlankVTMatrix::Bt709; break;
        case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_BT470BG: matrix = PlankVTMatrix::Bt601; break;
        case AVCOL_SPC_BT2020_NCL: matrix = PlankVTMatrix::Bt2020; break;
        default: return false;
        }
        const bool full = frame->color_range == AVCOL_RANGE_JPEG;
        if (!full && frame->color_range != AVCOL_RANGE_MPEG) return false;
        m_Color = plankVTColorParams(matrix, full, desc->comp[0].depth, desc->comp[0].shift > 0);
        const bool hardware = frame->format == AV_PIX_FMT_VIDEOTOOLBOX;
        auto buffer = hardware ? (CVPixelBufferRef)frame->data[3] : nullptr;
        if (hardware && (!buffer || !cvFormat(format, full) ||
                CVPixelBufferGetPixelFormatType(buffer) != cvFormat(format, full) ||
                CVPixelBufferGetPlaneCount(buffer) != size_t(m_PlaneCount))) return false;
        for (int p = 0; p < m_PlaneCount; ++p) {
            const int width = p ? AV_CEIL_RSHIFT(frame->width, desc->log2_chroma_w) : frame->width;
            const int height = p ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
            const bool pair = m_PlaneCount == 2 && p == 1;
            const MTLPixelFormat pixelFormat = desc->comp[0].depth == 10 ?
                (pair ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm) :
                (pair ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
            if (m_Cv[p]) { CFRelease(m_Cv[p]); m_Cv[p] = nullptr; }
            if (hardware) {
                [m_Textures[p] release]; m_Textures[p] = nil;
                if (CVPixelBufferGetWidthOfPlane(buffer,p) != size_t(width) ||
                        CVPixelBufferGetHeightOfPlane(buffer,p) != size_t(height) ||
                        CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, m_Cache, buffer, nullptr,
                            pixelFormat, width, height, p, &m_Cv[p]) != kCVReturnSuccess) return false;
                m_Textures[p] = [CVMetalTextureGetTexture(m_Cv[p]) retain];
            } else {
                if (!frame->data[p] || frame->linesize[p] < width * (pair ? 2 : 1) * (desc->comp[0].depth == 10 ? 2 : 1)) return false;
                auto& texture = m_Textures[p];
                if (texture && (texture.width != NSUInteger(width) || texture.height != NSUInteger(height) || texture.pixelFormat != pixelFormat)) {
                    [texture release]; texture = nil;
                }
                if (!texture) {
                    auto spec = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:width height:height mipmapped:NO];
                    spec.storageMode = MTLStorageModeShared; spec.usage = MTLTextureUsageShaderRead;
                    texture = [m_Device newTextureWithDescriptor:spec];
                }
                if (!texture) return false;
                [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:frame->data[p] bytesPerRow:frame->linesize[p]];
            }
            if (!m_Textures[p]) return false;
        }
        m_Imported = m_Frame;
        ++m_Status->imports;
        return true;
    }

    bool reduceHorizontal()
    {
        auto command = (id<MTLCommandBuffer>)m_Window->rendererInterface()->getResource(m_Window, QSGRendererInterface::CommandListResource);
        if (!command) return false;
        const auto source = m_Geometry.sourcePixels;
        const simd_float2 crop = {float(source.left() / m_Frame->size().width()), float(source.right() / m_Frame->size().width())};
        const int width = qMax(1, qRound(m_Geometry.destinationPixels.width()));
        for (int p = 0; p < m_PlaneCount; ++p) {
            auto& texture = m_Reduced[p];
            if (texture && (texture.width != NSUInteger(width) || texture.height != m_Textures[p].height)) { [texture release]; texture = nil; }
            if (!texture) {
                auto spec = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:width height:m_Textures[p].height mipmapped:NO];
                spec.storageMode = MTLStorageModePrivate; spec.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                texture = [m_Device newTextureWithDescriptor:spec];
            }
            if (!texture) return false;
            auto encoder = [command computeCommandEncoder];
            if (!encoder) return false;
            [encoder setComputePipelineState:m_Reduction];
            [encoder setTexture:m_Textures[p] atIndex:0]; [encoder setTexture:texture atIndex:1];
            [encoder setBytes:&crop length:sizeof(crop) atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(texture.width,texture.height,1) threadsPerThreadgroup:MTLSizeMake(m_Reduction.threadExecutionWidth,1,1)];
            [encoder endEncoding];
        }
        return true;
    }
    void fail(QtVideoStatus::Error error)
    {
        auto expected = QtVideoStatus::None;
        if (m_Status->error.compare_exchange_strong(expected,error)) qWarning("Qt Metal exact-format presentation failed (%d)", int(error));
        std::lock_guard<std::mutex> lock(m_Status->presentationMutex);
        m_Status->presentationChanged.notify_all();
    }
    QQuickWindow* const m_Window;
    std::shared_ptr<QtVideoStatus> m_Status;
    QtVideoFrame::Ptr m_Frame, m_Imported;
    QtSessionGeometry m_Geometry;
    id<MTLDevice> m_Device = nil;
    CVMetalTextureCacheRef m_Cache = nullptr;
    std::array<CVMetalTextureRef,3> m_Cv{};
    std::array<id<MTLTexture>,3> m_Textures{}, m_Reduced{};
    std::array<id<MTLRenderPipelineState>,2> m_Pipelines{};
    id<MTLRenderPipelineState> m_ClearPipeline = nil;
    id<MTLComputePipelineState> m_Reduction = nil;
    PlankVTColorParams m_Color{};
    int m_PlaneCount = 0;
    bool m_Reduce = false;
};
}

QSGNode* QtMetalVideoItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    ++m_Status->synchronizations;
    auto* node = static_cast<MetalVideoNode*>(oldNode);
    if (node && !node->hasStatus(m_Status)) { delete node; node = nullptr; }
    if (!node) node = new MetalVideoNode(window(), m_Status);
    node->synchronize(m_Frame, m_Geometry);
    return node;
}
