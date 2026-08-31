#pragma once

#include <QObject>
#include <QRect>
#include <QQmlEngine>
#include <QVariantList>
#include <QVector>

class StreamingPreferences : public QObject
{
    Q_OBJECT

public:
    static StreamingPreferences* get(QQmlEngine *qmlEngine = nullptr);

    Q_INVOKABLE void save();

    void reload();

    enum AudioConfig
    {
        AC_STEREO,
        AC_51_SURROUND,
        AC_71_SURROUND
    };
    Q_ENUM(AudioConfig)

    enum StationConnectVideoProfile
    {
        SCVP_H264_10BIT_444,
        SCVP_H264_8BIT_422,
        SCVP_H264_8BIT_444,
        SCVP_H264_10BIT_422,
        SCVP_NVENC_H264_8BIT_444,
        SCVP_NVENC_HEVC_8BIT_444,
        SCVP_NVENC_HEVC_10BIT_444,
        SCVP_COUNT,
    };
    Q_ENUM(StationConnectVideoProfile)

    enum StationConnectCaptureSource
    {
        SCCS_NVFBC_8BIT,
        SCCS_X11_NATIVE10,
    };
    Q_ENUM(StationConnectCaptureSource)

    static bool isStationConnectVideoProfileValid(int profile)
    {
        return profile >= SCVP_H264_10BIT_444 &&
               profile < SCVP_COUNT;
    }

    static bool isStationConnectProfileValidForCaptureSource(
            int profile, int captureSource)
    {
        if (!isStationConnectVideoProfileValid(profile) ||
                captureSource < SCCS_NVFBC_8BIT ||
                captureSource > SCCS_X11_NATIVE10) {
            return false;
        }

        if (captureSource == SCCS_X11_NATIVE10) {
            return profile == SCVP_H264_10BIT_444 ||
                   profile == SCVP_NVENC_HEVC_10BIT_444;
        }

        return true;
    }

    static bool isStationConnectNvencProfile(int profile)
    {
        return profile >= SCVP_NVENC_H264_8BIT_444 &&
               profile <= SCVP_NVENC_HEVC_10BIT_444;
    }

    static constexpr int StationConnectBitrateMinimumKbps = 10000;
    static constexpr int StationConnectBitrateMaximumKbps = 150000;
    static constexpr int StationConnectBitrateStepKbps = 500;
    static constexpr int StationConnectH264DefaultBitrateKbps = 80000;
    static constexpr int StationConnectHevcDefaultBitrateKbps = 50000;

    static int stationConnectDefaultBitrateForProfile(int profile)
    {
        return profile == SCVP_NVENC_HEVC_8BIT_444 ||
               profile == SCVP_NVENC_HEVC_10BIT_444 ?
                   StationConnectHevcDefaultBitrateKbps :
                   StationConnectH264DefaultBitrateKbps;
    }

    static int clampStationConnectBitrate(int bitrateKbps)
    {
        return qBound(StationConnectBitrateMinimumKbps,
                      bitrateKbps,
                      StationConnectBitrateMaximumKbps);
    }

    static QVector<int> stationConnectDefaultProfileBitrates()
    {
        QVector<int> bitrates;
        bitrates.reserve(SCVP_COUNT);
        for (int profile = SCVP_H264_10BIT_444;
             profile < SCVP_COUNT;
             ++profile) {
            bitrates.append(stationConnectDefaultBitrateForProfile(profile));
        }
        return bitrates;
    }

    static QVariantList stationConnectProfileBitratesToVariantList(
            const QVector<int>& bitrates)
    {
        QVariantList values;
        values.reserve(SCVP_COUNT);
        for (int profile = SCVP_H264_10BIT_444;
             profile < SCVP_COUNT;
             ++profile) {
            values.append(stationConnectBitrateForProfile(bitrates, profile));
        }
        return values;
    }

    static bool stationConnectProfileBitratesFromVariantList(
            const QVariantList& values, QVector<int>& bitrates)
    {
        if (values.size() != SCVP_COUNT) {
            return false;
        }

        QVector<int> parsed;
        parsed.reserve(SCVP_COUNT);
        for (const QVariant& value : values) {
            bool ok = false;
            const int bitrateKbps = value.toInt(&ok);
            if (!ok || bitrateKbps != clampStationConnectBitrate(bitrateKbps)) {
                return false;
            }
            parsed.append(bitrateKbps);
        }
        bitrates = parsed;
        return true;
    }

    static int stationConnectBitrateForProfile(
            const QVector<int>& bitrates, int profile)
    {
        if (isStationConnectVideoProfileValid(profile) &&
                bitrates.size() == SCVP_COUNT) {
            return clampStationConnectBitrate(bitrates.at(profile));
        }
        return stationConnectDefaultBitrateForProfile(profile);
    }

    enum WindowMode
    {
        // Preserve the historical numeric values stored in QSettings.
        WM_FULLSCREEN_DESKTOP = 1,
        WM_WINDOWED = 2
    };
    Q_ENUM(WindowMode)

    // New entries must go at the end of the enum
    // to avoid renumbering existing entries (which
    // would affect existing user preferences).
    enum Language
    {
        LANG_AUTO,
        LANG_EN,
        LANG_FR,
        LANG_ZH_CN,
        LANG_DE,
        LANG_NB_NO,
        LANG_RU,
        LANG_ES,
        LANG_JA,
        LANG_VI,
        LANG_TH,
        LANG_KO,
        LANG_HU,
        LANG_NL,
        LANG_SV,
        LANG_TR,
        LANG_UK,
        LANG_ZH_TW,
        LANG_PT,
        LANG_PT_BR,
        LANG_EL,
        LANG_IT,
        LANG_HI,
        LANG_PL,
        LANG_CS,
        LANG_HE,
        LANG_CKB,
        LANG_LT,
        LANG_ET,
    };
    Q_ENUM(Language);

    enum CaptureSysKeysMode
    {
        CSK_OFF,
        CSK_FULLSCREEN,
        CSK_ALWAYS,
    };
    Q_ENUM(CaptureSysKeysMode);

    enum StationConnectUnreachableAction
    {
        SCUA_ASK,
        SCUA_DISCONNECT,
    };
    Q_ENUM(StationConnectUnreachableAction);

    Q_PROPERTY(int fps MEMBER fps NOTIFY displayModeChanged)
    Q_PROPERTY(bool enableVsync MEMBER enableVsync NOTIFY enableVsyncChanged)
    Q_PROPERTY(bool playAudioOnHost MEMBER playAudioOnHost NOTIFY playAudioOnHostChanged)
    Q_PROPERTY(bool enableMdns MEMBER enableMdns NOTIFY enableMdnsChanged)
    Q_PROPERTY(bool mdnsDiscoveryManaged MEMBER mdnsDiscoveryManaged CONSTANT)
    Q_PROPERTY(bool connectionWarnings MEMBER connectionWarnings NOTIFY connectionWarningsChanged)
    Q_PROPERTY(bool detectNetworkBlocking MEMBER detectNetworkBlocking NOTIFY detectNetworkBlockingChanged)
    Q_PROPERTY(int quicUdpPayloadMtu MEMBER quicUdpPayloadMtu NOTIFY quicUdpPayloadMtuChanged)
    Q_PROPERTY(int stationConnectUnreachableTimeoutSeconds MEMBER stationConnectUnreachableTimeoutSeconds NOTIFY stationConnectUnreachableTimeoutChanged)
    Q_PROPERTY(StationConnectUnreachableAction stationConnectUnreachableAction MEMBER stationConnectUnreachableAction NOTIFY stationConnectUnreachableActionChanged)
    Q_PROPERTY(bool showPerformanceOverlay MEMBER showPerformanceOverlay NOTIFY showPerformanceOverlayChanged)
    Q_PROPERTY(AudioConfig audioConfig MEMBER audioConfig NOTIFY audioConfigChanged)
    Q_PROPERTY(bool stationConnectToolbarPinned MEMBER stationConnectToolbarPinned NOTIFY stationConnectToolbarPinnedChanged)
    Q_PROPERTY(WindowMode windowMode MEMBER windowMode NOTIFY windowModeChanged)
    Q_PROPERTY(WindowMode recommendedFullScreenMode MEMBER recommendedFullScreenMode CONSTANT)
    Q_PROPERTY(bool muteOnFocusLoss MEMBER muteOnFocusLoss NOTIFY muteOnFocusLossChanged)
    Q_PROPERTY(bool keepAwake MEMBER keepAwake NOTIFY keepAwakeChanged)
    Q_PROPERTY(CaptureSysKeysMode captureSysKeysMode MEMBER captureSysKeysMode NOTIFY captureSysKeysModeChanged)
    Q_PROPERTY(Language language MEMBER language NOTIFY languageChanged);

    Q_INVOKABLE bool retranslate();
    Q_INVOKABLE int stationConnectDefaultBitrateKbps(int profile) const
    {
        return stationConnectDefaultBitrateForProfile(profile);
    }
    Q_INVOKABLE QVariantList stationConnectDefaultProfileBitratesKbps() const
    {
        return stationConnectProfileBitratesToVariantList(
                    stationConnectDefaultProfileBitrates());
    }
    Q_INVOKABLE int stationConnectBitrateMinimumKbps() const
    {
        return StationConnectBitrateMinimumKbps;
    }
    Q_INVOKABLE int stationConnectBitrateMaximumKbps() const
    {
        return StationConnectBitrateMaximumKbps;
    }
    Q_INVOKABLE int stationConnectBitrateStepKbps() const
    {
        return StationConnectBitrateStepKbps;
    }

    // Directly accessible members for preferences
    int fps;
    bool enableVsync;
    bool playAudioOnHost;
    bool enableMdns;
    bool mdnsDiscoveryManaged;
    bool connectionWarnings;
    bool detectNetworkBlocking;
    bool showPerformanceOverlay;
    bool muteOnFocusLoss;
    bool keepAwake;
    int quicUdpPayloadMtu;
    int stationConnectUnreachableTimeoutSeconds;
    StationConnectUnreachableAction stationConnectUnreachableAction;
    AudioConfig audioConfig;
    int identityGbrBitDepth;
    bool stationConnectToolbarPinned;
    WindowMode windowMode;
    WindowMode recommendedFullScreenMode;
    Language language;
    CaptureSysKeysMode captureSysKeysMode;

signals:
    void displayModeChanged();
    void enableVsyncChanged();
    void playAudioOnHostChanged();
    void unsupportedFpsChanged();
    void enableMdnsChanged();
    void audioConfigChanged();
    void stationConnectToolbarPinnedChanged();
    void windowModeChanged();
    void connectionWarningsChanged();
    void detectNetworkBlockingChanged();
    void quicUdpPayloadMtuChanged();
    void stationConnectUnreachableTimeoutChanged();
    void stationConnectUnreachableActionChanged();
    void showPerformanceOverlayChanged();
    void muteOnFocusLossChanged();
    void captureSysKeysModeChanged();
    void keepAwakeChanged();
    void languageChanged();

private:
    explicit StreamingPreferences(QQmlEngine *qmlEngine);

    QString getSuffixFromLanguage(Language lang);

    QQmlEngine* m_QmlEngine;
};
