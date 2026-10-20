QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_windowswacom.cpp
HEADERS += ../../app/streaming/input/windowswacomlogic.h \
    ../../app/streaming/input/macrawwacomlogic.h \
    ../../app/streaming/input/linuxwacomreportworker.h
INCLUDEPATH += ../../app/streaming/input ../../moonlight-common-c/moonlight-common-c/src
