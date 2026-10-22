include(test_common.pri)
TARGET = test_publisher

# Drives the BUILT publisher and applet, as test_seed drives the applet:
# what it checks lives in two main()s, and a second main() does not link.
SOURCES += test_publisher.cpp
