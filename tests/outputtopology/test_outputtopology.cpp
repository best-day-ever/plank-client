#include <QtTest>

#include "outputtopology.h"

class TestOutputTopology : public QObject
{
    Q_OBJECT

private slots:
    void parsesQualificationVector();
    void roundTripsQualificationVector();
    void rejectsDuplicateIdentity();
};

void TestOutputTopology::parsesQualificationVector()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "STATIONCONNECT_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v2.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());

    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document.object(), topology, &error), qPrintable(error));
    QCOMPARE(topology.outputs.size(), 2);
    QCOMPARE(topology.desktopWidth, 3840);
    QCOMPARE(topology.featureFlags & NvOutputTopology::SupportedFeatureFlags, 255);
    QVERIFY((topology.featureFlags & NvOutputTopology::TopologyGenerationFeature) != 0);
    QVERIFY(!topology.generation.isEmpty());
    QVERIFY(topology.supportsScaledSpan());
    QVERIFY(topology.supportsSeparateDisplays());
    QCOMPARE(topology.layoutKind, QString("dual-horizontal"));
    QCOMPARE(topology.virtualMode, QString("1920x1080"));
    QVERIFY(topology.virtualLayout);
    QCOMPARE(topology.selectDisplayMode(QString()), QString("scaled-span"));
    QCOMPARE(topology.selectDisplayMode(QString("single-output")), QString("single-output"));
    QCOMPARE(topology.selectDisplayMode(QString("separate-displays")), QString("separate-displays"));
    QCOMPARE(topology.selectOutput(QString()), QString("x11:DP-0"));
    QCOMPARE(topology.selectOutput(QString("x11:DP-2")), QString("x11:DP-2"));
    QCOMPARE(topology.selectOutput(QString("x11:missing")), QString("x11:DP-0"));
    QCOMPARE(topology.resolveHostLayout(QString("configured")), QString("dual-horizontal"));
    QCOMPARE(topology.resolveVirtualMode(QString("configured"), QString()), QString("1920x1080"));
    QCOMPARE(topology.outputs.at(1).sourceX, 1920);
}

void TestOutputTopology::roundTripsQualificationVector()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "STATIONCONNECT_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v2.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(document.object(), topology));

    NvOutputTopology restored;
    QVERIFY(NvOutputTopology::fromJson(topology.toJson(), restored));
    QCOMPARE(restored.toJson(), topology.toJson());
    QCOMPARE(restored.selectDisplayMode(QString()), QString("scaled-span"));
}

void TestOutputTopology::rejectsDuplicateIdentity()
{
    QJsonObject output {
        {"id", "x11:DP-2"}, {"name", "DP-2"}, {"x", 0}, {"y", 0},
        {"width", 3840}, {"height", 2160}, {"rotation", 0},
        {"refresh_millihz", 60000}, {"primary", true}, {"virtual", true},
        {"source_rect", QJsonObject {{"x", 0}, {"y", 0},
                                      {"width", 3840}, {"height", 2160}}},
    };
    QJsonObject document {
        {"schema_version", 2}, {"feature_flags", 255}, {"generation", "test"},
        {"layout", QJsonObject {{"kind", "dual-horizontal"}, {"virtual", true},
                                 {"virtual_mode", "3840x2160"}, {"output_count", 2}}},
        {"desktop", QJsonObject {{"x", 0}, {"y", 0}, {"width", 3840}, {"height", 2160}}},
        {"outputs", QJsonArray {output, output}},
    };
    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
