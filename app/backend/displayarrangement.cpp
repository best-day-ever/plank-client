#include "displayarrangement.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>

namespace DisplayArrangement {

namespace {

bool strictInteger(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(minimum) ||
            number > double(maximum)) return false;
    out = qint64(number);
    return true;
}

bool strictInt(const QJsonObject& object, const char* key, int minimum, int maximum, int& out)
{
    qint64 value = 0;
    if (!strictInteger(object.value(QLatin1String(key)), minimum, maximum, value)) return false;
    out = int(value);
    return true;
}

bool sizeObject(const QJsonValue& value, QSize& size)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    int width = 0;
    int height = 0;
    if (!strictInt(object, "width", 1, 65536, width) || !strictInt(object, "height", 1, 65536, height)) return false;
    size = QSize(width, height);
    return true;
}

QString sizeText(const QSize& size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QJsonObject sizeJson(const QSize& size)
{
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}

bool isHex64(const QString& text)
{
    if (text.size() != 64) return false;
    for (const QChar c : text) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

bool fail(QString* error, const char* message)
{
    if (error != nullptr) *error = QString::fromLatin1(message);
    return false;
}

// max(W/cw, H/ch) compared exactly: a/b < c/d as cross products.
struct Downscale
{
    qint64 numerator;
    qint64 denominator;
};

Downscale downscale(const QSize& size, const QSize& carrier)
{
    const qint64 widthRatio = qint64(size.width()) * carrier.height();
    const qint64 heightRatio = qint64(size.height()) * carrier.width();
    return {qMax(widthRatio, heightRatio), qint64(carrier.width()) * carrier.height()};
}

int compareDownscale(const Downscale& a, const Downscale& b)
{
    // Numerators are at most 8192*8192*... well inside 64 bits after one product.
    const __int128 left = __int128(a.numerator) * b.denominator;
    const __int128 right = __int128(b.numerator) * a.denominator;
    return left < right ? -1 : (left > right ? 1 : 0);
}

}

QString preferenceName(Preference preference)
{
    switch (preference) {
    case Preference::Physical: return QStringLiteral("physical");
    case Preference::Virtual: return QStringLiteral("virtual");
    case Preference::Auto: default: return QStringLiteral("auto");
    }
}

bool preferenceFromName(const QString& name, Preference& preference)
{
    if (name == QLatin1String("auto")) preference = Preference::Auto;
    else if (name == QLatin1String("physical")) preference = Preference::Physical;
    else if (name == QLatin1String("virtual")) preference = Preference::Virtual;
    else return false;
    return true;
}

QString backingName(Backing backing)
{
    switch (backing) {
    case Backing::Physical: return QStringLiteral("physical");
    case Backing::PhysicalViewport: return QStringLiteral("physical-viewport");
    case Backing::Virtual: return QStringLiteral("virtual");
    case Backing::None: default: return QString();
    }
}

bool backingFromName(const QString& name, Backing& backing)
{
    if (name == QLatin1String("physical")) backing = Backing::Physical;
    else if (name == QLatin1String("physical-viewport")) backing = Backing::PhysicalViewport;
    else if (name == QLatin1String("virtual")) backing = Backing::Virtual;
    else return false;
    return true;
}

QStringList virtualPool()
{
    return {QStringLiteral("1024x2160"), QStringLiteral("1280x2160"), QStringLiteral("1920x1080"),
            QStringLiteral("1920x1200"), QStringLiteral("2560x1440"), QStringLiteral("2560x1600"),
            QStringLiteral("2560x2160"), QStringLiteral("3024x1890"), QStringLiteral("3440x1440"),
            QStringLiteral("3840x1600"), QStringLiteral("3840x2160"), QStringLiteral("4096x2160"),
            QStringLiteral("5120x2160")};
}

QSize parseSize(const QString& text)
{
    const int separator = text.indexOf(QLatin1Char('x'));
    if (separator <= 0 || separator == text.size() - 1 || text.size() > 11) return QSize();
    bool widthOk = false;
    bool heightOk = false;
    const int width = text.left(separator).toInt(&widthOk);
    const int height = text.mid(separator + 1).toInt(&heightOk);
    if (!widthOk || !heightOk || width <= 0 || height <= 0) return QSize();
    const QSize size(width, height);
    return sizeText(size) == text ? size : QSize();
}

bool Capabilities::fromJson(const QJsonObject& object, Capabilities& result, QString* error)
{
    Capabilities parsed;
    static const QStringList keys {
        QStringLiteral("version"), QStringLiteral("fingerprint"), QStringLiteral("max_outputs"),
        QStringLiteral("virtual_heads"), QStringLiteral("max_canvas"), QStringLiteral("packed_capture"),
        QStringLiteral("output_limits"), QStringLiteral("refresh_millihz"), QStringLiteral("physical_outputs"),
        QStringLiteral("encoding_limits")};
    for (const QString& key : keys) {
        if (!object.contains(key)) return fail(error, "Display capabilities lack a required field");
    }
    if (!strictInt(object, "version", 1, 1, parsed.version)) {
        return fail(error, "Unsupported display capabilities version");
    }
    parsed.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
    if (!object.value(QStringLiteral("fingerprint")).isString() || parsed.fingerprint.isEmpty() ||
            parsed.fingerprint.size() > 128) {
        return fail(error, "Invalid display capabilities fingerprint");
    }
    if (!strictInt(object, "max_outputs", 1, MaximumEntries, parsed.maxOutputs) ||
            !strictInt(object, "virtual_heads", 0, 3, parsed.virtualHeads) ||
            !sizeObject(object.value(QStringLiteral("max_canvas")), parsed.maxCanvas) ||
            !object.value(QStringLiteral("packed_capture")).isBool()) {
        return fail(error, "Invalid display capabilities limits");
    }
    parsed.packedCapture = object.value(QStringLiteral("packed_capture")).toBool();
    const QJsonValue limitsValue = object.value(QStringLiteral("output_limits"));
    if (!limitsValue.isObject()) return fail(error, "Invalid display output limits");
    const QJsonObject limits = limitsValue.toObject();
    int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    qint64 maxPixels = 0;
    if (!strictInt(limits, "min_width", 1, 65536, minWidth) || !strictInt(limits, "min_height", 1, 65536, minHeight) ||
            !strictInt(limits, "max_width", 1, 65536, maxWidth) ||
            !strictInt(limits, "max_height", 1, 65536, maxHeight) ||
            !strictInteger(limits.value(QStringLiteral("max_pixels")), 1, qint64(65536) * 65536, maxPixels) ||
            minWidth > maxWidth || minHeight > maxHeight) {
        return fail(error, "Invalid display output limits");
    }
    parsed.minOutput = QSize(minWidth, minHeight);
    parsed.maxOutput = QSize(maxWidth, maxHeight);
    parsed.maxPixels = maxPixels;
    const QJsonValue refresh = object.value(QStringLiteral("refresh_millihz"));
    if (!refresh.isArray() || refresh.toArray().isEmpty()) return fail(error, "Invalid display refresh rates");
    for (const QJsonValue& value : refresh.toArray()) {
        qint64 rate = 0;
        if (!strictInteger(value, 1000, 1000000, rate)) return fail(error, "Invalid display refresh rate");
        parsed.refreshMillihz.append(int(rate));
    }
    const QJsonValue outputs = object.value(QStringLiteral("physical_outputs"));
    if (!outputs.isArray() || outputs.toArray().size() > MaximumEntries) {
        return fail(error, "Invalid physical output list");
    }
    for (const QJsonValue& value : outputs.toArray()) {
        if (!value.isObject()) return fail(error, "Invalid physical output");
        const QJsonObject entry = value.toObject();
        PhysicalOutput output;
        output.id = entry.value(QStringLiteral("id")).toString();
        output.name = entry.value(QStringLiteral("name")).toString();
        output.displayName = entry.value(QStringLiteral("display_name")).toString();
        output.edidSha256 = entry.value(QStringLiteral("edid_sha256")).toString();
        output.preferred = parseSize(entry.value(QStringLiteral("preferred")).toString());
        if (output.id.isEmpty() || output.id.size() > 128 || output.name.isEmpty() || output.name.size() > 128 ||
                !entry.value(QStringLiteral("display_name")).isString() || output.displayName.size() > 128 ||
                !entry.value(QStringLiteral("edid_sha256")).isString() ||
                (!output.edidSha256.isEmpty() && !isHex64(output.edidSha256)) ||
                !output.preferred.isValid() || !entry.value(QStringLiteral("modes")).isArray()) {
            return fail(error, "Invalid physical output");
        }
        for (const PhysicalOutput& other : std::as_const(parsed.physicalOutputs)) {
            if (other.id == output.id) return fail(error, "Duplicate physical output");
        }
        for (const QJsonValue& mode : entry.value(QStringLiteral("modes")).toArray()) {
            const QSize size = parseSize(mode.toString());
            if (!mode.isString() || !size.isValid() || output.modes.contains(size)) {
                return fail(error, "Invalid physical output mode");
            }
            output.modes.append(size);
        }
        parsed.physicalOutputs.append(output);
    }
    if (parsed.maxOutputs > parsed.physicalOutputs.size() + parsed.virtualHeads) {
        return fail(error, "Display capabilities promise more outputs than the host has");
    }
    const QJsonValue encoding = object.value(QStringLiteral("encoding_limits"));
    if (!encoding.isObject()) return fail(error, "Invalid encoding limits");
    const QJsonObject encodingObject = encoding.toObject();
    for (auto entry = encodingObject.begin(); entry != encodingObject.end(); ++entry) {
        if (!entry.value().isObject() || entry.key().isEmpty() || entry.key().size() > 64) {
            return fail(error, "Invalid encoding limit");
        }
        const QJsonObject limit = entry.value().toObject();
        int width = 0, height = 0, qualifiedWidth = 0, qualifiedHeight = 0;
        if (!strictInt(limit, "width", 1, 65536, width) || !strictInt(limit, "height", 1, 65536, height) ||
                !strictInt(limit, "qualified_width", 0, width, qualifiedWidth) ||
                !strictInt(limit, "qualified_height", 0, height, qualifiedHeight)) {
            return fail(error, "Invalid encoding limit");
        }
        parsed.encodingLimits.insert(entry.key(), {QSize(width, height), QSize(qualifiedWidth, qualifiedHeight)});
    }
    parsed.valid = true;
    result = parsed;
    return true;
}

QJsonObject Capabilities::toJson() const
{
    if (!valid) return {};
    QJsonArray refresh;
    for (const int rate : refreshMillihz) refresh.append(rate);
    QJsonArray outputs;
    for (const PhysicalOutput& output : physicalOutputs) {
        QJsonArray modes;
        for (const QSize& mode : output.modes) modes.append(sizeText(mode));
        outputs.append(QJsonObject {
            {QStringLiteral("id"), output.id}, {QStringLiteral("name"), output.name},
            {QStringLiteral("display_name"), output.displayName}, {QStringLiteral("edid_sha256"), output.edidSha256},
            {QStringLiteral("preferred"), sizeText(output.preferred)}, {QStringLiteral("modes"), modes}});
    }
    QJsonObject encoding;
    for (auto entry = encodingLimits.begin(); entry != encodingLimits.end(); ++entry) {
        encoding.insert(entry.key(), QJsonObject {
            {QStringLiteral("width"), entry.value().maximum.width()},
            {QStringLiteral("height"), entry.value().maximum.height()},
            {QStringLiteral("qualified_width"), entry.value().qualified.width()},
            {QStringLiteral("qualified_height"), entry.value().qualified.height()}});
    }
    return {
        {QStringLiteral("version"), version},
        {QStringLiteral("fingerprint"), fingerprint},
        {QStringLiteral("max_outputs"), maxOutputs},
        {QStringLiteral("virtual_heads"), virtualHeads},
        {QStringLiteral("max_canvas"), sizeJson(maxCanvas)},
        {QStringLiteral("packed_capture"), packedCapture},
        {QStringLiteral("output_limits"), QJsonObject {
             {QStringLiteral("min_width"), minOutput.width()}, {QStringLiteral("min_height"), minOutput.height()},
             {QStringLiteral("max_width"), maxOutput.width()}, {QStringLiteral("max_height"), maxOutput.height()},
             {QStringLiteral("max_pixels"), double(maxPixels)}}},
        {QStringLiteral("refresh_millihz"), refresh},
        {QStringLiteral("physical_outputs"), outputs},
        {QStringLiteral("encoding_limits"), encoding},
    };
}

Capabilities Capabilities::fleetDefault()
{
    Capabilities caps;
    caps.valid = true;
    caps.version = 1;
    caps.fingerprint = QStringLiteral("fleet-default");
    caps.maxOutputs = 4;
    caps.virtualHeads = 3;
    // X screen Virtual 16384x8192 bounded by the NVENC maximum until the
    // host packs its capture.
    caps.maxCanvas = QSize(8192, 8192);
    caps.minOutput = QSize(640, 480);
    // A 2:1 ViewPortIn downscale over the largest carrier (5120x2160).
    caps.maxOutput = QSize(8192, 4320);
    caps.maxPixels = 33177600;
    caps.refreshMillihz = {60000};
    PhysicalOutput dongle;
    dongle.id = QStringLiteral("x11:HDMI-0");
    dongle.name = QStringLiteral("HDMI-0");
    dongle.displayName = QStringLiteral("MEC-O-3-H");
    dongle.preferred = QSize(3840, 2160);
    // The dummy plug's 60 Hz modes (59.9-60.1 Hz; its 800x600 and 1440x900 are not).
    dongle.modes = {QSize(4096, 2160), QSize(3840, 2160), QSize(1920, 1080), QSize(1280, 1024),
                    QSize(1280, 720), QSize(1024, 768), QSize(720, 480), QSize(640, 480)};
    caps.physicalOutputs = {dongle};
    caps.encodingLimits.insert(QStringLiteral("hevc-10-444-nvenc"), {QSize(8192, 8192), QSize(7680, 4320)});
    caps.encodingLimits.insert(QStringLiteral("h264-8-444-nvenc"), {QSize(4096, 4096), QSize(4096, 2160)});
    caps.encodingLimits.insert(QStringLiteral("hevc-10-420-nvenc"), {QSize(8192, 8192), QSize(7680, 4320)});
    caps.encodingLimits.insert(QStringLiteral("h264-8-420-nvenc"), {QSize(4096, 4096), QSize(4096, 2160)});
    return caps;
}

QString serialize(const QVector<Entry>& entries)
{
    QStringList parts;
    for (const Entry& entry : entries) {
        parts.append(QStringLiteral("%1x%2+%3+%4:%5").arg(entry.rect.width()).arg(entry.rect.height())
                     .arg(entry.rect.x()).arg(entry.rect.y()).arg(preferenceName(entry.preference)));
    }
    return QStringLiteral("1:") + parts.join(QLatin1Char(','));
}

QSize boundingSize(const QVector<Entry>& entries)
{
    int width = 0;
    int height = 0;
    for (const Entry& entry : entries) {
        width = qMax(width, entry.rect.x() + entry.rect.width());
        height = qMax(height, entry.rect.y() + entry.rect.height());
    }
    return QSize(width, height);
}

namespace {

// One entry as parsed: 64-bit so that every value a canonical request can
// carry (up to a signed 32-bit maximum) survives sums like x + width.
struct RawEntry
{
    qint64 width = 0;
    qint64 height = 0;
    qint64 x = 0;
    qint64 y = 0;
    Preference preference = Preference::Auto;
};

QString serializeRaw(const QVector<RawEntry>& entries)
{
    QStringList parts;
    for (const RawEntry& entry : entries) {
        parts.append(QStringLiteral("%1x%2+%3+%4:%5").arg(entry.width).arg(entry.height)
                     .arg(entry.x).arg(entry.y).arg(preferenceName(entry.preference)));
    }
    return QStringLiteral("1:") + parts.join(QLatin1Char(','));
}

// Checks 1-6.
QString parseRaw(const QString& request, QVector<RawEntry>& entries)
{
    entries.clear();
    // 1. Length.
    const QByteArray bytes = request.toUtf8();
    if (bytes.isEmpty() || bytes.size() > MaximumRequestBytes) return QStringLiteral("malformed");
    // 2. Grammar: "1:" entry ("," entry)*, entry := D "x" D "+" D "+" D ":" preference.
    if (!bytes.startsWith("1:")) return QStringLiteral("malformed");
    struct Raw
    {
        QByteArray numbers[4];
        QByteArray preference;
    };
    QVector<Raw> raws;
    int position = 2;
    const auto digits = [&bytes, &position](QByteArray& out) {
        const int start = position;
        while (position < bytes.size() && bytes.at(position) >= '0' && bytes.at(position) <= '9') ++position;
        out = bytes.mid(start, position - start);
        return !out.isEmpty();
    };
    const auto expect = [&bytes, &position](char c) {
        if (position >= bytes.size() || bytes.at(position) != c) return false;
        ++position;
        return true;
    };
    while (true) {
        Raw raw;
        if (!digits(raw.numbers[0]) || !expect('x') || !digits(raw.numbers[1]) || !expect('+') ||
                !digits(raw.numbers[2]) || !expect('+') || !digits(raw.numbers[3]) || !expect(':')) {
            return QStringLiteral("malformed");
        }
        const int start = position;
        while (position < bytes.size() && bytes.at(position) >= 'a' && bytes.at(position) <= 'z') ++position;
        raw.preference = bytes.mid(start, position - start);
        if (raw.preference != "auto" && raw.preference != "physical" && raw.preference != "virtual") {
            return QStringLiteral("malformed");
        }
        raws.append(raw);
        if (position == bytes.size()) break;
        if (!expect(',')) return QStringLiteral("malformed");
    }
    // 3. Count.
    if (raws.size() > MaximumEntries) return QStringLiteral("too_many_displays");
    // 4. Canonical form: no leading zeros (a lone "0" is canonical), and a
    // value that fits a signed 32-bit integer (anything larger has no
    // canonical form).
    QVector<RawEntry> parsed;
    for (const Raw& raw : std::as_const(raws)) {
        qint64 values[4] = {};
        for (int index = 0; index < 4; ++index) {
            const QByteArray& number = raw.numbers[index];
            if (number.size() > 1 && number.startsWith('0')) return QStringLiteral("not_canonical");
            if (number.size() > 10 || number.toLongLong() > std::numeric_limits<qint32>::max()) {
                return QStringLiteral("not_canonical");
            }
            values[index] = number.toLongLong();
        }
        RawEntry entry;
        entry.width = values[0];
        entry.height = values[1];
        entry.x = values[2];
        entry.y = values[3];
        preferenceFromName(QString::fromLatin1(raw.preference), entry.preference);
        parsed.append(entry);
    }
    if (serializeRaw(parsed) != request) return QStringLiteral("not_canonical");
    // 5. Odd values.
    for (const RawEntry& entry : std::as_const(parsed)) {
        if ((entry.width | entry.height | entry.x | entry.y) & 1) return QStringLiteral("odd_value");
    }
    // 6. Origin.
    qint64 minX = std::numeric_limits<qint64>::max();
    qint64 minY = std::numeric_limits<qint64>::max();
    for (const RawEntry& entry : std::as_const(parsed)) {
        minX = qMin(minX, entry.x);
        minY = qMin(minY, entry.y);
    }
    if (minX != 0 || minY != 0) return QStringLiteral("origin_not_zero");
    entries = parsed;
    return QString();
}

// Checks 7-10.
QString checkRaw(const QVector<RawEntry>& entries, const Capabilities& caps)
{
    // 7. Per-entry limits, in entry order.
    for (const RawEntry& entry : entries) {
        if (entry.width < caps.minOutput.width() || entry.height < caps.minOutput.height()) {
            return QStringLiteral("output_too_small");
        }
        if (entry.width > caps.maxOutput.width() || entry.height > caps.maxOutput.height() ||
                entry.width * entry.height > caps.maxPixels) {
            return QStringLiteral("output_too_large");
        }
    }
    // 8. Overlap (touching edges and gaps are fine).
    for (int a = 0; a < entries.size(); ++a) {
        for (int b = a + 1; b < entries.size(); ++b) {
            const RawEntry& first = entries.at(a);
            const RawEntry& second = entries.at(b);
            if (first.x < second.x + second.width && second.x < first.x + first.width &&
                    first.y < second.y + second.height && second.y < first.y + first.height) {
                return QStringLiteral("overlap");
            }
        }
    }
    // 9. Canvas.
    qint64 canvasWidth = 0;
    qint64 canvasHeight = 0;
    for (const RawEntry& entry : entries) {
        canvasWidth = qMax(canvasWidth, entry.x + entry.width);
        canvasHeight = qMax(canvasHeight, entry.y + entry.height);
    }
    if (canvasWidth > caps.maxCanvas.width() || canvasHeight > caps.maxCanvas.height()) {
        return QStringLiteral("canvas_too_large");
    }
    // 10. Outputs.
    if (entries.size() > caps.maxOutputs) return QStringLiteral("too_many_displays");
    return QString();
}

QVector<Entry> toEntries(const QVector<RawEntry>& raws)
{
    QVector<Entry> entries;
    for (const RawEntry& raw : raws) {
        Entry entry;
        entry.rect = QRect(int(raw.x), int(raw.y), int(raw.width), int(raw.height));
        entry.preference = raw.preference;
        entries.append(entry);
    }
    return entries;
}

}

QString parse(const QString& request, QVector<Entry>& entries)
{
    entries.clear();
    QVector<RawEntry> raws;
    const QString error = parseRaw(request, raws);
    if (!error.isEmpty()) return error;
    for (const RawEntry& raw : std::as_const(raws)) {
        // A rectangle must end within int: such a desktop fits no canvas anyway.
        if (raw.x + raw.width > std::numeric_limits<int>::max() ||
                raw.y + raw.height > std::numeric_limits<int>::max()) {
            return QStringLiteral("canvas_too_large");
        }
    }
    entries = toEntries(raws);
    return QString();
}

QString validate(const QString& request, const Capabilities& caps, QVector<Entry>* out)
{
    QVector<RawEntry> raws;
    QString error = parseRaw(request, raws);
    if (error.isEmpty()) error = checkRaw(raws, caps);
    if (!error.isEmpty()) return error;
    // Every value now fits the canvas, so QRect cannot overflow.
    if (out != nullptr) *out = toEntries(raws);
    return QString();
}

QSize carrierFor(const QSize& size, const QStringList& pool)
{
    QVector<QSize> modes;
    for (const QString& mode : pool) modes.append(parseSize(mode));
    for (const QSize& mode : std::as_const(modes)) {
        if (mode == size) return mode;
    }
    QSize covering;
    for (const QSize& mode : std::as_const(modes)) {
        if (mode.width() >= size.width() && mode.height() >= size.height() &&
                (!covering.isValid() ||
                 qint64(mode.width()) * mode.height() < qint64(covering.width()) * covering.height())) {
            covering = mode;
        }
    }
    if (covering.isValid()) return covering;
    QSize best;
    Downscale bestScale {0, 1};
    for (const QSize& mode : std::as_const(modes)) {
        if (!mode.isValid()) continue;
        const Downscale scale = downscale(size, mode);
        const int order = best.isValid() ? compareDownscale(scale, bestScale) : -1;
        if (order < 0 || (order == 0 && qint64(mode.width()) * mode.height() >
                                        qint64(best.width()) * best.height())) {
            best = mode;
            bestScale = scale;
        }
    }
    return best;
}

Resolution resolve(const QString& request, const Capabilities& caps)
{
    Resolution result;
    QVector<Entry> entries;
    result.error = validate(request, caps, &entries);
    if (!result.error.isEmpty()) return result;
    result.outputs.resize(entries.size());
    QVector<bool> physicalUsed(caps.physicalOutputs.size(), false);
    int nextHead = 1;
    // 1. Exact physical mode, unless the entry wants a virtual display.
    for (int index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries.at(index);
        if (entry.preference == Preference::Virtual) continue;
        for (int output = 0; output < caps.physicalOutputs.size(); ++output) {
            if (physicalUsed.at(output) || !caps.physicalOutputs.at(output).modes.contains(entry.rect.size())) continue;
            physicalUsed[output] = true;
            result.outputs[index] = {Backing::Physical, caps.physicalOutputs.at(output).id, entry.rect.size(), 0,
                                     QSize(), entry.rect};
            break;
        }
    }
    // 2. A virtual head, unless the entry wants a physical output.
    for (int index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries.at(index);
        if (result.outputs.at(index).backing != Backing::None || entry.preference == Preference::Physical) continue;
        if (nextHead > caps.virtualHeads) break;
        result.outputs[index] = {Backing::Virtual, QString(), QSize(), nextHead++, carrierFor(entry.rect.size()),
                                 entry.rect};
    }
    // 3. A physical output through ViewPortIn over its preferred mode.
    for (int index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries.at(index);
        if (result.outputs.at(index).backing != Backing::None || entry.preference == Preference::Virtual) continue;
        for (int output = 0; output < caps.physicalOutputs.size(); ++output) {
            if (physicalUsed.at(output)) continue;
            physicalUsed[output] = true;
            result.outputs[index] = {Backing::PhysicalViewport, caps.physicalOutputs.at(output).id,
                                     caps.physicalOutputs.at(output).preferred, 0, QSize(), entry.rect};
            break;
        }
    }
    // 4. Anything left fails the request.
    for (int index = 0; index < entries.size(); ++index) {
        if (result.outputs.at(index).backing != Backing::None) continue;
        switch (entries.at(index).preference) {
        case Preference::Physical: result.error = QStringLiteral("no_physical_output"); break;
        case Preference::Virtual: result.error = QStringLiteral("no_virtual_output"); break;
        case Preference::Auto: result.error = QStringLiteral("too_many_displays"); break;
        }
        result.outputs.clear();
        return result;
    }
    // 5. Unassigned physical outputs are switched off.
    for (int output = 0; output < caps.physicalOutputs.size(); ++output) {
        if (!physicalUsed.at(output)) result.hiddenPhysical.append(caps.physicalOutputs.at(output).id);
    }
    result.desktop = boundingSize(entries);
    result.ok = true;
    return result;
}

}
