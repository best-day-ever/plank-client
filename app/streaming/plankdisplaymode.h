#pragma once

#include <QSize>
#include <QString>

class PlankDisplayMode
{
public:
    // The ceiling of the legacy scaled-span path only (a 4K stream fitted to
    // the client); display arrangements use resolveClient instead.
    static QSize qualifiedMaximum();

    // Display arrangement: the stream is the workstation desktop 1:1. Returns
    // the canvas when it is even and fits the limit (encoder and decoder,
    // invalid for none), otherwise an invalid size and a user-facing reason.
    // Never scales silently: the planner already fitted the layout.
    static QSize resolveClient(const QSize& canvas, const QSize& limit, QString* error = nullptr);

    static QSize resolve(const QSize& detectedResolution,
                         const QSize& hostCanvasResolution = QSize(),
                         const QSize& maximumResolution = qualifiedMaximum());

private:
    static QSize fitWithin(const QSize& requestedResolution,
                           const QSize& maximumResolution);
};
