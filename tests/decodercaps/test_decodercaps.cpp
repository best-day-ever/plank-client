#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "decodercaps.h"

// The 8K decoder test on this Mac: whatever the chip, the answer is one of
// the test sizes or the 4K fallback, it is cached, and modes without an 8K
// test (H.264) have no answer.
class TestDecoderCaps : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void probesEachHevcProfileOnce();
    void modesWithoutATestHaveNoAnswer();

private:
    QTemporaryDir m_Settings;
};

void TestDecoderCaps::initTestCase()
{
    QVERIFY(m_Settings.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("PlankTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DecoderCaps"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_Settings.path());
}

void TestDecoderCaps::probesEachHevcProfileOnce()
{
    for (const char* mode : {"hevc-10-444-nvenc", "hevc-10-420-nvenc"}) {
        const QString encodingMode = QString::fromLatin1(mode);
        QVERIFY(!DecoderCaps::cachedMaximum(encodingMode).isValid());
        const QSize maximum = DecoderCaps::probe(encodingMode);
        qInfo() << encodingMode << "hardware maximum" << maximum;
        QVERIFY2(maximum == QSize(8192, 4320) || maximum == QSize(7680, 4320) ||
                 maximum == DecoderCaps::fallbackMaximum(), mode);
        QCOMPARE(DecoderCaps::cachedMaximum(encodingMode), maximum);
        QCOMPARE(DecoderCaps::maximum(encodingMode), maximum);
    }
    // A Mac host's modes share the test of the same profile.
    QCOMPARE(DecoderCaps::cachedMaximum(QStringLiteral("hevc-10-444-videotoolbox")),
             DecoderCaps::cachedMaximum(QStringLiteral("hevc-10-444-nvenc")));
}

void TestDecoderCaps::modesWithoutATestHaveNoAnswer()
{
    for (const char* mode : {"h264-8-444-nvenc", "h264-10-444-software", "", "unknown"}) {
        QVERIFY2(!DecoderCaps::probe(QString::fromLatin1(mode)).isValid(), mode);
        QVERIFY2(!DecoderCaps::maximum(QString::fromLatin1(mode)).isValid(), mode);
    }
}

QTEST_GUILESS_MAIN(TestDecoderCaps)
#include "test_decodercaps.moc"
