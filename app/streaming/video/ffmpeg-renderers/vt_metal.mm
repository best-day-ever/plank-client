// Nasty hack to avoid conflict between AVFoundation and
// libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "vt.h"
#undef AVMediaType

#include <SDL3/SDL_system.h>
#include <Limelight.h>
#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "path.h"

#import <Cocoa/Cocoa.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <dispatch/dispatch.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

extern "C" {
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
}

#include "vt_colors.h"
#include "streaming/plankpresentation.h"

#include <atomic>
#include <memory>
#include <vector>

#include <array>
#include <memory>
#include <vector>

struct Vertex
{
    vector_float4 position;
    vector_float2 texCoord;
};

#define MAX_VIDEO_PLANES 3

// A drawable may report presentation after the renderer has been destroyed.
// Callbacks retain only this state, never the renderer or its SDL windows.
struct MetalPresentationPacer
{
    SDL_Mutex* mutex = SDL_CreateMutex();
    SDL_Condition* condition = SDL_CreateCondition();
    int pending = 0;
    ~MetalPresentationPacer()
    {
        SDL_DestroyCondition(condition);
        SDL_DestroyMutex(mutex);
    }
};

struct MetalPresentationTarget
{
    PlankPresentationOutput output;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable = nil;
    id<MTLBuffer> vertices = nil;
    bool visible = false;
    QSize frameSize;
    QSize drawableSize;

    ~MetalPresentationTarget()
    {
        [drawable release];
        [vertices release];
        if (view) SDL_Metal_DestroyView(view);
    }
};

struct MetalFrameTextures
{
    std::array<CVMetalTextureRef, MAX_VIDEO_PLANES> cv {};
    ~MetalFrameTextures()
    {
        for (auto texture : cv) if (texture) CFRelease(texture);
    }
};

class VTMetalRenderer : public VTBaseRenderer
{
    friend class VTMetalRendererProbe;
public:
    VTMetalRenderer(bool hwAccel)
        : m_HwAccel(hwAccel),
          m_HwContext(nullptr),
          m_MetalLayer(nullptr),
          m_TextureCache(nullptr),
          m_CscParamsBuffer(nullptr),
          m_OverlayTextures{},
          m_OverlayLock(0),
          m_VideoPipelineState(nullptr),
          m_OverlayPipelineState(nullptr),
          m_ShaderLibrary(nullptr),
          m_CommandQueue(nullptr),
          m_SwMappingTextures{},
          m_LastColorSpace(-1),
          m_LastFullRange(false),
          m_Pacer(std::make_shared<MetalPresentationPacer>())
    {
    }

    virtual ~VTMetalRenderer() override
    { @autoreleasepool {
        for (auto& target : m_Secondaries) {
            if (target->vertexBuffer != nullptr) {
                [target->vertexBuffer release];
            }
            if (target->view != nullptr) {
                SDL_Metal_DestroyView(target->view);
            }
        }
        m_Secondaries.clear();

        if (m_PresentationCond != nullptr) {
            SDL_DestroyCondition(m_PresentationCond);
        }

        if (m_PresentationMutex != nullptr) {
            SDL_DestroyMutex(m_PresentationMutex);
        }

        if (m_HwContext != nullptr) {
            av_buffer_unref(&m_HwContext);
        }

        if (m_CscParamsBuffer != nullptr) {
            [m_CscParamsBuffer release];
        }

        if (m_VideoVertexBuffer != nullptr) {
            [m_VideoVertexBuffer release];
        }

        if (m_VideoPipelineState != nullptr) {
            [m_VideoPipelineState release];
        }

        for (int i = 0; i < Overlay::OverlayMax; i++) {
            if (m_OverlayTextures[i] != nullptr) {
                [m_OverlayTextures[i] release];
            }
        }

        for (int i = 0; i < MAX_VIDEO_PLANES; i++) {
            if (m_SwMappingTextures[i] != nullptr) {
                [m_SwMappingTextures[i] release];
            }
        }

        if (m_OverlayPipelineState != nullptr) {
            [m_OverlayPipelineState release];
        }

        if (m_ShaderLibrary != nullptr) {
            [m_ShaderLibrary release];
        }

        if (m_CommandQueue != nullptr) {
            [m_CommandQueue release];
        }

        if (m_TextureCache != nullptr) {
            CFRelease(m_TextureCache);
        }

        if (m_MetalView != nullptr) {
            SDL_Metal_DestroyView(m_MetalView);
        }
    }}

    void discardNextDrawable()
    { @autoreleasepool {
        for (auto& target : m_Targets) {
            [target->drawable release];
            target->drawable = nil;
        }
    }}

    virtual void waitToRender() override
    { @autoreleasepool {
        SDL_LockMutex(m_Pacer->mutex);
        if (m_Pacer->pending > 2) {
            SDL_WaitConditionTimeout(m_Pacer->condition, m_Pacer->mutex, 100);
        }
        SDL_UnlockMutex(m_Pacer->mutex);
        for (auto& target : m_Targets) {
            if (!target->drawable) target->drawable = [[target->layer nextDrawable] retain];
            if (!target->drawable) {
                // Keep both outputs on the same decoded frame if an output is
                // temporarily hidden/unavailable. Do not retain the other one.
                discardNextDrawable();
                return;
            }
        }
    }}

    virtual void cleanupRenderContext() override
    {
        discardNextDrawable();
    }

    // The video quad for one window: sourceRect of the frame (the whole frame
    // when invalid) aspect-fitted into the window's drawable.
    bool updateVertices(id<MTLBuffer>& buffer, int& lastFrameWidth, int& lastFrameHeight,
                        int& lastDrawableWidth, int& lastDrawableHeight,
                        SDL_Window* window, const QRectF& sourceRect, AVFrame* frame)
    {
        int drawableWidth, drawableHeight;
        if (!SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight))
            return false;

        // Check if anything has changed since the last vertex buffer upload
        if (buffer &&
                frame->width == lastFrameWidth && frame->height == lastFrameHeight &&
                drawableWidth == lastDrawableWidth && drawableHeight == lastDrawableHeight) {
            // Nothing to do
            return true;

        QRectF source(0, 0, frame->width, frame->height);
        if (sourceRect.isValid()) {
            source = sourceRect.intersected(source);
            if (source.isEmpty()) {
                return false;
            }
        }

        // Determine the correct scaled size for the video region
        SDL_Rect src, dst;
        src.x = src.y = 0;
        src.w = qMax(1, qRound(source.width()));
        src.h = qMax(1, qRound(source.height()));
        dst.x = dst.y = 0;
        dst.w = drawableWidth;
        dst.h = drawableHeight;
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // Convert screen space to normalized device coordinates
        SDL_FRect renderRect;
        StreamUtils::screenSpaceToNormalizedDeviceCoords(&dst, &renderRect, drawableWidth, drawableHeight);

        const float u0 = source.left() / frame->width;
        const float u1 = source.right() / frame->width;
        const float vTop = source.top() / frame->height;
        const float vBottom = source.bottom() / frame->height;
        Vertex verts[] =
        {
            { { renderRect.x, renderRect.y, 0.0f, 1.0f }, { u0, vBottom } },
            { { renderRect.x, renderRect.y+renderRect.h, 0.0f, 1.0f }, { u0, vTop } },
            { { renderRect.x+renderRect.w, renderRect.y, 0.0f, 1.0f }, { u1, vBottom } },
            { { renderRect.x+renderRect.w, renderRect.y+renderRect.h, 0.0f, 1.0f }, { u1, vTop } },
        };

        [buffer release];
        auto bufferOptions = MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged;
        buffer = [m_MetalLayer.device newBufferWithBytes:verts length:sizeof(verts) options:bufferOptions];
        if (!buffer) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create video vertex buffer");
            return false;
        }

        lastFrameWidth = frame->width;
        lastFrameHeight = frame->height;
        lastDrawableWidth = drawableWidth;
        lastDrawableHeight = drawableHeight;

        return true;
    }

    bool updateVideoRegionSizeForFrame(AVFrame* frame)
    {
        return updateVertices(m_VideoVertexBuffer, m_LastFrameWidth, m_LastFrameHeight,
                              m_LastDrawableWidth, m_LastDrawableHeight,
                              m_Window, m_PrimarySourceRect, frame);
    }

    int getFramePlaneCount(AVFrame* frame)
    {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            return CVPixelBufferGetPlaneCount((CVPixelBufferRef)frame->data[3]);
        }
        else {
            return av_pix_fmt_count_planes((AVPixelFormat)frame->format);
        }
    }


    bool updateColorSpaceForFrame(AVFrame* frame)
    {
        int colorspace = getFrameColorspace(frame);
        bool fullRange = isFrameFullRange(frame);
        if (colorspace != m_LastColorSpace || fullRange != m_LastFullRange) {
            discardNextDrawable();
            AVPixelFormat storage = (AVPixelFormat)frame->format;
            if (frame->hw_frames_ctx)
                storage = ((AVHWFramesContext*)frame->hw_frames_ctx->data)->sw_format;
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(storage);
            if (!descriptor || (descriptor->comp[0].depth != 8 && descriptor->comp[0].depth != 10))
                return false;
            const int depth = descriptor->comp[0].depth;
            const bool highBits = descriptor->comp[0].shift > 0;
            PlankVTMatrix matrix = PlankVTMatrix::Bt709;
            CFStringRef colorSpaceName = frame->color_trc == AVCOL_TRC_IEC61966_2_1 ?
                    kCGColorSpaceSRGB : kCGColorSpaceITUR_709;
            if (colorspace == COLORSPACE_IDENTITY_GBR) {
                matrix = PlankVTMatrix::IdentityGbr;
                colorSpaceName = kCGColorSpaceSRGB;
            } else if (colorspace == COLORSPACE_REC_601) {
                matrix = PlankVTMatrix::Bt601;
                colorSpaceName = kCGColorSpaceSRGB;
            } else if (colorspace == COLORSPACE_REC_2020) {
                matrix = PlankVTMatrix::Bt2020;
                colorSpaceName = kCGColorSpaceITUR_2020;
            }
            const auto paramBuffer = plankVTColorParams(matrix, fullRange, depth, highBits);
            CGColorSpaceRef newColorSpace = CGColorSpaceCreateWithName(colorSpaceName);
            m_MetalLayer.colorspace = newColorSpace;
            m_MetalLayer.pixelFormat = depth == 10 ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
            for (auto& target : m_Secondaries) {
                target->layer.colorspace = newColorSpace;
                target->layer.pixelFormat = m_MetalLayer.pixelFormat;
            }
            CGColorSpaceRelease(newColorSpace);

            // Create the new colorspace parameter buffer for our fragment shader
            [m_CscParamsBuffer release];
            auto bufferOptions = MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged;
            m_CscParamsBuffer = [m_MetalLayer.device newBufferWithBytes:(void*)&paramBuffer length:sizeof(paramBuffer) options:bufferOptions];
            if (!m_CscParamsBuffer) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create CSC parameters buffer");
                return false;
            }

            int planes = getFramePlaneCount(frame);
            SDL_assert(planes == 2 || planes == 3);

            MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
            pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
            pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:planes == 2 ? @"ps_draw_biplanar" : @"ps_draw_triplanar"] autorelease];
            pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
            [m_VideoPipelineState release];
            m_VideoPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
            if (!m_VideoPipelineState) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create video pipeline state");
                return false;
            }

            pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
            pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
            pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:@"ps_draw_rgb"] autorelease];
            pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
            pipelineDesc.colorAttachments[0].blendingEnabled = YES;
            pipelineDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            pipelineDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
            pipelineDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pipelineDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            pipelineDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pipelineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            [m_OverlayPipelineState release];
            m_OverlayPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
            if (!m_OverlayPipelineState) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create overlay pipeline state");
                return false;
            }

            m_LastColorSpace = colorspace;
            m_LastFullRange = fullRange;
        }

        return true;
    }

    id<MTLTexture> mapPlaneForSoftwareFrame(AVFrame* frame, int planeIndex)
    {
        const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        if (!formatDesc) {
            // This shouldn't be possible but handle it anyway
            SDL_assert(formatDesc);
            return nil;
        }

        SDL_assert(planeIndex < MAX_VIDEO_PLANES);

        NSUInteger planeWidth = planeIndex ? AV_CEIL_RSHIFT(frame->width, formatDesc->log2_chroma_w) : frame->width;
        NSUInteger planeHeight = planeIndex ? AV_CEIL_RSHIFT(frame->height, formatDesc->log2_chroma_h) : frame->height;

        // Recreate the texture if the plane size changes
        if (m_SwMappingTextures[planeIndex] && (m_SwMappingTextures[planeIndex].width != planeWidth ||
                                                m_SwMappingTextures[planeIndex].height != planeHeight)) {
            [m_SwMappingTextures[planeIndex] release];
            m_SwMappingTextures[planeIndex] = nil;
        }

        if (!m_SwMappingTextures[planeIndex]) {
            MTLPixelFormat metalFormat;

            switch (formatDesc->comp[planeIndex].step) {
            case 1:
                metalFormat = MTLPixelFormatR8Unorm;
                break;
            case 2:
                metalFormat = MTLPixelFormatR16Unorm;
                break;
            default:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unhandled plane step: %d (plane: %d)",
                             formatDesc->comp[planeIndex].step,
                             planeIndex);
                SDL_assert(false);
                return nil;
            }

            auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalFormat
                                                                              width:planeWidth
                                                                             height:planeHeight
                                                                          mipmapped:NO];
            texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
            texDesc.storageMode = MTLStorageModeManaged;
            texDesc.usage = MTLTextureUsageShaderRead;

            m_SwMappingTextures[planeIndex] = [m_MetalLayer.device newTextureWithDescriptor:texDesc];
            if (!m_SwMappingTextures[planeIndex]) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to allocate software frame texture");
                return nil;
            }
        }

        [m_SwMappingTextures[planeIndex] replaceRegion:MTLRegionMake2D(0, 0, planeWidth, planeHeight)
                                           mipmapLevel:0
                                             withBytes:frame->data[planeIndex]
                                           bytesPerRow:frame->linesize[planeIndex]];

        return m_SwMappingTextures[planeIndex];
    }

    // Caller frees frame after we return
    virtual void renderFrame(AVFrame* frame) override
    { @autoreleasepool {
        // Handle changes to the frame's colorspace from last time we rendered
        if (!updateColorSpaceForFrame(frame)) {
            // Trigger the main thread to recreate the decoder
            SDL_Event event;
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
            return;
        }

        for (auto& target : m_Targets) {
            if (!updateVideoRegionSizeForFrame(*target, frame)) {
                SDL_Event event = {};
                event.type = SDL_EVENT_RENDER_DEVICE_RESET;
                SDL_PushEvent(&event);
                return;
            }
            if (!target->drawable) return;
        }

        MetalFrameTextures textures;
        size_t planes = getFramePlaneCount(frame);
        SDL_assert(planes <= MAX_VIDEO_PLANES);

        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            CVPixelBufferRef pixBuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);

            // Create Metal textures for the planes of the CVPixelBuffer
            for (size_t i = 0; i < planes; i++) {
                MTLPixelFormat fmt;

                switch (CVPixelBufferGetPixelFormatType(pixBuf)) {
                case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
                    fmt = (i == 0) ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
                    break;

                case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
                case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
                    fmt = (i == 0) ? MTLPixelFormatR16Unorm : MTLPixelFormatRG16Unorm;
                    break;

                default:
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Unknown pixel format: %x",
                                 CVPixelBufferGetPixelFormatType(pixBuf));
                    return;
                }

                CVReturn err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, m_TextureCache, pixBuf, nullptr, fmt,
                                                                         CVPixelBufferGetWidthOfPlane(pixBuf, i),
                                                                         CVPixelBufferGetHeightOfPlane(pixBuf, i),
                                                                         i,
                                                                         &textures.cv[i]);
                if (err != kCVReturnSuccess) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "CVMetalTextureCacheCreateTextureFromImage() failed: %d",
                                 err);
                    return;
                }
            }
        }

        // The frame's planes as Metal textures, shared by every window.
        std::array<id<MTLTexture>, MAX_VIDEO_PLANES> planeTextures {};
        for (size_t i = 0; i < planes; i++) {
            planeTextures[i] = frame->format == AV_PIX_FMT_VIDEOTOOLBOX ?
                        CVMetalTextureGetTexture(cvMetalTextures[i]) : mapPlaneForSoftwareFrame(frame, i);
        }

        auto commandBuffer = [m_CommandQueue commandBuffer];
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                // Free textures after completion of rendering per CVMetalTextureCache requirements
                for (size_t i = 0; i < planes; i++) {
                    CFRelease(cvMetalTextures[i]);
                }
            }];
        }

        // One render pass per window, all in this command buffer: the video
        // quad, and on the primary window the overlays and toolbar.
        const auto encodeVideo = [&](id<MTLTexture> targetTexture, id<MTLBuffer> vertices) {
            auto passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
            passDescriptor.colorAttachments[0].texture = targetTexture;
            passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
            passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
            auto encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];
            [encoder setRenderPipelineState:m_VideoPipelineState];
            for (size_t i = 0; i < planes; i++) {
                [encoder setFragmentTexture:planeTextures[i] atIndex:i];
            }
            [encoder setFragmentBuffer:m_CscParamsBuffer offset:0 atIndex:0];
            [encoder setVertexBuffer:vertices offset:0 atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            return encoder;
        };

        for (auto& target : m_Secondaries) {
            // Pacing follows the primary: a window whose drawables are all
            // still in flight (a slower display) skips this frame.
            if (target->inFlight->load() >= (int)target->layer.maximumDrawableCount) {
                continue;
            }
            if (!updateVertices(target->vertexBuffer, target->lastFrameWidth, target->lastFrameHeight,
                                target->lastDrawableWidth, target->lastDrawableHeight,
                                target->window, target->sourceRect, frame)) {
                continue;
            }
            id<CAMetalDrawable> drawable = [target->layer nextDrawable];
            if (drawable == nil) {
                continue;
            }
            [encodeVideo(drawable.texture, target->vertexBuffer) endEncoding];
            target->inFlight->fetch_add(1);
            std::shared_ptr<std::atomic<int>> inFlight = target->inFlight;
            [drawable addPresentedHandler:^(id<MTLDrawable>) {
                inFlight->fetch_sub(1);
            }];
            [commandBuffer presentDrawable:drawable];
        }

        auto renderEncoder = encodeVideo(m_NextDrawable.texture, m_VideoVertexBuffer);

            // Only one output paces a frame; the secondary output must not double
            // the in-flight count. A late callback cannot touch a deleted renderer.
            if (target->output.primary && target->layer.displaySyncEnabled) {
                auto pacer = m_Pacer;
                SDL_LockMutex(pacer->mutex);
                pacer->pending++;
                SDL_UnlockMutex(pacer->mutex);
                [target->drawable addPresentedHandler:^(id<MTLDrawable>) {
                    SDL_LockMutex(pacer->mutex);
                    pacer->pending--;
                    SDL_SignalCondition(pacer->condition);
                    SDL_UnlockMutex(pacer->mutex);
                }];
            }
            [commandBuffer presentDrawable:target->drawable];
        }
        [commandBuffer commit];
        // One submission, with frame textures retained until both passes finish.
        [commandBuffer waitUntilCompleted];
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Metal presentation command failed: %s",
                commandBuffer.error.localizedDescription.UTF8String);
            SDL_Event event = {};
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
        }
        discardNextDrawable();
    }}

    id<MTLDevice> getMetalDevice() {
        if (qgetenv("VT_FORCE_METAL") == "0") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Avoiding Metal renderer due to VT_FORCE_METAL=0 override.");
            return nullptr;
        }

        NSArray<id<MTLDevice>> *devices = [MTLCopyAllDevices() autorelease];
        if (devices.count == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No Metal device found!");
            return nullptr;
        }

        for (id<MTLDevice> device in devices) {
            if (device.isLowPower || device.hasUnifiedMemory) {
                return device;
            }
        }

        if (!m_HwAccel) {
            // Metal software decoding is always available
            return [MTLCreateSystemDefaultDevice() autorelease];
        }
        else if (qgetenv("VT_FORCE_METAL") == "1") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using Metal renderer due to VT_FORCE_METAL=1 override.");
            return [MTLCreateSystemDefaultDevice() autorelease];
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Avoiding Metal renderer due to use of dGPU/eGPU. Use VT_FORCE_METAL=1 to override.");
        }

        return nullptr;
    }

    virtual bool initialize(PDECODER_PARAMETERS params) override
    { @autoreleasepool {
        int err;
        if (!m_Pacer->mutex || !m_Pacer->condition) return false;

        id<MTLDevice> device = getMetalDevice();
        if (!device) {
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Selected Metal device: %s",
                    device.name.UTF8String);

        if (m_HwAccel && !checkDecoderCapabilities(device, params)) {
            return false;
        }

        err = av_hwdevice_ctx_create(&m_HwContext,
                                     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                     nullptr,
                                     nullptr,
                                     0);
        if (err < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "av_hwdevice_ctx_create() failed for VT decoder: %d",
                        err);
            return false;
        }

        QVector<PlankPresentationOutput> outputs;
        if (params->presentationLayout && params->presentationLayout->isMultiOutput()) {
            m_PresentationCanvas = params->presentationLayout->canvasSize;
            outputs = params->presentationLayout->outputs;
            if (!m_PresentationCanvas.isValid() || outputs.size() != 2) return false;
        } else {
            int width = 0, height = 0;
            if (!SDL_GetWindowSizeInPixels(params->window, &width, &height)) return false;
            m_PresentationCanvas = QSize(width, height);
            outputs.append({params->window, QRect(QPoint(0, 0), m_PresentationCanvas), true});
        }
        for (const auto& output : outputs) {
            auto target = std::make_unique<MetalPresentationTarget>();
            target->output = output;
            target->view = SDL_Metal_CreateView(output.window);
            if (!target->view) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create Metal view for presentation output: %s", SDL_GetError());
                return false;
            }
            target->layer = (CAMetalLayer*)SDL_Metal_GetLayer(target->view);
            if (!target->layer) return false;
            target->layer.device = device;
            target->layer.wantsExtendedDynamicRangeContent = !!(params->videoFormat & VIDEO_FORMAT_MASK_10BIT);
            target->layer.maximumDrawableCount = 3;
            target->layer.displaySyncEnabled = params->enableVsync;
            if (output.primary) m_MetalLayer = target->layer;
            m_Targets.push_back(std::move(target));
        }
        if (!m_MetalLayer) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Metal presentation initialized: outputs=%zu canvas=%dx%d",
            m_Targets.size(), m_PresentationCanvas.width(), m_PresentationCanvas.height());

        // One window per workstation display: every other window gets its own
        // Metal view and layer on the same device, queue, texture cache and
        // pipelines, and shows its own rectangle of each frame.
        if (params->presentationLayout != nullptr && params->presentationLayout->usesSourceRects()) {
            for (const auto& output : params->presentationLayout->outputs) {
                if (output.window == m_Window) {
                    m_PrimarySourceRect = output.sourceRect;
                    continue;
                }
                auto target = std::make_unique<SecondaryTarget>();
                target->window = output.window;
                target->sourceRect = output.sourceRect;
                target->view = SDL_Metal_CreateView(output.window);
                if (!target->view) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "SDL_Metal_CreateView() failed for a PLANK output window: %s",
                                 SDL_GetError());
                    return false;
                }
                target->layer = (CAMetalLayer*)SDL_Metal_GetLayer(target->view);
                target->layer.device = device;
                target->layer.wantsExtendedDynamicRangeContent = m_MetalLayer.wantsExtendedDynamicRangeContent;
                target->layer.maximumDrawableCount = 3;
                target->layer.displaySyncEnabled = params->enableVsync;
                m_Secondaries.push_back(std::move(target));
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Metal renderer presenting %zu window(s) from source rectangles",
                        m_Secondaries.size() + 1);
        }

        // Create the Metal texture cache for our CVPixelBuffers
        CFStringRef keys[1] = { kCVMetalTextureUsage };
        NSUInteger values[1] = { MTLTextureUsageShaderRead };
        auto cacheAttributes = CFDictionaryCreate(kCFAllocatorDefault, (const void**)keys, (const void**)values, 1, nullptr, nullptr);
        err = CVMetalTextureCacheCreate(kCFAllocatorDefault, cacheAttributes, m_MetalLayer.device, nullptr, &m_TextureCache);
        CFRelease(cacheAttributes);

        if (err != kCVReturnSuccess) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CVMetalTextureCacheCreate() failed: %d",
                         err);
            return false;
        }

        // Compile our shaders
        QString shaderSource = QString::fromUtf8(Path::readDataFile("vt_renderer.metal"));
        NSError* shaderError = nil;
        m_ShaderLibrary = [m_MetalLayer.device newLibraryWithSource:shaderSource.toNSString() options:nullptr error:&shaderError];
        if (!m_ShaderLibrary) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to compile shaders: %s", shaderError.localizedDescription.UTF8String);
            return false;
        }

        // Create a command queue for submission
        m_CommandQueue = [m_MetalLayer.device newCommandQueue];
        return true;
    }}

    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override
    { @autoreleasepool {
        SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
        bool overlayEnabled = Session::get()->getOverlayManager().isOverlayEnabled(type);
        updateOverlayTexture(type, newSurface, overlayEnabled);
    }}

    // Consumes newSurface. The UI thread prepares each replacement while the
    // render thread continues using the last complete texture.
    void updateOverlayTexture(Overlay::OverlayType type, SDL_Surface* newSurface,
                              bool overlayEnabled)
    { @autoreleasepool {
        if (newSurface == nullptr && overlayEnabled) {
            // The overlay is enabled and there is no new surface. Leave the old texture alone.
            return;
        }

        // Only an explicit hide may publish an empty texture slot.
        if (!overlayEnabled) {
            SDL_LockSpinlock(&m_OverlayLock);
            auto oldTexture = m_OverlayTextures[type];
            m_OverlayTextures[type] = nullptr;
            SDL_UnlockSpinlock(&m_OverlayLock);
            [oldTexture release];
            SDL_DestroySurface(newSurface);
            return;
        }

        // Create a texture to hold our pixel data
        SDL_assert(!SDL_MUSTLOCK(newSurface));
        SDL_assert(newSurface->format == SDL_PIXELFORMAT_ARGB8888);
        auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                          width:newSurface->w
                                                                         height:newSurface->h
                                                                      mipmapped:NO];
        texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        texDesc.storageMode = MTLStorageModeManaged;
        texDesc.usage = MTLTextureUsageShaderRead;
        auto newTexture = [m_MetalLayer.device newTextureWithDescriptor:texDesc];
        if (newTexture == nil) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to allocate Metal overlay texture; retaining the previous image");
            SDL_DestroySurface(newSurface);
            return;
        }

        // Load the pixel data into the new texture
        [newTexture replaceRegion:MTLRegionMake2D(0, 0, newSurface->w, newSurface->h)
                      mipmapLevel:0
                        withBytes:newSurface->pixels
                      bytesPerRow:newSurface->pitch];

        // The surface is no longer required
        SDL_DestroySurface(newSurface);
        newSurface = nullptr;

        SDL_LockSpinlock(&m_OverlayLock);
        auto oldTexture = m_OverlayTextures[type];
        m_OverlayTextures[type] = newTexture;
        SDL_UnlockSpinlock(&m_OverlayLock);
        // Readers retain under the same lock; submitted Metal commands retain
        // their resources. Never block the render thread during allocation/upload.
        [oldTexture release];
    }}

    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override
    {
        if (m_HwAccel) {
            context->hw_device_ctx = av_buffer_ref(m_HwContext);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Metal renderer with %s decoding",
                    m_HwAccel ? "hardware" : "software");

        return true;
    }

    virtual bool needsTestFrame() override
    {
        // We used to trust VT to tell us whether decode will work, but
        // there are cases where it can lie because the hardware technically
        // can decode the format but VT is unserviceable for some other reason.
        // Decoding the test frame will tell us for sure whether it will work.
        return true;
    }

    int getDecoderColorspace() override
    {
        return COLORSPACE_REC_709;
    }

    int getDecoderCapabilities() override
    {
        return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
               CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }

    int getRendererAttributes() override
    {
        // Metal supports HDR output
        return RENDERER_ATTRIBUTE_HDR_SUPPORT;
    }

    bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override
    {
        if (m_HwAccel) {
            return pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX;
        }
        else {
            if (pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX) {
                // VideoToolbox frames are always supported
                return true;
            }
            else {
                // Otherwise it's supported if we can map it
                const int expectedPixelDepth = (videoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8;
                const int expectedLog2ChromaW = (videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 0 : 1;
                const int expectedLog2ChromaH = (videoFormat & (VIDEO_FORMAT_MASK_YUV444 | VIDEO_FORMAT_MASK_YUV422)) ? 0 : 1;

                const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get(pixelFormat);
                if (!formatDesc) {
                    // This shouldn't be possible but handle it anyway
                    SDL_assert(formatDesc);
                    return false;
                }

                int planes = av_pix_fmt_count_planes(pixelFormat);
                return (planes == 2 || planes == 3) &&
                       formatDesc->comp[0].depth == expectedPixelDepth &&
                       formatDesc->log2_chroma_w == expectedLog2ChromaW &&
                       formatDesc->log2_chroma_h == expectedLog2ChromaH;
            }
        }
    }

    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override
    {
        auto unhandledStateFlags = info->stateChangeFlags;

        // We can always handle size changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_SIZE;

        // We can handle monitor changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;

        // If nothing is left, we handled everything
        return unhandledStateFlags == 0;
    }

    bool letterboxesAgainstLiveDrawable() override
    {
        // updateVideoRegionSizeForFrame() letterboxes each frame against the
        // live SDL_GetWindowSizeInPixels() drawable.
        return true;
    }

    bool letterboxesDecodedFrameSize() override
    {
        // updateVideoRegionSizeForFrame() fits frame->width/height.
        return true;
    }

private:
    // A window other than the primary one (one per workstation display).
    struct SecondaryTarget
    {
        SDL_Window* window = nullptr;
        SDL_MetalView view = nullptr;
        CAMetalLayer* layer = nullptr;
        QRectF sourceRect;
        id<MTLBuffer> vertexBuffer = nullptr;
        int lastFrameWidth = -1;
        int lastFrameHeight = -1;
        int lastDrawableWidth = -1;
        int lastDrawableHeight = -1;
        // Drawables queued but not yet presented. Shared with the presented
        // handlers, which may run after this renderer is gone.
        std::shared_ptr<std::atomic<int>> inFlight = std::make_shared<std::atomic<int>>(0);
    };

    bool m_HwAccel;
    SDL_Window* m_Window;
    std::vector<std::unique_ptr<SecondaryTarget>> m_Secondaries;
    // The primary window's rectangle of each frame; invalid: the whole frame.
    QRectF m_PrimarySourceRect;
    AVBufferRef* m_HwContext;
    CAMetalLayer* m_MetalLayer;
    CVMetalTextureCacheRef m_TextureCache;
    id<MTLBuffer> m_CscParamsBuffer;
    id<MTLTexture> m_OverlayTextures[Overlay::OverlayMax];
    SDL_SpinLock m_OverlayLock;
    id<MTLRenderPipelineState> m_VideoPipelineState;
    id<MTLRenderPipelineState> m_OverlayPipelineState;
    id<MTLLibrary> m_ShaderLibrary;
    id<MTLCommandQueue> m_CommandQueue;
    id<MTLTexture> m_SwMappingTextures[MAX_VIDEO_PLANES];
    int m_LastColorSpace;
    bool m_LastFullRange;
    QSize m_PresentationCanvas;
    std::vector<std::unique_ptr<MetalPresentationTarget>> m_Targets;
    std::shared_ptr<MetalPresentationPacer> m_Pacer;
};

IFFmpegRenderer* VTMetalRendererFactory::createRenderer(bool hwAccel) {
    return new VTMetalRenderer(hwAccel);
}
