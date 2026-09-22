#pragma once

// The display layout planner: the client's monitors (ClientDisplayProbe) plus
// the user's intent for that monitor set (DisplayProfile) plus what the
// workstation can do (display_capabilities, feature flags) give one plan:
//
//   - which monitors the workstation shows, at which size, where;
//   - the canonical plankDisplayArrangement request for hosts with the display
//     arrangement extension (0x8000000), or today's single/dual-horizontal
//     layout and modes for older Linux hosts, or the Mac-host desktop mode;
//   - the backing each display is expected to get (a preview: the host decides);
//   - warnings, each with a one-click action where there is one.
//
// Pure (Qt Core only): the wizard, the connect-time check and the streaming
// Session all run the same function on the same inputs, so what the wizard
// shows is what a connect does.

#include "displayarrangement.h"
#include "displayprofile.h"
#include "outputtopology.h"

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DisplayPlanner {

// Seams between neighbouring desktops overlap by at least this many pixels
// so the pointer can always cross.
static constexpr int MinimumSeam = 64;

struct HostInfo
{
    bool known = false;
    int platform = 0;      // RemoteStreamSetup::Platform: 0 unknown, 1 Linux, 2 macOS
    int featureFlags = 0;
    // The host's display_capabilities (valid when it publishes them).
    DisplayArrangement::Capabilities capabilities;
    // The encoding mode the stream will use ("hevc-10-444-nvenc"); empty when unknown.
    QString encodingMode;

    bool isLinux() const { return known && platform == 1; }
    bool isMac() const { return known && platform == 2; }
    // Not yet connected: planned with the fleet's defaults, labelled "expected".
    bool unknownHost() const { return !known || platform == 0; }
    bool supportsArrangement() const
    {
        return unknownHost() ||
                (isLinux() && (featureFlags & DisplayArrangement::Feature) != 0 && capabilities.valid);
    }
};

struct Limits
{
    // Largest frame this client decodes in hardware for the stream's profile;
    // invalid while unknown.
    QSize decoderMaximum;
    // macOS "Displays have separate Spaces": without it one app cannot put a
    // fullscreen window on every display.
    bool separateSpaces = true;
};

struct SizeOption
{
    QString value; // "exact", "looks-like", "preset:WxH", "custom"
    QString label;
    QSize size;
};

struct Output
{
    QString key;
    QString name;
    bool builtIn = false;
    bool notch = false;
    quint32 platformId = 0;
    QRect clientBounds;        // logical client desktop (points)
    QSize exactSize;           // pixel-exact (notch-safe fullscreen viewport)
    QSize looksLikeSize;       // "looks like"; invalid on 1x displays
    QString sizeChoice;        // the profile's choice ("exact", "looks-like", "preset:WxH", "custom:WxH")
    QString preference;        // auto | physical | virtual
    bool on = true;            // the user's toggle
    bool included = false;     // on, and shown by the workstation
    bool primary = false;
    QSize size;                // planned workstation desktop size
    QPoint position;           // planned workstation desktop position
    int arrangementIndex = -1; // entry number in the request
    bool scaled = false;       // stepped down to fit the limits
    QString badge;             // exact | looks-like | preset | custom | scaled | closest
    DisplayArrangement::Backing backing = DisplayArrangement::Backing::None; // expected
    QString backingOutput;     // physical output id for physical backings
    QVector<SizeOption> sizeOptions;
};

struct Warning
{
    QString code;        // codec, decoder, too-many, bitrate, mixed-dpi, low-refresh, spaces, old-host,
                         // unqualified, arrangement
    QString text;        // user-facing
    QString action;      // use-hevc, match-text-size, choose-screens, update-host, open-spaces, "" for none
    QString actionLabel;
    QString key;         // the monitor it is about, when it is about one
};

struct Plan
{
    bool ok = false;
    QString error;             // user-facing, when !ok
    QVector<Output> outputs;   // every probed monitor, in probe order
    QSize canvas;              // the workstation desktop's bounding box
    // Display arrangement hosts (and the preview before the first connect).
    QString arrangement;       // canonical plankDisplayArrangement
    bool backingExpected = false; // backing previewed from defaults, not this workstation
    // Linux hosts without the extension: today's layouts.
    bool legacy = false;
    QString legacyHostLayout;
    QStringList legacyModes;
    bool legacyFitted = false;
    // macOS hosts: the Match client desktop mode.
    QString macMode;
    int macScale = 1;
    QString presentation;      // windows | single, after the limits
    QString scaling;           // native | fit
    QVector<Warning> warnings;

    const Output* find(const QString& key) const
    {
        for (const Output& output : outputs) {
            if (output.key == key) return &output;
        }
        return nullptr;
    }
    int includedCount() const
    {
        int count = 0;
        for (const Output& output : outputs) count += output.included ? 1 : 0;
        return count;
    }
};

// The proposal for a new monitor set: every monitor on at its pixel-exact
// size, the menu-bar display primary, one window per display.
DisplayProfile::Profile proposal(const QVector<NvClientDisplay>& displays);

Plan plan(const QVector<NvClientDisplay>& displays, const DisplayProfile::Profile& profile, const HostInfo& host,
          const Limits& limits = Limits());

// A user-facing sentence for a PlankDisplayArrangementError code (the
// host's 400/409 answers and the preview's own checks).
QString errorText(const QString& code, const DisplayArrangement::Capabilities& capabilities);

// Building blocks, exposed for the tests.
QSize exactSize(const NvClientDisplay& display);
QSize looksLikeSize(const NvClientDisplay& display);
QSize evenSize(const QSize& size);

// Places desktops of the given pixel sizes like the client's logical
// rectangles: primary at the origin, neighbours across shared edges, at
// least MinimumSeam pixels of every seam shared, no overlap, even values,
// normalised to the origin.
QVector<QPoint> arrange(const QVector<QRect>& logical, const QVector<QSize>& pixels, int primary);

}
