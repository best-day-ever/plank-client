#pragma once

#include "sessionwindow.h"
#include <QWindow>
#include <memory>

// Cocoa owns the native window/Spaces behavior. Qt owns the scene and events;
// a render worker draws that scene directly into a ten-bit SDR Metal drawable.
class QtMetalSessionShell final : public QWindow
{
    Q_OBJECT
public:
    QtMetalSessionShell();
    ~QtMetalSessionShell() override;
    QtSessionWindow& surface() { return *m_Surface; }
    static QSize defaultWindowedSize(QSize available, QSize desktop);
    void prepareForSession(QSize desktop, bool fullscreen);
    void showSession(bool fullscreen);
    void stopRendering();
    bool renderingStopped() const;
    bool ownsKeyboard() const;
    void setKeepAwake(bool enabled);

signals:
    void renderingFailed(QString reason);
    void renderingFinished();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct Rendering;
    void requestRender();
    void synchronizeFrame();
    void frameFinished();
    void updateSceneSize();
    void updateKeepAwake();
    std::unique_ptr<Rendering> m_Rendering;
    std::unique_ptr<QtSessionWindow> m_Surface;
    QSize m_WindowedSize;
    bool m_Queued = false;
    bool m_Dirty = true;
    bool m_RenderingFrame = false;
    bool m_RestoreWindowedSize = false;
    bool m_KeepAwake = false;
};
