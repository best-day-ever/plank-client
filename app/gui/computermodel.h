#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        AuthorizedRole,
        BusyRole,
        StatusUnknownRole,
        ServerSupportedRole,
        StationConnectAuthenticationRole,
        ManualBookmarkRole,
        AddressRole
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE void authenticateComputer(int computerIndex, QString username, QString password);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    Q_INVOKABLE Session* createSessionForStationConnectDesktop(int computerIndex);

    Q_INVOKABLE QStringList stationConnectDisplayChoices(int computerIndex) const;

    Q_INVOKABLE int stationConnectDisplayChoice(int computerIndex) const;

    Q_INVOKABLE int stationConnectVideoProfile(int computerIndex) const;

    Q_INVOKABLE bool editComputerBookmark(int computerIndex, QString address,
                                          QString nickname, int displayChoice,
                                          int videoProfile);

signals:
    void authenticationCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handleAuthenticationCompleted(NvComputer* computer, QString error);

private:
    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
