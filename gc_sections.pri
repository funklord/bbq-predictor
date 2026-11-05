# Let the linker drop what nothing calls (the global size rule, beside -Os).
#
# Each function and object gets its own section, and the link keeps only
# the sections something reaches. Without it the linker keeps every
# object on the link line whole, so each unused function in a file rides
# along in every binary that links it -- which -Os cannot see, working one
# function at a time. Measured on this tree in project.md sec 5.3.
#
# Included by core.pri, for the applet, the publisher and the Android
# library, and by test/test_common.pri, so the suite runs code collected
# the same way. Code reached only by name is collected unless something
# keeps it: an exported symbol is kept, which covers the main() Qt's
# Android loader looks up and every JNI entry point.
QMAKE_CFLAGS   += -ffunction-sections -fdata-sections
QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
QMAKE_LFLAGS   += -Wl,--gc-sections
