#pragma once

#include <atomic>
#include <cstdint>

// One stream per process. These independent snapshots remain readable when a
// decoder or GPU call holds its own locks. They are local diagnostics only:
// returning from a renderer is not proof that a monitor displayed the frame.
namespace PlankVideoPipeline {
enum class Stage : std::uint8_t {
    Unknown, WaitingInput, SubmitPacket, ReceiveOutput, SubmitPacer,
    BetweenCalls, WaitRenderer, RenderFrame, RenderReturned, WaitRenderQueue,
    PreparePacket, CollectStatistics
};
inline std::atomic<std::uint64_t> decodeOperation {0}, renderOperation {0};
inline std::atomic<std::uint64_t> decodedFrames {0}, rendererReturns {0};

inline void record(std::atomic<std::uint64_t>& lane, Stage stage, std::uint64_t nowMs)
{
    // One atomic publishes the operation and its entry time together.
    lane.store((nowMs << 8) | static_cast<std::uint8_t>(stage), std::memory_order_relaxed);
}
inline void reset()
{
    decodeOperation.store(0, std::memory_order_relaxed);
    renderOperation.store(0, std::memory_order_relaxed);
    decodedFrames.store(0, std::memory_order_relaxed);
    rendererReturns.store(0, std::memory_order_relaxed);
}
struct Operation {
    Stage stage;
    std::uint64_t ageMs;
};
inline Operation read(const std::atomic<std::uint64_t>& lane, std::uint64_t nowMs)
{
    const auto value = lane.load(std::memory_order_relaxed);
    const auto entered = value >> 8;
    return {static_cast<Stage>(value & 0xff), value == 0 || nowMs < entered ? 0 : nowMs - entered};
}
inline const char* name(Stage stage)
{
    switch (stage) {
    case Stage::WaitingInput: return "waiting-input";
    case Stage::SubmitPacket: return "send-packet";
    case Stage::ReceiveOutput: return "receive-output";
    case Stage::SubmitPacer: return "submit-pacer";
    case Stage::BetweenCalls: return "between-calls";
    case Stage::WaitRenderer: return "wait-renderer";
    case Stage::RenderFrame: return "render-frame";
    case Stage::RenderReturned: return "render-returned";
    case Stage::WaitRenderQueue: return "wait-render-queue";
    case Stage::PreparePacket: return "prepare-packet";
    case Stage::CollectStatistics: return "collect-statistics";
    default: return "unknown";
    }
}
}
