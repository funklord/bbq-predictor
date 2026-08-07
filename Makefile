# =============================================================================
# Top-level Makefile -- bbqpredictor
#
# PURPOSE
#   The single entry point for the tree. The Qt app itself is built by
#   qmake (bbqpredictor.pro), because moc/uic/rcc do not fit hand-written
#   pattern rules; everything else here is plain make. This file is the
#   wrapper that makes the two look like one interface -- the same split
#   beerssh uses. See project.md sec 5.
#
# TARGETS
#   make              -- build the app (optimized, no sanitizers)
#   make DEBUG=1      -- unoptimized, symbol-rich build. Checked for being
#                        SET, not for a value -- see BUILD FLAGS.
#   make SANITIZE=1   -- add ASan+UBSan, independent of DEBUG
#   make run          -- build and run it
#   make test         -- run the test suite. There is NOT ONE YET, and this
#                        target says so and fails rather than reporting a
#                        pass over nothing.
#   make check        -- everything that must pass before a commit
#   make style        -- indentation gate, plus project.md against the tree
#   make hooks        -- install the commit-msg hook from tools/hooks/
#   make install      -- install the binary and its desktop entry
#   make uninstall    -- remove what install put there
#   make clean        -- remove compilation intermediates only; the built
#                        binary is left in place, so `make install` stays
#                        possible without a rebuild
#   make veryclean    -- clean, plus the binary and the build directories
#   make distclean    -- veryclean, plus stray editor files
#   make help         -- this list
#
# BUILD FLAGS
#   DEBUG and SANITIZE are never given a `?=` default: every check is
#   `ifdef` (set vs unset), so a `?= 0` default would make them
#   permanently "set" and impossible to turn off.
#
# TOOLCHAIN INJECTION
#   Every other variable is `?=`, so the command line and the environment
#   always win over the defaults here:
#     make CXX=clang++ DEBUG=1
#     make QMAKE=/opt/qt6/bin/qmake
#     make BUILD_DIR=/tmp/bbq-asan SANITIZE=1
# =============================================================================

CXX   ?= g++
QMAKE ?= qmake6

TARGET = bbqpredictor

# Every packaging file is named after this, and a mismatch shows up as an
# app that installs, runs, and cannot be found by a launcher.
APP_ID = se.vibes.bbqpredictor

# The one place the version is stated (project.md sec 0). Passed into the
# build rather than repeated in a source file, because a second copy is a
# second thing to forget.
VERSION ?= $(shell cat VERSION)

# Settable, per the global rule. Everything generated lands under here.
BUILD_DIR ?= build

PREFIX  ?= /usr/local
DESTDIR ?=

SOURCES = $(wildcard src/*.cpp src/*/*.cpp)
HEADERS = $(wildcard src/*.h src/*/*.h)

ifdef DEBUG
    QMAKE_CONFIG = CONFIG+=debug CONFIG-=release
else
    QMAKE_CONFIG = CONFIG+=release CONFIG-=debug
endif

ifdef SANITIZE
    QMAKE_CONFIG += QMAKE_CXXFLAGS+=-fsanitize=address,undefined \
                    QMAKE_CXXFLAGS+=-fno-omit-frame-pointer \
                    QMAKE_LFLAGS+=-fsanitize=address,undefined
endif

all: $(TARGET)

$(BUILD_DIR)/Makefile: bbqpredictor.pro
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && $(QMAKE) $(CURDIR)/bbqpredictor.pro $(QMAKE_CONFIG) \
	        QMAKE_CXX=$(CXX) BBQ_VERSION=$(VERSION)

# The sources and headers are prerequisites so that adding a file rebuilds
# through qmake's Makefile rather than being missed. qmake's own generated
# Makefile tracks per-object dependencies; this rule only has to notice
# that the set changed.
$(TARGET): $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(BUILD_DIR)
	cp $(BUILD_DIR)/$(TARGET) $(TARGET)

run: $(TARGET)
	./$(TARGET)

# There is no test suite yet (project.md sec 5).
#
# This target exists and FAILS, rather than being absent or printing a
# cheerful nothing. A `make test` that exits 0 over an empty suite is the
# vacuous pass the guidelines warn about: it reads as "the tests pass" in
# every log and every habit, and the day a real suite arrives nobody
# notices it was never running.
test:
	@echo "test: there is no test suite yet." >&2
	@echo "test:   This target fails deliberately. A green 'make test' over" >&2
	@echo "test:   an empty suite is indistinguishable from a real pass, and" >&2
	@echo "test:   that is the failure mode worth refusing." >&2
	@echo "test:   Remove this message when tests/ exists, and add 'test'" >&2
	@echo "test:   to the 'check' target below." >&2
	@exit 1

style: style-source style-docs

style-source:
	python3 tools/style_gate.py check

# project.md is authoritative, so it is held to the tree: a heading that
# appears twice means whichever one you find, the other is the one with
# the answer.
style-docs:
	python3 tools/style_gate.py docs

# What must pass before committing. GNU's meaning of `check`, and what the
# sibling projects already do.
#
# `test` is NOT in this list yet, and that is deliberate: it fails by
# construction until a suite exists (see above), and a `check` that cannot
# pass is a `check` nobody runs. Add it the moment tests/ is real -- that
# is the whole point of the failing target.
check: style

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently
# on a fresh clone.
hooks:
	@test -d .git || { echo "hooks: not a git repository" >&2; exit 1; }
	@install -m 0755 tools/hooks/commit-msg .git/hooks/commit-msg
	@echo "hooks: commit-msg installed from tools/hooks/"

# The binary alone is not an installation: without a .desktop entry the
# app is invisible to every launcher, which for a tray applet people
# expect to autostart is most of how it gets run.
install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 packaging/$(APP_ID).desktop \
	        $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop

# clean removes intermediates only; $(TARGET) survives, so `make install`
# stays possible without a rebuild.
clean:
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi

# Removes a whole directory, having first checked it is one we could
# plausibly have made.
#
# The build directory is ours -- this Makefile created it and qmake owns
# its contents -- so removing it wholesale is a named delete rather than a
# wildcard sweep. That argument holds only for the path actually passed,
# and `rm -rf $(VAR)` is exactly how a clean target eats something it
# should not: a variable unset, mistyped, or overridden on the command
# line with an absolute path. BUILD_DIR is settable by design, so
# `make veryclean BUILD_DIR=$$HOME` is a command somebody can type.
#
# Each path is checked before it goes: relative, no parent escape, not the
# working directory. An empty variable disappears in the shell's word
# splitting and removes nothing, which is the safe direction to fail in.
define bbq_remove_tree
	@for dir in $(1); do \
		case "$$dir" in \
			/*) echo "veryclean: refusing to remove the absolute path $$dir" >&2; exit 1 ;; \
			*..*) echo "veryclean: refusing to remove $$dir -- it escapes the tree" >&2; exit 1 ;; \
			.|./) echo "veryclean: refusing to remove the working directory" >&2; exit 1 ;; \
		esac; \
		rm -rf "$$dir"; \
	done
endef

veryclean: clean
	$(call bbq_remove_tree,$(BUILD_DIR))
	rm -f $(TARGET)

distclean: veryclean
	rm -f .qmake.stash
	find . -name '*~' -o -name '*.swp' | xargs -r rm -f

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: all run test check style style-source style-docs hooks \
        install uninstall clean veryclean distclean help
