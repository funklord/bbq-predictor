include(test_common.pri)

TARGET = test_server
SOURCES += test_server.cpp \
	$$PWD/../src/server/policy.cpp
HEADERS += \
	$$PWD/../src/server/policy.h
