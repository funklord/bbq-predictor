# Copied from ~/.claude/tool/android.mk -- the source. Keep in sync;
# fix drift the moment you notice it.
# =============================================================================
# android.mk -- the shared Android vocabulary for private projects
#
# Spread verbatim into each project's tool/ from ~/.claude/tool/android.mk,
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
#   ANDROID_ARTIFACT    the path the build rule leaves the APK at
#   android-build       a rule that produces the APK, however this project
#                       builds; qmake and CMake differ and neither belongs here
#
# WHAT THIS FILE SUPPLIES
#   The variable names, the preflight, the versionCode, the adb plumbing,
#   and the signature check that beerssh learned the hard way.
# =============================================================================

ANDROID_SDK_ROOT ?= $(HOME)/Android/Sdk

# The OLDEST Android the app runs on -- its minSdk, not its target.
#
# It does not set minSdk; Qt does, and this must be kept equal to what Qt
# declares. Read it back from a built package rather than trusting either
# side:
#
#     aapt2 dump badging <apk> | grep minSdkVersion
#
# EXPORTED, because tool/build-openssl-android.sh reads it and gets the
# direction of the risk wrong if it disagrees. A dependency cross-compiled
# for a HIGHER API than the app resolves symbols at build time that are
# absent at run time on the oldest device the app claims to support -- so
# the failure lands on somebody else's old phone, not on the desk. That
# script defaulted to 28 while the package declared 26, and said in a
# comment that the two matched.
#
# 26 is a FLOOR BENEATH EVERY ADOPTER, which is the only thing it can
# safely be. It used to say "26 is Qt's own floor: the kit's generated
# gradle.properties says qtMinSdkVersion=26", and that cited the wrong
# number for the wrong reason.
#
# ANDROID_API and qtMinSdkVersion are different quantities. This one is
# the NDK level DEPENDENCIES are cross-compiled against; qtMinSdkVersion
# is what the APP declares, which Qt writes per build. They do not track
# each other: measured 2026-09-01, hydra sets nothing here, takes this
# default of 26, and ships an app declaring 28.
#
# Nor is qtMinSdkVersion a property of the kit. The same Qt 6.12.0
# generates 26 for beerssh and bbq-predictor and 28 for hydra and
# fuzzypickles. So "track what Qt declares" names no single number, and
# raising this default to 28 would cross-compile dependencies at 28 for
# two trees whose apps declare 26 -- exactly the failure the paragraph
# above describes, introduced by the fix for it.
#
# So the default belongs at or below the LOWEST minimum any adopter
# declares, and it is checked against the artifact rather than against a
# comment: `aapt2 dump badging <apk> | grep minSdkVersion`. RAISE IT
# PROJECT-SIDE, above the include,
# when this project's own code needs a later libc -- `ANDROID_API = 28`
# and the ?= below yields. bionic marks getrandom __INTRODUCED_IN(28), so
# a tree calling it compiled against 26 fails inside its own entropy
# source, naming neither this variable nor the floor it wanted; that cost
# one adopter a debugging session on a clean rebuild. If a compile fails
# on a libc symbol that exists on your desktop, this is the knob.
ANDROID_API      ?= 26
export ANDROID_API

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

# The NDK is found, not asked for, matching what the help in every adopter
# already promises. sdkmanager installs NDKs under $(ANDROID_SDK_ROOT)/ndk/
# one directory per version, so the newest by version sort is the default
# and an explicit ANDROID_NDK_ROOT still wins. Before this, the variable
# was exported and never defaulted: android-check failed on a machine with
# the NDK in the standard place, in all four adopters at once, and only a
# session that happened to carry the variable in its environment could
# build -- which reads as "works for whoever set it up" and is exactly the
# per-machine friction the fragment exists to remove.
ANDROID_NDK_ROOT ?= $(lastword $(sort $(wildcard $(ANDROID_SDK_ROOT)/ndk/*)))
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

# Find the kit if nobody named one. hydra's, taken here because it removes
# the single most tedious thing about this build.
#
# Without it every android target needs QT_ANDROID_ROOT spelled out on the
# command line, which is a path with a version number in it that changes
# under you at every Qt update. hydra's default named 6.11.1 -- a version
# that machine did not have -- so the default was one that had to be
# overridden every time, which is no default at all.
#
# ANDROID_ABI is the input here and the kit is the answer, which is the
# reverse of the derivation below. The two do not fight: this runs only when
# QT_ANDROID_ROOT is unset, and whatever it finds is then read back by the
# ABI derivation and cross-checked by android-check, so a kit found here
# that disagrees with an explicit ANDROID_ABI is still refused by name.
#
# `sort -V`, because a lexical sort puts 6.3.2 above 6.10.0 and would pick
# the older kit for as long as nobody noticed. `ifndef` rather than `?=` so
# the search runs once instead of at every reference.
QT_ROOT ?= $(HOME)/Qt
QT_KIT_arm64-v8a   = android_arm64_v8a
QT_KIT_armeabi-v7a = android_armv7
QT_KIT_x86_64      = android_x86_64
QT_KIT_x86         = android_x86

ifndef QT_ANDROID_ROOT
ifneq ($(ANDROID_ABI),)
QT_ANDROID_ROOT := $(shell ls -d \
        $(QT_ROOT)/*/$(QT_KIT_$(ANDROID_ABI)) 2>/dev/null | sort -V | tail -1)
else
# No ABI named either, so prefer arm64-v8a over whatever sorts last.
#
# Sorting the kit names and taking the last gives android_x86_64, which is
# an emulator build. That installs on no phone anybody owns, and it is the
# worst kind of wrong default: everything succeeds, the APK is real, and it
# is discovered by `adb install` refusing it. Every physical device this
# would be built for is arm64.
QT_ANDROID_ROOT := $(shell \
        ls -d $(QT_ROOT)/*/android_arm64_v8a 2>/dev/null | sort -V | tail -1 \
        || true)
ifeq ($(QT_ANDROID_ROOT),)
QT_ANDROID_ROOT := $(shell ls -d $(QT_ROOT)/*/android_* 2>/dev/null \
                            | sort -V | tail -1)
endif
endif
endif

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

# A project's own preflight runs as part of this one, not instead of it.
#
# Every adopter has something to check that is nobody else's business:
# beerssh needs libssh built for the ABI, fuzzypickles its vendored archives,
# hydra its kit selection. Left without somewhere to put those, a project
# redefines android-check -- and make takes the last definition it read,
# silently, so the shared preflight stops running the moment a project adds
# a check of its own. That is the worst possible failure for a preflight:
# the checks that were paid for in broken builds are the ones that vanish.
#
# So name a target and this one will depend on it:
#
#   ANDROID_CHECK_LOCAL = android-check-beerssh
#   android-check-beerssh:
#           @test -f deps/... || { echo "..." >&2; exit 1; }
#
# Unset, nothing extra runs and nothing is reported -- a project with no
# local preflight is not missing one.
# Set it wherever you like, including below this include. Make expands an
# explicit rule's prerequisites AS IT READS THE RULE, so a plain
# `android-check: $(ANDROID_CHECK_LOCAL)` would see the variable's value at
# this point in the file and nowhere else -- a project setting it further
# down would get an android-check with no local prerequisite, running the
# shared checks, printing nothing about the missing one and passing. That is
# the same silent retirement this hook exists to prevent, arriving by a
# different door. Measured on a fixture before it was written this way.
#
# It runs LAST, after the shared checks, and that ordering is the point.
#
# It was a prerequisite first -- `android-check: $$(ANDROID_CHECK_LOCAL)` --
# which make satisfies BEFORE the recipe, so the project's check went first
# and a failure in it meant the shared checks never ran. That is backwards,
# because a local check is almost always downstream of the shared ones.
# Measured in beerssh with `make android-check QT_ANDROID_ROOT=`: it reported
# "dependencies for  are not built" with an empty ABI and told the reader to
# run a script with no argument. The ABI was empty BECAUSE no kit was named,
# which is the shared check's message -- so the local check pre-empted the
# check that would have explained it, and offered its own symptom as the
# diagnosis.
#
# Invoked with $(MAKE) at the end of the recipe rather than ordered among
# prerequisites, because prerequisite order is not guaranteed under `make
# -j`. A recursive call is sequenced by construction.
ANDROID_CHECK_LOCAL ?=

android-check:
	@# ANDROID_API must not exceed what the app declares.
	@#
	@# The invariant the comment on ANDROID_API states, checked rather
	@# than asserted -- it was written 2026-09-01 with nothing enforcing
	@# it, and an adopter whose app drops below the default would break
	@# it silently. Dependencies cross-compiled ABOVE the app's declared
	@# minimum resolve symbols that are absent on the oldest device the
	@# app claims to support, so the failure lands on somebody else's
	@# phone. Below is the safe side and equal is fine.
	@#
	@# It reads the generated gradle.properties, which is the file the
	@# old comment cited as ANDROID_API's own justification -- it is not
	@# that, but it IS where the app's declared minimum can be read
	@# without a device or aapt2.
	@#
	@# **This cannot check a first build**, because that file does not
	@# exist until Qt has generated it, and the check passes quietly when
	@# it is absent. Said out loud rather than left as a surprise: a
	@# green android-check on a clean tree has not verified this.
	@decl="$(ANDROID_BUILD_DIR)/android-build/gradle.properties"; \
	if [ -f "$$decl" ]; then \
		want=`sed -n 's/^qtMinSdkVersion=\([0-9][0-9]*\)$$/\1/p' "$$decl"`; \
		if [ -n "$$want" ] && [ "$(ANDROID_API)" -gt "$$want" ]; then \
			echo "android: ANDROID_API is $(ANDROID_API), the app declares minSdk $$want." >&2; \
			echo "android:   Dependencies would be cross-compiled against a later" >&2; \
			echo "android:   libc than the oldest device this app admits to, and" >&2; \
			echo "android:   the failure lands there rather than here." >&2; \
			echo "android:   Lower ANDROID_API, or raise what the app declares." >&2; \
			exit 1; \
		fi; \
	fi
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
	@# The kit's own library name, which is the ABI the LINKER will use.
	@#
	@# hydra's, and it is better evidence than either source above:
	@# qdevice.pri is a text file describing intent and the directory name is
	@# Qt's to change, while libQt6Core_<abi>.so is the artifact that gets
	@# linked. `qmake -query QT_ARCH` was the obvious source and was tried
	@# first -- all four kits on the machine this was written for answer
	@# `**Unknown**`, so it cannot tell them apart at all.
	@#
	@# A kit that will not say what it is FAILS here rather than passing. An
	@# unconfirmed ABI is the state this check exists to refuse, and the cost
	@# of guessing is a package named after an architecture it does not
	@# contain -- which installs on nothing and cannot be told by looking.
	@core=$$(ls $(QT_ANDROID_ROOT)/lib/libQt6Core_*.so 2>/dev/null | head -1); \
	if [ -z "$$core" ]; then \
		echo "android: $(QT_ANDROID_ROOT) ships no libQt6Core_<abi>.so," >&2; \
		echo "android:   so its ABI cannot be confirmed; refusing to guess." >&2; \
		exit 1; \
	fi; \
	kit=$${core##*/libQt6Core_}; kit=$${kit%.so}; \
	if [ "$$kit" != "$(ANDROID_ABI)" ]; then \
		echo "android: ABI mismatch. ANDROID_ABI is $(ANDROID_ABI), but that" >&2; \
		echo "android:   kit links $$kit -- read from its own libQt6Core." >&2; \
		echo "android:   kit: $(QT_ANDROID_ROOT)" >&2; \
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
	@# Last, so the shared checks above have already explained anything they
	@# would have explained. Guarded on the variable being set, since a bare
	@# `$(MAKE)` with no target would run the default goal.
	$(if $(ANDROID_CHECK_LOCAL),@$(MAKE) --no-print-directory $(ANDROID_CHECK_LOCAL))

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
