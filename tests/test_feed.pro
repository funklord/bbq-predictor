include(test_common.pri)
TARGET = test_feed
QT += sql

# The feed and everything it owns. Not in test_common.pri because no
# other binary needs a network client to make its point.
SOURCES += test_feed.cpp \
	$$PWD/../src/wu/feed.cpp \
	$$PWD/../src/wu/client.cpp \
	$$PWD/../src/wu/key_source.cpp \
	$$PWD/../src/store/history.cpp

HEADERS += \
	$$PWD/../src/wu/feed.h \
	$$PWD/../src/wu/client.h \
	$$PWD/../src/wu/key_source.h \
	$$PWD/../src/store/history.h
