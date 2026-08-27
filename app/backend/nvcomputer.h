#pragma once

#include "nvhttp.h"
#include "nvaddress.h"
#include "outputtopology.h"
#include "settings/streamingpreferences.h"

#include <QThread>
#include <QReadWriteLock>
#include <QSettings>
#include <QRunnable>

class CopySafeReadWriteLock : public QReadWriteLock
{
public:
    CopySafeReadWriteLock() = default;

    // Don't actually copy the QReadWriteLock
    CopySafeReadWriteLock(const CopySafeReadWriteLock&) : QReadWriteLock() {}
    CopySafeReadWriteLock& operator=(const CopySafeReadWriteLock &) { return *this; }
};

class NvComputer
{
    friend class PcMonitorThread;
    friend class ComputerManager;
    friend class PendingAuthenticationTask;
    friend class Session;

private:
    void sortAppList();

    bool updateAppList(QVector<NvApp> newAppList);

public:
    NvComputer() = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer(const NvComputer&) = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer& operator=(const NvComputer &) = default;

    explicit NvComputer(NvHTTP& http, QString serverInfo);

    explicit NvComputer(QSettings& settings);

    NvComputer(NvAddress manualAddress, QString nickname, int videoProfile,
               int captureSource, int bitrateKbps);

    bool
    updateManualBookmark(NvAddress manualAddress, QString nickname,
                         QString scalingMode,
                         QString hostLayout, QString virtualMode1,
                         QString virtualMode2,
                         int videoProfile, int captureSource, int bitrateKbps);

    void
    setRemoteAddress(QHostAddress);

    bool
    update(const NvComputer& that, NvAddress expectedAddress = NvAddress());

    bool
    acceptsServerUuid(const QString& candidateUuid) const;

    enum ReachabilityType
    {
        RI_UNKNOWN,
        RI_LAN,
        RI_VPN,
    };

    ReachabilityType
    getActiveAddressReachability() const;

    QVector<NvAddress>
    uniqueAddresses() const;

    void
    serialize(QSettings& settings, bool serializeApps) const;

    // Caller is responsible for synchronizing read access to both hosts
    bool
    isEqualSerialized(const NvComputer& that) const;

    enum AuthorizationState
    {
        AS_UNKNOWN,
        AS_AUTHORIZED,
        AS_UNAUTHORIZED
    };

    enum ComputerState
    {
        CS_UNKNOWN,
        CS_ONLINE,
        CS_OFFLINE
    };

    // Ephemeral traits
    ComputerState state;
    AuthorizationState authorizationState;
    NvAddress activeAddress;
    uint16_t activeHttpsPort;
    int currentGameId;
    QString gfeVersion;
    QString appVersion;
    QVector<NvDisplayMode> displayModes;
    int maxLumaPixelsHEVC;
    int serverCodecModeSupport;
    QString gpuModel;
    bool isSupportedServerVersion;
    bool stationConnectAuthentication = false;
    int stationConnectHostMetadataVersion = 0;
    QString stationConnectHostVersion;
    QString sessionToken;
    int stationConnectTopologyVersion = 0;
    int stationConnectFeatureFlags = 0;
    NvOutputTopology outputTopology;

    // Persisted traits
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress ipv6Address;
    NvAddress manualAddress;
    QString name;
    bool hasCustomName;
    QString uuid;
    QVector<NvApp> appList;
    bool isNvidiaServerSoftware;
    QString stationConnectScalingMode;
    QString stationConnectHostLayout;
    QString stationConnectVirtualMode1;
    QString stationConnectVirtualMode2;
    int stationConnectVideoProfile = 0;
    int stationConnectCaptureSource = 0;
    int stationConnectBitrateKbps =
            StreamingPreferences::StationConnectH264DefaultBitrateKbps;
    bool manualBookmark = false;
    QString serverUuid;
    // Remember to update isEqualSerialized() when adding fields here!

    // Synchronization
    mutable CopySafeReadWriteLock lock;

private:
    uint16_t externalPort;
};
