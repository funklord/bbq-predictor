#!/bin/sh
# Cross-compile OpenSSL for Android (project.md sec 11.5).
#
# Qt for Android does not ship OpenSSL, and without it QSslSocket cannot
# start: the applet on the phone reports "TLS initialization failed" and
# Qt logs "qt.tlsbackend.ossl: Failed to load libssl/libcrypto". Every
# provider this program reads is HTTPS, so that is the whole of it.
#
# Usage:
#   tools/build-openssl-android.sh [abi]        # default arm64-v8a
#
# Produces deps/android/<abi>/lib/{libcrypto_3.so,libssl_3.so}, which
# bbq-predictor.pro adds to ANDROID_EXTRA_LIBS.
#
# THE SOURCE COMES FROM APT, not from a URL in this script.
#
# `apt-get source openssl` gives a version the distribution pinned and
# vetted, with a signature and a checksum apt verifies before we see it.
# A tarball fetched from upstream by a line in a shell script would put
# that trust decision here instead, where nobody reviews it. Debian is
# already the source of everything else on this machine.
#
# What it does NOT use is the system's libssl. That is x86-64 and linked
# against glibc; the phone is aarch64 and links against bionic, so the
# binary is wrong twice over. Only the source is portable.
set -e

abi=${1:-arm64-v8a}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)

case "$abi" in
	arm64-v8a)   target=android-arm64 ;;
	armeabi-v7a) target=android-arm ;;
	x86_64)      target=android-x86_64 ;;
	x86)         target=android-x86 ;;
	*)
		echo "unknown ABI: $abi" >&2
		echo "expected one of: arm64-v8a armeabi-v7a x86_64 x86" >&2
		exit 1
		;;
esac

if [ -z "$ANDROID_NDK_ROOT" ] || [ ! -d "$ANDROID_NDK_ROOT" ]; then
	echo "ANDROID_NDK_ROOT is not set or does not exist." >&2
	echo "  It must be the NDK the Qt kit names in mkspecs/qdevice.pri;" >&2
	echo "  make android-check compares them." >&2
	exit 1
fi

for tool in patchelf perl make; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "$tool is not installed." >&2
		exit 1
	}
done

# The API level the app is built for, which is its minSdk.
#
# Building OpenSSL for a HIGHER level than the app resolves symbols at
# build time that are absent at run time -- and the device that finds out
# is the oldest one a user has, not this one.
#
# It said 28, and claimed in this comment that 28 was "Qt's own default
# and the minSdk this project declares". The package declares 26:
#
#     aapt2 dump badging bbq-predictor-0.1.0-arm64-v8a.apk
#     minSdkVersion:'26'
#     targetSdkVersion:'36'
#
# So the number was two levels too high, in the direction that breaks on
# somebody else's phone, and the comment asserting otherwise is what kept
# anyone from checking. tools/android.mk exports ANDROID_API so a build
# driven from there agrees by construction; this default is for a run by
# hand, and it matches.
api=${ANDROID_API:-26}

prefix="$root/deps/android/$abi"
work="$root/deps/build/$abi"

mkdir -p "$prefix"

# An already-unpacked tree may be given instead of fetching one. Useful
# offline, and for building exactly what somebody has to hand.
if [ -n "$OPENSSL_SRC" ]; then
	echo "openssl: using the source at $OPENSSL_SRC"
	src="$OPENSSL_SRC"
else
	echo "openssl: fetching source through apt"

	# Cleared only on the path that fills it. Wiping it unconditionally
	# destroyed a source tree handed in through OPENSSL_SRC that happened
	# to live here -- the script deleting its own input, which is what a
	# computed path in an rm is for.
	rm -rf "$work"
	mkdir -p "$work"

	(
		cd "$work"
		# --download-only would leave it packed; this unpacks and applies
		# the distribution's patches, which is the point of using apt.
		apt-get source openssl
	)

	src=$(find "$work" -maxdepth 1 -type d -name 'openssl-*' -print -quit)
fi
if [ -z "$src" ]; then
	echo "openssl: apt-get source produced no source directory." >&2
	echo "  Check that a deb-src line is configured in /etc/apt/sources.list." >&2
	exit 1
fi

# Refuse OpenSSL 1.x outright.
#
# Qt's old Tools/OpenSSL on this machine carries 1.1.1q, dated July 2022
# and end-of-life since September 2023. It is sitting right there, it
# compiles, and using it would put a TLS library with years of unpatched
# advisories inside an app whose whole purpose is fetching over HTTPS.
# A convenient source is not the same as a safe one, and this is the one
# place in the project where the difference is measured in other people's
# security rather than in a wrong graph.
# Two places, because the two generations state it differently and the
# check has to work on both to be able to refuse one of them. OpenSSL 3
# keeps VERSION.dat at the top level and GENERATES opensslv.h during
# configure -- reading only the header found nothing in a 3.5.6 tree and
# the guard failed open, which for a security check is the wrong
# direction to fail in.
if [ -f "$src/VERSION.dat" ]; then
	version=$(awk -F= '
		$1 == "MAJOR" { major = $2 }
		$1 == "MINOR" { minor = $2 }
		$1 == "PATCH" { patch = $2 }
		END { if (major != "") print major "." minor "." patch }
	' "$src/VERSION.dat")
else
	version=$(sed -n 's/.*OPENSSL_VERSION_TEXT *"OpenSSL \([0-9.a-z]*\).*/\1/p' \
	                 "$src/include/openssl/opensslv.h" 2>/dev/null | head -1)
fi

case "$version" in
	3.*)
		;;
	"")
		echo "openssl: cannot read a version out of $src" >&2
		exit 1
		;;
	*)
		echo "openssl: $src is OpenSSL $version, and this wants 3.x." >&2
		echo "  1.1.1 went end-of-life in September 2023. Building it would" >&2
		echo "  ship known-unpatched TLS in an app that speaks only HTTPS." >&2
		exit 1
		;;
esac

echo "openssl: building $version for $abi, API $api"

host_tag=linux-x86_64
toolchain="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$host_tag/bin"
if [ ! -d "$toolchain" ]; then
	echo "openssl: no toolchain at $toolchain" >&2
	exit 1
fi

PATH="$toolchain:$PATH"
export PATH ANDROID_NDK_ROOT

# THE NAMES MUST BE libcrypto_3.so AND libssl_3.so, not libcrypto.so.
#
# Qt's OpenSSL backend on Android dlopens the libraries BY NAME, and the
# name it asks for is built in qsslsocket_openssl_symbols.cpp as
#
#     "ssl" + ANDROID_OPENSSL_SUFFIX, defaulting to "_" QT_OPENSSL_VERSION
#
# which for OpenSSL 3.x is "_3". So it asks the Android linker for
# libssl_3.so, the linker looks in the application's own library
# directory, and unsuffixed libraries are simply not what it asked for.
#
# Qt does have a fallback that globs for "libssl.*" and would match an
# unsuffixed name -- an earlier version of this script relied on it and
# said so in a comment. It cannot work here: that fallback scans
# LD_LIBRARY_PATH plus /lib, /usr/lib and /system/lib, and an Android
# app process has LD_LIBRARY_PATH unset (measured) while its libraries
# live in /data/app/<package>-<hash>/lib/<abi>, which is on the LINKER's
# search path but on no directory list Qt scans. Both of Qt's routes
# therefore missed, the backend's isValid() returned false, and
# QSslSocket::availableBackends() filtered it out -- reporting
# "cert-only" with the plugin loaded, constructed and present.
#
# The suffix is applied through OpenSSL's own shared_extension rather
# than by renaming afterwards. Renaming would leave libssl_3.so with a
# DT_NEEDED on libcrypto.so and a stale SONAME, and patchelf is not the
# answer either: it corrupted the version-needed table of these very
# libraries when it was tried, producing a file that loads on the desk
# and not on the phone.
cat > "$src/Configurations/50-android-qt.conf" <<'EOF'
## Generated by tools/build-openssl-android.sh -- see the note there.
## Qt for Android dlopens libssl_3.so and libcrypto_3.so by name, so the
## suffix has to be part of the build rather than applied afterwards:
## this way the SONAME and the inter-library DT_NEEDED agree with the
## filenames.
##
## The name is qt-android-<arch> and NOT android-<arch>-qt, which is not
## a style choice. Configurations/15-android.conf derives the toolchain
## triple from the LAST dash-separated component of the target name:
##
##     $config{target} =~ m|[^-]+-([^-]+)$|;
##
## so a target ending in "-qt" resolves its architecture to "qt", finds
## no triplet for it, and builds a CROSS_COMPILE of "" -- which fails
## much later as "28-clang: not found", naming neither the target nor
## the architecture.
my %targets = (
    "qt-android-arm64" => {
        inherit_from     => [ "android-arm64" ],
        shared_extension => "_3.so",
    },
    "qt-android-arm" => {
        inherit_from     => [ "android-arm" ],
        shared_extension => "_3.so",
    },
    "qt-android-x86_64" => {
        inherit_from     => [ "android-x86_64" ],
        shared_extension => "_3.so",
    },
    "qt-android-x86" => {
        inherit_from     => [ "android-x86" ],
        shared_extension => "_3.so",
    },
);
EOF

# An unsuffixed library left in the source tree by an earlier run must go
# before the link, and this is not tidying.
#
# libssl is linked with -lcrypto, so the linker takes whichever libcrypto
# it finds in the tree and records THAT file's SONAME as the dependency.
# With a stale libcrypto.so sitting beside the new libcrypto_3.so, the
# freshly built libssl_3.so came out needing libcrypto.so -- a library
# the package does not carry, under a name nothing would provide. Named
# rather than globbed, because these two files are known and a wildcard
# in a source tree is how something else gets deleted.
rm -f "$src/libcrypto.so" "$src/libssl.so"

(
	cd "$src"

	# no-tests because nothing here runs them on the device, and they are
	# most of the build. no-apps for the same reason: the applet links
	# the libraries and never executes the openssl command.
	./Configure "qt-$target" -D__ANDROID_API__="$api" \
		no-tests no-apps shared \
		--prefix="$prefix" --openssldir="$prefix/ssl"

	# libcrypto first, on its own, so that the symlink below exists
	# before anything links against it.
	make -j4 libcrypto_3.so

	# THE SYMLINK IS LOad-BEARING, and it is why libssl is built in a
	# second step rather than in one go.
	#
	# OpenSSL links libssl with "-lcrypto", and that spelling searches
	# for libcrypto.so or libcrypto.a -- it does not match
	# libcrypto_3.so. With the shared library renamed and no symlink,
	# the linker silently fell back to the STATIC libcrypto.a and
	# absorbed the whole of it into libssl_3.so: a 6.6 MB libssl where
	# 1.0 MB was expected, exporting none of libcrypto's symbols and
	# importing none either, because they had all been resolved
	# internally and hidden. The app would then have carried two copies
	# of OpenSSL, with SSL objects made by one and handed to the other.
	#
	# A symlink fixes it exactly: the linker follows libcrypto.so to
	# the real file and records that file's SONAME -- libcrypto_3.so --
	# as the dependency. The name on disk is what the linker searches
	# for; the SONAME inside is what it writes down.
	ln -sf libcrypto_3.so libcrypto.so

	# build_libs rather than the default target, which would also build
	# the apps this has just declined.
	make -j4 build_libs

	# The symlink is scaffolding for the link step and has no business
	# in the output, where it would be one more unsuffixed name for
	# somebody to mistake for the library Qt wants.
	rm -f libcrypto.so
)

mkdir -p "$prefix/lib" "$prefix/include"
cp -a "$src/include/openssl" "$prefix/include/" 2>/dev/null || true

# The _3 suffix is the name Qt asks the linker for; see the note above.
for lib in crypto ssl; do
	built="$src/lib${lib}_3.so"
	if [ ! -f "$built" ]; then
		echo "openssl: lib${lib}_3.so was not produced" >&2
		echo "  the Configure target should have been qt-$target, which" >&2
		echo "  sets shared_extension to _3.so." >&2
		exit 1
	fi

	cp "$built" "$prefix/lib/lib${lib}_3.so"
done

# Nothing unsuffixed may be left behind from an earlier run of this
# script. Qt would not load it, but its presence would make the wrong
# name look plausible to the next person reading the directory -- and it
# is exactly the file whose absence this whole change is about.
rm -f "$prefix/lib/libcrypto.so" "$prefix/lib/libssl.so"

echo "openssl: verifying the result is what Android will accept"
for lib in crypto ssl; do
	target_file="$prefix/lib/lib${lib}_3.so"

	if [ "$abi" = "arm64-v8a" ] && ! file "$target_file" | grep -q "ARM aarch64"; then
		echo "openssl: lib${lib}_3.so is not aarch64" >&2
		exit 1
	fi

	# The SONAME has to carry the suffix too. A library whose file is
	# libssl_3.so but whose SONAME says libssl.so records the wrong name
	# in whatever links against it, and the mismatch surfaces at load
	# time on the device rather than here.
	soname=$(patchelf --print-soname "$target_file")
	if [ "$soname" != "lib${lib}_3.so" ]; then
		echo "openssl: lib${lib}_3.so has soname $soname, which will not resolve" >&2
		exit 1
	fi
done

# Everything libssl asks for must either be in the package or be part of
# Android. This is the check that would otherwise fail on the device, and
# only on the device -- and it is what catches a rename that changed the
# filenames while leaving the dependency between them pointing at names
# no longer present.
saw_crypto=no

for needed in $(patchelf --print-needed "$prefix/lib/libssl_3.so"); do
	case "$needed" in
		libcrypto_3.so) saw_crypto=yes
			[ -f "$prefix/lib/libcrypto_3.so" ] || {
				echo "openssl: libssl_3 needs libcrypto_3.so and it is not here" >&2
				exit 1
			} ;;
		libc.so|libdl.so|libm.so|liblog.so) ;;
		*)
			echo "openssl: libssl_3 needs $needed, which the APK will not carry" >&2
			exit 1
			;;
	esac
done

# libssl MUST depend on libcrypto, and saying so is the point.
#
# The loop above can only fail on a dependency that is present and
# wrong. It passed a libssl that named no libcrypto at all -- because
# the static archive had been linked into it -- and reported success in
# exactly the tone it uses when everything is right. An absence is the
# thing this check exists to catch, so the absence is now the assertion.
if [ "$saw_crypto" != yes ]; then
	echo "openssl: libssl_3.so does not depend on libcrypto_3.so." >&2
	echo "  That means -lcrypto resolved to the static libcrypto.a and" >&2
	echo "  the whole of it went inside libssl_3.so. Check that the" >&2
	echo "  libcrypto.so symlink existed when libssl was linked." >&2
	exit 1
fi

# And it must not have absorbed libcrypto anyway. Size is the cheapest
# signal: a real libssl for this ABI is around a megabyte, and one
# carrying a private copy of libcrypto is six or seven.
ssl_bytes=$(stat -c %s "$prefix/lib/libssl_3.so")
if [ "$ssl_bytes" -gt 3000000 ]; then
	echo "openssl: libssl_3.so is $ssl_bytes bytes, which is far too big." >&2
	echo "  It has almost certainly absorbed libcrypto.a." >&2
	exit 1
fi

echo "openssl: $prefix/lib"
ls -l "$prefix/lib"
