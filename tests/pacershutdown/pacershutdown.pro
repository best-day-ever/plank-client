QT += core gui testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = pacershutdown

INCLUDEPATH += ../../app
isEmpty(PLANK_COMMON_SOURCE): PLANK_COMMON_SOURCE = $$PWD/../../moonlight-common-c/moonlight-common-c
INCLUDEPATH += $$PLANK_COMMON_SOURCE/src
win32 {
    DEFINES += NOMINMAX
    INCLUDEPATH += ../../libs/windows/include ../../libs/windows/include/x64 ../../libs/windows/include/x64/SDL3
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL3 -lSDL3_ttf -lavcodec -lavutil -lgdi32 -luser32
    SOURCES += ../../app/streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.cpp
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl3 sdl3-ttf libavcodec libavutil
}

# Compile the real queue, worker and destructor. Only the display-refresh query
# and GPU renderer are replaced; this test needs neither a window nor a GPU.
SOURCES += test_pacershutdown.cpp \
    ../../app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp \
    ../../app/streaming/avsynccontroller.cpp

unix: LIBS += -pthread
