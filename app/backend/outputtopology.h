#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

struct NvOutput
{
    QString id;
    QString name;
    int x;
    int y;
    int width;
    int height;
    int rotation;
    int refreshMillihz;
    bool primary;
};

struct NvOutputTopology
{
    static const int ProtocolVersion = 1;
    static const int OutputTopologyFeature = 0x1;
    static const int SelectedOutputFeature = 0x2;
    static const int UnifiedAbsoluteInputFeature = 0x4;
    static const int ScaledSpanFeature = 0x8;
    static const int TopologyGenerationFeature = 0x10;
    static const int SupportedFeatureFlags = OutputTopologyFeature |
                                             SelectedOutputFeature |
                                             UnifiedAbsoluteInputFeature |
                                             ScaledSpanFeature |
                                             TopologyGenerationFeature;
    static const char* SingleOutputMode;
    static const char* ScaledSpanMode;

    static bool fromJson(const QJsonObject& object, NvOutputTopology& topology,
                         QString* error = nullptr);
    QJsonObject toJson() const;

    QString selectOutput(QString persistedId) const;
    QString selectDisplayMode(QString persistedMode) const;
    bool supportsScaledSpan() const;
    bool contains(QString outputId) const;

    int schemaVersion = 0;
    int featureFlags = 0;
    QString generation;
    int desktopX = 0;
    int desktopY = 0;
    int desktopWidth = 0;
    int desktopHeight = 0;
    QVector<NvOutput> outputs;
};
