#pragma once
#include "sessiongeometry.h"
#include "videoframe.h"
#include <QQuickItem>
#include <atomic>
#include <mutex>
#include <condition_variable>

struct QtVideoStatus
{
    enum Error { None, WrongBackend, ImportFailed, PrecisionOrDrawFailed, FenceFailed };
    std::atomic<Error> error{None};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> imports{0};
    std::atomic<uint64_t> synchronizations{0};
    std::atomic<uint64_t> swaps{0};
    bool visibleAtDelivery = false, exposedAtDelivery = false; // protected by presentationMutex
    std::atomic<bool> livePresented{false};
    std::mutex presentationMutex;
    std::condition_variable presentationChanged;
    QtVideoFrame::Ptr drawnFrame;
    std::weak_ptr<const QtVideoFrame> swappedFrame;
    std::chrono::steady_clock::time_point drawStarted;
};

// Shared GUI-owned frame/generation state and presentation feedback. Platform
// nodes only import and draw pixels; they do not implement their own queues.
class QtVideoItem : public QQuickItem
{
public:
    explicit QtVideoItem(QQuickItem* parent = nullptr);
    ~QtVideoItem() override;
    void setFrame(QtVideoFrame::Ptr frame);
    void setPresentation(const QtSessionGeometry& geometry);
    void setPrimaryOutput(bool primary);
    void beginGeneration(std::shared_ptr<QtVideoStatus> status);
    std::shared_ptr<const QtVideoStatus> status() const { return m_Status; }
    std::shared_ptr<QtVideoStatus> presentationFeedback() const { return m_Status; }
protected:
    QtVideoFrame::Ptr m_Frame;
    QtSessionGeometry m_Geometry;
    std::shared_ptr<QtVideoStatus> m_Status = std::make_shared<QtVideoStatus>();
private:
    QMetaObject::Connection m_SwapConnection;
    QMetaObject::Connection m_WindowConnection;
    bool m_PrimaryOutput = false;
    void connectPresentationFeedback();
};
