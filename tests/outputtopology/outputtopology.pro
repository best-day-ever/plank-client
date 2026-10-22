QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += \
    test_outputtopology.cpp \
    ../../app/backend/outputtopology.cpp \
    ../../app/backend/displayarrangement.cpp

HEADERS += ../../app/backend/outputtopology.h \
    ../../app/backend/displayarrangement.h
INCLUDEPATH += ../../app/backend
