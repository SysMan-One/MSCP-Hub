#!/bin/sh
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/init.sh
#
#	ABSTRACT:	The scripts of OpenRC and System V init, as CMake made them
#			in the build directory: all four are shell the shell can
#			read; the System V script of the server refuses to start
#			without its parameter file (LSB 6) and says it is not
#			running (LSB 3); the one of T4 starts MHMON on a block of
#			T_STAT HOLD, says it runs, stops it, and the file of T4 is
#			there and closed.  The installed paths in a copy are put to
#			those of the build directory; the rest is given by the
#			variables the scripts take from /etc/default.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	22-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		22-OCT-2026	RRL	X01-18 : Initial revision.
#		23-OCT-2026	RRL	X01-19ECO6 : the node of T_STAT waited for, not a second; the block held 15 seconds.
#

BIN=$1
TSTAT=$2
MHMON=$3
OUT=`mktemp -d`
FAILS=0

fail ()
{
	echo "%INIT-E-FAILED, $*"
	FAILS=`expr $FAILS + 1`
}

for f in openrc/mscphub openrc/mscphub-t4 sysv/mscphub sysv/mscphub-t4
do
	[ -f "$BIN/$f" ] || { fail "$f was not made"; continue; }
	grep -q '@MH_' "$BIN/$f" && fail "$f has a path not filled in"
	sh -n "$BIN/$f" || fail "$f is not shell"
done

#
#	The server: no parameter file, no start
#
export MSCPHUB_CONFIG=$OUT/none.conf MSCPHUB_LOG=$OUT/mscphub.log MSCPHUB_PIDFILE=$OUT/mscphub.pid

sh "$BIN/sysv/mscphub" start >"$OUT/out" 2>&1
RC=$?
[ $RC -eq 6 ] || fail "start without the parameter file: $RC, not 6: `cat $OUT/out`"
[ -f "$MSCPHUB_PIDFILE" ] && fail "start without the parameter file left a PID file"

sh "$BIN/sysv/mscphub" status >/dev/null 2>&1
RC=$?
[ $RC -eq 3 ] || fail "status of a server that never ran: $RC, not 3"

#
#	The collector of T4, on a block that T_STAT publishes
#
export MHMON_DIR=$OUT/t4 MHMON_LOG=$OUT/t4.log MHMON_PIDFILE=$OUT/t4.pid MHMON_INTERVAL=1

sed "s|^PROGRAM=.*|PROGRAM=$MHMON|" "$BIN/sysv/mscphub-t4" >"$OUT/t4init"

"$TSTAT" HOLD 15 > "$OUT/node" &
HOLD=$!
for i in 1 2 3 4 5 6 7 8 9 10
do
	[ -s "$OUT/node" ] && break
	sleep 1
done
export MHMON_NODE=`cat "$OUT/node"`
[ -n "$MHMON_NODE" ] || fail "T_STAT HOLD gave no node in 10 seconds"

sh "$OUT/t4init" start >"$OUT/out" 2>&1 || fail "start of T4: `cat $OUT/out`"
sleep 3

sh "$OUT/t4init" status >"$OUT/out" 2>&1 || fail "status of T4 running: `cat $OUT/out`"
ls "$MHMON_DIR"/T4_*_RUN.CSV >/dev/null 2>&1 || fail "no file of T4 is being written: `cat $MHMON_LOG`"

sh "$OUT/t4init" stop >"$OUT/out" 2>&1
grep -q "done" "$OUT/out" || fail "stop of T4: `cat $OUT/out`"

sh "$OUT/t4init" status >/dev/null 2>&1
RC=$?
[ $RC -eq 3 ] || fail "status of T4 stopped: $RC, not 3"

ls "$MHMON_DIR"/T4_*_RUN.CSV >/dev/null 2>&1 && fail "the file of T4 was not closed"
ls "$MHMON_DIR"/T4_${MHMON_NODE}_*.CSV >/dev/null 2>&1 || fail "no file of T4 for $MHMON_NODE"

wait $HOLD
rm -rf "$OUT"

if [ $FAILS -ne 0 ]
then
	echo "%INIT-E-SUMMARY, $FAILS check(s) failed"
	exit 1
fi

echo "%INIT-S-SUMMARY, the init scripts checked"
exit 0
