QT += core network testlib
QT -= gui
CONFIG += console testcase c++17 link_pkgconfig
PKGCONFIG += openssl
TEMPLATE = app
SOURCES += test_hosttlsguard.cpp ../../app/backend/hosttlsguard.cpp ../../app/backend/hosttruststore.cpp
HEADERS += ../../app/backend/hosttlsguard.h ../../app/backend/hosttruststore.h
