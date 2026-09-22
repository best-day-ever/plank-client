#include "displaysetupcontroller.h"

#include "backend/clientdisplayprobe.h"
#include "backend/displaymonitor.h"
#include "backend/plankbroker.h"
#include "backend/remotedisplaysetup.h"
#include "backend/remotestreamsetup.h"
#include "settings/streamingpreferences.h"

#ifdef Q_OS_DARWIN
#include "streaming/macdisplayinfo.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QLocale>
#include <QSettings>
#include <QUrl>

namespace {

QString sizeText(const QSize& size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

QString badgeText(const QString& badge)
{
    if (badge == QLatin1String("exact")) return DisplaySetupController::tr("Pixel-exact");
    if (badge == QLatin1String("looks-like")) return DisplaySetupController::tr("Looks like");
    if (badge == QLatin1String("preset")) return DisplaySetupController::tr("Screen mode");
    if (badge == QLatin1String("custom")) return DisplaySetupController::tr("Custom");
    if (badge == QLatin1String("scaled")) return DisplaySetupController::tr("Scaled down");
    if (badge == QLatin1String("closest")) return DisplaySetupController::tr("Closest size");
    return QString();
}

QString backingText(DisplayArrangement::Backing backing)
{
    switch (backing) {
    case DisplayArrangement::Backing::Physical: return DisplaySetupController::tr("Workstation screen");
    case DisplayArrangement::Backing::PhysicalViewport: return DisplaySetupController::tr("Workstation screen, scaled");
    case DisplayArrangement::Backing::Virtual: return DisplaySetupController::tr("Virtual display");
    case DisplayArrangement::Backing::None: default: return QString();
    }
}

QVariantMap rect(int x, int y, int width, int height)
{
    return {{QStringLiteral("x"), x}, {QStringLiteral("y"), y},
            {QStringLiteral("width"), width}, {QStringLiteral("height"), height}};
}

}

DisplaySetupController::DisplaySetupController(QObject* parent)
    : QObject(parent),
      m_Monitor(new DisplayMonitor(this))
{
    m_Displays = m_Monitor->displays();
    m_Fingerprint = m_Monitor->fingerprint();
    m_Label = m_Monitor->label();
    connect(m_Monitor, &DisplayMonitor::displaysChanged, this, &DisplaySetupController::onDisplaysChanged);

    // Layouts saved before display profiles: once, never deleting a key.
    QSettings settings;
    const int migrated = DisplayProfile::migrate(settings, m_Fingerprint, m_Label,
                                                 DisplayPlanner::proposal(m_Displays));
    if (migrated > 0) {
        qInfo() << "Display setup: migrated" << migrated << "workstation layout(s) to display profiles";
    }
    if (DisplayProfile::chooseBannerPending(settings)) {
        setBanner(QStringLiteral("choose"),
                  tr("BDE Fernweh can now show every one of your screens on the workstation, each at its exact size. "
                     "Your current layout was kept."));
    }
    begin(QString(), QString(), QString());
}

DisplaySetupController::~DisplaySetupController() = default;

DisplayPlanner::HostInfo DisplaySetupController::hostInfo(const QString& hostId) const
{
    DisplayPlanner::HostInfo host;
    QSettings settings;
    RemoteStreamSetup::Capabilities caps;
    if (PlankBroker::isHostId(hostId)) {
        caps = RemoteStreamSetup::loadCapabilities(settings, hostId);
    }
    host.known = caps.known;
    host.platform = caps.platform;
    host.featureFlags = caps.featureFlags;
    if (!caps.displayCapabilities.isEmpty()) {
        DisplayArrangement::Capabilities::fromJson(
                    QJsonDocument::fromJson(caps.displayCapabilities.toUtf8()).object(), host.capabilities);
    }
    // The encoding the next connect will use: this workstation's own
    // choice, else the remote access defaults, else the built-in default.
    const RemoteStreamSetup::Setup own = PlankBroker::isHostId(hostId) ?
                RemoteStreamSetup::loadHost(settings, hostId) : RemoteStreamSetup::Setup();
    const RemoteStreamSetup::Resolution stream = RemoteStreamSetup::resolve(
                own, nullptr, RemoteStreamSetup::loadDefaults(settings), caps.platform);
    host.encodingMode = StreamingPreferences::plankEncodingMode(stream.setup.videoProfile);
    return host;
}

DisplayPlanner::Limits DisplaySetupController::limits() const
{
    DisplayPlanner::Limits limits;
#ifdef Q_OS_DARWIN
    limits.separateSpaces = MacDisplayInfo::screensHaveSeparateSpaces();
#endif
    return limits;
}

void DisplaySetupController::begin(const QString& hostId, const QString& hostName, const QString& reason)
{
    m_HostId = PlankBroker::isHostId(hostId) ? hostId : QString();
    m_HostName = hostName;
    m_Reason = reason;
    m_Monitor->refresh();
    m_Displays = m_Monitor->displays();
    m_Fingerprint = m_Monitor->fingerprint();
    m_Label = m_Monitor->label();
    m_Host = hostInfo(m_HostId);
    QSettings settings;
    const DisplayProfile::Resolved resolved = DisplayProfile::resolveForHost(settings, m_HostId, m_Fingerprint);
    m_Profile = resolved.source == DisplayProfile::Resolved::Global ||
            resolved.source == DisplayProfile::Resolved::HostCustom ?
                resolved.profile : DisplayPlanner::proposal(m_Displays);
    // Global layouts may also be looked at while a workstation is legacy.
    if (resolved.source == DisplayProfile::Resolved::Legacy) {
        DisplayProfile::Profile global;
        if (DisplayProfile::loadGlobal(settings, m_Fingerprint, global)) m_Profile = global;
    }
    m_Scope = !m_HostId.isEmpty() && DisplayProfile::hostMode(settings, m_HostId) == DisplayProfile::HostMode::Custom ?
                QStringLiteral("workstation") : QStringLiteral("global");
    replan();
    emit displaysChanged();
}

void DisplaySetupController::replan()
{
    m_Plan = DisplayPlanner::plan(m_Displays, m_Profile, m_Host, limits());
    emit changed();
}

const DisplayPlanner::Output* DisplaySetupController::planned(const QString& key) const
{
    return m_Plan.find(key);
}

void DisplaySetupController::setMonitorOn(const QString& key, bool on)
{
    if (planned(key) == nullptr) return;
    m_Profile.choice(key).on = on;
    replan();
}

void DisplaySetupController::setPrimary(const QString& key)
{
    if (planned(key) == nullptr) return;
    m_Profile.primary = key;
    m_Profile.choice(key).on = true;
    replan();
}

void DisplaySetupController::setSize(const QString& key, const QString& value)
{
    if (planned(key) == nullptr) return;
    DisplayProfile::MonitorChoice& choice = m_Profile.choice(key);
    DisplayProfile::MonitorChoice parsed = choice;
    if (!DisplayProfile::sizeFromText(value, parsed)) return;
    choice.size = parsed.size;
    choice.fixedSize = parsed.fixedSize;
    replan();
}

bool DisplaySetupController::setCustomSize(const QString& key, int width, int height)
{
    const DisplayPlanner::Output* output = planned(key);
    if (output == nullptr) return false;
    const DisplayArrangement::Capabilities caps = m_Host.capabilities.valid ?
                m_Host.capabilities : DisplayArrangement::Capabilities::fleetDefault();
    width -= width % 2;
    height -= height % 2;
    if (width < caps.minOutput.width() || height < caps.minOutput.height() ||
            width > caps.maxOutput.width() || height > caps.maxOutput.height() ||
            qint64(width) * height > caps.maxPixels) {
        return false;
    }
    DisplayProfile::MonitorChoice& choice = m_Profile.choice(key);
    choice.size = DisplayProfile::SizeMode::Custom;
    choice.fixedSize = QSize(width, height);
    replan();
    return true;
}

void DisplaySetupController::setPreference(const QString& key, const QString& preference)
{
    DisplayArrangement::Preference parsed;
    if (planned(key) == nullptr || !DisplayArrangement::preferenceFromName(preference, parsed)) return;
    m_Profile.choice(key).backing = preference;
    replan();
}

void DisplaySetupController::setManual(bool manual)
{
    if (manual && !m_Profile.manual) {
        // Start from what the desk arrangement gives now.
        for (const DisplayPlanner::Output& output : std::as_const(m_Plan.outputs)) {
            if (!output.included) continue;
            DisplayProfile::MonitorChoice& choice = m_Profile.choice(output.key);
            choice.hasPosition = true;
            choice.position = output.position;
        }
    }
    m_Profile.manual = manual;
    replan();
}

void DisplaySetupController::setPosition(const QString& key, int x, int y)
{
    if (planned(key) == nullptr) return;
    if (!m_Profile.manual) setManual(true);
    DisplayProfile::MonitorChoice& choice = m_Profile.choice(key);
    choice.hasPosition = true;
    choice.position = QPoint(x - x % 2, y - y % 2);
    replan();
}

void DisplaySetupController::setPresentation(const QString& presentation)
{
    if (presentation != QLatin1String("windows") && presentation != QLatin1String("single")) return;
    m_Profile.presentation = presentation;
    replan();
}

void DisplaySetupController::setScaling(const QString& scaling)
{
    if (scaling != QLatin1String("native") && scaling != QLatin1String("fit")) return;
    m_Profile.scaling = scaling;
    replan();
}

void DisplaySetupController::resetToProposal()
{
    m_Profile = DisplayPlanner::proposal(m_Displays);
    replan();
}

void DisplaySetupController::runAction(const QString& action, const QString& key)
{
    if (action == QLatin1String("match-text-size")) {
        // Retina screens at "looks like" so text is the same size everywhere.
        for (const DisplayPlanner::Output& output : std::as_const(m_Plan.outputs)) {
            if (output.included && output.looksLikeSize.isValid() && output.size == output.exactSize) {
                m_Profile.choice(output.key).size = DisplayProfile::SizeMode::LooksLike;
            }
        }
        replan();
    } else if (action == QLatin1String("use-hevc")) {
        QSettings settings;
        const StreamingPreferences::PlankVideoProfile hevc = StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444;
        if (!m_HostId.isEmpty()) {
            RemoteStreamSetup::Setup setup = RemoteStreamSetup::loadHost(settings, m_HostId);
            if (setup.mode != RemoteStreamSetup::Custom) {
                const RemoteStreamSetup::Resolution current = RemoteStreamSetup::resolve(
                            setup, nullptr, RemoteStreamSetup::loadDefaults(settings), m_Host.platform);
                setup = current.setup;
            }
            setup.mode = RemoteStreamSetup::Custom;
            setup.captureSource = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
            setup.videoProfile = hevc;
            RemoteStreamSetup::saveHost(settings, m_HostId, setup);
        } else {
            RemoteStreamSetup::Setup defaults = RemoteStreamSetup::loadDefaults(settings);
            if (defaults.mode != RemoteStreamSetup::Custom) {
                defaults = RemoteStreamSetup::builtInDefaults(RemoteStreamSetup::LinuxPlatform);
            }
            defaults.captureSource = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
            defaults.videoProfile = hevc;
            RemoteStreamSetup::saveDefaults(settings, defaults);
        }
        settings.sync();
        m_Host = hostInfo(m_HostId);
        replan();
    } else if (action == QLatin1String("open-spaces")) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("x-apple.systempreferences:com.apple.Desktop-Settings.extension")));
    } else {
        emit actionRequested(action, key);
    }
}

bool DisplaySetupController::accept()
{
    if (m_Fingerprint.isEmpty() || !m_Plan.ok) return false;
    QSettings settings;
    if (m_Scope == QLatin1String("workstation") && !m_HostId.isEmpty()) {
        DisplayProfile::saveHost(settings, m_HostId, m_Fingerprint, m_Profile);
        DisplayProfile::setHostMode(settings, m_HostId, DisplayProfile::HostMode::Custom);
    } else {
        DisplayProfile::saveGlobal(settings, m_Fingerprint, m_Label, m_Profile);
        if (!m_HostId.isEmpty()) DisplayProfile::setHostMode(settings, m_HostId, DisplayProfile::HostMode::Follow);
    }
    DisplayProfile::clearChooseBanner(settings);
    settings.sync();
    if (m_BannerKind == QLatin1String("new") || m_BannerKind == QLatin1String("choose")) setBanner(QString(), QString());
    emit savedChanged();
    return true;
}

void DisplaySetupController::cancel()
{
    // Back to what is saved, for the Settings summary.
    begin(QString(), QString(), QString());
}

void DisplaySetupController::setHostMode(const QString& hostId, const QString& mode)
{
    if (!PlankBroker::isHostId(hostId)) return;
    QSettings settings;
    DisplayProfile::setHostMode(settings, hostId, mode == QLatin1String("custom") ? DisplayProfile::HostMode::Custom :
                                                                                     DisplayProfile::HostMode::Follow);
    settings.sync();
    if (hostId == m_HostId) {
        m_Scope = mode == QLatin1String("custom") ? QStringLiteral("workstation") : QStringLiteral("global");
        emit changed();
    }
    emit savedChanged();
}

bool DisplaySetupController::useFixedLayout(const QString& hostId, int layoutChoice, const QString& virtualMode1,
                                            const QString& virtualMode2, int scalingChoice)
{
    if (!PlankBroker::isHostId(hostId)) return false;
    RemoteDisplaySetup::Setup setup;
    setup.hostLayout = RemoteDisplaySetup::layoutForChoice(layoutChoice);
    setup.virtualMode1 = virtualMode1;
    setup.virtualMode2 = virtualMode2;
    setup.scalingMode = RemoteDisplaySetup::scalingForChoice(scalingChoice);
    QSettings settings;
    if (!RemoteDisplaySetup::save(settings, hostId, setup)) return false;
    DisplayProfile::setHostMode(settings, hostId, DisplayProfile::HostMode::Legacy);
    settings.sync();
    emit savedChanged();
    return true;
}

QVariantMap DisplaySetupController::hostSummary(const QString& hostId)
{
    QVariantMap result;
    if (!PlankBroker::isHostId(hostId)) return result;
    QSettings settings;
    const DisplayProfile::HostMode mode = DisplayProfile::hostMode(settings, hostId);
    result.insert(QStringLiteral("mode"), mode == DisplayProfile::HostMode::Custom ? QStringLiteral("custom") :
                                          mode == DisplayProfile::HostMode::Legacy ? QStringLiteral("legacy") :
                                                                                     QStringLiteral("follow"));
    const DisplayProfile::Resolved resolved = DisplayProfile::resolveForHost(settings, hostId, m_Fingerprint);
    result.insert(QStringLiteral("configured"), resolved.source != DisplayProfile::Resolved::None);
    if (resolved.source == DisplayProfile::Resolved::Legacy) {
        const RemoteDisplaySetup::Setup legacy = RemoteDisplaySetup::load(settings, hostId);
        QString text;
        switch (RemoteDisplaySetup::choiceForLayout(legacy.hostLayout)) {
        case RemoteDisplaySetup::MatchClient: text = tr("Fixed: match my displays (older layout)"); break;
        case RemoteDisplaySetup::Physical: text = tr("Fixed: the workstation's own screens"); break;
        case RemoteDisplaySetup::SingleVirtual:
            text = tr("Fixed: one virtual display, %1").arg(ClientDisplayProbe::modeText(legacy.virtualMode1));
            break;
        case RemoteDisplaySetup::DualVirtual:
            text = tr("Fixed: two virtual displays, %1 + %2").arg(ClientDisplayProbe::modeText(legacy.virtualMode1),
                                                                 ClientDisplayProbe::modeText(legacy.virtualMode2));
            break;
        default: text = tr("Fixed layout"); break;
        }
        result.insert(QStringLiteral("summary"), text);
        return result;
    }
    const DisplayProfile::Profile profile = resolved.source == DisplayProfile::Resolved::None ?
                DisplayPlanner::proposal(m_Displays) : resolved.profile;
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(m_Displays, profile, hostInfo(hostId), limits());
    result.insert(QStringLiteral("summary"), resolved.source == DisplayProfile::Resolved::None ?
                      tr("Not set up for these screens yet") : summaryOf(plan));
    return result;
}

void DisplaySetupController::forget(const QString& fingerprint)
{
    QSettings settings;
    DisplayProfile::forgetGlobal(settings, fingerprint);
    settings.sync();
    emit savedChanged();
}

void DisplaySetupController::setBanner(const QString& kind, const QString& text)
{
    if (kind == m_BannerKind && text == m_BannerText) return;
    m_BannerKind = kind;
    m_BannerText = text;
    emit bannerChanged();
}

void DisplaySetupController::dismissBanner()
{
    if (m_BannerKind == QLatin1String("choose")) {
        QSettings settings;
        DisplayProfile::clearChooseBanner(settings);
    }
    setBanner(QString(), QString());
}

void DisplaySetupController::refreshDisplays()
{
    m_Monitor->refresh();
}

bool DisplaySetupController::hasSavedLayout() const
{
    QSettings settings;
    DisplayProfile::Profile profile;
    return DisplayProfile::loadGlobal(settings, m_Fingerprint, profile);
}

bool DisplaySetupController::needsSetup() const
{
    return !m_Fingerprint.isEmpty() && !hasSavedLayout();
}

void DisplaySetupController::acceptProposal()
{
    if (m_Fingerprint.isEmpty()) return;
    QSettings settings;
    DisplayProfile::saveGlobal(settings, m_Fingerprint, m_Label, DisplayPlanner::proposal(m_Displays));
    settings.sync();
    emit savedChanged();
}

void DisplaySetupController::onDisplaysChanged(const QString& fingerprint, const QString& label, bool setChanged)
{
    m_Displays = m_Monitor->displays();
    m_Fingerprint = fingerprint;
    m_Label = label;
    qInfo() << "Display setup: screens changed;" << (setChanged ? "new monitor set" : "same monitors")
            << "count" << m_Displays.size();
    if (setChanged && !m_Fingerprint.isEmpty()) {
        QSettings settings;
        if (hasSavedLayout()) {
            setBanner(QStringLiteral("saved"), tr("Using your saved layout for %1.").arg(label));
        } else if (DisplayProfile::autoAccept(settings)) {
            acceptProposal();
            setBanner(QStringLiteral("saved"), tr("Using the suggested layout for %1.").arg(label));
        } else if (DisplayProfile::askOnChange(settings)) {
            setBanner(QStringLiteral("new"), tr("New screen setup: %1.").arg(label));
        } else {
            setBanner(QString(), QString());
        }
    }
    // An open setup follows the screens; choices for monitors still there stay.
    m_Host = hostInfo(m_HostId);
    replan();
    emit displaysChanged();
    emit savedChanged();
}

QVariantList DisplaySetupController::monitors() const
{
    QVariantList list;
    for (const DisplayPlanner::Output& output : m_Plan.outputs) {
        QVariantList options;
        int sizeIndex = -1;
        for (const DisplayPlanner::SizeOption& option : output.sizeOptions) {
            if (option.value == output.sizeChoice ||
                    (option.value == QLatin1String("custom") && output.sizeChoice.startsWith(QLatin1String("custom:")))) {
                sizeIndex = int(options.size());
            }
            options.append(QVariantMap {{QStringLiteral("value"), option.value}, {QStringLiteral("label"), option.label},
                                        {QStringLiteral("width"), option.size.width()},
                                        {QStringLiteral("height"), option.size.height()}});
        }
        if (sizeIndex < 0 && output.sizeChoice.startsWith(QLatin1String("preset:"))) {
            // A saved mode this workstation does not list (another host's):
            // shown just before "Custom…".
            sizeIndex = qMax(0, int(options.size()) - 1);
            QString label = output.sizeChoice.mid(7);
            label.replace(QLatin1Char('x'), QStringLiteral(" × "));
            options.insert(sizeIndex, QVariantMap {{QStringLiteral("value"), output.sizeChoice},
                                                   {QStringLiteral("label"), label},
                                                   {QStringLiteral("width"), 0}, {QStringLiteral("height"), 0}});
        }
        QVariantMap map {
            {QStringLiteral("key"), output.key},
            {QStringLiteral("name"), output.name},
            {QStringLiteral("builtIn"), output.builtIn},
            {QStringLiteral("notch"), output.notch},
            {QStringLiteral("primary"), output.primary},
            {QStringLiteral("on"), output.on},
            {QStringLiteral("included"), output.included},
            {QStringLiteral("clientX"), output.clientBounds.x()},
            {QStringLiteral("clientY"), output.clientBounds.y()},
            {QStringLiteral("clientWidth"), output.clientBounds.width()},
            {QStringLiteral("clientHeight"), output.clientBounds.height()},
            {QStringLiteral("exactText"), sizeText(output.exactSize)},
            {QStringLiteral("x"), output.position.x()},
            {QStringLiteral("y"), output.position.y()},
            {QStringLiteral("width"), output.size.width()},
            {QStringLiteral("height"), output.size.height()},
            {QStringLiteral("sizeText"), output.included ? sizeText(output.size) : QString()},
            {QStringLiteral("sizeChoice"), output.sizeChoice},
            {QStringLiteral("sizeOptions"), options},
            {QStringLiteral("sizeIndex"), qMax(0, sizeIndex)},
            {QStringLiteral("badge"), output.included ? output.badge : QString()},
            {QStringLiteral("badgeText"), output.included ? badgeText(output.badge) : QString()},
            {QStringLiteral("backing"), DisplayArrangement::backingName(output.backing)},
            {QStringLiteral("backingText"), backingText(output.backing)},
            {QStringLiteral("preference"), output.preference},
            {QStringLiteral("entry"), output.arrangementIndex},
            {QStringLiteral("scaled"), output.scaled},
        };
        list.append(map);
    }
    return list;
}

QVariantMap DisplaySetupController::clientExtent() const
{
    QRect extent;
    for (const NvClientDisplay& display : m_Displays) extent = extent.united(display.bounds);
    return rect(extent.x(), extent.y(), extent.width(), extent.height());
}

QVariantMap DisplaySetupController::desktop() const
{
    return rect(0, 0, m_Plan.canvas.width(), m_Plan.canvas.height());
}

QVariantList DisplaySetupController::warnings() const
{
    QVariantList list;
    for (const DisplayPlanner::Warning& warning : m_Plan.warnings) {
        list.append(QVariantMap {{QStringLiteral("code"), warning.code}, {QStringLiteral("text"), warning.text},
                                 {QStringLiteral("action"), warning.action},
                                 {QStringLiteral("actionLabel"), warning.actionLabel},
                                 {QStringLiteral("key"), warning.key}});
    }
    return list;
}

QString DisplaySetupController::hostKind() const
{
    if (!m_Plan.macMode.isEmpty()) return QStringLiteral("mac");
    if (m_Plan.legacy) return QStringLiteral("legacy");
    if (m_Plan.backingExpected) return QStringLiteral("expected");
    return QStringLiteral("arrangement");
}

void DisplaySetupController::setScope(const QString& scope)
{
    const QString next = scope == QLatin1String("workstation") && !m_HostId.isEmpty() ?
                QStringLiteral("workstation") : QStringLiteral("global");
    if (next == m_Scope) return;
    m_Scope = next;
    emit changed();
}

QVariantList DisplaySetupController::savedLayouts() const
{
    QSettings settings;
    QVariantList list;
    for (const DisplayProfile::SavedSet& set : DisplayProfile::savedSets(settings)) {
        const QDateTime saved = QDateTime::fromString(set.saved, Qt::ISODate);
        list.append(QVariantMap {
            {QStringLiteral("fingerprint"), set.fingerprint},
            {QStringLiteral("label"), set.label.isEmpty() ? tr("Unnamed screens") : set.label},
            {QStringLiteral("saved"), saved.isValid() ? QLocale().toString(saved.toLocalTime().date(), QLocale::ShortFormat)
                                                      : QString()},
            {QStringLiteral("current"), set.fingerprint == m_Fingerprint},
        });
    }
    return list;
}

bool DisplaySetupController::askOnChange() const
{
    QSettings settings;
    return DisplayProfile::askOnChange(settings);
}

void DisplaySetupController::setAskOnChange(bool ask)
{
    QSettings settings;
    DisplayProfile::setAskOnChange(settings, ask);
    emit savedChanged();
}

bool DisplaySetupController::autoAccept() const
{
    QSettings settings;
    return DisplayProfile::autoAccept(settings);
}

void DisplaySetupController::setAutoAccept(bool accept)
{
    QSettings settings;
    DisplayProfile::setAutoAccept(settings, accept);
    emit savedChanged();
}

QString DisplaySetupController::summaryOf(const DisplayPlanner::Plan& plan)
{
    if (!plan.ok) return plan.error;
    QStringList sizes;
    for (const DisplayPlanner::Output& output : plan.outputs) {
        if (output.included) sizes.append(sizeText(output.size));
    }
    if (!plan.macMode.isEmpty()) {
        return tr("Workstation desktop %1").arg(ClientDisplayProbe::modeText(plan.macMode));
    }
    if (plan.legacy) {
        return tr("%1 (older workstation layout)").arg(sizes.join(QStringLiteral(" + ")));
    }
    if (sizes.size() == 1) return tr("One screen, %1").arg(sizes.first());
    return tr("%n screen(s): %1, desktop %2", nullptr, int(sizes.size()))
            .arg(sizes.join(QStringLiteral(" + ")), sizeText(plan.canvas));
}
