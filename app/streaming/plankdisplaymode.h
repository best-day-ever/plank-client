#pragma once

#include <QSize>

class PlankDisplayMode
{
public:
    static QSize qualifiedMaximum();

    static QSize resolve(const QSize& detectedResolution,
                         const QSize& hostCanvasResolution = QSize(),
                         const QSize& maximumResolution = qualifiedMaximum());

private:
    static QSize fitWithin(const QSize& requestedResolution,
                           const QSize& maximumResolution);
};
