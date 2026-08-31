#pragma once

#include <QString>
#include <QtGlobal>

namespace StationConnectNetwork
{

// ZeroTier carries an inner IPv4/UDP/QUIC packet inside a physical UDP
// payload capped at 1432 bytes on the qualified path. A 1344-byte QUIC UDP
// payload remains below that boundary for both normal and extended ZeroTier
// frames while yielding a 1280-byte KyProto video RaptorQ symbol.
constexpr quint16 ZeroTierPhysicalUdpPayloadLimit = 1432;
constexpr quint16 ZeroTierExtendedFrameOverhead = 51;
constexpr quint16 InnerIpv4UdpOverhead = 28;
constexpr quint16 MinimumQuicUdpPayloadMtu = 1200;
constexpr quint16 MaximumQuicUdpPayloadMtu = 65527;
constexpr quint16 ZeroTierQuicUdpPayloadMtu = 1344;
constexpr quint16 ConservativeQuicDatagramOverhead = 38;
constexpr quint16 KyProtoVideoFecHeaderSize = 26;
constexpr quint16 ZeroTierQuicApplicationDatagramSize =
        ZeroTierQuicUdpPayloadMtu - ConservativeQuicDatagramOverhead;
constexpr quint16 ZeroTierRaptorQVideoSymbolSize =
        ZeroTierQuicApplicationDatagramSize - KyProtoVideoFecHeaderSize;

static_assert(ZeroTierQuicUdpPayloadMtu + InnerIpv4UdpOverhead +
                  ZeroTierExtendedFrameOverhead <= ZeroTierPhysicalUdpPayloadLimit,
              "ZeroTier QUIC packets must not require overlay fragmentation");
static_assert(ZeroTierRaptorQVideoSymbolSize == 1280,
              "The qualified ZeroTier path must retain 1280-byte RaptorQ symbols");

inline quint16 quicUdpPayloadMtuForRoute(int configuredMtu, bool isZeroTier)
{
    if (configuredMtu >= MinimumQuicUdpPayloadMtu &&
            configuredMtu <= MaximumQuicUdpPayloadMtu) {
        return quint16(configuredMtu);
    }

    return isZeroTier ? ZeroTierQuicUdpPayloadMtu : 0;
}

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
