include(test_common.pri)
TARGET = test_publisher

# Drives the BUILT publisher and applet, as test_seed drives the applet:
# what it checks lives in two main()s, and a second main() does not link.
#
# Plus the demand's own reader, to read back what a run saved: the
# archive's bytes are its format, and a second parser here would be a
# second thing to keep in step with it.
QT += sql
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional
SOURCES += test_publisher.cpp \
	$$PWD/../src/publish/demand.cpp \
	$$PWD/../src/wire/format.cpp \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c
HEADERS += \
	$$PWD/../src/publish/demand.h \
	$$PWD/../src/wire/format.h
