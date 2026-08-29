#pragma once

namespace StationConnectPacketSize
{

constexpr int VideoEncryptionHeaderSize = 32;
constexpr int MaxRtpHeaderSize = 16;
constexpr int InnerIpv4UdpOverhead = 28;
constexpr int ZeroTierFrameOverhead = 38;
constexpr int ZeroTierExtendedFrameOverhead = 51;
constexpr int ZeroTierPhysicalPayloadMtu = 1432;
constexpr int DatasmashEnvelopeSize = 16;
constexpr int ConservativeQuicPacketOverhead = 48;

// This is deliberately one 16-byte step below the direct-frame maximum of
// 1344. It also fits an extended ZeroTier frame without fragmentation.
constexpr int VpnVideoPacketSize = 1328;
constexpr int DatasmashFramingBudget = DatasmashEnvelopeSize +
                                       ConservativeQuicPacketOverhead;
constexpr int MinimumPhysicalPathMtu = 1280;
constexpr int MaximumPhysicalPathMtu = 9000;

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

constexpr int datasmashPhysicalPayload(int moonlightPacketSize,
                                       int tunnelFrameOverhead = 0)
{
    return sunshineEncryptedUdpPayload(moonlightPacketSize) +
           DatasmashFramingBudget + InnerIpv4UdpOverhead +
           tunnelFrameOverhead;
}

constexpr int datasmashVideoPacketSize(int legacyPacketSize)
{
    return legacyPacketSize > DatasmashFramingBudget ?
                legacyPacketSize - DatasmashFramingBudget : 0;
}

constexpr int ExtendedPathFramingOverhead = zeroTierPhysicalPayload(
        0, ZeroTierExtendedFrameOverhead);

constexpr int videoPacketSizeForPhysicalMtu(int physicalMtu)
{
    return physicalMtu > ExtendedPathFramingOverhead ?
                (physicalMtu - ExtendedPathFramingOverhead) -
                    ((physicalMtu - ExtendedPathFramingOverhead) % 16) : 0;
}

static_assert(VpnVideoPacketSize % 16 == 0,
              "Moonlight video packet sizes must be 16-byte aligned");
static_assert(zeroTierPhysicalPayload(VpnVideoPacketSize,
                                     ZeroTierExtendedFrameOverhead) <=
                  ZeroTierPhysicalPayloadMtu,
              "StationConnect VPN packets must not require ZeroTier fragmentation");
static_assert(videoPacketSizeForPhysicalMtu(1432) == VpnVideoPacketSize,
              "The qualified ZeroTier MTU must derive the qualified packet size");
static_assert(datasmashVideoPacketSize(VpnVideoPacketSize) == 1264,
              "Datasmash packet sizing must remain 16-byte aligned");
static_assert(datasmashPhysicalPayload(
                  datasmashVideoPacketSize(VpnVideoPacketSize),
                  ZeroTierExtendedFrameOverhead) <= ZeroTierPhysicalPayloadMtu,
              "Datasmash VPN packets must not require ZeroTier fragmentation");

}
