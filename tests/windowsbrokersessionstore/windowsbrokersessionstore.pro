QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app
SOURCES += test_windowsbrokersessionstore.cpp ../../app/backend/brokersessionstore.cpp
HEADERS += ../../app/backend/brokersessionstore.h
INCLUDEPATH += ../../app/backend
LIBS += crypt32.lib
