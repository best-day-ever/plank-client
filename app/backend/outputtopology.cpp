#include "outputtopology.h"

#include <algorithm>
#include <tuple>

#include <QJsonArray>

const char* NvOutputTopology::NativeScalingMode = "native";
const char* NvOutputTopology::ScaledSpanMode = "scaled-span";
const char* NvOutputTopology::MatchClientHostLayout = "match-client";
const char* NvOutputTopology::PhysicalHostLayout = "physical";
const char* NvOutputTopology::SingleHostLayout = "single";
const char* NvOutputTopology::DualHorizontalHostLayout = "dual-horizontal";

namespace {
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
    return {QStringLiteral("1024x2160"), QStringLiteral("1280x720"),
            QStringLiteral("1280x1024"), QStringLiteral("1280x2160"),
            QStringLiteral("1920x1080"), QStringLiteral("1920x1200"),
            QStringLiteral("2560x1440"), QStringLiteral("2560x1600"),
            QStringLiteral("2560x2160"),
            QStringLiteral("3440x1440"), QStringLiteral("3840x1600"),
            QStringLiteral("3840x2160"), QStringLiteral("4096x2160")};
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
    return QSize(first.width() + second.width(),
                 qMax(first.height(), second.height()));
}

bool NvOutputTopology::fromJson(const QJsonObject& object,
                                NvOutputTopology& topology, QString* error)
{
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
                                    DynamicHostLayoutFeature)) !=
                (OutputTopologyFeature | SelectedOutputFeature |
                 UnifiedAbsoluteInputFeature |
                 HostLayoutMetadataFeature |
                 CompositeSourceRegionsFeature |
                 HostLayoutBindingFeature |
                 IndependentVirtualModesFeature |
                 DynamicHostLayoutFeature) ||
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
    if (!layout.value("virtual_modes").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid host virtual-mode list");
        }
        return false;
    }
    for (const QJsonValue& mode : layout.value("virtual_modes").toArray()) {
        if (!mode.isString() || !qualifiedVirtualModes().contains(mode.toString())) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid host virtual mode");
            }
            return false;
        }
        parsed.virtualModes.append(mode.toString());
    }
    if (!validLayoutKind(parsed.layoutKind) ||
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
    topology = parsed;
    return true;
}

QJsonObject NvOutputTopology::toJson() const
{
    QJsonArray serializedOutputs;
    for (const NvOutput& output : outputs) {
        serializedOutputs.append(QJsonObject {
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
        });
    }
    return QJsonObject {
        {"schema_version", schemaVersion},
        {"feature_flags", featureFlags},
        {"generation", generation},
        {"layout", QJsonObject {
            {"kind", layoutKind}, {"virtual", virtualLayout},
            {"virtual_modes", QJsonArray::fromStringList(virtualModes)},
            {"output_count", outputs.size()},
        }},
        {"desktop", QJsonObject {
            {"x", desktopX}, {"y", desktopY},
            {"width", desktopWidth}, {"height", desktopHeight},
        }},
        {"outputs", serializedOutputs},
    };
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

bool NvOutputTopology::resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                                  QString& hostLayout,
                                                  QStringList& virtualModes,
                                                  QString* error)
{
    hostLayout.clear();
    virtualModes.clear();
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

    for (const NvClientDisplay& display : displays) {
        const QString mode = QStringLiteral("%1x%2")
                .arg(display.nativeSize.width()).arg(display.nativeSize.height());
        if (!qualifiedVirtualModes().contains(mode)) {
            if (error != nullptr) {
                *error = QStringLiteral("Client monitor resolution %1 is not a qualified StationConnect virtual mode.")
                        .arg(mode);
            }
            hostLayout.clear();
            virtualModes.clear();
            return false;
        }
        virtualModes.append(mode);
    }
    hostLayout = displays.size() == 1 ? QString::fromLatin1(SingleHostLayout) :
                                       QString::fromLatin1(DualHorizontalHostLayout);
    return true;
}
