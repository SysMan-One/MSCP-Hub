#!/bin/sh
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/params.sh
#
#	ABSTRACT:	The checks of the parameters: every mandatory one is asked
#			for, a value out of its range is refused, before the
#			server touches the LAN.  No privilege is needed.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	17-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		20-OCT-2026	RRL	X01-10 : Initial public version.
#

MH=$1
FAILS=0

expect ()
{
	WANT=$1
	shift
	OUT=`"$MH" "$@" 2>&1`
	RC=$?

	if [ $RC -eq 0 ] || ! echo "$OUT" | grep -q -- "$WANT"
	then
		echo "%PARAMS-E-FAILED, $* : expected $WANT, rc=$RC, got: $OUT"
		FAILS=`expr $FAILS + 1`
	fi
}

ALL="/INTERFACE=lo /NODE=MHUB /SYSTEMID=65200 /GROUP=4242 /AUTHORIZE=5A3C9E1700000000"

expect "NOPARAM, the parameter /INTERFACE" /NODE=MHUB
expect "NOPARAM, the parameter /NODE" /INTERFACE=lo
expect "NOPARAM, the parameter /SYSTEMID" /INTERFACE=lo /NODE=MHUB
expect "NOPARAM, the parameter /GROUP" /INTERFACE=lo /NODE=MHUB /SYSTEMID=1
expect "NOPARAM, the parameter /AUTHORIZE" /INTERFACE=lo /NODE=MHUB /SYSTEMID=1 /GROUP=1
expect "IVPARAM.*/NODE" $ALL /NODE=TOOLONGNAME
expect "IVPARAM.*/NODE" $ALL /NODE=MH-1
expect "IVPARAM.*/SYSTEMID" $ALL /SYSTEMID=70000
expect "IVPARAM.*/GROUP" $ALL /GROUP=65280
expect "IVPARAM.*/LISTEN" $ALL /LISTEN=0
expect "IVPARAM.*/CREDITS" $ALL /CREDITS=65
expect "IVPARAM.*/TIMEZONE" $ALL /TIMEZONE=MSK
expect "IVPARAM.*/TIMEZONE" $ALL /TIMEZONE=+15:00

if [ $FAILS -ne 0 ]
then
	echo "%PARAMS-E-SUMMARY, $FAILS check(s) failed"
	exit 1
fi

echo "%PARAMS-S-SUMMARY, all checks passed"
exit 0
