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

# The version, the language level, the size flags, network and sql: all
# of it shared with the publisher, so it is stated once, in core.pri.
include(core.pri)

QT += widgets

# Positioning is how "near me" is answered (sec 13.3). Optional at run
# time: with no source, or with the permission refused, the station list
# falls back to searching for a place by name -- which is the only route
# on a desktop anyway, so the fallback is a path that gets exercised
# rather than one that waits for a failure to be discovered.
QT += positioning

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
	#
	# The fallback is only for a qmake run that does not come through the
	# Makefile -- Qt Creator, or qmake by hand. It said 33, which Play has
	# refused for new apps and updates since 2026-08-31, so such a build
	# made an app that could not be uploaded (sec 11.12). 36 matches
	# tool/android.mk's default.
	isEmpty(BBQ_TARGET_API): BBQ_TARGET_API = 36
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

# What the window adds to core.pri, which holds everything that does not
# draw.
SOURCES += \
	src/main.cpp \
	src/graph/forecast_graph.cpp \
	src/graph/interpolate.cpp \
	src/graph/simplify.cpp \
	src/graph/sweep.cpp \
	src/graph/ticks.cpp \
	src/model/band_file.cpp \
	src/ui/accessibility.cpp \
	src/ui/layout.cpp \
	src/ui/locator.cpp \
	src/ui/theme.cpp \
	src/ui/flow_layout.cpp \
	src/ui/widget_picture.cpp \
	src/ui/main_window.cpp \
	src/ui/tray_icon.cpp

HEADERS += \
	src/graph/forecast_graph.h \
	src/graph/interpolate.h \
	src/graph/simplify.h \
	src/graph/sweep.h \
	src/graph/ticks.h \
	src/model/band_file.h \
	src/ui/accessibility.h \
	src/ui/layout.h \
	src/ui/locator.h \
	src/ui/theme.h \
	src/ui/flow_layout.h \
	src/ui/widget_picture.h \
	src/ui/main_window.h \
	src/ui/tray_icon.h
