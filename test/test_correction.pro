include(test_common.pri)
TARGET = test_correction
QT += sql

SOURCES += test_correction.cpp \
	$$PWD/../src/model/correction.cpp \
	$$PWD/../src/store/history.cpp

HEADERS += \
	$$PWD/../src/model/correction.h \
	$$PWD/../src/store/history.h
