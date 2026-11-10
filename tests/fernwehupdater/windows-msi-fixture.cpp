// Use the same Qt temporary-directory/file path as the updater. PowerShell's
// New-Item inherits a different DACL and cannot reproduce the service failure.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) return 1;
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/bde-fernweh-update-XXXXXX"));
    if (!directory.isValid()) return 2;
    const QString package = directory.filePath(QStringLiteral("update with spaces.msi"));
    if (!QFile::copy(app.arguments().at(1), package)) return 3;
    directory.setAutoRemove(false);
    QTextStream(stdout) << QDir::toNativeSeparators(package) << Qt::endl;
    return 0;
}
