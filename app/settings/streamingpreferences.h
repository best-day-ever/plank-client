#pragma once

#include <QObject>
#include <QRect>
#include <QStringList>
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

    enum PlankVideoProfile
    {
        PLANK_PROFILE_H264_10BIT_444,
        PLANK_PROFILE_H264_8BIT_422,
        PLANK_PROFILE_H264_8BIT_444,
        PLANK_PROFILE_H264_10BIT_422,
        PLANK_PROFILE_NVENC_H264_8BIT_444,
        PLANK_PROFILE_NVENC_HEVC_8BIT_444,
        PLANK_PROFILE_NVENC_HEVC_10BIT_444,
        PLANK_PROFILE_APPLE_HEVC_10BIT_420,
        PLANK_PROFILE_APPLE_HEVC_10BIT_444,
        // NVENC 4:2:0, limited-range BT.709: bandwidth profiles for remote links.
        PLANK_PROFILE_NVENC_H264_8BIT_420,
        PLANK_PROFILE_NVENC_HEVC_10BIT_420,
        PLANK_PROFILE_COUNT,
    };
    Q_ENUM(PlankVideoProfile)

    enum PlankCaptureSource
    {
        PLANK_CAPTURE_NVFBC_8BIT,
        PLANK_CAPTURE_X11_NATIVE10,
        PLANK_CAPTURE_SCREENCAPTUREKIT,
    };
    Q_ENUM(PlankCaptureSource)

    static bool isPlankAppleProfile(int profile)
    {
        return profile == PLANK_PROFILE_APPLE_HEVC_10BIT_420 ||
               profile == PLANK_PROFILE_APPLE_HEVC_10BIT_444;
    }

    static QString plankAppleEncodingMode(int profile)
    {
        if (profile == PLANK_PROFILE_APPLE_HEVC_10BIT_420) return QStringLiteral("hevc-10-420-videotoolbox");
        if (profile == PLANK_PROFILE_APPLE_HEVC_10BIT_444) return QStringLiteral("hevc-10-444-videotoolbox");
        return {};
    }

    static bool isPlankVideoProfileValid(int profile)
    {
        return profile >= PLANK_PROFILE_H264_10BIT_444 &&
               profile < PLANK_PROFILE_COUNT;
    }

    // Host encoding mode for a profile, as the host names it in launch
    // requests and in its advertised PlankEncodingModes list; empty for an
    // invalid profile.
    static QString plankEncodingMode(int profile)
    {
        switch (profile) {
        case PLANK_PROFILE_H264_8BIT_422: return QStringLiteral("h264-8-422-software");
        case PLANK_PROFILE_H264_8BIT_444: return QStringLiteral("h264-8-444-software");
        case PLANK_PROFILE_H264_10BIT_422: return QStringLiteral("h264-10-422-software");
        case PLANK_PROFILE_H264_10BIT_444: return QStringLiteral("h264-10-444-software");
        case PLANK_PROFILE_NVENC_H264_8BIT_444: return QStringLiteral("h264-8-444-nvenc");
        case PLANK_PROFILE_NVENC_HEVC_8BIT_444: return QStringLiteral("hevc-8-444-nvenc");
        case PLANK_PROFILE_NVENC_HEVC_10BIT_444: return QStringLiteral("hevc-10-444-nvenc");
        case PLANK_PROFILE_APPLE_HEVC_10BIT_420:
        case PLANK_PROFILE_APPLE_HEVC_10BIT_444: return plankAppleEncodingMode(profile);
        case PLANK_PROFILE_NVENC_H264_8BIT_420: return QStringLiteral("h264-8-420-nvenc");
        case PLANK_PROFILE_NVENC_HEVC_10BIT_420: return QStringLiteral("hevc-10-420-nvenc");
        default: return {};
        }
    }

    static bool isPlankProfileValidForCaptureSource(
            int profile, int captureSource)
    {
        if (!isPlankVideoProfileValid(profile) ||
                captureSource < PLANK_CAPTURE_NVFBC_8BIT ||
                captureSource > PLANK_CAPTURE_SCREENCAPTUREKIT) {
            return false;
        }

        if (captureSource == PLANK_CAPTURE_SCREENCAPTUREKIT ||
                isPlankAppleProfile(profile)) {
            return captureSource == PLANK_CAPTURE_SCREENCAPTUREKIT &&
                   isPlankAppleProfile(profile);
        }

        if (captureSource == PLANK_CAPTURE_X11_NATIVE10) {
            return profile == PLANK_PROFILE_H264_10BIT_444 ||
                   profile == PLANK_PROFILE_NVENC_HEVC_10BIT_444;
        }

        return true;
    }

    static bool isPlankNvenc420Profile(int profile)
    {
        return profile == PLANK_PROFILE_NVENC_H264_8BIT_420 ||
               profile == PLANK_PROFILE_NVENC_HEVC_10BIT_420;
    }

    static bool isPlankNvencProfile(int profile)
    {
        return (profile >= PLANK_PROFILE_NVENC_H264_8BIT_444 &&
                profile <= PLANK_PROFILE_NVENC_HEVC_10BIT_444) ||
               isPlankNvenc420Profile(profile);
    }

    static bool isPlankH264NvencProfile(int profile)
    {
        return profile == PLANK_PROFILE_NVENC_H264_8BIT_444 ||
               profile == PLANK_PROFILE_NVENC_H264_8BIT_420;
    }

    static bool isPlankVirtualModeValidForProfile(const QString& mode,
                                                   int profile)
    {
        const QStringList dimensions = mode.split(QLatin1Char('x'));
        bool widthValid = false;
        bool heightValid = false;
        const int width = dimensions.value(0).toInt(&widthValid);
        const int height = dimensions.value(1).toInt(&heightValid);
        if (!isPlankVideoProfileValid(profile) || dimensions.size() != 2 ||
                !widthValid || !heightValid || width <= 0 || height <= 0) {
            return false;
        }

        if (isPlankAppleProfile(profile)) {
            return width <= 5120 && height <= 2160 && width % 2 == 0 && height % 2 == 0;
        }
        return !isPlankH264NvencProfile(profile) ||
               (width <= 4096 && height <= 2160);
    }

    // Every PLANK session streams at this rate; the Frame rate preference
    // applies to non-PLANK hosts only.
    static constexpr int PlankFramesPerSecond = 60;

    static constexpr int PlankBitrateMinimumKbps = 10000;
    static constexpr int PlankBitrateMaximumKbps = 150000;
    static constexpr int PlankBitrateStepKbps = 500;
    static constexpr int PlankH264DefaultBitrateKbps = 80000;
    static constexpr int PlankHevcDefaultBitrateKbps = 50000;
    // 4:2:0 exists for links that cannot carry the 4:4:4 targets.
    static constexpr int PlankH264Yuv420DefaultBitrateKbps = 50000;
    static constexpr int PlankHevcYuv420DefaultBitrateKbps = 30000;

    static int plankDefaultBitrateForProfile(int profile)
    {
        if (profile == PLANK_PROFILE_NVENC_H264_8BIT_420) {
            return PlankH264Yuv420DefaultBitrateKbps;
        }
        if (profile == PLANK_PROFILE_NVENC_HEVC_10BIT_420) {
            return PlankHevcYuv420DefaultBitrateKbps;
        }
        return profile == PLANK_PROFILE_NVENC_HEVC_8BIT_444 ||
               profile == PLANK_PROFILE_NVENC_HEVC_10BIT_444 ||
               isPlankAppleProfile(profile) ?
                   PlankHevcDefaultBitrateKbps :
                   PlankH264DefaultBitrateKbps;
    }

    static int clampPlankBitrate(int bitrateKbps)
    {
        return qBound(PlankBitrateMinimumKbps,
                      bitrateKbps,
                      PlankBitrateMaximumKbps);
    }

    static QVector<int> plankDefaultProfileBitrates()
    {
        QVector<int> bitrates;
        bitrates.reserve(PLANK_PROFILE_COUNT);
        for (int profile = PLANK_PROFILE_H264_10BIT_444;
             profile < PLANK_PROFILE_COUNT;
             ++profile) {
            bitrates.append(plankDefaultBitrateForProfile(profile));
        }
        return bitrates;
    }

    static QVariantList plankProfileBitratesToVariantList(
            const QVector<int>& bitrates)
    {
        QVariantList values;
        values.reserve(PLANK_PROFILE_COUNT);
        for (int profile = PLANK_PROFILE_H264_10BIT_444;
             profile < PLANK_PROFILE_COUNT;
             ++profile) {
            values.append(plankBitrateForProfile(bitrates, profile));
        }
        return values;
    }

    static bool plankProfileBitratesFromVariantList(
            const QVariantList& values, QVector<int>& bitrates)
    {
        // Profile IDs are append-only. Preserve every existing bookmark value
        // when a newly introduced profile has no saved value yet.
        if (values.isEmpty() || values.size() > PLANK_PROFILE_COUNT) {
            return false;
        }

        QVector<int> parsed;
        parsed.reserve(PLANK_PROFILE_COUNT);
        for (const QVariant& value : values) {
            bool ok = false;
            const int bitrateKbps = value.toInt(&ok);
            if (!ok || bitrateKbps != clampPlankBitrate(bitrateKbps)) {
                return false;
            }
            parsed.append(bitrateKbps);
        }
        while (parsed.size() < PLANK_PROFILE_COUNT) {
            parsed.append(plankDefaultBitrateForProfile(parsed.size()));
        }
        bitrates = parsed;
        return true;
    }

    static int plankBitrateForProfile(
            const QVector<int>& bitrates, int profile)
    {
        if (isPlankVideoProfileValid(profile) &&
                profile < bitrates.size()) {
            return clampPlankBitrate(bitrates.at(profile));
        }
        return plankDefaultBitrateForProfile(profile);
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

    enum PlankUnreachableAction
    {
        PLANK_UNREACHABLE_ASK,
        PLANK_UNREACHABLE_DISCONNECT,
    };
    Q_ENUM(PlankUnreachableAction);

    Q_PROPERTY(int fps MEMBER fps NOTIFY displayModeChanged)
    Q_PROPERTY(bool enableVsync MEMBER enableVsync NOTIFY enableVsyncChanged)
    Q_PROPERTY(bool playAudioOnHost MEMBER playAudioOnHost NOTIFY playAudioOnHostChanged)
    Q_PROPERTY(bool enableMdns MEMBER enableMdns NOTIFY enableMdnsChanged)
    Q_PROPERTY(bool mdnsDiscoveryManaged MEMBER mdnsDiscoveryManaged CONSTANT)
    Q_PROPERTY(bool connectionWarnings MEMBER connectionWarnings NOTIFY connectionWarningsChanged)
    Q_PROPERTY(int quicUdpPayloadMtu MEMBER quicUdpPayloadMtu NOTIFY quicUdpPayloadMtuChanged)
    Q_PROPERTY(int plankUnreachableTimeoutSeconds MEMBER plankUnreachableTimeoutSeconds NOTIFY plankUnreachableTimeoutChanged)
    Q_PROPERTY(PlankUnreachableAction plankUnreachableAction MEMBER plankUnreachableAction NOTIFY plankUnreachableActionChanged)
    Q_PROPERTY(bool showPerformanceOverlay MEMBER showPerformanceOverlay NOTIFY showPerformanceOverlayChanged)
    Q_PROPERTY(AudioConfig audioConfig MEMBER audioConfig NOTIFY audioConfigChanged)
    Q_PROPERTY(bool microphoneAutomatic MEMBER microphoneAutomatic NOTIFY microphoneAutomaticChanged)
    Q_PROPERTY(bool microphoneAutomaticInput MEMBER microphoneAutomaticInput NOTIFY microphoneAutomaticInputChanged)
    Q_PROPERTY(bool plankToolbarPinned MEMBER plankToolbarPinned NOTIFY plankToolbarPinnedChanged)
    Q_PROPERTY(WindowMode windowMode MEMBER windowMode NOTIFY windowModeChanged)
    Q_PROPERTY(WindowMode recommendedFullScreenMode MEMBER recommendedFullScreenMode CONSTANT)
    Q_PROPERTY(bool muteOnFocusLoss MEMBER muteOnFocusLoss NOTIFY muteOnFocusLossChanged)
    Q_PROPERTY(bool keepAwake MEMBER keepAwake NOTIFY keepAwakeChanged)
    Q_PROPERTY(CaptureSysKeysMode captureSysKeysMode MEMBER captureSysKeysMode NOTIFY captureSysKeysModeChanged)
    Q_PROPERTY(bool immersiveKeyboardMode MEMBER immersiveKeyboardMode NOTIFY immersiveKeyboardModeChanged)
    Q_PROPERTY(Language language MEMBER language NOTIFY languageChanged);
    // Remote (broker) mode, bde-linux docs/plank-broker.md section 10.1
    Q_PROPERTY(QString brokerHost MEMBER brokerHost NOTIFY brokerChanged)
    Q_PROPERTY(int brokerPort MEMBER brokerPort NOTIFY brokerChanged)
    Q_PROPERTY(QStringList brokerPins MEMBER brokerPins NOTIFY brokerChanged)
    // Passkey relying party (FreeIPA domain), section 13.4
    Q_PROPERTY(QString passkeyRpId MEMBER passkeyRpId NOTIFY brokerChanged)

    Q_INVOKABLE bool retranslate();
    // Replaces the broker SPKI pin list from user text (one pin per line or
    // comma separated); returns the entries that were rejected as invalid.
    Q_INVOKABLE QStringList setBrokerPinsFromText(const QString& text);
    Q_INVOKABLE void resetBrokerDefaults();
    Q_INVOKABLE int plankDefaultBitrateKbps(int profile) const
    {
        return plankDefaultBitrateForProfile(profile);
    }
    Q_INVOKABLE QVariantList plankDefaultProfileBitratesKbps() const
    {
        return plankProfileBitratesToVariantList(
                    plankDefaultProfileBitrates());
    }
    Q_INVOKABLE int plankFramesPerSecond() const
    {
        return PlankFramesPerSecond;
    }
    Q_INVOKABLE int plankBitrateMinimumKbps() const
    {
        return PlankBitrateMinimumKbps;
    }
    Q_INVOKABLE int plankBitrateMaximumKbps() const
    {
        return PlankBitrateMaximumKbps;
    }
    Q_INVOKABLE int plankBitrateStepKbps() const
    {
        return PlankBitrateStepKbps;
    }
    Q_INVOKABLE bool plankVirtualModeSupportedForProfile(
            QString mode, int profile) const
    {
        mode.replace(QChar(0x00D7), QLatin1Char('x'));
        return isPlankVirtualModeValidForProfile(mode, profile);
    }

    // Directly accessible members for preferences
    int fps;
    bool enableVsync;
    bool playAudioOnHost;
    bool enableMdns;
    bool mdnsDiscoveryManaged;
    bool connectionWarnings;
    bool showPerformanceOverlay;
    bool muteOnFocusLoss;
    bool keepAwake;
    int quicUdpPayloadMtu;
    int plankUnreachableTimeoutSeconds;
    PlankUnreachableAction plankUnreachableAction;
    AudioConfig audioConfig;
    bool microphoneAutomatic;
    bool microphoneAutomaticInput;
    bool plankToolbarPinned;
    WindowMode windowMode;
    WindowMode recommendedFullScreenMode;
    Language language;
    QString brokerHost;
    int brokerPort;
    QStringList brokerPins;
    QString passkeyRpId;
    CaptureSysKeysMode captureSysKeysMode;
    bool immersiveKeyboardMode;

signals:
    void displayModeChanged();
    void enableVsyncChanged();
    void playAudioOnHostChanged();
    void unsupportedFpsChanged();
    void enableMdnsChanged();
    void audioConfigChanged();
    void microphoneAutomaticChanged();
    void microphoneAutomaticInputChanged();
    void plankToolbarPinnedChanged();
    void windowModeChanged();
    void connectionWarningsChanged();
    void quicUdpPayloadMtuChanged();
    void plankUnreachableTimeoutChanged();
    void plankUnreachableActionChanged();
    void showPerformanceOverlayChanged();
    void muteOnFocusLossChanged();
    void captureSysKeysModeChanged();
    void immersiveKeyboardModeChanged();
    void keepAwakeChanged();
    void languageChanged();
    void brokerChanged();

private:
    explicit StreamingPreferences(QQmlEngine *qmlEngine);

    QString getSuffixFromLanguage(Language lang);

    QQmlEngine* m_QmlEngine;
};
