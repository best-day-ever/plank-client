#pragma once

// The largest HEVC frame this Mac decodes in hardware, per stream profile.
// A display arrangement can make the stream 8K or wider; software decoding
// such a stream is never acceptable, so the planner keeps the workstation
// desktop within what a real test frame proved. Each profile's black 8K IDR
// frames (7680x4320, 8192x4320; decodercaps-test-frames.h) are decoded once
// with VideoToolbox, hardware required, and the answer is cached per chip,
// OS build and profile in the Client's settings.

#include <QSize>
#include <QString>

namespace DecoderCaps {

// No 8K test frame decoded: what the qualified 4K decode path is trusted with.
inline QSize fallbackMaximum()
{
    return QSize(4096, 2304);
}

// The cached maximum for an encoding mode ("hevc-10-444-nvenc",
// "hevc-10-420-nvenc", ...). Invalid when nothing is cached yet or the mode
// has no 8K test (H.264: the host's encoder stops at 4096 anyway).
QSize cachedMaximum(const QString& encodingMode);

// Decodes the test frames now (blocking, any thread), caches and returns the
// result; invalid for modes without a test. macOS only; invalid elsewhere.
QSize probe(const QString& encodingMode);

// cachedMaximum, probing first when nothing is cached.
QSize maximum(const QString& encodingMode);

}
