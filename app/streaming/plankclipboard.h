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

    // chunk has already passed the shared wire decoder (including total and
    // payload length limits); this class owns only contiguous assembly state.
    AppendResult appendChunk(const PlankClipboardChunk& chunk)
    {
        const auto flags = chunk.flags;
        const auto generationValue = chunk.generation;
        const auto totalSizeValue = chunk.total;
        const auto chunkOffset = chunk.offset;
        const auto chunkSize = chunk.size;
        const auto chunkData = chunk.bytes;

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
        plank_clipboard_header(frame.data(), generation, totalSize, offset, chunkSize);
        std::memcpy(frame.data() + PLANK_CLIPBOARD_HEADER_BYTES, text + offset, chunkSize);
        frames.push_back(std::move(frame));
        offset += chunkSize;
    }
    return frames;
}

}  // namespace plank::clipboard
