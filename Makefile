# =============================================================================
# Top-level Makefile -- bbq-predictor
#
# PURPOSE
#   The single entry point for the tree. The Qt app itself is built by
#   qmake (bbq-predictor.pro), because moc/uic/rcc do not fit hand-written
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
#   make test         -- build and run the suite. Built by this target and
#                        by nothing else, so a plain build stays fast --
#                        which is paid for by never judging a test from a
#                        binary this target did not rebuild.
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

TARGET = bbq-predictor

# Every packaging file is named after this, and a mismatch shows up as an
# app that installs, runs, and cannot be found by a launcher.
APP_ID = se.vibes.bbq-predictor

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

$(BUILD_DIR)/Makefile: bbq-predictor.pro
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && $(QMAKE) $(CURDIR)/bbq-predictor.pro $(QMAKE_CONFIG) \
	        QMAKE_CXX=$(CXX) BBQ_VERSION=$(VERSION)

# The sources and headers are prerequisites so that adding a file rebuilds
# through qmake's Makefile rather than being missed. qmake's own generated
# Makefile tracks per-object dependencies; this rule only has to notice
# that the set changed.
# Copied beside the target and RENAMED over it, never written through.
#
# Writing directly fails with ETXTBSY the moment the previous build is
# running -- the kernel refuses to modify a file it is executing -- so
# `make` broke for exactly as long as the app was open. That is a poor
# trade for a GUI you are supposed to leave running while you work on it.
#
# rename(2) replaces the directory entry rather than the file. The
# running process keeps its own inode and carries on undisturbed with
# the old code, and the next launch picks up the new binary. It is also
# atomic, so nothing ever sees a half-copied executable at this path.
#
# The temporary lives in the same directory on purpose: rename cannot
# cross filesystems, and /tmp frequently is one.
$(TARGET): $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(BUILD_DIR)
	cp $(BUILD_DIR)/$(TARGET) $(TARGET).new
	mv -f $(TARGET).new $(TARGET)

run: $(TARGET)
	./$(TARGET)

# --- the suite ---------------------------------------------------------
TEST_BUILD_DIR ?= build-tests
TEST_SOURCES = $(wildcard tests/*.cpp tests/*.pro tests/*.pri)

$(TEST_BUILD_DIR)/Makefile: tests/tests.pro $(TEST_SOURCES)
	mkdir -p $(TEST_BUILD_DIR)
	cd $(TEST_BUILD_DIR) && $(QMAKE) $(CURDIR)/tests/tests.pro $(QMAKE_CONFIG) \
	        QMAKE_CXX=$(CXX)

tests-build: $(TEST_BUILD_DIR)/Makefile
	$(MAKE) -C $(TEST_BUILD_DIR)

# Runs every binary the suite built, and reports which one failed rather
# than only that something did.
#
# QtTest forks gdb on a fatal signal and leaves it attached with the
# binary stopped, which is how a sibling project accumulated 15 GB of
# resident memory across one session -- a stopped process ignores
# SIGTERM, so the obvious pkill cleans up nothing. Disabled here; ask
# for a backtrace explicitly with BBQ_TEST_STACK_DUMP=1.
ifdef BBQ_TEST_STACK_DUMP
    TEST_CRASH_ENV =
else
    TEST_CRASH_ENV = QTEST_DISABLE_STACK_DUMP=1 QTEST_DISABLE_CORE_DUMP=1
endif

# Each binary is bounded from outside as well as being short by
# construction: a suite that hangs must not hang the machine that ran it.
TEST_TIMEOUT ?= 120

test: tests-build
	@failed=0; ran=0; \
	for binary in $(TEST_BUILD_DIR)/test_*; do \
		[ -x "$$binary" ] && [ -f "$$binary" ] || continue; \
		ran=$$((ran + 1)); \
		echo "--- $$binary"; \
		$(TEST_CRASH_ENV) timeout $(TEST_TIMEOUT) "$$binary" || failed=$$((failed + 1)); \
	done; \
	if [ "$$ran" -eq 0 ]; then \
		echo "test: no test binaries were found in $(TEST_BUILD_DIR)." >&2; \
		echo "test:   That is a collapsed suite, not a clean one -- a run" >&2; \
		echo "test:   over zero binaries exits 0 and reads exactly like a" >&2; \
		echo "test:   pass. Check that tests/tests.pro still lists them." >&2; \
		exit 1; \
	fi; \
	echo "test: $$ran binary(ies), $$failed failed"; \
	[ "$$failed" -eq 0 ]

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
# `test` joined this the moment tests/ became real, which was the whole
# point of the target that failed until then.
check: style test

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
	rm -f $(TARGET).new
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi
	@if [ -f $(TEST_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(TEST_BUILD_DIR) clean; fi

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
	$(call bbq_remove_tree,$(BUILD_DIR) $(TEST_BUILD_DIR))
	rm -f $(TARGET)

# `distclean` removes what the build generated and `veryclean` does not.
# `.qmake.stash` is qmake's, is named, and is disposable by construction.
#
# **It no longer sweeps the tree for editor droppings.** `*~` and `*.swp` are
# not build output: they belong to somebody's editor, and the build system
# has no business deleting files it did not create. The sweep was also
# unbounded -- `find .` walks `.git` too, and it was measured deleting files
# in there. `git clean -xdn` lists that class of file and is the person's
# call, not the build's.
distclean: veryclean
	rm -f .qmake.stash

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: all run test tests-build check style style-source style-docs hooks \
        install uninstall clean veryclean distclean help
