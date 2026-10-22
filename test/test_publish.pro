include(test_common.pri)

TARGET = test_publish
QT += sql
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional \
	$$PWD/../src/wire/vendor
SOURCES += test_publish.cpp \
	$$PWD/../src/publish/encode.cpp \
	$$PWD/../src/publish/publish.cpp \
	$$PWD/../src/store/history.cpp \
	$$PWD/../src/publish/signing_key.cpp \
	$$PWD/../src/wire/vendor/situ.c \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c
HEADERS += \
	$$PWD/../src/publish/encode.h \
	$$PWD/../src/publish/publish.h \
	$$PWD/../src/store/history.h \
	$$PWD/../src/publish/signing_key.h \
	$$PWD/../src/wire/vendor/bbq.hpp \
	$$PWD/../src/wire/vendor/situ.h \
	$$PWD/../src/wire/vendor/situ.hpp
