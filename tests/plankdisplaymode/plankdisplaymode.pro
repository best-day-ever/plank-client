QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app

SOURCES += \
    test_plankdisplaymode.cpp \
    ../../app/streaming/plankdisplaymode.cpp

HEADERS += ../../app/streaming/plankdisplaymode.h
INCLUDEPATH += ../../app/streaming
