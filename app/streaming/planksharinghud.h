#pragma once

#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

// The stream HUD uses the same Unicode-capable system font as the toolbar.
// Names are plain text, bounded and elided; no rich text or desktop font pin.
inline QImage plankSharingHud(const QString& text, bool viewOnly)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPixelSize(14);
    const QFontMetrics metrics(font);
    const QString label = metrics.elidedText(text.simplified().left(256), Qt::ElideRight, 600);
    const int height = qMax(32, metrics.height() + 14);
    QImage image(metrics.horizontalAdvance(label) + 42, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(24, 27, 34, 225));
    painter.setPen(QColor(255, 255, 255, 45));
    painter.drawRoundedRect(QRectF(0.5, 0.5, image.width() - 1, height - 1), 8, 8);
    painter.setPen(Qt::NoPen);
    painter.setBrush(viewOnly ? QColor(255, 197, 82) : QColor(76, 205, 145));
    painter.drawEllipse(QPointF(14, height / 2.0), 3.5, 3.5);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(27, 0, image.width() - 34, height), Qt::AlignVCenter, label);
    return image;
}
