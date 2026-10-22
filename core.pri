# =============================================================================
# core.pri -- everything bbq-predictor and bbq-predictor-publish share
#
# The providers, the store, the feed and the published format: every source
# that draws nothing (project.md sec 15.13). The window includes this and
# adds the widgets; the publisher includes it and adds the signing and the
# encoder, and is built with no Gui at all, because it runs on a server
# with no display and installing the desktop's libraries there for a
# program that never draws was the cost being removed.
#
# Nothing here may need QtGui. `make check-publish-libraries` reads the
# publisher's ELF header and fails if Gui, Widgets or Positioning appears
# in it, which is where a QColor added to a model file would surface.
# =============================================================================

# Passed in by the top-level Makefile so the version lives in one place --
# the VERSION file. The fallback exists so that opening this in Qt Creator,
# or any bare qmake run, still produces something that builds and can say
# what it is.
isEmpty(BBQ_VERSION): BBQ_VERSION = 0.0.0-nomake
DEFINES += BBQ_VERSION_STRING=\\\"$$BBQ_VERSION\\\"

# network is here from the start rather than added later: the whole
# program is a client for somebody else's HTTP endpoint (project.md sec 2).
QT += network sql

# sql is for the permanent history (project.md sec 12). It brings the
# SQLite driver plugin, which Qt builds in-tree; measured before choosing
# the dependency -- both Qt6Sql and libqsqlite.so were already installed
# here, so it costs a packaging dependency and one more module in the
# Android kit rather than a new package.

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

# AND THE SAME AGAIN FOR THE DEBUG-INFO VARIANT, which is a different
# qmake variable and not a modifier of the one above.
#
# `CONFIG+=force_debug_info` does not add -g to the release flags: it
# switches qmake to QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO, which
# defaults to `-O2 -g`. Patching only the line above therefore left the
# packaged build at -O2 -- the global rule to optimise for size,
# silently reverted by a switch whose name says nothing about
# optimisation.
#
# Measured when it happened: the shipped binary went from 388,784 to
# 478,896 bytes, 23% larger, with a green lintian and no other sign.
QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO -= -O2
QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO += -Os
QMAKE_CFLAGS_RELEASE   -= -O2
QMAKE_CFLAGS_RELEASE   += -Os

# And the C half of that variant, which the lines above missed in turn:
# the packaged build compiled monocypher and situ's runtime at -O2 -g
# while every C++ file beside them was -Os. Found reading the publisher's
# generated Makefile after a package build (sec 15.13).
QMAKE_CFLAGS_RELEASE_WITH_DEBUGINFO -= -O2
QMAKE_CFLAGS_RELEASE_WITH_DEBUGINFO += -Os

INCLUDEPATH += $$PWD/src

SOURCES += \
	$$PWD/src/cli/options.cpp \
	$$PWD/src/met/nowcast.cpp \
	$$PWD/src/model/composite.cpp \
	$$PWD/src/model/correction.cpp \
	$$PWD/src/model/grill.cpp \
	$$PWD/src/model/series.cpp \
	$$PWD/src/model/settings.cpp \
	$$PWD/src/model/units.cpp \
	$$PWD/src/net/probe.cpp \
	$$PWD/src/net/tls_backend.cpp \
	$$PWD/src/openmeteo/forecast.cpp \
	$$PWD/src/store/history.cpp \
	$$PWD/src/wu/client.cpp \
	$$PWD/src/wu/feed.cpp \
	$$PWD/src/wu/fetch_once.cpp \
	$$PWD/src/wu/key_source.cpp \
	$$PWD/src/wu/reader.cpp

HEADERS += \
	$$PWD/src/cli/options.h \
	$$PWD/src/met/nowcast.h \
	$$PWD/src/model/composite.h \
	$$PWD/src/model/correction.h \
	$$PWD/src/model/grill.h \
	$$PWD/src/model/sample.h \
	$$PWD/src/model/series.h \
	$$PWD/src/model/settings.h \
	$$PWD/src/model/units.h \
	$$PWD/src/net/probe.h \
	$$PWD/src/net/tls_backend.h \
	$$PWD/src/openmeteo/forecast.h \
	$$PWD/src/store/history.h \
	$$PWD/src/wu/client.h \
	$$PWD/src/wu/feed.h \
	$$PWD/src/wu/fetch_once.h \
	$$PWD/src/wu/fetch_verdict.h \
	$$PWD/src/wu/key_source.h \
	$$PWD/src/wu/reader.h

# The published format, in every build: the applet reads and verifies
# what vibes.se serves, on a phone as much as anywhere (project.md sec
# 15.12). monocypher is for the signature and the index's digests.
INCLUDEPATH += $$PWD/monocypher/src $$PWD/monocypher/src/optional \
	$$PWD/src/wire/vendor
SOURCES += \
	$$PWD/src/server/transport.cpp \
	$$PWD/src/server/policy.cpp \
	$$PWD/src/wire/format.cpp \
	$$PWD/src/wire/read.cpp \
	$$PWD/src/wire/vendor/situ.c \
	$$PWD/monocypher/src/monocypher.c \
	$$PWD/monocypher/src/optional/monocypher-ed25519.c
HEADERS += \
	$$PWD/src/server/transport.h \
	$$PWD/src/server/policy.h \
	$$PWD/src/wire/format.h \
	$$PWD/src/wire/read.h \
	$$PWD/src/wire/vendor/bbq.hpp \
	$$PWD/src/wire/vendor/situ.h \
	$$PWD/src/wire/vendor/situ.hpp
