QT += core qml testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = profile-tests
INCLUDEPATH += ../../app
SOURCES += profile_tests.cpp ../../app/settings/streamingpreferences.cpp
HEADERS += ../../app/settings/streamingpreferences.h
