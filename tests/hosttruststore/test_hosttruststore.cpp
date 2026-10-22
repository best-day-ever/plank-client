#include "../../app/backend/hosttruststore.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <future>

class HostTrustStoreTest : public QObject
{
    Q_OBJECT
    const QString address = QStringLiteral("https://host.example.org:28989");
    const QByteArray first = QByteArray(32, 'a'), second = QByteArray(32, 'b'), third = QByteArray(32, 'c');
    using Status = HostTrustStore::Status;
private slots:
    void normalizeEndpoints()
    {
        QCOMPARE(HostTrustStore::endpoint(QUrl("https://HOST.example.org.:28989/a?b=1")), address);
        QCOMPARE(HostTrustStore::endpoint(QUrl("https://[2001:db8:0:0::1]:28989")),
                 QString("https://[2001:db8::1]:28989"));
        QVERIFY(HostTrustStore::endpoint(QUrl("http://host.example.org:28989")).isEmpty());
        QVERIFY(HostTrustStore::endpoint(QUrl("https://user@host.example.org:28989")).isEmpty());
        QVERIFY(HostTrustStore::endpoint(QUrl("https://host.example.org")).isEmpty());
        QVERIFY(HostTrustStore::endpoint(QUrl("https://host.example.org:0")).isEmpty());
    }

    void explicitFirstConnectionOnly()
    {
        QTemporaryDir dir;
        HostTrustStore store(dir.filePath("trust"));
        QCOMPARE(store.check(address, first).status, Status::Unknown);
        QCOMPARE(store.check(address, second).status, Status::Unknown);
        QCOMPARE(store.check(address, first, true).status, Status::Trusted);
        HostTrustStore reopened(dir.filePath("trust"));
        QCOMPARE(reopened.check(address, first).status, Status::Trusted);
        QCOMPARE(reopened.check(address, second, true).status, Status::Changed);
        QCOMPARE(reopened.check(address, second).previousKey, first);
        QCOMPARE(reopened.check(address, first).status, Status::Trusted);
        QCOMPARE(reopened.check("https://host.example.org:28990", second).status, Status::Unknown);
    }

    void replacementIsExactAndAtomic()
    {
        QTemporaryDir dir;
        HostTrustStore store(dir.filePath("trust"));
        QCOMPARE(store.replace(address, first, second).status, Status::Error);
        QCOMPARE(store.check(address, first, true).status, Status::Trusted);
        // Merely opening/cancelling a warning has no effect on the saved identity.
        QCOMPARE(store.check(address, second).status, Status::Changed);
        QCOMPARE(store.check(address, first).status, Status::Trusted);
        QCOMPARE(store.replace(address, first, second).status, Status::Trusted);
        QCOMPARE(store.replace(address, first, third).status, Status::Error);
        QCOMPARE(store.check(address, second).status, Status::Trusted);
        QCOMPARE(store.replace(address, first, second).status, Status::Trusted);
        QCOMPARE(store.replace(address, second, third).status, Status::Trusted);
        QCOMPARE(store.check(address, second).status, Status::Changed);
    }

    void damagedStoreFailsClosed()
    {
        QTemporaryDir dir;
        HostTrustStore store(dir.filePath("trust"));
        QCOMPARE(store.check(address, first, true).status, Status::Trusted);
        QFile file(dir.filePath("trust/identities.json"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{broken"); file.close();
        QCOMPARE(store.check(address, first).status, Status::Error);
        QCOMPARE(store.check(address, second, true).status, Status::Error);
        QCOMPARE(store.replace(address, first, second).status, Status::Error);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{broken"));
    }

    void cannotWriteDoesNotEstablishTrust()
    {
        QTemporaryDir dir;
        QFile occupied(dir.filePath("not-directory"));
        QVERIFY(occupied.open(QIODevice::WriteOnly)); occupied.write("unchanged"); occupied.close();
        HostTrustStore store(occupied.fileName());
        QCOMPARE(store.check(address, first, true).status, Status::Error);
        QCOMPARE(store.check(address, QByteArray(31, 'a'), true).status, Status::Error);
    }

    void concurrentEnrollmentNeverOverwrites()
    {
        QTemporaryDir dir;
        HostTrustStore store(dir.filePath("trust"));
        auto a = std::async(std::launch::async, [&] { return store.check(address, first, true); });
        auto b = std::async(std::launch::async, [&] { return store.check(address, second, true); });
        const auto ar = a.get(), br = b.get();
        QVERIFY((ar.status == Status::Trusted && br.status == Status::Changed) ||
                (br.status == Status::Trusted && ar.status == Status::Changed));
    }

    void bookmarkDeletionDoesNotEraseTrust()
    {
        QTemporaryDir dir;
        HostTrustStore store(dir.filePath("trust"));
        QCOMPARE(store.check(address, first, true).status, Status::Trusted);
        QFile bookmark(dir.filePath("bookmarks.ini"));
        QVERIFY(bookmark.open(QIODevice::WriteOnly)); bookmark.write("bookmark"); bookmark.close();
        QVERIFY(bookmark.remove());
        HostTrustStore reopened(dir.filePath("trust"));
        QCOMPARE(reopened.check(address, second, true).status, Status::Changed);
    }
};
QTEST_GUILESS_MAIN(HostTrustStoreTest)
#include "test_hosttruststore.moc"
