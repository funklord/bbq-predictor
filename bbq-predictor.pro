# =============================================================================
# bbq-predictor.pro -- qmake project for the Qt Widgets weather applet
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
TARGET = bbq-predictor

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

# --- Android ---------------------------------------------------------------
# The only platform conditional in this file. Everything above is shared by
# construction, which is the point: the mobile difference is a LAYOUT
# (project.md sec 10), not a second program.
android {
	# Play rejects any upload whose versionCode does not exceed the last it
	# accepted, so a hardcoded number allows one upload and blocks every
	# update after it. The Makefile computes it from VERSION and passes it
	# in; qmake has no arithmetic to do it here.
	#
	# The fallback matters: opening this in Qt Creator, or any bare qmake
	# run, must still produce something installable.
	isEmpty(BBQ_VERSION_CODE): BBQ_VERSION_CODE = 1
	ANDROID_VERSION_CODE = $$BBQ_VERSION_CODE
	ANDROID_VERSION_NAME = $$BBQ_VERSION

	# Set here rather than as <uses-sdk> in a manifest: the Gradle plugin
	# takes these from the build file and warns about, or overrides, a
	# manifest that also declares them.
	# Passed in rather than fixed here, because the target API a build can
	# use is bounded by the platforms actually installed -- and the number
	# Play requires for an upload moves independently of them. A build that
	# hardcodes it fails on a machine whose SDK is one release behind, and
	# fails talking about Gradle rather than about the SDK.
	isEmpty(BBQ_TARGET_API): BBQ_TARGET_API = 33
	ANDROID_MIN_SDK_VERSION = 26
	ANDROID_TARGET_SDK_VERSION = $$BBQ_TARGET_API
}

INCLUDEPATH += $$PWD/src

SOURCES += \
	src/main.cpp \
	src/graph/forecast_graph.cpp \
	src/graph/interpolate.cpp \
	src/met/nowcast.cpp \
	src/model/composite.cpp \
	src/openmeteo/forecast.cpp \
	src/model/grill.cpp \
	src/model/series.cpp \
	src/model/settings.cpp \
	src/ui/layout.cpp \
	src/ui/main_window.cpp \
	src/ui/tray_icon.cpp \
	src/wu/client.cpp \
	src/wu/feed.cpp \
	src/wu/fetch_once.cpp \
	src/wu/key_source.cpp \
	src/wu/reader.cpp

HEADERS += \
	src/graph/forecast_graph.h \
	src/graph/interpolate.h \
	src/met/nowcast.h \
	src/model/composite.h \
	src/openmeteo/forecast.h \
	src/model/grill.h \
	src/model/sample.h \
	src/model/series.h \
	src/model/settings.h \
	src/ui/layout.h \
	src/ui/main_window.h \
	src/ui/tray_icon.h \
	src/wu/client.h \
	src/wu/feed.h \
	src/wu/fetch_once.h \
	src/wu/key_source.h \
	src/wu/reader.h
