include(test_common.pri)

TARGET = test_serve
SOURCES += test_serve.cpp \
	$$PWD/../src/serve/serve_core.c
HEADERS += \
	$$PWD/../src/serve/serve_core.h
