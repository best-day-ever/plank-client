#include "outputtopology.h"

#include <QJsonArray>

const char* NvOutputTopology::SingleOutputMode = "single-output";
const char* NvOutputTopology::ScaledSpanMode = "scaled-span";

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
}

bool NvOutputTopology::fromJson(const QJsonObject& object,
                                NvOutputTopology& topology, QString* error)
{
    NvOutputTopology parsed;
    if (!requireInteger(object, "schema_version", parsed.schemaVersion) ||
            parsed.schemaVersion != ProtocolVersion ||
            !requireInteger(object, "feature_flags", parsed.featureFlags) ||
            (parsed.featureFlags & (OutputTopologyFeature | SelectedOutputFeature |
                                    UnifiedAbsoluteInputFeature)) !=
                (OutputTopologyFeature | SelectedOutputFeature |
                 UnifiedAbsoluteInputFeature) ||
            !object.value("generation").isString() ||
            !object.value("desktop").isObject() ||
            !object.value("outputs").isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unsupported or malformed output topology header");
        }
        return false;
    }
    parsed.generation = object.value("generation").toString();
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
                !entry.value("primary").isBool() || output.width <= 0 ||
                output.height <= 0 || parsed.contains(output.id)) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid or duplicate output entry");
            }
            return false;
        }
        output.primary = entry.value("primary").toBool();
        parsed.outputs.append(output);
    }
    if (parsed.outputs.isEmpty()) {
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
        });
    }
    return QJsonObject {
        {"schema_version", schemaVersion},
        {"feature_flags", featureFlags},
        {"generation", generation},
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

QString NvOutputTopology::selectOutput(QString persistedId) const
{
    if (contains(persistedId)) {
        return persistedId;
    }
    for (const NvOutput& output : outputs) {
        if (output.primary) {
            return output.id;
        }
    }
    return outputs.isEmpty() ? QString() : outputs.first().id;
}

bool NvOutputTopology::supportsScaledSpan() const
{
    return (featureFlags & ScaledSpanFeature) != 0 && outputs.size() > 1;
}

QString NvOutputTopology::selectDisplayMode(QString persistedMode) const
{
    if (persistedMode == ScaledSpanMode && supportsScaledSpan()) {
        return persistedMode;
    }
    if (persistedMode == SingleOutputMode) {
        return persistedMode;
    }
    return supportsScaledSpan() ? QString(ScaledSpanMode) : QString(SingleOutputMode);
}
