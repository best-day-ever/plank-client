#pragma once

#include "backend/displayplanner.h"
#include "backend/displayprofile.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class DisplayMonitor;

// Display setup, exposed to QML as the DisplaySetup singleton
// (DisplaySetupPanel.qml, DisplaySetupDialog.qml, DisplayAdvancedPage.qml,
// the onboarding "displays" step, Settings > Displays and the change banner).
// All layout logic lives in DisplayPlanner; this class keeps the profile being
// edited, re-plans on every change and stores the result (DisplayProfile).
//
// An edit starts with begin(hostId, ...) (empty for "every workstation"),
// changes the working profile through the set*() calls and ends with
// accept() (saved for the monitor set, globally or for that workstation) or
// cancel(). The preview uses the workstation's capabilities from its last
// connect, or the fleet's defaults ("expected") before the first one.
class DisplaySetupController : public QObject
{
    Q_OBJECT
    // The monitors connected right now.
    Q_PROPERTY(QString fingerprint READ fingerprint NOTIFY displaysChanged)
    Q_PROPERTY(QString label READ label NOTIFY displaysChanged)
    Q_PROPERTY(int monitorCount READ monitorCount NOTIFY displaysChanged)
    Q_PROPERTY(bool hasSavedLayout READ hasSavedLayout NOTIFY savedChanged)
    // The layout being edited.
    Q_PROPERTY(QString hostId READ hostId NOTIFY changed)
    Q_PROPERTY(QString hostName READ hostName NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    // Per monitor: key, name, builtIn, notch, primary, on, included,
    // clientX/Y/Width/Height (points), x/y/width/height (planned pixels),
    // sizeChoice, sizeOptions [{value, label, width, height}], sizeIndex,
    // badge, badgeText, backing, backingText, preference, entry, scaled.
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY changed)
    Q_PROPERTY(QVariantMap clientExtent READ clientExtent NOTIFY changed)
    Q_PROPERTY(QVariantMap desktop READ desktop NOTIFY changed)
    // {code, text, action, actionLabel, key}
    Q_PROPERTY(QVariantList warnings READ warnings NOTIFY changed)
    Q_PROPERTY(bool planOk READ planOk NOTIFY changed)
    Q_PROPERTY(QString planError READ planError NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(bool backingExpected READ backingExpected NOTIFY changed)
    // expected (not connected yet), arrangement, legacy, mac
    Q_PROPERTY(QString hostKind READ hostKind NOTIFY changed)
    Q_PROPERTY(QString presentation READ presentation NOTIFY changed)
    Q_PROPERTY(QString effectivePresentation READ effectivePresentation NOTIFY changed)
    Q_PROPERTY(QString scaling READ scaling NOTIFY changed)
    Q_PROPERTY(bool manual READ manual NOTIFY changed)
    // Where accept() saves: "global" (every workstation) or "workstation".
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY changed)
    // Settings > Displays.
    Q_PROPERTY(QVariantList savedLayouts READ savedLayouts NOTIFY savedChanged)
    Q_PROPERTY(bool askOnChange READ askOnChange WRITE setAskOnChange NOTIFY savedChanged)
    Q_PROPERTY(bool autoAccept READ autoAccept WRITE setAutoAccept NOTIFY savedChanged)
    // The change banner: "", "saved", "new" or "choose".
    Q_PROPERTY(QString bannerKind READ bannerKind NOTIFY bannerChanged)
    Q_PROPERTY(QString bannerText READ bannerText NOTIFY bannerChanged)

public:
    explicit DisplaySetupController(QObject* parent = nullptr);
    ~DisplaySetupController() override;

    Q_INVOKABLE void begin(const QString& hostId, const QString& hostName, const QString& reason);
    Q_INVOKABLE void setMonitorOn(const QString& key, bool on);
    Q_INVOKABLE void setPrimary(const QString& key);
    // exact, looks-like or preset:WxH
    Q_INVOKABLE void setSize(const QString& key, const QString& value);
    // Even, within what the workstation drives; false when refused.
    Q_INVOKABLE bool setCustomSize(const QString& key, int width, int height);
    // auto, physical or virtual
    Q_INVOKABLE void setPreference(const QString& key, const QString& preference);
    // Hand placement (advanced): switches the layout to manual.
    Q_INVOKABLE void setPosition(const QString& key, int x, int y);
    Q_INVOKABLE void setManual(bool manual);
    Q_INVOKABLE void setPresentation(const QString& presentation);
    Q_INVOKABLE void setScaling(const QString& scaling);
    Q_INVOKABLE void resetToProposal();
    // A warning's action: match-text-size, use-hevc, open-spaces; the others
    // (choose-screens, update-host) are handed to QML through actionRequested.
    Q_INVOKABLE void runAction(const QString& action, const QString& key);
    Q_INVOKABLE bool accept();
    Q_INVOKABLE void cancel();

    // A workstation's display mode: follow (the global layouts) or custom.
    Q_INVOKABLE void setHostMode(const QString& hostId, const QString& mode);
    // "Older workstations": a fixed layout for this workstation (the
    // RemoteDisplaySetup choices), which also switches it to legacy mode.
    Q_INVOKABLE bool useFixedLayout(const QString& hostId, int layoutChoice, const QString& virtualMode1,
                                    const QString& virtualMode2, int scalingChoice);
    // {mode: follow|custom|legacy, summary, configured}
    Q_INVOKABLE QVariantMap hostSummary(const QString& hostId);
    Q_INVOKABLE void forget(const QString& fingerprint);
    Q_INVOKABLE void dismissBanner();
    Q_INVOKABLE void refreshDisplays();
    // No saved layout for the monitors connected now.
    Q_INVOKABLE bool needsSetup() const;
    // Saves the proposal for the current monitors (auto-accept).
    Q_INVOKABLE void acceptProposal();

    QString fingerprint() const { return m_Fingerprint; }
    QString label() const { return m_Label; }
    int monitorCount() const { return int(m_Displays.size()); }
    bool hasSavedLayout() const;
    QString hostId() const { return m_HostId; }
    QString hostName() const { return m_HostName; }
    QString reason() const { return m_Reason; }
    QVariantList monitors() const;
    QVariantMap clientExtent() const;
    QVariantMap desktop() const;
    QVariantList warnings() const;
    bool planOk() const { return m_Plan.ok; }
    QString planError() const { return m_Plan.error; }
    QString summary() const { return summaryOf(m_Plan); }
    bool backingExpected() const { return m_Plan.backingExpected; }
    QString hostKind() const;
    QString presentation() const { return m_Profile.presentation; }
    QString effectivePresentation() const { return m_Plan.presentation; }
    QString scaling() const { return m_Profile.scaling; }
    bool manual() const { return m_Profile.manual; }
    QString scope() const { return m_Scope; }
    void setScope(const QString& scope);
    QVariantList savedLayouts() const;
    bool askOnChange() const;
    void setAskOnChange(bool ask);
    bool autoAccept() const;
    void setAutoAccept(bool accept);
    QString bannerKind() const { return m_BannerKind; }
    QString bannerText() const { return m_BannerText; }

    static QString summaryOf(const DisplayPlanner::Plan& plan);

signals:
    void changed();
    void displaysChanged();
    void savedChanged();
    void bannerChanged();
    void actionRequested(QString action, QString key);

private:
    void onDisplaysChanged(const QString& fingerprint, const QString& label, bool setChanged);
    void replan();
    void setBanner(const QString& kind, const QString& text);
    DisplayPlanner::HostInfo hostInfo(const QString& hostId) const;
    DisplayPlanner::Limits limits() const;
    const DisplayPlanner::Output* planned(const QString& key) const;

    DisplayMonitor* m_Monitor = nullptr;
    QVector<NvClientDisplay> m_Displays;
    QString m_Fingerprint;
    QString m_Label;
    DisplayProfile::Profile m_Profile;
    DisplayPlanner::HostInfo m_Host;
    DisplayPlanner::Plan m_Plan;
    QString m_HostId;
    QString m_HostName;
    QString m_Reason;
    QString m_Scope = QStringLiteral("global");
    QString m_BannerKind;
    QString m_BannerText;
    // Encoding modes whose decoder test runs or ran in this process.
    mutable QSet<QString> m_DecoderProbesStarted;
};
