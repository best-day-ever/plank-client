QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += test_decodercaps.cpp
OBJECTIVE_SOURCES += ../../app/streaming/video/decodercaps.mm
HEADERS += \
    ../../app/streaming/video/decodercaps.h \
    ../../app/streaming/video/decodercaps-test-frames.h

INCLUDEPATH += ../../app/streaming/video
LIBS += -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation
