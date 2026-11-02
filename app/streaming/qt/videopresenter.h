#pragma once

#include "decodedvideodelivery.h"
#ifdef Q_OS_MACOS
#include "metalvideonode.h"
using QtPlatformVideoItem = QtMetalVideoItem;
#else
#include "eglvideonode.h"
using QtPlatformVideoItem = QtEglVideoItem;
#endif
#include "videomailbox.h"
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>
#include <vector>

// GUI-thread owner. A decoder gets only immutable device references and a
// worker-safe delivery handle, never a QWindow or a native Wayland object.
class QtVideoPresenter final : public QObject
{
    Q_OBJECT
public:
    explicit QtVideoPresenter(QObject* parent = nullptr);
    ~QtVideoPresenter() override;
    void addOutput(QQuickWindow* window, const QtSessionGeometry& geometry, bool primary);
    void setGeometry(QQuickWindow* window, const QtSessionGeometry& geometry);
    void initializeDevice();
#ifdef Q_OS_LINUX
    QtSoftwareVideoDevice::Ptr softwareDevice() const { return m_SoftwareDevice; }
    QString vaapiRenderNode() const { return m_RenderNode; }
#endif
    bool hasPresentedLiveFrame() const;
    std::function<QtDecodedVideoDelivery()> deliveryFactory() const;
    void stop();

signals:
    void deviceReady(bool available, QString reason);
    void presentationFailed(QString reason);

private:
    struct State;
    struct Output {
        QPointer<QQuickWindow> window;
        QPointer<QtPlatformVideoItem> item;
        QtSessionGeometry geometry;
        bool primary;
    };
    void deliver(QtVideoFrame::Ptr frame, const QtVideoMailbox::Producer& producer);
#ifdef Q_OS_LINUX
    void finishDevice(QtSoftwareVideoDevice::Ptr device, QString node, QString error);
#endif
    std::shared_ptr<State> m_State;
    QtVideoMailbox m_Mailbox;
    std::vector<Output> m_Outputs;
#ifdef Q_OS_LINUX
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice;
    QString m_RenderNode;
#endif
    bool m_Started = false;
    QTimer m_ErrorTimer;
    std::shared_ptr<QtVideoStatus> m_DisplayedGeneration;
};
