!macx: error("This fixture requires macOS and Metal")
QT += core gui quick
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macqtvideo
include(../../app/streaming/qt/qtvideo.pri)
OBJECTIVE_SOURCES += test_macqtvideo.mm
