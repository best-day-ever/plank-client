// Exercise the production texture update with deterministic Metal allocation
// and upload doubles. No display, GPU, Host, credentials or input required.
#include "../../app/streaming/video/ffmpeg-renderers/vt_metal.mm"
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

Session* Session::s_ActiveSession = nullptr;
QAtomicInt g_AsyncLoggingEnabled = 0;
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }
float Overlay::OverlayManager::getOverlayHorizontalPosition(Overlay::OverlayType) const { return 0.5f; }
QByteArray Path::readDataFile(QString) { return {}; }

static void require(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

struct UploadGate
{
    enum Stage { None, Allocation, Upload };
    Stage stage = None;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;

    void pause(Stage at)
    {
        if (stage != at) return;
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        require(condition.wait_for(lock, std::chrono::seconds(5), [this] { return released; }),
                "upload thread did not resume");
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }),
                "upload thread did not reach the requested stage");
    }

    void resume()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

static std::atomic<int> liveTextures {0};

// These doubles intentionally implement only the selectors the production
// updater needs. They are never submitted to a real Metal command encoder.
@interface TestOverlayTexture : NSObject {
@public
    UploadGate* gate;
    std::vector<uint8_t> pixels;
    NSUInteger width;
    NSUInteger height;
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes bytesPerRow:(NSUInteger)pitch;
@end

@implementation TestOverlayTexture
- (instancetype)init
{
    self = [super init];
    if (self) ++liveTextures;
    return self;
}
- (void)dealloc
{
    --liveTextures;
    [super dealloc];
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes bytesPerRow:(NSUInteger)pitch
{
    (void)region; (void)level;
    gate->pause(UploadGate::Upload);
    pixels.resize(width * height * 4);
    for (NSUInteger y = 0; y < height; ++y)
        memcpy(pixels.data() + y * width * 4,
               static_cast<const uint8_t*>(bytes) + y * pitch, width * 4);
}
@end

@interface TestOverlayDevice : NSObject {
@public
    UploadGate gate;
    bool failAllocation;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor;
@end

@implementation TestOverlayDevice
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor
{
    gate.pause(UploadGate::Allocation);
    if (failAllocation) return nil;
    TestOverlayTexture* texture = [[TestOverlayTexture alloc] init];
    texture->gate = &gate;
    texture->width = descriptor.width;
    texture->height = descriptor.height;
    return (id<MTLTexture>)texture;
}
@end

@interface TestOverlayLayer : NSObject {
@public
    TestOverlayDevice* fixtureDevice;
}
- (id<MTLDevice>)device;
@end
@implementation TestOverlayLayer
- (id<MTLDevice>)device { return (id<MTLDevice>)fixtureDevice; }
@end

class VTMetalRendererProbe
{
    struct Fixture
    {
        VTMetalRenderer renderer {false};
        TestOverlayDevice* device = [[TestOverlayDevice alloc] init];
        TestOverlayLayer* layer = [[TestOverlayLayer alloc] init];
        Fixture()
        {
            layer->fixtureDevice = device;
            renderer.m_MetalLayer = (CAMetalLayer*)layer;
        }
        ~Fixture() { [layer release]; [device release]; }

        TestOverlayTexture* acquire(Overlay::OverlayType type = Overlay::OverlayToolbar)
        {
            // Match the render thread's retained read under the same lock.
            SDL_LockSpinlock(&renderer.m_OverlayLock);
            auto texture = (TestOverlayTexture*)[renderer.m_OverlayTextures[type] retain];
            SDL_UnlockSpinlock(&renderer.m_OverlayLock);
            return texture;
        }

        void update(uint8_t value, int width = 32, int height = 16,
                    Overlay::OverlayType type = Overlay::OverlayToolbar)
        {
            auto surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
            require(surface != nullptr, "surface allocation");
            memset(surface->pixels, value, surface->pitch * surface->h);
            renderer.updateOverlayTexture(type, surface, true);
        }
    };

    static void check(TestOverlayTexture* texture, uint8_t value, NSUInteger width = 32,
                      NSUInteger height = 16)
    {
        require(texture != nil, "visible overlay has no texture");
        require(texture->width == width && texture->height == height, "wrong texture dimensions");
        require(texture->pixels.size() == width * height * 4, "incomplete upload published");
        for (auto byte : texture->pixels) require(byte == value, "partially updated image");
    }

    static void delayedReplacement(UploadGate::Stage stage)
    {
        Fixture fixture;
        fixture.update(17);
        auto oldTexture = fixture.acquire();
        fixture.device->gate.stage = stage;
        std::thread updater([&] { fixture.update(34, 64, 20); });
        fixture.device->gate.wait();
        auto during = fixture.acquire();
        require(during == oldTexture, "replacement removed the visible texture before upload completed");
        check(during, 17);
        [during release];
        fixture.device->gate.resume();
        updater.join();
        auto after = fixture.acquire();
        require(after != oldTexture, "replacement was not published");
        check(after, 34, 64, 20);
        // A render reader may retain the old image past publication of the new one.
        check(oldTexture, 17);
        [oldTexture release]; [after release];
        std::printf("PASS delayed %s retains the old complete image\n",
                    stage == UploadGate::Allocation ? "allocation" : "upload");
    }

    static void failedAllocationAndVisibility()
    {
        Fixture fixture;
        fixture.update(51);
        auto original = fixture.acquire();
        fixture.device->failAllocation = true;
        fixture.update(68);
        auto after = fixture.acquire();
        require(after == original, "allocation failure discarded the valid image");
        check(after, 51);
        [after release];
        fixture.renderer.updateOverlayTexture(Overlay::OverlayToolbar, nullptr, true);
        after = fixture.acquire();
        require(after == original, "empty update discarded the valid image");
        [after release];
        auto unused = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_ARGB8888);
        require(unused != nullptr, "surface allocation");
        fixture.renderer.updateOverlayTexture(Overlay::OverlayToolbar, unused, false);
        require(fixture.acquire() == nil, "explicit hide retained a visible image");
        check(original, 51);
        [original release];
        fixture.device->failAllocation = false;
        fixture.update(85);
        after = fixture.acquire();
        check(after, 85);
        [after release];
        std::puts("PASS allocation failure, empty update, explicit hide and re-enable");
    }

    static void independentOverlaysAndConcurrentReads()
    {
        Fixture fixture;
        fixture.update(102);
        fixture.update(119, 32, 16, Overlay::OverlayDebug);
        std::atomic<bool> complete {false};
        std::atomic<unsigned> reads {0};
        std::promise<void> firstRead;
        auto ready = firstRead.get_future();
        std::thread reader([&] {
            while (!complete.load()) {
                auto texture = fixture.acquire();
                require(texture != nil && !texture->pixels.empty(), "concurrent reader lost the toolbar");
                check(texture, texture->pixels[0]);
                [texture release];
                if (++reads == 1) firstRead.set_value();
            }
        });
        require(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "concurrent reader did not start");
        for (int i = 0; i < 1000; ++i) fixture.update(uint8_t(i));
        complete.store(true);
        reader.join();
        require(reads > 0, "concurrent reader was not exercised");
        auto debug = fixture.acquire(Overlay::OverlayDebug);
        check(debug, 119);
        [debug release];
        std::puts("PASS concurrent replacement and independent overlay slots");
    }

public:
    static void run()
    {
        delayedReplacement(UploadGate::Allocation);
        require(liveTextures == 0, "allocation test leaked a texture");
        delayedReplacement(UploadGate::Upload);
        require(liveTextures == 0, "upload test leaked a texture");
        failedAllocationAndVisibility();
        require(liveTextures == 0, "failure/visibility test leaked a texture");
        independentOverlaysAndConcurrentReads();
        require(liveTextures == 0, "concurrent test leaked a texture");
        std::puts("PASS texture ownership balanced");
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    @autoreleasepool {
        require(SDL_Init(0), "SDL initialization");
        VTMetalRendererProbe::run();
        SDL_Quit();
    }
    return 0;
}
