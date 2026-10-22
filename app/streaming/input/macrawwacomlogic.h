#pragma once
#include <plank.h>
#include <QtEndian>
#include <cstdint>
#include <cstring>

namespace MacWacomWire {
struct Control {
    std::uint16_t type = 0, interfaceId = 0, generation = 0;
    std::uint32_t transaction = 0, size = 0;
};
inline bool parse(const unsigned char* data, std::size_t length, Control& out)
{
    if (!data || length < sizeof(PLANK_RAW_HID_WIRE_HEADER)) return false;
    PLANK_RAW_HID_WIRE_HEADER h;
    std::memcpy(&h, data, sizeof(h));
    if (qFromLittleEndian(h.magic) != PLANK_RAW_HID_WIRE_MAGIC ||
        qFromLittleEndian(h.version) != PLANK_RAW_HID_WIRE_VERSION) return false;
    out = {qFromLittleEndian(h.type), qFromLittleEndian(h.interfaceId),
           qFromLittleEndian(h.generation), qFromLittleEndian(h.transactionId),
           qFromLittleEndian(h.payloadLength)};
    if (!out.generation || out.interfaceId >= PLANK_RAW_HID_MAX_INTERFACES ||
        out.size > PLANK_RAW_HID_MAX_PAYLOAD_SIZE || length != sizeof(h) + out.size) return false;
    switch (out.type) {
    case PLANK_RAW_HID_ATTACH_RESULT: return out.size == sizeof(std::int32_t);
    case PLANK_RAW_HID_GET_REPORT: return out.size == 2;
    case PLANK_RAW_HID_SET_REPORT:
    case PLANK_RAW_HID_OUTPUT: return out.size >= 2 && out.size <= 1 + PLANK_RAW_HID_MAX_REPORT_SIZE;
    default: return false;
    }
}
// Linux UHID uses feature/output/input = 0/1/2; IOKit uses input/output/feature.
inline int ioReportType(unsigned char wireType)
{
    return wireType <= 2 ? 2 - wireType : -1;
}
// Linux reports include the ID slot even for unnumbered devices. IOKit omits
// that synthetic zero slot, while numbered reports retain the real first byte.
inline std::size_t reportPrefix(unsigned char id) { return id == 0 ? 1 : 0; }
}
