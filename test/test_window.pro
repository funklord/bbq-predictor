include(test_common.pri)
TARGET = test_window
QT += gui widgets sql positioning

# The window and everything it owns. It is the largest link in the
# suite, and deliberately so: what this binary exists to check is the
# WIRING between the parts, which is exactly what a narrower link would
# stub out (project.md sec 14.10).
SOURCES += test_window.cpp \
	$$PWD/../src/graph/forecast_graph.cpp \
	$$PWD/../src/model/correction.cpp \
	$$PWD/../src/model/settings.cpp \
	$$PWD/../src/store/history.cpp \
	$$PWD/../src/ui/accessibility.cpp \
	$$PWD/../src/ui/locator.cpp \
	$$PWD/../src/ui/main_window.cpp \
	$$PWD/../src/ui/theme.cpp \
	$$PWD/../src/ui/widget_picture.cpp \
	$$PWD/../src/wu/client.cpp \
	$$PWD/../src/wu/feed.cpp \
	$$PWD/../src/wu/key_source.cpp

HEADERS += \
	$$PWD/../src/graph/forecast_graph.h \
	$$PWD/../src/model/correction.h \
	$$PWD/../src/model/settings.h \
	$$PWD/../src/store/history.h \
	$$PWD/../src/ui/accessibility.h \
	$$PWD/../src/ui/locator.h \
	$$PWD/../src/ui/main_window.h \
	$$PWD/../src/ui/theme.h \
	$$PWD/../src/ui/widget_picture.h \
	$$PWD/../src/wu/client.h \
	$$PWD/../src/wu/feed.h \
	$$PWD/../src/wu/key_source.h
