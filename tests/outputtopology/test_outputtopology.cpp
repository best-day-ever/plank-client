#include <QtTest>

#include "outputtopology.h"

class TestOutputTopology : public QObject
{
    Q_OBJECT

private slots:
    void parsesQualificationVector();
    void roundTripsQualificationVector();
    void rejectsDuplicateIdentity();
    void rejectsConfiguredModeMismatch();
    void acceptsTallCinemaModes();
};

void TestOutputTopology::parsesQualificationVector()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "STATIONCONNECT_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v4.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());

    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document.object(), topology, &error), qPrintable(error));
    QCOMPARE(topology.outputs.size(), 2);
    QCOMPARE(topology.desktopWidth, 5120);
    QCOMPARE(topology.featureFlags & NvOutputTopology::SupportedFeatureFlags, 1023);
    QVERIFY((topology.featureFlags & NvOutputTopology::TopologyGenerationFeature) != 0);
    QVERIFY(!topology.generation.isEmpty());
    QVERIFY(topology.supportsScaledSpan());
    QVERIFY(topology.supportsSeparateDisplays());
    QCOMPARE(topology.layoutKind, QString("dual-horizontal"));
    QCOMPARE(topology.virtualModes,
             QStringList({QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
    QVERIFY(topology.virtualLayout);
    QCOMPARE(topology.selectDisplayMode(QString()), QString("scaled-span"));
    QCOMPARE(topology.selectDisplayMode(QString("single-output")), QString("single-output"));
    QCOMPARE(topology.selectDisplayMode(QString("separate-displays")), QString("separate-displays"));
    QCOMPARE(topology.selectOutput(QString()), QString("x11:DP-0"));
    QCOMPARE(topology.selectOutput(QString("x11:DP-2")), QString("x11:DP-2"));
    QCOMPARE(topology.selectOutput(QString("x11:missing")), QString("x11:DP-0"));
    QCOMPARE(topology.resolveHostLayout(QString("configured")), QString("dual-horizontal"));
    QCOMPARE(topology.resolveVirtualModes(QString("configured"), QString(), QString()),
             topology.virtualModes);
    QCOMPARE(topology.resolveVirtualModes(QString("dual-horizontal"),
                                          QString("4096x2160"),
                                          QString("1024x2160")),
             QStringList({QStringLiteral("4096x2160"), QStringLiteral("1024x2160")}));
    QCOMPARE(topology.outputs.at(0).configuredMode, QString("3840x2160"));
    QCOMPARE(topology.outputs.at(1).configuredMode, QString("1280x2160"));
    QCOMPARE(topology.outputs.at(1).sourceX, 3840);
}

void TestOutputTopology::roundTripsQualificationVector()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "STATIONCONNECT_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v4.json");
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
        {"configured_mode", "3840x2160"},
        {"source_rect", QJsonObject {{"x", 0}, {"y", 0},
                                      {"width", 3840}, {"height", 2160}}},
    };
    QJsonObject document {
        {"schema_version", 4}, {"feature_flags", 1023}, {"generation", "test"},
        {"layout", QJsonObject {{"kind", "dual-horizontal"}, {"virtual", true},
                                 {"virtual_modes", QJsonArray {"3840x2160", "3840x2160"}},
                                 {"output_count", 2}}},
        {"desktop", QJsonObject {{"x", 0}, {"y", 0}, {"width", 7680}, {"height", 2160}}},
        {"outputs", QJsonArray {output, output}},
    };
    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

void TestOutputTopology::rejectsConfiguredModeMismatch()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v4.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    QJsonArray outputs = document.value("outputs").toArray();
    QJsonObject second = outputs.at(1).toObject();
    second["configured_mode"] = QStringLiteral("1024x2160");
    outputs[1] = second;
    document["outputs"] = outputs;

    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

void TestOutputTopology::acceptsTallCinemaModes()
{
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("1024x2160")),
             QSize(1024, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("4096x2160")),
             QSize(4096, 2160));
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("5120x2160")).isValid());
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
