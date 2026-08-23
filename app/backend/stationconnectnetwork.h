#pragma once

#include <QString>

namespace StationConnectNetwork
{

inline bool isZeroTierInterface(const QString& interfaceName,
                                const QString& humanReadableName)
{
    if (humanReadableName.startsWith(QStringLiteral("ZeroTier"), Qt::CaseInsensitive)) {
        return true;
    }

    // Linux ZeroTier interfaces use a 10-character ztXXXXXXXX kernel name.
    // Qt reports them as Ethernet rather than Virtual, so type-based VPN
    // detection alone cannot identify them.
    const QString normalizedName = interfaceName.toLower();
    if (normalizedName.size() != 10 || !normalizedName.startsWith(QStringLiteral("zt"))) {
        return false;
    }

    for (int i = 2; i < normalizedName.size(); i++) {
        const ushort character = normalizedName.at(i).unicode();
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9'))) {
            return false;
        }
    }

    return true;
}

}
