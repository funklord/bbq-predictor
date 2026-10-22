include(test_common.pri)
INCLUDEPATH += $$PWD/../monocypher/src $$PWD/../monocypher/src/optional \
	$$PWD/../src/wire/vendor
TARGET = test_window
QT += gui widgets sql positioning

# The window and everything it owns. It is the largest link in the
# suite, and deliberately so: what this binary exists to check is the
# WIRING between the parts, which is exactly what a narrower link would
# stub out (project.md sec 14.10).
SOURCES += test_window.cpp \
	$$PWD/../src/graph/forecast_graph.cpp \
	$$PWD/../src/graph/simplify.cpp \
	$$PWD/../src/model/correction.cpp \
	$$PWD/../src/model/settings.cpp \
	$$PWD/../src/store/history.cpp \
	$$PWD/../src/ui/accessibility.cpp \
	$$PWD/../src/ui/locator.cpp \
	$$PWD/../src/ui/main_window.cpp \
	$$PWD/../src/ui/theme.cpp \
	$$PWD/../src/ui/tray_icon.cpp \
	$$PWD/../src/ui/flow_layout.cpp \
	$$PWD/../src/ui/widget_picture.cpp \
	$$PWD/../src/wu/client.cpp \
	$$PWD/../src/wu/feed.cpp \
	$$PWD/../src/server/transport.cpp \
	$$PWD/../src/server/policy.cpp \
	$$PWD/../src/wire/format.cpp \
	$$PWD/../src/wire/read.cpp \
	$$PWD/../src/wire/vendor/situ.c \
	$$PWD/../monocypher/src/monocypher.c \
	$$PWD/../monocypher/src/optional/monocypher-ed25519.c \
	$$PWD/../src/wu/key_source.cpp

HEADERS += \
	$$PWD/../src/graph/forecast_graph.h \
	$$PWD/../src/graph/simplify.h \
	$$PWD/../src/model/correction.h \
	$$PWD/../src/model/settings.h \
	$$PWD/../src/store/history.h \
	$$PWD/../src/ui/accessibility.h \
	$$PWD/../src/ui/locator.h \
	$$PWD/../src/ui/main_window.h \
	$$PWD/../src/ui/theme.h \
	$$PWD/../src/ui/tray_icon.h \
	$$PWD/../src/ui/flow_layout.h \
	$$PWD/../src/ui/widget_picture.h \
	$$PWD/../src/wu/client.h \
	$$PWD/../src/wu/feed.h \
	$$PWD/../src/server/transport.h \
	$$PWD/../src/server/policy.h \
	$$PWD/../src/wire/format.h \
	$$PWD/../src/wire/read.h \
	$$PWD/../src/wire/vendor/bbq.hpp \
	$$PWD/../src/wire/vendor/situ.h \
	$$PWD/../src/wire/vendor/situ.hpp \
	$$PWD/../src/wu/key_source.h
