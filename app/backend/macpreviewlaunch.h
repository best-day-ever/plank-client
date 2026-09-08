#pragma once

#include "outputtopology.h"
#include <Limelight.h>
#include <QByteArray>

// Typed schema-1 preview only. Never reinterpret Linux's PLS1 launch response
// or infer services from a platform name or a decoder's capabilities.
namespace MacPreviewLaunch {

inline QJsonObject request(const NvOutputTopology& topology, int bitrateKbps,
                           int udpPayloadSize)
{
    NvOutputTopology checked;
    if (topology.featureFlags != NvOutputTopology::FixedCaptureFlags ||
            !NvOutputTopology::fromJson(topology.toJson(), checked) ||
            bitrateKbps < 10000 || bitrateKbps > 150000 ||
            udpPayloadSize < 1200 || udpPayloadSize > 65527) return {};
    return {{"schema_version", 1}, {"capture_generation", checked.generation},
            {"capture_id", checked.outputs.first().id},
            {"width", checked.desktopWidth}, {"height", checked.desktopHeight},
            {"encoding_mode", "hevc-10-420-videotoolbox"}, {"frame_rate", 60},
            {"bitrate_kbps", bitrateKbps}, {"max_udp_payload_size", udpPayloadSize}};
}

struct Reply {
    QByteArray transportToken;
    PLANK_NATIVE_SESSION_CONFIGURATION configuration {};
};

inline bool parseReply(const QJsonObject& object, const NvOutputTopology& topology,
                       int approvedControlPort, int udpPayloadSize, Reply& reply)
{
    // Always clear an earlier successful result before parsing a new response.
    reply.transportToken.fill('\0');
    reply = {};
    if (request(topology, 10000, udpPayloadSize).isEmpty() ||
            approvedControlPort < 1 || approvedControlPort > 65535 ||
            object.size() != 7 || object.value("schema_version") != QJsonValue(1) ||
            object.value("state") != QJsonValue("connecting") ||
            object.value("udp_port") != QJsonValue(approvedControlPort) ||
            object.value("max_udp_payload_size") != QJsonValue(udpPayloadSize) ||
            object.value("capture") != topology.toJson().value("capture") ||
            object.value("services") != QJsonValue(QJsonObject {
                {"audio", true}, {"input", true}, {"cursor", "embedded"}})) return false;

    const QString token = object.value("transport_token").toString();
    if (token.size() != 44) return false;
    const QByteArray encoded = token.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != 32 || decoded.decoded.toBase64() != encoded) return false;
    reply.transportToken = encoded;
    reply.configuration.structSize = sizeof(reply.configuration);
    reply.configuration.negotiatedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
    // Schema 1 explicitly supports PLD1 bitrate updates/acknowledgements.
    reply.configuration.hostFeatureFlags = LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK;
    reply.configuration.sessionPort = static_cast<uint32_t>(approvedControlPort);
    reply.configuration.serviceFlags = PLANK_NATIVE_SERVICE_AUDIO | PLANK_NATIVE_SERVICE_INPUT;
    reply.configuration.audioPacketDurationMs = 5;
    reply.configuration.opusConfiguration.sampleRate = 48000;
    reply.configuration.opusConfiguration.channelCount = 2;
    reply.configuration.opusConfiguration.streams = 1;
    reply.configuration.opusConfiguration.coupledStreams = 1;
    reply.configuration.opusConfiguration.mapping[0] = 0;
    reply.configuration.opusConfiguration.mapping[1] = 1;
    return true;
}

} // namespace MacPreviewLaunch
