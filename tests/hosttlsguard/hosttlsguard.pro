QT += core gui network qml testlib
CONFIG += console testcase c++17 link_pkgconfig
PKGCONFIG += openssl
macx: QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir openssl)
TEMPLATE = app
SOURCES += test_hosttlsguard.cpp ../../app/backend/hosttlsguard.cpp ../../app/backend/hosttruststore.cpp \
    ../../app/backend/nvhttp.cpp ../../app/backend/nvaddress.cpp \
    ../../app/backend/outputtopology.cpp ../../app/backend/displayarrangement.cpp
HEADERS += ../../app/backend/hosttlsguard.h ../../app/backend/hosttruststore.h ../../app/backend/nvhttp.h
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
