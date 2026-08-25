include(test_common.pri)
TARGET = test_history
QT += sql

SOURCES += test_history.cpp \
	$$PWD/../src/store/history.cpp

HEADERS += \
	$$PWD/../src/store/history.h
