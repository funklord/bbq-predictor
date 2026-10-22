include(test_common.pri)

TARGET = test_serve_loop
SOURCES += test_serve_loop.cpp \
	$$PWD/../src/serve/serve_core.c \
	$$PWD/../src/serve/serve_loop.c
HEADERS += \
	$$PWD/../src/serve/serve_core.h \
	$$PWD/../src/serve/serve_loop.h
LIBS += -lpthread
