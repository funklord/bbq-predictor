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
QT += widgets network sql

# Positioning is how "near me" is answered (sec 13.3). Optional at run
# time: with no source, or with the permission refused, the station list
# falls back to searching for a place by name -- which is the only route
# on a desktop anyway, so the fallback is a path that gets exercised
# rather than one that waits for a failure to be discovered.
QT += positioning

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
QMAKE_CFLAGS_RELEASE   -= -O2
QMAKE_CFLAGS_RELEASE   += -Os

# --- Android ---------------------------------------------------------------
# The only platform conditional in this file. Everything above is shared by
# construction, which is the point: the mobile difference is a LAYOUT
# (project.md sec 10), not a second program.
android {
	# The manifest is the project's own, not Qt's generated one, because
	# it carries a permission. Taken verbatim from the Qt template and
	# changed in exactly one place, so a Qt upgrade can be diffed against
	# the new template rather than guessed at.
	ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android

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

	# OpenSSL, without which there is no TLS and therefore no data at all
	# (project.md sec 11.5). Qt for Android does not ship it, and every
	# provider this program reads is HTTPS -- so its absence is not a
	# degraded build, it is a build that can fetch nothing.
	#
	# Built by tool/build-openssl-android.sh from the distribution's own
	# source. Conditional, because a tree that has not run that script
	# should still produce an installable package rather than refusing to
	# configure -- it simply cannot fetch, and says so on screen.
	BBQ_SSL_LIB = $$PWD/deps/android/$$ANDROID_TARGET_ARCH/lib
	# The _3 suffix is not decoration: Qt's OpenSSL backend dlopens
	# "libssl_3.so" by name on Android, and an unsuffixed library is not
	# the file it asks for. See tool/build-openssl-android.sh.
	BBQ_SSL_CRYPTO = $$BBQ_SSL_LIB/libcrypto_3.so
	BBQ_SSL_SSL = $$BBQ_SSL_LIB/libssl_3.so

	exists($$BBQ_SSL_SSL) {
		ANDROID_EXTRA_LIBS += $$BBQ_SSL_CRYPTO $$BBQ_SSL_SSL

		message("openssl: bundling $$BBQ_SSL_LIB")
	} else {
		warning("openssl: $$BBQ_SSL_SSL is missing -- the package will " \
		        "have no TLS. Run tool/build-openssl-android.sh.")
	}
}

INCLUDEPATH += $$PWD/src

SOURCES += \
	src/main.cpp \
	src/graph/forecast_graph.cpp \
	src/graph/interpolate.cpp \
	src/met/nowcast.cpp \
	src/net/probe.cpp \
	src/net/tls_backend.cpp \
	src/model/composite.cpp \
	src/model/correction.cpp \
	src/openmeteo/forecast.cpp \
	src/model/grill.cpp \
	src/model/series.cpp \
	src/model/settings.cpp \
	src/store/history.cpp \
	src/ui/accessibility.cpp \
	src/ui/layout.cpp \
	src/ui/locator.cpp \
	src/ui/theme.cpp \
	src/ui/flow_layout.cpp \
	src/ui/widget_picture.cpp \
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
	src/net/probe.h \
	src/net/tls_backend.h \
	src/model/composite.h \
	src/model/correction.h \
	src/openmeteo/forecast.h \
	src/model/grill.h \
	src/model/sample.h \
	src/model/series.h \
	src/model/settings.h \
	src/store/history.h \
	src/ui/accessibility.h \
	src/ui/layout.h \
	src/ui/locator.h \
	src/ui/theme.h \
	src/ui/flow_layout.h \
	src/ui/widget_picture.h \
	src/ui/main_window.h \
	src/ui/tray_icon.h \
	src/wu/client.h \
	src/wu/feed.h \
	src/wu/fetch_once.h \
	src/wu/key_source.h \
	src/wu/reader.h
