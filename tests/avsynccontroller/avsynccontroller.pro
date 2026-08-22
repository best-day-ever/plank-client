QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += \
    test_avsynccontroller.cpp \
    ../../app/streaming/avsynccontroller.cpp

HEADERS += ../../app/streaming/avsynccontroller.h
INCLUDEPATH += ../../app/streaming
