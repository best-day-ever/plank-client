#pragma once

#include <QObject>
#include <QVariant>

class ComputerManager;
class NvComputer;
class Session;
class StreamingPreferences;

namespace CliStartStream
{

class Event;
class LauncherPrivate;

class Launcher : public QObject
{
    Q_OBJECT
    Q_DECLARE_PRIVATE_D(m_DPtr, Launcher)

public:
    explicit Launcher(QString computer, QString app,
                      StreamingPreferences* preferences,
                      QString plankUsername = QString(),
                      QString plankPassword = QString(),
                      QObject *parent = nullptr);
    ~Launcher() override;
    Q_INVOKABLE void execute(ComputerManager *manager);
    Q_INVOKABLE bool isExecuted() const;

signals:
    void searchingComputer();
    void authenticatingComputer();
    void searchingApp();
    void sessionCreated(QString appName, Session *session);
    void failed(QString text);

private slots:
    void onComputerFound(NvComputer *computer);
    void onComputerUpdated(NvComputer *computer);
    void onAuthenticationCompleted(NvComputer *computer, QString error);
    void onTimeout();

private:
    QScopedPointer<LauncherPrivate> m_DPtr;
};

}
