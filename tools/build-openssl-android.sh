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

# The API level the app is built for. 28 matches Qt's own default and the
# minSdk this project declares; building OpenSSL for a HIGHER level than
# the app would resolve symbols at build time that are absent at run time.
api=${ANDROID_API:-28}

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

(
	cd "$src"

	# no-tests because nothing here runs them on the device, and they are
	# most of the build. no-apps for the same reason: the applet links
	# the libraries and never executes the openssl command.
	./Configure "$target" -D__ANDROID_API__="$api" \
		no-tests no-apps shared \
		--prefix="$prefix" --openssldir="$prefix/ssl"

	# build_libs rather than the default target, which would also build
	# the apps this has just declined.
	make -j4 build_libs
)

mkdir -p "$prefix/lib" "$prefix/include"
cp -a "$src/include/openssl" "$prefix/include/" 2>/dev/null || true

# The names are the ABI-suffixed ones androiddeployqt expects of an extra
# library, and they still match what Qt's TLS backend searches for -- its
# plugin looks for "libcrypto.*" and "libssl.*" rather than an exact name.
for lib in crypto ssl; do
	built="$src/lib$lib.so"
	if [ ! -f "$built" ]; then
		echo "openssl: lib$lib.so was not produced" >&2
		exit 1
	fi

	cp "$built" "$prefix/lib/lib$lib.so"
done

echo "openssl: verifying the result is what Android will accept"
for lib in crypto ssl; do
	target_file="$prefix/lib/lib$lib.so"

	if [ "$abi" = "arm64-v8a" ] && ! file "$target_file" | grep -q "ARM aarch64"; then
		echo "openssl: lib$lib.so is not aarch64" >&2
		exit 1
	fi

	soname=$(patchelf --print-soname "$target_file")
	if [ "$soname" != "lib$lib.so" ]; then
		echo "openssl: lib$lib.so has soname $soname, which will not resolve" >&2
		exit 1
	fi
done

# Everything libssl asks for must either be in the package or be part of
# Android. This is the check that would otherwise fail on the device, and
# only on the device.
for needed in $(patchelf --print-needed "$prefix/lib/libssl.so"); do
	case "$needed" in
		libcrypto.so) [ -f "$prefix/lib/libcrypto.so" ] || {
			echo "openssl: libssl needs libcrypto.so and it is not here" >&2
			exit 1
		} ;;
		libc.so|libdl.so|libm.so|liblog.so) ;;
		*)
			echo "openssl: libssl needs $needed, which the APK will not carry" >&2
			exit 1
			;;
	esac
done

echo "openssl: $prefix/lib"
ls -l "$prefix/lib"
