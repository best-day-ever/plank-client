QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += test_macpen.cpp

HEADERS += ../../app/streaming/input/macpen.h \
    ../../app/streaming/input/pentiltencoding.h
INCLUDEPATH += ../../app/streaming/input \
    ../../moonlight-common-c/moonlight-common-c/src
