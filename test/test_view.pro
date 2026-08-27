include(test_common.pri)
TARGET = test_view

# The only test that needs widgets: it drives the graph's own gesture
# handlers, so the widget has to exist and be paintable. Offscreen, and
# the binary sets that itself rather than trusting the environment.
QT += gui widgets

SOURCES += test_view.cpp \
	$$PWD/../src/graph/forecast_graph.cpp \
	$$PWD/../src/ui/accessibility.cpp \
	$$PWD/../src/ui/theme.cpp

HEADERS += \
	$$PWD/../src/graph/forecast_graph.h \
	$$PWD/../src/ui/accessibility.h \
	$$PWD/../src/ui/theme.h
