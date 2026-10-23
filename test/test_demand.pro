include(test_common.pri)

TARGET = test_demand
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional
SOURCES += test_demand.cpp \
	$$PWD/../src/publish/demand.cpp \
	$$PWD/../src/wire/format.cpp \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c
HEADERS += \
	$$PWD/../src/publish/demand.h \
	$$PWD/../src/wire/format.h
