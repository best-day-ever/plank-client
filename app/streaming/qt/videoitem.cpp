#include "videoitem.h"
#include <QQuickWindow>
#include <QThread>

QtVideoItem::QtVideoItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setZ(-1);
    m_WindowConnection = connect(this, &QQuickItem::windowChanged, this,
                                 [this] { connectPresentationFeedback(); });
}

QtVideoItem::~QtVideoItem()
{
    // QQuickItem's destructor detaches the item from its window and emits
    // windowChanged before QObject disconnects its signals. Do not invoke a
    // derived method after the derived object's lifetime has ended.
    QObject::disconnect(m_WindowConnection);
    QObject::disconnect(m_SwapConnection);
}

void QtVideoItem::setPrimaryOutput(bool primary)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_PrimaryOutput = primary;
    connectPresentationFeedback();
}

void QtVideoItem::beginGeneration(std::shared_ptr<QtVideoStatus> status)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!status || status == m_Status) return;
    m_Status = std::move(status);
    m_Frame.reset();
    connectPresentationFeedback();
    update();
}

void QtVideoItem::connectPresentationFeedback()
{
    QObject::disconnect(m_SwapConnection);
    if (!window()) return;
    const auto status = m_Status;
    const bool primary = m_PrimaryOutput;
    // Direct: frameSwapped comes from Qt's render thread. No queue of GUI
    // events, no raw item/window capture, and one clock source for a span.
    m_SwapConnection = connect(window(), &QQuickWindow::frameSwapped, this, [status, primary] {
        ++status->swaps;
        QtVideoFrame::Ptr frame;
        std::chrono::steady_clock::time_point started;
        {
            std::lock_guard<std::mutex> lock(status->presentationMutex);
            frame = std::move(status->drawnFrame);
            started = status->drawStarted;
            if (frame) {
                status->swappedFrame = frame;
                if (!frame->isQualification()) status->livePresented.store(true);
            }
            status->presentationChanged.notify_all();
        }
        if (frame && primary) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            frame->presented(uint64_t(elapsed));
        }
    }, Qt::DirectConnection);
}

void QtVideoItem::setFrame(QtVideoFrame::Ptr frame)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_Frame = std::move(frame);
    update();
}

void QtVideoItem::setPresentation(const QtSessionGeometry& geometry)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_Geometry = geometry;
    setSize(geometry.logicalSize);
    update();
}

