#include "nvcomputer.h"
#include "nvapp.h"
#include "settings/compatfetcher.h"
#include "settings/streamingpreferences.h"
#include "stationconnectnetwork.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QNetworkProxy>

#define SER_NAME "hostname"
#define SER_UUID "uuid"
#define SER_LOCALADDR "localaddress"
#define SER_LOCALPORT "localport"
#define SER_REMOTEADDR "remoteaddress"
#define SER_REMOTEPORT "remoteport"
#define SER_MANUALADDR "manualaddress"
#define SER_MANUALPORT "manualport"
#define SER_IPV6ADDR "ipv6address"
#define SER_IPV6PORT "ipv6port"
#define SER_APPLIST "apps"
#define SER_CUSTOMNAME "customname"
#define SER_NVIDIASOFTWARE "nvidiasw"
#define SER_SELECTEDOUTPUT "stationconnect-selected-output"
#define SER_DISPLAYMODE "stationconnect-display-mode"
#define SER_HOSTLAYOUT "stationconnect-host-layout"
#define SER_VIRTUALMODE "stationconnect-virtual-mode"
#define SER_VIDEOPROFILE "stationconnect-video-profile"
#define SER_OUTPUTTOPOLOGY "stationconnect-output-topology"
#define SER_MANUALBOOKMARK "stationconnect-manual-bookmark"
#define SER_SERVERUUID "stationconnect-server-uuid"

namespace {
QString manualBookmarkUuid(const NvAddress& address)
{
    const QByteArray bookmarkKey = address.toString().toUtf8();
    return QStringLiteral("stationconnect-bookmark-") +
            QString::fromLatin1(QCryptographicHash::hash(
                                    bookmarkKey, QCryptographicHash::Sha256).toHex().left(32));
}
}

NvComputer::NvComputer(NvAddress address, QString nickname, int videoProfile)
{
    this->uuid = manualBookmarkUuid(address);
    this->name = nickname;
    this->hasCustomName = true;
    this->manualAddress = address;
    this->manualBookmark = true;
    this->stationConnectVideoProfile = videoProfile;
    this->stationConnectHostLayout = NvOutputTopology::ConfiguredHostLayout;
    this->stationConnectVirtualMode = QStringLiteral("1920x1080");
    this->state = CS_UNKNOWN;
    this->authorizationState = AS_UNKNOWN;
    this->currentGameId = 0;
    this->activeHttpsPort = 0;
    this->maxLumaPixelsHEVC = 0;
    this->serverCodecModeSupport = 0;
    this->isSupportedServerVersion = true;
    this->isNvidiaServerSoftware = false;
    this->externalPort = address.port();
}

bool NvComputer::updateManualBookmark(NvAddress address, QString nickname,
                                      QString displayMode, QString outputId,
                                      QString hostLayout, QString virtualMode,
                                      int videoProfile)
{
    QWriteLocker writeLocker(&lock);
    Q_ASSERT(manualBookmark);

    const bool addressChanged = manualAddress != address;
    if (addressChanged) {
        uuid = manualBookmarkUuid(address);
        manualAddress = address;
        localAddress = NvAddress();
        remoteAddress = NvAddress();
        ipv6Address = NvAddress();
        activeAddress = NvAddress();
        activeHttpsPort = 0;
        externalPort = address.port();
        serverUuid.clear();
        appList.clear();
        outputTopology = NvOutputTopology();
        sessionToken.clear();
        authorizationState = AS_UNKNOWN;
        state = CS_UNKNOWN;
        currentGameId = 0;
        stationConnectAuthentication = false;
        stationConnectTopologyVersion = 0;
        stationConnectFeatureFlags = 0;
        displayModes.clear();
        maxLumaPixelsHEVC = 0;
        serverCodecModeSupport = 0;
        gpuModel.clear();
        gfeVersion.clear();
        appVersion.clear();
        isSupportedServerVersion = true;
        isNvidiaServerSoftware = false;
    }

    name = nickname;
    hasCustomName = true;
    selectedDisplayMode = displayMode;
    selectedOutputId = outputId;
    stationConnectHostLayout = hostLayout;
    stationConnectVirtualMode = virtualMode;
    stationConnectVideoProfile = videoProfile;
    return addressChanged;
}

NvComputer::NvComputer(QSettings& settings)
{
    this->name = settings.value(SER_NAME).toString();
    this->uuid = settings.value(SER_UUID).toString();
    this->hasCustomName = settings.value(SER_CUSTOMNAME).toBool();
    this->localAddress = NvAddress(settings.value(SER_LOCALADDR).toString(),
                                   settings.value(SER_LOCALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->remoteAddress = NvAddress(settings.value(SER_REMOTEADDR).toString(),
                                    settings.value(SER_REMOTEPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->ipv6Address = NvAddress(settings.value(SER_IPV6ADDR).toString(),
                                  settings.value(SER_IPV6PORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->manualAddress = NvAddress(settings.value(SER_MANUALADDR).toString(),
                                    settings.value(SER_MANUALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->isNvidiaServerSoftware = settings.value(SER_NVIDIASOFTWARE).toBool();
    this->selectedOutputId = settings.value(SER_SELECTEDOUTPUT).toString();
    this->selectedDisplayMode = settings.value(SER_DISPLAYMODE).toString();
    this->stationConnectHostLayout =
            settings.value(SER_HOSTLAYOUT,
                           NvOutputTopology::ConfiguredHostLayout).toString();
    if (this->stationConnectHostLayout != NvOutputTopology::ConfiguredHostLayout &&
            this->stationConnectHostLayout != NvOutputTopology::PhysicalHostLayout &&
            this->stationConnectHostLayout != NvOutputTopology::SingleHostLayout &&
            this->stationConnectHostLayout != NvOutputTopology::DualHorizontalHostLayout) {
        this->stationConnectHostLayout = NvOutputTopology::ConfiguredHostLayout;
    }
    this->stationConnectVirtualMode =
            settings.value(SER_VIRTUALMODE, QStringLiteral("1920x1080")).toString();
    if (this->stationConnectVirtualMode != QStringLiteral("1920x1080") &&
            this->stationConnectVirtualMode != QStringLiteral("3840x2160")) {
        this->stationConnectVirtualMode = QStringLiteral("1920x1080");
    }
    this->stationConnectVideoProfile = qBound(
            static_cast<int>(StreamingPreferences::SCVP_H264_10BIT_444),
            settings.value(SER_VIDEOPROFILE,
                           static_cast<int>(StreamingPreferences::SCVP_H264_10BIT_444)).toInt(),
            static_cast<int>(StreamingPreferences::SCVP_H264_10BIT_422));
    this->manualBookmark = settings.value(SER_MANUALBOOKMARK, false).toBool();
    this->serverUuid = settings.value(SER_SERVERUUID).toString();
    const QJsonDocument serializedTopology = QJsonDocument::fromJson(
            settings.value(SER_OUTPUTTOPOLOGY).toByteArray());
    if (serializedTopology.isObject()) {
        NvOutputTopology::fromJson(serializedTopology.object(), this->outputTopology);
    }

    int appCount = settings.beginReadArray(SER_APPLIST);
    this->appList.reserve(appCount);
    for (int i = 0; i < appCount; i++) {
        settings.setArrayIndex(i);

        NvApp app(settings);
        this->appList.append(app);
    }
    settings.endArray();
    sortAppList();

    this->currentGameId = 0;
    this->authorizationState = AS_UNKNOWN;
    this->state = CS_UNKNOWN;
    this->gfeVersion = nullptr;
    this->appVersion = nullptr;
    this->maxLumaPixelsHEVC = 0;
    this->serverCodecModeSupport = 0;
    this->gpuModel = nullptr;
    this->isSupportedServerVersion = true;
    this->externalPort = this->remoteAddress.port();
    this->activeHttpsPort = 0;
    this->stationConnectAuthentication = false;
    this->stationConnectTopologyVersion = 0;
    this->stationConnectFeatureFlags = 0;
    this->sessionToken.clear();
}

void NvComputer::setRemoteAddress(QHostAddress address)
{
    QWriteLocker lock(&this->lock);

    Q_ASSERT(this->externalPort != 0);

    this->remoteAddress = NvAddress(address, this->externalPort);
}

void NvComputer::serialize(QSettings& settings, bool serializeApps) const
{
    QReadLocker lock(&this->lock);

    settings.setValue(SER_NAME, name);
    settings.setValue(SER_CUSTOMNAME, hasCustomName);
    settings.setValue(SER_UUID, uuid);
    settings.setValue(SER_LOCALADDR, localAddress.address());
    settings.setValue(SER_LOCALPORT, localAddress.port());
    settings.setValue(SER_REMOTEADDR, remoteAddress.address());
    settings.setValue(SER_REMOTEPORT, remoteAddress.port());
    settings.setValue(SER_IPV6ADDR, ipv6Address.address());
    settings.setValue(SER_IPV6PORT, ipv6Address.port());
    settings.setValue(SER_MANUALADDR, manualAddress.address());
    settings.setValue(SER_MANUALPORT, manualAddress.port());
    settings.remove("srvcert");
    settings.setValue(SER_NVIDIASOFTWARE, isNvidiaServerSoftware);
    settings.setValue(SER_SELECTEDOUTPUT, selectedOutputId);
    settings.setValue(SER_DISPLAYMODE, selectedDisplayMode);
    settings.setValue(SER_HOSTLAYOUT, stationConnectHostLayout);
    settings.setValue(SER_VIRTUALMODE, stationConnectVirtualMode);
    settings.setValue(SER_VIDEOPROFILE, stationConnectVideoProfile);
    settings.setValue(SER_MANUALBOOKMARK, manualBookmark);
    settings.setValue(SER_SERVERUUID, serverUuid);
    if (!outputTopology.outputs.isEmpty()) {
        settings.setValue(SER_OUTPUTTOPOLOGY,
                          QJsonDocument(outputTopology.toJson()).toJson(QJsonDocument::Compact));
    } else {
        settings.remove(SER_OUTPUTTOPOLOGY);
    }

    // Avoid deleting an existing applist if we couldn't get one
    if (!appList.isEmpty() && serializeApps) {
        settings.remove(SER_APPLIST);
        settings.beginWriteArray(SER_APPLIST);
        for (int i = 0; i < appList.count(); i++) {
            settings.setArrayIndex(i);
            appList.at(i).serialize(settings);
        }
        settings.endArray();
    }
}

bool NvComputer::isEqualSerialized(const NvComputer &that) const
{
    return this->name == that.name &&
           this->hasCustomName == that.hasCustomName &&
           this->uuid == that.uuid &&
           this->localAddress == that.localAddress &&
           this->remoteAddress == that.remoteAddress &&
           this->ipv6Address == that.ipv6Address &&
           this->manualAddress == that.manualAddress &&
           this->isNvidiaServerSoftware == that.isNvidiaServerSoftware &&
           this->selectedOutputId == that.selectedOutputId &&
           this->selectedDisplayMode == that.selectedDisplayMode &&
           this->stationConnectHostLayout == that.stationConnectHostLayout &&
           this->stationConnectVirtualMode == that.stationConnectVirtualMode &&
           this->stationConnectVideoProfile == that.stationConnectVideoProfile &&
           this->manualBookmark == that.manualBookmark &&
           this->serverUuid == that.serverUuid &&
           this->outputTopology.toJson() == that.outputTopology.toJson() &&
           this->appList == that.appList;
}

void NvComputer::sortAppList()
{
    std::stable_sort(appList.begin(), appList.end(), [](const NvApp& app1, const NvApp& app2) {
       return app1.name.toLower() < app2.name.toLower();
    });
}

NvComputer::NvComputer(NvHTTP& http, QString serverInfo)
{
    this->manualBookmark = false;

    this->hasCustomName = false;
    this->name = NvHTTP::getXmlString(serverInfo, "hostname");
    if (this->name.isEmpty()) {
        this->name = "UNKNOWN";
    }

    this->uuid = NvHTTP::getXmlString(serverInfo, "uniqueid");
    QString codecSupport = NvHTTP::getXmlString(serverInfo, "ServerCodecModeSupport");
    if (!codecSupport.isEmpty()) {
        this->serverCodecModeSupport = codecSupport.toInt();
    }
    else {
        // Assume H.264 is always supported
        this->serverCodecModeSupport = SCM_H264;
    }

    QString maxLumaPixelsHEVC = NvHTTP::getXmlString(serverInfo, "MaxLumaPixelsHEVC");
    if (!maxLumaPixelsHEVC.isEmpty()) {
        this->maxLumaPixelsHEVC = maxLumaPixelsHEVC.toInt();
    }
    else {
        this->maxLumaPixelsHEVC = 0;
    }

    this->displayModes = NvHTTP::getDisplayModeList(serverInfo);
    std::stable_sort(this->displayModes.begin(), this->displayModes.end(),
                     [](const NvDisplayMode& mode1, const NvDisplayMode& mode2) {
        return (uint64_t)mode1.width * mode1.height * mode1.refreshRate <
                (uint64_t)mode2.width * mode2.height * mode2.refreshRate;
    });

    // We can get an IPv4 loopback address if we're using the GS IPv6 Forwarder
    this->localAddress = NvAddress(NvHTTP::getXmlString(serverInfo, "LocalIP"), http.httpPort());
    if (this->localAddress.address().startsWith("127.")) {
        this->localAddress = NvAddress();
    }

    QString httpsPort = NvHTTP::getXmlString(serverInfo, "HttpsPort");
    if (httpsPort.isEmpty() || (this->activeHttpsPort = httpsPort.toUShort()) == 0) {
        this->activeHttpsPort = DEFAULT_HTTPS_PORT;
    }

    // This is an extension which is not present in GFE. It is present for Sunshine to be able
    // to support dynamic HTTP WAN ports without requiring the user to manually enter the port.
    QString remotePortStr = NvHTTP::getXmlString(serverInfo, "ExternalPort");
    if (remotePortStr.isEmpty() || (this->externalPort = remotePortStr.toUShort()) == 0) {
        this->externalPort = http.httpPort();
    }

    QString remoteAddress = NvHTTP::getXmlString(serverInfo, "ExternalIP");
    if (!remoteAddress.isEmpty()) {
        this->remoteAddress = NvAddress(remoteAddress, this->externalPort);
    }
    else {
        this->remoteAddress = NvAddress();
    }

    // Real Nvidia host software (GeForce Experience and RTX Experience) both use the 'Mjolnir'
    // codename in the state field and no version of Sunshine does. We can use this to bypass
    // some assumptions about Nvidia hardware that don't apply to Sunshine hosts.
    this->isNvidiaServerSoftware = NvHTTP::getXmlString(serverInfo, "state").contains("MJOLNIR");

    this->stationConnectAuthentication =
            NvHTTP::getXmlString(serverInfo, "StationConnectAuth") == "1";
    this->stationConnectTopologyVersion =
            NvHTTP::getXmlString(serverInfo, "StationConnectTopologyVersion").toInt();
    this->stationConnectFeatureFlags =
            NvHTTP::getXmlString(serverInfo, "StationConnectFeatureFlags").toInt();
    this->authorizationState = NvHTTP::getXmlString(serverInfo, "PairStatus") == "1" ?
                AS_AUTHORIZED : AS_UNAUTHORIZED;
    this->currentGameId = NvHTTP::getCurrentGame(serverInfo);
    this->appVersion = NvHTTP::getXmlString(serverInfo, "appversion");
    this->gfeVersion = NvHTTP::getXmlString(serverInfo, "GfeVersion");
    this->gpuModel = NvHTTP::getXmlString(serverInfo, "gputype");
    this->activeAddress = http.address();
    this->state = NvComputer::CS_ONLINE;
    this->isSupportedServerVersion = CompatFetcher::isGfeVersionSupported(this->gfeVersion);
}

NvComputer::ReachabilityType NvComputer::getActiveAddressReachability() const
{
    NvAddress copyOfActiveAddress;

    {
        QReadLocker readLocker(&lock);

        if (activeAddress.isNull()) {
            return ReachabilityType::RI_UNKNOWN;
        }

        // Grab a copy of the active address to avoid having to hold
        // the computer lock while doing socket operations
        copyOfActiveAddress = activeAddress;
    }

    QTcpSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.connectToHost(copyOfActiveAddress.address(), copyOfActiveAddress.port());
    if (s.waitForConnected(3000)) {
        Q_ASSERT(!s.localAddress().isNull());
        Q_ASSERT(!s.peerAddress().isNull());

        const auto allInterfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& nic : allInterfaces) {
            // Ensure the interface is up
            if ((nic.flags() & QNetworkInterface::IsUp) == 0) {
                continue;
            }

            const auto allInterfaceAddresses = nic.addressEntries();
            for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
                if (addr.ip() == s.localAddress()) {
                    qInfo() << "Found matching interface:" << nic.humanReadableName() << nic.hardwareAddress() << nic.flags();

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
                    qInfo() << "Interface Type:" << nic.type();
                    qInfo() << "Interface MTU:" << nic.maximumTransmissionUnit();

                    if (nic.type() == QNetworkInterface::Virtual ||
                            nic.type() == QNetworkInterface::Ppp) {
                        // Treat PPP and virtual interfaces as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.maximumTransmissionUnit() != 0 && nic.maximumTransmissionUnit() < 1500) {
                        // Treat MTUs under 1500 as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.flags() & QNetworkInterface::IsPointToPoint) {
                        // Treat point-to-point links as likely VPNs.
                        // This check detects OpenVPN on Unix-like OSes.
                        return ReachabilityType::RI_VPN;
                    }

#ifdef Q_OS_WINDOWS
                    if (nic.name().startsWith("iftype53_") || nic.name().startsWith("iftype131_")) {
                        // Match by NDIS interface type. These values are Microsoft's recommended values for VPN connections:
                        // https://learn.microsoft.com/en-US/troubleshoot/windows-client/networking/windows-connection-manager-disconnects-wlan#more-information
                        //
                        // The following VPNs use IF_TYPE_PROP_VIRTUAL under Windows:
                        //  - WireguardNT VPNs
                        //  - All WinTun-based VPNs (such as Slack Nebula)
                        //  - OpenVPN with tap-windows6
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.hardwareAddress().startsWith("00:FF", Qt::CaseInsensitive)) {
                        // OpenVPN TAP interfaces have a MAC address starting with 00:FF on Windows
                        return ReachabilityType::RI_VPN;
                    }

                    if (StationConnectNetwork::isZeroTierInterface(nic.name(),
                                                                   nic.humanReadableName())) {
                        // Qt reports Linux ZeroTier interfaces as Ethernet, so
                        // recognize their ztXXXXXXXX kernel name explicitly.
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().contains("VPN")) {
                        // This one is just a final VPN heuristic if all else fails
                        return ReachabilityType::RI_VPN;
                    }

                    // Didn't meet any of our VPN heuristics. Let's see if the peer address is on-link.
                    Q_ASSERT(addr.prefixLength() >= 0);
                    if (addr.prefixLength() >= 0 && s.localAddress().isInSubnet(s.peerAddress(), addr.prefixLength())) {
                        return ReachabilityType::RI_LAN;
                    }

                    // Default to unknown if nothing else matched
                    return ReachabilityType::RI_UNKNOWN;
                }
            }
        }

        qWarning() << "No match found for address:" << s.localAddress();
        return ReachabilityType::RI_UNKNOWN;
    }
    else {
        // If we fail to connect, just pretend that it's not a VPN
        qWarning() << "Unable to check for reachability within 3 seconds";
        return ReachabilityType::RI_UNKNOWN;
    }
}

bool NvComputer::updateAppList(QVector<NvApp> newAppList) {
    if (appList == newAppList) {
        return false;
    }

    // Propagate client-side attributes to the new app list
    for (const NvApp& existingApp : std::as_const(appList)) {
        for (NvApp& newApp : newAppList) {
            if (existingApp.id == newApp.id) {
                newApp.hidden = existingApp.hidden;
                newApp.directLaunch = existingApp.directLaunch;
            }
        }
    }

    appList = newAppList;
    sortAppList();
    return true;
}

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QReadLocker readLocker(&lock);
    QVector<NvAddress> uniqueAddressList;

    // Start with addresses correctly ordered
    uniqueAddressList.append(activeAddress);
    uniqueAddressList.append(localAddress);
    uniqueAddressList.append(remoteAddress);
    uniqueAddressList.append(ipv6Address);
    uniqueAddressList.append(manualAddress);

    // Prune duplicates (always giving precedence to the first)
    for (int i = 0; i < uniqueAddressList.count(); i++) {
        if (uniqueAddressList[i].isNull()) {
            uniqueAddressList.remove(i);
            i--;
            continue;
        }
        for (int j = i + 1; j < uniqueAddressList.count(); j++) {
            if (uniqueAddressList[i] == uniqueAddressList[j]) {
                // Always remove the later occurrence
                uniqueAddressList.remove(j);
                j--;
            }
        }
    }

    // We must have at least 1 address
    Q_ASSERT(!uniqueAddressList.isEmpty());

    return uniqueAddressList;
}

bool NvComputer::update(const NvComputer& that, NvAddress expectedAddress)
{
    bool changed = false;

    // Lock us for write and them for read
    QWriteLocker thisLock(&this->lock);
    QReadLocker thatLock(&that.lock);

    if (!expectedAddress.isNull() && expectedAddress != activeAddress &&
            expectedAddress != localAddress && expectedAddress != remoteAddress &&
            expectedAddress != ipv6Address && expectedAddress != manualAddress) {
        return false;
    }

    // A manual bookmark has a stable local UUID before its server identity is
    // known. Bind it to the first server that successfully answers, then reject
    // any different identity at that saved address.
    if (manualBookmark) {
        Q_ASSERT(serverUuid.isEmpty() || serverUuid == that.uuid);
        if (serverUuid.isEmpty()) {
            serverUuid = that.uuid;
            changed = true;
        }
    }
    else {
        Q_ASSERT(this->uuid == that.uuid);
    }

#define ASSIGN_IF_CHANGED(field)       \
    if (this->field != that.field) {   \
        this->field = that.field;      \
        changed = true;                \
    }

#define ASSIGN_IF_CHANGED_AND_NONEMPTY(field) \
    if (!that.field.isEmpty() &&              \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

#define ASSIGN_IF_CHANGED_AND_NONNULL(field)  \
    if (!that.field.isNull() &&               \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

    if (!hasCustomName) {
        // Only overwrite the name if it's not custom
        ASSIGN_IF_CHANGED(name);
    }
    ASSIGN_IF_CHANGED_AND_NONNULL(localAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(remoteAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(ipv6Address);
    ASSIGN_IF_CHANGED_AND_NONNULL(manualAddress);
    ASSIGN_IF_CHANGED(activeHttpsPort);
    ASSIGN_IF_CHANGED(externalPort);
    ASSIGN_IF_CHANGED(stationConnectAuthentication);
    ASSIGN_IF_CHANGED(stationConnectTopologyVersion);
    ASSIGN_IF_CHANGED(stationConnectFeatureFlags);
    if (stationConnectAuthentication && sessionToken.isEmpty()) {
        if (authorizationState != AS_UNAUTHORIZED) {
            authorizationState = AS_UNAUTHORIZED;
            changed = true;
        }
    }
    else {
        ASSIGN_IF_CHANGED(authorizationState);
    }
    ASSIGN_IF_CHANGED(serverCodecModeSupport);
    ASSIGN_IF_CHANGED(currentGameId);
    ASSIGN_IF_CHANGED(activeAddress);
    ASSIGN_IF_CHANGED(state);
    ASSIGN_IF_CHANGED(gfeVersion);
    ASSIGN_IF_CHANGED(appVersion);
    ASSIGN_IF_CHANGED(isSupportedServerVersion);
    ASSIGN_IF_CHANGED(isNvidiaServerSoftware);
    ASSIGN_IF_CHANGED(maxLumaPixelsHEVC);
    ASSIGN_IF_CHANGED(gpuModel);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(displayModes);

    if (!that.appList.isEmpty()) {
        // updateAppList() handles merging client-side attributes
        updateAppList(that.appList);
    }

    return changed;
}

bool NvComputer::acceptsServerUuid(const QString& candidateUuid) const
{
    QReadLocker readLocker(&lock);
    if (manualBookmark) {
        return serverUuid.isEmpty() || serverUuid == candidateUuid;
    }
    return uuid == candidateUuid;
}
