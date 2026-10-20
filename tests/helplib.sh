#!/bin/sh
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/helplib.sh
#
#	ABSTRACT:	/HELP is answered out of the library built with the
#			server: the top topic, a parameter, a message.
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
MSCPHUB_HELPLIB=$2
export MSCPHUB_HELPLIB
FAILS=0

expect ()
{
	WANT=$1
	shift
	OUT=`"$MH" "$@" </dev/null 2>&1`

	if ! echo "$OUT" | grep -q -- "$WANT"
	then
		echo "%HELPLIB-E-FAILED, $* : expected $WANT, got: $OUT"
		FAILS=`expr $FAILS + 1`
	fi
}

expect "MSCPHUB runs on Linux" /HELP
expect "SCSSYSTEMID" "/HELP=/SYSTEMID"
expect "same low 16 bits" "/HELP=Messages IDCLASH"
expect "NOTOPIC" "/HELP=NOSUCHTOPIC"
expect "wrong cluster password" "/HELP=Messages AUTHFAIL"
expect "the node is going down" "/HELP=Messages CHCLOSE"
expect "30 retransmissions" "/HELP=Messages VCCLOSE"
expect "DKA100,/srv/vms/data1.img" "/HELP=/UNIT0"
expect "would crash the OpenVMS" "/HELP=Messages MSCPERR"

if [ $FAILS -ne 0 ]
then
	echo "%HELPLIB-E-SUMMARY, $FAILS check(s) failed"
	exit 1
fi

echo "%HELPLIB-S-SUMMARY, all checks passed"
exit 0
