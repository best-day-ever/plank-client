#pragma once

// Display arrangement extension (Linux Host, feature 0x8000000): the client
// half of protocol/output-topology.md "Display arrangement extension".
//
//   Capabilities  the host's display_capabilities (topology document), also
//                 cached per workstation to preview a layout before it connects
//   serialize     the canonical launch request, plankDisplayArrangement
//   validate      the request checks in the contract's order
//   resolve       the backing rule. The host is the authority; the client
//                 runs this port only to preview what the host will do.
//
// Pure (Qt Core only) so the planner tests exercise it against the shared
// vectors in tests/protocol/display-arrangement-v1.json.

#include <QJsonObject>
#include <QMap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DisplayArrangement {

static constexpr int Feature = 0x8000000;
static constexpr int MaximumEntries = 4;
static constexpr int MaximumRequestBytes = 160;

enum class Preference { Auto, Physical, Virtual };
enum class Backing { None, Physical, PhysicalViewport, Virtual };

QString preferenceName(Preference preference);
bool preferenceFromName(const QString& name, Preference& preference);
QString backingName(Backing backing);
bool backingFromName(const QString& name, Backing& backing);

struct Entry
{
    QRect rect;
    Preference preference = Preference::Auto;
};

struct PhysicalOutput
{
    QString id;
    QString name;
    QString displayName;
    QString edidSha256;
    QSize preferred;
    QVector<QSize> modes;
};

struct EncodingLimit
{
    QSize maximum;
    QSize qualified;
};

struct Capabilities
{
    bool valid = false;
    int version = 0;
    QString fingerprint;
    int maxOutputs = 0;
    int virtualHeads = 0;
    QSize maxCanvas;
    bool packedCapture = false;
    QSize minOutput;
    QSize maxOutput;
    qint64 maxPixels = 0;
    QVector<int> refreshMillihz;
    QVector<PhysicalOutput> physicalOutputs;
    QMap<QString, EncodingLimit> encodingLimits;

    // Strict: every field of version 1, in range, consistent.
    static bool fromJson(const QJsonObject& object, Capabilities& capabilities, QString* error = nullptr);
    QJsonObject toJson() const;
    bool operator==(const Capabilities& other) const { return toJson() == other.toJson(); }

    // What the fleet looks like before a workstation has been seen: the
    // hybrid policy with one HDMI dummy plug (the shared vectors'
    // "fleet-hybrid"). Previews based on it are labelled "expected".
    static Capabilities fleetDefault();
};

struct ResolvedOutput
{
    Backing backing = Backing::None;
    QString output;   // physical output id (physical, physical-viewport)
    QSize mode;       // physical mode driven (physical: W×H, viewport: preferred)
    int head = 0;     // virtual head, 1-based (virtual)
    QSize carrier;    // virtual carrier timing (virtual)
    QRect rect;       // desktop rectangle
};

struct Resolution
{
    bool ok = false;
    QString error;    // an error code from the contract when !ok
    QVector<ResolvedOutput> outputs; // in request order
    QStringList hiddenPhysical;      // physical outputs switched off for the lease
    QSize desktop;                   // bounding box
};

// The qualified virtual EDID pool, in pool order (the carrier timings).
QStringList virtualPool();
QSize parseSize(const QString& text); // "WxH" canonical, else invalid

// Canonical request: "1:WxH+X+Y:pref,...", entry 0 first.
QString serialize(const QVector<Entry>& entries);

// Checks 1-6 (length, grammar, count, canonical form, odd values, origin);
// returns the error code or an empty string and fills entries.
QString parse(const QString& request, QVector<Entry>& entries);

// Checks 1-10 against the host's capabilities.
QString validate(const QString& request, const Capabilities& capabilities, QVector<Entry>* entries = nullptr);

// Checks 1-10 and the backing rule (11).
Resolution resolve(const QString& request, const Capabilities& capabilities);

// Carrier for a virtual display of this size from the pool (backing rule 2).
QSize carrierFor(const QSize& size, const QStringList& pool = virtualPool());

// Bounding box of the entries' rectangles, from the origin.
QSize boundingSize(const QVector<Entry>& entries);

}
