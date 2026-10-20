#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace WindowsWacom {
// Windows HID reads are padded to the largest report in a collection. Linux
// hid-wacom expects the actual size for the report ID, including constant bits.
// Parse the descriptor, never infer a report's size by stripping zero bytes.
struct ReportSizes {
    std::array<std::array<std::size_t, 256>, 3> bytes{}; // input/output/feature
    bool numbered = false;
};
inline bool reportSizes(const unsigned char* data, std::size_t length,
                        ReportSizes& result)
{
    struct Globals { std::uint32_t size = 0, count = 0, id = 0; } globals;
    std::vector<Globals> stack;
    std::array<std::array<std::size_t, 256>, 3> bits{};
    bool numbered = false;
    for (std::size_t offset = 0; offset < length;) {
        const auto prefix = data[offset++];
        if (prefix == 0xfe) { // Long items cannot define the report layout.
            if (length - offset < 2) return false;
            const auto size = data[offset];
            offset += 2;
            if (size > length - offset) return false;
            offset += size;
            continue;
        }
        const std::size_t size = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (size > length - offset) return false;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < size; ++i) value |= std::uint32_t(data[offset++]) << (i * 8);
        const auto type = (prefix >> 2) & 3;
        const auto tag = prefix >> 4;
        if (type == 1) {
            if (tag == 7) globals.size = value;
            else if (tag == 9) globals.count = value;
            else if (tag == 8) {
                if (!value || value > 255) return false;
                globals.id = value;
                numbered = true;
            }
            else if (tag == 10) {
                if (stack.size() == 16) return false;
                stack.push_back(globals);
            }
            else if (tag == 11) {
                if (stack.empty()) return false;
                globals = stack.back();
                stack.pop_back();
            }
        }
        else if (type == 0 && (tag == 8 || tag == 9 || tag == 11)) {
            const auto reportType = tag == 8 ? 0 : tag == 9 ? 1 : 2;
            if (globals.size > 32768 || globals.count > 32768) return false;
            const std::uint64_t added = std::uint64_t(globals.size) * globals.count;
            auto& total = bits[reportType][globals.id];
            if (added > 32768 || total + added > 32768) return false;
            total += static_cast<std::size_t>(added);
        }
    }
    if (!stack.empty()) return false;
    ReportSizes parsed;
    parsed.numbered = numbered;
    bool haveReport = false;
    for (std::size_t type = 0; type < 3; ++type) {
        for (std::size_t id = 0; id < 256; ++id) {
            if (!bits[type][id]) continue;
            if (numbered && id == 0) return false;
            parsed.bytes[type][id] = (bits[type][id] + 7) / 8 + (numbered ? 1 : 0);
            if (parsed.bytes[type][id] > 4096) return false;
            haveReport = true;
        }
    }
    result = parsed;
    return haveReport;
}
struct ReportRequest {
    unsigned type = 0, id = 0;
    std::size_t reportLength = 0;
    std::vector<unsigned char> buffer;
    bool valid = false;
};
inline ReportRequest reportRequest(const ReportSizes& sizes, bool get,
                                   const unsigned char* payload, std::size_t length)
{
    ReportRequest request;
    if (!payload || length < 2 || (get && length != 2)) return request;
    request.type = get ? payload[1] : payload[0];
    request.id = get ? payload[0] : payload[1];
    if (request.type > 2 || (!sizes.numbered && request.id)) return request;
    const unsigned index = request.type == 0 ? 2 : request.type == 1 ? 1 : 0;
    const auto actual = sizes.bytes[index][request.id];
    if (!actual) return request;
    // Linux raw requests and HIDAPI both retain an ID slot for unnumbered
    // reports. Only interrupt reads omit it. Preserve zero-valued data bytes.
    const auto prefix = sizes.numbered ? 0 : 1;
    request.reportLength = actual + prefix;
    const auto maximum = *std::max_element(sizes.bytes[index].begin(), sizes.bytes[index].end()) + prefix;
    if (maximum > 4096 || (!get && length - 1 != request.reportLength)) return request;
    request.buffer.assign(get ? maximum : length - 1, 0);
    if (get) request.buffer[0] = static_cast<unsigned char>(request.id);
    else std::copy(payload + 1, payload + length, request.buffer.begin());
    request.valid = true;
    return request;
}
inline bool compatibilityCollection(unsigned page, unsigned usage)
{
    // Windows reserves keyboard/mouse TLCs. Their compatibility events use
    // ordinary SDL keyboard/mouse input; full Wacom packets use digitizer or
    // vendor collections. Do not let a protected compatibility TLC prevent
    // opening every tablet-specific collection.
    return page == 1 && (usage == 1 || usage == 2 || usage == 6 || usage == 7);
}
inline bool normalizedOnlyProduct(std::uint16_t product)
{
    // UHID cannot supply the physical USB interface type these models need.
    return product == 0x0314 || product == 0x0315 || product == 0x0317;
}
inline std::size_t inputLength(const ReportSizes& sizes,
                               const unsigned char* data, std::size_t length)
{
    if (!length) return 0;
    const auto expected = sizes.bytes[0][sizes.numbered ? data[0] : 0];
    return expected && expected <= length ? expected : 0;
}
} // namespace WindowsWacom
