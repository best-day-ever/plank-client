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
        StationConnectHostVersionRole,
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

    Q_INVOKABLE int stationConnectScalingChoice(int computerIndex) const;

    Q_INVOKABLE int stationConnectVideoProfile(int computerIndex) const;

    Q_INVOKABLE int stationConnectCaptureSource(int computerIndex) const;

    Q_INVOKABLE int stationConnectHostLayoutChoice(int computerIndex) const;

    // -1 is unknown/offline, 0 is physical, and 1 is virtual.
    Q_INVOKABLE int stationConnectHostDisplayPolicy(int computerIndex) const;

    Q_INVOKABLE int stationConnectVirtualMode1Choice(int computerIndex) const;

    Q_INVOKABLE int stationConnectVirtualMode2Choice(int computerIndex) const;

    Q_INVOKABLE bool editComputerBookmark(int computerIndex, QString address,
                                          QString nickname, int scalingChoice,
                                          int hostLayoutChoice,
                                          int virtualMode1Choice,
                                          int virtualMode2Choice,
                                          int videoProfile, int captureSource);

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
