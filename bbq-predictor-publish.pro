# =============================================================================
# bbq-predictor-publish.pro -- the headless publisher (project.md sec 15.13)
#
# Built through the top-level Makefile, like the applet. core.pri is
# everything the two share; this adds the signing, the encoder and a main
# that builds no QApplication.
#
# No Gui. The publisher runs on a server with no display, and linking the
# window's modules put the desktop's libraries there for a program that
# never draws. `make check-publish-libraries` holds the line.
# =============================================================================

TEMPLATE = app
TARGET = bbq-predictor-publish

include(core.pri)

QT -= gui
CONFIG += console

SOURCES += \
	src/publish/main.cpp \
	src/publish/demand.cpp \
	src/publish/encode.cpp \
	src/publish/publish.cpp \
	src/publish/signing_key.cpp

HEADERS += \
	src/publish/demand.h \
	src/publish/encode.h \
	src/publish/publish.h \
	src/publish/signing_key.h
