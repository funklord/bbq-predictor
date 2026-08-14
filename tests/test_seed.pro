include(test_common.pri)
TARGET = test_seed

# The only test that drives the BUILT PROGRAM rather than linking a part
# of it. --seed-verification's refusal lives in main(), which no test
# binary can link -- a second main() does not link -- so the guard can
# only be checked by running the thing and reading what it did.
SOURCES += test_seed.cpp
