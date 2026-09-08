#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

// Numeric-only, bounded diagnostic storage. No frame ownership or playback policy.
class FrameFlowBuffer {
public:
    struct Row {
        uint64_t timeNs;
        int64_t ptsUs;
        int64_t frame;
        int stage;
        int key;
        uint64_t bytes;
        int depth;
        uint64_t durationNs;
    };
    static constexpr size_t Capacity = 49152;
    static constexpr uint64_t WindowNs = 120000000000ULL;

    explicit FrameFlowBuffer(bool enabled) : m_Enabled(enabled) {}
    bool enabled() const { return m_Enabled; }

    void record(Row row) {
        if (!m_Enabled) return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Started) {
            m_Started = true;
            m_Origin = row.timeNs;
            m_Rows.reserve(Capacity);
        }
        // A delayed producer can hold an earlier timestamp than the first writer.
        if (row.timeNs >= m_Origin && row.timeNs - m_Origin < WindowNs &&
                m_Rows.size() < Capacity) {
            m_Rows.push_back(row);
        }
    }

    // Caller must stop producers before flushing. Reusable after decoder reset.
    std::vector<Row> take() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<Row> rows;
        rows.swap(m_Rows);
        m_Started = false;
        m_Origin = 0;
        return rows;
    }

private:
    const bool m_Enabled;
    bool m_Started = false;
    uint64_t m_Origin = 0;
    std::mutex m_Mutex;
    std::vector<Row> m_Rows;
};
