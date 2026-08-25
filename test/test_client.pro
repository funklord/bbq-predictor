include(test_common.pri)
TARGET = test_client

# The client and the key source, which no other test binary links. They
# are not in test_common.pri because every binary would then carry a
# network client it never uses.
SOURCES += test_client.cpp \
	$$PWD/../src/wu/client.cpp \
	$$PWD/../src/wu/key_source.cpp

HEADERS += \
	$$PWD/../src/wu/client.h \
	$$PWD/../src/wu/key_source.h
