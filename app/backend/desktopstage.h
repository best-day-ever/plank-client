#pragma once

#include <QJsonObject>
#include <QString>

// Evidence from successful OS authentication, not independent launch authority.
inline bool plankAuthenticatedGreeter(const QJsonObject& response)
{
    return response.value(QStringLiteral("state")).toString() == QStringLiteral("authenticated") &&
            !response.value(QStringLiteral("session_token")).toString().isEmpty() &&
            response.value(QStringLiteral("desktop_stage")).toString() == QStringLiteral("greeter");
}

// Single-user desktop policy (DesktopSignOutFeature): a refused launch or
// resume names the account that owns the workstation desktop. UI and retry
// hints only; the host re-checks ownership before it signs anyone out.
struct PlankDesktopSignOut
{
    enum class State { None, Available, Connected, Started };
    // What the Client does next with a refused launch or resume.
    enum class Step { NotApplicable, InUse, Confirm, AwaitGreeter, Unexpected };

    State state = State::None;
    QString owner;

    // The owner is echoed back verbatim in plankSignOutOwner, so reject
    // anything that is not a plausible single account name.
    static bool validOwner(const QString& owner)
    {
        if (owner.isEmpty() || owner.size() > 256) return false;
        for (const QChar c : owner) {
            if (c.isSpace() || c.category() == QChar::Other_Control) return false;
        }
        return true;
    }

    static PlankDesktopSignOut fromResponse(int statusCode, const QString& owner,
                                            const QString& signOut)
    {
        PlankDesktopSignOut result;
        if (!validOwner(owner)) return result;
        if (statusCode == 409 && signOut == QStringLiteral("available")) {
            result.state = State::Available;
        } else if (statusCode == 409 && signOut == QStringLiteral("connected")) {
            result.state = State::Connected;
        } else if (statusCode == 503 && signOut == QStringLiteral("started")) {
            result.state = State::Started;
        } else {
            return result;
        }
        result.owner = owner;
        return result;
    }

    // requestedOwner is the account this Client already asked the host to
    // sign out (empty before the user confirmed). A reconnect never offers
    // sign-out: it only reports that the desktop now belongs to someone else.
    static Step nextStep(const PlankDesktopSignOut& reply, bool reconnecting,
                         bool credentialsAvailable, const QString& requestedOwner)
    {
        switch (reply.state) {
        case State::None:
            return Step::NotApplicable;
        case State::Connected:
            return Step::InUse;
        case State::Available:
            if (reconnecting || !credentialsAvailable) return Step::InUse;
            // The host refused the sign-out it offered; do not prompt in a loop.
            return reply.owner == requestedOwner ? Step::Unexpected : Step::Confirm;
        case State::Started:
            return !requestedOwner.isEmpty() && reply.owner == requestedOwner ?
                        Step::AwaitGreeter : Step::Unexpected;
        }
        return Step::Unexpected;
    }

    // While the signed-out session is still being torn down the host may
    // briefly report the same owner again; anything else is a new conflict.
    static bool stillSigningOut(const PlankDesktopSignOut& reply, const QString& requestedOwner)
    {
        return reply.state == State::Available && !requestedOwner.isEmpty() &&
                reply.owner == requestedOwner;
    }
};
