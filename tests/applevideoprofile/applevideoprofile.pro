QT -= gui core
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = applevideoprofile
PKGCONFIG += libavcodec libavutil libswresample
SOURCES += test_applevideoprofile.cpp
INCLUDEPATH += ../../app/streaming/video
