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
#   make test-clocks  -- the suite at six clock readings around midnight,
#                        because a fixture that depends on the hour is
#                        invisible until the hour comes round. Needs
#                        faketime, and REFUSES rather than skipping
#                        without it (sec 16.105)
#   make check        -- everything that must pass before a commit
#   make style        -- eleven gates: indentation, project.md against
#                        the tree, every signal has a listener, the
#                        manual page against the options, palette
#                        contrast, the fetch exit codes against the
#                        unit, the XML files, the build wiring, the
#                        setup calls that fail silently, QtTest slots
#                        QtTest would not run, and every source being
#                        linked by some test
#   make hooks        -- install the commit-msg hook from tool/hooks/
#   make install      -- install the binary, desktop entry, icon and
#                        manual page under PREFIX
#   make check-boot   -- boot the server package under sysvinit and
#                        OpenRC in hull vms (KVM, hull)
#   make deb          -- a Debian package: the applet, a systemd timer
#                        that fetches on its own clock, and the
#                        bbq-predictor account it runs as. Lands in
#                        $(DEB_DIR). Runs the suite on the way through
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
#   The target names and the adb plumbing come from tool/android.mk, which
#   is spread verbatim from ~/.claude/tool/android.mk so a habit learned in
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
APP_ID = se.vibes.bbq_predictor

# The desktop entry and icon, which are NOT the Android id.
#
# APP_ID must be a legal Java package name, so it carries an underscore;
# freedesktop ids conventionally use the hyphen the project's own name
# has. Sharing one variable between them broke `make install` the day
# the Android id was introduced -- it looked for a file with an
# underscore that has never existed -- and nothing noticed, because
# nothing in the build or the gates installs anything. Packaging is what
# found it.
DESKTOP_ID = se.vibes.bbq-predictor

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

# The leaks this project has judged, so a sanitized run that reports
# anything else means something (project.md sec 16.100). Without it the
# suite is red for one Qt-internal allocation and stops being read.
ifdef SANITIZE
    TEST_SANITIZE_ENV = LSAN_OPTIONS=suppressions=$(CURDIR)/tool/leaks.supp
endif

# Debug info WITHOUT a debug build, which is what packaging wants.
#
# The shipped binary is unaffected: dh_strip moves the debug sections out
# into the -dbgsym package and strips what it leaves behind, so this
# costs build time and the size of a package nobody installs by accident,
# and buys a backtrace from a crash on somebody else's machine.
#
# Without it that package is built anyway and holds nothing, which
# lintian reports as debug-file-with-no-debug-symbols -- a package
# promising symbols and shipping a build-id link. `force_debug_info` is
# qmake's own switch for this and adds -g to a release build.
ifdef DEBUG_INFO
    QMAKE_CONFIG += CONFIG+=force_debug_info
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
include tool/android.mk

ANDROID_BUILD_DIR ?= build-android-$(ANDROID_ABI)
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

# The daemon that serves the published archive (project.md sec 15.12.2).
# Plain C on libc, no Qt, so it is built by the C compiler here rather
# than by qmake. One compile-and-link call: the headers are prerequisites
# so an edit to one rebuilds it, and $(filter %.c,$^) keeps them off the
# command line. Same naming and rename-into-place as the applet, for the
# same reasons -- a sanitized build stays in its BUILD_DIR, and a running
# daemon does not make the build fail with ETXTBSY.
SERVE_TARGET = bbq-predictor-serve
ifeq ($(BUILD_DIR),build)
SERVE_ARTIFACT = $(SERVE_TARGET)
else
SERVE_ARTIFACT = $(BUILD_DIR)/$(SERVE_TARGET)
endif
# After CFLAGS on the command line, not before: dpkg-buildflags hands
# over -O2, and the last -O wins -- the package was built for speed until
# the order changed. Size is this workspace's rule (build-and-commit.md).
SERVE_CFLAGS = -std=c11 -Os -Wall -Wextra -Isrc

$(SERVE_ARTIFACT): src/serve/serve.c src/serve/serve_loop.c \
                   src/serve/serve_core.c $(wildcard src/serve/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVE_CFLAGS) $(LDFLAGS) \
	        -o $@.new $(filter %.c,$^)
	mv -f $@.new $@

# The headless publisher (project.md sec 15.13): core.pri and the signing,
# on QtCore, QtNetwork and QtSql. A qmake project of its own in a build
# directory of its own, since qmake writes one Makefile and one flat object
# directory per project, and two projects in one directory overwrite each
# other's -- the client.o collision of sec 15.12.6, one level up.
PUBLISH_TARGET = bbq-predictor-publish
PUBLISH_BUILD_DIR = $(BUILD_DIR)/publish
ifeq ($(BUILD_DIR),build)
PUBLISH_ARTIFACT = $(PUBLISH_TARGET)
else
PUBLISH_ARTIFACT = $(PUBLISH_BUILD_DIR)/$(PUBLISH_TARGET)
endif

# SERVER_ONLY=1 builds, tests and installs the server half alone: the
# publisher and the daemon (project.md sec 15.15). For a release whose Qt
# predates the applet's -- Devuan daedalus and Debian bookworm have 6.4,
# and the window uses 6.5's colour scheme and permissions -- where the
# server builds and the applet does not, and a server does not want it.
# The Debian profile pkg.bbq-predictor.server-only sets it.
SERVER_ONLY ?=
ifeq ($(SERVER_ONLY),1)
APPLET_ARTIFACT =
else
APPLET_ARTIFACT = $(ARTIFACT)
endif

all: $(APPLET_ARTIFACT) $(SERVE_ARTIFACT) $(PUBLISH_ARTIFACT)

# The vendored submodules, fetched when absent and never moved
# (makefile-hazards: the build fetches what it vendors, and fails loudly
# when it cannot). monocypher signs and verifies the published archive
# (project.md sec 15.12.2); without it the build would stop on a missing
# header much later, naming nothing useful.
#
# Asks git nothing when every directory is populated, so an ordinary build
# spawns nothing. When git refuses, its own words are printed: the message
# said "this is not a git checkout" for any refusal, and for a day CI's
# real one -- a checkout owned by another uid, "dubious ownership" -- read
# as an export. A submodule present at another commit is left alone:
# that is somebody's work, and moving a pin is a reviewed commit.
SUBMODULE_PATHS = monocypher

submodules:
	@missing=''; \
	for p in $(SUBMODULE_PATHS); do \
		if [ ! -d "$$p" ] || [ -z "$$(ls -A "$$p" 2>/dev/null)" ]; then \
			missing="$$missing $$p"; \
		fi; \
	done; \
	if [ -z "$$missing" ]; then exit 0; fi; \
	if ! command -v git >/dev/null 2>&1; then \
		echo "error: not checked out:$$missing, and git is not installed" >&2; \
		exit 1; \
	fi; \
	if ! said=$$(git rev-parse --git-common-dir 2>&1); then \
		echo "error: not checked out:$$missing, and git will not read this" >&2; \
		echo "       tree; git says:" >&2; \
		echo "$$said" | sed 's/^/       /' >&2; \
		echo "       (an export by git archive carries no submodule contents)" >&2; \
		exit 1; \
	fi; \
	echo "==> fetching submodules:$$missing"; \
	git submodule update --init -- $$missing || { \
		echo "error: fetching$$missing failed; it is public HTTPS and" >&2; \
		echo "       wants only a network" >&2; \
		exit 1; \
	}

$(BUILD_DIR)/Makefile: bbq-predictor.pro | submodules
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

$(PUBLISH_BUILD_DIR)/Makefile: bbq-predictor-publish.pro core.pri | submodules
	mkdir -p $(PUBLISH_BUILD_DIR)
	cd $(PUBLISH_BUILD_DIR) && $(QMAKE) $(CURDIR)/bbq-predictor-publish.pro \
	        $(QMAKE_CONFIG) QMAKE_CXX=$(CXX) BBQ_VERSION=$(VERSION)

$(PUBLISH_BUILD_DIR)/$(PUBLISH_TARGET): $(PUBLISH_BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(PUBLISH_BUILD_DIR)

ifeq ($(BUILD_DIR),build)
$(PUBLISH_TARGET): $(PUBLISH_BUILD_DIR)/$(PUBLISH_TARGET)
	cp $(PUBLISH_BUILD_DIR)/$(PUBLISH_TARGET) $(PUBLISH_TARGET).new
	mv -f $(PUBLISH_TARGET).new $(PUBLISH_TARGET)
endif

# The publisher links nothing that draws (sec 15.13), read from the ELF
# header the dynamic loader reads. A QColor or a QPixmap reaching a core
# source pulls Gui in silently -- the build succeeds and the server package
# gains the desktop's libraries -- so this is where it surfaces. It also
# requires Core, so an unreadable or empty header fails rather than
# passing over nothing.
PUBLISH_FORBIDDEN = Qt6Gui Qt6Widgets Qt6Positioning Qt6OpenGL Qt6DBus

check-publish-libraries: $(PUBLISH_ARTIFACT)
	@needed=$$(readelf -d $(PUBLISH_ARTIFACT) | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); \
	if ! echo "$$needed" | grep -q '^libQt6Core\.so'; then \
		echo "check-publish-libraries: no libQt6Core in $(PUBLISH_ARTIFACT)'s" >&2; \
		echo "  header, so this read nothing it can vouch for" >&2; \
		exit 1; \
	fi; \
	bad=0; \
	for module in $(PUBLISH_FORBIDDEN); do \
		if echo "$$needed" | grep -q "^lib$$module\.so"; then \
			echo "check-publish-libraries: $(PUBLISH_ARTIFACT) links lib$$module" >&2; \
			bad=1; \
		fi; \
	done; \
	[ $$bad -eq 0 ] || exit 1; \
	echo "check-publish-libraries: $$(echo "$$needed" | grep -c '^libQt6') Qt module(s)," \
	     "none that draws: $$(echo "$$needed" | grep '^libQt6' | tr '\n' ' ')"

run: $(ARTIFACT)
	./$(ARTIFACT)

# --- Android ---------------------------------------------------------------
# The build rule is this project's; everything around it is tool/android.mk.
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
# A directory of its own for the server-only suite, which builds fewer
# projects: one tree reused across the two would keep whichever Makefile
# qmake wrote first and build the wrong set without a word.
ifeq ($(SERVER_ONLY),1)
TEST_BUILD_DIR ?= build-test-server
else
TEST_BUILD_DIR ?= build-test
endif

# What the tests that drive the built programs are told. Without the
# applet, test_publisher is TOLD there is none, rather than finding it
# absent, which it is right to fail on.
ifeq ($(SERVER_ONLY),1)
TEST_APPLET_ENV = BBQ_NO_APPLET=1
else
TEST_APPLET_ENV = BBQ_APP_BINARY="$(abspath $(ARTIFACT))"
endif
TEST_SOURCES = $(wildcard test/*.cpp test/*.pro test/*.pri)

$(TEST_BUILD_DIR)/Makefile: test/tests.pro $(TEST_SOURCES) | submodules
	mkdir -p $(TEST_BUILD_DIR)
	cd $(TEST_BUILD_DIR) && $(QMAKE) $(CURDIR)/test/tests.pro $(QMAKE_CONFIG) \
	        QMAKE_CXX=$(CXX) BBQ_SERVER_ONLY=$(SERVER_ONLY)

tests-build: $(TEST_BUILD_DIR)/Makefile
	$(MAKE) -C $(TEST_BUILD_DIR)

# The suite at hostile clock readings (project.md sec 16.105).
#
# A fixture that quietly depends on the hour is invisible until the hour
# comes round. One did: a station is quiet when its newest sample is
# TODAY'S and more than forty-five minutes old, so the case cannot be
# built in the first forty-five minutes of a day, and the test asserting
# it was wrong for seventy-eight minutes of every day from the day it was
# written. It surfaced because a session ran past midnight, and inside
# the window it reads as a regression in whatever is being changed.
#
# The times are chosen for the boundary rather than spread evenly: either
# side of midnight, inside the window that was broken, and one ordinary
# afternoon as the control that says the sweep can pass at all.
#
# NOT part of `check`. It runs the whole suite once per reading and is a
# deliberate act, like the sanitized build.
#
# It REFUSES rather than skipping when faketime is absent. A check that
# needs a tool it cannot find, and says nothing, is indistinguishable
# from one that ran -- and this is exactly the class of defect that hides
# in the difference.
TEST_CLOCKS ?= 00:05:00 00:30:00 01:10:00 12:00:00 23:50:00 23:59:30

test-clocks: tests-build $(APPLET_ARTIFACT) $(PUBLISH_ARTIFACT)
	@command -v faketime >/dev/null 2>&1 || { \
		echo "test-clocks: faketime is not installed." >&2; \
		echo "test-clocks:   apt install faketime" >&2; \
		echo "test-clocks:   Refusing rather than skipping: a check that" >&2; \
		echo "test-clocks:   says nothing reads exactly like one that ran." >&2; \
		exit 1; \
	}
	@today=$$(date +%Y-%m-%d); failed=0; ran=0; \
	for at in $(TEST_CLOCKS); do \
		echo "--- $$today $$at"; \
		ran=$$((ran + 1)); \
		for binary in $(TEST_BUILD_DIR)/test_*; do \
			[ -x "$$binary" ] && [ -f "$$binary" ] || continue; \
			$(TEST_APPLET_ENV) \
			BBQ_PUBLISH_BINARY="$(abspath $(PUBLISH_ARTIFACT))" $(TEST_CRASH_ENV) \
			        faketime "$$today $$at" \
			        timeout $(TEST_TIMEOUT) "$$binary" >/dev/null 2>&1 \
			        || { echo "test-clocks: $$binary failed at $$at" >&2; \
			             failed=$$((failed + 1)); }; \
		done; \
	done; \
	if [ "$$ran" -eq 0 ]; then \
		echo "test-clocks: no clock readings were given" >&2; exit 1; \
	fi; \
	echo "test-clocks: $$ran reading(s), $$failed failure(s)"; \
	[ "$$failed" -eq 0 ]

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
# a skipped guard reads exactly like a guard that passed. test_publisher
# does the same with the publisher, through BBQ_PUBLISH_BINARY.
test: tests-build $(APPLET_ARTIFACT) $(PUBLISH_ARTIFACT)
	@failed=0; ran=0; \
	for binary in $(TEST_BUILD_DIR)/test_*; do \
		[ -x "$$binary" ] && [ -f "$$binary" ] || continue; \
		ran=$$((ran + 1)); \
		echo "--- $$binary"; \
		$(TEST_APPLET_ENV) \
		BBQ_PUBLISH_BINARY="$(abspath $(PUBLISH_ARTIFACT))" $(TEST_CRASH_ENV) $(TEST_SANITIZE_ENV) timeout $(TEST_TIMEOUT) "$$binary" || failed=$$((failed + 1)); \
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

style: style-source style-docs style-signals style-man style-palette style-exits style-xml style-wiring \
       style-setup style-tests style-links

style-source:
	python3 tool/style_gate.py check

# project.md is authoritative, so it is held to the tree: a heading that
# appears twice means whichever one you find, the other is the one with
# the answer.
style-docs:
	python3 tool/style_gate.py docs

# The fetch exit codes are a contract between the program, its header,
# the systemd unit that forgives one of them, and the manual page. Drift
# fails silently in the worst direction: a timer that marks itself failed
# on every quiet station, or one that forgives the outage it exists to
# report.
style-xml:
	python3 tool/xml_gate.py

style-wiring:
	python3 tool/build_wiring.py

# Calls that must happen once and whose absence nothing else notices: the
# program builds, the suite passes, and a platform nobody tests here
# misbehaves for somebody else (sec 16.71.2).
style-setup:
	python3 tool/setup_calls.py

# A QtTest slot whose name ends in _data is read as a DATA FUNCTION, so
# it never runs and the suite reports the same "all passed" it would
# report if the test were there (project.md sec 16.95).
style-tests:
	python3 tool/test_slots.py

# A source file nobody linked is untested in the one way the suite
# cannot report: it still builds, still passes, and does not contain
# the file. Sec 16.111.
style-links:
	python3 tool/test_links.py

style-exits:
	python3 tool/exit_codes.py

# Every colour drawn on another must stay legible on it, in both
# schemes. The knowledge is WHICH pairs are drawn together: a naive sweep
# of every colour against the plot background reports eleven violations
# in a palette that has none, because gridlines are meant to be faint and
# the readout's text sits on the readout's own box.
style-palette:
	python3 tool/palette_contrast.py

# The manual page is a page of countable claims about the program, and
# options accrete faster than anybody re-reads it. Checked both ways: an
# option missing from the page cannot be discovered, and one documented
# but not accepted sends a reader to type something that fails.
style-man:
	python3 tool/man_options.py

# A signal nobody connected is not a compile error, not a warning and not
# a failing test -- the emit runs and nothing happens. Twice now that has
# been how a feature was missing rather than broken.
style-signals:
	python3 tool/signal_listeners.py

# What must pass before committing. GNU's meaning of `check`, and what the
# sibling projects already do.
#
# `test` joined this the moment test/ became real, which was the whole
# point of the target that failed until then.
check: style test check-publish-libraries check-android-compile

# THE ANDROID-ONLY CODE, COMPILED (project.md sec 11.13).
#
# A branch under Q_OS_ANDROID is compiled by nothing else in `check`, so
# a mistake there passes every gate and the whole suite: one did, for
# half a day, until an APK was built (sec 2.6.1.4). This compiles exactly
# the sources holding such a branch, with the kit's own flags taken from
# the Makefile qmake writes for Android, and links and packages nothing.
#
# In a directory of its own, so it neither waits on nor disturbs the APK
# build's. Without an Android kit it FAILS, naming why, rather than
# passing over nothing; ANDROID_COMPILE=no skips it on purpose.
ANDROID_COMPILE ?= yes
ANDROID_COMPILE_DIR = build-android-compile-$(ANDROID_ABI)
ANDROID_ONLY_SOURCES = $(shell grep -lE 'Q_OS_ANDROID|__ANDROID__' $(SOURCES))
ANDROID_ONLY_OBJECTS = $(patsubst %.cpp,%.o,$(notdir $(ANDROID_ONLY_SOURCES)))

$(ANDROID_COMPILE_DIR)/Makefile: bbq-predictor.pro core.pri
	mkdir -p $(ANDROID_COMPILE_DIR)
	cd $(ANDROID_COMPILE_DIR) && $(QT_ANDROID_ROOT)/bin/qmake \
	        $(CURDIR)/bbq-predictor.pro $(QMAKE_CONFIG) \
	        BBQ_VERSION=$(VERSION) BBQ_VERSION_CODE=$(ANDROID_VERSION_CODE) \
	        BBQ_TARGET_API=$(ANDROID_TARGET_API)

check-android-compile:
ifeq ($(ANDROID_COMPILE),no)
	@echo "check-android-compile: skipped, because ANDROID_COMPILE=no"
else
	@if [ -z "$(ANDROID_ONLY_OBJECTS)" ]; then \
		echo "check-android-compile: no source holds Q_OS_ANDROID, which" >&2; \
		echo "  is not the same as having no Android code -- the" >&2; \
		echo "  pattern has stopped matching" >&2; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory android-check >/dev/null || { \
		echo "check-android-compile: no usable Android kit (make android-check" >&2; \
		echo "  says why). Install one, or skip this with ANDROID_COMPILE=no." >&2; \
		exit 1; \
	}
	@$(MAKE) --no-print-directory $(ANDROID_COMPILE_DIR)/Makefile >/dev/null 2>&1 || { \
		echo "check-android-compile: qmake could not configure the Android tree" >&2; \
		exit 1; \
	}
	@$(MAKE) --no-print-directory -C $(ANDROID_COMPILE_DIR) $(ANDROID_ONLY_OBJECTS) \
	        >$(ANDROID_COMPILE_DIR)/compile.log 2>&1 || { \
		grep -E 'error' $(ANDROID_COMPILE_DIR)/compile.log >&2; \
		echo "check-android-compile: an Android-only source does not compile;" >&2; \
		echo "  $(ANDROID_COMPILE_DIR)/compile.log has the rest" >&2; \
		exit 1; \
	}
	@echo "check-android-compile: $(words $(ANDROID_ONLY_OBJECTS)) Android-only" \
	      "source(s) compile for $(ANDROID_ABI): $(notdir $(ANDROID_ONLY_SOURCES))"
endif

# The daemon's request path under libFuzzer (project.md sec 15.12.2).
#
# Bounded by FUZZ_SECONDS inside the fuzzer, not by a wrapper, so a run
# typed by hand stops as well. Not part of check: it needs clang and
# takes a minute, and a fuzz run that found nothing yesterday proves
# little about today. A crash leaves its input in the corpus directory.
FUZZ_SECONDS ?= 60
FUZZ_DIR ?= $(TEST_BUILD_DIR)/fuzz

# The applet's reader under libFuzzer (project.md sec 15.12), bounded by
# FUZZ_SECONDS the same way. Seeded from real bodies the encoder writes,
# so the run starts inside every structure. Refuses to start if the
# reader calls framed() or required(), which read out of bounds before
# the vendored situ commit and which this run does not cover
# (src/wire/vendor/README.md).
WIRE_FUZZ_FLAGS = -std=c++17 -g -O1 -fPIC -Isrc -Isrc/wire/vendor \
                  -Imonocypher/src -Imonocypher/src/optional \
                  $(shell pkg-config --cflags Qt6Core)
# FUZZ_FRAMING=1 adds fuzz_wire.cpp's fifth case, which holds the
# generated framed() and required() to their contract. Off by default
# because it finds a situ fault the reader does not reach (situ's inbox,
# 20261026T1249Z): month_body answers differently for the prefix it named.
# On, it is the check that situ's fix works.
ifeq ($(FUZZ_FRAMING),1)
WIRE_FUZZ_FLAGS += -DBBQ_FUZZ_FRAMING
endif
WIRE_FUZZ_SOURCES = src/wire/read.cpp src/wire/format.cpp src/model/series.cpp \
                    src/wire/vendor/situ.c monocypher/src/monocypher.c \
                    monocypher/src/optional/monocypher-ed25519.c

fuzz-wire: test/fuzz_wire.cpp test/fuzz_wire_seed.cpp $(WIRE_FUZZ_SOURCES) \
           $(wildcard src/wire/*.h src/wire/vendor/*.h src/wire/vendor/*.hpp)
	@if grep -nE '(\.|::)(framed|required)\(' src/wire/read.cpp; then \
		echo "fuzz-wire: the reader calls framed() or required(); see" >&2; \
		echo "fuzz-wire:   src/wire/vendor/README.md" >&2; \
		exit 1; \
	fi
	mkdir -p $(FUZZ_DIR)/wire-corpus
	clang++ $(WIRE_FUZZ_FLAGS) -fsanitize=fuzzer,address,undefined \
	        -fno-sanitize-recover=undefined test/fuzz_wire.cpp \
	        $(WIRE_FUZZ_SOURCES) -o $(FUZZ_DIR)/fuzz_wire \
	        $(shell pkg-config --libs Qt6Core)
	clang++ $(WIRE_FUZZ_FLAGS) test/fuzz_wire_seed.cpp src/publish/encode.cpp \
	        src/publish/signing_key.cpp $(WIRE_FUZZ_SOURCES) \
	        -o $(FUZZ_DIR)/fuzz_wire_seed $(shell pkg-config --libs Qt6Core)
	$(FUZZ_DIR)/fuzz_wire_seed $(FUZZ_DIR)/wire-corpus
	cd $(FUZZ_DIR) && ./fuzz_wire -max_total_time=$(FUZZ_SECONDS) \
	        -max_len=65536 -rss_limit_mb=2048 wire-corpus

# The providers' response readers under libFuzzer (project.md sec 2),
# bounded by FUZZ_SECONDS the same way. They parse what the internet sends,
# and the publisher runs them -- holding the signing key -- for stations
# strangers named through the demand list (sec 15.14). Seeded from the JSON
# the reader tests embed, with a dictionary of the fields the readers ask
# for (test/fuzz_providers_seed.py).
# Two of the three files also hold their QObject network clients, which
# need moc to link though the fuzzer never constructs one.
PROVIDERS_MOC = $(shell $(QMAKE) -query QT_HOST_LIBEXECS)/moc
PROVIDERS_FUZZ_SOURCES = src/wu/reader.cpp src/openmeteo/forecast.cpp \
                         src/met/nowcast.cpp src/model/series.cpp

fuzz-providers: test/fuzz_providers.cpp test/fuzz_providers_seed.py \
                $(PROVIDERS_FUZZ_SOURCES) \
                $(wildcard src/wu/*.h src/openmeteo/*.h src/met/*.h src/model/*.h)
	mkdir -p $(FUZZ_DIR)/providers-corpus
	$(PROVIDERS_MOC) src/openmeteo/forecast.h -o $(FUZZ_DIR)/moc_forecast.cpp
	$(PROVIDERS_MOC) src/met/nowcast.h -o $(FUZZ_DIR)/moc_nowcast.cpp
	clang++ -std=c++17 -g -O1 -fPIC -Isrc $(shell pkg-config --cflags Qt6Core Qt6Network) \
	        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	        test/fuzz_providers.cpp $(PROVIDERS_FUZZ_SOURCES) \
	        $(FUZZ_DIR)/moc_forecast.cpp $(FUZZ_DIR)/moc_nowcast.cpp \
	        -o $(FUZZ_DIR)/fuzz_providers $(shell pkg-config --libs Qt6Core Qt6Network)
	python3 -I test/fuzz_providers_seed.py $(FUZZ_DIR)/providers-corpus \
	        $(FUZZ_DIR)/providers.dict
	cd $(FUZZ_DIR) && ./fuzz_providers -max_total_time=$(FUZZ_SECONDS) \
	        -max_len=65536 -rss_limit_mb=2048 -dict=providers.dict providers-corpus

fuzz-serve: test/fuzz_serve.c src/serve/serve_core.c src/serve/serve_core.h
	mkdir -p $(FUZZ_DIR)/corpus
	clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined \
	        -fno-sanitize-recover=undefined -Isrc \
	        test/fuzz_serve.c src/serve/serve_core.c -o $(FUZZ_DIR)/fuzz_serve
	cd $(FUZZ_DIR) && ./fuzz_serve -max_total_time=$(FUZZ_SECONDS) \
	        -max_len=96 -rss_limit_mb=1024 corpus

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently
# on a fresh clone.
# **Where the hooks live is git's question, not the filesystem's.** This
# asked `test -d .git`, which is false in a linked worktree and in a
# submodule checkout: there `.git` is a regular FILE naming the real
# gitdir, and both are git repositories. So `make hooks` refused with
# "not a git repository" inside one that is.
#
# Testing -e instead would only move the failure one line down, because
# the install writes into `.git/hooks/`, which is not a directory there
# either. Asking git answers both halves at once, and from a worktree it
# returns the MAIN repository's hooks directory -- which is the one git
# actually runs.
#
# git's absence is reported as its own thing rather than as "not a git
# repository", which would be a message naming a cause nothing tested.
# Where finished packages land, per the settled inventory.
DEB_DIR ?= $(BUILD_DIR)/deb

# A binary package, built the Debian-native way: dpkg-buildpackage
# driving a dh rules file, which is what every sibling that packages
# uses. It writes into the parent directory, so the artifacts are
# collected from there by name rather than by a glob that could sweep
# somebody else's build.
# dpkg-buildpackage writes its artifacts into the PARENT directory, so
# this collects them. The list comes from the .changes file rather than
# from names spelled here, because the names are dpkg's to choose and it
# has already changed one of them: this rule asked for
# `-dbgsym_....ddeb` and dpkg produced `-dbgsym_....deb`, so every build
# left a debug package sitting outside the project, for ever and in
# silence. Reading the manifest cannot go stale that way.
#
# And it FAILS rather than shrugging, twice: once if a file the manifest
# names is not there, and once if anything matching the package name is
# still in the parent afterwards. A collector is proved by what it
# leaves behind, and the previous one reported success while leaking.
#
# The version is debian/changelog's, read when the recipe runs. It was
# spelled $(VERSION)-1, which held only while the changelog had one entry:
# 0.1.0-2 made the rule look for a .changes file dpkg had not named that
# way and report a successful build as "not produced".
# The server half for Devuan daedalus and Debian bookworm, built in a
# bookworm container because their libraries are older than this
# machine's (project.md sec 15.15). Needs docker; where the account is
# only in the docker group by sg, `sg docker -c 'make deb-daedalus'`.
deb-daedalus:
	tool/build-in-container.sh debian:bookworm-slim daedalus1 pkg.bbq-predictor.server-only

# The server package booted, not run by hand (project.md sec 15.12.5): the
# package in DEB_DIR installed into Devuan excalibur, baked once under
# sysvinit and once under OpenRC, and each image booted in a hull vm whose
# PID 1 is its own init. test/boot/payload.sh then checks what init did --
# the daemon up, dropped and chrooted, answering, cron running -- and
# starts nothing itself. Not part of check: it needs KVM, hull and the
# network, and takes some minutes. `make deb` first. No docker and no root:
# hull builds the image as well as booting it.
#
# hull is a separate program, not vendored, so HULL names the binary:
# found on PATH unless set. Not a path into a sibling checkout, which
# defaulted here until 2026-10-25 -- this tree builds and checks from its
# own files, and somebody working on it has no such checkout.
# Terminates: each bake is under timeout 1200, each boot under timeout
# 300, and a vm ends with its hull run.
HULL ?= hull
BOOT_INITS ?= sysvinit openrc

check-boot:
	@command -v $(HULL) >/dev/null 2>&1 || { \
		echo "check-boot: $(HULL) is not runnable. It needs hull, which" >&2; \
		echo "check-boot:   is not part of this tree: put it on PATH, or" >&2; \
		echo "check-boot:   set HULL= to the binary." >&2; \
		exit 1; }
	@deb=$(DEB_DIR)/bbq-predictor-server_$$(dpkg-parsechangelog -S Version)_$$(dpkg --print-architecture).deb; \
	if [ ! -f "$$deb" ]; then \
		echo "check-boot: no $$deb; run make deb first" >&2; exit 1; \
	fi; \
	mkdir -p $(BUILD_DIR)/boot; \
	for init in $(BOOT_INITS); do \
		echo "check-boot: $$init"; \
		TMPDIR=$(BUILD_DIR)/boot HULL=$(HULL) \
			sh test/boot/bake.sh $$init "$$deb" bbq-boot-$$init || exit 1; \
		timeout 300 $(HULL) run -f test/boot/$$init.conf \
			2>$(BUILD_DIR)/boot/$$init.log || { \
			echo "check-boot: $$init failed; hull said:" >&2; \
			grep -v emulated $(BUILD_DIR)/boot/$$init.log >&2; exit 1; }; \
	done

deb:
	dpkg-buildpackage -b -us -uc
	mkdir -p $(DEB_DIR)
	@arch=$$(dpkg --print-architecture); \
	version=$$(dpkg-parsechangelog -S Version) || exit 1; \
	changes=bbq-predictor_$${version}_$$arch.changes; \
	if [ ! -f "../$$changes" ]; then \
		echo "deb: ../$$changes was not produced" >&2; exit 1; \
	fi; \
	for f in $$(awk '/^Files:/{on=1;next} /^[^ ]/{on=0} on{print $$NF}' \
	                "../$$changes") "$$changes"; do \
		if [ ! -f "../$$f" ]; then \
			echo "deb: ../$$f is in the manifest and not on disk" >&2; \
			exit 1; \
		fi; \
		mv -f "../$$f" $(DEB_DIR)/; \
		echo "deb: $(DEB_DIR)/$$f"; \
	done; \
	left=$$(ls ../bbq-predictor*_$${version}_* 2>/dev/null || true); \
	if [ -n "$$left" ]; then \
		echo "deb: left in the parent directory:" >&2; \
		echo "$$left" >&2; \
		exit 1; \
	fi

hooks:
	@if ! command -v git >/dev/null 2>&1; then \
		echo "hooks: git is not installed, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	dir=$$(git rev-parse --git-common-dir 2>/dev/null); \
	if [ -z "$$dir" ]; then \
		echo "hooks: not a git repository, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	mkdir -p "$$dir/hooks"; \
	install -m 0755 tool/hooks/commit-msg "$$dir/hooks/commit-msg"; \
	echo "hooks: commit-msg installed from tool/hooks/ into $$dir/hooks/"

# The binary alone is not an installation: without a .desktop entry the
# app is invisible to every launcher, which for a tray applet people
# expect to autostart is most of how it gets run.
ifeq ($(SERVER_ONLY),1)
install: install-server
else
install: install-applet install-server
endif

install-applet: $(ARTIFACT)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(ARTIFACT) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 packaging/$(DESKTOP_ID).desktop \
	        $(DESTDIR)$(PREFIX)/share/applications/$(DESKTOP_ID).desktop

	# The icon the desktop entry has been naming since the first commit.
	# Without it the launcher shows a placeholder, which is what it did.
	mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 packaging/$(DESKTOP_ID).svg \
	        $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(DESKTOP_ID).svg

	# The manual page. Named for the PROGRAM rather than the desktop
	# entry: what a person types is $(TARGET), and `man` looks the name
	# up by what was typed.
	mkdir -p $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0644 packaging/$(TARGET).1 \
	        $(DESTDIR)$(PREFIX)/share/man/man1/$(TARGET).1

# The archive server (project.md sec 15.12.2, 15.13): the daemon, so
# sbin and section 8, and the publisher, which a person types to make
# the key, so bin and section 1. The scheduler's wrapper goes under lib,
# because nobody types it.
install-server: $(SERVE_ARTIFACT) $(PUBLISH_ARTIFACT)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	mkdir -p $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)$(PREFIX)/share/man/man8
	install -m 0755 $(SERVE_ARTIFACT) $(DESTDIR)$(PREFIX)/sbin/$(SERVE_TARGET)
	install -m 0755 $(PUBLISH_ARTIFACT) $(DESTDIR)$(PREFIX)/bin/$(PUBLISH_TARGET)
	install -m 0644 packaging/$(PUBLISH_TARGET).1 \
	        $(DESTDIR)$(PREFIX)/share/man/man1/$(PUBLISH_TARGET).1
	install -m 0644 packaging/$(SERVE_TARGET).8 \
	        $(DESTDIR)$(PREFIX)/share/man/man8/$(SERVE_TARGET).8
	mkdir -p $(DESTDIR)$(PREFIX)/lib/$(TARGET)
	install -m 0755 packaging/fetch $(DESTDIR)$(PREFIX)/lib/$(TARGET)/fetch

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(DESKTOP_ID).desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(DESKTOP_ID).svg
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/$(TARGET).1
	rm -f $(DESTDIR)$(PREFIX)/sbin/$(SERVE_TARGET)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PUBLISH_TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/$(PUBLISH_TARGET).1
	rm -f $(DESTDIR)$(PREFIX)/share/man/man8/$(SERVE_TARGET).8
	rm -f $(DESTDIR)$(PREFIX)/lib/$(TARGET)/fetch

# clean removes intermediates only; $(TARGET) survives, so `make install`
# stays possible without a rebuild.
clean:
	rm -f $(TARGET).new $(SERVE_TARGET).new $(PUBLISH_TARGET).new
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi
	@if [ -f $(PUBLISH_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(PUBLISH_BUILD_DIR) clean; fi
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
	$(call bbq_remove_tree,$(BUILD_DIR) $(TEST_BUILD_DIR) build-test-server $(ANDROID_BUILD_DIR) \
	        $(ANDROID_COMPILE_DIR))
	rm -f $(TARGET) $(SERVE_TARGET) $(PUBLISH_TARGET)

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
        style-signals style-man style-palette style-exits style-xml \
        style-wiring style-setup style-tests style-links test-clocks fuzz-serve fuzz-wire \
        fuzz-providers \
        submodules check-publish-libraries check-android-compile \
        android android-aab \
        deb-daedalus check-boot install install-applet install-server uninstall clean veryclean distclean help
