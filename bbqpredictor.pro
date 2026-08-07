# =============================================================================
# bbqpredictor.pro -- qmake project for the Qt Widgets weather applet
#
# Built through the top-level Makefile, not by hand. `make` wraps qmake so
# that the Qt subtree and the plain Makefiles present one interface, which
# is the split beerssh uses; see project.md sec 1 and sec 5.
#
# No QtQuick/QML, ever -- project.md sec 1, and the global harmonization
# rule behind it. Nothing above requests it; this comment is the tripwire
# for the next person tempted to add `QT += quick` for "just one screen".
# =============================================================================

TEMPLATE = app
TARGET = bbqpredictor

# Passed in by the top-level Makefile so the version lives in one place --
# the VERSION file. The fallback exists so that opening this in Qt Creator,
# or any bare qmake run, still produces something that builds and can say
# what it is.
isEmpty(BBQ_VERSION): BBQ_VERSION = 0.0.0-nomake
DEFINES += BBQ_VERSION_STRING=\\\"$$BBQ_VERSION\\\"

# network is here from the start rather than added later: the whole
# program is a client for somebody else's HTTP endpoint (project.md sec 2).
QT += widgets network

CONFIG += c++17
CONFIG -= app_bundle

# Size over speed, per the global rule.
#
# This is the only place saying so takes effect. qmake's release default is
# -O2 and it is applied by the Makefile qmake GENERATES, so passing -Os to
# the top-level make would be silently dropped -- the same shape as the
# signing flags beerssh lost that way, and just as invisible in the output.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os
QMAKE_CFLAGS_RELEASE   -= -O2
QMAKE_CFLAGS_RELEASE   += -Os

INCLUDEPATH += $$PWD/src

SOURCES += \
	src/main.cpp \
	src/graph/forecast_graph.cpp \
	src/ui/main_window.cpp \
	src/ui/tray_icon.cpp \
	src/wu/client.cpp \
	src/wu/fetch_once.cpp \
	src/wu/key_source.cpp

HEADERS += \
	src/graph/forecast_graph.h \
	src/ui/main_window.h \
	src/ui/tray_icon.h \
	src/wu/client.h \
	src/wu/fetch_once.h \
	src/wu/key_source.h
