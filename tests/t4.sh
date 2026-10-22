#!/bin/sh
#
#	FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
#
#	MODULE:		tests/t4.sh
#
#	ABSTRACT:	MHMON records a server in the T4 style: T_STAT HOLD
#			publishes a block whose unit does 1000 reads a second,
#			MHMON records it every second; the file must have the
#			four rows of the header of T4 - the node, the date, the
#			time, the column names with the Sample Time first - and
#			every row the same number of fields, and the rate of
#			the unit must be 1000.
#
#	AUTHOR:		StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
#
#	CREATION DATE:	22-OCT-2026
#
#	MODIFICATION HISTORY:
#
#		22-OCT-2026	RRL	X01-17 : Initial revision.
#

TSTAT=$1
MHMON=$2
OUT=`mktemp -d`
FAILS=0

fail ()
{
	echo "%T4-E-FAILED, $*"
	FAILS=`expr $FAILS + 1`
}

"$TSTAT" HOLD 6 > "$OUT/node" &
HOLD=$!
sleep 1
NODE=`cat "$OUT/node"`

"$MHMON" DISK /NODE=$NODE /NODISPLAY /RECORD="$OUT/t4.csv" /INTERVAL=1 /ENDING=`date -d '+4 sec' +%H:%M:%S` 2>"$OUT/err"
wait $HOLD

F="$OUT/t4.csv"

[ -s "$F" ] || fail "no file: `cat $OUT/err`"

[ "`sed -n 1p $F | cut -d, -f1`" = "$NODE" ] || fail "row 1 is not the node: `sed -n 1p $F`"
sed -n 2p "$F" | grep -q '^[0-9][0-9]-[A-Z][A-Z][A-Z]-[0-9][0-9][0-9][0-9]$' || fail "row 2 is not a date: `sed -n 2p $F`"
sed -n 3p "$F" | grep -q '^[0-9][0-9]:[0-9][0-9]$' || fail "row 3 is not a time: `sed -n 3p $F`"
sed -n 4p "$F" | grep -q '^\[MSCPHUB\]Sample Time,' || fail "row 4 does not begin with the Sample Time: `sed -n 4p $F`"

[ `sed -n '5,$p' "$F" | wc -l` -ge 2 ] || fail "fewer than 2 samples"
[ `sed -n '4,$p' "$F" | awk -F, '{print NF}' | sort -u | wc -l` -eq 1 ] || fail "the rows have different numbers of fields"

#	The rate of the unit: the column $42$DKA100 Op Rate, the last sample
COL=`sed -n 4p "$F" | tr , '\n' | grep -n 'DKA100 Op Rate' | cut -d: -f1`
RATE=`tail -1 "$F" | cut -d, -f$COL`

case "$RATE" in
	9[5-9][0-9].*|10[0-4][0-9].*)	;;
	*)				fail "the rate of the unit is $RATE, not about 1000" ;;
esac

rm -rf "$OUT"

if [ $FAILS -ne 0 ]
then
	echo "%T4-E-SUMMARY, $FAILS check(s) failed"
	exit 1
fi

echo "%T4-S-SUMMARY, a T4 file of `echo $NODE` checked"
exit 0
