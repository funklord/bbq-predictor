#!/bin/sh
# test/boot/bake.sh INIT DEB REF -- Devuan excalibur with INIT (sysvinit or
# openrc), cron, and DEB installed as apt installs it on a host, built by
# `hull build` into hull's store as REF (project.md sec 15.12.5). No docker
# and no root: each step runs in a chroot machine with this user's subuid
# range mapped, so apt is root there and the owners it makes are kept.
#
# The build file and a copy of DEB are written to a directory of their own
# under TMPDIR, since hull resolves a build's paths against its file. The
# first step is cached by hull until its week is out; the second runs
# whenever the package changes.
# Terminates: the build is under timeout 1200; the directory is removed on
# exit.
set -eu
init=$1 deb=$2 ref=$3
case "$init" in
sysvinit) pkgs="sysvinit-core" ;;
openrc) pkgs="sysvinit-core openrc" ;;
*) echo "bake.sh: INIT is sysvinit or openrc" >&2; exit 2 ;;
esac
hull=${HULL:-hull}
dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT INT TERM
cp -- "$deb" "$dir/server.deb"

# What the postinst made, checked in the build: a vm shows every owner as
# root (hull's gap, project.md sec 15.12.5), so this is the only place
# they can be checked.
check='owners=$(stat -c %U:%G /var/lib/bbq-predictor /var/lib/bbq-predictor/published /var/lib/bbq-predictor/demand | sort -u); [ "$owners" = bbq-predictor:bbq-predictor ] || { echo "owners: $owners"; exit 1; }; [ ! -s /var/lib/bbq-predictor/demand ] || { echo "the demand file is not empty"; exit 1; }; echo "bake: $(dpkg-query -W -f="\${Package} \${Version}" bbq-predictor-server), owners $owners, rc2.d: $(ls /etc/rc2.d | grep bbq || echo none), openrc: $(ls /etc/runlevels/default 2>/dev/null | grep bbq || echo none)"'

cat >"$dir/build.conf" <<CONF
(build
  (base "docker.io/devuan/devuan:excalibur")
  (steps
    (step
      (name "init")
      (network true)
      (run "apt-get -qq update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends $pkgs cron iproute2 procps bash >/dev/null 2>&1 && apt-get clean"))
    (step
      (name "server")
      (network true)
      (copy (path (from "./server.deb") (to "/root/server.deb")))
      (run "DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends /root/server.deb >/dev/null 2>&1 && rm /root/server.deb && apt-get clean && $(printf '%s' "$check" | sed 's/\\/\\\\/g; s/"/\\"/g')"))))
CONF
# hull's own status decides, never a filter's: a pipe into grep would
# have passed a failed build, which is how CI passed a failed suite
# (project.md sec 18).
status=0
timeout 1200 "$hull" build -f "$dir/build.conf" "$ref" >"$dir/build.log" 2>&1 || status=$?
grep -E '^bake:|^owners:|demand file' "$dir/build.log" || true
if [ "$status" -ne 0 ]; then
	echo "bake.sh: hull build exited $status:" >&2
	grep -v emulated "$dir/build.log" | tail -20 >&2
	exit 1
fi
