include(test_common.pri)

TARGET = test_server_client
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional
SOURCES += test_server_client.cpp \
	$$PWD/../src/server/client.cpp \
	$$PWD/../src/wire/format.cpp \
	$$PWD/../src/publish/encode.cpp \
	$$PWD/../src/publish/signing_key.cpp \
	$$PWD/../src/serve/serve_core.c \
	$$PWD/../src/serve/serve_loop.c \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c
HEADERS += \
	$$PWD/../src/server/client.h \
	$$PWD/../src/wire/format.h \
	$$PWD/../src/publish/encode.h \
	$$PWD/../src/publish/signing_key.h \
	$$PWD/../src/serve/serve_core.h \
	$$PWD/../src/serve/serve_loop.h
LIBS += -lpthread
