#include "outputtopology.h"

#include <algorithm>
#include <tuple>

#include <QJsonArray>
#include <QUuid>
#include <cmath>

const char* NvOutputTopology::NativeScalingMode = "native";
const char* NvOutputTopology::ScaledSpanMode = "scaled-span";
const char* NvOutputTopology::MatchClientHostLayout = "match-client";
const char* NvOutputTopology::PhysicalHostLayout = "physical";
const char* NvOutputTopology::SingleHostLayout = "single";
const char* NvOutputTopology::DualHorizontalHostLayout = "dual-horizontal";

bool NvOutputTopology::supportsDescription(int version, int featureFlags)
{
    if (version != ProtocolVersion) return false;
    if (featureFlags & FixedCaptureFeature) return featureFlags == FixedCaptureFlags;
    const int linuxDescription = OutputTopologyFeature | SelectedOutputFeature |
            UnifiedAbsoluteInputFeature;
    return (featureFlags & linuxDescription) == linuxDescription;
}

namespace {
QJsonObject applePreviewProfile(const QString& mode)
{
    const bool fullChroma = mode == QLatin1String("hevc-10-444-videotoolbox");
    if (!fullChroma && mode != QLatin1String("hevc-10-420-videotoolbox")) return {};
    return {{"capture_source", "screencapturekit"}, {"encoder_backend", "videotoolbox"},
            {"encoding_mode", mode}, {"codec", "hevc"},
            {"profile", fullChroma ? "rext" : "main10"}, {"bit_depth", 10}, {"chroma", fullChroma ? "4:4:4" : "4:2:0"},
            {"range", "full"}, {"matrix", "bt709"}, {"primaries", "bt709"},
            {"transfer", "srgb"}, {"rgb_identity", false}};
}

bool parseFixedCapture(const QJsonObject& object, NvOutputTopology& result)
{
    if (object.size() != 4 || object.value("schema_version") != QJsonValue(NvOutputTopology::ProtocolVersion) ||
            object.value("feature_flags") != QJsonValue(NvOutputTopology::FixedCaptureFlags) ||
            !object.value("capture").isObject()) return false;
    const QString generation = object.value("generation").toString();
    if (QUuid(generation).isNull() || QUuid(generation).toString(QUuid::WithoutBraces) != generation) return false;
    const auto capture = object.value("capture").toObject();
    const QString id = capture.value("id").toString();
    const QString encodingMode = capture.value("encoding_profile").toObject().value("encoding_mode").toString();
    const auto profile = applePreviewProfile(encodingMode);
    if (capture.size() != 5 || id.isEmpty() || id.size() > 128 || !capture.value("logical_bounds").isObject() ||
            profile.isEmpty() || capture.value("encoding_profile") != QJsonValue(profile)) return false;
    auto dimension = [&capture](const char* key) {
        const QJsonValue value = capture.value(key);
        if (!value.isDouble()) return 0;
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < 2 || number > 8192 || std::floor(number) != number) return 0;
        const int integer = static_cast<int>(number);
        return integer % 2 == 0 ? integer : 0;
    };
    const int width = dimension("width"), height = dimension("height");
    if (!width || !height) return false;
    const auto bounds = capture.value("logical_bounds").toObject();
    if (bounds.size() != 4) return false;
    for (const char* key : {"x", "y", "width", "height"}) {
        const auto value = bounds.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 65536) return false;
    }
    const QRectF logical(bounds.value("x").toDouble(), bounds.value("y").toDouble(),
                         bounds.value("width").toDouble(), bounds.value("height").toDouble());
    if (logical.width() <= 0 || logical.height() <= 0) return false;
    NvOutputTopology parsed;
    parsed.schemaVersion = NvOutputTopology::ProtocolVersion;
    parsed.featureFlags = NvOutputTopology::FixedCaptureFlags;
    parsed.generation = generation;
    parsed.desktopWidth = width;
    parsed.desktopHeight = height;
    parsed.layoutKind = parsed.startupLayoutKind = QStringLiteral("fixed");
    parsed.allowedLayoutKinds = {QStringLiteral("fixed")};
    parsed.captureLogicalBounds = logical;
    parsed.appleEncodingMode = encodingMode;
    NvOutput output;
    output.id = id;
    output.name = QStringLiteral("Current capture display");
    output.primary = true;
    output.width = output.sourceWidth = width;
    output.height = output.sourceHeight = height;
    parsed.outputs.append(output);
    result = parsed;
    return true;
}

bool requireInteger(const QJsonObject& object, const char* name, int& value)
{
    const QJsonValue field = object.value(name);
    if (!field.isDouble()) {
        return false;
    }
    const double number = field.toDouble();
    value = static_cast<int>(number);
    return number == value;
}

bool validLayoutKind(const QString& kind)
{
    return kind == NvOutputTopology::PhysicalHostLayout ||
            kind == NvOutputTopology::SingleHostLayout ||
            kind == NvOutputTopology::DualHorizontalHostLayout;
}

}

QStringList NvOutputTopology::qualifiedVirtualModes()
{
    return {QStringLiteral("1024x2160"), QStringLiteral("1280x2160"),
            QStringLiteral("1920x1080"), QStringLiteral("1920x1200"),
            QStringLiteral("2560x1440"), QStringLiteral("2560x1600"),
            QStringLiteral("2560x2160"), QStringLiteral("3024x1890"),
            QStringLiteral("3440x1440"), QStringLiteral("3840x1600"),
            QStringLiteral("3840x2160"), QStringLiteral("4096x2160"),
            QStringLiteral("5120x2160")};
}

bool NvOutputTopology::hostAcceptsVirtualMode(const QString& mode, int hostFeatureFlags)
{
    if (!qualifiedVirtualModes().contains(mode)) {
        return false;
    }
    // A 14-inch MacBook Pro's fullscreen viewport below the camera housing.
    return mode != QLatin1String("3024x1890") ||
            (hostFeatureFlags & NotchSafeLaptopModesFeature) != 0;
}

QStringList NvOutputTopology::virtualModesForHost(int hostFeatureFlags)
{
    QStringList modes;
    for (const QString& mode : qualifiedVirtualModes()) {
        if (hostAcceptsVirtualMode(mode, hostFeatureFlags)) {
            modes.append(mode);
        }
    }
    return modes;
}

QSize NvOutputTopology::virtualModeSize(const QString& mode)
{
    const QStringList parts = mode.split(QLatin1Char('x'));
    if (!qualifiedVirtualModes().contains(mode) || parts.size() != 2) {
        return QSize();
    }
    return QSize(parts[0].toInt(), parts[1].toInt());
}

QSize NvOutputTopology::virtualCanvasSize(const QString& hostLayout,
                                          const QStringList& virtualModes)
{
    const QSize first = virtualModeSize(virtualModes.value(0));
    if (hostLayout == SingleHostLayout) {
        return first;
    }
    if (hostLayout != DualHorizontalHostLayout || !first.isValid()) {
        return QSize();
    }

    const QSize second = virtualModeSize(virtualModes.value(1));
    if (!second.isValid()) {
        return QSize();
    }
    const int width = first.width() + second.width();
    if (width > MaximumVirtualCanvasWidth) {
        return QSize();
    }
    return QSize(width, qMax(first.height(), second.height()));
}

bool NvOutputTopology::fromJson(const QJsonObject& object,
                                NvOutputTopology& topology, QString* error)
{
    if (object.contains("capture") ||
            (object.value("feature_flags").toInt() & FixedCaptureFeature)) {
        const bool valid = parseFixedCapture(object, topology);
        if (!valid && error) *error = QStringLiteral("Unsupported or malformed fixed capture description");
        return valid;
    }
    NvOutputTopology parsed;
    if (!requireInteger(object, "schema_version", parsed.schemaVersion) ||
            parsed.schemaVersion != ProtocolVersion ||
            !requireInteger(object, "feature_flags", parsed.featureFlags) ||
            (parsed.featureFlags & (OutputTopologyFeature | SelectedOutputFeature |
                                    UnifiedAbsoluteInputFeature |
                                    HostLayoutMetadataFeature |
                                    CompositeSourceRegionsFeature |
                                    HostLayoutBindingFeature |
                                    IndependentVirtualModesFeature |
                                    DynamicHostLayoutFeature |
                                    TemporaryPhysicalLayoutFeature |
                                    FixedTransportMtuFeature |
                                    SessionTakeoverFeature)) !=
                (OutputTopologyFeature | SelectedOutputFeature |
                 UnifiedAbsoluteInputFeature |
                 HostLayoutMetadataFeature |
                 CompositeSourceRegionsFeature |
                 HostLayoutBindingFeature |
                 IndependentVirtualModesFeature |
                 DynamicHostLayoutFeature |
                 TemporaryPhysicalLayoutFeature |
                 FixedTransportMtuFeature |
                 SessionTakeoverFeature) ||
            !object.value("generation").isString() ||
            !object.value("layout").isObject() ||
            !object.value("desktop").isObject() ||
            !object.value("outputs").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unsupported or malformed output topology header");
        }
        return false;
    }
    parsed.generation = object.value("generation").toString();
    const QJsonObject layout = object.value("layout").toObject();
    int declaredOutputCount = 0;
    parsed.layoutKind = layout.value("kind").toString();
    parsed.startupLayoutKind = layout.value("startup_kind").toString();
    if (!layout.value("allowed_kinds").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host allowed-layout list");
        }
        return false;
    }
    for (const QJsonValue& kind : layout.value("allowed_kinds").toArray()) {
        if (!kind.isString() || !validLayoutKind(kind.toString()) ||
                parsed.allowedLayoutKinds.contains(kind.toString())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid host allowed layout");
            }
            return false;
        }
        parsed.allowedLayoutKinds.append(kind.toString());
    }
    if (!layout.value("virtual_modes").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host virtual-mode list");
        }
        return false;
    }
    for (const QJsonValue& mode : layout.value("virtual_modes").toArray()) {
        if (!mode.isString() || !hostAcceptsVirtualMode(mode.toString(), parsed.featureFlags)) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid host virtual mode");
            }
            return false;
        }
        parsed.virtualModes.append(mode.toString());
    }
    if (!validLayoutKind(parsed.layoutKind) ||
            !validLayoutKind(parsed.startupLayoutKind) ||
            parsed.allowedLayoutKinds.isEmpty() ||
            !parsed.allowedLayoutKinds.contains(parsed.layoutKind) ||
            (parsed.startupLayoutKind == PhysicalHostLayout &&
             parsed.allowedLayoutKinds != QStringList {
                 QString::fromLatin1(PhysicalHostLayout),
                 QString::fromLatin1(SingleHostLayout),
                 QString::fromLatin1(DualHorizontalHostLayout)}) ||
            (parsed.startupLayoutKind != PhysicalHostLayout &&
             parsed.allowedLayoutKinds != QStringList {
                 QString::fromLatin1(SingleHostLayout),
                 QString::fromLatin1(DualHorizontalHostLayout)}) ||
            !layout.value("virtual").isBool() ||
            !requireInteger(layout, "output_count", declaredOutputCount) ||
            declaredOutputCount <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host display layout metadata");
        }
        return false;
    }
    parsed.virtualLayout = layout.value("virtual").toBool();
    if ((parsed.layoutKind == PhysicalHostLayout &&
         (parsed.virtualLayout || !parsed.virtualModes.isEmpty())) ||
            (parsed.layoutKind == SingleHostLayout &&
             (!parsed.virtualLayout || parsed.virtualModes.size() != 1)) ||
            (parsed.layoutKind == DualHorizontalHostLayout &&
             (!parsed.virtualLayout || parsed.virtualModes.size() != 2))) {
        if (error != nullptr) {
            *error = QStringLiteral("Inconsistent host display layout metadata");
        }
        return false;
    }
    const QJsonObject desktop = object.value("desktop").toObject();
    if (!requireInteger(desktop, "x", parsed.desktopX) ||
            !requireInteger(desktop, "y", parsed.desktopY) ||
            !requireInteger(desktop, "width", parsed.desktopWidth) ||
            !requireInteger(desktop, "height", parsed.desktopHeight) ||
            parsed.desktopWidth <= 0 || parsed.desktopHeight <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid output topology desktop bounds");
        }
        return false;
    }

    // The encoded capture: the desktop, unless a host with the display
    // arrangement extension publishes capture_size (during an arrangement
    // lease; with each output's capture_rect). source_rect keeps its schema-13
    // meaning (desktop coordinates) either way. Ignored without the bit.
    parsed.captureWidth = parsed.desktopWidth;
    parsed.captureHeight = parsed.desktopHeight;
    if ((parsed.featureFlags & DisplayArrangementFeature) != 0 && object.contains("capture_size")) {
        const QJsonObject capture = object.value("capture_size").toObject();
        if (!object.value("capture_size").isObject() || capture.size() != 2 ||
                !requireInteger(capture, "width", parsed.captureWidth) ||
                !requireInteger(capture, "height", parsed.captureHeight) ||
                parsed.captureWidth < 2 || parsed.captureHeight < 2 ||
                parsed.captureWidth > 16384 || parsed.captureHeight > 16384) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid capture size");
            }
            return false;
        }
        parsed.capturePublished = true;
    }

    for (const QJsonValue& value : object.value("outputs").toArray()) {
        if (!value.isObject()) {
            return false;
        }
        const QJsonObject entry = value.toObject();
        NvOutput output;
        output.id = entry.value("id").toString();
        output.name = entry.value("name").toString();
        if (output.id.isEmpty() || output.name.isEmpty() ||
                !requireInteger(entry, "x", output.x) ||
                !requireInteger(entry, "y", output.y) ||
                !requireInteger(entry, "width", output.width) ||
                !requireInteger(entry, "height", output.height) ||
                !requireInteger(entry, "rotation", output.rotation) ||
                !requireInteger(entry, "refresh_millihz", output.refreshMillihz) ||
                !entry.value("primary").isBool() ||
                !entry.value("virtual").isBool() ||
                !entry.value("source_rect").isObject() || output.width <= 0 ||
                output.height <= 0 || parsed.contains(output.id)) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid or duplicate output entry");
            }
            return false;
        }
        output.primary = entry.value("primary").toBool();
        output.virtualOutput = entry.value("virtual").toBool();
        output.configuredMode = entry.value("configured_mode").toString();
        const QJsonObject sourceRect = entry.value("source_rect").toObject();
        if (!requireInteger(sourceRect, "x", output.sourceX) ||
                !requireInteger(sourceRect, "y", output.sourceY) ||
                !requireInteger(sourceRect, "width", output.sourceWidth) ||
                !requireInteger(sourceRect, "height", output.sourceHeight) ||
                output.sourceX < 0 || output.sourceY < 0 ||
                output.sourceWidth <= 0 || output.sourceHeight <= 0 ||
                output.sourceX + output.sourceWidth > parsed.desktopWidth ||
                output.sourceY + output.sourceHeight > parsed.desktopHeight ||
                output.virtualOutput != parsed.virtualLayout ||
                (parsed.virtualLayout &&
                 (parsed.outputs.size() >= parsed.virtualModes.size() ||
                  output.configuredMode != parsed.virtualModes[parsed.outputs.size()] ||
                  virtualModeSize(output.configuredMode) != QSize(output.width, output.height))) ||
                (!parsed.virtualLayout && !output.configuredMode.isEmpty())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid composite source rectangle or output provenance");
            }
            return false;
        }
        // Display arrangement: where the output sits in the encoded capture.
        // Published with capture_size (during an arrangement lease), and the
        // same as source_rect unless the capture is packed. Ignored without
        // the bit, like capture_size.
        output.captureX = output.sourceX;
        output.captureY = output.sourceY;
        output.captureWidth = output.sourceWidth;
        output.captureHeight = output.sourceHeight;
        if ((parsed.featureFlags & DisplayArrangementFeature) != 0) {
            const bool hasCaptureRect = entry.contains("capture_rect");
            const QJsonObject captureRect = entry.value("capture_rect").toObject();
            if (hasCaptureRect != parsed.capturePublished ||
                    (hasCaptureRect &&
                     (!entry.value("capture_rect").isObject() || captureRect.size() != 4 ||
                      !requireInteger(captureRect, "x", output.captureX) ||
                      !requireInteger(captureRect, "y", output.captureY) ||
                      !requireInteger(captureRect, "width", output.captureWidth) ||
                      !requireInteger(captureRect, "height", output.captureHeight) ||
                      output.captureX < 0 || output.captureY < 0 ||
                      output.captureWidth <= 0 || output.captureHeight <= 0 ||
                      output.captureX + output.captureWidth > parsed.captureWidth ||
                      output.captureY + output.captureHeight > parsed.captureHeight))) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid output capture rectangle");
                }
                return false;
            }
        }
        parsed.outputs.append(output);
    }
    if (parsed.outputs.isEmpty() || parsed.outputs.size() != declaredOutputCount ||
            (parsed.layoutKind == SingleHostLayout && parsed.outputs.size() != 1) ||
            (parsed.layoutKind == DualHorizontalHostLayout && parsed.outputs.size() != 2)) {
        if (error != nullptr) {
            *error = QStringLiteral("Host reported no connected outputs");
        }
        return false;
    }
    // Display arrangement: additive fields, read (strictly) only when the
    // host advertises the extension. Without the bit they are ignored, as
    // every schema-13 parser before it does.
    if ((parsed.featureFlags & DisplayArrangementFeature) != 0 &&
            !parseDisplayArrangement(object, layout, parsed, error)) {
        return false;
    }
    topology = parsed;
    return true;
}

bool NvOutputTopology::parseDisplayArrangement(const QJsonObject& object, const QJsonObject& layout,
                                               NvOutputTopology& parsed, QString* error)
{
    const auto fail = [error](const char* message) {
        if (error != nullptr) *error = QString::fromLatin1(message);
        return false;
    };
    QString capabilitiesError;
    if (!object.value(QStringLiteral("display_capabilities")).isObject() ||
            !DisplayArrangement::Capabilities::fromJson(object.value(QStringLiteral("display_capabilities")).toObject(),
                                                        parsed.displayCapabilities, &capabilitiesError)) {
        if (error != nullptr) *error = QStringLiteral("Invalid display capabilities: ") + capabilitiesError;
        return false;
    }
    parsed.startupPolicy = layout.value(QStringLiteral("startup_policy")).toString();
    if (parsed.startupPolicy != QLatin1String("physical") && parsed.startupPolicy != QLatin1String("virtual") &&
            parsed.startupPolicy != QLatin1String("hybrid")) {
        return fail("Invalid host display startup policy");
    }
    const QJsonValue arrangementValue = layout.value(QStringLiteral("arrangement"));
    const QJsonObject arrangement = arrangementValue.toObject();
    const QJsonObject transition = arrangement.value(QStringLiteral("transition")).toObject();
    parsed.arrangementRequest = arrangement.value(QStringLiteral("request")).toString();
    parsed.arrangementState = transition.value(QStringLiteral("state")).toString();
    parsed.arrangementReason = transition.value(QStringLiteral("reason")).toString();
    QVector<DisplayArrangement::Entry> entries;
    if (!arrangementValue.isObject() || !arrangement.value(QStringLiteral("request")).isString() ||
            !arrangement.value(QStringLiteral("transition")).isObject() ||
            !transition.value(QStringLiteral("reason")).isString() || parsed.arrangementReason.size() > 64 ||
            (parsed.arrangementState != QLatin1String("idle") && parsed.arrangementState != QLatin1String("pending") &&
             parsed.arrangementState != QLatin1String("applied") && parsed.arrangementState != QLatin1String("failed")) ||
            (!parsed.arrangementRequest.isEmpty() &&
             !DisplayArrangement::parse(parsed.arrangementRequest, entries).isEmpty())) {
        return fail("Invalid host display arrangement state");
    }
    const QJsonArray outputs = object.value(QStringLiteral("outputs")).toArray();
    QVector<bool> seen(DisplayArrangement::MaximumEntries, false);
    for (int index = 0; index < outputs.size(); ++index) {
        const QJsonObject entry = outputs.at(index).toObject();
        NvOutput& output = parsed.outputs[index];
        DisplayArrangement::Backing backing;
        output.backing = entry.value(QStringLiteral("backing")).toString();
        if (!DisplayArrangement::backingFromName(output.backing, backing) ||
                !requireInteger(entry, "arrangement_index", output.arrangementIndex) ||
                output.arrangementIndex < -1 || output.arrangementIndex >= DisplayArrangement::MaximumEntries ||
                (output.arrangementIndex >= 0 && seen.at(output.arrangementIndex))) {
            return fail("Invalid output backing or arrangement index");
        }
        if (output.arrangementIndex >= 0) seen[output.arrangementIndex] = true;
    }
    return true;
}

bool NvOutputTopology::matchesRequestedArrangement(const QString& request) const
{
    QVector<DisplayArrangement::Entry> entries;
    if (!displayArrangementPublished() || request.isEmpty() || arrangementRequest != request ||
            arrangementState != QLatin1String("applied") ||
            !DisplayArrangement::parse(request, entries).isEmpty() || outputs.size() != entries.size()) {
        return false;
    }
    QVector<bool> seen(entries.size(), false);
    for (const NvOutput& output : outputs) {
        const int index = output.arrangementIndex;
        if (index < 0 || index >= entries.size() || seen.at(index) ||
                QRect(output.x - desktopX, output.y - desktopY, output.width, output.height) != entries.at(index).rect) {
            return false;
        }
        seen[index] = true;
    }
    return true;
}

QJsonObject NvOutputTopology::toJson() const
{
    if (featureFlags == FixedCaptureFlags && outputs.size() == 1) {
        return {{"schema_version", schemaVersion}, {"feature_flags", featureFlags},
                {"generation", generation}, {"capture", QJsonObject {
                    {"id", outputs.first().id}, {"width", desktopWidth}, {"height", desktopHeight},
                    {"logical_bounds", QJsonObject {{"x", captureLogicalBounds.x()}, {"y", captureLogicalBounds.y()},
                        {"width", captureLogicalBounds.width()}, {"height", captureLogicalBounds.height()}}},
                    {"encoding_profile", applePreviewProfile(appleEncodingMode)}}}};
    }
    const bool arrangement = displayArrangementPublished();
    QJsonArray serializedOutputs;
    for (const NvOutput& output : outputs) {
        QJsonObject entry {
            {"id", output.id}, {"name", output.name},
            {"x", output.x}, {"y", output.y},
            {"width", output.width}, {"height", output.height},
            {"rotation", output.rotation},
            {"refresh_millihz", output.refreshMillihz},
            {"primary", output.primary},
            {"virtual", output.virtualOutput},
            {"configured_mode", output.configuredMode},
            {"source_rect", QJsonObject {
                {"x", output.sourceX}, {"y", output.sourceY},
                {"width", output.sourceWidth}, {"height", output.sourceHeight},
            }},
        };
        if (arrangement) {
            entry.insert("backing", output.backing);
            entry.insert("arrangement_index", output.arrangementIndex);
            if (capturePublished) {
                entry.insert("capture_rect", QJsonObject {
                    {"x", output.captureX}, {"y", output.captureY},
                    {"width", output.captureWidth}, {"height", output.captureHeight}});
            }
        }
        serializedOutputs.append(entry);
    }
    QJsonObject serializedLayout {
        {"kind", layoutKind}, {"virtual", virtualLayout},
        {"virtual_modes", QJsonArray::fromStringList(virtualModes)},
        {"output_count", outputs.size()},
        {"startup_kind", startupLayoutKind},
        {"allowed_kinds", QJsonArray::fromStringList(allowedLayoutKinds)},
    };
    if (arrangement) {
        serializedLayout.insert("startup_policy", startupPolicy);
        serializedLayout.insert("arrangement", QJsonObject {
            {"request", arrangementRequest},
            {"transition", QJsonObject {{"state", arrangementState}, {"reason", arrangementReason}}},
        });
    }
    QJsonObject document {
        {"schema_version", schemaVersion},
        {"feature_flags", featureFlags},
        {"generation", generation},
        {"layout", serializedLayout},
        {"desktop", QJsonObject {
            {"x", desktopX}, {"y", desktopY},
            {"width", desktopWidth}, {"height", desktopHeight},
        }},
        {"outputs", serializedOutputs},
    };
    if (arrangement) document.insert("display_capabilities", displayCapabilities.toJson());
    if (arrangement && capturePublished) {
        document.insert("capture_size", QJsonObject {{"width", captureWidth}, {"height", captureHeight}});
    }
    return document;
}

bool NvOutputTopology::contains(QString outputId) const
{
    for (const NvOutput& output : outputs) {
        if (output.id == outputId) {
            return true;
        }
    }
    return false;
}

bool NvOutputTopology::displayPolicyKnown() const
{
    if (schemaVersion == ProtocolVersion && featureFlags == FixedCaptureFlags) return true;
    return schemaVersion == ProtocolVersion &&
            validLayoutKind(layoutKind) && validLayoutKind(startupLayoutKind) &&
            !allowedLayoutKinds.isEmpty();
}

bool NvOutputTopology::allowsBookmarkHostLayout(const QString& layout) const
{
    if (featureFlags == FixedCaptureFlags) return layout == QStringLiteral("fixed") || layout == MatchClientHostLayout;
    if (!displayPolicyKnown()) {
        return true;
    }
    if (layout == MatchClientHostLayout) {
        return allowedLayoutKinds.contains(SingleHostLayout) &&
                allowedLayoutKinds.contains(DualHorizontalHostLayout);
    }
    return allowedLayoutKinds.contains(layout);
}

bool NvOutputTopology::matchesRequestedHostLayout(const QString& layout,
                                                  const QStringList& modes) const
{
    if (layoutKind != layout || virtualModes != modes) {
        return false;
    }
    if (layout == PhysicalHostLayout) {
        return !virtualLayout && !outputs.isEmpty();
    }
    if (layout == SingleHostLayout) {
        return virtualLayout && outputs.size() == 1;
    }
    if (layout != DualHorizontalHostLayout || !virtualLayout || outputs.size() != 2) {
        return false;
    }

    const NvOutput& left = outputs.at(0);
    const NvOutput& right = outputs.at(1);
    return left.y == right.y &&
            right.x == left.x + left.width &&
            desktopX == left.x &&
            desktopY == left.y &&
            desktopWidth == left.width + right.width &&
            desktopHeight == qMax(left.height, right.height) &&
            left.sourceX == 0 && left.sourceY == 0 &&
            right.sourceX == left.width && right.sourceY == 0;
}

int NvOutputTopology::hostPlatform(int version, int flags)
{
    if (!supportsDescription(version, flags)) return 0;
    return flags == FixedCaptureFlags ? 2 : 1;
}

QSize NvOutputTopology::macDisplayModeSize(const QString& mode)
{
    const auto parts = mode.split(QLatin1Char('x'));
    if (parts.size() != 2) return {};
    bool widthOk, heightOk;
    const int width = parts[0].toInt(&widthOk), height = parts[1].toInt(&heightOk);
    if (!widthOk || !heightOk || width < 2 || height < 2 || width > 8192 || height > 8192 ||
            width % 2 || height % 2 || mode != QStringLiteral("%1x%2").arg(width).arg(height)) return {};
    return QSize(width, height);
}

QJsonObject NvOutputTopology::macDisplayRequest(const QString& mode, const QString& encodingMode, int scale)
{
    const QSize size = macDisplayModeSize(mode);
    if (!size.isValid() || (scale != 1 && scale != 2) ||
            (encodingMode != QLatin1String("hevc-10-420-videotoolbox") &&
             encodingMode != QLatin1String("hevc-10-444-videotoolbox"))) return {};
    return {{"schema_version", 3}, {"width", size.width()}, {"height", size.height()},
            {"scale", scale}, {"encoding_mode", encodingMode}};
}

QString NvOutputTopology::resolveMacClientDisplayMode(const QVector<NvClientDisplay>& displays, QString* error, int* scale)
{
    if (error) error->clear();
    if (scale) *scale = 1;
    if (displays.isEmpty() || displays.size() > 2) {
        if (error) *error = QStringLiteral("Match client displays requires exactly one or two active client monitors.");
        return {};
    }
    if (displays.size() == 2) {
        const QRect a = displays[0].bounds, b = displays[1].bounds;
        if (!(a.right() < b.left() || b.right() < a.left()) ||
                a.top() > b.bottom() || b.top() > a.bottom()) {
            if (error) *error = QStringLiteral("Match client displays currently requires two monitors arranged left to right.");
            return {};
        }
    }
    int width = 0, height = 0, canvasScale = 0;
    for (const auto& display : displays) {
        const QSize pixels = display.backingSize.isValid() ? display.backingSize : display.nativeSize;
        const int displayScale = display.backingSize.isValid() && pixels == display.bounds.size() * 2 ? 2 : 1;
        if ((display.backingSize.isValid() && pixels != display.bounds.size() * displayScale) ||
                (canvasScale && canvasScale != displayScale)) {
            if (error) *error = QStringLiteral("Match Client requires the same 1x or 2x Retina scale on both monitors. Use a manual resolution for mixed scaling.");
            return {};
        }
        canvasScale = displayScale;
        const QString size = QStringLiteral("%1x%2").arg(pixels.width()).arg(pixels.height());
        if (!display.bounds.isValid() || !macDisplayModeSize(size).isValid()) {
            if (error) *error = QStringLiteral("Mac desktop dimensions must be even pixel counts between 2 and 8192. Detected %1.").arg(size);
            return {};
        }
        width += pixels.width();
        height = qMax(height, pixels.height());
    }
    const QString mode = QStringLiteral("%1x%2").arg(width).arg(height);
    if (!macDisplayModeSize(mode).isValid()) {
        if (error) *error = QStringLiteral("The client display canvas (%1) exceeds the Mac desktop size limit.").arg(mode);
        return {};
    }
    if (scale) *scale = canvasScale;
    return mode;
}

QSize NvOutputTopology::clientMatchTarget(const QSize& desktopPixels, const QSize& panelPixels)
{
    const QSize base = desktopPixels.isValid() ? desktopPixels : panelPixels;
    if (!base.isValid() || base.isEmpty()) {
        return QSize();
    }
    if (!panelPixels.isValid() || panelPixels.isEmpty() ||
            (base.width() <= panelPixels.width() && base.height() <= panelPixels.height())) {
        return base;
    }
    const double scale = qMin(double(panelPixels.width()) / base.width(),
                              double(panelPixels.height()) / base.height());
    return QSize(qBound(1, int(std::lround(base.width() * scale)), panelPixels.width()),
                 qBound(1, int(std::lround(base.height() * scale)), panelPixels.height()));
}

QSize NvOutputTopology::clientMatchTarget(const NvClientDisplay& display)
{
    // Streams are presented in native fullscreen, which macOS places below the camera housing: match that
    // viewport (16:10 on notched MacBooks) rather than the full desktop, or it is always letterboxed.
    const QSize desktop = display.fullscreenSize.isValid() ? display.fullscreenSize : display.backingSize;
    return clientMatchTarget(desktop, display.nativeSize);
}

QStringList NvOutputTopology::rankedVirtualModes(const QSize& target, const QStringList& candidateModes)
{
    if (!target.isValid() || target.isEmpty()) {
        return {};
    }
    struct Candidate {
        QString mode;
        bool fits;
        double aspectError;
        qint64 area;
    };
    const QString exact = QStringLiteral("%1x%2").arg(target.width()).arg(target.height());
    const bool portrait = target.height() > target.width();
    const double targetAspect = double(target.width()) / target.height();
    QVector<Candidate> candidates;
    QStringList ranked;
    for (const QString& mode : candidateModes) {
        const QSize size = virtualModeSize(mode);
        if (!size.isValid()) {
            continue;
        }
        if (mode == exact) {
            ranked.append(mode);
            continue;
        }
        // The ultra-tall halves (1024/1280/2560x2160, narrower than 4:3) are
        // made for pairs: a landscape display only gets a landscape mode, a
        // portrait display only a portrait one.
        const bool landscapeMode = size.width() * 3 >= size.height() * 4;
        const bool portraitMode = size.height() > size.width();
        if (portrait ? !portraitMode : !landscapeMode) {
            continue;
        }
        candidates.append({mode,
                           size.width() <= target.width() && size.height() <= target.height(),
                           std::fabs(std::log((double(size.width()) / size.height()) / targetAspect)),
                           qint64(size.width()) * size.height()});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.fits != b.fits) return a.fits;
        // 1920x1200 and 2560x1600 share one aspect ratio; compare with a
        // tolerance so rounding never decides between them.
        if (std::fabs(a.aspectError - b.aspectError) > 1e-9) return a.aspectError < b.aspectError;
        return a.fits ? a.area > b.area : a.area < b.area;
    });
    for (const Candidate& candidate : std::as_const(candidates)) {
        ranked.append(candidate.mode);
    }
    return ranked;
}

bool NvOutputTopology::resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                                  QString& hostLayout,
                                                  QStringList& virtualModes,
                                                  QString* error,
                                                  bool* fitted,
                                                  const QStringList& candidateModes)
{
    hostLayout.clear();
    virtualModes.clear();
    if (fitted != nullptr) {
        *fitted = false;
    }
    if (displays.size() < 1 || displays.size() > 2) {
        if (error != nullptr) {
            *error = QStringLiteral("Match client displays requires exactly one or two active client monitors.");
        }
        return false;
    }

    std::sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
        return std::make_tuple(left.bounds.x(), left.bounds.y()) <
                std::make_tuple(right.bounds.x(), right.bounds.y());
    });
    if (displays.size() == 2) {
        const QRect& left = displays.at(0).bounds;
        const QRect& right = displays.at(1).bounds;
        const bool horizontallySeparated = left.right() < right.left();
        const bool verticallyOverlapping =
                left.top() <= right.bottom() && right.top() <= left.bottom();
        if (!horizontallySeparated || !verticallyOverlapping) {
            if (error != nullptr) {
                *error = QStringLiteral("Match client displays currently requires two monitors arranged left to right.");
            }
            return false;
        }
    }

    QVector<QStringList> rankings;
    QStringList targets;
    QVector<int> choice;
    for (const NvClientDisplay& display : std::as_const(displays)) {
        const QSize target = clientMatchTarget(display);
        const QStringList ranked = rankedVirtualModes(target, candidateModes);
        if (ranked.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("The size of a client monitor could not be detected.");
            }
            return false;
        }
        rankings.append(ranked);
        targets.append(QStringLiteral("%1x%2").arg(target.width()).arg(target.height()));
        choice.append(0);
    }

    // Two displays share one virtual canvas; step the wider display down its
    // own ranking until the pair fits the host's canvas limit.
    const auto canvasWidth = [&]() {
        int width = 0;
        for (int index = 0; index < rankings.size(); ++index) {
            width += virtualModeSize(rankings[index][choice[index]]).width();
        }
        return width;
    };
    while (canvasWidth() > MaximumVirtualCanvasWidth) {
        int widest = -1;
        for (int index = 0; index < rankings.size(); ++index) {
            const int width = virtualModeSize(rankings[index][choice[index]]).width();
            bool narrower = false;
            for (int next = choice[index] + 1; next < rankings[index].size(); ++next) {
                narrower = narrower || virtualModeSize(rankings[index][next]).width() < width;
            }
            if (narrower && (widest < 0 ||
                             width > virtualModeSize(rankings[widest][choice[widest]]).width())) {
                widest = index;
            }
        }
        if (widest < 0) {
            if (error != nullptr) {
                *error = QStringLiteral("The client monitors are too wide together for a matched virtual layout.");
            }
            return false;
        }
        const int width = virtualModeSize(rankings[widest][choice[widest]]).width();
        do {
            ++choice[widest];
        } while (virtualModeSize(rankings[widest][choice[widest]]).width() >= width);
    }

    bool anyFitted = false;
    for (int index = 0; index < rankings.size(); ++index) {
        const QString& mode = rankings[index][choice[index]];
        anyFitted = anyFitted || mode != targets[index];
        virtualModes.append(mode);
    }
    if (fitted != nullptr) {
        *fitted = anyFitted;
    }
    hostLayout = displays.size() == 1 ? QString::fromLatin1(SingleHostLayout) :
                                       QString::fromLatin1(DualHorizontalHostLayout);
    return true;
}
