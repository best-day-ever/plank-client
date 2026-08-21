#pragma once

#include <QSize>

class StationConnectDisplayMode
{
public:
    static QSize qualifiedMaximum();

    static QSize resolve(bool autoResolution,
                         const QSize& detectedResolution,
                         const QSize& configuredResolution,
                         const QSize& maximumResolution = qualifiedMaximum());

private:
    static QSize fitWithin(const QSize& requestedResolution,
                           const QSize& maximumResolution);
};
