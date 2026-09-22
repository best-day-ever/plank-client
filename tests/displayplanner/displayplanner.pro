QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    test_displayplanner.cpp \
    ../../app/backend/outputtopology.cpp

HEADERS += \
    ../../app/backend/outputtopology.h \
    ../../app/backend/clientdisplayprobe.h

INCLUDEPATH += \
    ../../app/backend \
    ../../app
