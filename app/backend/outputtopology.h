#pragma once

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
};

struct NvClientDisplay
{
    QRect bounds;
    QSize nativeSize;
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
                                             AuthenticatedDesktopStageFeature;
    static const char* NativeScalingMode;
    static const char* ScaledSpanMode;
    static const char* MatchClientHostLayout;
    static const char* PhysicalHostLayout;
    static const char* SingleHostLayout;
    static const char* DualHorizontalHostLayout;

    static bool fromJson(const QJsonObject& object, NvOutputTopology& topology,
                         QString* error = nullptr);
    QJsonObject toJson() const;

    static bool resolveClientDisplayLayout(QVector<NvClientDisplay> displays,
                                           QString& hostLayout,
                                           QStringList& virtualModes,
                                           QString* error = nullptr);
    static QStringList qualifiedVirtualModes();
    static QSize virtualModeSize(const QString& mode);
    static QSize virtualCanvasSize(const QString& hostLayout,
                                   const QStringList& virtualModes);
    bool displayPolicyKnown() const;
    bool allowsBookmarkHostLayout(const QString& layout) const;
    bool matchesRequestedHostLayout(const QString& layout,
                                    const QStringList& modes) const;
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
};
