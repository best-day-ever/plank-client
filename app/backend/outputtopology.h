#pragma once

#include "displayarrangement.h"

#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

struct NvOutput
{
    QString id;
    QString name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;
    int refreshMillihz = 0;
    bool primary = false;
    bool virtualOutput = false;
    QString configuredMode;
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    // Display arrangement (0x8000000) only: how the host backs this output
    // (physical, physical-viewport, virtual) and its entry in the live
    // arrangement request, -1 outside one.
    QString backing;
    int arrangementIndex = -1;
    // Display arrangement only: the output's rectangle in the encoded
    // capture (capture_rect); source_rect unless the capture is packed.
    int captureX = 0;
    int captureY = 0;
    int captureWidth = 0;
    int captureHeight = 0;

    QRect captureRect() const
    {
        return captureWidth > 0 && captureHeight > 0 ? QRect(captureX, captureY, captureWidth, captureHeight) :
                                                       QRect(sourceX, sourceY, sourceWidth, sourceHeight);
    }
};

struct NvClientDisplay
{
    QRect bounds;
    QSize nativeSize;     // physical panel pixels where the platform knows them
    QSize backingSize {}; // macOS current compositor pixels; absent on other platforms
    QSize fullscreenSize {}; // macOS native-fullscreen viewport in backing pixels: the desktop below the
                             // camera housing on notched MacBooks (e.g. 3024x1890 on a 14" panel)
    // Probe v2 (display setup). Appended so existing aggregate initialisers keep their meaning.
    QString key {};        // stable monitor identity: "uuid:<UUID>", else vendor/model/serial
    QString name {};       // what the system calls the monitor ("LG UltraFine", "Built-in Retina Display")
    bool builtIn = false;  // the laptop's own panel
    bool main = false;     // the menu-bar (primary) display
    bool mirrored = false; // other displays mirror this one; they are collapsed into it
    int refreshMillihz = 0; // 0 when unknown
    int rotation = 0;      // degrees clockwise
    bool notch = false;    // a camera housing cuts into the top of the panel
    quint32 platformId = 0; // session-local platform display id (CGDirectDisplayID); never persisted
};

struct NvOutputTopology
{
    static const int ProtocolVersion = 13;
    static const int OutputTopologyFeature = 0x1;
    static const int SelectedOutputFeature = 0x2;
    static const int UnifiedAbsoluteInputFeature = 0x4;
    static const int ScaledSpanFeature = 0x8;
    static const int TopologyGenerationFeature = 0x10;
    static const int HostLayoutMetadataFeature = 0x20;
    static const int CompositeSourceRegionsFeature = 0x40;
    static const int HostLayoutBindingFeature = 0x80;
    static const int IndependentVirtualModesFeature = 0x100;
    static const int DynamicHostLayoutFeature = 0x200;
    static const int TemporaryPhysicalLayoutFeature = 0x400;
    static const int CaptureSourceSelectionFeature = 0x800;
    static const int EncoderBackendSelectionFeature = 0x1000;
    static const int NvfbcHevc10NvencFeature = 0x2000;
    static const int FixedTransportMtuFeature = 0x4000;
    static const int SessionTakeoverFeature = 0x8000;
    static const int DesktopHandoffNoticeFeature = 0x10000;
    static const int AuthenticatedDesktopStageFeature = 0x20000;
    static const int WorkerInstanceFeature = 0x40000;
    // Fixed capture description only. Not part of the Linux launch feature
    // mask: parsing this does not grant input, layout changes or media launch.
    static const int FixedCaptureFeature = 0x80000;
    static const int MacDesktopPreparationFeature = 0x100000;
    static const int MacEncodingProfileFeature = 0x200000;
    // Single-user Linux desktop: a launch refused because another account
    // owns the desktop names that owner and may offer to sign it out.
    static const int DesktopSignOutFeature = 0x400000;
    static const int ClipboardSyncFeature = 0x800000;
    static const int ClipboardFilesFeature = 0x1000000;
    // Only advertise a clipboard receiver/sender when this client implements
    // it. Linux must not cause the host to read or transmit unused clipboard data.
#ifdef Q_OS_MACOS
    static const int PlatformClipboardSyncFeature = ClipboardSyncFeature;
    static const int PlatformClipboardFilesFeature = ClipboardFilesFeature;
#else
    static const int PlatformClipboardSyncFeature = 0;
    static const int PlatformClipboardFilesFeature = 0;
#endif
    // Linux host: NvFBC into direct NVENC 4:2:0 (H.264 High 8-bit, HEVC
    // Main10), limited-range BT.709, for bandwidth-limited links.
    static const int NvfbcNvenc420Feature = 0x2000000;
    // Virtual modes matching a notched Apple laptop's fullscreen viewport
    // (3024x1890). Offered and accepted only when the host advertises it.
    static const int NotchSafeLaptopModesFeature = 0x4000000;
    // Linux host: up to four desktop outputs of any size in client positions
    // (plankDisplayArrangement), backed by a physical output at an exact mode
    // or a virtual display. protocol/output-topology.md.
    static const int DisplayArrangementFeature = DisplayArrangement::Feature;
    static const int FixedCaptureFlags = FixedCaptureFeature | OutputTopologyFeature |
            TopologyGenerationFeature | HostLayoutMetadataFeature | CompositeSourceRegionsFeature |
            MacDesktopPreparationFeature | MacEncodingProfileFeature;
    static const int MaximumVirtualCanvasWidth = 8192;
    static const int SupportedFeatureFlags = OutputTopologyFeature |
                                             SelectedOutputFeature |
                                             UnifiedAbsoluteInputFeature |
                                             ScaledSpanFeature |
                                             TopologyGenerationFeature |
                                             HostLayoutMetadataFeature |
                                             CompositeSourceRegionsFeature |
                                             HostLayoutBindingFeature |
                                             IndependentVirtualModesFeature |
                                             DynamicHostLayoutFeature |
                                             TemporaryPhysicalLayoutFeature |
                                             CaptureSourceSelectionFeature |
                                             EncoderBackendSelectionFeature |
                                             NvfbcHevc10NvencFeature |
                                             FixedTransportMtuFeature |
                                             SessionTakeoverFeature |
                                             DesktopHandoffNoticeFeature |
                                             AuthenticatedDesktopStageFeature |
                                             WorkerInstanceFeature |
                                             DesktopSignOutFeature |
                                             PlatformClipboardSyncFeature |
                                             PlatformClipboardFilesFeature |
                                             NvfbcNvenc420Feature |
                                             NotchSafeLaptopModesFeature |
                                             DisplayArrangementFeature;
    static const char* NativeScalingMode;
    static const char* ScaledSpanMode;
    static const char* MatchClientHostLayout;
    static const char* PhysicalHostLayout;
    static const char* SingleHostLayout;
    static const char* DualHorizontalHostLayout;

    // Discovery decides whether to fetch topology; the authenticated JSON
    // parser still validates the complete platform-specific contract.
    static bool supportsDescription(int version, int featureFlags);
    // UI hint only: unknown=0, Linux=1, macOS=2. Never authorization.
    static int hostPlatform(int version, int featureFlags);
    static bool fromJson(const QJsonObject& object, NvOutputTopology& topology,
                         QString* error = nullptr);
    // XRandR can briefly report no outputs while the host applies a mode.
    // Only this exact empty snapshot is retryable; all other invalid topology
    // documents remain protocol errors.
    static bool temporarilyEmpty(const QJsonObject& object);
    // The display arrangement fields (feature 0x8000000), strict.
    static bool parseDisplayArrangement(const QJsonObject& object, const QJsonObject& layout,
                                        NvOutputTopology& topology, QString* error);
    QJsonObject toJson() const;

    // Match client displays on a Linux host. Each display is matched to the
    // best qualified virtual mode for its match target: an exact hit when
    // there is one, otherwise the closest supported mode (see
    // rankedVirtualModes). fitted reports whether any display was not an
    // exact hit, so the stream is presented scaled to fit (letterboxed).
    static bool resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                           QString& hostLayout,
                                           QStringList& virtualModes,
                                           QString* error = nullptr,
                                           bool* fitted = nullptr,
                                           const QStringList& candidateModes = qualifiedVirtualModes());
    // The pixel size "Match client displays" aims for: the current desktop
    // backing pixels (what the client presents into), capped at the physical
    // panel with the aspect ratio kept. A macOS "More Space" backing larger
    // than the panel would only spend bitrate on pixels that are thrown away.
    static QSize clientMatchTarget(const QSize& desktopPixels, const QSize& panelPixels);
    static QSize clientMatchTarget(const NvClientDisplay& display);
    // Qualified virtual modes in order of preference for one client display:
    // the exact mode first; then modes that fit inside the target (no
    // upscale) by closest aspect ratio, then largest area; then, only when
    // nothing fits, modes by closest aspect ratio, then smallest area.
    // Ultra-tall halves are only candidates for portrait targets.
    // candidateModes: qualifiedVirtualModes(), or virtualModesForHost() once
    // the host's feature flags are known.
    static QStringList rankedVirtualModes(const QSize& target,
                                          const QStringList& candidateModes = qualifiedVirtualModes());
    static QStringList qualifiedVirtualModes();
    // Whether a host advertising these feature flags accepts the mode.
    static bool hostAcceptsVirtualMode(const QString& mode, int hostFeatureFlags);
    // The qualified modes a host advertising these feature flags accepts.
    static QStringList virtualModesForHost(int hostFeatureFlags);
    static QString resolveMacClientDisplayMode(const QVector<NvClientDisplay>& displays,
                                               QString* error = nullptr, int* scale = nullptr);
    static QJsonObject macDisplayRequest(const QString& mode, const QString& encodingMode, int scale);
    static QSize virtualModeSize(const QString& mode);
    static QSize macDisplayModeSize(const QString& mode);
    static QSize virtualCanvasSize(const QString& hostLayout,
                                   const QStringList& virtualModes);
    bool displayPolicyKnown() const;
    bool allowsBookmarkHostLayout(const QString& layout) const;
    bool matchesRequestedHostLayout(const QString& layout,
                                    const QStringList& modes) const;
    // The host applied exactly this canonical arrangement: the live request,
    // an applied transition, and one output per entry at the requested
    // rectangle (relative to the desktop origin).
    bool matchesRequestedArrangement(const QString& arrangement) const;
    bool displayArrangementPublished() const
    {
        return (featureFlags & DisplayArrangementFeature) != 0 && displayCapabilities.valid;
    }
    bool contains(QString outputId) const;

    int schemaVersion = 0;
    int featureFlags = 0;
    QString generation;
    int desktopX = 0;
    int desktopY = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    QString layoutKind;
    QString startupLayoutKind;
    QStringList allowedLayoutKinds;
    bool virtualLayout = false;
    QStringList virtualModes;
    QVector<NvOutput> outputs;
    QRectF captureLogicalBounds;
    QString appleEncodingMode = QStringLiteral("hevc-10-420-videotoolbox");
    // The encoded frame capture rectangles refer to: capture_size during an
    // arrangement lease (display arrangement only), else the desktop size.
    int captureWidth = 0;
    int captureHeight = 0;
    bool capturePublished = false;
    QSize captureSize() const
    {
        return QSize(captureWidth > 0 ? captureWidth : desktopWidth, captureHeight > 0 ? captureHeight : desktopHeight);
    }
    // Display arrangement (0x8000000) only; ignored without the bit.
    DisplayArrangement::Capabilities displayCapabilities;
    QString startupPolicy;           // physical | virtual | hybrid
    QString arrangementRequest;      // the live canonical request, or ""
    QString arrangementState;        // idle | pending | applied | failed
    QString arrangementReason;       // a short code, or ""
};
