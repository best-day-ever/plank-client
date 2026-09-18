#pragma once

#include <QtEndian>
#include <plank_clipboard_wire.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <plank.h>
}

namespace plank::clipboard {
static_assert(PLANK_CLIPBOARD_MAX_TEXT_SIZE == PLANK_CLIPBOARD_TEXT_LIMIT);
static_assert(sizeof(PLANK_CLIPBOARD_WIRE_HEADER) == PLANK_CLIPBOARD_HEADER_BYTES);

inline bool validUtf8(const char* data, std::size_t size)
{
    return plank_clipboard_valid_text(reinterpret_cast<const uint8_t*>(data), size);
}

enum class AppendResult {
    Rejected,
    Incomplete,
    Complete,
};

struct Assembly {
    bool active = false;
    std::uint64_t generation = 0;
    std::uint32_t totalSize = 0;
    std::uint32_t nextOffset = 0;
    std::vector<std::uint8_t> bytes;

    void reset()
    {
        active = false;
        generation = 0;
        totalSize = 0;
        nextOffset = 0;
        bytes.clear();
    }

    AppendResult appendChunk(const PLANK_CLIPBOARD_WIRE_HEADER& wire,
                             const std::uint8_t* chunkData)
    {
        const auto magic = qFromLittleEndian(wire.magic);
        const auto version = qFromLittleEndian(wire.version);
        const auto reserved = qFromLittleEndian(wire.reserved);
        const auto flags = qFromLittleEndian(wire.flags);
        const auto generationValue = qFromLittleEndian(wire.generation);
        const auto totalSizeValue = qFromLittleEndian(wire.totalSize);
        const auto chunkOffset = qFromLittleEndian(wire.chunkOffset);
        const auto chunkSize = qFromLittleEndian(wire.chunkSize);
        constexpr std::uint32_t knownFlags =
            PLANK_CLIPBOARD_FLAG_FIRST_CHUNK | PLANK_CLIPBOARD_FLAG_LAST_CHUNK;

        if (magic != PLANK_CLIPBOARD_WIRE_MAGIC ||
                version != PLANK_CLIPBOARD_WIRE_VERSION ||
                reserved != 0 ||
                generationValue == 0 ||
                (flags & ~knownFlags) != 0 ||
                totalSizeValue == 0 ||
                totalSizeValue > PLANK_CLIPBOARD_MAX_TEXT_SIZE ||
                chunkSize == 0 ||
                chunkSize > PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE ||
                chunkOffset > totalSizeValue ||
                chunkSize > totalSizeValue - chunkOffset ||
                chunkData == nullptr) {
            reset();
            return AppendResult::Rejected;
        }

        if ((flags & PLANK_CLIPBOARD_FLAG_FIRST_CHUNK) != 0) {
            if (chunkOffset != 0) {
                reset();
                return AppendResult::Rejected;
            }
            active = true;
            generation = generationValue;
            totalSize = totalSizeValue;
            nextOffset = 0;
            bytes.assign(totalSize, 0);
        }

        if (!active || generation != generationValue || totalSize != totalSizeValue ||
                chunkOffset != nextOffset) {
            reset();
            return AppendResult::Rejected;
        }

        std::memcpy(bytes.data() + chunkOffset, chunkData, chunkSize);
        nextOffset += chunkSize;

        if ((flags & PLANK_CLIPBOARD_FLAG_LAST_CHUNK) == 0) {
            if (nextOffset == totalSize) {
                reset();
                return AppendResult::Rejected;
            }
            return AppendResult::Incomplete;
        }

        if (nextOffset != totalSize ||
                !validUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size())) {
            reset();
            return AppendResult::Rejected;
        }

        active = false;
        return AppendResult::Complete;
    }
};

inline void writeHeader(PLANK_CLIPBOARD_WIRE_HEADER& header,
                        std::uint32_t flags,
                        std::uint64_t generation,
                        std::uint32_t totalSize,
                        std::uint32_t chunkOffset,
                        std::uint32_t chunkSize)
{
    header.magic = qToLittleEndian(static_cast<std::uint32_t>(PLANK_CLIPBOARD_WIRE_MAGIC));
    header.version = qToLittleEndian(static_cast<std::uint16_t>(PLANK_CLIPBOARD_WIRE_VERSION));
    header.reserved = 0;
    header.flags = qToLittleEndian(flags);
    header.generation = qToLittleEndian(generation);
    header.totalSize = qToLittleEndian(totalSize);
    header.chunkOffset = qToLittleEndian(chunkOffset);
    header.chunkSize = qToLittleEndian(chunkSize);
}

inline std::vector<std::vector<std::uint8_t>> buildEventFrames(
        const std::uint8_t* text, std::size_t textSize, std::uint64_t generation,
        std::uint32_t maxChunkSize)
{
    std::vector<std::vector<std::uint8_t>> frames;
    if (text == nullptr || textSize == 0 ||
            textSize > PLANK_CLIPBOARD_MAX_TEXT_SIZE ||
            maxChunkSize == 0 || maxChunkSize > PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE ||
            generation == 0 || !validUtf8(reinterpret_cast<const char*>(text), textSize)) {
        return frames;
    }

    const auto totalSize = static_cast<std::uint32_t>(textSize);
    for (std::uint32_t offset = 0; offset < totalSize;) {
        const auto chunkSize = std::min(maxChunkSize, totalSize - offset);
        std::vector<std::uint8_t> frame(sizeof(PLANK_CLIPBOARD_WIRE_HEADER) + chunkSize);
        PLANK_CLIPBOARD_WIRE_HEADER header {};
        std::uint32_t flags = 0;
        if (offset == 0) {
            flags |= PLANK_CLIPBOARD_FLAG_FIRST_CHUNK;
        }
        if (offset + chunkSize == totalSize) {
            flags |= PLANK_CLIPBOARD_FLAG_LAST_CHUNK;
        }
        writeHeader(header, flags, generation, totalSize, offset, chunkSize);
        std::memcpy(frame.data(), &header, sizeof(header));
        std::memcpy(frame.data() + sizeof(header), text + offset, chunkSize);
        frames.push_back(std::move(frame));
        offset += chunkSize;
    }
    return frames;
}

}  // namespace plank::clipboard
