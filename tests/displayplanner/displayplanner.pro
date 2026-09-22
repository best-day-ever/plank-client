QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    test_displayplanner.cpp \
    ../../app/backend/outputtopology.cpp \
    ../../app/backend/displayarrangement.cpp \
    ../../app/backend/displayplanner.cpp \
    ../../app/streaming/plankpresentation.cpp

HEADERS += \
    ../../app/backend/outputtopology.h \
    ../../app/backend/clientdisplayprobe.h \
    ../../app/backend/displayarrangement.h \
    ../../app/backend/displayprofile.h \
    ../../app/backend/displayplanner.h \
    ../../app/streaming/plankpresentation.h

INCLUDEPATH += \
    ../../app/backend \
    ../../app
