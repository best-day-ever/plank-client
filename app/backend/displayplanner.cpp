#include "displayplanner.h"

#include "clientdisplayprobe.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace DisplayPlanner {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("DisplayPlanner", text, disambiguation, n);
}

QString sizeLabel(const QSize& size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

qint64 area(const QSize& size)
{
    return qint64(size.width()) * size.height();
}

int evenValue(int value)
{
    return value - (value % 2);
}

enum class Side { Right, Left, Below, Above };

// Whether b sits directly against a on the client desktop, and on which side
// of a. macOS keeps arranged displays touching; allow a point of rounding.
bool touching(const QRect& a, const QRect& b, Side& side)
{
    constexpr int Tolerance = 2;
    const int aRight = a.x() + a.width();
    const int aBottom = a.y() + a.height();
    const int bRight = b.x() + b.width();
    const int bBottom = b.y() + b.height();
    const bool verticalOverlap = a.y() < bBottom && b.y() < aBottom;
    const bool horizontalOverlap = a.x() < bRight && b.x() < aRight;
    if (verticalOverlap && qAbs(b.x() - aRight) <= Tolerance) side = Side::Right;
    else if (verticalOverlap && qAbs(a.x() - bRight) <= Tolerance) side = Side::Left;
    else if (horizontalOverlap && qAbs(b.y() - aBottom) <= Tolerance) side = Side::Below;
    else if (horizontalOverlap && qAbs(a.y() - bBottom) <= Tolerance) side = Side::Above;
    else return false;
    return true;
}

// Breadth-first order from the primary over the client's edge adjacency;
// displays not connected to it follow in index order.
QVector<int> adjacencyOrder(const QVector<QRect>& logical, int primary)
{
    QVector<int> order;
    QVector<bool> seen(logical.size(), false);
    const auto walk = [&](int start) {
        std::queue<int> queue;
        queue.push(start);
        seen[start] = true;
        while (!queue.empty()) {
            const int a = queue.front();
            queue.pop();
            order.append(a);
            for (int b = 0; b < logical.size(); ++b) {
                Side side;
                if (!seen.at(b) && touching(logical.at(a), logical.at(b), side)) {
                    seen[b] = true;
                    queue.push(b);
                }
            }
        }
    };
    if (primary >= 0 && primary < logical.size()) walk(primary);
    for (int index = 0; index < logical.size(); ++index) {
        if (!seen.at(index)) walk(index);
    }
    return order;
}

QSize fitAspect(const QSize& size, double scale)
{
    return evenSize(QSize(qMax(2, int(std::floor(size.width() * scale))),
                          qMax(2, int(std::floor(size.height() * scale)))));
}

bool fitsOutputLimits(const QSize& size, const DisplayArrangement::Capabilities& caps)
{
    return size.width() <= caps.maxOutput.width() && size.height() <= caps.maxOutput.height() &&
            area(size) <= caps.maxPixels;
}

// One step smaller for a display that does not fit: its "looks like" size
// when that is smaller, else the largest known mode of about the same shape
// that is smaller, else three quarters. Never below the host's minimum.
QSize stepDown(const Output& output, const QSize& current, const DisplayArrangement::Capabilities& caps)
{
    if (output.looksLikeSize.isValid() && area(output.looksLikeSize) < area(current) &&
            output.looksLikeSize.width() <= current.width() && output.looksLikeSize.height() <= current.height()) {
        return output.looksLikeSize;
    }
    QVector<QSize> modes;
    for (const QString& mode : DisplayArrangement::virtualPool()) modes.append(DisplayArrangement::parseSize(mode));
    for (const DisplayArrangement::PhysicalOutput& physical : caps.physicalOutputs) {
        for (const QSize& mode : physical.modes) {
            if (!modes.contains(mode)) modes.append(mode);
        }
    }
    const double aspect = double(current.width()) / current.height();
    QSize best;
    for (const QSize& mode : std::as_const(modes)) {
        if (!mode.isValid() || mode.width() > current.width() || mode.height() > current.height() ||
                area(mode) >= area(current) || mode.width() < caps.minOutput.width() ||
                mode.height() < caps.minOutput.height()) {
            continue;
        }
        // About the same shape: within 2% of the aspect ratio.
        if (std::fabs(std::log((double(mode.width()) / mode.height()) / aspect)) > 0.02) continue;
        if (!best.isValid() || area(mode) > area(best)) best = mode;
    }
    if (best.isValid()) return best;
    const QSize smaller = fitAspect(current, 0.75);
    if (smaller.width() < caps.minOutput.width() || smaller.height() < caps.minOutput.height()) return QSize();
    return smaller;
}

QSize chosenSize(const Output& output)
{
    DisplayProfile::MonitorChoice choice;
    if (!DisplayProfile::sizeFromText(output.sizeChoice, choice)) return output.exactSize;
    switch (choice.size) {
    case DisplayProfile::SizeMode::LooksLike:
        return output.looksLikeSize.isValid() ? output.looksLikeSize : output.exactSize;
    case DisplayProfile::SizeMode::Preset:
    case DisplayProfile::SizeMode::Custom:
        return choice.fixedSize;
    case DisplayProfile::SizeMode::Exact:
    default:
        return output.exactSize;
    }
}

QString badgeFor(const Output& output)
{
    if (output.scaled) return QStringLiteral("scaled");
    if (output.sizeChoice.startsWith(QLatin1String("preset:"))) return QStringLiteral("preset");
    if (output.sizeChoice.startsWith(QLatin1String("custom:"))) return QStringLiteral("custom");
    if (output.sizeChoice == QLatin1String("looks-like") && output.looksLikeSize.isValid()) {
        return QStringLiteral("looks-like");
    }
    return QStringLiteral("exact");
}

QVector<SizeOption> sizeOptions(const Output& output, const DisplayArrangement::Capabilities& caps)
{
    QVector<SizeOption> options;
    options.append({QStringLiteral("exact"), tr("%1 (pixel-exact)").arg(sizeLabel(output.exactSize)),
                    output.exactSize});
    if (output.looksLikeSize.isValid()) {
        options.append({QStringLiteral("looks-like"), tr("Looks like %1").arg(sizeLabel(output.looksLikeSize)),
                        output.looksLikeSize});
    }
    QVector<QSize> presets;
    for (const DisplayArrangement::PhysicalOutput& physical : caps.physicalOutputs) {
        for (const QSize& mode : physical.modes) {
            if (!presets.contains(mode) && mode != output.exactSize) presets.append(mode);
        }
    }
    for (const QSize& mode : std::as_const(presets)) {
        options.append({QStringLiteral("preset:%1x%2").arg(mode.width()).arg(mode.height()),
                        tr("%1 (workstation screen mode)").arg(sizeLabel(mode)), mode});
    }
    options.append({QStringLiteral("custom"), tr("Custom…"), QSize()});
    return options;
}

void warn(Plan& plan, const QString& code, const QString& text, const QString& action = QString(),
          const QString& actionLabel = QString(), const QString& key = QString())
{
    plan.warnings.append({code, text, action, actionLabel, key});
}

double pointScale(const NvClientDisplay& display)
{
    const QSize pixels = display.backingSize.isValid() ? display.backingSize : display.nativeSize;
    return display.bounds.width() > 0 && pixels.isValid() ? double(pixels.width()) / display.bounds.width() : 1.0;
}

// Picks the primary: the profile's choice when it is on, else the menu-bar
// display, else the display at the desktop origin, else the first one on.
int primaryIndex(const QVector<Output>& outputs, const QVector<NvClientDisplay>& displays, const QString& preferred)
{
    for (int index = 0; index < outputs.size(); ++index) {
        if (outputs.at(index).on && !preferred.isEmpty() && outputs.at(index).key == preferred) return index;
    }
    for (int index = 0; index < outputs.size(); ++index) {
        if (outputs.at(index).on && displays.at(index).main) return index;
    }
    for (int index = 0; index < outputs.size(); ++index) {
        if (outputs.at(index).on && displays.at(index).bounds.contains(QPoint(0, 0))) return index;
    }
    for (int index = 0; index < outputs.size(); ++index) {
        if (outputs.at(index).on) return index;
    }
    return -1;
}

void commonWarnings(Plan& plan, const QVector<NvClientDisplay>& displays)
{
    // GNOME on X11 has one global text scale: a 2x-sized display next to a
    // 1x-sized one makes text tiny on one of them.
    bool dense = false;
    bool plain = false;
    for (int index = 0; index < plan.outputs.size(); ++index) {
        const Output& output = plan.outputs.at(index);
        if (!output.included) continue;
        const bool pixelExactHiDpi = output.looksLikeSize.isValid() && output.size == output.exactSize &&
                pointScale(displays.at(index)) >= 1.5;
        (pixelExactHiDpi ? dense : plain) = true;
    }
    if (dense && plain) {
        warn(plan, QStringLiteral("mixed-dpi"),
             tr("Text will look much smaller on the Retina screens than on the others: the workstation uses one "
                "text size for all screens."),
             QStringLiteral("match-text-size"), tr("Match text size"));
    }
    for (int index = 0; index < plan.outputs.size(); ++index) {
        const Output& output = plan.outputs.at(index);
        const int refresh = displays.at(index).refreshMillihz;
        if (output.included && refresh > 0 && refresh < 59000) {
            warn(plan, QStringLiteral("low-refresh"),
                 tr("%1 refreshes at %2 Hz; the workstation sends 60 frames per second.")
                     .arg(output.name, QString::number(refresh / 1000.0, 'f', refresh % 1000 ? 1 : 0)),
                 QString(), QString(), output.key);
        }
    }
    const qint64 pixels = area(plan.canvas);
    if (pixels > qint64(3840) * 2160 * 3 / 2) {
        // The HEVC 4:4:4 default spends about 50 Mbps on a 4K desktop.
        const int megabits = int(std::lround(50.0 * pixels / (3840.0 * 2160.0) / 10.0) * 10);
        warn(plan, QStringLiteral("bitrate"),
             tr("This layout is %1 pixels wide in total and needs about %2 Mbps for a sharp picture.")
                 .arg(plan.canvas.width()).arg(megabits));
    }
}

void planMac(Plan& plan, const QVector<NvClientDisplay>& displays, const QString& presentationChoice)
{
    Q_UNUSED(presentationChoice);
    QVector<NvClientDisplay> on;
    QVector<int> indices;
    for (int index = 0; index < plan.outputs.size(); ++index) {
        if (plan.outputs.at(index).on) {
            on.append(displays.at(index));
            indices.append(index);
        }
    }
    QString error;
    int scale = 1;
    const QString mode = NvOutputTopology::resolveMacClientDisplayMode(
                ClientDisplayProbe::macMatchDisplays(on, true), &error, &scale);
    if (mode.isEmpty()) {
        plan.error = error;
        return;
    }
    plan.macMode = mode;
    plan.macScale = scale;
    plan.canvas = NvOutputTopology::macDisplayModeSize(mode);
    QVector<int> order = indices;
    std::sort(order.begin(), order.end(), [&displays](int a, int b) {
        return displays.at(a).bounds.x() < displays.at(b).bounds.x();
    });
    int x = 0;
    for (const int index : std::as_const(order)) {
        Output& output = plan.outputs[index];
        output.included = true;
        const QVector<NvClientDisplay> one = ClientDisplayProbe::macMatchDisplays({displays.at(index)}, true);
        output.size = one.first().backingSize;
        output.position = QPoint(x, 0);
        x += output.size.width();
        output.badge = QStringLiteral("exact");
    }
    plan.presentation = QStringLiteral("single");
    plan.ok = true;
}

void planLegacy(Plan& plan, const QVector<NvClientDisplay>& displays, int primary, const HostInfo& host)
{
    // The primary plus one neighbour on its left or right: older hosts only
    // know one display or two side by side, from the qualified mode list.
    QVector<int> selected {primary};
    int left = -1;
    for (int index = 0; index < plan.outputs.size(); ++index) {
        Side side;
        if (index == primary || !plan.outputs.at(index).on ||
                !touching(displays.at(primary).bounds, displays.at(index).bounds, side)) {
            continue;
        }
        if (side == Side::Right) {
            selected.append(index);
            break;
        }
        if (side == Side::Left && left < 0) left = index;
    }
    if (selected.size() == 1 && left >= 0) selected.append(left);
    std::sort(selected.begin(), selected.end(), [&displays](int a, int b) {
        return displays.at(a).bounds.x() < displays.at(b).bounds.x();
    });
    QVector<NvClientDisplay> chosen;
    for (const int index : std::as_const(selected)) chosen.append(displays.at(index));
    QString error;
    if (!NvOutputTopology::resolveClientDisplayLayout(chosen, plan.legacyHostLayout, plan.legacyModes, &error,
                                                      &plan.legacyFitted,
                                                      NvOutputTopology::virtualModesForHost(host.featureFlags))) {
        plan.error = error;
        return;
    }
    plan.legacy = true;
    int x = 0;
    int height = 0;
    for (int position = 0; position < selected.size(); ++position) {
        Output& output = plan.outputs[selected.at(position)];
        output.included = true;
        output.size = NvOutputTopology::virtualModeSize(plan.legacyModes.at(position));
        output.position = QPoint(x, 0);
        output.badge = output.size == output.exactSize ? QStringLiteral("exact") : QStringLiteral("closest");
        x += output.size.width();
        height = qMax(height, output.size.height());
    }
    plan.canvas = QSize(x, height);
    int on = 0;
    for (const Output& output : std::as_const(plan.outputs)) on += output.on ? 1 : 0;
    if (on > selected.size() || plan.legacyFitted) {
        warn(plan, QStringLiteral("old-host"),
             on > selected.size() ?
                 tr("This workstation's PLANK shows at most two screens side by side, from a fixed list of "
                    "sizes. Update PLANK on the workstation to use every screen at its exact size.") :
                 tr("This workstation's PLANK only offers a fixed list of sizes, so the closest one is used. "
                    "Update PLANK on the workstation for exact sizes."),
             QStringLiteral("update-host"), tr("How to update"));
    }
    plan.ok = true;
}

// Hand-placed positions (advanced setup) for these displays, normalised and
// even; empty when any of them has none or they overlap at these sizes.
QVector<QPoint> manualPositions(const Plan& plan, const QVector<int>& included, const DisplayProfile::Profile& profile)
{
    if (!profile.manual) return {};
    QVector<QRect> rects;
    for (const int index : included) {
        const DisplayProfile::MonitorChoice* choice = profile.find(plan.outputs.at(index).key);
        if (choice == nullptr || !choice->hasPosition) return {};
        rects.append(QRect(QPoint(evenValue(choice->position.x()), evenValue(choice->position.y())),
                           plan.outputs.at(index).size));
    }
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (int a = 0; a < rects.size(); ++a) {
        for (int b = a + 1; b < rects.size(); ++b) {
            if (rects.at(a).intersects(rects.at(b))) return {};
        }
        minX = qMin(minX, rects.at(a).x());
        minY = qMin(minY, rects.at(a).y());
    }
    QVector<QPoint> positions;
    for (const QRect& rect : std::as_const(rects)) positions.append(QPoint(rect.x() - minX, rect.y() - minY));
    return positions;
}

void planArrangement(Plan& plan, const QVector<NvClientDisplay>& displays, int primary, const HostInfo& host,
                     const Limits& limits, const DisplayProfile::Profile& profile)
{
    const DisplayArrangement::Capabilities caps = host.capabilities.valid ?
                host.capabilities : DisplayArrangement::Capabilities::fleetDefault();
    plan.backingExpected = !host.capabilities.valid;

    // Which monitors: every one that is on, above the host's output budget
    // the primary and its nearest neighbours.
    QVector<QRect> allLogical;
    for (const NvClientDisplay& display : displays) allLogical.append(display.bounds);
    QVector<int> candidates;
    for (const int index : adjacencyOrder(allLogical, primary)) {
        if (plan.outputs.at(index).on) candidates.append(index);
    }
    QStringList left;
    for (int position = 0; position < candidates.size(); ++position) {
        Output& output = plan.outputs[candidates.at(position)];
        if (position < caps.maxOutputs) {
            output.included = true;
        } else {
            left.append(output.name);
        }
    }
    if (!left.isEmpty()) {
        warn(plan, QStringLiteral("too-many"),
             tr("The workstation can show %n screen(s) at once; %1 stays local. Turn a screen off to choose which.",
                nullptr, caps.maxOutputs).arg(left.join(QStringLiteral(", "))),
             QStringLiteral("choose-screens"), tr("Choose screens"));
    }
    QVector<int> included;
    for (int index = 0; index < plan.outputs.size(); ++index) {
        if (plan.outputs.at(index).included) included.append(index);
    }

    // Sizes: the user's choice, stepped down while it breaks the host's
    // per-display limits.
    for (const int index : std::as_const(included)) {
        Output& output = plan.outputs[index];
        output.size = evenSize(chosenSize(output));
        while (output.size.isValid() && !fitsOutputLimits(output.size, caps)) {
            output.size = stepDown(output, output.size, caps);
            output.scaled = true;
        }
        if (!output.size.isValid() || output.size.width() < caps.minOutput.width() ||
                output.size.height() < caps.minOutput.height()) {
            plan.error = errorText(output.size.isValid() ? QStringLiteral("output_too_small") :
                                                           QStringLiteral("output_too_large"), caps);
            return;
        }
    }

    // The whole desktop must fit the host's canvas; without packed capture the
    // capture is the desktop, so it must also fit the encoder and this
    // client's decoder.
    QSize canvasLimit = caps.maxCanvas;
    bool encoderLimited = false;
    bool decoderLimited = false;
    if (!caps.packedCapture) {
        const auto encoding = caps.encodingLimits.constFind(host.encodingMode);
        if (encoding != caps.encodingLimits.constEnd() && encoding->maximum.isValid()) {
            encoderLimited = encoding->maximum.width() < canvasLimit.width() ||
                    encoding->maximum.height() < canvasLimit.height();
            canvasLimit = canvasLimit.boundedTo(encoding->maximum);
        }
        if (limits.decoderMaximum.isValid()) {
            decoderLimited = limits.decoderMaximum.width() < canvasLimit.width() ||
                    limits.decoderMaximum.height() < canvasLimit.height();
            canvasLimit = canvasLimit.boundedTo(limits.decoderMaximum);
        }
    }
    QVector<QRect> logical;
    for (const int index : std::as_const(included)) logical.append(displays.at(index).bounds);
    const int primaryPosition = int(included.indexOf(primary));
    QVector<QPoint> positions;
    bool shrankForCanvas = false;
    for (int attempt = 0;; ++attempt) {
        QVector<QSize> pixels;
        for (const int index : std::as_const(included)) pixels.append(plan.outputs.at(index).size);
        positions = manualPositions(plan, included, profile);
        if (positions.isEmpty()) {
            if (profile.manual && attempt == 0) {
                warn(plan, QStringLiteral("arrangement"),
                     tr("The hand-placed layout does not fit these screens; they are arranged like your desk "
                        "instead."), QStringLiteral("choose-screens"), tr("Arrange"));
            }
            positions = arrange(logical, pixels, primaryPosition);
        }
        QSize canvas;
        for (int position = 0; position < included.size(); ++position) {
            canvas = canvas.expandedTo(QSize(positions.at(position).x() + pixels.at(position).width(),
                                             positions.at(position).y() + pixels.at(position).height()));
        }
        plan.canvas = canvas;
        if (canvas.width() <= canvasLimit.width() && canvas.height() <= canvasLimit.height()) break;
        // Step the largest display that can still get smaller down once.
        int largest = -1;
        QSize next;
        for (const int index : std::as_const(included)) {
            const QSize smaller = stepDown(plan.outputs.at(index), plan.outputs.at(index).size, caps);
            if (smaller.isValid() && (largest < 0 || area(plan.outputs.at(index).size) >
                                                     area(plan.outputs.at(largest).size))) {
                largest = index;
                next = smaller;
            }
        }
        if (largest < 0 || attempt > 64) {
            plan.error = errorText(QStringLiteral("canvas_too_large"), caps);
            return;
        }
        plan.outputs[largest].size = next;
        plan.outputs[largest].scaled = true;
        shrankForCanvas = true;
    }
    for (int position = 0; position < included.size(); ++position) {
        plan.outputs[included.at(position)].position = positions.at(position);
    }

    // The request: the primary first, then the others left to right, top to bottom.
    QVector<int> entries = included;
    std::stable_sort(entries.begin(), entries.end(), [&plan, primary](int a, int b) {
        if ((a == primary) != (b == primary)) return a == primary;
        const QPoint pa = plan.outputs.at(a).position;
        const QPoint pb = plan.outputs.at(b).position;
        return pa.x() != pb.x() ? pa.x() < pb.x() : pa.y() < pb.y();
    });
    QVector<DisplayArrangement::Entry> request;
    for (int position = 0; position < entries.size(); ++position) {
        Output& output = plan.outputs[entries.at(position)];
        output.arrangementIndex = position;
        DisplayArrangement::Entry entry;
        entry.rect = QRect(output.position, output.size);
        DisplayArrangement::preferenceFromName(output.preference, entry.preference);
        request.append(entry);
    }
    plan.arrangement = DisplayArrangement::serialize(request);
    const DisplayArrangement::Resolution resolution = DisplayArrangement::resolve(plan.arrangement, caps);
    if (!resolution.ok) {
        plan.error = errorText(resolution.error, caps);
        return;
    }
    for (int position = 0; position < entries.size(); ++position) {
        Output& output = plan.outputs[entries.at(position)];
        output.backing = resolution.outputs.at(position).backing;
        output.backingOutput = resolution.outputs.at(position).output;
    }

    if (shrankForCanvas && encoderLimited && host.encodingMode.startsWith(QLatin1String("h264"))) {
        const auto hevc = caps.encodingLimits.constFind(QStringLiteral("hevc-10-444-nvenc"));
        const bool hevcHelps = hevc != caps.encodingLimits.constEnd() &&
                hevc->maximum.width() > canvasLimit.width();
        warn(plan, QStringLiteral("codec"),
             tr("H.264 streams at most %1 pixels wide, so some screens are scaled down.")
                 .arg(caps.encodingLimits.value(host.encodingMode).maximum.width()),
             hevcHelps ? QStringLiteral("use-hevc") : QString(), hevcHelps ? tr("Use H.265 4:4:4") : QString());
    } else if (shrankForCanvas && decoderLimited) {
        warn(plan, QStringLiteral("decoder"),
             tr("This Mac decodes at most %1 in hardware, so some screens are scaled down.")
                 .arg(sizeLabel(limits.decoderMaximum)));
    } else if (shrankForCanvas) {
        warn(plan, QStringLiteral("canvas"),
             tr("The screens together are larger than the workstation's desktop (%1), so some are scaled down.")
                 .arg(sizeLabel(caps.maxCanvas)));
    }
    const auto encoding = caps.encodingLimits.constFind(host.encodingMode);
    if (encoding != caps.encodingLimits.constEnd() && encoding->qualified.isValid() &&
            (plan.canvas.width() > encoding->qualified.width() || plan.canvas.height() > encoding->qualified.height())) {
        warn(plan, QStringLiteral("unqualified"),
             tr("The workstation has been tested at up to %1 with this encoding; larger desktops may not reach "
                "60 frames per second.").arg(sizeLabel(encoding->qualified)));
    }
    plan.ok = true;
}

}

QString errorText(const QString& code, const DisplayArrangement::Capabilities& caps)
{
    if (code == QLatin1String("output_too_small")) {
        return tr("A screen is smaller than the workstation supports (at least %1).").arg(sizeLabel(caps.minOutput));
    }
    if (code == QLatin1String("output_too_large")) {
        return tr("A screen is larger than the workstation supports (at most %1).").arg(sizeLabel(caps.maxOutput));
    }
    if (code == QLatin1String("canvas_too_large")) {
        return tr("The screens together are larger than the workstation's desktop (at most %1).")
                .arg(sizeLabel(caps.maxCanvas));
    }
    if (code == QLatin1String("too_many_displays")) {
        return tr("The workstation can show at most %n screen(s) at once.", nullptr, qMax(1, caps.maxOutputs));
    }
    if (code == QLatin1String("no_physical_output")) {
        return tr("A screen is set to use one of the workstation's own outputs, but none is free.");
    }
    if (code == QLatin1String("no_virtual_output")) {
        return tr("A screen is set to use a virtual display, but the workstation has none free.");
    }
    if (code == QLatin1String("overlap")) return tr("Two screens overlap.");
    if (code == QLatin1String("not_negotiated")) {
        return tr("The workstation did not accept the display layout. Update PLANK on the workstation.");
    }
    return tr("The workstation refused the display layout (%1).").arg(code.isEmpty() ? tr("no reason") : code);
}

QSize evenSize(const QSize& size)
{
    if (!size.isValid()) return size;
    return QSize(qMax(2, evenValue(size.width())), qMax(2, evenValue(size.height())));
}

QSize exactSize(const NvClientDisplay& display)
{
    const QSize target = NvOutputTopology::clientMatchTarget(display);
    return target.isValid() ? evenSize(target) : QSize();
}

QSize looksLikeSize(const NvClientDisplay& display)
{
    const QSize exact = exactSize(display);
    const double scale = pointScale(display);
    if (!exact.isValid() || scale < 1.25) return QSize();
    return evenSize(QSize(int(std::lround(exact.width() / scale)), int(std::lround(exact.height() / scale))));
}

QVector<QPoint> arrange(const QVector<QRect>& logical, const QVector<QSize>& pixels, int primary)
{
    const int count = int(logical.size());
    QVector<QRect> placed(count);
    QVector<bool> done(count, false);
    if (count == 0 || pixels.size() != count) return {};
    if (primary < 0 || primary >= count) primary = 0;

    const auto overlapsPlaced = [&](const QRect& rect, int& other) {
        for (int index = 0; index < count; ++index) {
            if (done.at(index) && placed.at(index).intersects(rect)) {
                other = index;
                return true;
            }
        }
        return false;
    };
    const auto place = [&](int b, int a, Side side) {
        const QRect& anchor = placed.at(a);
        const QRect& la = logical.at(a);
        const QRect& lb = logical.at(b);
        const QSize size = pixels.at(b);
        int x = 0;
        int y = 0;
        if (side == Side::Right || side == Side::Left) {
            x = side == Side::Right ? anchor.x() + anchor.width() : anchor.x() - size.width();
            const double scale = double(anchor.height()) / qMax(1, la.height());
            y = anchor.y() + int(std::lround((lb.y() - la.y()) * scale));
            const int seam = qMin(MinimumSeam, qMin(anchor.height(), size.height()));
            if (qMin(anchor.y() + anchor.height(), y + size.height()) - qMax(anchor.y(), y) < seam) {
                y = y > anchor.y() ? anchor.y() + anchor.height() - seam : anchor.y() - size.height() + seam;
            }
            y = evenValue(y);
        } else {
            y = side == Side::Below ? anchor.y() + anchor.height() : anchor.y() - size.height();
            const double scale = double(anchor.width()) / qMax(1, la.width());
            x = anchor.x() + int(std::lround((lb.x() - la.x()) * scale));
            const int seam = qMin(MinimumSeam, qMin(anchor.width(), size.width()));
            if (qMin(anchor.x() + anchor.width(), x + size.width()) - qMax(anchor.x(), x) < seam) {
                x = x > anchor.x() ? anchor.x() + anchor.width() - seam : anchor.x() - size.width() + seam;
            }
            x = evenValue(x);
        }
        QRect rect(x, y, size.width(), size.height());
        // Different scales can make neighbours collide where the client's
        // points did not: move away from the anchor until nothing overlaps.
        int other = -1;
        for (int guard = 0; guard < 4 * count && overlapsPlaced(rect, other); ++guard) {
            const QRect overlap = placed.at(other).intersected(rect);
            switch (side) {
            case Side::Right: rect.translate(overlap.width(), 0); break;
            case Side::Left: rect.translate(-overlap.width(), 0); break;
            case Side::Below: rect.translate(0, overlap.height()); break;
            case Side::Above: rect.translate(0, -overlap.height()); break;
            }
        }
        placed[b] = rect;
        done[b] = true;
    };

    const auto walk = [&](int start) {
        std::queue<int> queue;
        queue.push(start);
        while (!queue.empty()) {
            const int a = queue.front();
            queue.pop();
            for (int b = 0; b < count; ++b) {
                Side side;
                if (done.at(b) || !touching(logical.at(a), logical.at(b), side)) continue;
                place(b, a, side);
                queue.push(b);
            }
        }
    };
    placed[primary] = QRect(QPoint(0, 0), pixels.at(primary));
    done[primary] = true;
    walk(primary);
    // A display that touches none of the others (a monitor between them was
    // turned off): to the right of everything placed so far, top-aligned.
    for (int index = 0; index < count; ++index) {
        if (done.at(index)) continue;
        int right = 0;
        int top = std::numeric_limits<int>::max();
        for (int other = 0; other < count; ++other) {
            if (!done.at(other)) continue;
            right = qMax(right, placed.at(other).x() + placed.at(other).width());
            top = qMin(top, placed.at(other).y());
        }
        placed[index] = QRect(QPoint(right, top), pixels.at(index));
        done[index] = true;
        walk(index);
    }
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const QRect& rect : std::as_const(placed)) {
        minX = qMin(minX, rect.x());
        minY = qMin(minY, rect.y());
    }
    QVector<QPoint> positions;
    for (const QRect& rect : std::as_const(placed)) positions.append(QPoint(rect.x() - minX, rect.y() - minY));
    return positions;
}

DisplayProfile::Profile proposal(const QVector<NvClientDisplay>& displays)
{
    DisplayProfile::Profile profile;
    const QStringList keys = ClientDisplayProbe::uniqueKeys(displays);
    for (const QString& key : keys) profile.choice(key);
    for (int index = 0; index < displays.size(); ++index) {
        if (displays.at(index).main) profile.primary = keys.at(index);
    }
    return profile;
}

Plan plan(const QVector<NvClientDisplay>& displays, const DisplayProfile::Profile& profile, const HostInfo& host,
          const Limits& limits)
{
    Plan result;
    result.scaling = profile.scaling;
    result.presentation = QStringLiteral("single");
    const QStringList keys = ClientDisplayProbe::uniqueKeys(displays);
    const DisplayArrangement::Capabilities caps = host.capabilities.valid ?
                host.capabilities : DisplayArrangement::Capabilities::fleetDefault();
    for (int index = 0; index < displays.size(); ++index) {
        const NvClientDisplay& display = displays.at(index);
        Output output;
        output.key = keys.at(index);
        output.name = ClientDisplayProbe::displayName(display);
        output.builtIn = display.builtIn;
        output.notch = display.notch;
        output.platformId = display.platformId;
        output.clientBounds = display.bounds;
        output.exactSize = exactSize(display);
        output.looksLikeSize = looksLikeSize(display);
        const DisplayProfile::MonitorChoice* choice = profile.find(output.key);
        output.on = choice == nullptr || choice->on;
        output.sizeChoice = choice != nullptr ? DisplayProfile::sizeText(*choice) : QStringLiteral("exact");
        output.preference = choice != nullptr ? choice->backing : QStringLiteral("auto");
        output.sizeOptions = sizeOptions(output, caps);
        result.outputs.append(output);
    }
    const int primary = primaryIndex(result.outputs, displays, profile.primary);
    if (primary < 0) {
        result.error = displays.isEmpty() ? tr("No screens were found on this computer.") :
                                            tr("Choose at least one screen for the workstation.");
        return result;
    }
    result.outputs[primary].primary = true;

    if (host.isMac()) {
        planMac(result, displays, profile.presentation);
    } else if (!host.supportsArrangement()) {
        planLegacy(result, displays, primary, host);
    } else {
        planArrangement(result, displays, primary, host, limits, profile);
    }
    for (Output& output : result.outputs) {
        if (output.included && output.badge.isEmpty()) output.badge = badgeFor(output);
    }
    if (!result.ok) return result;

    const int shown = result.includedCount();
    if (shown > 1 && !host.isMac() && profile.presentation == QLatin1String("windows")) {
        if (limits.separateSpaces) {
            result.presentation = QStringLiteral("windows");
        } else {
            warn(result, QStringLiteral("spaces"),
                 tr("\"Displays have separate Spaces\" is off in System Settings, so the workstation is shown in "
                    "one window across your screens."),
                 QStringLiteral("open-spaces"), tr("Open Desktop & Dock settings"));
        }
    }
    commonWarnings(result, displays);
    return result;
}

}
