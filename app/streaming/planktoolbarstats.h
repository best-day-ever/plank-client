#pragma once

#include <QFont>
#include <QFontMetrics>
#include <QString>
#include <algorithm>
#include <cstdint>

namespace PlankToolbarStats {

constexpr int RttLeft = 229;
constexpr int RttWidth = 52;
constexpr int EncoderTargetLeft = RttLeft + RttWidth + 8;
constexpr int WindowControlsWidth = 113;

inline QString networkRttText(std::uint32_t milliseconds)
{
    return milliseconds == 0 ? QStringLiteral("—") :
                              QStringLiteral("%1 ms").arg(milliseconds);
}

inline QFont encoderTargetFont()
{
    QFont font;
    font.setPixelSize(12);
    font.setWeight(QFont::DemiBold);
    return font;
}

inline QString encoderTargetText(int kilobitsPerSecond)
{
    return QStringLiteral("Encoder target  %1 Mbps")
            .arg(kilobitsPerSecond / 1000.0, 0, 'f', 1);
}

// Reserve the widest valid value once. Slider movement must not move the
// slider endpoints, toolbar controls or their hit regions under the pointer.
inline int encoderTargetWidth(int minimumKbps, int maximumKbps, int stepKbps)
{
    const QFontMetrics metrics(encoderTargetFont());
    int width = 0;
    for (int value = minimumKbps; value <= maximumKbps; value += stepKbps) {
        width = std::max(width, metrics.horizontalAdvance(encoderTargetText(value)));
    }
    return width;
}

}
