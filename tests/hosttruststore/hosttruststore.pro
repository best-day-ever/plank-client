QT += core network testlib
QT -= gui
CONFIG += console testcase c++17
TEMPLATE = app
SOURCES += test_hosttruststore.cpp ../../app/backend/hosttruststore.cpp
HEADERS += ../../app/backend/hosttruststore.h
