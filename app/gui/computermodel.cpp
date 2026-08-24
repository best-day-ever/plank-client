#include "computermodel.h"

#include <QThreadPool>

#include <utility>

namespace {
QVector<const NvOutput*> orderedOutputs(const NvOutputTopology& topology)
{
    QVector<const NvOutput*> outputs;
    for (const NvOutput& output : topology.outputs) {
        if (output.primary) {
            outputs.append(&output);
        }
    }
    for (const NvOutput& output : topology.outputs) {
        if (!output.primary) {
            outputs.append(&output);
        }
    }
    return outputs;
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
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case StationConnectAuthenticationRole:
        return computer->stationConnectAuthentication;
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
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[StationConnectAuthenticationRole] = "stationConnectAuthentication";
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

QStringList ComputerModel::stationConnectDisplayChoices(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    const NvOutputTopology& topology = computer->outputTopology;
    QStringList choices;
    if (topology.outputs.isEmpty()) {
        choices.append(tr("Scaled desktop span"));
        choices.append(tr("Primary display"));
        return choices;
    }
    if (topology.supportsScaledSpan()) {
        choices.append(tr("Scaled desktop span (%1×%2)")
                       .arg(topology.desktopWidth).arg(topology.desktopHeight));
    }
    for (const NvOutput* output : orderedOutputs(topology)) {
        QString label = tr("%1 — %2×%3")
                .arg(output->name).arg(output->width).arg(output->height);
        if (output->primary) {
            label += tr(" (Primary)");
        }
        choices.append(label);
    }
    return choices;
}

int ComputerModel::stationConnectDisplayChoice(int computerIndex) const
{
    Q_ASSERT(computerIndex >= 0 && computerIndex < m_Computers.count());
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    const NvOutputTopology& topology = computer->outputTopology;
    if (topology.outputs.isEmpty()) {
        return computer->selectedDisplayMode == NvOutputTopology::SingleOutputMode ? 1 : 0;
    }
    if (computer->selectedDisplayMode == NvOutputTopology::ScaledSpanMode &&
            topology.supportsScaledSpan()) {
        return 0;
    }
    int index = topology.supportsScaledSpan() ? 1 : 0;
    for (const NvOutput* output : orderedOutputs(topology)) {
        if (output->id == computer->selectedOutputId) {
            return index;
        }
        ++index;
    }
    return 0;
}

bool ComputerModel::editComputerBookmark(int computerIndex, QString address,
                                         QString nickname, int displayChoice)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return false;
    }

    NvComputer* computer = m_Computers[computerIndex];
    QString displayMode;
    QString selectedOutputId;
    {
        QReadLocker lock(&computer->lock);
        const NvOutputTopology& topology = computer->outputTopology;
        if (topology.outputs.isEmpty()) {
            if (displayChoice < 0 || displayChoice > 1) {
                return false;
            }
            displayMode = displayChoice == 0 ? NvOutputTopology::ScaledSpanMode :
                                               NvOutputTopology::SingleOutputMode;
        }
        else {
            int outputIndex = displayChoice;
            if (topology.supportsScaledSpan()) {
                if (displayChoice == 0) {
                    displayMode = NvOutputTopology::ScaledSpanMode;
                }
                else {
                    --outputIndex;
                }
            }
            if (displayMode.isEmpty()) {
                const QVector<const NvOutput*> outputs = orderedOutputs(topology);
                if (outputIndex < 0 || outputIndex >= outputs.size()) {
                    return false;
                }
                displayMode = NvOutputTopology::SingleOutputMode;
                selectedOutputId = outputs[outputIndex]->id;
            }
        }
    }

    return m_ComputerManager->editManualBookmark(computer, std::move(address),
                                                  std::move(nickname), displayMode,
                                                  selectedOutputId);
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

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
        : m_Computer(computer) {}

    void run()
    {
        m_Computer->wake();
    }

private:
    NvComputer* m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

class DeferredTestConnectionTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    void run()
    {
        unsigned int portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (portTestResult == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
        }
        else {
            char blockedPorts[512];
            LiStringifyPortFlags(portTestResult, "\n", blockedPorts, sizeof(blockedPorts));
            emit connectionTestCompleted(portTestResult, QString(blockedPorts));
        }
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(int)
{
    DeferredTestConnectionTask* testConnectionTask = new DeferredTestConnectionTask();
    QObject::connect(testConnectionTask, &DeferredTestConnectionTask::connectionTestCompleted,
                     this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(testConnectionTask);
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
