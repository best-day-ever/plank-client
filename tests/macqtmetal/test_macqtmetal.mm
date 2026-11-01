// Exercise Qt's public custom-target path with the production video shader.
// No native window, TCC request, input injection or network connection.
#import <Metal/Metal.h>
#include "../../app/streaming/video/ffmpeg-renderers/vt_colors.h"
#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

class Overlay final : public QQuickItem
{
public:
    explicit Overlay(QQuickItem* parent) : QQuickItem(parent) { setFlag(ItemHasContents); }
private:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override
    {
        auto* node = static_cast<QSGSimpleRectNode*>(old);
        if (!node) node = new QSGSimpleRectNode;
        node->setRect(boundingRect());
        node->setColor(Qt::white);
        return node;
    }
};

struct Vertex { simd_float4 position; simd_float2 uv; };
constexpr Vertex Vertices[] = {{{-1, 1, 0, 1}, {0, 0}}, {{-1, -1, 0, 1}, {0, 1}},
                               {{ 1, 1, 0, 1}, {1, 0}}, {{ 1, -1, 0, 1}, {1, 1}}};

// Independent RGB oracle. The permutation distinguishes all channel orders;
// the gray ramp detects clipping, implicit transfer conversion and 8-bit loss.
std::array<uint16_t, 3> rgb(int x, bool gray)
{
    const uint16_t g = x % 1024;
    return gray ? std::array<uint16_t, 3>{g, g, g} :
                  std::array<uint16_t, 3>{uint16_t((g * 73) % 1024), g, uint16_t(1023 - g)};
}

void run(id<MTLDevice> device, id<MTLLibrary> library, bool planar, bool gray)
{
    auto queue = [device newCommandQueue];
    require(queue != nil, "Metal command queue");
    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setColor(Qt::black);
    window.setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndCommandQueue(device, queue));
    require(control.initialize(), "Qt Metal render control initialization");
    require(window.rendererInterface()->graphicsApi() == QSGRendererInterface::Metal, "Qt must use Metal");
    Overlay overlay(window.contentItem());

    NSError* error = nil;
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vs_draw"];
    descriptor.fragmentFunction = [library newFunctionWithName:planar ? @"ps_draw_triplanar" : @"ps_draw_biplanar"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGB10A2Unorm;
    auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    require(pipeline != nil, "10-bit video pipeline");
    const auto params = plankVTColorParams(PlankVTMatrix::IdentityGbr, true, 10, !planar);
    id<MTLTexture> planes[3] = {nil, nil, nil};
    id<MTLCommandBuffer> submitted = nil;
    unsigned draws = 0;
    const auto connection = QObject::connect(&window, &QQuickWindow::beforeRenderPassRecording,
        &window, [&] {
            window.beginExternalCommands();
            auto* rif = window.rendererInterface();
            auto encoder = (__bridge id<MTLRenderCommandEncoder>)rif->getResource(&window, QSGRendererInterface::CommandEncoderResource);
            submitted = (__bridge id<MTLCommandBuffer>)rif->getResource(&window, QSGRendererInterface::CommandListResource);
            require(encoder != nil && submitted != nil, "Qt must expose its active Metal encoder");
            [encoder setRenderPipelineState:pipeline];
            [encoder setVertexBytes:Vertices length:sizeof(Vertices) atIndex:0];
            [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
            [encoder setFragmentTexture:planes[0] atIndex:0];
            [encoder setFragmentTexture:planes[1] atIndex:1];
            if (planar) [encoder setFragmentTexture:planes[2] atIndex:2];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            ++draws;
            window.endExternalCommands();
        }, Qt::DirectConnection);

    // Replace targets and return to the original dimensions: retained scene
    // resources must not keep drawing to the previous texture after a resize.
    for (const QSize size : {QSize(1024, 8), QSize(5120, 2160), QSize(3840, 2160), QSize(1024, 8)}) {
        for (int p = 0; p < (planar ? 3 : 2); ++p) {
            const int channels = !planar && p == 1 ? 2 : 1;
            auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                channels == 2 ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm
                width:size.width() height:size.height() mipmapped:NO];
            desc.storageMode = MTLStorageModeShared;
            desc.usage = MTLTextureUsageShaderRead;
            planes[p] = [device newTextureWithDescriptor:desc];
            require(planes[p] != nil, "source plane allocation");
            std::vector<uint16_t> pixels(size_t(size.width()) * size.height() * channels);
            for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
                const auto c = rgb(x, gray);
                const size_t offset = (size_t(y) * size.width() + x) * channels;
                pixels[offset] = c[p == 0 ? 1 : p == 1 ? 2 : 0] << (planar ? 0 : 6);
                if (channels == 2) pixels[offset + 1] = c[0] << 6;
            }
            [planes[p] replaceRegion:MTLRegionMake2D(0, 0, size.width(), size.height()) mipmapLevel:0
                withBytes:pixels.data() bytesPerRow:size.width() * channels * sizeof(uint16_t)];
        }
        auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm
            width:size.width() height:size.height() mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget;
        auto target = [device newTextureWithDescriptor:desc];
        require(target != nil, "10-bit target allocation");
        window.setGeometry(0, 0, size.width(), size.height());
        window.contentItem()->setSize(size);
        overlay.setPosition(QPointF(0, size.height() - 1));
        overlay.setSize(QSizeF(size.width(), 1));
        window.setRenderTarget(QQuickRenderTarget::fromMetalTexture(target,
            uint(MTLPixelFormatRGB10A2Unorm), size));
        const unsigned before = draws;
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        control.endFrame();
        require(draws == before + 1 && submitted != nil, "one composed frame per submission");
        // Test-only GPU readback. Production presentation must never do this.
        [submitted waitUntilCompleted];
        require(submitted.status == MTLCommandBufferStatusCompleted, "Qt Metal command completed");
        std::vector<uint32_t> row(size.width());
        for (const int y : {0, size.height() / 2, size.height() - 1}) {
            [target getBytes:row.data() bytesPerRow:size.width() * sizeof(uint32_t)
                fromRegion:MTLRegionMake2D(0, y, size.width(), 1) mipmapLevel:0];
            std::array<bool, 1024> levels{};
            for (int x = 0; x < size.width(); ++x) {
                const auto expected = y == size.height() - 1 ? std::array<uint16_t, 3>{1023, 1023, 1023} : rgb(x, gray);
                for (int c = 0; c < 3; ++c) {
                    const int actual = (row[x] >> (10 * c)) & 1023;
                    if (actual != expected[c]) {
                        std::fprintf(stderr, "FAIL %dx%d planar=%d gray=%d x=%d y=%d channel=%d actual=%d expected=%d\n",
                            size.width(), size.height(), planar, gray, x, y, c, actual, expected[c]);
                        throw std::runtime_error("Qt composition changed a ten-bit code value");
                    }
                }
                levels[(row[x] >> 10) & 1023] = true;
            }
            if (y != size.height() - 1) for (bool present : levels) require(present, "all 1,024 levels survive composition");
        }
        std::printf("PASS Qt/Metal SDR RGB10A2 %dx%d storage=%s pattern=%s overlay=opaque levels=1024\n",
            size.width(), size.height(), planar ? "gbrp10le" : "p410le", gray ? "gray" : "rgb");
    }
    QObject::disconnect(connection);
    control.invalidate();
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    QGuiApplication app(argc, argv);
    @autoreleasepool {
        auto device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP: no Metal device; precision not qualified"); return 77; }
        try {
            NSError* error = nil;
            NSString* source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
            require(source != nil, "production Metal shader source");
            auto library = [device newLibraryWithSource:source options:nil error:&error];
            require(library != nil, "production Metal shader compilation");
            for (bool planar : {true, false}) for (bool gray : {true, false}) run(device, library, planar, gray);
            std::puts("PASS Qt/Metal composition gate (offscreen; display and decoder acceptance remain separate)");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: %s\n", e.what());
            return 1;
        }
    }
    return 0;
}
