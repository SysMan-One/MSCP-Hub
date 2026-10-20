#!/bin/sh
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/docs.sh
#
#	ABSTRACT:	The documentation follows the code: every parameter of
#			the table of the head module has a topic in both help
#			libraries and a paragraph in the manual page, and every
#			message of the catalogue has a topic in both libraries.
#			A parameter the server does not know is only warned of,
#			so a missing one in the documents is found here or not
#			at all.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	18-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		20-OCT-2026	RRL	X01-10 : Initial public version.
#

SRC=$1
FAILS=0

fail ()
{
	echo "%DOCS-E-MISSING, $*"
	FAILS=`expr $FAILS + 1`
}

#	The parameters: $ASCINI("name") of S_OPTS; /UNIT0 ... /UNIT7 are one topic
OPTS=`sed -n 's/.*\$ASCINI("\([a-z0-9]*\)").*/\1/p' "$SRC/src/mscphub.c" | tr a-z A-Z | sed 's/^UNIT[0-9]$/UNIT0/' | sort -u`

[ -n "$OPTS" ] || fail "no parameter found in src/mscphub.c"

for O in $OPTS
do
	for F in doc/mscphublib.md doc/mscphublib_ru.md
	do
		grep -q "^## /$O " "$SRC/$F" || fail "/$O has no topic in $F"
	done

	grep -q "/$O" "$SRC/doc/mscphub.8.in" || fail "/$O is not in doc/mscphub.8.in"
done

#	The messages: MH$_NAME of the catalogue, but the NORMAL of success
MSGS=`sed -n 's/^#define[ 	]*MH\$_\([A-Z]*\)[ 	].*/\1/p' "$SRC/include/mh_def.h" | grep -v '^NORMAL$'`

[ -n "$MSGS" ] || fail "no message found in include/mh_def.h"

for M in $MSGS
do
	for F in doc/mscphublib.md doc/mscphublib_ru.md
	do
		grep -q "^### $M\$" "$SRC/$F" || fail "message $M has no topic in $F"
	done
done

if [ $FAILS -ne 0 ]
then
	echo "%DOCS-E-SUMMARY, $FAILS check(s) failed"
	exit 1
fi

echo "%DOCS-S-SUMMARY, `echo $OPTS | wc -w` parameters and `echo $MSGS | wc -w` messages documented"
exit 0
