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
    void parsesFixedCapture();
    void rejectsInvalidFixedCapture();
    void recognizesDescriptionCapabilities();
    void matchesMacClientCanvas();
};

void TestOutputTopology::recognizesDescriptionCapabilities()
{
    const int version = NvOutputTopology::ProtocolVersion;
    const int fixed = NvOutputTopology::FixedCaptureFlags;
    const int linuxFlags = NvOutputTopology::SupportedFeatureFlags;
    QVERIFY(NvOutputTopology::supportsDescription(version, fixed));
    QVERIFY(NvOutputTopology::supportsDescription(version, linuxFlags));
    QVERIFY(!NvOutputTopology::supportsDescription(version - 1, fixed));
    QVERIFY(!NvOutputTopology::supportsDescription(version + 1, linuxFlags));
    QVERIFY(!NvOutputTopology::supportsDescription(version, 0));
    QVERIFY(!NvOutputTopology::supportsDescription(version, NvOutputTopology::OutputTopologyFeature));
    QVERIFY(!NvOutputTopology::supportsDescription(version, fixed ^ NvOutputTopology::HostLayoutMetadataFeature));
    QVERIFY(!NvOutputTopology::supportsDescription(version, fixed | NvOutputTopology::SelectedOutputFeature));
    QCOMPARE(NvOutputTopology::hostPlatform(version, fixed), 2);
    QCOMPARE(NvOutputTopology::hostPlatform(version, linuxFlags), 1);
    QCOMPARE(NvOutputTopology::hostPlatform(version, 0), 0);
    QCOMPARE(NvOutputTopology::hostPlatform(version - 1, fixed), 0);
}

void TestOutputTopology::matchesMacClientCanvas()
{
    // Pixel dimensions are independent of logical compositor scaling.
    for (QSize logical : {QSize(3840,2160), QSize(3072,1728), QSize(1920,1080)}) {
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({{QRect(QPoint(-100,0), logical), QSize(3840,2160)}}), QString("3840x2160"));
    }
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({{QRect(0,0,5120,2160), QSize(5120,2160)}}), QString("5120x2160"));
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(2560,0,2560,2160), QSize(2560,2160)},
        {QRect(0,0,2560,2160), QSize(2560,2160)}}), QString("5120x2160"));
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,3840,2160), QSize(3840,2160)},
        {QRect(3840,0,3840,2160), QSize(3840,2160)}}).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(1920,1080)},
        {QRect(0,1080,1920,1080), QSize(1920,1080)}}).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({}).isEmpty());
}

static QJsonObject fixedCaptureFixture()
{
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + "/tests/protocol/fixed-capture-v13.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void TestOutputTopology::parsesFixedCapture()
{
    const auto fixture = fixedCaptureFixture();
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(fixture, topology));
    QCOMPARE(topology.featureFlags, NvOutputTopology::FixedCaptureFlags);
    QCOMPARE(topology.desktopWidth, 3840);
    QCOMPARE(topology.desktopHeight, 2160);
    QCOMPARE(topology.captureLogicalBounds, QRectF(-1920, 0, 1920, 1080));
    QCOMPARE(topology.outputs.size(), 1);
    QCOMPARE(topology.toJson(), fixture);
    QVERIFY(!(topology.featureFlags & NvOutputTopology::UnifiedAbsoluteInputFeature));
    QVERIFY(!(topology.featureFlags & NvOutputTopology::SessionTakeoverFeature));
    QVERIFY(!(NvOutputTopology::SupportedFeatureFlags & NvOutputTopology::FixedCaptureFeature));
    // Reuse is atomic, including failures; never retain stale logical bounds.
    QVERIFY(!NvOutputTopology::fromJson({}, topology));
    QCOMPARE(topology.toJson(), fixture);
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    for (QSize points : {QSize(3840, 2160), QSize(2560, 1440)}) {
        auto varied = fixture;
        auto capture = varied["capture"].toObject();
        capture["logical_bounds"] = QJsonObject {{"x", 100}, {"y", -500},
            {"width", points.width()}, {"height", points.height()}};
        varied["capture"] = capture;
        QVERIFY(NvOutputTopology::fromJson(varied, topology));
        QCOMPARE(topology.captureLogicalBounds, QRectF(QPointF(100, -500), points));
        QCOMPARE(topology.desktopWidth, 3840);
        QCOMPARE(topology.toJson(), varied);
    }
}

void TestOutputTopology::rejectsInvalidFixedCapture()
{
    const auto fixture = fixedCaptureFixture();
    QVERIFY(!fixture.isEmpty());
    NvOutputTopology topology;
    for (QJsonValue bad : {QJsonValue(-1), QJsonValue(0), QJsonValue(3), QJsonValue(8194),
                          QJsonValue(1e99), QJsonValue(3840.5), QJsonValue("3840")}) {
        auto object = fixture;
        auto capture = object["capture"].toObject();
        capture["width"] = bad; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
    for (const char* field : {"rgb_identity", "chroma", "range", "encoding_mode", "transfer"}) {
        auto object = fixture;
        auto capture = object["capture"].toObject();
        auto profile = capture["encoding_profile"].toObject();
        profile[field] = "incorrect"; capture["encoding_profile"] = profile; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
    auto object = fixture;
    object["feature_flags"] = NvOutputTopology::FixedCaptureFlags | NvOutputTopology::SessionTakeoverFeature;
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    object = fixture; object["generation"] = "not-a-generation";
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    object = fixture; object["layout"] = QJsonObject();
    QVERIFY(!NvOutputTopology::fromJson(object, topology));
    for (QJsonValue bad : {QJsonValue(0), QJsonValue(-1), QJsonValue(1e99), QJsonValue("1920")}) {
        object = fixture;
        auto capture = object["capture"].toObject();
        auto logical = capture["logical_bounds"].toObject();
        logical["width"] = bad; capture["logical_bounds"] = logical; object["capture"] = capture;
        QVERIFY(!NvOutputTopology::fromJson(object, topology));
    }
}

void TestOutputTopology::parsesQualificationVector()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "PLANK_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVERIFY(document.isObject());

    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document.object(), topology, &error), qPrintable(error));
    QCOMPARE(topology.outputs.size(), 2);
    QCOMPARE(topology.desktopWidth, 5120);
    QCOMPARE(topology.featureFlags & NvOutputTopology::SupportedFeatureFlags, 524287);
    QVERIFY((topology.featureFlags & NvOutputTopology::CaptureSourceSelectionFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::EncoderBackendSelectionFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::NvfbcHevc10NvencFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::FixedTransportMtuFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::SessionTakeoverFeature) != 0);
    QVERIFY((topology.featureFlags & NvOutputTopology::TopologyGenerationFeature) != 0);
    QVERIFY(!topology.generation.isEmpty());
    QCOMPARE(topology.layoutKind, QString("dual-horizontal"));
    QCOMPARE(topology.virtualModes,
             QStringList({QStringLiteral("3840x2160"), QStringLiteral("1280x2160")}));
    QVERIFY(topology.virtualLayout);
    QCOMPARE(topology.startupLayoutKind, QStringLiteral("physical"));
    QCOMPARE(topology.allowedLayoutKinds,
             QStringList({QStringLiteral("physical"), QStringLiteral("single"),
                          QStringLiteral("dual-horizontal")}));
    QCOMPARE(topology.outputs.at(0).configuredMode, QString("3840x2160"));
    QCOMPARE(topology.outputs.at(1).configuredMode, QString("1280x2160"));
    QCOMPARE(topology.outputs.at(1).sourceX, 3840);
}

void TestOutputTopology::roundTripsQualificationVector()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QVERIFY2(!root.isEmpty(), "PLANK_REPO_ROOT must identify the repository root");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
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
        {"schema_version", 12}, {"feature_flags", 65535}, {"generation", "test"},
        {"layout", QJsonObject {{"kind", "dual-horizontal"}, {"virtual", true},
                                 {"virtual_modes", QJsonArray {"3840x2160", "3840x2160"}},
                                 {"output_count", 2}, {"startup_kind", "physical"},
                                 {"allowed_kinds", QJsonArray {
                                      "physical", "single", "dual-horizontal"}}}},
        {"desktop", QJsonObject {{"x", 0}, {"y", 0}, {"width", 7680}, {"height", 2160}}},
        {"outputs", QJsonArray {output, output}},
    };
    NvOutputTopology topology;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
}

void TestOutputTopology::rejectsConfiguredModeMismatch()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
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
    QCOMPARE(modes.size(), 12);
    QCOMPARE(modes.at(6), QStringLiteral("2560x2160"));
    QCOMPARE(modes.at(9), QStringLiteral("3840x2160"));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("1024x2160")),
             QSize(1024, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("4096x2160")),
             QSize(4096, 2160));
    QCOMPARE(NvOutputTopology::virtualModeSize(QStringLiteral("5120x2160")),
             QSize(5120, 2160));
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("1280x720")).isValid());
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("1280x1024")).isValid());
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
    topology.startupLayoutKind = NvOutputTopology::PhysicalHostLayout;
    topology.allowedLayoutKinds = {
        QString::fromLatin1(NvOutputTopology::PhysicalHostLayout),
        QString::fromLatin1(NvOutputTopology::SingleHostLayout),
        QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout)
    };
    topology.virtualLayout = false;
    QVERIFY(topology.displayPolicyKnown());
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));

    topology.layoutKind = NvOutputTopology::SingleHostLayout;
    topology.startupLayoutKind = NvOutputTopology::SingleHostLayout;
    topology.allowedLayoutKinds = {
        QString::fromLatin1(NvOutputTopology::SingleHostLayout),
        QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout)
    };
    topology.virtualLayout = true;
    QVERIFY(!topology.allowsBookmarkHostLayout(QStringLiteral("physical")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("match-client")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("single")));
    QVERIFY(topology.allowsBookmarkHostLayout(QStringLiteral("dual-horizontal")));
}

void TestOutputTopology::validatesRequestedLayoutGeometry()
{
    const QByteArray root = qgetenv("PLANK_REPO_ROOT");
    QFile file(QString::fromUtf8(root) + "/tests/protocol/output-topology-v13.json");
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
        {QRect(0, 0, 1280, 1024), QSize(1280, 1024)},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                unsupportedDisplay, layout, modes, &error));
    QVERIFY(error.contains(QStringLiteral("not a qualified")));
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
