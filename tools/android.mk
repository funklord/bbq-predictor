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
ANDROID_TARGET_API ?= 33

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
		echo "android:   make android QT_ANDROID_ROOT=\$$HOME/Qt/6.10.0/android_arm64_v8a" >&2; \
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
	@if ! command -v javac >/dev/null 2>&1 && [ -z "$(JAVA_HOME)" ]; then \
		echo "android: no javac on PATH and JAVA_HOME is unset." >&2; \
		echo "android:   Gradle needs a JDK; a JRE alone will not do, and it" >&2; \
		echo "android:   says so only as a toolchain capability error." >&2; \
		exit 1; \
	fi
	@echo "android: kit $(QT_ANDROID_ROOT)"
	@echo "android:   abi $(ANDROID_ABI), versionCode $(ANDROID_VERSION_CODE)"

# Whoever signed it, read from the artifact rather than announced.
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
		echo "android: no apksigner in the SDK; cannot say who signed this"; \
	else \
		dn=$$($$signer verify --print-certs "$(1)" 2>/dev/null \
		      | sed -n 's/^Signer #1 certificate DN: //p'); \
		echo "android: signed by $$dn"; \
		if [ -n "$(ANDROID_KEYSTORE)" ] && echo "$$dn" | grep -q "Android Debug"; then \
			echo "android: a keystore was given but the artifact is DEBUG-signed" >&2; \
			exit 1; \
		fi; \
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
