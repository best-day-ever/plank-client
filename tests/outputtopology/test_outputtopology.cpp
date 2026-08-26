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
    void enforcesHostDisplayPolicy();
    void validatesRequestedLayoutGeometry();
    void matchesOneClientDisplay();
    void matchesTwoClientDisplaysLeftToRight();
    void rejectsUnsupportedClientLayouts();
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
    QCOMPARE(topology.layoutKind, QString("dual-horizontal"));
    QCOMPARE(topology.virtualModes,
             QStringList({QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
    QVERIFY(topology.virtualLayout);
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
    const QStringList modes = NvOutputTopology::qualifiedVirtualModes();
    QCOMPARE(modes.size(), 13);
    QCOMPARE(modes.at(8), QStringLiteral("2560x2160"));
    QCOMPARE(modes.at(11), QStringLiteral("3840x2160"));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("1024x2160")),
             QSize(1024, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("4096x2160")),
             QSize(4096, 2160));
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("5120x2160")).isValid());
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("single"), {QStringLiteral("2560x2160")}),
             QSize(2560, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}),
             QSize(5120, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("1024x2160")}),
             QSize(5120, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("1280x2160")}),
             QSize(5376, 2160));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("4096x2160"), QStringLiteral("4096x2160")}),
             QSize(8192, 2160));
    QVERIFY(!NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"),
                 {QStringLiteral("3840x2160"), QStringLiteral("5120x2160")}).isValid());
}

void TestOutputTopology::enforcesHostDisplayPolicy()
{
    NvOutputTopology topology;
    QVERIFY(!topology.displayPolicyKnown());
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));

    topology.schemaVersion = NvOutputTopology::ProtocolVersion;
    topology.layoutKind = NvOutputTopology::PhysicalHostLayout;
    topology.virtualLayout = false;
    QVERIFY(topology.displayPolicyKnown());
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("single")));

    topology.layoutKind = NvOutputTopology::SingleHostLayout;
    topology.virtualLayout = true;
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("dual-horizontal")));
}

void TestOutputTopology::validatesRequestedLayoutGeometry()
{
    const QByteArray root = qgetenv("STATIONCONNECT_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v4.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();

    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(document, topology));
    QVERIFY(topology.matchesRequestedHostLayout(
                QStringLiteral("dual-horizontal"),
                {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));

    QJsonArray outputs = document.value("outputs").toArray();
    QJsonObject second = outputs.at(1).toObject();
    second["x"] = 2560;
    second["source_rect"] = QJsonObject {
        {"x", 2560}, {"y", 0}, {"width", 1280}, {"height", 2160}
    };
    outputs[1] = second;
    document["outputs"] = outputs;
    document["desktop"] = QJsonObject {
        {"x", 0}, {"y", 0}, {"width", 3840}, {"height", 2160}
    };

    QVERIFY(NvOutputTopology::fromJson(document, topology));
    QVERIFY(!topology.matchesRequestedHostLayout(
                QStringLiteral("dual-horizontal"),
                {QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
}

void TestOutputTopology::matchesOneClientDisplay()
{
    QString layout;
    QStringList modes;
    QString error;
    const QVector<NvClientDisplay> displays {
        {QRect(0, 0, 3840, 2160), QSize(3840, 2160)},
    };
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 displays, layout, modes, &error),
             qPrintable(error));
    QCOMPARE(layout, QStringLiteral("single"));
    QCOMPARE(modes, QStringList({QStringLiteral("3840x2160")}));
}

void TestOutputTopology::matchesTwoClientDisplaysLeftToRight()
{
    QString layout;
    QStringList modes;
    QString error;
    QVector<NvClientDisplay> displays {
        {QRect(3840, 0, 1280, 2160), QSize(1280, 2160)},
        {QRect(0, 0, 3840, 2160), QSize(3840, 2160)},
    };
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 displays, layout, modes, &error), qPrintable(error));
    QCOMPARE(layout, QStringLiteral("dual-horizontal"));
    QCOMPARE(modes, QStringList({QStringLiteral("3840x2160"),
                                 QStringLiteral("1280x2160")}));
}

void TestOutputTopology::rejectsUnsupportedClientLayouts()
{
    QString layout;
    QStringList modes;
    QString error;
    const QVector<NvClientDisplay> threeDisplays {
        {QRect(0, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(1920, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(3840, 0, 1920, 1080), QSize(1920, 1080)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                threeDisplays, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("one or two")));

    const QVector<NvClientDisplay> verticalDisplays {
        {QRect(0, 0, 1920, 1080), QSize(1920, 1080)},
        {QRect(0, 1080, 1920, 1080), QSize(1920, 1080)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                verticalDisplays, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("left to right")));

    const QVector<NvClientDisplay> unsupportedDisplay {
        {QRect(0, 0, 5120, 2160), QSize(5120, 2160)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                unsupportedDisplay, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("not a qualified")));
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
