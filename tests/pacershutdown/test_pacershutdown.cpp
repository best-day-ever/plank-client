#include <QtTest>
#include <QSemaphore>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

// The only platform query in Pacer::initialize(). No display server is needed.
int StreamUtils::getDisplayRefreshRate(SDL_Window*) { return 60; }

namespace {
void acquire(QSemaphore& semaphore)
{
    if (!semaphore.tryAcquire(1, 2000)) {
        qFatal("Timed out waiting for test worker");
    }
}

// A broken join must fail the test process, not hang the package build.
class Watchdog {
public:
    Watchdog() : m_Thread([this] {
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (!m_Condition.wait_for(lock, std::chrono::seconds(15), [this] { return m_Done; })) {
            qFatal("Pacer shutdown hung");
        }
    }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Done = true;
        }
        m_Condition.notify_one();
        m_Thread.join();
    }
private:
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    bool m_Done = false;
    std::thread m_Thread;
};

class Renderer : public IFFmpegRenderer {
public:
    bool initialize(PDECODER_PARAMETERS) override { return true; }
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return true; }
    bool isRenderThreadSupported() override { return threaded; }
    void waitToRender() override {
        waitEntered.release();
        if (blockWait) acquire(waitRelease);
    }
    void renderFrame(AVFrame*) override {
        renderThread = SDL_GetCurrentThreadID();
        ++renders;
        renderEntered.release();
        if (blockRender) acquire(renderRelease);
    }
    void cleanupRenderContext() override {
        cleanupThread = SDL_GetCurrentThreadID();
        ++cleanups;
    }

    bool threaded = true, blockWait = false, blockRender = false;
    int renders = 0, cleanups = 0; // Read only after the worker is joined.
    SDL_ThreadID renderThread = 0, cleanupThread = 0;
    QSemaphore waitEntered, waitRelease, renderEntered, renderRelease;
};

AVFrame* frameWithReleaseCounter(std::atomic<int>& releases)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) qFatal("Unable to allocate test frame");
    frame->buf[0] = av_buffer_create(static_cast<uint8_t*>(av_malloc(1)), 1,
        [](void* opaque, uint8_t* data) {
            ++*static_cast<std::atomic<int>*>(opaque);
            av_free(data);
        }, &releases, 0);
    if (!frame->buf[0]) qFatal("Unable to allocate test frame buffer");
    frame->pts = 0;
    frame->pkt_dts = SDL_GetTicks();
    return frame;
}

class AsyncVsync : public IVsyncSource {
public:
    explicit AsyncVsync(QSemaphore& entered) : m_Entered(entered) {}
    bool initialize(SDL_Window*, int) override { return true; }
    bool isAsync() override { m_Entered.release(); return true; }
private:
    QSemaphore& m_Entered;
};
}

class TestPacerShutdown : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qunsetenv("PLANK_AV_SYNC_TELEMETRY");
        QVERIFY(SDL_Init(SDL_INIT_EVENTS));
        SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_ERROR);
    }
    void cleanupTestCase() { SDL_Quit(); }
    void uninitializedCleanup();
    void mainThreadCleanup();
    void idleRenderThread();
    void stopWhileWaitingForRenderer();
    void stopDuringRender();
    void waitBoundary_data();
    void waitBoundary();
    void asyncVsyncShutdown();

private:
    static void waitForStop(Pacer* pacer) {
        QDeadlineTimer deadline(2000);
        while (!pacer->m_Stopping && !deadline.hasExpired()) QThread::usleep(100);
        if (!pacer->m_Stopping) qFatal("Shutdown did not publish stop predicate");
    }
};

void TestPacerShutdown::uninitializedCleanup()
{
    Watchdog watchdog;
    VIDEO_STATS stats {};
    Renderer renderer;
    { Pacer pacer(&renderer, &stats); }
    QCOMPARE(renderer.cleanups, 1);
    QCOMPARE(renderer.cleanupThread, SDL_GetCurrentThreadID());
}

void TestPacerShutdown::mainThreadCleanup()
{
    Watchdog watchdog;
    VIDEO_STATS stats {};
    Renderer renderer;
    renderer.threaded = false;
    std::atomic<int> releases {0};
    {
        Pacer pacer(&renderer, &stats);
        QVERIFY(pacer.initialize(nullptr, 60, false));
        pacer.submitFrame(frameWithReleaseCounter(releases));
        pacer.renderOnMainThread();
        pacer.submitFrame(frameWithReleaseCounter(releases));
    }
    QCOMPARE(renderer.renders, 1);
    QCOMPARE(renderer.cleanups, 1);
    QCOMPARE(renderer.cleanupThread, SDL_GetCurrentThreadID());
    QCOMPARE(releases.load(), 2);
    SDL_FlushEvent(SDL_EVENT_USER);
}

void TestPacerShutdown::idleRenderThread()
{
    Watchdog watchdog;
    for (int i = 0; i < 300; ++i) {
        VIDEO_STATS stats {};
        Renderer renderer;
        auto pacer = std::make_unique<Pacer>(&renderer, &stats);
        QVERIFY(pacer->initialize(nullptr, 60, false));
        QVERIFY(pacer->m_RenderThread);
        if (i % 3 != 0) acquire(renderer.waitEntered);
        if (i % 3 == 2) QThread::usleep(100);
        pacer.reset();
        QCOMPARE(renderer.renders, 0);
        QCOMPARE(renderer.cleanups, 1);
        QVERIFY(renderer.cleanupThread != SDL_GetCurrentThreadID());
    }
}

void TestPacerShutdown::stopWhileWaitingForRenderer()
{
    Watchdog watchdog;
    VIDEO_STATS stats {};
    Renderer renderer;
    renderer.blockWait = true;
    std::atomic<int> releases {0};
    auto pacer = std::make_unique<Pacer>(&renderer, &stats);
    QVERIFY(pacer->initialize(nullptr, 60, false));
    acquire(renderer.waitEntered);
    pacer->submitFrame(frameWithReleaseCounter(releases));
    Pacer* stopping = pacer.release();
    std::thread shutdown([&] { delete stopping; });
    waitForStop(stopping); // The controlled renderer keeps the object alive.
    renderer.waitRelease.release();
    shutdown.join();
    QCOMPARE(renderer.renders, 0);
    QCOMPARE(renderer.cleanups, 1);
    QCOMPARE(releases.load(), 1);
}

void TestPacerShutdown::stopDuringRender()
{
    Watchdog watchdog;
    VIDEO_STATS stats {};
    Renderer renderer;
    renderer.blockRender = true;
    std::atomic<int> releases {0};
    auto pacer = std::make_unique<Pacer>(&renderer, &stats);
    QVERIFY(pacer->initialize(nullptr, 60, false));
    pacer->submitFrame(frameWithReleaseCounter(releases));
    acquire(renderer.renderEntered);
    pacer->submitFrame(frameWithReleaseCounter(releases));
    Pacer* stopping = pacer.release();
    std::thread shutdown([&] { delete stopping; });
    waitForStop(stopping);
    renderer.renderRelease.release();
    shutdown.join();
    QCOMPARE(renderer.renders, 1);
    QCOMPARE(renderer.cleanups, 1);
    QCOMPARE(renderer.cleanupThread, renderer.renderThread);
    QCOMPARE(releases.load(), 2);
}

void TestPacerShutdown::waitBoundary_data()
{
    QTest::addColumn<int>("queue");
    QTest::newRow("render") << 0;
    QTest::newRow("pacing") << 1;
    QTest::newRow("vsync") << 2;
}

void TestPacerShutdown::waitBoundary()
{
    QFETCH(int, queue);
    Watchdog watchdog;
    VIDEO_STATS stats {};
    Renderer renderer;
    auto pacer = std::make_unique<Pacer>(&renderer, &stats);
    struct Boundary {
        Pacer* pacer;
        QWaitCondition* condition;
        QSemaphore checked, enterWait;
        bool woke = false, stoppedBeforeWait = false;
    } boundary;
    boundary.pacer = pacer.get();
    boundary.condition = queue == 0 ? &pacer->m_RenderQueueNotEmpty :
                         queue == 1 ? &pacer->m_PacingQueueNotEmpty : &pacer->m_VsyncSignalled;

    // Widen the exact predicate-check -> wait boundary with the real queue
    // mutex and condition. No production timing hooks or replacement Qt locks.
    SDL_Thread* waiter = SDL_CreateThread([](void* context) {
        auto& b = *static_cast<Boundary*>(context);
        QMutexLocker lock(&b.pacer->m_FrameQueueLock);
        if (!b.pacer->m_Stopping) {
            b.checked.release();
            acquire(b.enterWait);
            b.stoppedBeforeWait = b.pacer->m_Stopping;
            // Bound the synthetic waiter so a regression fails, not hangs.
            b.woke = b.condition->wait(&b.pacer->m_FrameQueueLock, 2000);
        }
        return 0;
    }, "QueueBoundary", &boundary);
    QVERIFY(waiter);
    // Use the V-sync join slot for this synthetic waiter. Real render-thread
    // cleanup and its unbounded wait are covered by the tests above.
    pacer->m_VsyncThread = waiter;
    acquire(boundary.checked);
    QSemaphore shutdownStarted;
    Pacer* stopping = pacer.release();
    std::thread shutdown([&] { shutdownStarted.release(); delete stopping; });
    acquire(shutdownStarted);
    QThread::msleep(50);
    boundary.enterWait.release();
    shutdown.join();
    QVERIFY(!boundary.stoppedBeforeWait);
    QVERIFY(boundary.woke);
    QCOMPARE(renderer.cleanups, 1);
}

void TestPacerShutdown::asyncVsyncShutdown()
{
    Watchdog watchdog;
    for (int i = 0; i < 100; ++i) {
        VIDEO_STATS stats {};
        Renderer renderer;
        renderer.threaded = false;
        QSemaphore entered;
        auto pacer = std::make_unique<Pacer>(&renderer, &stats);
        QVERIFY(pacer->initialize(nullptr, 60, false));
        pacer->m_VsyncSource = new AsyncVsync(entered);
        pacer->m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "TestVsync", pacer.get());
        QVERIFY(pacer->m_VsyncThread);
        acquire(entered);
        // Exercise both the initial wait and handleVsync()'s empty pacing wait.
        if (i % 2) pacer->signalVsync();
        pacer.reset();
        QCOMPARE(renderer.cleanups, 1);
    }
}

QTEST_GUILESS_MAIN(TestPacerShutdown)
#include "test_pacershutdown.moc"
