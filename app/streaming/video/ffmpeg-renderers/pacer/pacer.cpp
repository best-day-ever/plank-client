#include "pacer.h"
#include "streaming/avsynccontroller.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <VersionHelpers.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>
#include <algorithm>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 4

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

namespace {
uint32_t histogramPercentile(const std::array<uint32_t, 1001>& histogram,
                             unsigned int percentile)
{
    uint64_t sampleCount = 0;
    for (uint32_t count : histogram) {
        sampleCount += count;
    }

    if (sampleCount == 0 || percentile == 0 || percentile > 100) {
        return 0;
    }

    const uint64_t targetRank = (sampleCount * percentile + 99) / 100;
    uint64_t cumulativeSamples = 0;
    for (size_t latency = 0; latency < histogram.size(); latency++) {
        cumulativeSamples += histogram[latency];
        if (cumulativeSamples >= targetRank) {
            return latency;
        }
    }

    SDL_assert(false);
    return 0;
}

uint64_t histogramSampleCount(const std::array<uint32_t, 1001>& histogram)
{
    uint64_t sampleCount = 0;
    for (uint32_t count : histogram) {
        sampleCount += count;
    }
    return sampleCount;
}
}

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats),
    m_AvSyncTelemetryEnabled(qEnvironmentVariableIntValue("STATIONCONNECT_AV_SYNC_TELEMETRY") > 0),
    m_LastVideoTelemetryTime(0)
{

}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }

    if (histogramSampleCount(m_QueueLatencyHistogram) >= 60) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Client pacer queue latency p95/p99/max: %u/%u/%u ms",
                    histogramPercentile(m_QueueLatencyHistogram, 95),
                    histogramPercentile(m_QueueLatencyHistogram, 99),
                    m_MaxQueueLatencyMs);
    }
    if (histogramSampleCount(m_RendererCallLatencyHistogram) >= 60) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Client renderer call latency p95/p99/max: %u/%u/%u ms",
                    histogramPercentile(m_RendererCallLatencyHistogram, 95),
                    histogramPercentile(m_RendererCallLatencyHistogram, 99),
                    m_MaxRendererCallLatencyMs);
    }
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : m_PacingQueueHistory) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        int queueDepth = m_PacingQueue.count();
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        recordFrameDrop(DropReason::PacingCatchUp, frame, queueDepth, frameDropTarget);
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    // The render queue is empty before the first frame arrives. Seed that
    // state so the startup burst gets the same 500 ms grace period as a queue
    // that drains during steady-state playback. Without this entry, an empty
    // history selects a target depth of zero and can discard a single queued
    // frame before the history has had a chance to observe an empty queue.
    m_RenderQueueHistory.enqueue(0);

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            // Don't use D3DKMTWaitForVerticalBlankEvent() on Windows 7, because
            // it blocks during other concurrent DX operations (like actually rendering).
            if (IsWindows8OrGreater()) {
                m_VsyncSource = new DxVsyncSource(this);
            }
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    Uint32 beforeRender = SDL_GetTicks();
    const uint32_t queueLatencyMs = beforeRender - frame->pkt_dts;
    m_VideoStats->totalPacerTime += queueLatencyMs;
    m_QueueLatencyHistogram[std::min<uint32_t>(
        queueLatencyMs,
        m_QueueLatencyHistogram.size() - 1)]++;
    m_MaxQueueLatencyMs = std::max(m_MaxQueueLatencyMs, queueLatencyMs);

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    Uint32 afterRender = SDL_GetTicks();

    const uint32_t rendererCallLatencyMs = afterRender - beforeRender;
    m_VideoStats->totalRenderTime += rendererCallLatencyMs;
    m_RendererCallLatencyHistogram[std::min<uint32_t>(
        rendererCallLatencyMs,
        m_RendererCallLatencyHistogram.size() - 1)]++;
    m_MaxRendererCallLatencyMs = std::max(m_MaxRendererCallLatencyMs, rendererCallLatencyMs);
    m_VideoStats->renderedFrames++;

    if (frame->pts >= 0) {
        StationConnectAvSync::publishVideoClock(frame->pts, afterRender);
    }

    if (m_AvSyncTelemetryEnabled && frame->pts >= 0 &&
            (m_LastVideoTelemetryTime == 0 || afterRender - m_LastVideoTelemetryTime >= 1000)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "StationConnect A/V video clock: media=%lld render=%u queue=%u renderer=%u",
                    static_cast<long long>(frame->pts),
                    afterRender,
                    queueLatencyMs,
                    rendererCallLatencyMs);
        m_LastVideoTelemetryTime = afterRender;
    }
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : m_RenderQueueHistory) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        int queueDepth = m_RenderQueue.count();
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        recordFrameDrop(DropReason::RenderCatchUp, frame, queueDepth, frameDropTarget);
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::recordFrameDrop(DropReason reason, AVFrame* frame, int queueDepth, int targetDepth)
{
    const char* reasonName = "unknown";

    m_VideoStats->pacerDroppedFrames++;
    switch (reason) {
    case DropReason::PacingCatchUp:
        m_VideoStats->pacingQueueDroppedFrames++;
        reasonName = "pacing-catch-up";
        break;
    case DropReason::RenderCatchUp:
        m_VideoStats->renderQueueDroppedFrames++;
        reasonName = "render-catch-up";
        break;
    case DropReason::QueueOverflow:
        m_VideoStats->queueOverflowDroppedFrames++;
        reasonName = "queue-overflow";
        break;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Client pacer dropped frame: reason=%s age=%u ms queue=%d target=%d total=%u",
                reasonName,
                SDL_GetTicks() - (Uint32)frame->pkt_dts,
                queueDepth,
                targetDepth,
                m_VideoStats->pacerDroppedFrames);
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        recordFrameDrop(DropReason::QueueOverflow, frame, queue.size() + 1, MAX_QUEUED_FRAMES);
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
