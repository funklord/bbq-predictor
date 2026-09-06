include(test_common.pri)
TARGET = test_cli

SOURCES += test_cli.cpp \
	$$PWD/../src/cli/options.cpp

HEADERS += \
	$$PWD/../src/cli/options.h
