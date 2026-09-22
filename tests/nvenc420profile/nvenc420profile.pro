QT -= gui core
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = nvenc420profile
PKGCONFIG += libavcodec libavutil libswresample
SOURCES += test_nvenc420profile.cpp
INCLUDEPATH += ../../app/streaming/video
