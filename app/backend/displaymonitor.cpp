#include "displaymonitor.h"

#include "clientdisplayprobe.h"

#include <QGuiApplication>
#include <QScreen>
#include <QStringList>

#ifdef Q_OS_DARWIN
#include "streaming/macdisplayinfo.h"

#include <ApplicationServices/ApplicationServices.h>
#endif

namespace {

// Everything the display setup cares about, as text: a change here is worth
// reporting, anything else (a colour profile, say) is not.
QString signature(const QVector<NvClientDisplay>& displays)
{
    QStringList parts;
    for (const NvClientDisplay& display : displays) {
        parts.append(QStringLiteral("%1 %2,%3 %4x%5 %6x%7 %8x%9 r%10 m%11")
                     .arg(display.key)
                     .arg(display.bounds.x()).arg(display.bounds.y())
                     .arg(display.bounds.width()).arg(display.bounds.height())
                     .arg(display.backingSize.width()).arg(display.backingSize.height())
                     .arg(display.fullscreenSize.width()).arg(display.fullscreenSize.height())
                     .arg(display.rotation).arg(display.main ? 1 : 0));
    }
    return parts.join(QLatin1Char(';'));
}

#ifdef Q_OS_DARWIN
void reconfigured(CGDirectDisplayID, CGDisplayChangeSummaryFlags flags, void* context)
{
    // Called once before (BeginConfiguration) and once per display after.
    if (flags & kCGDisplayBeginConfigurationFlag) return;
    auto* monitor = static_cast<DisplayMonitor*>(context);
    QMetaObject::invokeMethod(monitor, "refreshLater", Qt::QueuedConnection);
}
#endif

}

DisplayMonitor::DisplayMonitor(QObject* parent)
    : QObject(parent)
{
    m_Debounce.setSingleShot(true);
    m_Debounce.setInterval(DebounceMs);
    connect(&m_Debounce, &QTimer::timeout, this, &DisplayMonitor::check);
#ifdef Q_OS_DARWIN
    MacDisplayInfo::refresh();
    CGDisplayRegisterReconfigurationCallback(reconfigured, this);
#else
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
            watchScreen(screen);
            scheduleCheck();
        });
        connect(app, &QGuiApplication::screenRemoved, this, &DisplayMonitor::scheduleCheck);
        connect(app, &QGuiApplication::primaryScreenChanged, this, &DisplayMonitor::scheduleCheck);
        for (QScreen* screen : QGuiApplication::screens()) watchScreen(screen);
    }
#endif
    m_Displays = ClientDisplayProbe::probe();
    m_Fingerprint = ClientDisplayProbe::fingerprint(m_Displays);
    m_Label = ClientDisplayProbe::label(m_Displays);
    m_Signature = signature(m_Displays);
}

DisplayMonitor::~DisplayMonitor()
{
#ifdef Q_OS_DARWIN
    CGDisplayRemoveReconfigurationCallback(reconfigured, this);
#endif
}

void DisplayMonitor::watchScreen(QObject* object)
{
    auto* screen = qobject_cast<QScreen*>(object);
    if (screen == nullptr) return;
    connect(screen, &QScreen::geometryChanged, this, &DisplayMonitor::scheduleCheck);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, &DisplayMonitor::scheduleCheck);
}

void DisplayMonitor::scheduleCheck()
{
    m_Debounce.start();
}

void DisplayMonitor::refreshLater()
{
    scheduleCheck();
}

void DisplayMonitor::refresh()
{
    m_Debounce.stop();
    check();
}

void DisplayMonitor::check()
{
#ifdef Q_OS_DARWIN
    MacDisplayInfo::refresh();
#endif
    const QVector<NvClientDisplay> displays = ClientDisplayProbe::probe();
    const QString nextSignature = signature(displays);
    if (nextSignature == m_Signature) return;
    const QString nextFingerprint = ClientDisplayProbe::fingerprint(displays);
    const bool setChanged = nextFingerprint != m_Fingerprint;
    m_Displays = displays;
    m_Fingerprint = nextFingerprint;
    m_Label = ClientDisplayProbe::label(displays);
    m_Signature = nextSignature;
    emit displaysChanged(m_Fingerprint, m_Label, setChanged);
}
