# Shared by every test binary. The suite links the model and the readers
# but never the widgets, so it needs no display and no QApplication.
QT += testlib network
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app

INCLUDEPATH += $$PWD/../src

# EVERY SUBPROJECT IS GENERATED INTO ONE DIRECTORY, so each must own its
# object files (project.md sec 15.9).
#
# tests.pro is TEMPLATE = subdirs whose members all live in this
# directory, so qmake writes every Makefile.test_* into the same build
# tree -- and without this they all compile to the same object names.
# Measured in one build: moc_forecast.o written nine times and series.o
# four, by different targets, with different -D flags. Serially that is
# merely wasteful; under -j it is a race, and dh_auto_test runs make -j,
# so the PACKAGE build failed at random with undefined moc symbols in
# whichever target lost.
#
# The name comes from the .pro file rather than from TARGET, because
# every subproject includes this before setting TARGET.
#
# obj/ rather than a test_* name: `make test` globs test_* for binaries
# to run, and a directory that matched would be a second thing to get
# right.
_test_name = $$basename(_PRO_FILE_)
_test_name ~= s/\.pro$//
OBJECTS_DIR = obj/$${_test_name}
MOC_DIR = obj/$${_test_name}

# Size over speed here too, per the global rule.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os

SOURCES += \
	$$PWD/../src/met/nowcast.cpp \
	$$PWD/../src/model/composite.cpp \
	$$PWD/../src/model/grill.cpp \
	$$PWD/../src/model/series.cpp \
	$$PWD/../src/graph/interpolate.cpp \
	$$PWD/../src/ui/layout.cpp \
	$$PWD/../src/openmeteo/forecast.cpp \
	$$PWD/../src/wu/reader.cpp

HEADERS += \
	$$PWD/../src/met/nowcast.h \
	$$PWD/../src/model/composite.h \
	$$PWD/../src/model/grill.h \
	$$PWD/../src/model/sample.h \
	$$PWD/../src/model/series.h \
	$$PWD/../src/graph/interpolate.h \
	$$PWD/../src/ui/layout.h \
	$$PWD/../src/openmeteo/forecast.h \
	$$PWD/../src/wu/reader.h
