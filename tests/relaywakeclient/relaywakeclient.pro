QT += core network testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += \
    test_relaywakeclient.cpp \
    ../../app/backend/relaywakeclient.cpp

HEADERS += ../../app/backend/relaywakeclient.h

INCLUDEPATH += ../../app/backend
