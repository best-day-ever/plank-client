#pragma once

#include <QtEndian>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <plank.h>
}

namespace plank::clipboard {

inline bool validUtf8(const char* data, std::size_t size)
{
    if (data == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < size;) {
        const auto byte = static_cast<unsigned char>(data[index]);
        if (byte <= 0x7F) {
            ++index;
            continue;
        }
        std::size_t continuation = 0;
        if ((byte & 0xE0) == 0xC0) {
            continuation = 1;
        }
        else if ((byte & 0xF0) == 0xE0) {
            continuation = 2;
        }
        else if ((byte & 0xF8) == 0xF0) {
            continuation = 3;
        }
        else {
            return false;
        }
        if (index + continuation >= size) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<unsigned char>(data[index + offset]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
        }
        index += continuation + 1;
    }
    return true;
}

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

    bool appendChunk(const PLANK_CLIPBOARD_WIRE_HEADER& wire,
                     const std::uint8_t* chunkData)
    {
        const auto magic = qFromLittleEndian(wire.magic);
        const auto version = qFromLittleEndian(wire.version);
        const auto flags = qFromLittleEndian(wire.flags);
        const auto generationValue = qFromLittleEndian(wire.generation);
        const auto totalSizeValue = qFromLittleEndian(wire.totalSize);
        const auto chunkOffset = qFromLittleEndian(wire.chunkOffset);
        const auto chunkSize = qFromLittleEndian(wire.chunkSize);
        constexpr std::uint32_t knownFlags =
            PLANK_CLIPBOARD_FLAG_FIRST_CHUNK | PLANK_CLIPBOARD_FLAG_LAST_CHUNK;

        if (magic != PLANK_CLIPBOARD_WIRE_MAGIC ||
                version != PLANK_CLIPBOARD_WIRE_VERSION ||
                (flags & ~knownFlags) != 0 ||
                totalSizeValue == 0 ||
                totalSizeValue > PLANK_CLIPBOARD_MAX_TEXT_SIZE ||
                chunkSize > PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE ||
                chunkOffset > totalSizeValue ||
                chunkSize > totalSizeValue - chunkOffset) {
            reset();
            return false;
        }

        if ((flags & PLANK_CLIPBOARD_FLAG_FIRST_CHUNK) != 0) {
            if (chunkOffset != 0) {
                reset();
                return false;
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
            return false;
        }

        std::memcpy(bytes.data() + chunkOffset, chunkData, chunkSize);
        nextOffset += chunkSize;

        if ((flags & PLANK_CLIPBOARD_FLAG_LAST_CHUNK) == 0) {
            return false;
        }

        if (nextOffset != totalSize ||
                !validUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size())) {
            reset();
            return false;
        }

        active = false;
        return true;
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
            maxChunkSize == 0) {
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
