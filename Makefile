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
#   make android      -- the Android APK (needs a Qt kit, NDK, SDK, JDK)
#   make android-check     -- everything that build needs, checked by name
#   make android-install   -- put that APK on the attached phone (adb)
#   make android-run       -- install it and launch it
#   make android-log       -- follow this app's log, and only this app's
#   make android-uninstall -- remove it from the phone
#   make android-aab       -- the Play bundle instead; needs a keystore
#
#   The android-* targets from `install` down need a device on adb.
#   Note `make install` is the DESKTOP one -- it installs the binary and
#   its desktop entry on this machine, not on a phone.
#   make help         -- this list
#
# ANDROID
#   The target names and the adb plumbing come from tools/android.mk, which
#   is spread verbatim from ~/.claude/tools/android.mk so a habit learned in
#   one project is correct in the next. This file supplies only the build
#   rule, because qmake and CMake differ and that difference is the project's.
#
#     make android QT_ANDROID_ROOT=$HOME/Qt/6.12.0/android_arm64_v8a \
#                  ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018
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

# Stated before the include, and load-bearing.
#
# `include` is where make FIRST sees a target, so a fragment pulled in ahead
# of `all` silently becomes the default goal -- and a plain `make` then runs
# android-check and fails, having built nothing. That is exactly what
# happened here, and it survived four sessions because none of them built.
.DEFAULT_GOAL := all

# The shared vocabulary. Included before the rules below so a project rule
# can use ANDROID_ABI and friends without redefining any of them.
include tools/android.mk

ANDROID_BUILD_DIR ?= build-android
ANDROID_ARTIFACT = $(ANDROID_BUILD_DIR)/$(TARGET)-$(VERSION)-$(ANDROID_ABI).apk

# Where a build lands, and why a custom BUILD_DIR must not land here.
#
# A non-default BUILD_DIR means an ISOLATED build -- sanitized, cross, a
# second ABI -- and isolating it is pointless if the result then replaces
# the plain binary in the tree. It did: the recipe below copied to
# ./$(TARGET) unconditionally, so the README's own example,
#
#     make BUILD_DIR=/tmp/bbq-asan SANITIZE=1
#
# left a sanitized binary sitting at ./$(TARGET), where it is slower,
# behaves differently, and looks exactly like the ordinary one. Found by
# running it. build-and-commit.md asks for the opposite in as many words.
ifeq ($(BUILD_DIR),build)
ARTIFACT = $(TARGET)
else
ARTIFACT = $(BUILD_DIR)/$(TARGET)
endif

all: $(ARTIFACT)

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
$(BUILD_DIR)/$(TARGET): $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(BUILD_DIR)

# Only for the default build directory, per the note above.
$(TARGET): $(BUILD_DIR)/$(TARGET)
	cp $(BUILD_DIR)/$(TARGET) $(TARGET).new
	mv -f $(TARGET).new $(TARGET)

run: $(ARTIFACT)
	./$(ARTIFACT)

# --- Android ---------------------------------------------------------------
# The build rule is this project's; everything around it is tools/android.mk.
$(ANDROID_BUILD_DIR)/Makefile: android-check bbq-predictor.pro
	mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && $(QT_ANDROID_ROOT)/bin/qmake \
	        $(CURDIR)/bbq-predictor.pro $(QMAKE_CONFIG) \
	        BBQ_VERSION=$(VERSION) BBQ_VERSION_CODE=$(ANDROID_VERSION_CODE) \
	        BBQ_TARGET_API=$(ANDROID_TARGET_API)

# Named for what it holds, because Gradle's own name for it says nothing
# about which app, which version or which ABI you are looking at.
# Qt's generated `apk` target is two steps: build-and-install, then
# androiddeployqt. Only the second is replaced here, and only to pass
# --android-platform, which the generated rule hardcodes away. The first is
# reused rather than reimplemented, because an orchestration copied is an
# orchestration to keep in step.
#
# Gradle then runs a SECOND time, for one property, and that is the whole
# reason TLS works on the device.
#
# Qt's TLS backend finds OpenSSL by SCANNING a directory for files matching
# libcrypto.* and libssl.*. With the modern default the libraries are never
# unpacked -- they sit inside the APK, where a directory scan cannot see
# them -- so everything looks right and TLS still fails: the libraries are
# in the package, the app runs, and Qt reports "Failed to load
# libssl/libcrypto". legacyPackaging=true sets extractNativeLibs, and
# Android then writes them to lib/<abi>/ where Qt is looking.
#
# androiddeployqt has no flag for it and writes gradle.properties itself, so
# the property goes on Gradle's command line where nothing overwrites it.
# The obvious shortcut -- androiddeployqt --no-build, then Gradle -- does
# not work: from a clean tree that leaves no Gradle project at all. It
# appeared to work only because a previous build had left one behind, which
# is the same stale-state trap this project keeps finding.
android: $(ANDROID_BUILD_DIR)/Makefile
	#
	# The staging directory is cleared first, because nothing else
	# prunes it.
	#
	# androiddeployqt COPIES extra libraries in and leaves whatever is
	# already there. Two libraries dropped from ANDROID_EXTRA_LIBS, and
	# deleted from deps/, still shipped in the package -- the build was
	# reporting a configuration that no longer existed anywhere in the
	# tree. Removing a file from a project should remove it from the
	# artifact.
	#
	# Wholesale removal is safe in exactly this shape: the directory is
	# one this build created, it is named relative to a BUILD_DIR the
	# project owns, and the next step refills it.
	@test -n "$(ANDROID_BUILD_DIR)" || { echo "ANDROID_BUILD_DIR is empty" >&2; exit 1; }
	rm -rf $(ANDROID_BUILD_DIR)/android-build/libs
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk_install_target
	$(ANDROID_DEPLOY_QT) \
	        --input $(ANDROID_BUILD_DIR)/android-$(TARGET)-deployment-settings.json \
	        --output $(ANDROID_BUILD_DIR)/android-build \
	        --android-platform $(ANDROID_PLATFORM) \
	        --apk $(ANDROID_BUILD_DIR)/android-build/$(TARGET).apk
	cd $(ANDROID_BUILD_DIR)/android-build && ./gradlew \
	        -PlegacyPackaging=true assembleDebug
	@src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk \
	        -name '*.apk' -print -quit); \
	if [ -z "$$src" ]; then echo "android: no .apk was produced" >&2; exit 1; fi; \
	cp "$$src" $(ANDROID_ARTIFACT); \
	echo "android: $(ANDROID_ARTIFACT)"
	$(call android_verify_signature,$(ANDROID_ARTIFACT))

android-aab: $(ANDROID_BUILD_DIR)/Makefile
	@if [ -z "$(ANDROID_KEYSTORE)" ]; then \
		echo "android-aab: ANDROID_KEYSTORE is not set." >&2; \
		echo "android-aab:   Play will not take a debug-signed bundle, and a" >&2; \
		echo "android-aab:   versionCode it HAS taken cannot be reused." >&2; \
		exit 1; \
	fi
	$(MAKE) -C $(ANDROID_BUILD_DIR) aab

# --- the suite ---------------------------------------------------------
TEST_BUILD_DIR ?= build-test
TEST_SOURCES = $(wildcard test/*.cpp test/*.pro test/*.pri)

$(TEST_BUILD_DIR)/Makefile: test/tests.pro $(TEST_SOURCES)
	mkdir -p $(TEST_BUILD_DIR)
	cd $(TEST_BUILD_DIR) && $(QMAKE) $(CURDIR)/test/tests.pro $(QMAKE_CONFIG) \
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

# The suite depends on the APPLICATION as well as on itself, which is new
# and deliberate: test_seed drives the built binary to check that
# --seed-verification refuses the real archive, and a guard of that kind
# cannot be tested by linking a library. BBQ_APP_BINARY tells it where to
# look, so the test fails loudly rather than skipping when it is absent --
# a skipped guard reads exactly like a guard that passed.
test: tests-build $(ARTIFACT)
	@failed=0; ran=0; \
	for binary in $(TEST_BUILD_DIR)/test_*; do \
		[ -x "$$binary" ] && [ -f "$$binary" ] || continue; \
		ran=$$((ran + 1)); \
		echo "--- $$binary"; \
		BBQ_APP_BINARY="$(abspath $(ARTIFACT))" $(TEST_CRASH_ENV) timeout $(TEST_TIMEOUT) "$$binary" || failed=$$((failed + 1)); \
	done; \
	if [ "$$ran" -eq 0 ]; then \
		echo "test: no test binaries were found in $(TEST_BUILD_DIR)." >&2; \
		echo "test:   That is a collapsed suite, not a clean one -- a run" >&2; \
		echo "test:   over zero binaries exits 0 and reads exactly like a" >&2; \
		echo "test:   pass. Check that test/tests.pro still lists them." >&2; \
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
# `test` joined this the moment test/ became real, which was the whole
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
	install -m 0755 $(ARTIFACT) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 packaging/$(APP_ID).desktop \
	        $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop

	# The icon the desktop entry has been naming since the first commit.
	# Without it the launcher shows a placeholder, which is what it did.
	mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 packaging/$(APP_ID).svg \
	        $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg

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
	$(call bbq_remove_tree,$(BUILD_DIR) $(TEST_BUILD_DIR) $(ANDROID_BUILD_DIR))
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
        android android-aab \
        install uninstall clean veryclean distclean help
