# =============================================================================
# tests.pro -- the suite, built by `make test` and by nothing else
#
# Its own project and its own build directory, because the app's .pro
# has a main() and a test binary cannot have two. The sources it needs
# are named individually rather than globbed: the widgets are not under
# test and pulling them in would make the suite depend on a display.
#
# TEMPLATE = subdirs would build one binary; this builds one per area so
# a failure names its subject in the target that failed.
# =============================================================================

TEMPLATE = subdirs
SUBDIRS = interpolate model reader providers layout client feed history

interpolate.file = test_interpolate.pro
model.file = test_model.pro
reader.file = test_reader.pro
providers.file = test_providers.pro
layout.file = test_layout.pro
client.file = test_client.pro
feed.file = test_feed.pro
history.file = test_history.pro
