#include "computermodel.h"

#include <utility>

namespace {
QString hostLayoutFromChoice(int choice)
{
    switch (choice) {
    case 0: return QString::fromLatin1(NvOutputTopology::MatchClientHostLayout);
    case 1: return QString::fromLatin1(NvOutputTopology::PhysicalHostLayout);
    case 2: return QString::fromLatin1(NvOutputTopology::SingleHostLayout);
    case 3: return QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout);
    default: return QString();
    }
}

QString virtualModeFromChoice(int choice)
{
    return NvOutputTopology::qualifiedVirtualModes().value(choice);
}
}

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::authenticationCompleted,
            this, &ComputerModel::handleAuthenticationCompleted);

    m_Computers = m_ComputerManager->getComputers();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Q_ASSERT(index.row() < m_Computers.count());

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case AuthorizedRole:
        return computer->authorizationState == NvComputer::AS_AUTHORIZED;
    case BusyRole:
        return computer->currentGameId != 0;
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case StationConnectAuthenticationRole:
        return computer->stationConnectAuthentication;
    case StationConnectHostVersionRole:
        return computer->stationConnectHostMetadataVersion >= 1 ?
                    computer->stationConnectHostVersion : QString();
    case ManualBookmarkRole:
        return computer->manualBookmark;
    case AddressRole:
        // New StationConnect bookmarks have a durable manual address, but
        // workstation records created before bookmarks do not. Never expose
        // NvAddress's diagnostic <NULL> sentinel in the main workstation row.
        if (!computer->manualAddress.isNull()) {
            return computer->manualAddress.toString();
        }
        if (!computer->activeAddress.isNull()) {
            return computer->activeAddress.toString();
        }
        if (!computer->localAddress.isNull()) {
            return computer->localAddress.toString();
        }
        if (!computer->remoteAddress.isNull()) {
            return computer->remoteAddress.toString();
        }
        if (!computer->ipv6Address.isNull()) {
            return computer->ipv6Address.toString();
        }
        return QString();
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[AuthorizedRole] = "authorized";
    names[BusyRole] = "busy";
    names[StatusUnknownRole] = "statusUnknown";
    names[StationConnectAuthenticationRole] = "stationConnectAuthentication";
    names[StationConnectHostVersionRole] = "stationConnectHostVersion";
    names[ManualBookmarkRole] = "manualBookmark";
    names[AddressRole] = "address";

    return names;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app, nullptr, m_ComputerManager);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

Session* ComputerModel::createSessionForStationConnectDesktop(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    for (NvApp& app : computer->appList) {
        if (app.name == QStringLiteral("Desktop")) {
            return new Session(computer, app, nullptr, m_ComputerManager);
        }
    }

    return nullptr;
}

int ComputerModel::stationConnectScalingChoice(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return computer->stationConnectScalingMode == NvOutputTopology::NativeScalingMode ? 0 : 1;
}

int ComputerModel::stationConnectVideoProfile(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return computer->stationConnectVideoProfile;
}

int ComputerModel::stationConnectCaptureSource(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return computer->stationConnectCaptureSource;
}

QVariantList ComputerModel::stationConnectProfileBitratesKbps(
        int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return StreamingPreferences::stationConnectProfileBitratesToVariantList(
                computer->stationConnectProfileBitratesKbps);
}

int ComputerModel::stationConnectHostLayoutChoice(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    if (computer->stationConnectHostLayout == NvOutputTopology::PhysicalHostLayout) {
        return 1;
    }
    if (computer->stationConnectHostLayout == NvOutputTopology::SingleHostLayout) {
        return 2;
    }
    if (computer->stationConnectHostLayout == NvOutputTopology::DualHorizontalHostLayout) {
        return 3;
    }
    return 0;
}

int ComputerModel::stationConnectHostDisplayPolicy(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    if (computer->state != NvComputer::CS_ONLINE ||
            computer->authorizationState != NvComputer::AS_AUTHORIZED ||
            !computer->outputTopology.displayPolicyKnown()) {
        return -1;
    }
    return computer->outputTopology.allowedLayoutKinds.contains(
                NvOutputTopology::PhysicalHostLayout) ? 1 : 0;
}

int ComputerModel::stationConnectVirtualMode1Choice(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return NvOutputTopology::qualifiedVirtualModes().indexOf(
                computer->stationConnectVirtualMode1);
}

int ComputerModel::stationConnectVirtualMode2Choice(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return NvOutputTopology::qualifiedVirtualModes().indexOf(
                computer->stationConnectVirtualMode2);
}

bool ComputerModel::editComputerBookmark(int computerIndex, QString address,
                                         QString nickname, int scalingChoice,
                                         int hostLayoutChoice,
                                         int virtualMode1Choice,
                                         int virtualMode2Choice,
                                         int videoProfile, int captureSource,
                                         const QVariantList& profileBitratesKbps)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return false;
    }

    NvComputer* computer = m_Computers[computerIndex];
    const QString hostLayout = hostLayoutFromChoice(hostLayoutChoice);
    const QString virtualMode1 = virtualModeFromChoice(virtualMode1Choice);
    const QString virtualMode2 = virtualModeFromChoice(virtualMode2Choice);
    if (hostLayout.isEmpty() || virtualMode1.isEmpty() || virtualMode2.isEmpty()) {
        return false;
    }
    if (scalingChoice < 0 || scalingChoice > 1) {
        return false;
    }
    const QString scalingMode = scalingChoice == 0 ?
                QString::fromLatin1(NvOutputTopology::NativeScalingMode) :
                QString::fromLatin1(NvOutputTopology::ScaledSpanMode);

    return m_ComputerManager->editManualBookmark(computer, std::move(address),
                                                  std::move(nickname), scalingMode,
                                                  hostLayout,
                                                  virtualMode1, virtualMode2,
                                                  videoProfile, captureSource,
                                                  profileBitratesKbps);
}

void ComputerModel::deleteComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(m_Computers[computerIndex]);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

void ComputerModel::authenticateComputer(int computerIndex, QString username,
                                         QString password)
{
    Q_ASSERT(computerIndex < m_Computers.count());
    m_ComputerManager->authenticateHost(m_Computers[computerIndex],
                                        std::move(username), std::move(password));
}

void ComputerModel::handleAuthenticationCompleted(NvComputer*, QString error)
{
    emit authenticationCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        emit dataChanged(createIndex(index, 0), createIndex(index, 0));
    }
}

#include "computermodel.moc"
