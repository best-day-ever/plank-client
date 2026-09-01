#-------------------------------------------------
#
# Project created by QtCreator 2018-05-05T17:41:00
#
#-------------------------------------------------

QT -= core gui

TARGET = moonlight-common-c
TEMPLATE = lib

# Build a static library
CONFIG += staticlib

# Include global qmake defs
include(../globaldefs.pri)

unix:!macx:contains(CONFIG, plank-transport) {
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$(PLANK_TRANSPORT_DIR)
    }
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$clean_path($$PWD/../../../protocol/plank-transport)
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_input.h) {
        error("PLANK transport input header is missing: $$PLANK_TRANSPORT_DIR")
    }
    INCLUDEPATH += $$PLANK_TRANSPORT_DIR/include
    DEFINES += PLANK_TRANSPORT=1
}

win32 {
    contains(QT_ARCH, i386) {
        INCLUDEPATH += $$PWD/../libs/windows/include/x86
    }
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../libs/windows/include/x64
    }
    contains(QT_ARCH, arm64) {
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64
    }

    INCLUDEPATH += $$PWD/../libs/windows/include
    DEFINES += HAS_QOS_FLOWID=1 HAS_PQOS_FLOWID=1
}
macx {
    INCLUDEPATH += $$PWD/../libs/mac/include
}
unix:!macx {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl
    DEFINES += HAVE_CLOCK_GETTIME=1
}

COMMON_C_DIR = $$PWD/moonlight-common-c
SOURCES += \
    $$COMMON_C_DIR/src/AudioStream.c \
    $$COMMON_C_DIR/src/Connection.c \
    $$COMMON_C_DIR/src/ControlStream.c \
    $$COMMON_C_DIR/src/FakeCallbacks.c \
    $$COMMON_C_DIR/src/InputStream.c \
    $$COMMON_C_DIR/src/LinkedBlockingQueue.c \
    $$COMMON_C_DIR/src/Misc.c \
    $$COMMON_C_DIR/src/Platform.c \
    $$COMMON_C_DIR/src/PlatformCrypto.c \
    $$COMMON_C_DIR/src/PlatformSockets.c \
    $$COMMON_C_DIR/src/VideoFrameAssembler.c \
    $$COMMON_C_DIR/src/VideoStream.c
HEADERS += \
    $$COMMON_C_DIR/src/Limelight.h
INCLUDEPATH += \
    $$COMMON_C_DIR/src
DEFINES += HAS_SOCKLEN_T

CONFIG(debug, debug|release) {
    # Enable asserts on debug builds
    DEFINES += LC_DEBUG
}

# Older GCC versions defaulted to GNU89
*-g++ {
    QMAKE_CFLAGS += -std=gnu99
}

# Disable unused parameter warnings on GCC and Clang
*-g++|*-clang* {
    QMAKE_CFLAGS_WARN_ON += -Wno-unused-parameter
}
