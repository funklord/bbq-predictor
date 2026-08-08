# Shared by every test binary. The suite links the model and the readers
# but never the widgets, so it needs no display and no QApplication.
QT += testlib network
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app

INCLUDEPATH += $$PWD/../src

# Size over speed here too, per the global rule.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os

SOURCES += \
	$$PWD/../src/met/nowcast.cpp \
	$$PWD/../src/model/composite.cpp \
	$$PWD/../src/model/grill.cpp \
	$$PWD/../src/model/series.cpp \
	$$PWD/../src/graph/interpolate.cpp \
	$$PWD/../src/openmeteo/forecast.cpp \
	$$PWD/../src/wu/reader.cpp

HEADERS += \
	$$PWD/../src/met/nowcast.h \
	$$PWD/../src/model/composite.h \
	$$PWD/../src/model/grill.h \
	$$PWD/../src/model/sample.h \
	$$PWD/../src/model/series.h \
	$$PWD/../src/graph/interpolate.h \
	$$PWD/../src/openmeteo/forecast.h \
	$$PWD/../src/wu/reader.h
