#!/bin/sh
# test/boot/bake.sh INIT DEB REF -- Devuan excalibur with INIT (sysvinit or openrc),
# cron, and DEB installed as apt installs it on a host, committed to hull's
# store as REF. Baked in docker rather than by `hull build`, whose steps
# have no /dev/null (hull inbox 20261023T1630Z). The owners come through
# the tar, which hull commit reads as root's would.
# Terminates: one container under timeout 900, removed by name on exit.
set -eu
init=$1 deb=$2 ref=$3
case "$init" in
sysvinit) pkgs="sysvinit-core" ;;
openrc) pkgs="sysvinit-core openrc" ;;
*) echo "bake.sh: INIT is sysvinit or openrc" >&2; exit 2 ;;
esac
hull=${HULL:-hull}
name=bbq-bake-$init-$$
tar=$(mktemp --suffix=.tar)
trap 'docker rm -f "$name" >/dev/null 2>&1 || true; rm -f "$tar"' EXIT INT TERM
timeout 900 docker run --name "$name" -v "$(realpath "$deb"):/server.deb:ro" \
	devuan/devuan:excalibur sh -c '
	set -eu
	export DEBIAN_FRONTEND=noninteractive
	apt-get -qq update >/dev/null
	apt-get -qq install -y --no-install-recommends '"$pkgs"' cron iproute2 procps bash \
		>/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
	apt-get -qq install -y --no-install-recommends /server.deb >>/tmp/apt.log 2>&1 ||
		{ tail -20 /tmp/apt.log; exit 1; }
	apt-get clean
	# What the postinst made, checked here: a vm boot shows every owner as
	# root, so this is the only place they can be.
	owners=$(stat -c %U:%G /var/lib/bbq-predictor /var/lib/bbq-predictor/published \
	                      /var/lib/bbq-predictor/demand | sort -u)
	[ "$owners" = bbq-predictor:bbq-predictor ] || { echo "owners: $owners"; exit 1; }
	[ ! -s /var/lib/bbq-predictor/demand ] || { echo "the demand file is not empty"; exit 1; }
	echo "bake: $(dpkg-query -W -f="\${Package} \${Version}" bbq-predictor-server)," \
	     "owners $owners, rc2.d: $(ls /etc/rc2.d | grep bbq || echo none)," \
	     "openrc: $(ls /etc/runlevels/default 2>/dev/null | grep bbq || echo none)"
'
docker export -o "$tar" "$name"
"$hull" commit --tar "$tar" "$ref"
