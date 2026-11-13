#pragma once

#include <QJsonObject>
#include <QString>
#include <atomic>
#include <cstdint>

// Only fixed vocabulary and numeric errors may leave the session. Never copy
// SDL/decoder messages: those can contain local paths or endpoint identities.
class SessionDiagnostics {
public:
    enum class Phase { Prepared, Initializing, Negotiating, TransportReady,
                       CreatingWindow, WindowCreated, CreatingDecoder, DecoderReady };
    enum class End { Unknown, InitializationFailed, StartupFailed, Cancelled,
                     WindowCreationFailed, DecoderFailed, DisplayFailed,
                     QuitEvent, WindowClose, Hotkey, Toolbar, ApplicationExit,
                     ThreadInterrupted, ConnectionTerminated, SessionTakenOver,
                     ReconnectFailed, ReconnectTimeout, SessionDestroyed, FrameFormatChanged };

    void phase(Phase phase) { m_Phase.store(phase); }
    void windowCreated() { m_WindowCreated.store(true); phase(Phase::WindowCreated); }
    void decoderReady() { m_DecoderReady.store(true); phase(Phase::DecoderReady); }
    void videoReceived() { m_VideoReceived.store(true, std::memory_order_relaxed); }
    void rendererProgress() { m_RendererProgress.store(true, std::memory_order_relaxed); }
    void end(End reason, int code = 0) {
        // Preserve the initiating cause when cleanup itself triggers a normal
        // transport callback, cancellation or a second window-close event.
        std::uint64_t empty = 0;
        const auto value = (std::uint64_t(reason) << 32) | std::uint32_t(code);
        m_End.compare_exchange_strong(empty, value);
    }
    QJsonObject snapshot() const {
        static const char* const phases[] = {"prepared", "initializing", "negotiating", "transport_ready",
            "creating_window", "window_created", "creating_decoder", "decoder_ready"};
        static const char* const reasons[] = {"unknown", "initialization_failed", "startup_failed", "cancelled",
            "window_creation_failed", "decoder_failed", "display_failed", "quit_event", "window_close",
            "hotkey", "toolbar", "application_exit", "thread_interrupted", "connection_terminated",
            "session_taken_over", "reconnect_failed", "reconnect_timeout", "session_destroyed", "frame_format_changed"};
        const auto end = m_End.load();
        return {{"phase", QString::fromLatin1(phases[int(m_Phase.load())])},
                {"termination_reason", QString::fromLatin1(reasons[end >> 32])},
                {"error_code", int(std::int32_t(end & 0xffffffffu))},
                {"window_created", m_WindowCreated.load()}, {"decoder_ready", m_DecoderReady.load()},
                {"video_received", m_VideoReceived.load(std::memory_order_relaxed)},
                {"renderer_progress", m_RendererProgress.load(std::memory_order_relaxed)}};
    }
private:
    std::atomic<Phase> m_Phase{Phase::Prepared};
    std::atomic<std::uint64_t> m_End{0};
    std::atomic_bool m_WindowCreated{false}, m_DecoderReady{false}, m_VideoReceived{false}, m_RendererProgress{false};
};
