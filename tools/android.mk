# Copied from ~/.claude/tools/android.mk -- the source. Keep in sync;
# fix drift the moment you notice it.
# =============================================================================
# android.mk -- the shared Android vocabulary for private projects
#
# Spread verbatim into each project's tools/ from ~/.claude/tools/android.mk,
# the same model as style_gate.py and for the same reason: a copy in the
# repository is reachable by CI, which a file under ~/.claude is not.
#
# harmonization.md settled the names. This file is where they live, so that
# a habit learned in one project is correct in the next:
#
#   make android            debug build, installable on any device
#   make android-aab        the Play bundle; needs a keystore
#   make android-install    install the artifact on the attached device
#   make android-run        install and launch it
#   make android-log        follow this app's log, and only this app's
#   make android-uninstall  remove it
#   make android-check      everything the build needs, checked by name
#
# `apk` is deliberately NOT a target name. netcfgd's `apk` is Alpine's
# packaging command, and one word meaning two things across sibling trees is
# how somebody eventually runs the wrong one.
#
#
# INCLUDE THIS AFTER YOUR DEFAULT GOAL, OR SET ONE.
#
# `include` is where make first sees a target, so pulling this fragment in
# ahead of a project's `all` makes android-check the default goal: a plain
# `make` then runs the preflight, fails for want of QT_ANDROID_ROOT, and
# builds nothing. bbq-predictor did this and did not notice for four
# sessions. Either include it below `all`, or say so explicitly:
#
#   .DEFAULT_GOAL := all
#
# The fragment cannot fix this for you -- it has no way to know what your
# default goal is meant to be.
#
# WHAT THE PROJECT SUPPLIES
#   APP_ID              reverse-DNS id; the package and the launcher use it
#   VERSION             the one place a version is stated
#   ANDROID_BUILD_DIR   where the Qt build tree goes
#   android-build       a rule that produces the APK, however this project
#                       builds; qmake and CMake differ and neither belongs here
#
# WHAT THIS FILE SUPPLIES
#   The variable names, the preflight, the versionCode, the adb plumbing,
#   and the signature check that beerssh learned the hard way.
# =============================================================================

ANDROID_SDK_ROOT ?= $(HOME)/Android/Sdk
ANDROID_API      ?= 26

# 36, and the floor is Qt's rather than this project's.
#
# It was 33, and Qt 6.12 will not build against it. Qt pulls androidx.core
# transitively, and each release of that library raises the compileSdk it
# demands of anything depending on it: 1.16 wanted 35, 1.17 wants 36. Gradle
# refuses in AAR-metadata terms rather than in SDK terms --
#
#     3 issues were found when checking AAR metadata:
#     1.  Dependency 'androidx.core:core:1.17.0' requires libraries and
#         applications that depend on it to compile against version 36 or
#         later of the Android APIs.
#
# -- which names a library nobody wrote down and never mentions this
# variable, so the connection back to here has to be made from memory.
#
# NOT a check, deliberately. Which androidx versions a given Qt release
# resolves is not something a Makefile can know without asking Gradle, and a
# guess would go stale silently -- worse than the real error, which at least
# names the version it wants. The number is raised when a build says so.
ANDROID_TARGET_API ?= 36

# EXPORTED, not merely set, and this is load-bearing.
#
# androiddeployqt reads the SDK location out of the deployment-settings JSON,
# which qmake writes from the ENVIRONMENT rather than from any make variable.
# Leave these unexported and qmake falls back to whatever path was baked into
# the Qt installation -- /opt/android/sdk on the machine this was found on --
# so the build compiles every source, links the shared object, and only then
# fails at packaging with "Directory /opt/android/sdk/platforms does not
# exist": a path nobody configured, named by nothing the project can see.
export ANDROID_SDK_ROOT
export ANDROID_NDK_ROOT

# Which platform Gradle compiles against, named rather than guessed.
#
# androiddeployqt's default is "the highest available", and its idea of
# highest is wrong when the SDK holds an extension platform: with android-36
# installed beside android-33-ext5 it chose the latter, and Gradle then
# refused the build because Qt's own AndroidX dependencies require at least
# 34. The failure names neither the platform it picked nor where it picked it
# from, so naming it here removes a heuristic that is demonstrably wrong on a
# perfectly ordinary SDK.
ANDROID_PLATFORM ?= android-$(ANDROID_TARGET_API)

# The host-side deploy tool, asked for rather than assumed. Qt puts it with
# the HOST tools, not in the Android kit, and the directory is named
# differently per platform -- qmake knows where it is.
ifdef QT_ANDROID_ROOT
ANDROID_HOST_BINS := $(shell $(QT_ANDROID_ROOT)/bin/qmake -query QT_HOST_BINS 2>/dev/null)
endif

ANDROID_DEPLOY_QT ?= $(ANDROID_HOST_BINS)/androiddeployqt

# The JDK, resolved and EXPORTED rather than left to Gradle to find.
#
# Gradle picks its own toolchain and will happily choose a JRE: on the
# machine this was written for it took java-21-openjdk, which ships no
# compiler, and failed with "does not provide the required capabilities:
# [JAVA_COMPILER]" while a perfectly good JDK 17 sat on PATH. The preflight
# had passed, honestly and uselessly -- it checked one JVM and the build used
# another, which is a check verifying something other than what it protects.
#
# Resolving JAVA_HOME from javac and exporting it makes the two the same JVM,
# so the preflight now guarantees the compiler Gradle actually runs.
ifeq ($(origin JAVA_HOME), undefined)
JAVA_HOME := $(patsubst %/bin/javac,%,$(realpath $(shell command -v javac 2>/dev/null)))
endif

export JAVA_HOME

ANDROID_ADB       = $(ANDROID_SDK_ROOT)/platform-tools/adb

# The ABI is the Qt KIT's, read from it rather than chosen a second time.
#
# beerssh found what happens otherwise: building against the x86_64 kit with
# ANDROID_ABI left at arm64-v8a produced a package named for an ABI it did
# not contain, which installs on nothing it claims to target and cannot be
# told apart by looking at the file.
ifdef QT_ANDROID_ROOT
ANDROID_KIT_ABI := $(shell sed -n 's/^DEFAULT_ANDROID_ABIS *= *//p' \
                            $(QT_ANDROID_ROOT)/mkspecs/qdevice.pri 2>/dev/null)

ifeq ($(ANDROID_KIT_ABI),)
ANDROID_KIT_ABI := $(patsubst android_%,%,$(notdir $(patsubst %/,%,$(QT_ANDROID_ROOT))))
ANDROID_KIT_ABI := $(subst arm64_v8a,arm64-v8a,$(ANDROID_KIT_ABI))
ANDROID_KIT_ABI := $(subst armv7,armeabi-v7a,$(ANDROID_KIT_ABI))
ANDROID_KIT_ABI := $(filter arm64-v8a armeabi-v7a x86 x86_64,$(ANDROID_KIT_ABI))
endif
endif

ANDROID_ABI ?= $(ANDROID_KIT_ABI)

# Play refuses an upload whose versionCode does not exceed the last it took,
# so a hardcoded number allows exactly one upload and blocks every update
# after it. Derived as major*10000 + minor*100 + patch: 0.1.0 is 100.
#
# Override to re-upload the SAME version after a rejected release, which is
# the case that leaves you needing it.
ANDROID_VERSION_CODE ?= $(shell echo $(VERSION) | awk -F. \
        '{printf "%d", ($$1 * 10000) + ($$2 * 100) + $$3}')

# Signing comes from the environment and never from the tree.
#
# A keystore committed anywhere is a keystore that has to be replaced, and
# the upload key is the one thing a store account cannot regenerate for you.
ANDROID_KEYSTORE  ?=
ANDROID_KEY_ALIAS ?=

android-check:
	@if [ -z "$(QT_ANDROID_ROOT)" ]; then \
		echo "android: QT_ANDROID_ROOT is not set." >&2; \
		echo "android:   point it at a Qt-for-Android kit, e.g." >&2; \
		echo "android:   make android QT_ANDROID_ROOT=\$$HOME/Qt/<version>/android_arm64_v8a" >&2; \
		exit 1; \
	fi
	@if [ -z "$(ANDROID_ABI)" ]; then \
		echo "android: cannot tell which ABI this kit builds." >&2; \
		echo "android:   neither $(QT_ANDROID_ROOT)/mkspecs/qdevice.pri nor the" >&2; \
		echo "android:   kit's directory name says. Name it yourself:" >&2; \
		echo "android:   make android ANDROID_ABI=arm64-v8a" >&2; \
		exit 1; \
	fi
	@if [ -n "$(ANDROID_KIT_ABI)" ] && [ "$(ANDROID_ABI)" != "$(ANDROID_KIT_ABI)" ]; then \
		echo "android: ANDROID_ABI is $(ANDROID_ABI), the kit builds $(ANDROID_KIT_ABI)." >&2; \
		echo "android:   qmake follows the kit, so building on would name the" >&2; \
		echo "android:   artifact after an ABI it does not contain." >&2; \
		exit 1; \
	fi
	@if [ -z "$(ANDROID_NDK_ROOT)" ] || [ ! -d "$(ANDROID_NDK_ROOT)" ]; then \
		echo "android: ANDROID_NDK_ROOT is not set or does not exist." >&2; \
		echo "android:   the kit above was built against a particular NDK; a" >&2; \
		echo "android:   different one may fail in ways that name neither." >&2; \
		exit 1; \
	fi
	@if [ ! -d "$(ANDROID_SDK_ROOT)/platform-tools" ]; then \
		echo "android: no Android SDK at $(ANDROID_SDK_ROOT)." >&2; \
		echo "android:   the SDK is SEPARATE from the NDK -- Gradle needs" >&2; \
		echo "android:   platform-tools, a platform and build-tools." >&2; \
		exit 1; \
	fi
	@if [ -z "$(JAVA_HOME)" ] && ! command -v javac >/dev/null 2>&1; then \
		echo "android: no javac on PATH and JAVA_HOME is unset." >&2; \
		echo "android:   Gradle needs a JDK; a JRE alone will not do, and it" >&2; \
		echo "android:   says so only as a toolchain capability error." >&2; \
		exit 1; \
	fi
	@if [ ! -x "$(JAVA_HOME)/bin/javac" ]; then \
		echo "android: JAVA_HOME=$(JAVA_HOME) has no bin/javac." >&2; \
		echo "android:   That is a JRE. Gradle reports it as a toolchain" >&2; \
		echo "android:   lacking JAVA_COMPILER, naming the JVM but not the" >&2; \
		echo "android:   reason." >&2; \
		exit 1; \
	fi
	@kit_ndk=$$(sed -n 's/.*android-ndk-r\([0-9]*\).*/\1/p' \
	                 "$(QT_ANDROID_ROOT)/mkspecs/qdevice.pri" 2>/dev/null | head -1); \
	used_ndk=$$(sed -n 's/^Pkg.Revision *= *\([0-9]*\).*/\1/p' \
	                 "$(ANDROID_NDK_ROOT)/source.properties" 2>/dev/null | head -1); \
	if [ -n "$$kit_ndk" ] && [ -n "$$used_ndk" ] && [ "$$kit_ndk" != "$$used_ndk" ]; then \
		echo "android: NDK $$used_ndk, but this Qt kit was built against r$$kit_ndk." >&2; \
		echo "android:   That mismatch is not a build error. It compiles, links," >&2; \
		echo "android:   packages, signs and installs, and then dlopen refuses the" >&2; \
		echo "android:   Qt libraries on the device for a missing libc++ symbol." >&2; \
		echo "android:   Install the matching NDK, or set ANDROID_NDK_MISMATCH_OK=1" >&2; \
		echo "android:   if you have a reason." >&2; \
		[ -n "$(ANDROID_NDK_MISMATCH_OK)" ] || exit 1; \
	fi
	@echo "android:   jdk $(JAVA_HOME)"
	@echo "android: kit $(QT_ANDROID_ROOT)"
	@echo "android:   abi $(ANDROID_ABI), versionCode $(ANDROID_VERSION_CODE)"

# Whoever signed it, read from the artifact rather than announced.
#
# The pattern has to cope with more than one apksigner output format. It read
# only "Signer #1 certificate DN:", and build-tools 37 prints "V2 Signer:
# certificate DN:" instead -- so the check matched nothing, announced "signed
# by " with an empty name, and the debug-key guard below could never fire. The
# check written to stop a debug-signed release had been quietly disabled by a
# tool update, which is the same failure one layer up. An unreadable signature
# is now an error rather than a blank.
#
# beerssh shipped a "release build" signed with the Android debug key, and
# the only reason anybody noticed is that somebody ran apksigner by hand:
# flags passed to Qt's GENERATED Makefile are dropped silently, so the build
# reports success and produces a debug-signed package under a message saying
# otherwise. Signed with the debug key is the one packaging mistake that
# cannot be caught by looking at the file.
define android_verify_signature
	@signer=$$(ls $(ANDROID_SDK_ROOT)/build-tools/*/apksigner 2>/dev/null | tail -1); \
	if [ -z "$$signer" ]; then \
		echo "android: NO apksigner in the SDK -- who signed this is unchecked" >&2; \
		exit 0; \
	fi; \
	dn=$$($$signer verify --print-certs "$(1)" 2>/dev/null \
	      | sed -n -e 's/^Signer #1 certificate DN: //p' \
	               -e 's/^V[0-9]* Signer: certificate DN: //p' \
	      | head -1); \
	if [ -z "$$dn" ]; then \
		echo "android: cannot read the signature of $(1)." >&2; \
		echo "android:   apksigner ran and printed nothing this recognises," >&2; \
		echo "android:   so the signer is UNKNOWN. That must not read as a" >&2; \
		echo "android:   pass: it is how a debug-signed release ships." >&2; \
		exit 1; \
	fi; \
	echo "android: signed by $$dn"; \
	if [ -n "$(ANDROID_KEYSTORE)" ] && echo "$$dn" | grep -q "Android Debug"; then \
		echo "android: a keystore was given but the artifact is DEBUG-signed" >&2; \
		exit 1; \
	fi
endef

android-install: android
	$(ANDROID_ADB) install -r $(ANDROID_ARTIFACT)

android-run: android-install
	$(ANDROID_ADB) shell am start -n \
	        $(APP_ID)/org.qtproject.qt.android.bindings.QtActivity

# This app's log and nothing else. `adb logcat` unfiltered is every process
# on the device, which is how a real message gets lost rather than read.
android-log:
	@pid=$$($(ANDROID_ADB) shell pidof -s $(APP_ID) 2>/dev/null); \
	if [ -z "$$pid" ]; then \
		echo "android-log: $(APP_ID) is not running on the device" >&2; \
		exit 1; \
	fi; \
	$(ANDROID_ADB) logcat --pid=$$pid

android-uninstall:
	$(ANDROID_ADB) uninstall $(APP_ID)

.PHONY: android-check android-install android-run android-log android-uninstall
