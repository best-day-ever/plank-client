#pragma once

#include "backend/clientdisplayprobe.h"
#include "backend/displayprofile.h"

#include <QSet>

// A display notification is an observation, not an applied workstation layout.
// Keep the applied baseline and explicit Keep choices until the next apply.
class ClientDisplayChange
{
public:
    static QString signature(const QVector<NvClientDisplay>& displays)
    {
        QStringList parts;
        for (const auto& display : displays) {
            parts.append(QStringLiteral("%1@%2,%3,%4x%5/%6x%7").arg(display.key)
                         .arg(display.bounds.x()).arg(display.bounds.y())
                         .arg(display.bounds.width()).arg(display.bounds.height())
                         .arg(display.backingSize.width()).arg(display.backingSize.height()));
        }
        parts.sort();
        return parts.join(QLatin1Char(';'));
    }

    void applied(const QVector<NvClientDisplay>& displays)
    {
        m_Applied = m_Observed = signature(displays);
        m_Kept.clear();
    }

    bool observe(const QVector<NvClientDisplay>& displays)
    {
        const auto next = signature(displays);
        if (next == m_Observed) return false;
        m_Observed = next;
        return true;
    }

    void keep() { m_Kept.insert(m_Observed); }
    bool matchesApplied() const { return m_Observed == m_Applied; }

    bool shouldPrompt(QSettings& settings) const
    {
        return DisplayProfile::askOnChange(settings) &&
                m_Observed != m_Applied && !m_Kept.contains(m_Observed);
    }

private:
    QString m_Applied;
    QString m_Observed;
    QSet<QString> m_Kept;
};
