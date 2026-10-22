QT += core gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17
TEMPLATE = app
SOURCES += test_collaboration.cpp ../../app/streaming/plankpresentation.cpp
HEADERS += ../../app/backend/collaboration.h
RESOURCES += ../../app/qml.qrc
