QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app

SOURCES += \
    test_outputtopology.cpp \
    ../../app/backend/outputtopology.cpp

HEADERS += ../../app/backend/outputtopology.h
INCLUDEPATH += ../../app/backend
