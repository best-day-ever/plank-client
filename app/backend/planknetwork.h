#pragma once

#include <QString>
#include <QtGlobal>

namespace PlankNetwork
{

// ZeroTier carries an inner IPv4/UDP/QUIC packet inside a physical UDP
// payload capped at 1432 bytes on the qualified path. A 1344-byte QUIC UDP
// payload remains below that boundary for both normal and extended ZeroTier
// frames while yielding a 1280-byte KyProto video RaptorQ symbol. This is an
// automatic ceiling, not permission to exceed a smaller interface MTU.
constexpr quint16 ZeroTierPhysicalUdpPayloadLimit = 1432;
constexpr quint16 ZeroTierExtendedFrameOverhead = 51;
constexpr quint16 InnerIpv4UdpOverhead = 28;
constexpr quint16 InnerIpv6UdpOverhead = 48;
constexpr quint16 MinimumQuicUdpPayloadMtu = 1200;
constexpr quint16 MaximumQuicUdpPayloadMtu = 65527;
constexpr quint16 MaximumAutomaticQuicUdpPayloadMtu = 1452;
constexpr quint16 AutomaticPathSafetyMargin = 20;
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

inline quint16 quicUdpPayloadMtuForRoute(int configuredMtu,
                                         bool isZeroTier,
                                         quint32 interfaceMtu = 0,
                                         bool isIpv6 = false)
{
    // Zero is reserved for rejection here, never transport path discovery.
    if (configuredMtu != 0 &&
            (configuredMtu < MinimumQuicUdpPayloadMtu ||
             configuredMtu > MaximumQuicUdpPayloadMtu)) {
        return 0;
    }
    const quint32 networkOverhead = isIpv6 ? InnerIpv6UdpOverhead :
                                            InnerIpv4UdpOverhead;
    quint32 interfacePayloadLimit = MaximumQuicUdpPayloadMtu;
    if (interfaceMtu != 0) {
        // Check before subtracting, including interfaces below QUIC's minimum.
        if (interfaceMtu < MinimumQuicUdpPayloadMtu + networkOverhead +
                           AutomaticPathSafetyMargin) {
            return 0;
        }
        interfacePayloadLimit = interfaceMtu - networkOverhead - AutomaticPathSafetyMargin;
    }

    // An explicit override may replace automatic policy, but must fit any known
    // interface limit. Keep the 20-byte margin for both automatic/manual values.
    if (configuredMtu != 0) {
        return quint32(configuredMtu) <= interfacePayloadLimit ? quint16(configuredMtu) : 0;
    }

    // Unknown MTU is distinct from a known, unusably small MTU. Retain the
    // qualified ZeroTier ceiling / minimum-QUIC fallback only for unknown MTU.
    const quint16 automaticCeiling = isZeroTier ? ZeroTierQuicUdpPayloadMtu :
            (interfaceMtu ? MaximumAutomaticQuicUdpPayloadMtu : MinimumQuicUdpPayloadMtu);
    return quint16(qMin(interfacePayloadLimit, quint32(automaticCeiling)));
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
