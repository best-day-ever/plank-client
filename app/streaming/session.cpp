#include "session.h"
#include "stationconnectpacketsize.h"
#include "settings/streamingpreferences.h"
#include "streaming/avsynccontroller.h"
#include "streaming/stationconnectdisplaymode.h"
#include "streaming/stationconnecttoolbar.h"
#include "streaming/streamutils.h"
#include "backend/computermanager.h"

#include <Limelight.h>
#include <SDL.h>
#include "utils.h"

#ifdef HAVE_FFMPEG
#include "video/ffmpeg.h"
#endif

#ifdef HAVE_SLVIDEO
#include "video/slvid.h"
#endif

#ifdef Q_OS_WIN32
// Scaling the icon down on Win32 looks dreadful, so render at lower res
#define ICON_SIZE 32
#else
#define ICON_SIZE 64
#endif

// HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
#include <SDL_syswm.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif


#define SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER 100
#define SDL_CODE_STATIONCONNECT_RECONNECT 105
#define SDL_CODE_STATIONCONNECT_BITRATE_APPLIED 106

#include <openssl/rand.h>

#include <QtEndian>
#include <QCoreApplication>
#include <QThreadPool>
#include <QImage>
#include <QGuiApplication>
#include <QCursor>
#include <QWindow>
#include <QScreen>

#define CONN_TEST_SERVER "qt.conntest.moonlight-stream.org"

CONNECTION_LISTENER_CALLBACKS Session::k_ConnCallbacks = {
    Session::clStageStarting,
    nullptr,
    Session::clStageFailed,
    nullptr,
    Session::clConnectionTerminated,
    Session::clLogMessage,
    nullptr,
    Session::clConnectionStatusUpdate,
    Session::clSetHdrMode,
    nullptr,
    nullptr,
    nullptr,
    Session::clRawHidControl,
    Session::clVideoBitrateApplied,
};

Session* Session::s_ActiveSession;
QSemaphore Session::s_ActiveSessionSemaphore(1);

void Session::clStageStarting(int stage)
{
    // We know this is called on the same thread as LiStartConnection()
    // which happens to be the main thread, so it's cool to interact
    // with the GUI in these callbacks.
    emit s_ActiveSession->stageStarting(QString::fromLocal8Bit(LiGetStageName(stage)));
}

void Session::clStageFailed(int stage, int errorCode)
{
    if (s_ActiveSession->m_Reconnecting.load()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "StationConnect reconnect stage failed: %s (%d)",
                    LiGetStageName(stage), errorCode);
        return;
    }

    // Perform the port test now, while we're on the async connection thread and not blocking the UI.
    unsigned int portFlags = LiGetPortFlagsFromStage(stage);
    s_ActiveSession->m_PortTestResults = LiTestClientConnectivity(CONN_TEST_SERVER, 443, portFlags);

    char failingPorts[128];
    LiStringifyPortFlags(portFlags, ", ", failingPorts, sizeof(failingPorts));
    emit s_ActiveSession->stageFailed(QString::fromLocal8Bit(LiGetStageName(stage)), errorCode, QString(failingPorts));
}

void Session::clConnectionTerminated(int errorCode)
{
    if (s_ActiveSession->m_Computer->stationConnectAuthentication &&
            s_ActiveSession->m_CanReconnect.load() &&
            !s_ActiveSession->m_Reconnecting.load() &&
            !s_ActiveSession->m_ReconnectRequested.exchange(true)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "StationConnect transport ended (%d); requesting bounded reconnect",
                    errorCode);
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_STATIONCONNECT_RECONNECT;
        SDL_PushEvent(&event);
        return;
    }

    if (s_ActiveSession->m_Reconnecting.load()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "StationConnect reconnect attempt ended: %d", errorCode);
        return;
    }

    unsigned int portFlags = LiGetPortFlagsFromTerminationErrorCode(errorCode);
    s_ActiveSession->m_PortTestResults = LiTestClientConnectivity(CONN_TEST_SERVER, 443, portFlags);

    // Display the termination dialog if this was not intended
    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC:
        s_ActiveSession->m_UnexpectedTermination = true;

        char ports[128];
        SDL_assert(portFlags != 0);
        LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
        emit s_ActiveSession->displayLaunchError(tr("No video received from host.") + "\n\n"+
                                                 tr("Check your firewall and port forwarding rules for port(s): %1").arg(ports));
        break;

    case ML_ERROR_NO_VIDEO_FRAME:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                                 tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                                 tr("Try disabling HDR mode, changing the streaming resolution, or changing your host PC's display resolution."));
        break;

    default:
        s_ActiveSession->m_UnexpectedTermination = true;

        // We'll assume large errors are hex values
        bool hexError = qAbs(errorCode) > 1000;
        emit s_ActiveSession->displayLaunchError(tr("Connection terminated") + "\n\n" +
                                                 tr("Error code: %1").arg(errorCode, hexError ? 8 : 0, hexError ? 16 : 10, QChar('0')));
        break;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Connection terminated: %d",
                 errorCode);

    // Push a quit event to the main loop
    SDL_Event event;
    event.type = SDL_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

void Session::clLogMessage(const char* format, ...)
{
    va_list ap;

    va_start(ap, format);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION,
                    SDL_LOG_PRIORITY_INFO,
                    format,
                    ap);
    va_end(ap);
}

void Session::clConnectionStatusUpdate(int connectionStatus)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Connection status update: %d",
                connectionStatus);

    if (!s_ActiveSession->m_Preferences->connectionWarnings) {
        return;
    }

    switch (connectionStatus)
    {
    case CONN_STATUS_POOR:
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
        break;
    case CONN_STATUS_OKAY:
        s_ActiveSession->m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
        break;
    }
}

void Session::clSetHdrMode(bool enabled)
{
    // If we're in the process of recreating our decoder when we get
    // this callback, we'll drop it. The main thread will make the
    // callback when it finishes creating the new decoder.
    if (SDL_AtomicTryLock(&s_ActiveSession->m_DecoderLock)) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            decoder->setHdrMode(enabled);
        }
        SDL_AtomicUnlock(&s_ActiveSession->m_DecoderLock);
    }
}

void Session::clRawHidControl(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession != nullptr && s_ActiveSession->m_InputHandler != nullptr) {
        s_ActiveSession->m_InputHandler->handleRawHidControl(data, length);
    }
}

void Session::clVideoBitrateApplied(
        uint32_t requestedKbps, uint32_t appliedKbps, uint32_t peakKbps)
{
    if (s_ActiveSession == nullptr) {
        return;
    }

    s_ActiveSession->m_ConfirmedBitrateRequestKbps.store(
                static_cast<int>(requestedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitrateAppliedKbps.store(
                static_cast<int>(appliedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitratePeakKbps.store(
                static_cast<int>(peakKbps), std::memory_order_relaxed);

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_STATIONCONNECT_BITRATE_APPLIED;
    SDL_PushEvent(&event);
}

bool Session::chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                            SDL_Window* window, int videoFormat, int width, int height,
                            int frameRate, bool enableVsync, bool enableFramePacing, bool testOnly,
                            IVideoDecoder*& chosenDecoder, bool enableIdentityGbr)
{
    DECODER_PARAMETERS params;

    // We should never have vsync enabled for test-mode.
    // It introduces unnecessary delay for renderers that may
    // block while waiting for a backbuffer swap.
    SDL_assert(!enableVsync || !testOnly);

    params.width = width;
    params.height = height;
    params.frameRate = frameRate;
    params.videoFormat = videoFormat;
    params.window = window;
    params.enableVsync = enableVsync;
    params.enableFramePacing = enableFramePacing;
    params.enableIdentityGbr = enableIdentityGbr;
    params.testOnly = testOnly;
    params.vds = vds;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "V-sync %s",
                enableVsync ? "enabled" : "disabled");

#ifdef HAVE_SLVIDEO
    chosenDecoder = new SLVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SLVideo video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load SLVideo decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#ifdef HAVE_FFMPEG
    chosenDecoder = new FFmpegVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "FFmpeg-based video decoder chosen");
        return true;
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load FFmpeg decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#if !defined(HAVE_FFMPEG) && !defined(HAVE_SLVIDEO)
#error No video decoding libraries available!
#endif

    // If we reach this, we didn't initialize any decoders successfully
    return false;
}

bool Session::isIdentityGbrEnabledForFormat(int videoFormat) const
{
    return (videoFormat == VIDEO_FORMAT_H264_HIGH8_444 ||
            videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT10_444) &&
           (m_Computer->serverCodecModeSupport & SCM_IDENTITY_GBR_444);
}

int Session::drSetup(int videoFormat, int width, int height, int frameRate, void *, int)
{
    s_ActiveSession->m_ActiveVideoFormat = videoFormat;
    s_ActiveSession->m_ActiveVideoWidth = width;
    s_ActiveSession->m_ActiveVideoHeight = height;
    s_ActiveSession->m_ActiveVideoFrameRate = frameRate;

    // Defer decoder setup until we've started streaming so we
    // don't have to hide and show the SDL window (which seems to
    // cause pointer hiding to break on Windows).

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);
    if (s_ActiveSession->isIdentityGbrEnabledForFormat(videoFormat)) {
        if (videoFormat == VIDEO_FORMAT_H264_HIGH8_444) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video precision: native 8-bit RGB -> "
                        "8-bit H.264 4:4:4 -> 8-bit RGB identity presentation");
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Video precision: 8-bit-source/up-converted -> "
                        "10-bit %s 4:4:4 -> 10-bit RGB identity presentation",
                        videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ? "H.264" : "HEVC");
        }
    }

    return 0;
}

int Session::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    // Use a lock since we'll be yanking this decoder out
    // from underneath the session when we initiate destruction.
    // We need to destroy the decoder on the main thread to satisfy
    // some API constraints (like DXVA2). If we can't acquire it,
    // that means the decoder is about to be destroyed, so we can
    // safely return DR_OK and wait for the IDR frame request by
    // the decoder reinitialization code.

    if (SDL_AtomicTryLock(&s_ActiveSession->m_DecoderLock)) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            int ret = decoder->submitDecodeUnit(du);
            SDL_AtomicUnlock(&s_ActiveSession->m_DecoderLock);
            return ret;
        }
        else {
            SDL_AtomicUnlock(&s_ActiveSession->m_DecoderLock);
            return DR_OK;
        }
    }
    else {
        // Decoder is going away. Ignore anything coming in until
        // the lock is released.
        return DR_OK;
    }
}

void Session::getDecoderInfo(SDL_Window* window,
                             bool& isHardwareAccelerated, bool& isFullScreenOnly,
                             QSize& maxResolution)
{
    IVideoDecoder* decoder;

    // Since AV1 support on the host side is in its infancy, let's not consider
    // _only_ a working AV1 decoder to be acceptable and still show the warning
    // dialog indicating lack of hardware decoding support.

    // Try a regular hardware accelerated HEVC decoder now
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_H265, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }


#if 0 // See AV1 comment at the top of this function
    if (chooseDecoder(StreamingPreferences::VDS_FORCE_HARDWARE,
                      window, VIDEO_FORMAT_AV1_MAIN8, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }
#endif

    // If we still didn't find a hardware decoder, try H.264 now.
    // This will fall back to software decoding, so it should always work.
    if (chooseDecoder(StreamingPreferences::VDS_AUTO,
                      window, VIDEO_FORMAT_H264, 1920, 1080, 60,
                      false, false, true, decoder)) {
        isHardwareAccelerated = decoder->isHardwareAccelerated();
        isFullScreenOnly = decoder->isAlwaysFullScreen();
        maxResolution = decoder->getDecoderMaxResolution();
        delete decoder;

        return;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Failed to find ANY working H.264 or HEVC decoder!");
}

Session::DecoderAvailability
Session::getDecoderAvailability(SDL_Window* window,
                                StreamingPreferences::VideoDecoderSelection vds,
                                int videoFormat, int width, int height, int frameRate,
                                bool enableIdentityGbr)
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(vds, window, videoFormat, width, height, frameRate,
                       false, false, true, decoder, enableIdentityGbr)) {
        return DecoderAvailability::None;
    }

    bool hw = decoder->isHardwareAccelerated();

    delete decoder;

    return hw ? DecoderAvailability::Hardware : DecoderAvailability::Software;
}

bool Session::populateDecoderProperties(SDL_Window* window)
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(m_Preferences->videoDecoderSelection,
                       window,
                       m_SupportedVideoFormats.first(),
                       m_StreamConfig.width,
                       m_StreamConfig.height,
                       m_StreamConfig.fps,
                       false, false, true, decoder,
                       isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.first()))) {
        return false;
    }

    m_VideoCallbacks.capabilities = decoder->getDecoderCapabilities();
    if (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) {
        // It is an error to pass a push callback when in pull mode
        m_VideoCallbacks.submitDecodeUnit = nullptr;
    }
    else {
        m_VideoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
    }

    {
        bool ok;

        m_StreamConfig.colorSpace = qEnvironmentVariableIntValue("COLOR_SPACE_OVERRIDE", &ok);
        if (ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Using colorspace override: %d",
                        m_StreamConfig.colorSpace);
        }
        else {
            m_StreamConfig.colorSpace = decoder->getDecoderColorspace();
        }

        m_StreamConfig.colorRange = qEnvironmentVariableIntValue("COLOR_RANGE_OVERRIDE", &ok);
        if (ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Using color range override: %d",
                        m_StreamConfig.colorRange);
        }
        else {
            m_StreamConfig.colorRange = decoder->getDecoderColorRange();
        }
    }

    if (decoder->isAlwaysFullScreen()) {
        m_IsFullScreen = true;
    }

    delete decoder;

    return true;
}

Session::Session(NvComputer* computer, NvApp& app,
                 StreamingPreferences *preferences,
                 ComputerManager *computerManager)
    : m_Preferences(preferences ? preferences : StreamingPreferences::get()),
      m_IsFullScreen(m_Preferences->windowMode != StreamingPreferences::WM_WINDOWED || !WMUtils::isRunningDesktopEnvironment()),
      m_Computer(computer),
      m_ComputerManager(computerManager),
      m_App(app),
      m_Window(nullptr),
      m_VideoDecoder(nullptr),
      m_DecoderLock(0),
      m_AudioMuted(false),
      m_FullScreenFlag(SDL_WINDOW_FULLSCREEN_DESKTOP),
      m_QtWindow(nullptr),
      m_UnexpectedTermination(true), // Failure prior to streaming is unexpected
      m_ReconnectRequested(false),
      m_Reconnecting(false),
      m_CanReconnect(false),
      m_InputHandler(nullptr),
      m_FlushingWindowEventsRef(0),
      m_AsyncConnectionSuccess(false),
      m_PortTestResults(0),
      m_OpusDecoder(nullptr),
      m_AudioRenderer(nullptr),
      m_AudioSampleCount(0),
      m_DropAudioEndTime(0),
      m_AudioMediaFramesReceived(0),
      m_AvSyncTelemetryEnabled(qEnvironmentVariableIntValue("STATIONCONNECT_AV_SYNC_TELEMETRY") > 0),
      m_LastAudioTelemetryTime(0),
      m_CurrentRenderedFps(0.0f),
      m_CurrentVideoMbps(0.0f)
{
    if (m_Computer->stationConnectAuthentication) {
        if (m_ComputerManager != nullptr) {
            m_ComputerManager->takeStationConnectReconnectCredentials(
                        m_Computer, m_StationConnectUsername,
                        m_StationConnectPassword);
            m_CanReconnect.store(!m_StationConnectUsername.isEmpty() &&
                                 !m_StationConnectPassword.isEmpty());
        }
        StationConnectAvSync::resetVideoClock();

        // StationConnect is a qualified workstation protocol, not a generic
        // game-streaming profile. Its stream size is selected after SDL video
        // initialization from the target client display or explicit override.
        // Keep bitrateKbps user-controlled: SettingsView persists the bitrate
        // slider value and initialize() copies it into the stream configuration.
        m_Preferences->fps = 60;
        m_Preferences->identityGbrBitDepth = 10;
        m_Preferences->videoCodecConfig = StreamingPreferences::VCC_FORCE_H264;
        // Intel's hardware path cannot decode the qualified H.264 High 10
        // 4:4:4 identity profile. Keep StationConnect on the proven FFmpeg
        // software path instead of allowing automatic selection to downgrade
        // the stream to an 8-bit profile.
        m_Preferences->videoDecoderSelection = StreamingPreferences::VDS_FORCE_SOFTWARE;
    }
}

Session::~Session()
{
    clearStationConnectReconnectCredentials();
}

void Session::clearStationConnectReconnectCredentials()
{
    m_CanReconnect.store(false);
    m_StationConnectPassword.fill(QChar('\0'));
    m_StationConnectPassword.clear();
    m_StationConnectUsername.clear();
}

bool Session::initialize()
{
#ifdef Q_OS_DARWIN
    if (qEnvironmentVariableIntValue("I_WANT_BUGGY_FULLSCREEN") == 0) {
        // If we have a notch and the user specified one of the two native display modes
        // (notched or notchless), override the fullscreen mode to ensure it works as expected.
        // - SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=0 will place the video underneath the notch
        // - SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1 will place the video below the notch
        bool shouldUseFullScreenSpaces = true;
        SDL_DisplayMode desktopMode;
        SDL_Rect safeArea;
        for (int displayIndex = 0; StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea); displayIndex++) {
            // Check if this display has a notch (safeArea != desktopMode)
            if (desktopMode.h != safeArea.h || desktopMode.w != safeArea.w) {
                // Check if we're trying to stream at the full native resolution (including notch)
                if (m_Preferences->width == desktopMode.w && m_Preferences->height == desktopMode.h) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Overriding default fullscreen mode for native fullscreen resolution");
                    shouldUseFullScreenSpaces = false;
                    break;
                }
                else if (m_Preferences->width == safeArea.w && m_Preferences->height == safeArea.h) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Overriding default fullscreen mode for native safe area resolution");
                    shouldUseFullScreenSpaces = true;
                    break;
                }
            }
        }

        // Using modesetting on modern versions of macOS is extremely unreliable
        // and leads to hangs, deadlocks, and other nasty stuff. The only time
        // people seem to use it is to get the full screen on notched Macs,
        // which setting SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES=1 also accomplishes
        // with much less headache.
        //
        // https://github.com/moonlight-stream/moonlight-qt/issues/973
        // https://github.com/moonlight-stream/moonlight-qt/issues/999
        // https://github.com/moonlight-stream/moonlight-qt/issues/1211
        // https://github.com/moonlight-stream/moonlight-qt/issues/1218
        SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, shouldUseFullScreenSpaces ? "1" : "0");
    }
#endif

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return false;
    }

    const QSize stationConnectResolution = m_Computer->stationConnectAuthentication ?
                                               configureStationConnectDisplayMode() : QSize();

    LiInitializeStreamConfiguration(&m_StreamConfig);
    m_StreamConfig.width = stationConnectResolution.isValid() ?
                               stationConnectResolution.width() : m_Preferences->width;
    m_StreamConfig.height = stationConnectResolution.isValid() ?
                                stationConnectResolution.height() : m_Preferences->height;

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

    // Create a hidden window to use for decoder initialization tests
    SDL_Window* testWindow = SDL_CreateWindow("", x, y, width, height,
                                              SDL_WINDOW_HIDDEN | StreamUtils::getPlatformWindowFlags());
    if (!testWindow) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create test window with platform flags: %s",
                    SDL_GetError());

        testWindow = SDL_CreateWindow("", x, y, width, height, SDL_WINDOW_HIDDEN);
        if (!testWindow) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create window for hardware decode test: %s",
                         SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
    }

    qInfo() << "Server GPU:" << m_Computer->gpuModel;
    qInfo() << "Server GFE version:" << m_Computer->gfeVersion;

    LiInitializeVideoCallbacks(&m_VideoCallbacks);
    m_VideoCallbacks.setup = drSetup;

    m_StreamConfig.fps = m_Preferences->fps;
    m_StreamConfig.bitrate = m_Preferences->bitrateKbps;

#ifndef STEAM_LINK
    // Opt-in to all encryption features if we detect that the platform
    // has AES cryptography acceleration instructions and more than 2 cores.
    if (StreamUtils::hasFastAes() && SDL_GetCPUCount() > 2) {
        m_StreamConfig.encryptionFlags = ENCFLG_ALL;
    }
    else {
        // Enable audio encryption as long as we're not on Steam Link.
        // That hardware can hardly handle Opus decoding at all.
        m_StreamConfig.encryptionFlags = ENCFLG_AUDIO;
    }
#endif

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Video bitrate: %d kbps",
                m_StreamConfig.bitrate);

    RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesKey),
               sizeof(m_StreamConfig.remoteInputAesKey));

    // Only the first 4 bytes are populated in the RI key IV
    RAND_bytes(reinterpret_cast<unsigned char*>(m_StreamConfig.remoteInputAesIv), 4);

    switch (m_Preferences->audioConfig)
    {
    case StreamingPreferences::AC_STEREO:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        break;
    case StreamingPreferences::AC_51_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
        break;
    case StreamingPreferences::AC_71_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
        break;
    }

    LiInitializeAudioCallbacks(&m_AudioCallbacks);
    m_AudioCallbacks.init = arInit;
    m_AudioCallbacks.cleanup = arCleanup;
    m_AudioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
    m_AudioCallbacks.capabilities = getAudioRendererCapabilities(m_StreamConfig.audioConfiguration);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel count: %d",
                CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio channel mask: %X",
                CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));

    // StationConnect has one qualified identity profile. Do not silently
    // downgrade to 8-bit or 4:2:0 when decoder probing fails.
    if (m_Preferences->identityGbrBitDepth == 10 &&
            isIdentityGbrEnabledForFormat(VIDEO_FORMAT_H264_HIGH10_444)) {
        m_SupportedVideoFormats.append(VIDEO_FORMAT_H264_HIGH10_444);
    }

    switch (m_Preferences->videoCodecConfig)
    {
    case StreamingPreferences::VCC_AUTO:
    {
        // Codecs are checked in order of ascending decode complexity to ensure
        // the the deprioritized list prefers lighter codecs for software decoding

        // H.264 is already the lowest priority codec, so we don't need to do
        // any probing for deprioritization for it here.

        const bool identityGbr = isIdentityGbrEnabledForFormat(VIDEO_FORMAT_H265_REXT10_444);
        auto hevcDA = getDecoderAvailability(testWindow,
                                             m_Preferences->videoDecoderSelection,
                                             identityGbr ? VIDEO_FORMAT_H265_REXT10_444 : VIDEO_FORMAT_H265_REXT8_444,
                                             m_StreamConfig.width,
                                             m_StreamConfig.height,
                                             m_StreamConfig.fps,
                                             identityGbr);

        if (hevcDA != DecoderAvailability::Hardware) {
            m_SupportedVideoFormats.deprioritizeByMask(VIDEO_FORMAT_MASK_H265);
        }

        // AV1 host support remains less broadly available than H.264/HEVC.
        m_SupportedVideoFormats.deprioritizeByMask(VIDEO_FORMAT_MASK_AV1);

#ifdef Q_OS_DARWIN
        {
            // Prior to GFE 3.11, GFE did not allow us to constrain
            // the number of reference frames, so we have to fixup the SPS
            // to allow decoding via VideoToolbox on macOS. Since we don't
            // have fixup code for HEVC, just avoid it if GFE is too old.
            QVector<int> gfeVersion = NvHTTP::parseQuad(m_Computer->gfeVersion);
            if (gfeVersion.isEmpty() || // Very old versions don't have GfeVersion at all
                    gfeVersion[0] < 3 ||
                    (gfeVersion[0] == 3 && gfeVersion[1] < 11)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Disabling HEVC on macOS due to old GFE version");
                m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_H265);
            }
        }
#endif
        break;
    }
    case StreamingPreferences::VCC_FORCE_H264:
        m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_H264);
        break;
    case StreamingPreferences::VCC_FORCE_HEVC:
    case StreamingPreferences::VCC_FORCE_HEVC_HDR_DEPRECATED:
        m_SupportedVideoFormats.removeByMask(~VIDEO_FORMAT_MASK_H265);
        break;
    case StreamingPreferences::VCC_FORCE_AV1:
        // We'll try to fall back to HEVC first if AV1 fails. We'd rather not fall back
        // straight to H.264 if the user asked for AV1 and the host doesn't support it.
        m_SupportedVideoFormats.removeByMask(~(VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_H265));
        break;
    }

    SDL_assert((m_SupportedVideoFormats & ~VIDEO_FORMAT_MASK_YUV444) == 0);

#if !SDL_VERSION_ATLEAST(2, 0, 11)
    // HACK: Using a full-screen window breaks mouse capture on the Pi's LXDE
    // GUI environment. Force the session to use windowed mode (which won't
    // really matter anyway because the MMAL renderer always draws full-screen).
    if (qgetenv("DESKTOP_SESSION") == "LXDE-pi") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Forcing windowed mode on LXDE-Pi");
        m_FullScreenFlag = 0;
    }
#endif

    // Check for validation errors/warnings and emit
    // signals for them, if appropriate
    bool ret = validateLaunch(testWindow);

    if (ret) {
        // Video format is now locked in
        m_StreamConfig.supportedVideoFormats = m_SupportedVideoFormats.front();

        // Populate decoder-dependent properties.
        // Must be done after validateLaunch() since m_StreamConfig is finalized.
        ret = populateDecoderProperties(testWindow);
    }

    SDL_DestroyWindow(testWindow);

    if (!ret) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    return true;
}

void Session::emitLaunchWarning(QString text)
{
    // Emit the warning to the UI
    emit displayLaunchWarning(text);

    // Wait a little bit so the user can actually read what we just said.
    // This wait is a little longer than the actual toast timeout (3 seconds)
    // to allow it to transition off the screen before continuing.
    uint32_t start = SDL_GetTicks();
    while (!SDL_TICKS_PASSED(SDL_GetTicks(), start + 3500)) {
        SDL_Delay(5);

        if (!m_ThreadedExec) {
            // Pump the UI loop while we wait if we're on the main thread
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }
    }
}

bool Session::validateLaunch(SDL_Window* testWindow)
{
    if (!m_Computer->isSupportedServerVersion) {
        emit displayLaunchError(tr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(m_Computer->name));
        return false;
    }

    if (m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_FORCE_SOFTWARE) {
        emitLaunchWarning(tr("Your settings selection to force software decoding may cause poor streaming performance."));
    }

    if (m_SupportedVideoFormats & VIDEO_FORMAT_MASK_AV1) {
        if (m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport & SCM_MASK_AV1) == 0) {
            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
                emitLaunchWarning(tr("Your host software or GPU doesn't support encoding AV1."));
            }

            // Moonlight-common-c will handle this case already, but we want
            // to set this explicitly here so we can do our hardware acceleration
            // check below.
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_AV1);
        }
    }

    if (m_SupportedVideoFormats & VIDEO_FORMAT_MASK_H265) {
        if (m_Computer->maxLumaPixelsHEVC == 0) {
            if (m_Preferences->videoCodecConfig == StreamingPreferences::VCC_FORCE_HEVC) {
                emitLaunchWarning(tr("Your host PC doesn't support encoding HEVC."));
            }

            // Moonlight-common-c will handle this case already, but we want
            // to set this explicitly here so we can do our hardware acceleration
            // check below.
            m_SupportedVideoFormats.removeByMask(VIDEO_FORMAT_MASK_H265);
        }
    }

    if (!(m_Computer->serverCodecModeSupport & SCM_MASK_YUV444)) {
        emit displayLaunchError(tr("This host does not support StationConnect's required 4:4:4 video profile."));
        return false;
    }

    m_SupportedVideoFormats.removeByMask(
                ~m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport));
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("The selected codec has no StationConnect 4:4:4 profile shared by this host and client."));
        return false;
    }

    // Automatic decoder selection may legitimately choose software for a
    // profile that the GPU cannot decode. That is an expected capability
    // result, so do not interrupt each connection with a warning.
    while (!m_SupportedVideoFormats.isEmpty()) {
        const auto availability = getDecoderAvailability(
                    testWindow,
                    m_Preferences->videoDecoderSelection,
                    m_SupportedVideoFormats.front(),
                    m_StreamConfig.width,
                    m_StreamConfig.height,
                    m_StreamConfig.fps,
                    isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.front()));
        if (availability == DecoderAvailability::None ||
                (m_Preferences->videoDecoderSelection == StreamingPreferences::VDS_FORCE_HARDWARE &&
                 availability != DecoderAvailability::Hardware)) {
            m_SupportedVideoFormats.removeFirst();
        }
        else {
            break;
        }
    }
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("This client cannot decode any StationConnect 4:4:4 profile offered by the host."));
        return false;
    }

    if (m_StreamConfig.width >= 3840) {
        // Only allow 4K on GFE 3.x+
        if (m_Computer->gfeVersion.isEmpty() || m_Computer->gfeVersion.startsWith("2.")) {
            emitLaunchWarning(tr("GeForce Experience 3.0 or higher is required for 4K streaming."));

            m_StreamConfig.width = 1920;
            m_StreamConfig.height = 1080;
        }
    }

    // Test if audio works at the specified audio configuration
    bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);

    // Gracefully degrade to stereo if surround sound doesn't work
    if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration) > 2) {
        audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
        if (audioTestPassed) {
            m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
        }
    }

    // If nothing worked, warn the user that audio will not work
    if (!audioTestPassed) {
        emitLaunchWarning(tr("Failed to open audio device. Audio will be unavailable during this session."));
    }

    // NVENC will fail to initialize when any dimension exceeds 4096 using:
    // - H.264 on all versions of NVENC
    // - HEVC prior to Pascal
    //
    // However, if we aren't using Nvidia hosting software, don't assume anything about
    // encoding capabilities by using HEVC Main 10 support. It will likely be wrong.
    if ((m_StreamConfig.width > 4096 || m_StreamConfig.height > 4096) && m_Computer->isNvidiaServerSoftware) {
        // Pascal added support for 8K HEVC encoding support. Maxwell 2 could encode HEVC but only up to 4K.
        // We can't directly identify Pascal, but we can look for HEVC Main10 which was added in the same generation.
        if (m_Computer->maxLumaPixelsHEVC == 0 || !(m_Computer->serverCodecModeSupport & SCM_HEVC_MAIN10)) {
            emit displayLaunchError(tr("Your host PC's GPU doesn't support streaming video resolutions over 4K."));
            return false;
        }
        else if ((m_SupportedVideoFormats & ~VIDEO_FORMAT_MASK_H264) == 0) {
            emit displayLaunchError(tr("Video resolutions over 4K are not supported by the H.264 codec."));
            return false;
        }
    }

    return true;
}


class DeferredSessionCleanupTask : public QRunnable
{
public:
    DeferredSessionCleanupTask(Session* session) :
        m_Session(session) {}

private:
    virtual ~DeferredSessionCleanupTask() override
    {
        // Allow another session to start now that we're cleaned up
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();

        // Notify that the session is ready to be cleaned up
        emit m_Session->readyForDeletion();
    }

    void run() override
    {
        emit m_Session->sessionFinished(m_Session->m_PortTestResults);

        // The video decoder must already be destroyed, since it could
        // try to interact with APIs that can only be called between
        // LiStartConnection() and LiStopConnection().
        SDL_assert(m_Session->m_VideoDecoder == nullptr);

        // Finish cleanup of the connection state
        LiStopConnection();

    }

    Session* m_Session;
};

int Session::getTargetDisplayIndex() const
{
    int displayIndex = 0;

    if (m_Window != nullptr) {
        displayIndex = SDL_GetWindowDisplayIndex(m_Window);
        if (displayIndex < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SDL_GetWindowDisplayIndex() failed: %s",
                        SDL_GetError());
            displayIndex = 0;
        }
    }
    // Create our window on the same display that Qt's UI
    // was being displayed on.
    else {
        if (m_QtWindow != nullptr) {
            QScreen* screen = m_QtWindow->screen();
            if (screen != nullptr) {
                QRect displayRect = screen->geometry();

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt UI screen is at (%d,%d)",
                            displayRect.x(), displayRect.y());
                for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
                    SDL_Rect displayBounds;

                    if (SDL_GetDisplayBounds(i, &displayBounds) == 0) {
                        if (displayBounds.x == displayRect.x() &&
                            displayBounds.y == displayRect.y()) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "SDL found matching display %d",
                                        i);
                            displayIndex = i;
                            break;
                        }
                    }
                    else {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "SDL_GetDisplayBounds(%d) failed: %s",
                                    i, SDL_GetError());
                    }
                }
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Qt window is not associated with a QScreen!");
            }
        }
    }

    return displayIndex;
}

QSize Session::configureStationConnectDisplayMode()
{
    const int displayIndex = getTargetDisplayIndex();
    SDL_DisplayMode nativeMode;
    SDL_Rect safeArea;
    QSize detectedResolution;

    if (StreamUtils::getNativeDesktopMode(displayIndex, &nativeMode, &safeArea)) {
        detectedResolution = QSize(safeArea.w, safeArea.h);
    }
    else if (SDL_GetDesktopDisplayMode(displayIndex, &nativeMode) == 0) {
        detectedResolution = QSize(nativeMode.w, nativeMode.h);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Using current desktop mode because native display detection failed");
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Client display resolution detection failed: %s",
                    SDL_GetError());
    }

    const QSize configuredResolution(m_Preferences->width, m_Preferences->height);
    QSize exactNativeResolution;
    {
        QReadLocker lock(&m_Computer->lock);
        if (m_Computer->selectedDisplayMode == NvOutputTopology::ScaledSpanMode) {
            exactNativeResolution = QSize(m_Computer->outputTopology.desktopWidth,
                                          m_Computer->outputTopology.desktopHeight);
        }
        else if (m_Computer->selectedDisplayMode == NvOutputTopology::SingleOutputMode) {
            for (const NvOutput& output : m_Computer->outputTopology.outputs) {
                if (output.id == m_Computer->selectedOutputId) {
                    exactNativeResolution = QSize(output.width, output.height);
                    break;
                }
            }
        }
    }
    const QSize selectedResolution = StationConnectDisplayMode::resolve(
        m_Preferences->stationConnectAutoResolution,
        detectedResolution,
        configuredResolution,
        exactNativeResolution);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "StationConnect client resolution: detected=%dx%d host-native=%dx%d configured=%dx%d selected=%dx%d mode=%s",
                detectedResolution.width(), detectedResolution.height(),
                exactNativeResolution.width(), exactNativeResolution.height(),
                configuredResolution.width(), configuredResolution.height(),
                selectedResolution.width(), selectedResolution.height(),
                m_Preferences->stationConnectAutoResolution ? "auto" : "override");
    return selectedResolution;
}

void Session::getWindowDimensions(int& x, int& y,
                                  int& width, int& height)
{
    const int displayIndex = getTargetDisplayIndex();

    SDL_Rect usableBounds;
    if (SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0) {
        // Don't use more than 80% of the display to leave room for system UI
        // and ensure the target size is not odd (otherwise one of the sides
        // of the image will have a one-pixel black bar next to it).
        SDL_Rect src, dst;
        src.x = src.y = dst.x = dst.y = 0;
        src.w = m_StreamConfig.width;
        src.h = m_StreamConfig.height;
        dst.w = ((int)SDL_ceilf(usableBounds.w * 0.80f) & ~0x1);
        dst.h = ((int)SDL_ceilf(usableBounds.h * 0.80f) & ~0x1);

        // Scale the window size while preserving aspect ratio
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // If the stream window can fit within the usable drawing area with 1:1
        // scaling, do that rather than filling the screen.
        if (m_StreamConfig.width < dst.w && m_StreamConfig.height < dst.h) {
            width = m_StreamConfig.width;
            height = m_StreamConfig.height;
        }
        else {
            width = dst.w;
            height = dst.h;
        }
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetDisplayUsableBounds() failed: %s",
                     SDL_GetError());

        width = m_StreamConfig.width;
        height = m_StreamConfig.height;
    }

    x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
}

void Session::updateOptimalWindowDisplayMode()
{
    SDL_DisplayMode desktopMode, bestMode, mode;
    int displayIndex = SDL_GetWindowDisplayIndex(m_Window);

    // Try the current display mode first. On macOS, this will be the normal
    // scaled desktop resolution setting.
    if (SDL_GetDesktopDisplayMode(displayIndex, &desktopMode) == 0) {
        // If this doesn't fit the selected resolution, use the native
        // resolution of the panel (unscaled).
        if (desktopMode.w < m_ActiveVideoWidth || desktopMode.h < m_ActiveVideoHeight) {
            SDL_Rect safeArea;
            if (!StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
                return;
            }
        }
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_GetDesktopDisplayMode() failed: %s",
                    SDL_GetError());
        return;
    }

    // Start with the native desktop resolution and try to find
    // the highest refresh rate that our stream FPS evenly divides.
    bestMode = desktopMode;
    bestMode.refresh_rate = 0;
    for (int i = 0; i < SDL_GetNumDisplayModes(displayIndex); i++) {
        if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
            if (mode.w == desktopMode.w && mode.h == desktopMode.h &&
                    mode.refresh_rate % m_StreamConfig.fps == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Found display mode with desktop resolution: %dx%dx%d",
                            mode.w, mode.h, mode.refresh_rate);
                if (mode.refresh_rate > bestMode.refresh_rate) {
                    bestMode = mode;
                }
            }
        }
    }

    // If we didn't find a mode that matched the current resolution and
    // had a high enough refresh rate, start looking for lower resolution
    // modes that can meet the required refresh rate and minimum video
    // resolution. We will also try to pick a display mode that matches
    // aspect ratio closest to the video stream.
    if (bestMode.refresh_rate == 0) {
        float bestModeAspectRatio = 0;
        float videoAspectRatio = (float)m_ActiveVideoWidth / (float)m_ActiveVideoHeight;
        for (int i = 0; i < SDL_GetNumDisplayModes(displayIndex); i++) {
            if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
                float modeAspectRatio = (float)mode.w / (float)mode.h;
                if (mode.w >= m_ActiveVideoWidth && mode.h >= m_ActiveVideoHeight &&
                        mode.refresh_rate % m_StreamConfig.fps == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Found display mode with video resolution: %dx%dx%d",
                                mode.w, mode.h, mode.refresh_rate);
                    if (mode.refresh_rate >= bestMode.refresh_rate &&
                            (bestModeAspectRatio == 0 || fabs(videoAspectRatio - modeAspectRatio) <= fabs(videoAspectRatio - bestModeAspectRatio))) {
                        bestMode = mode;
                        bestModeAspectRatio = modeAspectRatio;
                    }
                }
            }
        }
    }

    if (bestMode.refresh_rate == 0) {
        // We may find no match if the user has moved a 120 FPS
        // stream onto a 60 Hz monitor (since no refresh rate can
        // divide our FPS setting). We'll stick to the default in
        // this case.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No matching display mode found; using desktop mode");
        bestMode = desktopMode;
    }

    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        // Only print when the window is actually in full-screen exclusive mode,
        // otherwise we're not actually using the mode we've set here
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Chosen best display mode: %dx%dx%d",
                    bestMode.w, bestMode.h, bestMode.refresh_rate);
    }

    SDL_SetWindowDisplayMode(m_Window, &bestMode);
}

void Session::toggleFullscreen()
{
    bool fullScreen = !(SDL_GetWindowFlags(m_Window) & m_FullScreenFlag);

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
    // Destroy the video decoder before toggling full-screen because D3D9 can try
    // to put the window back into full-screen before we've managed to destroy
    // the renderer. This leads to excessive flickering and can cause the window
    // decorations to get messed up as SDL and D3D9 fight over the window style.
    //
    // On Apple Silicon Macs, the AVSampleBufferDisplayLayer may cause WindowServer
    // to deadlock when transitioning out of fullscreen. Destroy the decoder before
    // exiting fullscreen as a workaround. See issue #973.
    SDL_AtomicLock(&m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_AtomicUnlock(&m_DecoderLock);
#endif

    // Actually enter/leave fullscreen
    SDL_SetWindowFullscreen(m_Window, fullScreen ? m_FullScreenFlag : 0);

    // Input handler might need to start/stop keyboard grab after changing modes
    m_InputHandler->updateKeyboardGrabState();

    // Input handler might need stop/stop mouse grab after changing modes
    m_InputHandler->updatePointerRegionLock();
}

class AsyncConnectionStartThread : public QThread
{
public:
    AsyncConnectionStartThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Async Conn Start");
    }

    void run() override
    {
        m_Session->m_AsyncConnectionSuccess = m_Session->startConnectionAsync();
    }

    Session* m_Session;
};

// Called in a non-main thread
bool Session::startConnectionAsync(bool reconnecting)
{
    // Wait 1.5 seconds before connecting to let the user
    // have time to read any messages present on the segue
    if (!reconnecting) {
        SDL_Delay(1500);
    }

    // StationConnect never terminates a host application remotely. Only resume
    // the already-running Desktop application or launch it from an idle host.
    Q_ASSERT(m_Computer->currentGameId == 0 ||
             m_Computer->currentGameId == m_App.id);

    QString rtspSessionUrl;

    try {
        NvHTTP http(m_Computer);
        const auto startApp = [&]() {
            http.startApp(m_Computer->currentGameId != 0 ? "resume" : "launch",
                          m_App.id, &m_StreamConfig,
                          m_Preferences->playAudioOnHost,
                          0,
                          false,
                          m_Computer->selectedOutputId,
                          m_Computer->selectedDisplayMode,
                          m_Computer->outputTopology.generation,
                          m_Computer->stationConnectTopologyVersion,
                          m_Computer->stationConnectFeatureFlags &
                              NvOutputTopology::SupportedFeatureFlags,
                          rtspSessionUrl);
        };
        try {
            startApp();
        } catch (const GfeHttpResponseException& e) {
            if (reconnecting && m_Computer->stationConnectAuthentication &&
                    m_Computer->currentGameId == 0 &&
                    e.getStatusCode() == 400) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = m_App.id;
                }
                qInfo() << "StationConnect worker already has an active Desktop stream; resuming it";
                startApp();
            }
            else if (reconnecting && m_Computer->stationConnectAuthentication &&
                    m_Computer->currentGameId != 0 &&
                    e.getStatusCode() == 503) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = 0;
                }
                qInfo() << "StationConnect replacement worker has no app to resume; "
                           "launching a fresh Desktop stream";
                startApp();
            }
            else {
                const bool generationBinding =
                        (m_Computer->stationConnectFeatureFlags &
                         NvOutputTopology::TopologyGenerationFeature) != 0;
                if (!m_Computer->stationConnectAuthentication || !generationBinding ||
                        e.getStatusCode() != 409) {
                    throw;
                }

                const NvOutputTopology topology = http.getOutputTopology();
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->outputTopology = topology;
                    m_Computer->selectedOutputId =
                            topology.selectOutput(m_Computer->selectedOutputId);
                    m_Computer->selectedDisplayMode =
                            topology.selectDisplayMode(m_Computer->selectedDisplayMode);
                }
                if (m_ComputerManager != nullptr) {
                    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                }
                qInfo() << "StationConnect refreshed stale topology generation and will retry launch:"
                        << topology.generation;
                startApp();
            }
        }

        // Record the successful launch immediately. If low-level transport
        // setup fails afterward, the next bounded attempt must resume this
        // app instead of issuing a second launch request.
        if (m_Computer->stationConnectAuthentication) {
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->currentGameId = m_App.id;
        }

        if (m_Computer->stationConnectAuthentication) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
            if (m_ComputerManager != nullptr) {
                m_ComputerManager->clientSideAttributeUpdated(m_Computer);
            }
            qInfo() << "StationConnect authentication token consumed after launch";
        }
    } catch (const GfeHttpResponseException& e) {
        if (!reconnecting) {
            emit displayLaunchError(tr("Host returned error: %1").arg(e.toQString()));
        } else {
            qWarning() << "StationConnect reconnect launch failed:" << e.toQString();
        }
        return false;
    } catch (const QtNetworkReplyException& e) {
        if (!reconnecting) {
            emit displayLaunchError(e.toQString());
        } else {
            qWarning() << "StationConnect reconnect transport setup failed:"
                       << e.toQString();
        }
        return false;
    }

    QByteArray hostnameStr = m_Computer->activeAddress.address().toLatin1();
    QByteArray siAppVersion = m_Computer->appVersion.toLatin1();

    SERVER_INFORMATION hostInfo;
    hostInfo.address = hostnameStr.data();
    hostInfo.serverInfoAppVersion = siAppVersion.data();
    hostInfo.serverCodecModeSupport = m_Computer->serverCodecModeSupport;

    // Older GFE versions didn't have this field
    QByteArray siGfeVersion;
    if (!m_Computer->gfeVersion.isEmpty()) {
        siGfeVersion = m_Computer->gfeVersion.toLatin1();
    }
    if (!siGfeVersion.isEmpty()) {
        hostInfo.serverInfoGfeVersion = siGfeVersion.data();
    }

    // Older GFE and Sunshine versions didn't have this field
    QByteArray rtspSessionUrlStr;
    if (!rtspSessionUrl.isEmpty()) {
        rtspSessionUrlStr = rtspSessionUrl.toLatin1();
        hostInfo.rtspSessionUrl = rtspSessionUrlStr.data();
    }

    if (m_Preferences->networkMtu != 0) {
        // Derive a fragmentation-safe, 16-byte-aligned video packet size from
        // the configured physical path MTU and conservative VPN framing.
        // NB: Using STREAM_CFG_AUTO will cap our packet size at 1024 for remote hosts.
        m_StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
        m_StreamConfig.packetSize = StationConnectPacketSize::videoPacketSizeForPhysicalMtu(
                    m_Preferences->networkMtu);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using configured physical MTU %d: video packet size %d bytes",
                    m_Preferences->networkMtu,
                    m_StreamConfig.packetSize);
    }
    else {
        // Use 1392 byte video packets by default
        m_StreamConfig.packetSize = 1392;

        // getActiveAddressReachability() does network I/O, so we only attempt to check
        // reachability if we've already contacted the PC successfully.
        switch (m_Computer->getActiveAddressReachability()) {
        case NvComputer::RI_LAN:
            // This address is on-link, so treat it as a local address
            // even if it's not in RFC 1918 space or it's an IPv6 address.
            m_StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
            break;
        case NvComputer::RI_VPN:
            // Keep the encrypted inner IPv4 packet inside one ZeroTier physical
            // payload, including RTP, UDP/IP, and extended-frame overhead.
            // Treat it as remote even if the target address is in RFC 1918 address space.
            m_StreamConfig.streamingRemotely = STREAM_CFG_REMOTE;
            m_StreamConfig.packetSize = StationConnectPacketSize::VpnVideoPacketSize;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using StationConnect VPN packet size: %d bytes",
                        m_StreamConfig.packetSize);
            break;
        default:
            // If we don't have reachability info, let moonlight-common-c decide.
            m_StreamConfig.streamingRemotely = STREAM_CFG_AUTO;
            break;
        }
    }

    // moonlight-common-c fills missing callbacks in the caller-owned table.
    // Restore the pull/push decoder contract before every reuse of this
    // Session for a StationConnect desktop handoff.
    m_VideoCallbacks.submitDecodeUnit =
            (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) ?
                nullptr : drSubmitDecodeUnit;

    int err = LiStartConnection(&hostInfo, &m_StreamConfig, &k_ConnCallbacks,
                                &m_VideoCallbacks, &m_AudioCallbacks,
                                NULL, 0, NULL, 0);
    if (err != 0) {
        // We already displayed an error dialog in the stage failure
        // listener.
        return false;
    }

    emit connectionStarted();
    return true;
}

bool Session::reconnectStationConnect()
{
    if (m_StationConnectUsername.isEmpty() ||
            m_StationConnectPassword.isEmpty()) {
        return false;
    }

    m_Reconnecting.store(true);
    m_OverlayManager.updateOverlayText(
                Overlay::OverlayStatusUpdate,
                "Reconnecting to workstation...");
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);

    m_InputHandler->raiseAllKeys();
    SDL_AtomicLock(&m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_AtomicUnlock(&m_DecoderLock);
    LiStopConnection();

    constexpr int MaximumAttempts = 20;
    for (int attempt = 1; attempt <= MaximumAttempts; ++attempt) {
        try {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                // A replacement media worker has no in-memory app state.
                // Prefer a fresh launch; startConnectionAsync() falls back to
                // resume when this is merely a transient same-worker outage.
                m_Computer->currentGameId = 0;
            }
            NvHTTP http(m_Computer);
            const QString token = http.authenticate(
                        m_StationConnectUsername,
                        m_StationConnectPassword);

            NvOutputTopology topology;
            const bool topologySupported =
                    m_Computer->stationConnectTopologyVersion ==
                        NvOutputTopology::ProtocolVersion &&
                    (m_Computer->stationConnectFeatureFlags &
                     (NvOutputTopology::OutputTopologyFeature |
                      NvOutputTopology::SelectedOutputFeature |
                      NvOutputTopology::UnifiedAbsoluteInputFeature)) ==
                    (NvOutputTopology::OutputTopologyFeature |
                     NvOutputTopology::SelectedOutputFeature |
                     NvOutputTopology::UnifiedAbsoluteInputFeature);
            if (topologySupported) {
                topology = http.getOutputTopology();
            }
            const QVector<NvApp> apps = http.getAppList();
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken = token;
                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                if (topologySupported) {
                    m_Computer->outputTopology = topology;
                    m_Computer->selectedOutputId =
                            topology.selectOutput(m_Computer->selectedOutputId);
                    m_Computer->selectedDisplayMode =
                            topology.selectDisplayMode(
                                m_Computer->selectedDisplayMode);
                }
                m_Computer->updateAppList(apps);
            }

            if (startConnectionAsync(true)) {
                m_InputHandler->resetRawHidAfterReconnect();
                m_Reconnecting.store(false);
                m_ReconnectRequested = false;
                m_UnexpectedTermination = false;
                m_OverlayManager.setOverlayState(
                            Overlay::OverlayStatusUpdate, false);

                SDL_Event resetEvent = {};
                resetEvent.type = SDL_RENDER_DEVICE_RESET;
                SDL_PushEvent(&resetEvent);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "StationConnect reconnect completed on attempt %d",
                            attempt);
                return true;
            }
        } catch (const GfeHttpResponseException& error) {
            qWarning() << "StationConnect reauthentication attempt" << attempt
                       << "failed:" << error.toQString();
        } catch (const QtNetworkReplyException& error) {
            qWarning() << "StationConnect reconnect attempt" << attempt
                       << "could not reach the host:" << error.toQString();
        }

        LiStopConnection();
        {
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->sessionToken.fill(QChar('\0'));
            m_Computer->sessionToken.clear();
            m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        }
        if (attempt != MaximumAttempts) {
            SDL_Delay(1000);
        }
    }

    m_Reconnecting.store(false);
    m_ReconnectRequested = false;
    m_UnexpectedTermination = true;
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
    return false;
}

void Session::flushWindowEvents()
{
    // Pump events to ensure all pending OS events are posted
    SDL_PumpEvents();

    // Insert a barrier to discard any additional window events.
    // We don't use SDL_FlushEvent() here because it could cause
    // important events to be lost.
    m_FlushingWindowEventsRef++;

    // This event will cause us to set m_FlushingWindowEvents back to false.
    SDL_Event flushEvent = {};
    flushEvent.type = SDL_USEREVENT;
    flushEvent.user.code = SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER;
    SDL_PushEvent(&flushEvent);
}

class ExecThread : public QThread
{
public:
    ExecThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Session Exec");
    }

    void run() override
    {
        m_Session->execInternal();
    }

    Session* m_Session;
};

void Session::exec(QWindow* qtWindow)
{
    m_QtWindow = qtWindow;

    // Use a separate thread for the streaming session on X11 or Wayland
    // to ensure we don't stomp on Qt's GL context. This breaks when using
    // the Qt EGLFS backend, so we will restrict this to X11
    m_ThreadedExec = WMUtils::isRunningX11() || WMUtils::isRunningWayland();

    if (m_ThreadedExec) {
        // Run the streaming session on a separate thread for Linux/BSD
        ExecThread execThread(this);
        execThread.start();

        // Until the SDL streaming window is created, we should continue
        // to update the Qt UI to allow warning messages to display and
        // make sure that the Qt window can hide itself.
        while (!execThread.wait(10) && m_Window == nullptr) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();

        // SDL is in charge now. Wait until the streaming thread exits
        // to further update the Qt window.
        execThread.wait();
    }
    else {
        // Run the streaming session on the main thread for Windows and macOS
        execInternal();
    }
}

void Session::execInternal()
{
    // Complete initialization in this deferred context to avoid
    // calling expensive functions in the constructor (during the
    // process of loading the StreamSegue).
    //
    // NB: This initializes the SDL video subsystem, so it must be
    // called on the main thread.
    if (!initialize()) {
        emit sessionFinished(0);
        emit readyForDeletion();
        return;
    }

    // Wait for any old session to finish cleanup
    s_ActiveSessionSemaphore.acquire();

    // We're now active
    s_ActiveSession = this;

    // Initialize input before starting the connection.
    // StationConnect is a remote-desktop product. Like RGS desktop mode, use
    // authoritative absolute coordinates and reserve relative capture for a
    // distinct game-mode path. This also gives receiver UI exact hit testing.
    m_InputHandler = new SdlInputHandler(*m_Preferences,
                                         m_StreamConfig.width,
                                         m_StreamConfig.height);

    AsyncConnectionStartThread asyncConnThread(this);
    if (!m_ThreadedExec) {
        // Kick off the async connection thread while we sit here and pump the event loop
        asyncConnThread.start();
        while (!asyncConnThread.wait(10)) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents();
        }

        // Pump the event loop one last time to ensure we pick up any events from
        // the thread that happened while it was in the final successful QThread::wait().
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents();
    }
    else {
        // We're already in a separate thread so run the connection operations
        // synchronously and don't pump the event loop. The main thread is already
        // pumping the event loop for us.
        asyncConnThread.run();
    }

    // If the connection failed, clean up and abort the connection.
    if (!m_AsyncConnectionSuccess) {
        delete m_InputHandler;
        m_InputHandler = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
        return;
    }

    int x, y, width, height;
    getWindowDimensions(x, y, width, height);

#ifdef STEAM_LINK
    // We need a little delay before creating the window or we will trigger some kind
    // of graphics driver bug on Steam Link that causes a jagged overlay to appear in
    // the top right corner randomly.
    SDL_Delay(500);
#endif

    // Request at least 8 bits per color for GL
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    // We always want a resizable window with High DPI enabled
    Uint32 defaultWindowFlags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;

    // We use only the computer name on macOS to match Apple conventions where the
    // app name is featured in the menu bar and the document name is in the title bar.
#ifdef Q_OS_DARWIN
    std::string windowName = QString(m_Computer->name).toStdString();
#else
    std::string windowName = QString(m_Computer->name + " - StationConnect").toStdString();
#endif

    m_Window = SDL_CreateWindow(windowName.c_str(),
                                x,
                                y,
                                width,
                                height,
                                defaultWindowFlags | StreamUtils::getPlatformWindowFlags());
    if (!m_Window) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());

        m_Window = SDL_CreateWindow(windowName.c_str(),
                                    x,
                                    y,
                                    width,
                                    height,
                                    defaultWindowFlags);
        if (!m_Window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow() failed: %s",
                         SDL_GetError());

            delete m_InputHandler;
            m_InputHandler = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
            return;
        }
    }

    if (!m_IsFullScreen) {
        // Windowed means a normal compositor-managed desktop window. Do not
        // inherit a maximized launcher state that can make it indistinguishable
        // from borderless mode on Wayland.
        SDL_SetWindowFullscreen(m_Window, 0);
        SDL_RestoreWindow(m_Window);
        SDL_SetWindowBordered(m_Window, SDL_TRUE);
        SDL_SetWindowResizable(m_Window, SDL_TRUE);
    }

    // HACK: Remove once proper Dark Mode support lands in SDL
#ifdef Q_OS_WIN32
    if (m_QtWindow != nullptr) {
        BOOL darkModeEnabled;

        // Query whether dark mode is enabled for our Qt window (which tracks the OS dark mode state)
        if (FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled))) &&
            FAILED(DwmGetWindowAttribute((HWND)m_QtWindow->winId(), DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled)))) {
            darkModeEnabled = FALSE;
        }

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);

        if (SDL_GetWindowWMInfo(m_Window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
            // If dark mode is enabled, propagate that to our SDL window
            if (darkModeEnabled) {
                if (FAILED(DwmSetWindowAttribute(info.info.win.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkModeEnabled, sizeof(darkModeEnabled)))) {
                    DwmSetWindowAttribute(info.info.win.window, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &darkModeEnabled, sizeof(darkModeEnabled));
                }

                // Toggle non-client rendering off and back on to ensure dark mode takes effect on Windows 10.
                // DWM doesn't seem to correctly invalidate the non-client area after enabling dark mode.
                DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_DISABLED;
                DwmSetWindowAttribute(info.info.win.window, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
                ncPolicy = DWMNCRP_ENABLED;
                DwmSetWindowAttribute(info.info.win.window, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
            }
        }
    }
#endif

    m_InputHandler->setWindow(m_Window);

    QImage iconImage(":/res/stationconnect-logo.png");
    iconImage = iconImage.scaled(ICON_SIZE,
                                 ICON_SIZE,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_RGBA8888);
    SDL_Surface* iconSurface = SDL_CreateRGBSurfaceWithFormatFrom((void*)iconImage.constBits(),
                                                                  iconImage.width(),
                                                                  iconImage.height(),
                                                                  32,
                                                                  4 * iconImage.width(),
                                                                  SDL_PIXELFORMAT_RGBA32);
#ifndef Q_OS_DARWIN
    // Other platforms seem to preserve our Qt icon when creating a new window.
    if (iconSurface != nullptr) {
        // This must be called before entering full-screen mode on Windows
        // or our icon will not persist when toggling to windowed mode
        SDL_SetWindowIcon(m_Window, iconSurface);
    }
#endif

    // Update the window display mode based on our current monitor
    // for if/when we enter full-screen mode.
    updateOptimalWindowDisplayMode();

    // Enter full screen if requested
    if (m_IsFullScreen) {
        SDL_SetWindowFullscreen(m_Window, m_FullScreenFlag);
    }

    bool needsFirstEnterCapture = false;
    bool needsPostDecoderCreationCapture = false;

    // HACK: For Wayland, we wait until we get the first SDL_WINDOWEVENT_ENTER
    // event where it seems to work consistently on GNOME. For other platforms,
    // especially where SDL may call SDL_RecreateWindow(), we must only capture
    // after the decoder is created.
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        // Native Wayland: Capture on SDL_WINDOWEVENT_ENTER
        needsFirstEnterCapture = true;
    }
    else {
        // X11/XWayland: Capture after decoder creation
        needsPostDecoderCreationCapture = true;
    }

    // Stop text input. SDL enables it by default
    // when we initialize the video subsystem, but this
    // causes an IME popup when certain keys are held down
    // on macOS.
    SDL_StopTextInput();

    // Disable the screen saver if requested
    if (m_Preferences->keepAwake) {
        SDL_DisableScreenSaver();
    }

    // Hide Qt's fake mouse cursor on EGLFS systems
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    }

    // Set timer resolution to 1 ms on Windows for greater
    // sleep precision and more accurate callback timing.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");

    int currentDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);

    // Now that we're about to stream, any SDL_QUIT event is expected
    // unless it comes from the connection termination callback where
    // (m_UnexpectedTermination is set back to true).
    m_UnexpectedTermination = false;

    // Toggle the stats overlay if requested by the user
    m_OverlayManager.setOverlayState(Overlay::OverlayDebug, m_Preferences->showPerformanceOverlay);

    if (m_Computer->stationConnectAuthentication) {
        m_StationConnectToolbar.reset(new StationConnectToolbar(
                    m_Window, m_OverlayManager, *m_InputHandler, *m_Preferences));
    }

    // Hijack this thread to be the SDL main thread. We have to do this
    // because we want to suspend all Qt processing until the stream is over.
    SDL_Event event;
    for (;;) {
        if (m_StationConnectToolbar) {
            m_StationConnectToolbar->setRenderedStats(
                        m_CurrentRenderedFps.load(std::memory_order_relaxed),
                        m_CurrentVideoMbps.load(std::memory_order_relaxed));
            m_StationConnectToolbar->update(SDL_GetTicks());
        }
#if SDL_VERSION_ATLEAST(2, 0, 18) && !defined(STEAM_LINK)
        // SDL 2.0.18 has a proper wait event implementation that uses platform
        // support to block on events rather than polling on Windows, macOS, X11,
        // and Wayland. It will fall back to 1 ms polling if a joystick is
        // connected, so we don't use it for STEAM_LINK to ensure we only poll
        // every 10 ms.
        //
        // NB: This behavior was introduced in SDL 2.0.16, but had a few critical
        // issues that could cause indefinite timeouts, delayed joystick detection,
        // and other problems.
        const int eventWaitTimeout = m_StationConnectToolbar ?
                    m_StationConnectToolbar->eventWaitTimeout() : 1000;
        if (!SDL_WaitEventTimeout(&event, eventWaitTimeout)) {
            continue;
        }
#else
        // We explicitly use SDL_PollEvent() and SDL_Delay() because
        // SDL_WaitEvent() has an internal SDL_Delay(10) inside which
        // blocks this thread too long for high polling rate mice and high
        // refresh rate displays.
        if (!SDL_PollEvent(&event)) {
#ifndef STEAM_LINK
            SDL_Delay(1);
#else
            // Waking every 1 ms to process input is too much for the low performance
            // ARM core in the Steam Link, so we will wait 10 ms instead.
            SDL_Delay(10);
#endif
            continue;
        }
#endif
        switch (event.type) {
        case SDL_QUIT:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Quit event received");
            goto DispatchDeferredCleanup;

        case SDL_USEREVENT:
            switch (event.user.code) {
            case SDL_CODE_STATIONCONNECT_RECONNECT:
                if (!reconnectStationConnect()) {
                    emit displayLaunchError(
                                tr("The workstation desktop changed, but the client could not reconnect within 20 seconds."));
                    goto DispatchDeferredCleanup;
                }
                break;
            case SDL_CODE_STATIONCONNECT_BITRATE_APPLIED:
                if (m_StationConnectToolbar) {
                    m_StationConnectToolbar->setAppliedBitrate(
                                m_ConfirmedBitrateRequestKbps.load(std::memory_order_relaxed),
                                m_ConfirmedBitrateAppliedKbps.load(std::memory_order_relaxed),
                                m_ConfirmedBitratePeakKbps.load(std::memory_order_relaxed));
                }
                break;
            case SDL_CODE_FRAME_READY:
                if (m_VideoDecoder != nullptr) {
                    m_VideoDecoder->renderFrameOnMainThread();
                }
                break;
            case SDL_CODE_FLUSH_WINDOW_EVENT_BARRIER:
                m_FlushingWindowEventsRef--;
                break;
            default:
                SDL_assert(false);
            }
            break;

        case SDL_WINDOWEVENT:
            if (m_StationConnectToolbar &&
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                m_StationConnectToolbar->notifyWindowChanged();
            }
            // Early handling of some events
            switch (event.window.event) {
            case SDL_WINDOWEVENT_FOCUS_LOST:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = true;
                }
                m_InputHandler->notifyFocusLost();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                if (m_Preferences->muteOnFocusLoss) {
                    m_AudioMuted = false;
                }
                m_InputHandler->notifyFocusGained();
                break;
            case SDL_WINDOWEVENT_LEAVE:
                m_InputHandler->notifyMouseLeave();
                break;
            }


            // Capture the mouse on SDL_WINDOWEVENT_ENTER if needed
            if (needsFirstEnterCapture && event.window.event == SDL_WINDOWEVENT_ENTER) {
                m_InputHandler->setCaptureActive(true);
                needsFirstEnterCapture = false;
            }

            // We want to recreate the decoder for resizes (full-screen toggles) and the initial shown event.
            // We use SDL_WINDOWEVENT_SIZE_CHANGED rather than SDL_WINDOWEVENT_RESIZED because the latter doesn't
            // seem to fire when switching from windowed to full-screen on X11.
            if (event.window.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
                (event.window.event != SDL_WINDOWEVENT_SHOWN || m_VideoDecoder != nullptr)) {
                // Check that the window display hasn't changed. If it has, we want
                // to recreate the decoder to allow it to adapt to the new display.
                // This will allow Pacer to pull the new display refresh rate.
#if SDL_VERSION_ATLEAST(2, 0, 18)
                // On SDL 2.0.18+, there's an event for this specific situation
                if (event.window.event != SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    break;
                }
#else
                // Prior to SDL 2.0.18, we must check the display index for each window event
                if (SDL_GetWindowDisplayIndex(m_Window) == currentDisplayIndex) {
                    break;
                }
#endif
            }
#ifdef Q_OS_WIN32
            // We can get a resize event after being minimized. Recreating the renderer at that time can cause
            // us to start drawing on the screen even while our window is minimized. Minimizing on Windows also
            // moves the window to -32000, -32000 which can cause a false window display index change. Avoid
            // that whole mess by never recreating the decoder if we're minimized.
            else if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
                break;
            }
#endif

            if (m_FlushingWindowEventsRef > 0) {
                // Ignore window events for renderer reset if flushing
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Dropping window event during flush: %d (%d %d)",
                            event.window.event,
                            event.window.data1,
                            event.window.data2);
                break;
            }

            // Allow the renderer to handle the state change without being recreated
            if (m_VideoDecoder) {
                bool forceRecreation = false;

                WINDOW_STATE_CHANGE_INFO windowChangeInfo = {};
                windowChangeInfo.window = m_Window;

                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_SIZE;

                    windowChangeInfo.width = event.window.data1;
                    windowChangeInfo.height = event.window.data2;
                }

                int newDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);
                if (newDisplayIndex != currentDisplayIndex) {
                    windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_DISPLAY;

                    windowChangeInfo.displayIndex = newDisplayIndex;

                    // If the refresh rates have changed, we will need to go through the full
                    // decoder recreation path to ensure Pacer is switched to the new display
                    // and that we apply any V-Sync disablement rules that may be needed for
                    // this display.
                    SDL_DisplayMode oldMode, newMode;
                    if (SDL_GetCurrentDisplayMode(currentDisplayIndex, &oldMode) < 0 ||
                            SDL_GetCurrentDisplayMode(newDisplayIndex, &newMode) < 0 ||
                            oldMode.refresh_rate != newMode.refresh_rate) {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Forcing renderer recreation due to refresh rate change between displays");
                        forceRecreation = true;
                    }
                }

                if (!forceRecreation && m_VideoDecoder->notifyWindowChanged(&windowChangeInfo)) {
                    // Update the window display mode based on our current monitor
                    // NB: Avoid a useless modeset by only doing this if it changed.
                    if (newDisplayIndex != currentDisplayIndex) {
                        currentDisplayIndex = newDisplayIndex;
                        updateOptimalWindowDisplayMode();
                    }

                    break;
                }
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Recreating renderer for window event: %d (%d %d)",
                        event.window.event,
                        event.window.data1,
                        event.window.data2);

            // Fall through
        case SDL_RENDER_DEVICE_RESET:
        case SDL_RENDER_TARGETS_RESET:

            if (event.type != SDL_WINDOWEVENT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Recreating renderer by internal request: %d",
                            event.type);
            }

            SDL_AtomicLock(&m_DecoderLock);

            // Destroy the old decoder
            delete m_VideoDecoder;

            // Insert a barrier to discard any additional window events
            // that could cause the renderer to be and recreated again.
            // We don't use SDL_FlushEvent() here because it could cause
            // important events to be lost.
            flushWindowEvents();

            // Update the window display mode based on our current monitor
            // NB: Avoid a useless modeset by only doing this if it changed.
            if (currentDisplayIndex != SDL_GetWindowDisplayIndex(m_Window)) {
                currentDisplayIndex = SDL_GetWindowDisplayIndex(m_Window);
                updateOptimalWindowDisplayMode();
            }

            // Now that the old decoder is dead, flush any events it may
            // have queued to reset itself (if this reset was the result
            // of state loss).
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_RENDER_DEVICE_RESET);
            SDL_FlushEvent(SDL_RENDER_TARGETS_RESET);

            {
                // If the stream exceeds the display refresh rate (plus some slack),
                // forcefully disable V-sync to allow the stream to render faster
                // than the display.
                int displayHz = StreamUtils::getDisplayRefreshRate(m_Window);
                bool enableVsync = m_Preferences->enableVsync;
                if (displayHz + 5 < m_StreamConfig.fps) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Disabling V-sync because refresh rate limit exceeded");
                    enableVsync = false;
                }

                // Choose a new decoder (hopefully the same one, but possibly
                // not if a GPU was removed or something).
                if (!chooseDecoder(m_Preferences->videoDecoderSelection,
                                   m_Window, m_ActiveVideoFormat, m_ActiveVideoWidth,
                                   m_ActiveVideoHeight, m_ActiveVideoFrameRate,
                                   enableVsync,
                                   enableVsync && m_Preferences->framePacing,
                                   false,
                                   s_ActiveSession->m_VideoDecoder,
                                   isIdentityGbrEnabledForFormat(m_ActiveVideoFormat))) {
                    SDL_AtomicUnlock(&m_DecoderLock);
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Failed to recreate decoder after reset");
                    emit displayLaunchError(tr("Unable to initialize video decoder. Please check your streaming settings and try again."));
                    goto DispatchDeferredCleanup;
                }

                // As of SDL 2.0.12, SDL_RecreateWindow() doesn't carry over mouse capture
                // or mouse hiding state to the new window. By capturing after the decoder
                // is set up, this ensures the window re-creation is already done.
                if (needsPostDecoderCreationCapture) {
                    m_InputHandler->setCaptureActive(true);
                    needsPostDecoderCreationCapture = false;
                }
            }

            // Request an IDR frame to complete the reset
            LiRequestIdrFrame();

            // Set HDR mode. We may miss the callback if we're in the middle
            // of recreating our decoder at the time the HDR transition happens.
            m_VideoDecoder->setHdrMode(LiGetCurrentHostDisplayHdrMode());

            // The replacement renderer has no copy of the prior overlay
            // texture, so publish the toolbar surface again after recreation.
            if (m_StationConnectToolbar) {
                m_StationConnectToolbar->notifyWindowChanged();
            }

            // After a window resize, we need to reset the pointer lock region
            m_InputHandler->updatePointerRegionLock();

            SDL_AtomicUnlock(&m_DecoderLock);
            break;

        case SDL_KEYUP:
        case SDL_KEYDOWN:
            m_InputHandler->handleKeyEvent(&event.key);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (m_StationConnectToolbar) {
                const auto action = m_StationConnectToolbar->handleMouseButton(event.button);
                if (action == StationConnectToolbar::Action::Disconnect) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "StationConnect toolbar disconnect requested");
                    goto DispatchDeferredCleanup;
                }
                if (action == StationConnectToolbar::Action::ToggleFullscreen) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "StationConnect toolbar fullscreen toggle requested");
                    toggleFullscreen();
                    m_StationConnectToolbar->notifyWindowChanged();
                    break;
                }
                if (action == StationConnectToolbar::Action::Minimize) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "StationConnect toolbar minimize requested");
                    SDL_MinimizeWindow(m_Window);
                    break;
                }
                if (action == StationConnectToolbar::Action::Consumed) {
                    break;
                }
            }
            m_InputHandler->handleMouseButtonEvent(&event.button);
            break;
        case SDL_MOUSEMOTION:
            if (m_StationConnectToolbar) {
                // The ordinary input path batches queued motion for efficient
                // transport. Aggregate it here when the toolbar is present so
                // the toolbar tracker and host receive the identical delta.
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    SDL_Event nextMotionEvent;
                    while (SDL_PeepEvents(&nextMotionEvent, 1, SDL_GETEVENT,
                                          SDL_MOUSEMOTION,
                                          SDL_MOUSEMOTION) > 0) {
                        if (nextMotionEvent.motion.which != SDL_TOUCH_MOUSEID) {
                            event.motion.timestamp =
                                    nextMotionEvent.motion.timestamp;
                            event.motion.x = nextMotionEvent.motion.x;
                            event.motion.y = nextMotionEvent.motion.y;
                            event.motion.xrel += nextMotionEvent.motion.xrel;
                            event.motion.yrel += nextMotionEvent.motion.yrel;
                        }
                    }
                }
                if (m_StationConnectToolbar->handleMouseMotion(event.motion)) {
                    break;
                }
            }
            m_InputHandler->handleMouseMotionEvent(
                        &event.motion, !m_StationConnectToolbar);
            break;
        case SDL_MOUSEWHEEL:
            if (m_StationConnectToolbar &&
                    m_StationConnectToolbar->handleMouseWheel(event.wheel)) {
                break;
            }
            m_InputHandler->handleMouseWheelEvent(&event.wheel);
            break;
        }
    }

DispatchDeferredCleanup:
    // Uncapture the mouse and hide the window immediately,
    // so we can return to the Qt GUI ASAP.
    m_InputHandler->setCaptureActive(false);
    SDL_EnableScreenSaver();
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
    if (QGuiApplication::platformName() == "eglfs") {
        QGuiApplication::restoreOverrideCursor();
    }

    // Raise any keys that are still down
    m_InputHandler->raiseAllKeys();

    // Destroy the input handler now. This must be destroyed
    // before allowing the UI to continue execution.
    delete m_InputHandler;
    m_InputHandler = nullptr;
    clearStationConnectReconnectCredentials();

    m_StationConnectToolbar.reset();

    // Destroy the decoder, since this must be done on the main thread
    // NB: This must happen before LiStopConnection() for pull-based
    // decoders.
    SDL_AtomicLock(&m_DecoderLock);
    delete m_VideoDecoder;
    m_VideoDecoder = nullptr;
    SDL_AtomicUnlock(&m_DecoderLock);

    // Propagate state changes from the SDL window back to the Qt window
    //
    // NB: We're making a conscious decision not to propagate the maximized
    // or normal state of the window here. The thinking is that users may
    // routinely maximize the streaming window simply to view the stream
    // in a larger window, but they don't necessarily want the UI in such
    // a large window.
    if (!m_IsFullScreen && m_QtWindow != nullptr && m_Window != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() | Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowStates() & Qt::WindowMinimized) {
            m_QtWindow->setWindowStates(m_QtWindow->windowStates() & ~Qt::WindowMinimized);
        }
#else
        if (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) {
            m_QtWindow->setWindowState(Qt::WindowMinimized);
        }
        else if (m_QtWindow->windowState() & Qt::WindowMinimized) {
            m_QtWindow->setWindowState(Qt::WindowNoState);
        }
#endif
    }

    // This must be called after the decoder is deleted, because
    // the renderer may want to interact with the window
    SDL_DestroyWindow(m_Window);

    if (iconSurface != nullptr) {
        SDL_FreeSurface(iconSurface);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    // Cleanup can take a while, so dispatch it to a worker thread.
    // When it is complete, it will release our s_ActiveSessionSemaphore
    // reference.
    QThreadPool::globalInstance()->start(new DeferredSessionCleanupTask(this));
}
