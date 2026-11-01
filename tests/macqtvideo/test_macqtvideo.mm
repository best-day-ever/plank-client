#include "metalvideonode.h"
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavutil/hwcontext.h>
}

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::array<uint16_t,3> rgb(int x)
{
    const uint16_t g = x % 1024;
    return {uint16_t((g * 73) % 1024), g, uint16_t(1023 - g)};
}
class Overlay final : public QQuickItem
{
public:
    explicit Overlay(QQuickItem* parent) : QQuickItem(parent) { setFlag(ItemHasContents); }
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override
    {
        auto* node = old ? static_cast<QSGSimpleRectNode*>(old) : new QSGSimpleRectNode;
        node->setRect(boundingRect()); node->setColor(Qt::white); return node;
    }
};

QtVideoFrame::Ptr frame(QSize size, bool hardware, int depth = 10, bool identity = true, bool checker = false)
{
    auto* source = av_frame_alloc();
    require(source, "allocate frame");
    source->width = size.width(); source->height = size.height();
    source->colorspace = identity ? AVCOL_SPC_RGB : AVCOL_SPC_BT709; source->color_range = AVCOL_RANGE_JPEG;
    source->color_trc = AVCOL_TRC_IEC61966_2_1;
    const auto format = hardware ?
        (identity ? (depth == 10 ? AV_PIX_FMT_P410LE : AV_PIX_FMT_NV24) : (depth == 10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12)) :
        (identity ? (depth == 10 ? AV_PIX_FMT_GBRP10LE : AV_PIX_FMT_GBRP) : (depth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P));
    source->format = hardware ? AV_PIX_FMT_VIDEOTOOLBOX : format;
    CVPixelBufferRef buffer = nullptr;
    if (hardware) {
        NSDictionary* attributes = @{(id)kCVPixelBufferMetalCompatibilityKey:@YES,
            (id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        require(CVPixelBufferCreate(kCFAllocatorDefault, size.width(), size.height(),
            identity ? (depth == 10 ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange : kCVPixelFormatType_444YpCbCr8BiPlanarFullRange) :
                (depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
            (CFDictionaryRef)attributes, &buffer) == kCVReturnSuccess, "CV P410 surface");
        AVBufferRef* device = nullptr;
        require(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) == 0, "VT test device");
        source->hw_frames_ctx = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);
        require(source->hw_frames_ctx, "VT frames context");
        auto* context = (AVHWFramesContext*)source->hw_frames_ctx->data;
        context->format = AV_PIX_FMT_VIDEOTOOLBOX; context->sw_format = format;
        context->width = size.width(); context->height = size.height();
        source->data[3] = (uint8_t*)buffer;
        source->buf[0] = av_buffer_create((uint8_t*)buffer, sizeof(buffer),
            [](void*, uint8_t* bytes) { CFRelease((CVPixelBufferRef)bytes); }, nullptr, 0);
        require(source->buf[0], "retain CV frame");
        require(CVPixelBufferLockBaseAddress(buffer,0) == kCVReturnSuccess, "write CV test pattern");
    } else require(av_frame_get_buffer(source,64) == 0, "software frame planes");
    for (int p = 0; p < (hardware ? 2 : 3); ++p) {
        auto* bytes = hardware ? (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(buffer,p) : source->data[p];
        const size_t stride = hardware ? CVPixelBufferGetBytesPerRowOfPlane(buffer,p) : source->linesize[p];
        const int channels = hardware && p == 1 ? 2 : 1;
        const QSize extent = identity || p == 0 ? size : size / 2;
        for (int y = 0; y < extent.height(); ++y) {
            auto* row = bytes + y * stride;
            for (int x = 0; x < extent.width(); ++x) {
                auto values = rgb(x);
                const int max = (1 << depth) - 1;
                if (depth == 8) values = {uint16_t((x * 73) % 256), uint16_t(x % 256), uint16_t(255-x%256)};
                if (checker) values.fill(((x+y)&1) ? max : 0);
                for (int c=0;c<channels;++c) {
                    const int value = !identity && p ? 1 << (depth-1) :
                        values[!identity || p == 0 ? 1 : p == 1 && c == 0 ? 2 : 0];
                    if (depth == 10) ((uint16_t*)row)[x*channels+c] = value << (hardware ? 6 : 0);
                    else row[x*channels+c] = uint8_t(value);
                }
            }
        }
    }
    if (hardware) CVPixelBufferUnlockBaseAddress(buffer,0);
    QString error;
    auto result = hardware ? QtVideoFrame::videoToolbox(source,format,error) :
                             QtVideoFrame::software(source,format,error);
    av_frame_free(&source);
    require(bool(result), qPrintable(error));
    return result;
}

void run(id<MTLDevice> device, bool hardware, int depth = 10, bool identity = true)
{
    auto queue = [[device newCommandQueue] autorelease];
    QQuickRenderControl control;
    QQuickWindow scene(&control);
    scene.setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndCommandQueue((MTLDevice*)device,(MTLCommandQueue*)queue));
    require(control.initialize(), "initialize Qt Metal");
    QtMetalVideoItem item(scene.contentItem());
    item.setPrimaryOutput(true);
    Overlay overlay(scene.contentItem());
    for (QSize size : {QSize(1024,8),QSize(5120,2160),QSize(3840,2160),QSize(1024,8)}) {
        @autoreleasepool {
            auto spec = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:size.width() height:size.height() mipmapped:NO];
            spec.storageMode = MTLStorageModeShared; spec.usage = MTLTextureUsageRenderTarget;
            auto target = [[device newTextureWithDescriptor:spec] autorelease];
            require(target, "create target");
            scene.resize(size); scene.contentItem()->setSize(size);
            scene.setRenderTarget(QQuickRenderTarget::fromMetalTexture((MTLTexture*)target,uint(MTLPixelFormatRGB10A2Unorm),size));
            overlay.setPosition({0.0,double(size.height()-1)}); overlay.setSize({double(size.width()),1.0});
            item.setPresentation({size,QRectF(QPointF(),size),QRectF(QPointF(),size),size,size});
            item.setFrame(frame(size,hardware,depth,identity));
            control.polishItems(); control.beginFrame(); control.sync(); control.render(); control.endFrame();
            emit scene.frameSwapped();
            require(item.status()->error == QtVideoStatus::None, "production video node failed");
            require(item.status()->livePresented, "missing presentation feedback");
            std::vector<uint32_t> row(size.width());
            for (int y : {0,size.height()/2,size.height()-1}) {
                [target getBytes:row.data() bytesPerRow:size.width()*4 fromRegion:MTLRegionMake2D(0,y,size.width(),1) mipmapLevel:0];
                for (int x = 0; x < size.width(); ++x) {
                    auto expected = rgb(x);
                    if (depth == 8) expected = {uint16_t(qRound((x*73%256)*1023.0/255)),
                        uint16_t(qRound((x%256)*1023.0/255)), uint16_t(qRound((255-x%256)*1023.0/255))};
                    if (!identity) expected.fill(expected[1]);
                    if (y == size.height()-1) expected.fill(1023);
                    for (int c=0;c<3;++c) {
                        const int actual=(row[x]>>(c*10))&1023;
                        if (std::abs(actual - expected[c]) > (depth == 8 ? 1 : 0)) {
                            std::fprintf(stderr,"mismatch hardware=%d size=%dx%d x=%d y=%d c=%d actual=%d expected=%d\n",hardware,size.width(),size.height(),x,y,c,actual,expected[c]);
                            throw std::runtime_error("production video composition changed a 10-bit code");
                        }
                    }
                }
            }
            std::printf("PASS production Qt Metal node %dx%d %s %d-bit %s full range and overlay\n",size.width(),size.height(),hardware?"CV":"software",depth,identity?"identity":"BT709 420");
        }
    }
    control.invalidate();
}
void runScaled(id<MTLDevice> device, bool hardware)
{
    auto queue = [[device newCommandQueue] autorelease];
    QQuickRenderControl control;
    QQuickWindow scene(&control);
    scene.setColor(Qt::black);
    scene.setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndCommandQueue((MTLDevice*)device,(MTLCommandQueue*)queue));
    require(control.initialize(), "initialize scaled Qt Metal");
    QtMetalVideoItem item(scene.contentItem());
    item.setPrimaryOutput(true);
    const QSize source(512,64);
    const auto picture = frame(source,hardware,10,true,true);
    // Both direct area filtering and the extreme-minification compute pass;
    // crop edges and letterbox must remain exact after rendering the scene.
    for (int divisor : {2,16}) {
        const QRect sourceRect(16,8,480,48);
        const QRect destination(4,3,480/divisor,12);
        const QSize size(destination.right()+5,18);
        auto spec = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:size.width() height:size.height() mipmapped:NO];
        spec.storageMode = MTLStorageModeShared; spec.usage = MTLTextureUsageRenderTarget;
        auto target = [[device newTextureWithDescriptor:spec] autorelease];
        require(target, "scaled target");
        scene.resize(size); scene.contentItem()->setSize(size);
        scene.setRenderTarget(QQuickRenderTarget::fromMetalTexture((MTLTexture*)target,uint(MTLPixelFormatRGB10A2Unorm),size));
        item.setPresentation({source,sourceRect,destination,size,size});
        item.setFrame(picture);
        control.polishItems(); control.beginFrame(); control.sync(); control.render(); control.endFrame();
        emit scene.frameSwapped();
        require(item.status()->error == QtVideoStatus::None, "scaled production video node");
        std::vector<uint32_t> pixels(size.width()*size.height());
        [target getBytes:pixels.data() bytesPerRow:size.width()*4 fromRegion:MTLRegionMake2D(0,0,size.width(),size.height()) mipmapLevel:0];
        for (int y=0;y<size.height();++y) for (int x=0;x<size.width();++x) for (int c=0;c<3;++c) {
            const int actual = (pixels[y*size.width()+x]>>(c*10))&1023;
            require(std::abs(actual-(destination.contains(x,y)?512:0)) <= 1, "scaled checkerboard or letterbox mismatch");
        }
        std::printf("PASS production Qt Metal %s crop, letterbox and %dx area reduction\n",hardware?"CV":"software",divisor);
    }
    control.invalidate();
}

}
int main(int argc,char** argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    QGuiApplication app(argc,argv);
    @autoreleasepool {
        auto device = [MTLCreateSystemDefaultDevice() autorelease];
        if (!device) { std::puts("SKIP: no Metal device; precision not qualified"); return 77; }
        try {
            for (bool hardware : {false,true}) {
                for (int depth : {8,10}) for (bool identity : {false,true}) run(device,hardware,depth,identity);
                runScaled(device,hardware);
            }
        }
        catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
    }
    return 0;
}
