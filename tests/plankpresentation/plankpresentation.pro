QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    test_plankpresentation.cpp \
    ../../app/streaming/plankpresentation.cpp

HEADERS += ../../app/streaming/plankpresentation.h \
    ../../app/streaming/input/plankmousemotion.h \
    ../../app/streaming/input/plankpointerlogic.h

INCLUDEPATH += ../../app

