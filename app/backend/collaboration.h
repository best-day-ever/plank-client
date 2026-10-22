#pragma once

#include <QJsonObject>
#include <QString>
#include <QSize>
#include <QRect>
#include <QVector>
#include <QtMath>
#include <QUrl>
#include <QUrlQuery>
#include <cstddef>

// Host consent is authority. Request identifiers and grants never select a
// permission; launch must independently confirm the host's effective role.
struct PlankCollaboration
{
    enum Permission { Owner = 0, View = 1, Control = 2 };
    enum State { Invalid, Pending, Denied, Approved, Revoked, Expired };
    State state = Invalid;
    Permission permission = View;
    QString requestId;
    QString grant;
    QJsonObject topology;
    QJsonObject capture;
    QSize size;
    int fps = 0;

    static bool opaque(const QString& text)
    {
        if (text.size() < 16 || text.size() > 512) return false;
        for (const QChar c : text) {
            if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                  (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                  (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  c == QLatin1Char('-') || c == QLatin1Char('_'))) return false;
        }
        return true;
    }

    static PlankCollaboration parse(const QJsonObject& object, const QString& expectedId = {})
    {
        PlankCollaboration reply;
        reply.requestId = object.value(QStringLiteral("request_id")).toString();
        if (!opaque(reply.requestId) || (!expectedId.isEmpty() && reply.requestId != expectedId)) return {};
        const QString state = object.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("pending")) reply.state = Pending;
        else if (state == QLatin1String("denied")) reply.state = Denied;
        else if (state == QLatin1String("expired")) reply.state = Expired;
        else if (state == QLatin1String("revoked")) reply.state = Revoked;
        else if (state == QLatin1String("view") || state == QLatin1String("control")) {
            reply.permission = state == QLatin1String("control") ? Control : View;
            reply.grant = object.value(QStringLiteral("grant")).toString();
            reply.capture = object.value(QStringLiteral("capture")).toObject();
            reply.topology = object.value(QStringLiteral("topology")).toObject();
            reply.size = QSize(reply.capture.value(QStringLiteral("width")).toInt(),
                               reply.capture.value(QStringLiteral("height")).toInt());
            reply.fps = reply.capture.value(QStringLiteral("fps")).toInt();
            if (!opaque(reply.grant) || reply.topology.isEmpty() ||
                    !reply.size.isValid() || reply.size.isEmpty() ||
                    reply.size.width() > 32768 || reply.size.height() > 32768 ||
                    reply.fps < 1 || reply.fps > 240) return {};
            reply.state = Approved;
        }
        return reply;
    }

    static QString launchVerb(bool collaborator, bool desktopRunning)
    {
        return !collaborator && desktopRunning ? QStringLiteral("resume") : QStringLiteral("launch");
    }

    static int wirePermission(const unsigned char* bytes, std::size_t size)
    {
        if (bytes == nullptr || size != 4 || bytes[0] || bytes[1] || bytes[2] || bytes[3] > 2) return -1;
        return bytes[3];
    }

    static QUrl diagnosticUrl(QUrl url)
    {
        QUrlQuery query(url);
        query.removeAllQueryItems(QStringLiteral("plankCollaboration"));
        if (url.path().startsWith(QStringLiteral("/plank/collaboration/")))
            query.removeAllQueryItems(QStringLiteral("request_id"));
        url.setQuery(query);
        return url;
    }

    // Place local viewing windows in the owner's spatial layout, scaled to
    // fit the current local screen. No desktop or bookmark setting is changed.
    static QVector<QRect> windowPlacement(const QVector<QRect>& outputs, const QRect& available)
    {
        if (outputs.isEmpty() || !available.isValid()) return {};
        QRect desktop;
        for (const auto& output : outputs) {
            if (!output.isValid() || output.isEmpty()) return {};
            desktop = desktop.united(output);
        }
        const double scale = qMin(available.width() * 0.8 / desktop.width(),
                                  available.height() * 0.8 / desktop.height());
        const QPoint origin(available.x() + qRound((available.width() - desktop.width() * scale) / 2),
                            available.y() + qRound((available.height() - desktop.height() * scale) / 2));
        QVector<QRect> result;
        for (const auto& output : outputs) {
            result.append(QRect(origin + QPoint(qRound((output.x() - desktop.x()) * scale),
                                                qRound((output.y() - desktop.y()) * scale)),
                QSize(qMax(1, qRound(output.width() * scale)), qMax(1, qRound(output.height() * scale)))));
        }
        return result;
    }

    // Native input types 1..5 are pointer/buttons/scroll/keyboard. UTF-8
    // injection, pen, raw HID and clipboard are never collaboration channels.
    static bool allowsInput(Permission permission, unsigned type)
    {
        return permission == Owner || (permission == Control && type >= 1 && type <= 5);
    }
};
