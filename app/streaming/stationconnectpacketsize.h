#pragma once

namespace StationConnectPacketSize
{

constexpr int VideoEncryptionHeaderSize = 32;
constexpr int MaxRtpHeaderSize = 16;
constexpr int InnerIpv4UdpOverhead = 28;
constexpr int ZeroTierFrameOverhead = 38;
constexpr int ZeroTierExtendedFrameOverhead = 51;
constexpr int ZeroTierPhysicalPayloadMtu = 1432;

// This is deliberately one 16-byte step below the direct-frame maximum of
// 1344. It also fits an extended ZeroTier frame without fragmentation.
constexpr int VpnVideoPacketSize = 1328;

constexpr int sunshineNegotiatedPacketSize(int moonlightPacketSize)
{
    return moonlightPacketSize - VideoEncryptionHeaderSize;
}

constexpr int sunshineEncryptedUdpPayload(int moonlightPacketSize)
{
    return sunshineNegotiatedPacketSize(moonlightPacketSize) +
           MaxRtpHeaderSize + VideoEncryptionHeaderSize;
}

constexpr int zeroTierPhysicalPayload(int moonlightPacketSize,
                                      int zeroTierFrameOverhead = ZeroTierFrameOverhead)
{
    return sunshineEncryptedUdpPayload(moonlightPacketSize) +
           InnerIpv4UdpOverhead + zeroTierFrameOverhead;
}

static_assert(VpnVideoPacketSize % 16 == 0,
              "Moonlight video packet sizes must be 16-byte aligned");
static_assert(zeroTierPhysicalPayload(VpnVideoPacketSize,
                                     ZeroTierExtendedFrameOverhead) <=
                  ZeroTierPhysicalPayloadMtu,
              "StationConnect VPN packets must not require ZeroTier fragmentation");

}
