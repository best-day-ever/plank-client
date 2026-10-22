macx {
    QT += core quick network
    CONFIG += console c++17 link_pkgconfig
    CONFIG -= app_bundle
    TEMPLATE = app
    TARGET = macmetaloverlay
    INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
    INCLUDEPATH += ../../qmdnsengine/qmdnsengine/src/include ../../qmdnsengine
    PKGCONFIG += sdl3 sdl3-ttf libavcodec libavutil opus openssl
    OBJECTIVE_SOURCES += test_macmetaloverlay.mm ../../app/streaming/video/ffmpeg-renderers/vt_base.mm
    SOURCES += ../../app/streaming/plankpresentation.cpp ../../app/streaming/streamutils.cpp
    LIBS += -framework Metal -framework QuartzCore -framework VideoToolbox -framework AVFoundation -framework CoreVideo
    QMAKE_LFLAGS += -Wl,-dead_strip
} else {
    error("This headless overlay test requires macOS")
}
