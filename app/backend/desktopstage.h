#pragma once

#include <QJsonObject>
#include <QString>

// UI-only evidence from a successful PAM response, not launch authority.
inline bool plankAuthenticatedGreeter(const QJsonObject& response)
{
    return response.value(QStringLiteral("state")).toString() == QStringLiteral("authenticated") &&
            !response.value(QStringLiteral("session_token")).toString().isEmpty() &&
            response.value(QStringLiteral("desktop_stage")).toString() == QStringLiteral("greeter");
}
