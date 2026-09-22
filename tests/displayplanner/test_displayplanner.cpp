#include <QtTest>

#include "clientdisplayprobe.h"
#include "displayarrangement.h"
#include "displayplanner.h"
#include "displayprofile.h"
#include "outputtopology.h"

#include <QJsonArray>
#include <QJsonDocument>

// Display setup: monitor identity, profiles and the layout planner.

namespace {

NvClientDisplay monitor(const QString& key, const QRect& bounds, const QSize& pixels,
                        const QString& name = QString(), bool builtIn = false)
{
    NvClientDisplay display {bounds, pixels, pixels};
    display.key = key;
    display.name = name;
    display.builtIn = builtIn;
    return display;
}

// A Mac display: logical points, backing pixels (the panel's own pixels
// unless given), optionally the notch-safe fullscreen viewport.
NvClientDisplay mac(const QString& key, const QRect& points, const QSize& backing, bool main = false,
                    const QSize& fullscreen = QSize(), const QSize& panel = QSize(), int refreshMillihz = 60000)
{
    NvClientDisplay display {points, panel.isValid() ? panel : backing, backing, fullscreen};
    display.key = key;
    display.name = key.mid(5);
    display.main = main;
    display.notch = fullscreen.isValid();
    display.builtIn = fullscreen.isValid();
    display.refreshMillihz = refreshMillihz;
    return display;
}

// 14" MacBook Pro at the default "looks like 1512 x 982": 3024x1890 below the notch.
NvClientDisplay macBook14(bool main = true)
{
    return mac(QStringLiteral("uuid:MBP14"), QRect(0, 0, 1512, 982), QSize(3024, 1964), main, QSize(3024, 1890));
}

QJsonObject vectors()
{
    QFile file(QString::fromUtf8(qgetenv("PLANK_REPO_ROOT")) + QStringLiteral("/tests/protocol/display-arrangement-v1.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

DisplayArrangement::Capabilities vectorCapabilities(const QString& name)
{
    DisplayArrangement::Capabilities caps;
    DisplayArrangement::Capabilities::fromJson(vectors().value(QStringLiteral("capabilities")).toObject()
                                               .value(name).toObject(), caps);
    return caps;
}

DisplayPlanner::HostInfo arrangementHost(const DisplayArrangement::Capabilities& caps,
                                         const QString& encodingMode = QStringLiteral("hevc-10-444-nvenc"))
{
    DisplayPlanner::HostInfo host;
    host.known = true;
    host.platform = 1;
    host.featureFlags = NvOutputTopology::NotchSafeLaptopModesFeature | DisplayArrangement::Feature;
    host.capabilities = caps;
    host.encodingMode = encodingMode;
    return host;
}

QStringList warningCodes(const DisplayPlanner::Plan& plan)
{
    QStringList codes;
    for (const auto& warning : plan.warnings) codes.append(warning.code);
    return codes;
}

}

class TestDisplayPlanner : public QObject
{
    Q_OBJECT

private slots:
    void monitorKeysPreferTheUuid();
    void fingerprintIgnoresSizeAndPosition();
    void duplicateMonitorsGetDistinctKeys();
    void labelsTheSetLeftToRight();
    void macMatchUsesTheFullscreenViewport();

    // Shared vectors (tests/protocol/display-arrangement-v1.json)
    void arrangementGrammarVectors();
    void arrangementResolveVectors();
    void capabilitiesParseStrictly();
    void fleetDefaultCapabilities();
    void carrierRule();

    // Planner
    void exactAndLooksLikeSizes();
    void proposesEveryMonitor();
    void macBookAlone();
    void macBookBesideUhd();
    void ultrawideAndStudioDisplayScaleToTheCanvas();
    void eightKMonitor();
    void verticalStack();
    void lShape();
    void seamsKeepSixtyFourPixels();
    void keepsPrimaryAndNeighboursAboveTheBudget();
    void turnedOffMonitorsLeaveNoGap();
    void honoursProfileChoices();
    void manualPlacement();
    void legacyHostFallback();
    void macHostMatchesTheDesktop();
    void warnsWithActions();
    void backingPreviewUsesTheHost();
    void arrangeInvariants();
};

void TestDisplayPlanner::monitorKeysPreferTheUuid()
{
    QCOMPARE(ClientDisplayProbe::monitorKey(QStringLiteral(" 1e2d3c4b-0000-4000-8000-00000000abcd "), 0x610, 0xa050, 7,
                                            true, QSize(3024, 1964)),
             QStringLiteral("uuid:1E2D3C4B-0000-4000-8000-00000000ABCD"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0x1e6d, 0x5b11, 0x1234, false, QSize(3840, 2160)),
             QStringLiteral("edid:1e6d-5b11-1234"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0, 0, 0, true, QSize(3024, 1964)),
             QStringLiteral("builtin:3024x1964"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0, 0, 0, false, QSize(1920, 1080)),
             QStringLiteral("panel:1920x1080"));
}

void TestDisplayPlanner::fingerprintIgnoresSizeAndPosition()
{
    const NvClientDisplay laptop = monitor(QStringLiteral("uuid:A"), QRect(0, 0, 1512, 982), QSize(3024, 1964));
    const NvClientDisplay external = monitor(QStringLiteral("uuid:B"), QRect(1512, 0, 2560, 1440), QSize(5120, 2880));
    const QString docked = ClientDisplayProbe::fingerprint({laptop, external});
    QCOMPARE(docked.size(), 20);
    // The same monitors placed and scaled differently, in any order.
    NvClientDisplay movedLaptop = laptop;
    movedLaptop.bounds = QRect(2560, 400, 1800, 1169);
    movedLaptop.backingSize = QSize(3600, 2338);
    NvClientDisplay movedExternal = external;
    movedExternal.bounds = QRect(0, 0, 3840, 2160);
    QCOMPARE(ClientDisplayProbe::fingerprint({movedExternal, movedLaptop}), docked);
    // A different set is a different fingerprint; no monitors, none at all.
    QVERIFY(ClientDisplayProbe::fingerprint({laptop}) != docked);
    QVERIFY(ClientDisplayProbe::fingerprint({}).isEmpty());
}

void TestDisplayPlanner::duplicateMonitorsGetDistinctKeys()
{
    QVector<NvClientDisplay> displays {
        monitor(QStringLiteral("edid:10ac-a0c4-0"), QRect(0, 0, 1920, 1080), QSize(1920, 1080)),
        monitor(QStringLiteral("edid:10ac-a0c4-0"), QRect(1920, 0, 1920, 1080), QSize(1920, 1080)),
        monitor(QString(), QRect(3840, 0, 1920, 1080), QSize(1920, 1080)),
    };
    ClientDisplayProbe::assignUniqueKeys(displays);
    QCOMPARE(displays.at(0).key, QStringLiteral("edid:10ac-a0c4-0"));
    QCOMPARE(displays.at(1).key, QStringLiteral("edid:10ac-a0c4-0#2"));
    QCOMPARE(displays.at(2).key, QStringLiteral("panel:1920x1080"));
    // Stable: assigning again changes nothing.
    const QVector<NvClientDisplay> again = displays;
    ClientDisplayProbe::assignUniqueKeys(displays);
    for (int index = 0; index < displays.size(); ++index) QCOMPARE(displays.at(index).key, again.at(index).key);
}

void TestDisplayPlanner::labelsTheSetLeftToRight()
{
    const NvClientDisplay laptop = monitor(QStringLiteral("uuid:A"), QRect(0, 0, 1512, 982), QSize(3024, 1964),
                                           QStringLiteral("Built-in Retina Display"), true);
    const NvClientDisplay external = monitor(QStringLiteral("uuid:B"), QRect(-2560, 0, 2560, 1440),
                                             QSize(5120, 2880), QStringLiteral("LG UltraFine"));
    QCOMPARE(ClientDisplayProbe::label({laptop, external}),
             QStringLiteral("LG UltraFine + Built-in Retina Display"));
    const NvClientDisplay unnamed = monitor(QStringLiteral("uuid:C"), QRect(1512, 0, 1920, 1080), QSize(1920, 1080));
    QCOMPARE(ClientDisplayProbe::label({laptop, unnamed}), QStringLiteral("Built-in Retina Display + Display"));
}

void TestDisplayPlanner::macMatchUsesTheFullscreenViewport()
{
    // 14" MacBook Pro at the default 1512x982 pt: 38 pt camera housing.
    NvClientDisplay notched {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1888)};
    const NvClientDisplay external {QRect(1512, -200, 1920, 1080), QSize(1920, 1080), QSize(1920, 1080)};
    const QVector<NvClientDisplay> windowed = ClientDisplayProbe::macMatchDisplays({notched, external}, false);
    QCOMPARE(windowed.at(0).bounds, notched.bounds);
    QCOMPARE(windowed.at(0).backingSize, QSize(3024, 1964));
    const QVector<NvClientDisplay> fullscreen = ClientDisplayProbe::macMatchDisplays({notched, external}, true);
    QCOMPARE(fullscreen.at(0).bounds, QRect(0, 38, 1512, 944));
    QCOMPARE(fullscreen.at(0).backingSize, QSize(3024, 1888));
    QCOMPARE(fullscreen.at(0).nativeSize, QSize(3024, 1888));
    QCOMPARE(fullscreen.at(1).bounds, external.bounds);
    QCOMPARE(fullscreen.at(1).backingSize, QSize(1920, 1080));
    int scale = 0;
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(fullscreen, nullptr, &scale), QString());
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({fullscreen.at(0)}, nullptr, &scale),
             QStringLiteral("3024x1888"));
    QCOMPARE(scale, 2);
}


void TestDisplayPlanner::arrangementGrammarVectors()
{
    const QJsonObject root = vectors();
    QVERIFY2(!root.isEmpty(), "PLANK_REPO_ROOT must identify the repository root");
    QCOMPARE(root.value(QStringLiteral("feature")).toInt(), DisplayArrangement::Feature);
    QCOMPARE(QJsonValue(root.value(QStringLiteral("virtual_pool"))).toArray().toVariantList(),
             QJsonArray::fromStringList(DisplayArrangement::virtualPool()).toVariantList());
    QCOMPARE(DisplayArrangement::virtualPool(), NvOutputTopology::qualifiedVirtualModes());
    const DisplayArrangement::Capabilities caps = vectorCapabilities(QStringLiteral("fleet-hybrid"));
    QVERIFY(caps.valid);
    const QJsonArray cases = root.value(QStringLiteral("grammar")).toArray();
    QVERIFY(cases.size() >= 20);
    for (const QJsonValue& value : cases) {
        const QJsonObject entry = value.toObject();
        const QString request = entry.value(QStringLiteral("request")).toString();
        QVector<DisplayArrangement::Entry> entries;
        const QString error = DisplayArrangement::validate(request, caps, &entries);
        if (entry.value(QStringLiteral("valid")).toBool()) {
            QVERIFY2(error.isEmpty(), qPrintable(request + QStringLiteral(" -> ") + error));
            QCOMPARE(DisplayArrangement::serialize(entries), request);
        } else {
            QCOMPARE(error, entry.value(QStringLiteral("error")).toString());
            QVERIFY(entries.isEmpty());
        }
    }
}

void TestDisplayPlanner::arrangementResolveVectors()
{
    const QJsonObject root = vectors();
    const QJsonArray cases = root.value(QStringLiteral("resolve")).toArray();
    QVERIFY(cases.size() >= 15);
    for (const QJsonValue& value : cases) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const DisplayArrangement::Capabilities caps =
                vectorCapabilities(entry.value(QStringLiteral("capabilities")).toString());
        QVERIFY2(caps.valid, qPrintable(name));
        const DisplayArrangement::Resolution resolution =
                DisplayArrangement::resolve(entry.value(QStringLiteral("request")).toString(), caps);
        if (entry.contains(QStringLiteral("error"))) {
            QVERIFY2(!resolution.ok, qPrintable(name));
            QCOMPARE(resolution.error, entry.value(QStringLiteral("error")).toString());
            continue;
        }
        QVERIFY2(resolution.ok, qPrintable(name + QStringLiteral(": ") + resolution.error));
        const QJsonObject expected = entry.value(QStringLiteral("result")).toObject();
        const QJsonArray outputs = expected.value(QStringLiteral("outputs")).toArray();
        QCOMPARE(resolution.outputs.size(), outputs.size());
        for (int index = 0; index < outputs.size(); ++index) {
            const QJsonObject want = outputs.at(index).toObject();
            const DisplayArrangement::ResolvedOutput& got = resolution.outputs.at(index);
            QCOMPARE(DisplayArrangement::backingName(got.backing), want.value(QStringLiteral("backing")).toString());
            const QJsonObject rect = want.value(QStringLiteral("rect")).toObject();
            QCOMPARE(got.rect, QRect(rect.value(QStringLiteral("x")).toInt(), rect.value(QStringLiteral("y")).toInt(),
                                     rect.value(QStringLiteral("width")).toInt(),
                                     rect.value(QStringLiteral("height")).toInt()));
            if (got.backing == DisplayArrangement::Backing::Virtual) {
                QCOMPARE(got.head, want.value(QStringLiteral("head")).toInt());
                QCOMPARE(got.carrier, DisplayArrangement::parseSize(want.value(QStringLiteral("carrier")).toString()));
            } else {
                QCOMPARE(got.output, want.value(QStringLiteral("output")).toString());
                QCOMPARE(got.mode, DisplayArrangement::parseSize(want.value(QStringLiteral("mode")).toString()));
            }
        }
        QStringList hidden;
        for (const QJsonValue& id : expected.value(QStringLiteral("hidden_physical")).toArray()) hidden.append(id.toString());
        QCOMPARE(resolution.hiddenPhysical, hidden);
        const QJsonObject desktop = expected.value(QStringLiteral("desktop")).toObject();
        QCOMPARE(resolution.desktop, QSize(desktop.value(QStringLiteral("width")).toInt(),
                                           desktop.value(QStringLiteral("height")).toInt()));
    }
}

void TestDisplayPlanner::capabilitiesParseStrictly()
{
    const QJsonObject fleet = vectors().value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("fleet-hybrid")).toObject();
    DisplayArrangement::Capabilities caps;
    QVERIFY(DisplayArrangement::Capabilities::fromJson(fleet, caps));
    QCOMPARE(caps.toJson(), fleet);
    QCOMPARE(caps.maxOutputs, 4);
    QCOMPARE(caps.virtualHeads, 3);
    QCOMPARE(caps.maxCanvas, QSize(8192, 8192));
    QCOMPARE(caps.physicalOutputs.size(), 1);
    QCOMPARE(caps.physicalOutputs.first().preferred, QSize(3840, 2160));
    QVERIFY(caps.physicalOutputs.first().modes.contains(QSize(4096, 2160)));
    QCOMPARE(caps.encodingLimits.value(QStringLiteral("h264-8-444-nvenc")).maximum, QSize(4096, 4096));
    DisplayArrangement::Capabilities copy;
    QVERIFY(DisplayArrangement::Capabilities::fromJson(caps.toJson(), copy));
    QVERIFY(copy == caps);

    const auto rejects = [&fleet](const std::function<void(QJsonObject&)>& edit) {
        QJsonObject broken = fleet;
        edit(broken);
        DisplayArrangement::Capabilities out;
        return !DisplayArrangement::Capabilities::fromJson(broken, out) && !out.valid;
    };
    QVERIFY(rejects([](QJsonObject& o) { o.remove(QStringLiteral("encoding_limits")); }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("version")] = 2; }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("max_outputs")] = 5; }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("virtual_heads")] = 0; })); // 4 > 1 + 0
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("max_outputs")] = 1.5; }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("fingerprint")] = QString(); }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("packed_capture")] = 0; }));
    QVERIFY(rejects([](QJsonObject& o) { o[QStringLiteral("refresh_millihz")] = QJsonArray(); }));
    QVERIFY(rejects([](QJsonObject& o) {
        QJsonObject limits = o.value(QStringLiteral("output_limits")).toObject();
        limits[QStringLiteral("min_width")] = 9000;
        o[QStringLiteral("output_limits")] = limits;
    }));
    QVERIFY(rejects([](QJsonObject& o) {
        QJsonArray outputs = o.value(QStringLiteral("physical_outputs")).toArray();
        QJsonObject output = outputs.first().toObject();
        output[QStringLiteral("edid_sha256")] = QStringLiteral("0D7DA54A");
        outputs[0] = output;
        o[QStringLiteral("physical_outputs")] = outputs;
    }));
    QVERIFY(rejects([](QJsonObject& o) {
        QJsonArray outputs = o.value(QStringLiteral("physical_outputs")).toArray();
        QJsonObject output = outputs.first().toObject();
        output[QStringLiteral("modes")] = QJsonArray {QStringLiteral("3840x2160"), QStringLiteral("03840x2160")};
        outputs[0] = output;
        o[QStringLiteral("physical_outputs")] = outputs;
    }));
    QVERIFY(rejects([](QJsonObject& o) {
        QJsonArray outputs = o.value(QStringLiteral("physical_outputs")).toArray();
        outputs.append(outputs.first());
        o[QStringLiteral("physical_outputs")] = outputs;
    }));
    QVERIFY(rejects([](QJsonObject& o) {
        QJsonObject limits = o.value(QStringLiteral("encoding_limits")).toObject();
        QJsonObject hevc = limits.value(QStringLiteral("hevc-10-444-nvenc")).toObject();
        hevc[QStringLiteral("qualified_width")] = 9000;
        limits[QStringLiteral("hevc-10-444-nvenc")] = hevc;
        o[QStringLiteral("encoding_limits")] = limits;
    }));
}

void TestDisplayPlanner::fleetDefaultCapabilities()
{
    const DisplayArrangement::Capabilities fleet = DisplayArrangement::Capabilities::fleetDefault();
    QVERIFY(fleet.valid);
    DisplayArrangement::Capabilities parsed;
    QVERIFY(DisplayArrangement::Capabilities::fromJson(fleet.toJson(), parsed));
    // The vectors' fleet-hybrid host, with the canvas the X screen allows
    // until the host packs its capture.
    const DisplayArrangement::Capabilities vector = vectorCapabilities(QStringLiteral("fleet-hybrid"));
    QCOMPARE(fleet.maxCanvas, QSize(8192, 4608));
    QCOMPARE(fleet.maxOutputs, vector.maxOutputs);
    QCOMPARE(fleet.virtualHeads, vector.virtualHeads);
    QCOMPARE(fleet.maxOutput, vector.maxOutput);
    QCOMPARE(fleet.maxPixels, vector.maxPixels);
    QCOMPARE(fleet.physicalOutputs.first().id, vector.physicalOutputs.first().id);
    QCOMPARE(fleet.physicalOutputs.first().preferred, vector.physicalOutputs.first().preferred);
    for (const QSize& mode : fleet.physicalOutputs.first().modes) {
        QVERIFY(vector.physicalOutputs.first().modes.contains(mode));
    }
    QVERIFY(!fleet.physicalOutputs.first().modes.contains(QSize(800, 600)));
}

void TestDisplayPlanner::carrierRule()
{
    QCOMPARE(DisplayArrangement::carrierFor(QSize(3024, 1890)), QSize(3024, 1890));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(3456, 2160)), QSize(3840, 2160));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(5120, 1440)), QSize(5120, 2160));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(5120, 2880)), QSize(5120, 2160));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(6016, 3384)), QSize(5120, 2160));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(2160, 3840)), QSize(5120, 2160));
    QCOMPARE(DisplayArrangement::carrierFor(QSize(1512, 944)), QSize(1920, 1080));
}

void TestDisplayPlanner::exactAndLooksLikeSizes()
{
    const NvClientDisplay laptop = macBook14();
    QCOMPARE(DisplayPlanner::exactSize(laptop), QSize(3024, 1890));
    QCOMPARE(DisplayPlanner::looksLikeSize(laptop), QSize(1512, 944));
    // "More Space" on the same panel: the desktop is capped at the panel.
    const NvClientDisplay moreSpace = mac(QStringLiteral("uuid:MBP14"), QRect(0, 0, 1800, 1169), QSize(3600, 2338),
                                          true, QSize(), QSize(3024, 1964));
    QCOMPARE(DisplayPlanner::exactSize(moreSpace), QSize(3024, 1964));
    const NvClientDisplay studio = mac(QStringLiteral("uuid:5K"), QRect(0, 0, 2560, 1440), QSize(5120, 2880));
    QCOMPARE(DisplayPlanner::exactSize(studio), QSize(5120, 2880));
    QCOMPARE(DisplayPlanner::looksLikeSize(studio), QSize(2560, 1440));
    // 1x monitors have no "looks like".
    const NvClientDisplay plain = mac(QStringLiteral("uuid:1080"), QRect(0, 0, 1920, 1080), QSize(1920, 1080));
    QCOMPARE(DisplayPlanner::exactSize(plain), QSize(1920, 1080));
    QVERIFY(!DisplayPlanner::looksLikeSize(plain).isValid());
    // Odd pixel counts become even.
    const NvClientDisplay odd = mac(QStringLiteral("uuid:odd"), QRect(0, 0, 1365, 767), QSize(1365, 767));
    QCOMPARE(DisplayPlanner::exactSize(odd), QSize(1364, 766));
}

void TestDisplayPlanner::proposesEveryMonitor()
{
    NvClientDisplay laptop = macBook14(false);
    const NvClientDisplay external = mac(QStringLiteral("uuid:UHD"), QRect(1512, 0, 1920, 1080), QSize(3840, 2160), true);
    const DisplayProfile::Profile profile = DisplayPlanner::proposal({laptop, external});
    QCOMPARE(profile.monitors.size(), 2);
    for (const auto& monitor : profile.monitors) {
        QVERIFY(monitor.on);
        QCOMPARE(monitor.size, DisplayProfile::SizeMode::Exact);
        QCOMPARE(monitor.backing, QStringLiteral("auto"));
    }
    // The menu-bar display is primary, even with the lid open beside it.
    QCOMPARE(profile.primary, QStringLiteral("uuid:UHD"));
    QCOMPARE(profile.presentation, QStringLiteral("windows"));
    QVERIFY(!profile.manual);
}

void TestDisplayPlanner::macBookAlone()
{
    const QVector<NvClientDisplay> displays {macBook14()};
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:3024x1890+0+0:auto"));
    QVERIFY(plan.backingExpected);
    QVERIFY(!plan.legacy);
    QCOMPARE(plan.canvas, QSize(3024, 1890));
    const DisplayPlanner::Output& output = plan.outputs.first();
    QVERIFY(output.included);
    QVERIFY(output.primary);
    QCOMPARE(output.arrangementIndex, 0);
    QCOMPARE(output.badge, QStringLiteral("exact"));
    // The dongle cannot drive it: a virtual display of exactly that size.
    QCOMPARE(output.backing, DisplayArrangement::Backing::Virtual);
    QCOMPARE(plan.presentation, QStringLiteral("single"));
    QVERIFY(plan.warnings.isEmpty());
    // The picker: exact, looks like, the dongle's modes, custom.
    QCOMPARE(output.sizeOptions.first().value, QStringLiteral("exact"));
    QCOMPARE(output.sizeOptions.at(1).value, QStringLiteral("looks-like"));
    QCOMPARE(output.sizeOptions.at(1).size, QSize(1512, 944));
    QCOMPARE(output.sizeOptions.at(2).value, QStringLiteral("preset:4096x2160"));
    QCOMPARE(output.sizeOptions.last().value, QStringLiteral("custom"));
}

void TestDisplayPlanner::macBookBesideUhd()
{
    // A UHD monitor at "looks like 1920 x 1080", 300 pt above the laptop's top edge.
    const QVector<NvClientDisplay> displays {
        macBook14(),
        mac(QStringLiteral("uuid:UHD"), QRect(1512, -300, 1920, 1080), QSize(3840, 2160)),
    };
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    // 300 pt at the laptop's 1890/982 px per pt is 577 px, made even.
    QCOMPARE(plan.arrangement, QStringLiteral("1:3024x1890+0+576:auto,3840x2160+3024+0:auto"));
    QCOMPARE(plan.canvas, QSize(6864, 2466));
    QCOMPARE(plan.outputs.at(0).backing, DisplayArrangement::Backing::Virtual);
    QCOMPARE(plan.outputs.at(1).backing, DisplayArrangement::Backing::Physical);
    QCOMPARE(plan.outputs.at(1).backingOutput, QStringLiteral("x11:HDMI-0"));
    QCOMPARE(plan.presentation, QStringLiteral("windows"));
    // The UHD monitor is pixel-exact at 2x, the laptop too: no mixed-DPI warning.
    QVERIFY(!warningCodes(plan).contains(QStringLiteral("mixed-dpi")));
    // The same desk on a host with only the dongle (no virtual heads): the
    // laptop goes through the dongle's viewport, the UHD monitor has no output.
    DisplayArrangement::Capabilities physicalOnly = DisplayArrangement::Capabilities::fleetDefault();
    physicalOnly.virtualHeads = 0;
    physicalOnly.maxOutputs = 1;
    const DisplayPlanner::Plan one = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                          arrangementHost(physicalOnly));
    QVERIFY2(one.ok, qPrintable(one.error));
    QCOMPARE(one.includedCount(), 1);
    QCOMPARE(one.arrangement, QStringLiteral("1:3024x1890+0+0:auto"));
    QCOMPARE(one.outputs.at(0).backing, DisplayArrangement::Backing::PhysicalViewport);
    QVERIFY(!one.backingExpected);
    QVERIFY(warningCodes(one).contains(QStringLiteral("too-many")));
}

void TestDisplayPlanner::ultrawideAndStudioDisplayScaleToTheCanvas()
{
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:UW"), QRect(0, 0, 3440, 1440), QSize(3440, 1440), true),
        mac(QStringLiteral("uuid:5K"), QRect(3440, 0, 2560, 1440), QSize(5120, 2880)),
    };
    // 3440 + 5120 = 8560 pixels: wider than the 8192 canvas, so the 5K display
    // steps down to "looks like" and says so.
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:3440x1440+0+0:auto,2560x1440+3440+0:auto"));
    QVERIFY(!plan.outputs.at(0).scaled);
    QVERIFY(plan.outputs.at(1).scaled);
    QCOMPARE(plan.outputs.at(1).badge, QStringLiteral("scaled"));
    QVERIFY(warningCodes(plan).contains(QStringLiteral("canvas")));
    QCOMPARE(plan.outputs.at(0).backing, DisplayArrangement::Backing::Virtual);
    QCOMPARE(plan.outputs.at(1).backing, DisplayArrangement::Backing::Virtual);
    // With a canvas that holds both (packed capture later), both are exact.
    DisplayArrangement::Capabilities wide = DisplayArrangement::Capabilities::fleetDefault();
    wide.maxCanvas = QSize(16384, 4608);
    wide.packedCapture = true;
    const DisplayPlanner::Plan exact = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                            arrangementHost(wide));
    QVERIFY2(exact.ok, qPrintable(exact.error));
    QCOMPARE(exact.arrangement, QStringLiteral("1:3440x1440+0+0:auto,5120x2880+3440+0:auto"));
    QVERIFY(!exact.outputs.at(1).scaled);
    QCOMPARE(exact.outputs.at(1).badge, QStringLiteral("exact"));
}

void TestDisplayPlanner::eightKMonitor()
{
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:8K"), QRect(0, 0, 3840, 2160), QSize(7680, 4320), true)};
    DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                     DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:7680x4320+0+0:auto"));
    QCOMPARE(plan.outputs.first().badge, QStringLiteral("exact"));
    QVERIFY(warningCodes(plan).contains(QStringLiteral("bitrate")));
    QVERIFY(!warningCodes(plan).contains(QStringLiteral("unqualified")));
    // A client that decodes at most 4096x2304: "looks like" and a warning.
    DisplayPlanner::Limits limits;
    limits.decoderMaximum = QSize(4096, 2304);
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), DisplayPlanner::HostInfo(), limits);
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:3840x2160+0+0:auto"));
    QVERIFY(plan.outputs.first().scaled);
    QVERIFY(warningCodes(plan).contains(QStringLiteral("decoder")));
    // H.264 streams at most 4096 wide.
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                arrangementHost(DisplayArrangement::Capabilities::fleetDefault(),
                                                QStringLiteral("h264-8-444-nvenc")));
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:3840x2160+0+0:auto"));
    const int codec = warningCodes(plan).indexOf(QStringLiteral("codec"));
    QVERIFY(codec >= 0);
    QCOMPARE(plan.warnings.at(codec).action, QStringLiteral("use-hevc"));
    // A 6K Pro Display XDR at 2x is exact too, above what was qualified.
    const QVector<NvClientDisplay> xdr {
        mac(QStringLiteral("uuid:6K"), QRect(0, 0, 3008, 1692), QSize(6016, 3384), true)};
    plan = DisplayPlanner::plan(xdr, DisplayPlanner::proposal(xdr),
                                arrangementHost(DisplayArrangement::Capabilities::fleetDefault()));
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:6016x3384+0+0:auto"));
    QVERIFY(!warningCodes(plan).contains(QStringLiteral("unqualified")));
}

void TestDisplayPlanner::verticalStack()
{
    // A UHD monitor above the laptop, left edges aligned.
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:UHD"), QRect(0, -1080, 1920, 1080), QSize(3840, 2160)),
        macBook14(),
    };
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:3024x1890+0+2160:auto,3840x2160+0+0:auto"));
    QCOMPARE(plan.canvas, QSize(3840, 4050));
    QCOMPARE(plan.outputs.at(1).arrangementIndex, 0);
    QCOMPARE(plan.outputs.at(0).arrangementIndex, 1);
}

void TestDisplayPlanner::lShape()
{
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:A"), QRect(0, 0, 1920, 1080), QSize(1920, 1080), true),
        mac(QStringLiteral("uuid:B"), QRect(1920, 0, 1920, 1080), QSize(1920, 1080)),
        mac(QStringLiteral("uuid:C"), QRect(0, 1080, 1920, 1080), QSize(1920, 1080)),
    };
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement,
             QStringLiteral("1:1920x1080+0+0:auto,1920x1080+0+1080:auto,1920x1080+1920+0:auto"));
    QCOMPARE(plan.canvas, QSize(3840, 2160));
    QCOMPARE(plan.outputs.at(0).backing, DisplayArrangement::Backing::Physical);
    QCOMPARE(plan.outputs.at(2).backing, DisplayArrangement::Backing::Virtual);
    QCOMPARE(plan.outputs.at(1).backing, DisplayArrangement::Backing::Virtual);
}

void TestDisplayPlanner::seamsKeepSixtyFourPixels()
{
    // Only 10 pt of the right monitor's edge touches the left one.
    const QVector<QRect> logical {QRect(0, 0, 1920, 1080), QRect(1920, 1070, 1920, 1080)};
    const QVector<QPoint> positions = DisplayPlanner::arrange(logical, {QSize(1920, 1080), QSize(1920, 1080)}, 0);
    QCOMPARE(positions.at(0), QPoint(0, 0));
    QCOMPARE(positions.at(1), QPoint(1920, 1080 - DisplayPlanner::MinimumSeam));
    // Stacked with 20 pt shared: the same on the other axis.
    const QVector<QPoint> stacked = DisplayPlanner::arrange({QRect(0, 0, 1920, 1080), QRect(-1900, 1080, 1920, 1080)},
                                                            {QSize(3840, 2160), QSize(1920, 1080)}, 0);
    QCOMPARE(stacked.at(1).y() - stacked.at(0).y(), 2160);
    QCOMPARE(stacked.at(1).x() + 1920 - stacked.at(0).x(), DisplayPlanner::MinimumSeam);
}

void TestDisplayPlanner::keepsPrimaryAndNeighboursAboveTheBudget()
{
    // Five 1080p monitors in a row, the middle one primary: the host shows four.
    QVector<NvClientDisplay> displays;
    for (int index = 0; index < 5; ++index) {
        displays.append(mac(QStringLiteral("uuid:M%1").arg(index), QRect((index - 2) * 1920, 0, 1920, 1080),
                            QSize(1920, 1080), index == 2));
    }
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                           DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.includedCount(), 4);
    QVERIFY(plan.outputs.at(2).primary);
    // Breadth first from the primary: both direct neighbours, then the next on the left.
    QVERIFY(plan.outputs.at(0).included);
    QVERIFY(plan.outputs.at(1).included);
    QVERIFY(plan.outputs.at(3).included);
    QVERIFY(!plan.outputs.at(4).included);
    QVERIFY(plan.outputs.at(4).on);
    const int warning = warningCodes(plan).indexOf(QStringLiteral("too-many"));
    QVERIFY(warning >= 0);
    QCOMPARE(plan.warnings.at(warning).action, QStringLiteral("choose-screens"));
    QCOMPARE(plan.arrangement, QStringLiteral("1:1920x1080+3840+0:auto,1920x1080+0+0:auto,"
                                              "1920x1080+1920+0:auto,1920x1080+5760+0:auto"));
}

void TestDisplayPlanner::turnedOffMonitorsLeaveNoGap()
{
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:A"), QRect(0, 0, 1920, 1080), QSize(1920, 1080), true),
        mac(QStringLiteral("uuid:B"), QRect(1920, 0, 1920, 1080), QSize(1920, 1080)),
        mac(QStringLiteral("uuid:C"), QRect(3840, 0, 2560, 1440), QSize(2560, 1440)),
    };
    DisplayProfile::Profile profile = DisplayPlanner::proposal(displays);
    profile.choice(QStringLiteral("uuid:B")).on = false;
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QVERIFY(!plan.outputs.at(1).included);
    QCOMPARE(plan.outputs.at(1).arrangementIndex, -1);
    QCOMPARE(plan.arrangement, QStringLiteral("1:1920x1080+0+0:auto,2560x1440+1920+0:auto"));
    QVERIFY(!warningCodes(plan).contains(QStringLiteral("too-many")));
    // Everything off: nothing to plan.
    for (const char* key : {"uuid:A", "uuid:C"}) profile.choice(QString::fromLatin1(key)).on = false;
    const DisplayPlanner::Plan none = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY(!none.ok);
    QVERIFY(!none.error.isEmpty());
}

void TestDisplayPlanner::honoursProfileChoices()
{
    const QVector<NvClientDisplay> displays {
        macBook14(),
        mac(QStringLiteral("uuid:UHD"), QRect(1512, 0, 1920, 1080), QSize(3840, 2160)),
    };
    DisplayProfile::Profile profile = DisplayPlanner::proposal(displays);
    profile.primary = QStringLiteral("uuid:UHD");
    profile.choice(QStringLiteral("uuid:MBP14")).size = DisplayProfile::SizeMode::LooksLike;
    QVERIFY(DisplayProfile::sizeFromText(QStringLiteral("preset:1920x1080"), profile.choice(QStringLiteral("uuid:UHD"))));
    profile.choice(QStringLiteral("uuid:UHD")).backing = QStringLiteral("virtual");
    profile.presentation = QStringLiteral("single");
    profile.scaling = QStringLiteral("fit");
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:1920x1080+1512+0:virtual,1512x944+0+0:auto"));
    QVERIFY(plan.outputs.at(1).primary);
    QCOMPARE(plan.outputs.at(0).badge, QStringLiteral("looks-like"));
    QCOMPARE(plan.outputs.at(1).badge, QStringLiteral("preset"));
    QCOMPARE(plan.outputs.at(1).backing, DisplayArrangement::Backing::Virtual);
    // 1512x944 is none of the dongle's modes: the laptop gets the next virtual head.
    QCOMPARE(plan.outputs.at(0).backing, DisplayArrangement::Backing::Virtual);
    QCOMPARE(plan.presentation, QStringLiteral("single"));
    QCOMPARE(plan.scaling, QStringLiteral("fit"));
    // A preference no output can satisfy is refused with a reason.
    profile.choice(QStringLiteral("uuid:MBP14")).backing = QStringLiteral("physical");
    profile.choice(QStringLiteral("uuid:UHD")).backing = QStringLiteral("physical");
    const DisplayPlanner::Plan refused = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY(!refused.ok);
    QVERIFY(refused.error.contains(QStringLiteral("outputs")));
}

void TestDisplayPlanner::manualPlacement()
{
    const QVector<NvClientDisplay> displays {
        mac(QStringLiteral("uuid:A"), QRect(0, 0, 1920, 1080), QSize(1920, 1080), true),
        mac(QStringLiteral("uuid:B"), QRect(1920, 0, 1920, 1080), QSize(1920, 1080)),
    };
    DisplayProfile::Profile profile = DisplayPlanner::proposal(displays);
    profile.manual = true;
    auto& a = profile.choice(QStringLiteral("uuid:A"));
    a.hasPosition = true;
    a.position = QPoint(100, 500);
    auto& b = profile.choice(QStringLiteral("uuid:B"));
    b.hasPosition = true;
    b.position = QPoint(2021, 0); // odd: made even
    QVERIFY(DisplayProfile::sizeFromText(QStringLiteral("custom:5120x1440"), b));
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.arrangement, QStringLiteral("1:1920x1080+0+500:auto,5120x1440+1920+0:auto"));
    QCOMPARE(plan.outputs.at(1).badge, QStringLiteral("custom"));
    // Overlapping hand placement falls back to the desk's arrangement.
    b.position = QPoint(1000, 500);
    const DisplayPlanner::Plan fallback = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY2(fallback.ok, qPrintable(fallback.error));
    QCOMPARE(fallback.arrangement, QStringLiteral("1:1920x1080+0+0:auto,5120x1440+1920+0:auto"));
    QVERIFY(warningCodes(fallback).contains(QStringLiteral("arrangement")));
}

void TestDisplayPlanner::legacyHostFallback()
{
    DisplayPlanner::HostInfo old;
    old.known = true;
    old.platform = 1;
    old.featureFlags = NvOutputTopology::NotchSafeLaptopModesFeature;
    QVector<NvClientDisplay> displays {macBook14()};
    DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), old);
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QVERIFY(plan.legacy);
    QVERIFY(plan.arrangement.isEmpty());
    QCOMPARE(plan.legacyHostLayout, QStringLiteral("single"));
    QCOMPARE(plan.legacyModes, QStringList({QStringLiteral("3024x1890")}));
    QVERIFY(!plan.legacyFitted);
    QVERIFY(plan.warnings.isEmpty());
    // Without the notch-safe modes: the closest qualified mode, and a hint to update.
    old.featureFlags = 0;
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), old);
    QCOMPARE(plan.legacyModes, QStringList({QStringLiteral("2560x1600")}));
    QVERIFY(plan.legacyFitted);
    QCOMPARE(plan.outputs.first().badge, QStringLiteral("closest"));
    QVERIFY(warningCodes(plan).contains(QStringLiteral("old-host")));
    // Three monitors: the primary and its right-hand neighbour, as today's
    // Match client would with those two.
    old.featureFlags = NvOutputTopology::NotchSafeLaptopModesFeature;
    displays = {mac(QStringLiteral("uuid:L"), QRect(-1920, 0, 1920, 1080), QSize(1920, 1080)),
                macBook14(),
                mac(QStringLiteral("uuid:R"), QRect(1512, 0, 2560, 1440), QSize(2560, 1440))};
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), old);
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.legacyHostLayout, QStringLiteral("dual-horizontal"));
    QCOMPARE(plan.legacyModes, QStringList({QStringLiteral("3024x1890"), QStringLiteral("2560x1440")}));
    QVERIFY(!plan.outputs.at(0).included);
    QVERIFY(warningCodes(plan).contains(QStringLiteral("old-host")));
    QString layout;
    QStringList modes;
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({displays.at(1), displays.at(2)}, layout, modes, nullptr,
                                                         nullptr, NvOutputTopology::virtualModesForHost(old.featureFlags)));
    QCOMPARE(layout, plan.legacyHostLayout);
    QCOMPARE(modes, plan.legacyModes);
    // Stacked monitors: the primary alone.
    displays = {mac(QStringLiteral("uuid:UHD"), QRect(0, -1080, 1920, 1080), QSize(3840, 2160)), macBook14()};
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), old);
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.legacyHostLayout, QStringLiteral("single"));
}

void TestDisplayPlanner::macHostMatchesTheDesktop()
{
    DisplayPlanner::HostInfo macHost;
    macHost.known = true;
    macHost.platform = 2;
    macHost.featureFlags = NvOutputTopology::FixedCaptureFlags;
    const QVector<NvClientDisplay> displays {macBook14()};
    const DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), macHost);
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.macMode, QStringLiteral("3024x1890"));
    QCOMPARE(plan.macScale, 2);
    QVERIFY(plan.arrangement.isEmpty());
    QVERIFY(!plan.legacy);
    QCOMPARE(plan.presentation, QStringLiteral("single"));
}

void TestDisplayPlanner::warnsWithActions()
{
    // A 2x laptop beside a 1x monitor.
    QVector<NvClientDisplay> displays {
        macBook14(),
        mac(QStringLiteral("uuid:1080"), QRect(1512, 0, 1920, 1080), QSize(1920, 1080), false, QSize(), QSize(), 30000),
    };
    DisplayProfile::Profile profile = DisplayPlanner::proposal(displays);
    DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY2(plan.ok, qPrintable(plan.error));
    int index = warningCodes(plan).indexOf(QStringLiteral("mixed-dpi"));
    QVERIFY(index >= 0);
    QCOMPARE(plan.warnings.at(index).action, QStringLiteral("match-text-size"));
    index = warningCodes(plan).indexOf(QStringLiteral("low-refresh"));
    QVERIFY(index >= 0);
    QCOMPARE(plan.warnings.at(index).key, QStringLiteral("uuid:1080"));
    QVERIFY(plan.warnings.at(index).text.contains(QStringLiteral("30 Hz")));
    // "Match text size" is looks-like on the 2x screens: the warning goes.
    profile.choice(QStringLiteral("uuid:MBP14")).size = DisplayProfile::SizeMode::LooksLike;
    plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo());
    QVERIFY(!warningCodes(plan).contains(QStringLiteral("mixed-dpi")));
    // Separate Spaces off: one window.
    DisplayPlanner::Limits limits;
    limits.separateSpaces = false;
    plan = DisplayPlanner::plan(displays, profile, DisplayPlanner::HostInfo(), limits);
    QCOMPARE(plan.presentation, QStringLiteral("single"));
    index = warningCodes(plan).indexOf(QStringLiteral("spaces"));
    QVERIFY(index >= 0);
    QCOMPARE(plan.warnings.at(index).action, QStringLiteral("open-spaces"));
}

void TestDisplayPlanner::backingPreviewUsesTheHost()
{
    // The physical-only laptop host of the vectors: its panel runs 1920x1080.
    const DisplayArrangement::Capabilities laptopHost = vectorCapabilities(QStringLiteral("physical-laptop"));
    QVector<NvClientDisplay> displays {mac(QStringLiteral("uuid:1080"), QRect(0, 0, 1920, 1080), QSize(1920, 1080), true)};
    DisplayPlanner::Plan plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays),
                                                     arrangementHost(laptopHost));
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QVERIFY(!plan.backingExpected);
    QCOMPARE(plan.outputs.first().backing, DisplayArrangement::Backing::Physical);
    QCOMPARE(plan.outputs.first().backingOutput, QStringLiteral("x11:DP-4"));
    QCOMPARE(plan.outputs.first().sizeOptions.size(), 2); // exact (= its mode) and custom
    displays = {macBook14()};
    plan = DisplayPlanner::plan(displays, DisplayPlanner::proposal(displays), arrangementHost(laptopHost));
    QVERIFY2(plan.ok, qPrintable(plan.error));
    QCOMPARE(plan.outputs.first().backing, DisplayArrangement::Backing::PhysicalViewport);
}

void TestDisplayPlanner::arrangeInvariants()
{
    // Every arrangement: origin 0, even, no overlap, neighbours share >= 64 px.
    const QVector<QVector<QRect>> desks {
        {QRect(0, 0, 1512, 982), QRect(1512, -300, 1920, 1080)},
        {QRect(0, 0, 1512, 982), QRect(-2560, 500, 2560, 1440), QRect(1512, -1000, 1080, 1920)},
        {QRect(0, 0, 1920, 1080), QRect(0, 1080, 1920, 1080), QRect(1920, 540, 1920, 1080), QRect(-1920, 1080, 1920, 1080)},
        {QRect(0, 0, 3440, 1440), QRect(3440, 1439, 2560, 1440)},
    };
    const QVector<QVector<QSize>> sizes {
        {QSize(3024, 1890), QSize(3840, 2160)},
        {QSize(3024, 1890), QSize(5120, 2880), QSize(1080, 1920)},
        {QSize(3840, 2160), QSize(1920, 1080), QSize(2560, 1440), QSize(1920, 1200)},
        {QSize(3440, 1440), QSize(5120, 2880)},
    };
    for (int desk = 0; desk < desks.size(); ++desk) {
        const QVector<QPoint> positions = DisplayPlanner::arrange(desks.at(desk), sizes.at(desk), 0);
        QCOMPARE(positions.size(), desks.at(desk).size());
        int minX = 1 << 30, minY = 1 << 30;
        QVector<QRect> rects;
        for (int index = 0; index < positions.size(); ++index) {
            QVERIFY((positions.at(index).x() & 1) == 0);
            QVERIFY((positions.at(index).y() & 1) == 0);
            minX = qMin(minX, positions.at(index).x());
            minY = qMin(minY, positions.at(index).y());
            rects.append(QRect(positions.at(index), sizes.at(desk).at(index)));
        }
        QCOMPARE(minX, 0);
        QCOMPARE(minY, 0);
        for (int a = 0; a < rects.size(); ++a) {
            for (int b = a + 1; b < rects.size(); ++b) QVERIFY2(!rects.at(a).intersects(rects.at(b)),
                                                                 qPrintable(QStringLiteral("desk %1").arg(desk)));
        }
        // The canonical request of such a layout is valid for a large enough host.
        QVector<DisplayArrangement::Entry> entries;
        for (const QRect& rect : std::as_const(rects)) entries.append({rect, DisplayArrangement::Preference::Auto});
        QVector<DisplayArrangement::Entry> parsed;
        QCOMPARE(DisplayArrangement::parse(DisplayArrangement::serialize(entries), parsed), QString());
    }
    // Seams: the two-desk case shares at least 64 px vertically.
    const QVector<QPoint> seam = DisplayPlanner::arrange(desks.at(3), sizes.at(3), 0);
    QVERIFY(qMin(seam.at(0).y() + 1440, seam.at(1).y() + 2880) - qMax(seam.at(0).y(), seam.at(1).y()) >=
            DisplayPlanner::MinimumSeam);
}

QTEST_GUILESS_MAIN(TestDisplayPlanner)
#include "test_displayplanner.moc"
