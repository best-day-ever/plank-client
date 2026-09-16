// Native GPU probe of the production renderer. No host, credentials or input
// injection. Standalone stubs below disable application overlays only.
#include "../../app/streaming/video/ffmpeg-renderers/vt_metal.mm"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
#include <stdexcept>

Session* Session::s_ActiveSession = nullptr;
QAtomicInt g_AsyncLoggingEnabled = 0;
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }
float Overlay::OverlayManager::getOverlayHorizontalPosition(Overlay::OverlayType) const { return 0.5f; }
QByteArray Path::readDataFile(QString name)
{
    QFile file(QString::fromLocal8Bit(qgetenv("PLANK_TEST_DATA_DIR")) + '/' + name);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("shader fixture unavailable");
    return file.readAll();
}

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct TestFrame
{
    AVFrame* frame = av_frame_alloc();
    CVPixelBufferRef buffer = nullptr;
    ~TestFrame() { av_frame_free(&frame); if (buffer) CFRelease(buffer); }
    void populate(bool hardware, AVBufferRef* device)
    {
        frame->width = 320; frame->height = 160;
        frame->colorspace = AVCOL_SPC_RGB;
        frame->color_range = AVCOL_RANGE_JPEG;
        frame->color_trc = AVCOL_TRC_IEC61966_2_1;
        frame->format = hardware ? AV_PIX_FMT_VIDEOTOOLBOX : AV_PIX_FMT_GBRP10LE;
        if (hardware) {
            NSDictionary* attributes = @{(id)kCVPixelBufferMetalCompatibilityKey: @YES,
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
            require(CVPixelBufferCreate(kCFAllocatorDefault, 320, 160,
                kCVPixelFormatType_444YpCbCr10BiPlanarFullRange,
                (CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess, "CVPixelBuffer allocation");
            require(CVPixelBufferLockBaseAddress(buffer, 0) == kCVReturnSuccess, "CVPixelBuffer lock");
            frame->data[3] = reinterpret_cast<uint8_t*>(buffer);
            frame->hw_frames_ctx = av_hwframe_ctx_alloc(device);
            require(frame->hw_frames_ctx != nullptr, "hardware frame metadata");
            auto context = (AVHWFramesContext*)frame->hw_frames_ctx->data;
            context->format = AV_PIX_FMT_VIDEOTOOLBOX;
            context->sw_format = AV_PIX_FMT_P410LE;
        } else {
            require(av_frame_get_buffer(frame, 32) == 0, "software frame allocation");
        }
        // Four unambiguous quadrants detect duplication, swapped outputs and
        // vertical inversion: red/green above blue/white.
        for (int y = 0; y < 160; ++y) for (int x = 0; x < 320; ++x) {
            const bool right = x >= 160, bottom = y >= 80;
            const uint16_t r = (!right && !bottom) || (right && bottom) ? 1023 : 0;
            const uint16_t g = right ? 1023 : 0;
            const uint16_t b = bottom ? 1023 : 0;
            if (hardware) {
                auto luma = (uint16_t*)((uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buffer, 0) +
                    y * CVPixelBufferGetBytesPerRowOfPlane(buffer, 0));
                auto chroma = (uint16_t*)((uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buffer, 1) +
                    y * CVPixelBufferGetBytesPerRowOfPlane(buffer, 1));
                luma[x] = g << 6; chroma[2*x] = b << 6; chroma[2*x+1] = r << 6;
            } else {
                ((uint16_t*)(frame->data[0] + y*frame->linesize[0]))[x] = g;
                ((uint16_t*)(frame->data[1] + y*frame->linesize[1]))[x] = b;
                ((uint16_t*)(frame->data[2] + y*frame->linesize[2]))[x] = r;
            }
        }
        if (hardware) CVPixelBufferUnlockBaseAddress(buffer, 0);
    }
};

class VTMetalRendererProbe
{
    static void checkPixel(id<MTLTexture> texture, id<MTLCommandQueue> queue,
                           float x, float y, bool red, bool green, bool blue)
    {
        require(texture.pixelFormat == MTLPixelFormatBGR10A2Unorm, "10-bit drawable required");
        auto buffer = [texture.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        auto command = [queue commandBuffer];
        auto blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(texture.width*x, texture.height*y, 0)
            sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buffer destinationOffset:0
            destinationBytesPerRow:256 destinationBytesPerImage:256];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, "readback command failed");
        const uint32_t pixel = *(uint32_t*)buffer.contents;
        [buffer release];
        const int rgb[] = {int((pixel >> 20) & 1023), int((pixel >> 10) & 1023), int(pixel & 1023)};
        const bool expected[] = {red, green, blue};
        for (int c = 0; c < 3; ++c) {
            if ((expected[c] && rgb[c] < 1000) || (!expected[c] && rgb[c] > 23)) {
                std::fprintf(stderr, "pixel mismatch: rgb=%d,%d,%d expected=%d,%d,%d\n",
                    rgb[0],rgb[1],rgb[2],red,green,blue);
                throw std::runtime_error("wrong display crop, orientation or color");
            }
        }
    }
public:
    static void run(SDL_Window* left, SDL_Window* right, bool hardware, bool dual, bool vsync)
    {
        PlankPresentationLayout layout {QSize(320, 160),
            {{left, QRect(0, 0, 160, 160), true}, {right, QRect(160, 0, 160, 160), false}}};
        DECODER_PARAMETERS params {};
        params.window = left;
        params.width = 320; params.height = 160;
        params.videoFormat = VIDEO_FORMAT_H265_REXT10_444;
        params.enableVsync = vsync;
        params.presentationLayout = dual ? &layout : nullptr;
        VTMetalRenderer renderer(hardware);
        require(renderer.initialize(&params), "renderer initialization");
        require(renderer.m_Targets.size() == (dual ? 2 : 1), "target count");
        for (auto& target : renderer.m_Targets) target->layer.framebufferOnly = NO;
        TestFrame fixture;
        fixture.populate(hardware, renderer.m_HwContext);
        // Prime format/geometry. The first format transition releases drawables.
        renderer.waitToRender(); renderer.renderFrame(fixture.frame);
        for (int iteration = 0; iteration < 3; ++iteration) {
            SDL_PumpEvents();
            renderer.waitToRender();
            std::vector<id<MTLTexture>> textures;
            for (auto& target : renderer.m_Targets) {
                require(target->drawable != nil, "drawable unavailable");
                textures.push_back([target->drawable.texture retain]);
            }
            renderer.renderFrame(fixture.frame);
            if (dual) {
                checkPixel(textures[0], renderer.m_CommandQueue, .5f, .25f, true, false, false);
                checkPixel(textures[0], renderer.m_CommandQueue, .5f, .75f, false, false, true);
                checkPixel(textures[1], renderer.m_CommandQueue, .5f, .25f, false, true, false);
                checkPixel(textures[1], renderer.m_CommandQueue, .5f, .75f, true, true, true);
            } else {
                checkPixel(textures[0], renderer.m_CommandQueue, .25f, .4f, true, false, false);
                checkPixel(textures[0], renderer.m_CommandQueue, .75f, .6f, true, true, true);
            }
            for (auto texture : textures) [texture release];
        }
        renderer.cleanupRenderContext();
        std::printf("PASS storage=%s outputs=%d vsync=%d\n", hardware ? "p410le" : "gbrp10le", dual ? 2 : 1, vsync);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
        if (!SDL_Init(SDL_INIT_VIDEO)) return 2;
        SDL_Window* left = SDL_CreateWindow("PLANK Metal probe: left", 320, 320,
            SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
        SDL_Window* right = SDL_CreateWindow("PLANK Metal probe: right", 480, 480,
            SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
        int count = 0; auto displays = SDL_GetDisplays(&count);
        if (!left || !right || count < 1) return 2;
        const int leftPos = SDL_WINDOWPOS_CENTERED_DISPLAY(displays[0]);
        const int rightPos = SDL_WINDOWPOS_CENTERED_DISPLAY(displays[count > 1 ? 1 : 0]);
        SDL_SetWindowPosition(left, leftPos, leftPos);
        SDL_SetWindowPosition(right, rightPos, rightPos);
        SDL_SyncWindow(left); SDL_SyncWindow(right);
        const bool fullscreen = app.arguments().contains("--fullscreen");
        if (fullscreen) {
            if (count != 2 || !SDL_SetWindowFullscreen(left, true) ||
                    !SDL_SetWindowFullscreen(right, true) ||
                    !SDL_SyncWindow(left) || !SDL_SyncWindow(right) ||
                    SDL_GetDisplayForWindow(left) != displays[0] ||
                    SDL_GetDisplayForWindow(right) != displays[1]) {
                std::fprintf(stderr, "FAIL: two distinct fullscreen outputs required\n");
                SDL_DestroyWindow(right); SDL_DestroyWindow(left); SDL_free(displays); SDL_Quit();
                return 2;
            }
        }
        std::printf("presentation=%s physical-displays=%d distinct-window-displays=%d\n",
            fullscreen ? "fullscreen" : "windowed", count,
            SDL_GetDisplayForWindow(left) != SDL_GetDisplayForWindow(right));
        SDL_free(displays);
        int result = 0;
        try {
            for (bool vsync : {false, true}) for (bool hardware : {false, true}) {
                VTMetalRendererProbe::run(left, right, hardware, true, vsync);
                VTMetalRendererProbe::run(left, right, hardware, false, vsync);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s (%s)\n", e.what(), SDL_GetError()); result = 1;
        }
        SDL_DestroyWindow(right); SDL_DestroyWindow(left); SDL_Quit();
        return result;
    }
}
