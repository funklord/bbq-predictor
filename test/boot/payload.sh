#!/bin/bash
# Runs as the workload in a vm whose PID 1 is the image's own init. Starts
# nothing of the server's: everything it checks, init did at boot.
# Terminates: every wait is bounded (120 s), every read under timeout 5.
set -u
fail=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=1; }

echo "pid 1: $(tr '\0' ' ' </proc/1/cmdline)"

# Wait for init to have run the server's script: the pid file, and a live
# process of the daemon behind it.
pid=
#
# 120 s, because sysinit alone took 20 s in the hull-built OpenRC image on a
# loaded host, and a boot test that races the boot measures the host.
for _ in $(seq 1200); do
	if [ -r /run/bbq-predictor-serve.pid ]; then
		pid=$(cat /run/bbq-predictor-serve.pid)
		[ -n "$pid" ] && grep -q bbq-predictor-serve /proc/$pid/cmdline 2>/dev/null && break
	fi
	pid=
	sleep 0.1
done
if [ -z "$pid" ]; then
	bad "init did not start the daemon within 120 s"
	ls /etc/rc2.d /etc/runlevels/default 2>/dev/null | grep -i bbq
	cat /var/log/bbq-predictor-serve.log 2>/dev/null
	exit 1
fi
ok "init started the daemon: pid $pid, $(tr '\0' ' ' </proc/$pid/cmdline)"
ppid=$(awk '/^PPid:/{print $2}' /proc/$pid/status)
echo "      parent $ppid ($(cat /proc/$ppid/comm 2>/dev/null))"

uid=$(awk '/^Uid:/{print $2}' /proc/$pid/status)
want=$(id -u bbq-predictor-serve 2>/dev/null)
[ -n "$want" ] && [ "$uid" = "$want" ] && ok "runs as bbq-predictor-serve ($uid)" || bad "runs as uid $uid, want ${want:-no such account}"
root=$(readlink /proc/$pid/root)
[ "$root" = /var/lib/bbq-predictor/published ] && ok "chrooted to $root" || bad "root is '$root'"
grep -E '^(CapEff|NoNewPrivs):' /proc/$pid/status | sed 's/^/      /'

# Owners as the booted system sees them, which the bake also asserts. A vm
# showed every owner as root until hull a1b6dd2 gave a stored image its
# manifest's owners on run; a host with no /etc/subuid range still falls
# back to root, which hull says on its log.
owners=$(stat -c %U:%G /var/lib/bbq-predictor /var/lib/bbq-predictor/published /var/lib/bbq-predictor/demand | sort -u)
if [ "$owners" = bbq-predictor:bbq-predictor ]; then
	ok "the state directory is bbq-predictor's"
else
	bad "state directory owners: $(echo $owners)"
fi
echo "      demand file at boot: $(stat -c %s /var/lib/bbq-predictor/demand) bytes"
sed 's/^/      | /' /var/lib/bbq-predictor/demand

ss -ltn | grep -qE ':7227 ' && ok "listening on 7227" || bad "not listening on 7227"
ss -ltn | grep -qE ':443 ' && ok "listening on 443" || bad "not listening on 443"

# A head request for a station nothing publishes: status 2, and a demand line.
ask() {
	local station=$1
	exec 3<>/dev/tcp/127.0.0.1/7227 || { echo "no connection"; return 1; }
	{ printf 'BBQR\001\003\000\000%s' "$station"; head -c $((32 - ${#station})) /dev/zero; head -c 8 /dev/zero; } >&3
	timeout 5 head -c 12 <&3 | od -An -tu1 | tr -s ' '
	exec 3>&- 3<&-
}
header=$(ask IBOOT1)
status=$(echo "$header" | awk '{print $5}')
[ "$status" = 2 ] && ok "an unpublished station is answered 'not served'" || bad "answer:$header"
sleep 0.5
grep -q '^IBOOT1 ' /var/lib/bbq-predictor/demand && ok "and recorded in the demand file" || bad "demand file: $(cat /var/lib/bbq-predictor/demand)"

# Waited for as the daemon was: the runlevel starts them in an order of its
# own, and under OpenRC cron came a second after the daemon, which a check
# made at once reported as cron not running.
for _ in $(seq 1200); do
	pidof cron >/dev/null && break
	sleep 0.1
done
pidof cron >/dev/null && ok "cron is running" || bad "cron is not running after 120 s"
[ -r /etc/cron.d/bbq-predictor ] && ok "the round is in /etc/cron.d" || bad "no /etc/cron.d/bbq-predictor"

# What shutdown will do with it, asked of the init that is running: OpenRC
# stops what its runlevel started, sysvinit runs the kill links in rc0.d.
if [ "$(cat /proc/1/comm)" = openrc-init ]; then
	if [ -e /etc/runlevels/default/bbq-predictor-serve ]; then
		ok "in OpenRC's default runlevel, which shutdown stops"
	else
		bad "not in OpenRC's default runlevel"
	fi
elif ls /etc/rc0.d 2>/dev/null | grep -q '^K..bbq-predictor-serve$'; then
	ok "stopped at runlevel 0 ($(ls /etc/rc0.d | grep bbq))"
else
	bad "nothing stops it at shutdown"
fi
echo "      rc2.d: $(ls /etc/rc2.d | tr '\n' ' ')"

exit $fail
