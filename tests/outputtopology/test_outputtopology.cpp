#include <QtTest>

#include "outputtopology.h"

class TestOutputTopology : public QObject
{
    Q_OBJECT

private slots:
    void parsesQualificationVector();
    void rejectsDuplicateIdentity();
};

void TestOutputTopology::parsesQualificationVector()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "STATIONCONNECT_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v1.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());

    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document.object(), topology, &error), qPrintable(error));
    QCOMPARE(topology.outputs.size(), 2);
    QCOMPARE(topology.desktopWidth, 5120);
    QCOMPARE(topology.featureFlags & NvOutputTopology::SupportedFeatureFlags, 31);
    QVERIFY((topology.featureFlags & NvOutputTopology::TopologyGenerationFeature) != 0);
    QVERIFY(!topology.generation.isEmpty());
    QVERIFY(topology.supportsScaledSpan());
    QCOMPARE(topology.selectDisplayMode(QString()), QString("scaled-span"));
    QCOMPARE(topology.selectDisplayMode(QString("single-output")), QString("single-output"));
    QCOMPARE(topology.selectOutput(QString()), QString("x11:DP-2"));
    QCOMPARE(topology.selectOutput(QString("x11:DP-1")), QString("x11:DP-1"));
    QCOMPARE(topology.selectOutput(QString("x11:missing")), QString("x11:DP-2"));
}

void TestOutputTopology::rejectsDuplicateIdentity()
{
    QJsonObject output {
        {"id", "x11:DP-2"}, {"name", "DP-2"}, {"x", 0}, {"y", 0},
        {"width", 3840}, {"height", 2160}, {"rotation", 0},
        {"refresh_millihz", 60000}, {"primary", true},
    };
    QJsonObject document {
        {"schema_version", 1}, {"feature_flags", 31}, {"generation", "test"},
        {"desktop", QJsonObject {{"x", 0}, {"y", 0}, {"width", 3840}, {"height", 2160}}},
        {"outputs", QJsonArray {output, output}},
    };
    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
