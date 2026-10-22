include(test_common.pri)
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional \
	$$PWD/../src/wire/vendor
TARGET = test_feed_server
QT += sql

# The feed and everything it owns. Not in test_common.pri because no
# other binary needs a network client to make its point.
SOURCES += test_feed_server.cpp \
	$$PWD/../src/publish/encode.cpp \
	$$PWD/../src/publish/signing_key.cpp \
	$$PWD/../src/serve/serve_core.c \
	$$PWD/../src/serve/serve_loop.c \
	$$PWD/../src/wu/feed.cpp \
	$$PWD/../src/server/transport.cpp \
	$$PWD/../src/server/policy.cpp \
	$$PWD/../src/wire/format.cpp \
	$$PWD/../src/wire/read.cpp \
	$$PWD/../src/wire/vendor/situ.c \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c \
	$$PWD/../src/wu/client.cpp \
	$$PWD/../src/wu/key_source.cpp \
	$$PWD/../src/store/history.cpp \
	$$PWD/../src/model/correction.cpp

HEADERS += \
	$$PWD/../src/wu/fetch_verdict.h \
	$$PWD/../src/wu/feed.h \
	$$PWD/../src/server/transport.h \
	$$PWD/../src/server/policy.h \
	$$PWD/../src/wire/format.h \
	$$PWD/../src/wire/read.h \
	$$PWD/../src/wire/vendor/bbq.hpp \
	$$PWD/../src/wire/vendor/situ.h \
	$$PWD/../src/wire/vendor/situ.hpp \
	$$PWD/../src/wu/client.h \
	$$PWD/../src/wu/key_source.h \
	$$PWD/../src/store/history.h \
	$$PWD/../src/model/correction.h
LIBS += -lpthread
HEADERS += \
	$$PWD/../src/publish/encode.h \
	$$PWD/../src/publish/signing_key.h \
	$$PWD/../src/serve/serve_core.h \
	$$PWD/../src/serve/serve_loop.h
