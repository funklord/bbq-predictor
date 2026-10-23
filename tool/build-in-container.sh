#!/bin/sh
# Build the Debian packages inside a container of another release
# (project.md sec 15.15), for a server whose libraries are older than this
# machine's: the holder's runs Devuan daedalus, which is Debian bookworm's
# libraries.
#
#   tool/build-in-container.sh IMAGE SUFFIX [PROFILES]
#   tool/build-in-container.sh debian:bookworm-slim daedalus1 \
#           pkg.bbq-predictor.server-only
#
# Builds HEAD as committed -- git archive, and monocypher at the commit its
# gitlink names -- never the working tree, since a package is a claim about
# a commit. The version gains ~SUFFIX, which sorts below the plain version
# so an upgrade of the host to a newer release replaces it. Build
# dependencies come from debian/control under the same profiles, through
# apt-get build-dep, so the container has exactly what is declared and no
# more: that is how a missing one was found (sec 15.15). The suite runs
# unless DEB_BUILD_OPTIONS says nocheck. The packages land in DEB_DIR.
#
# Terminates: the build inside is under timeout 3600 and the dependency
# install under 900. The container is named, and stopped by name if this
# script is interrupted, since a signal to this script does not reach it.
set -eu

image=${1:?usage: tool/build-in-container.sh IMAGE SUFFIX [PROFILES]}
suffix=${2:?usage: tool/build-in-container.sh IMAGE SUFFIX [PROFILES]}
profiles=${3:-}
out=${DEB_DIR:-build/deb}

case "$suffix" in
	*[!a-z0-9+.]* | "") echo "suffix: lowercase letters, digits, + and . only" >&2; exit 2 ;;
esac

# The export is this script's own directory, made fresh and removed after.
stage=build/container-$suffix
case "$stage" in
	/* | *..*) echo "refusing stage directory $stage" >&2; exit 2 ;;
esac
rm -rf -- "$stage"
mkdir -p "$stage/src" "$out"
git archive HEAD | tar -x -C "$stage/src"
git -C monocypher archive --prefix=monocypher/ HEAD | tar -x -C "$stage/src"
head=$(git rev-parse --short HEAD)

cat >"$stage/inside.sh" <<'EOF'
#!/bin/bash
set -u
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
apt-get update -qq >/dev/null 2>&1
cp -a /stage/src /build && cd /build || exit 1
profile_flag=${PROFILES:+-P $PROFILES}
[ -n "${DEB_BUILD_OPTIONS:-}" ] && case "$DEB_BUILD_OPTIONS" in *nocheck*)
	profile_flag="-P ${PROFILES:+$PROFILES,}nocheck" ;; esac
timeout 900 apt-get install -y -qq --no-install-recommends build-essential \
        >/tmp/apt.log 2>&1 &&
timeout 900 apt-get build-dep -y -qq $profile_flag ./ >>/tmp/apt.log 2>&1 || {
	echo "build dependencies did not install:"; tail -20 /tmp/apt.log; exit 1
}
version=$(dpkg-parsechangelog -S Version)
{
	echo "bbq-predictor (${version}~${SUFFIX}) unstable; urgency=low"
	echo
	echo "  * Built in $IMAGE${PROFILES:+ with $PROFILES}, from $HEAD."
	echo
	echo " -- Nabeel Sowan <nabeel@vibes.se>  $(date -R)"
	echo
	cat debian/changelog
} >/tmp/changelog && mv /tmp/changelog debian/changelog
DEB_BUILD_PROFILES=$PROFILES timeout 3600 make deb DEB_DIR=/out >/tmp/build.log 2>&1
code=$?
grep -E '^test: [0-9]|^deb:' /tmp/build.log
[ $code -ne 0 ] && tail -40 /tmp/build.log

# Root wrote them; hand them back to whoever ran the script, or nobody
# but root can remove them from the build tree.
find /out -maxdepth 1 -name "*~${SUFFIX}_*" -exec chown "$OWNER" {} +
exit $code
EOF

name=bbq-deb-$suffix-$$
trap 'docker kill "$name" >/dev/null 2>&1 || true' INT TERM
code=0
docker run --rm --name "$name" --stop-timeout 5 \
        -e IMAGE="$image" -e SUFFIX="$suffix" -e PROFILES="$profiles" -e HEAD="$head" \
        -e DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-}" -e OWNER="$(id -u):$(id -g)" \
        -v "$PWD/$stage:/stage:ro" -v "$PWD/$out:/out" \
        "$image" bash /stage/inside.sh || code=$?

rm -rf -- "$stage"
exit $code
