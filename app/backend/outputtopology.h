#pragma once

#include <QJsonObject>
#include <QString>
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
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

struct NvOutputTopology
{
    static const int ProtocolVersion = 2;
    static const int OutputTopologyFeature = 0x1;
    static const int SelectedOutputFeature = 0x2;
    static const int UnifiedAbsoluteInputFeature = 0x4;
    static const int ScaledSpanFeature = 0x8;
    static const int TopologyGenerationFeature = 0x10;
    static const int HostLayoutMetadataFeature = 0x20;
    static const int CompositeSourceRegionsFeature = 0x40;
    static const int HostLayoutBindingFeature = 0x80;
    static const int SupportedFeatureFlags = OutputTopologyFeature |
                                             SelectedOutputFeature |
                                             UnifiedAbsoluteInputFeature |
                                             ScaledSpanFeature |
                                             TopologyGenerationFeature |
                                             HostLayoutMetadataFeature |
                                             CompositeSourceRegionsFeature |
                                             HostLayoutBindingFeature;
    static const char* SingleOutputMode;
    static const char* ScaledSpanMode;
    static const char* SeparateDisplaysMode;
    static const char* ConfiguredHostLayout;
    static const char* PhysicalHostLayout;
    static const char* SingleHostLayout;
    static const char* DualHorizontalHostLayout;

    static bool fromJson(const QJsonObject& object, NvOutputTopology& topology,
                         QString* error = nullptr);
    QJsonObject toJson() const;

    QString selectOutput(QString persistedId) const;
    QString selectDisplayMode(QString persistedMode) const;
    QString resolveHostLayout(QString persistedLayout) const;
    QString resolveVirtualMode(QString persistedLayout,
                               QString persistedVirtualMode) const;
    bool supportsScaledSpan() const;
    bool supportsSeparateDisplays() const;
    bool contains(QString outputId) const;

    int schemaVersion = 0;
    int featureFlags = 0;
    QString generation;
    int desktopX = 0;
    int desktopY = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    QString layoutKind;
    bool virtualLayout = false;
    QString virtualMode;
    QVector<NvOutput> outputs;
};
