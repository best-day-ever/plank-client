#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>

#include "outputtopology.h"
#include "../../app/streaming/macdisplaygeometry.h"

class TestOutputTopology : public QObject
{
    Q_OBJECT

private slots:
    void advertisesOnlyImplementedClipboardSupport();
    void parsesQualificationVector();
    void roundTripsQualificationVector();
    void rejectsDuplicateIdentity();
    void rejectsConfiguredModeMismatch();
    void acceptsTallCinemaModes();
    void negotiatesNotchSafeLaptopMode();
    void enforcesHostDisplayPolicy();
    void validatesRequestedLayoutGeometry();
    void matchesOneClientDisplay();
    void matchesTwoClientDisplaysLeftToRight();
    void rejectsUnsupportedClientLayouts();
    void parsesFixedCapture();
    void rejectsInvalidFixedCapture();
    void recognizesDescriptionCapabilities();
    void matchesMacClientCanvas();
    void matchesRetinaClientCanvas();
    void matchesMacFullscreenViewport();
    void buildsMacDisplayRequest();
    void computesClientMatchTarget();
    void bestFitsOddClientDisplays_data();
    void bestFitsOddClientDisplays();
    void bestFitsTwoClientDisplays();
    void keepsHostTopologyParsingStrict();
};

void TestOutputTopology::advertisesOnlyImplementedClipboardSupport()
{
    const auto negotiated = NvOutputTopology::SupportedFeatureFlags &
                            NvOutputTopology::ClipboardSyncFeature;
    const auto files = NvOutputTopology::SupportedFeatureFlags &
                       NvOutputTopology::ClipboardFilesFeature;
#ifdef Q_OS_MACOS
    QCOMPARE(negotiated, NvOutputTopology::ClipboardSyncFeature);
    QCOMPARE(files, NvOutputTopology::ClipboardFilesFeature);
#else
    QCOMPARE(negotiated, 0);
    QCOMPARE(files, 0);
#endif
    // A host without the feature cannot enable it on any client platform.
    QCOMPARE(NvOutputTopology::SupportedFeatureFlags &
             ~NvOutputTopology::ClipboardSyncFeature &
             NvOutputTopology::ClipboardSyncFeature, 0);
}

void TestOutputTopology::matchesMacFullscreenViewport()
{
    for (int inset : {0, 24, 34, 38}) {
        int logicalHeight = 1107;
        int pixelHeight = 2214;
        QVERIFY(MacDisplayGeometry::insetTop(1710, logicalHeight, 3420, pixelHeight, inset));
        int scale = 0;
        QString error;
        const QVector<NvClientDisplay> displays = {{QRect(-1710, inset, 1710, logicalHeight),
            QSize(3420, pixelHeight), QSize(3420, pixelHeight)}};
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(displays, &error, &scale),
                 QStringLiteral("3420x%1").arg(pixelHeight));
        QCOMPARE(scale, 2);
        QVERIFY(error.isEmpty());
        const auto request = NvOutputTopology::macDisplayRequest(
            QStringLiteral("3420x%1").arg(pixelHeight), "hevc-10-444-videotoolbox", scale);
        QVERIFY(!request.isEmpty());
    }
}

void TestOutputTopology::buildsMacDisplayRequest()
{
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + "/tests/protocol/macos-display-v3.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-420-videotoolbox", 2), fixture);
    auto manual = fixture;
    manual["scale"] = 1;
    manual["encoding_mode"] = "hevc-10-444-videotoolbox";
    QCOMPARE(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-444-videotoolbox", 1), manual);
    QVERIFY(NvOutputTopology::macDisplayRequest("3420x2214", "hevc-10-444-videotoolbox", 3).isEmpty());
    QVERIFY(NvOutputTopology::macDisplayRequest("3420x2214", "invalid", 2).isEmpty());
    QVERIFY(NvOutputTopology::macDisplayRequest("3421x2214", "hevc-10-444-videotoolbox", 2).isEmpty());
}

void TestOutputTopology::matchesRetinaClientCanvas()
{
    int scale = 0;
    QString error;
    // Current compositor backing pixels, not panel-native pixels. Odd logical
    // heights are valid when the actual encoded height remains even.
    const QVector<NvClientDisplay> laptop = {{QRect(0,0,1710,1107), QSize(3024,1964), QSize(3420,2214)}};
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(laptop, &error, &scale), QString("3420x2214"));
    QCOMPARE(scale, 2);
    QVERIFY(error.isEmpty());
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(3840,2160), QSize(3840,2160)},
        {QRect(-1710,0,1710,1107), QSize(3024,1964), QSize(3420,2214)}}, &error, &scale), QString("7260x2214"));
    QCOMPARE(scale, 2);
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(1920,1080), QSize(1920,1080)}}, &error, &scale), QString("1920x1080"));
    QCOMPARE(scale, 1);
    // Linux clients retain their existing native-pixel policy.
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1920,1080), QSize(3840,2160)}}, &error, &scale), QString("3840x2160"));
    QCOMPARE(scale, 1);
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1710,1107), QSize(3420,2214), QSize(3420,2214)},
        {QRect(1710,0,1920,1080), QSize(1920,1080), QSize(1920,1080)}}, &error, &scale).isEmpty());
    QVERIFY(error.contains("same 1x or 2x"));
    QCOMPARE(scale, 1);
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,1710,1107), QSize(3024,1964), QSize(3024,1964)}}, &error, &scale).isEmpty());
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,2500,1500), QSize(5000,3000), QSize(5000,3000)},
        {QRect(2500,0,2500,1500), QSize(5000,3000), QSize(5000,3000)}}, &error, &scale).isEmpty());
}

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
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,3840,2160), QSize(3840,2160)},
        {QRect(3840,0,3840,2160), QSize(3840,2160)}}), QString("7680x2160"));
    for (QSize native : {QSize(3024,1964), QSize(3456,2234), QSize(2880,1864), QSize(2160,3840)}) {
        const QString mode = QStringLiteral("%1x%2").arg(native.width()).arg(native.height());
        const QVector<NvClientDisplay> screens = {{QRect(QPoint(0,0), native / 2), native}};
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(screens), mode);
        QCOMPARE(NvOutputTopology::macDisplayModeSize(mode), native);
        QVERIFY(!NvOutputTopology::virtualModeSize(mode).isValid());
        // A Linux host has no such virtual mode: the closest qualified mode
        // is used and flagged as fitted (letterboxed), never the raw size.
        QString layout;
        QStringList modes;
        bool fitted = false;
        QVERIFY(NvOutputTopology::resolveClientDisplayLayout(screens, layout, modes, nullptr, &fitted));
        QVERIFY(fitted);
        QVERIFY(!modes.contains(mode));
        QVERIFY(NvOutputTopology::qualifiedVirtualModes().contains(modes.value(0)));
    }
    for (const QString mode : {"0x2160", "3023x1964", "3024x1963", "8194x2160",
            "3840x8194", "-2x2", "03024x1964", "3024X1964", "3024x1964x2", "2x+2",
            "2x2 ", "9999999999999999999x2"}) {
        QVERIFY(!NvOutputTopology::macDisplayModeSize(mode).isValid());
    }
    QVERIFY(NvOutputTopology::resolveMacClientDisplayMode({
        {QRect(0,0,5120,2160), QSize(5120,2160)},
        {QRect(5120,0,5120,2160), QSize(5120,2160)}}).isEmpty());
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
    // Linux-only launch features must never alias a Mac fixed-capture bit.
    QVERIFY(!(NvOutputTopology::FixedCaptureFlags & NvOutputTopology::DesktopSignOutFeature));
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
    QCOMPARE(modes.size(), 13);
    QCOMPARE(modes.at(6), QStringLiteral("2560x2160"));
    QCOMPARE(modes.at(10), QStringLiteral("3840x2160"));
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

void TestOutputTopology::negotiatesNotchSafeLaptopMode()
{
    // A 14-inch MacBook Pro in its default "Looks like 1512x982" scaling:
    // native fullscreen stops below the camera housing (37 points).
    int logicalHeight = 982;
    int pixelHeight = 1964;
    QVERIFY(MacDisplayGeometry::insetTop(1512, logicalHeight, 3024, pixelHeight, 37));
    const QString viewport = QStringLiteral("3024x%1").arg(pixelHeight);
    QCOMPARE(viewport, QStringLiteral("3024x1890"));
    QVERIFY(NvOutputTopology::qualifiedVirtualModes().contains(viewport));
    QCOMPARE(NvOutputTopology::virtualModeSize(viewport), QSize(3024, 1890));
    // The panel itself is never presented 1:1, so it is not a mode.
    QVERIFY(!NvOutputTopology::virtualModeSize(QStringLiteral("3024x1964")).isValid());

    const int withoutFeature = NvOutputTopology::SupportedFeatureFlags &
            ~NvOutputTopology::NotchSafeLaptopModesFeature;
    QVERIFY(NvOutputTopology::hostAcceptsVirtualMode(viewport, NvOutputTopology::SupportedFeatureFlags));
    QVERIFY(!NvOutputTopology::hostAcceptsVirtualMode(viewport, withoutFeature));
    QVERIFY(NvOutputTopology::hostAcceptsVirtualMode(QStringLiteral("2560x1600"), withoutFeature));
    QVERIFY(!NvOutputTopology::hostAcceptsVirtualMode(QStringLiteral("3024x1964"),
                                                      NvOutputTopology::SupportedFeatureFlags));
    QCOMPARE(NvOutputTopology::virtualCanvasSize(
                 QStringLiteral("dual-horizontal"), {viewport, QStringLiteral("5120x2160")}),
             QSize(8144, 2160));

    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) +
               "/tests/protocol/output-topology-v13-notch-safe.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    NvOutputTopology topology;
    QString error;
    QVERIFY2(NvOutputTopology::fromJson(document, topology, &error), qPrintable(error));
    QVERIFY(topology.featureFlags & NvOutputTopology::NotchSafeLaptopModesFeature);
    QCOMPARE(topology.virtualModes, QStringList({viewport}));
    QVERIFY(topology.matchesRequestedHostLayout(QStringLiteral("single"), {viewport}));
    NvOutputTopology restored;
    QVERIFY(NvOutputTopology::fromJson(topology.toJson(), restored));
    QCOMPARE(restored.toJson(), topology.toJson());

    // A host that has not advertised the feature cannot report the mode.
    document["feature_flags"] = document.value("feature_flags").toInt() &
            ~NvOutputTopology::NotchSafeLaptopModesFeature;
    QVERIFY(!NvOutputTopology::fromJson(document, topology));
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

    // An odd monitor is no longer refused: 5:4 gets the closest aspect.
    const QVector<NvClientDisplay> oddDisplay {
        {QRect(0, 0, 1280, 1024), QSize(1280, 1024)},
    };
    bool fitted = false;
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout(
                oddDisplay, layout, modes, &error, &fitted));
    QVERIFY(fitted);
    QCOMPARE(modes, QStringList({QStringLiteral("1920x1200")}));

    const QVector<NvClientDisplay> undetected {
        {QRect(0, 0, 1280, 1024), QSize()},
    };
    QVERIFY(!NvOutputTopology::resolveClientDisplayLayout(
                undetected, layout, modes, &error, &fitted));
    QVERIFY(error.contains(QStringLiteral("could not be detected")));
    QVERIFY(modes.isEmpty());
    QVERIFY(layout.isEmpty());
}

void TestOutputTopology::computesClientMatchTarget()
{
    // Desktop backing below the panel (macOS 1x "1920x1200" on a 14" panel).
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(1920, 1200), QSize(3024, 1964)), QSize(1920, 1200));
    // Default Retina "looks like 1512x982": backing equals the panel.
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(3024, 1964), QSize(3024, 1964)), QSize(3024, 1964));
    // "More Space" renders 3600x2338 and scales it down onto the panel.
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(3600, 2338), QSize(3024, 1964)), QSize(3024, 1964));
    // 16" "More Space" 2056x1329@2x on a 3456x2234 panel.
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(4112, 2658), QSize(3456, 2234)), QSize(3456, 2234));
    // Unknown backing (not macOS) or unknown panel.
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(), QSize(2560, 1440)), QSize(2560, 1440));
    QCOMPARE(NvOutputTopology::clientMatchTarget(QSize(2560, 1440), QSize()), QSize(2560, 1440));
    QVERIFY(!NvOutputTopology::clientMatchTarget(QSize(), QSize()).isValid());
    const NvClientDisplay display {QRect(0, 0, 1800, 1169), QSize(3024, 1964), QSize(3600, 2338)};
    QCOMPARE(NvOutputTopology::clientMatchTarget(display), QSize(3024, 1964));
    // Notched 14" in the default "looks like 1512x982": native fullscreen is 1512x945 pt below the camera
    // housing, so the target is the 16:10 viewport and the closest mode fits it without letterboxing.
    const NvClientDisplay notched {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1890)};
    QCOMPARE(NvOutputTopology::clientMatchTarget(notched), QSize(3024, 1890));
    QString layout;
    QStringList modes;
    bool fitted = false;
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({notched}, layout, modes, nullptr, &fitted));
    QCOMPARE(modes, QStringList {QStringLiteral("2560x1600")});
    QVERIFY(fitted);
    // "More Space" 1800x1169 pt: the viewport 3600x2260 is capped to the panel.
    const NvClientDisplay moreSpace {QRect(0, 0, 1800, 1169), QSize(3024, 1964), QSize(3600, 2338), QSize(3600, 2260)};
    QCOMPARE(NvOutputTopology::clientMatchTarget(moreSpace), QSize(3024, 1898));
}

void TestOutputTopology::bestFitsOddClientDisplays_data()
{
    QTest::addColumn<QSize>("target");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("fitted");
    QTest::newRow("MacBook Pro 14") << QSize(3024, 1964) << "2560x1600" << true;
    QTest::newRow("MacBook Pro 16") << QSize(3456, 2234) << "2560x1600" << true;
    QTest::newRow("MacBook Air 13") << QSize(2560, 1664) << "2560x1600" << true;
    QTest::newRow("MacBook Air 15") << QSize(2940, 1912) << "2560x1600" << true;
    QTest::newRow("1x 16:10 desktop") << QSize(1920, 1200) << "1920x1200" << false;
    QTest::newRow("4K") << QSize(3840, 2160) << "3840x2160" << false;
    QTest::newRow("QHD") << QSize(2560, 1440) << "2560x1440" << false;
    // 5K 16:9: the same shape wins over the wider 5120x2160.
    QTest::newRow("5K") << QSize(5120, 2880) << "3840x2160" << true;
    QTest::newRow("Ultrawide 3440") << QSize(3440, 1440) << "3440x1440" << false;
    QTest::newRow("Ultrawide 3840x1600 fits") << QSize(3840, 1600) << "3840x1600" << false;
    QTest::newRow("5120x1440 super-wide") << QSize(5120, 1440) << "3440x1440" << true;
    // Smaller than every qualified mode (a Retina desktop at 1x points):
    // the closest aspect that enlarges least; presentation scales it down.
    QTest::newRow("1512x982 nothing fits") << QSize(1512, 982) << "1920x1200" << true;
    QTest::newRow("1366x768 nothing fits") << QSize(1366, 768) << "1920x1080" << true;
    // Portrait monitors get the tall modes.
    QTest::newRow("portrait 4K") << QSize(2160, 3840) << "1280x2160" << true;
    QTest::newRow("portrait exact") << QSize(1280, 2160) << "1280x2160" << false;
}

void TestOutputTopology::bestFitsOddClientDisplays()
{
    QFETCH(QSize, target);
    QFETCH(QString, mode);
    QFETCH(bool, fitted);
    QCOMPARE(NvOutputTopology::rankedVirtualModes(target).value(0), mode);

    QString layout;
    QStringList modes;
    QString error;
    bool wasFitted = !fitted;
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(
                 {{QRect(QPoint(0, 0), target), target}}, layout, modes, &error, &wasFitted),
             qPrintable(error));
    QCOMPARE(layout, QStringLiteral("single"));
    QCOMPARE(modes, QStringList({mode}));
    QCOMPARE(wasFitted, fitted);
    // A fitting mode never exceeds the client (no upscale) unless none fits.
    const QSize size = NvOutputTopology::virtualModeSize(mode);
    if (target.width() >= 1920 && target.height() >= 1080 && target.width() >= target.height()) {
        QVERIFY(size.width() <= target.width() && size.height() <= target.height());
    }
    // Every ranked mode is qualified; each appears once.
    const QStringList ranked = NvOutputTopology::rankedVirtualModes(target);
    for (const QString& candidate : ranked) {
        QVERIFY(NvOutputTopology::qualifiedVirtualModes().contains(candidate));
        QCOMPARE(ranked.count(candidate), 1);
    }
    QVERIFY(NvOutputTopology::rankedVirtualModes(QSize()).isEmpty());
}

void TestOutputTopology::bestFitsTwoClientDisplays()
{
    QString layout;
    QStringList modes;
    QString error;
    bool fitted = false;
    // MacBook Pro 14 (default Retina) left of a 4K monitor.
    QVERIFY2(NvOutputTopology::resolveClientDisplayLayout({
        {QRect(1512, 0, 3840, 2160), QSize(3840, 2160)},
        {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964)}},
        layout, modes, &error, &fitted), qPrintable(error));
    QCOMPARE(layout, QStringLiteral("dual-horizontal"));
    QCOMPARE(modes, QStringList({QStringLiteral("2560x1600"), QStringLiteral("3840x2160")}));
    QVERIFY(fitted);
    QCOMPARE(NvOutputTopology::virtualCanvasSize(layout, modes), QSize(6400, 2160));

    // Two exact qualified monitors stay exact.
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({
        {QRect(0, 0, 2560, 1440), QSize(2560, 1440)},
        {QRect(2560, 0, 1920, 1080), QSize(1920, 1080)}},
        layout, modes, &error, &fitted));
    QCOMPARE(modes, QStringList({QStringLiteral("2560x1440"), QStringLiteral("1920x1080")}));
    QVERIFY(!fitted);

    // Canvas guard: two 5120x2160 would be 10240 wide; each steps down its
    // own ranking (closest aspect first: 3440x1440) until the pair fits the
    // 8192 canvas.
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({
        {QRect(0, 0, 5120, 2160), QSize(5120, 2160)},
        {QRect(5120, 0, 5120, 2160), QSize(5120, 2160)}},
        layout, modes, &error, &fitted));
    QVERIFY(fitted);
    QCOMPARE(modes, QStringList({QStringLiteral("3440x1440"), QStringLiteral("3440x1440")}));
    const QSize canvas = NvOutputTopology::virtualCanvasSize(layout, modes);
    QVERIFY(canvas.isValid());
    QVERIFY(canvas.width() <= NvOutputTopology::MaximumVirtualCanvasWidth);
    QVERIFY(canvas.height() <= 2160);

    // 5K + 4K: the wider one steps down first.
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({
        {QRect(0, 0, 5120, 2160), QSize(5120, 2160)},
        {QRect(5120, 0, 3840, 2160), QSize(3840, 2160)}},
        layout, modes, &error, &fitted));
    QCOMPARE(modes, QStringList({QStringLiteral("3440x1440"), QStringLiteral("3840x2160")}));
    QVERIFY(NvOutputTopology::virtualCanvasSize(layout, modes).width() <= 8192);
}

void TestOutputTopology::keepsHostTopologyParsingStrict()
{
    // Best fit happens on the client only: a host that reports a mode off
    // the allowlist is still rejected, whatever the client asked for.
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + "/tests/protocol/output-topology-v13.json");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject vector = QJsonDocument::fromJson(file.readAll()).object();
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(vector, topology));
    for (const QString& odd : {QStringLiteral("3024x1964"), QStringLiteral("2560x1664"),
                               QStringLiteral("1512x982")}) {
        QJsonObject object = vector;
        QJsonObject layout = object["layout"].toObject();
        layout["virtual_modes"] = QJsonArray {odd, QStringLiteral("1280x2160")};
        object["layout"] = layout;
        QVERIFY2(!NvOutputTopology::fromJson(object, topology), qPrintable(odd));
        QVERIFY(!NvOutputTopology::virtualModeSize(odd).isValid());
    }
}

QTEST_APPLESS_MAIN(TestOutputTopology)
#include "test_outputtopology.moc"
