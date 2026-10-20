#define	__MODULE__	"MHUTL"
#define	__IDENT__	"X01-12"
#define	__REV__		"1.12.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_utl.c
**
**  ABSTRACT:	Service routines shared by the modules of the facility: the
**		message catalogue, the OpenVMS time, conversions of the
**		hexadecimal and MAC address notations.
**
**  DESCRIPTION: Diagnostics of interest to the developer go to $LOG with an
**		explicit STS$K_* severity; those of interest to the operator
**		are signalled by $MHMSG in the OpenVMS format, e.g.
**
**		    %MSCPHUB-F-IDCLASH, node ALPHA1 (08-00-2B-E5-40-01) uses system ID ...
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-12 : MISALIGN.
**
**--
*/

#include	<time.h>
#include	<errno.h>
#include	<unistd.h>
#include	<strings.h>
#include	<stdio.h>
#include	<stdlib.h>

#include	"mh_def.h"

/*
**  The catalogue, in the form UTIL$INIMSG takes; NOT const - UTIL$INIMSG
**  sorts it in place.  The parameters are named A_ on purpose: a designated
**  initializer is macro-expanded like anything else.
*/
#define	$MHREC(a_sts, a_txt)	{ .sts = (int) (a_sts), .textl = (unsigned char) (sizeof(a_txt) - 1), .text = a_txt }

static	EMSG_RECORD	s_msgtab [] = {
	$MHREC(MH$_NORMAL,	"NORMAL, normal successful completion"),
	$MHREC(MH$_STARTUP,	"STARTUP, MSCPHUB !AZ, node !AZ, system ID !UL, group !UL, on !AZ (!AZ)"),
	$MHREC(MH$_LISTEN,	"LISTEN, listening !UL second!%S before announcing the node"),
	$MHREC(MH$_ANNOUNCE,	"ANNOUNCE, node !AZ !AZ on !AZ, !UL other node!%S seen in group !UL"),
	$MHREC(MH$_SHUTDOWN,	"SHUTDOWN, signal !UL received, the server is stopping"),
	$MHREC(MH$_NOPARAM,	"NOPARAM, the parameter /!AZ is mandatory"),
	$MHREC(MH$_IVPARAM,	"IVPARAM, illegal value !AZ of /!AZ: !AZ"),
	$MHREC(MH$_SYSERR,	"SYSERR, !AZ failed, errno=!SL (!AZ)"),
	$MHREC(MH$_NEWNODE,	"NEWNODE, node !AZ, system ID !UL, adapter !AZ, CC V1.!UL (!UL octets)"),
	$MHREC(MH$_IDCLASH,	"IDCLASH, node !AZ (!AZ) uses the system ID low word !UL of this server"),
	$MHREC(MH$_NAMECLASH,	"NAMECLASH, node !AZ (!AZ, system ID !UL) uses the SCSNODE of this server"),
	$MHREC(MH$_MANYNODES,	"MANYNODES, more than !UL nodes on the LAN, node !AZ is not remembered"),
	$MHREC(MH$_NOTOPIC,	"NOTOPIC, sorry, no documentation on !AZ"),
	$MHREC(MH$_CHOPEN,	"CHOPEN, channel to node !AZ (!UL) is open, CC V1.!UL, pipe quota !UL, buffer !UL, services !XW/!XW"),
	$MHREC(MH$_CHCLOSE,	"CHCLOSE, channel to node !AZ (!UL) is closed: !AZ"),
	$MHREC(MH$_VCOPEN,	"VCOPEN, virtual circuit to node !AZ (!UL) is open: !AZ !AZ on !AZ, datagram !UL, message !UL"),
	$MHREC(MH$_VCCLOSE,	"VCCLOSE, virtual circuit to node !AZ (!UL) is closed: !AZ"),
	$MHREC(MH$_TRSTATS,	"TRSTATS, transport: !UQ packet!%S sent, !UQ retransmitted, !UQ received, !UQ duplicate!%S, !UQ out of order, !UQ of an old channel; window up to !UL"),
	$MHREC(MH$_CONOPEN,	"CONOPEN, connection !AZ of !AZ (!UL) to !AZ is open, credit !SL/!UL"),
	$MHREC(MH$_CONCLOSE,	"CONCLOSE, connection !AZ of !AZ (!UL) is closed: !AZ"),
	$MHREC(MH$_UNITCFG,	"UNITCFG, unit !AZ is !AZ, !UL block!%S!AZ, SLUN !XW"),
	$MHREC(MH$_MSCPERR,	"MSCPERR, end message of !AZ to !AZ of !AZ (!UL) held back: !AZ"),
	$MHREC(MH$_IOERR,	"IOERR, !AZ of unit !AZ, LBN !UL, !UL octets, failed: errno=!SL (!AZ)"),
	$MHREC(MH$_BLKERR,	"BLKERR, !AZ from !AZ (!UL), XCT_ID !XL!XL: !AZ, dropped"),
	$MHREC(MH$_BLKSTATS,	"BLKSTATS, block transfers: !UQ chunk!%S sent (!UQ octets), !UQ RET_CNF, !UQ REQ_DAT, !UQ chunk!%S taken (!UQ octets), !UQ dropped"),
	$MHREC(MH$_MSCPSTATS,	"MSCPSTATS, MSCP server: !UL command!%S in progress, !UQ octets of data buffers in use"),
	$MHREC(MH$_UNITSTATS,	"UNITSTATS, unit !AZ: ONLINE to !UL host!%S, !UL command!%S running, !UL waiting, !UQ read!%S (!UQ octets), !UQ write!%S (!UQ octets)"),
	$MHREC(MH$_BKSTATS,	"BKSTATS, unit !AZ: sector !UL, !UQ read-modify-write!%S, !UQ wait!%S for a range, !UQ group fdatasync!%S for !UQ write!%S"),
	$MHREC(MH$_IOSTUCK,	"IOSTUCK, !AZ of unit !AZ, LBN !UL, is in the backend for !UL seconds: cancelled"),
	$MHREC(MH$_DEVBUSY,	"DEVBUSY, the device !AZ is in use: mounted on this host, or another server has it"),
	$MHREC(MH$_NODESTATS,	"NODESTATS, node !AZ (!UL): channel !AZ, open !UL time!%S; virtual circuit !AZ, open !UL time!%S; !UL connection!%S open"),
	$MHREC(MH$_ACKSTATS,	"ACKSTATS, acknowledgements (/ACK=!AZ): !UQ asked for at once; !UQ with a packet of ours, !UQ alone: "
				"!UQ asked, !UQ for a duplicate, !UQ for !UL packets, !UQ when drained, !UQ on the tick"),
	$MHREC(MH$_LATSTATS,	"LATSTATS, unit !AZ, !AZ: !UQ command!%S, average !UQ us (backend !UQ, transfers !UQ, the rest !UQ), longest !UQ us; "
				"under 0.1/0.3/1/3/10 ms and longer: !UQ/!UQ/!UQ/!UQ/!UQ/!UQ"),
	$MHREC(MH$_MISALIGN,	"MISALIGN, !AZ starts !SL octets into a physical sector of !UL: every write is a read-modify-write of the disk"),
	$MHREC(MH$_UPTIME,	"UPTIME, MSCPHUB !AZ, node !AZ, up !UL day!%S !2ZL:!2ZL:!2ZL, OpenVMS time of !AZ"),
	$MHREC(MH$_UNSAFE,	"UNSAFE, /SYNC=NONE: a WRITE is answered before its data is on disk - for tests only"),
	$MHREC(MH$_AUTHFAIL,	"AUTHFAIL, node !AZ (!UL, !AZ) sent a CC message with a wrong cluster password, ignored"),
	$MHREC(MH$_STATS,	"STATS, rx !UQ frame!%S (!UQ CC, !UQ TR, !UQ dropped, !UQ own, !UQ bad password), tx !UQ frame!%S, !UQ error!%S")
	};

static	EMSG_RECORD_DESC	s_msgdsc = {
	.msgnr		= (unsigned) $ARRSZ(s_msgtab),
	.facno		= MH$K_FACNO,
	.msgrec		= s_msgtab,
	.facname	= __FAC__
	};

/*
**  OpenVMS time of the UNIX epoch: 40587 days from 17-NOV-1858, in 100 ns
*/
#define	MH$K_VMS_EPOCH		35067168000000000ULL

static	const char	s_hexdigits [] = "0123456789ABCDEF";

/*
**  The zone of the OpenVMS time, /TIMEZONE
*/
enum	{
	MH$K_TZ_LOCAL	= 0,			/* The zone of the host, summer time and all	*/
	MH$K_TZ_FIXED				/* UTC + S_TZOFF				*/
	};

static	int		s_tzmode = MH$K_TZ_LOCAL;
static	int		s_tzoff;		/* Seconds east of Greenwich			*/



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Register the message catalogue of the facility.  Called first: the
**	diagnostics of the command line are signalled through it.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_MSGTAB, S_MSGDSC; the message output goes to the standard error
**	until /LOGFILE redirects it.
**
**  RETURN VALUE:
**	The condition value of UTIL$INIMSG.
**--
*/
int	mh$inimsg	(void)
{
	__util$setlogfd(STDERR_FILENO);

	return	(int) __util$inimsg(&s_msgdsc);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Convert a UNIX time to the OpenVMS 64-bit binary time: 100 ns units
**	since 17-NOV-1858 00:00.  The SWINCARN and CURTIME of PPD START and the
**	TRANSMIT_TIME of a CC message are in this form; SWINCARN must be a
**	real one - 0 or UNIX seconds make the server "older" than any VMS boot,
**	which a VMS node with a clashing identity answers by CLUEXIT.
**
**  FORMAL PARAMETERS:
**
**	a_sec		Seconds of the UNIX epoch
**	a_nsec		Nanoseconds
**	a_vmstime	Output: the OpenVMS time
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the time precedes 17-NOV-1858.
**--
*/
int	mh$unix2vms	(
		int64_t		a_sec,
		int64_t		a_nsec,
		uint64_t *	a_vmstime
			)
{
int64_t	l_ticks;

	l_ticks = a_sec * 10000000LL + a_nsec / 100;

	if ( (l_ticks < 0) && ((uint64_t) (-l_ticks) > MH$K_VMS_EPOCH) )
		return	STS$K_ERROR;

	*a_vmstime = (uint64_t) ((int64_t) MH$K_VMS_EPOCH + l_ticks);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take the zone the OpenVMS time of the server is in.  An OpenVMS node
**	keeps local time, not UTC; the SWINCARN of PPD START and the CURTIME
**	of the SB are shown by SCACP and SHOW CLUSTER as that node's own.
**	Only their order matters to the protocol, so
**	the zone is for the eyes of the operator.
**
**  FORMAL PARAMETERS:
**
**	a_spec		LOCAL - the zone of the host (TZ, /etc/localtime), with
**			its summer time; UTC; [+|-]hh[:mm] - a fixed offset
**			east of Greenwich, up to 14 hours.  NULL or "" - LOCAL.
**
**  IMPLICIT OUTPUTS:
**	S_TZMODE, S_TZOFF.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - not one of the forms.
**--
*/
int	mh$tz_set	(
	const	char *		a_spec
			)
{
int	l_sign = 1, l_hh = 0, l_mm = 0, l_n = 0;

	if ( !a_spec || !*a_spec || !strcasecmp(a_spec, "LOCAL") )
		{
		s_tzmode = MH$K_TZ_LOCAL;
		return	STS$K_SUCCESS;
		}

	s_tzmode = MH$K_TZ_FIXED;
	s_tzoff = 0;

	if ( !strcasecmp(a_spec, "UTC") )
		return	STS$K_SUCCESS;

	if ( (*a_spec == '+') || (*a_spec == '-') )
		l_sign = (*a_spec++ == '-') ? -1 : 1;

	for ( ; (*a_spec >= '0') && (*a_spec <= '9') && (l_n < 2); a_spec++, l_n++ )
		l_hh = l_hh * 10 + (*a_spec - '0');

	if ( !l_n )
		return	STS$K_ERROR;

	if ( *a_spec == ':' )
		{
		a_spec++;

		for ( l_n = 0; (*a_spec >= '0') && (*a_spec <= '9') && (l_n < 2); a_spec++, l_n++ )
			l_mm = l_mm * 10 + (*a_spec - '0');

		if ( l_n != 2 )
			return	STS$K_ERROR;
		}

	if ( *a_spec || (l_mm > 59) || (l_hh * 60 + l_mm > 14 * 60) )
		return	STS$K_ERROR;

	s_tzoff = l_sign * (l_hh * 3600 + l_mm * 60);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The zone of /TIMEZONE as text: "UTC+03:00", "the host, UTC+03:00".
**
**  FORMAL PARAMETERS:
**
**	a_buf		Output: the text, ASCIZ
**	a_bufsz		Its size, 32 is enough
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$tz_name	(
		char *		a_buf,
		unsigned	a_bufsz
			)
{
time_t		l_now = time(NULL);
struct tm	l_tm;
long		l_off = s_tzoff;

	if ( (s_tzmode == MH$K_TZ_LOCAL) && localtime_r(&l_now, &l_tm) )
		l_off = l_tm.tm_gmtoff;

	snprintf(a_buf, a_bufsz, "%sUTC%c%02ld:%02ld", (s_tzmode == MH$K_TZ_LOCAL) ? "the host, " : "",
		(l_off < 0) ? '-' : '+', labs(l_off) / 3600, (labs(l_off) / 60) % 60);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The current OpenVMS time, from the real-time clock, in the zone of
**	/TIMEZONE.
**
**  FORMAL PARAMETERS:
**
**	a_vmstime	Output: the OpenVMS time
**
**  IMPLICIT INPUTS:
**	S_TZMODE, S_TZOFF.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - CLOCK_GETTIME failed, ERRNO is set.
**--
*/
int	mh$vmstime	(
		uint64_t *	a_vmstime
			)
{
struct timespec	l_ts;
struct tm	l_tm;
int64_t		l_off = s_tzoff;

	if ( clock_gettime(CLOCK_REALTIME, &l_ts) )
		return	$LOG_FAO(STS$K_ERROR, "clock_gettime(CLOCK_REALTIME) failed, errno=!SL", errno);

	if ( (s_tzmode == MH$K_TZ_LOCAL) && localtime_r(&l_ts.tv_sec, &l_tm) )
		l_off = l_tm.tm_gmtoff;

	return	mh$unix2vms((int64_t) l_ts.tv_sec + l_off, (int64_t) l_ts.tv_nsec, a_vmstime);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The value of one hexadecimal digit, either case.
**
**  FORMAL PARAMETERS:
**
**	a_chr		The character
**	a_val		Output: 0 - 15
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - not a hexadecimal digit.
**--
*/
static	int	s_mh$xdigit	(
		char		a_chr,
		unsigned *	a_val
			)
{
	if ( (a_chr >= '0') && (a_chr <= '9') )
		*a_val = (unsigned) (a_chr - '0');
	else if ( (a_chr >= 'A') && (a_chr <= 'F') )
		*a_val = (unsigned) (a_chr - 'A' + 10);
	else if ( (a_chr >= 'a') && (a_chr <= 'f') )
		*a_val = (unsigned) (a_chr - 'a' + 10);
	else	return	STS$K_ERROR;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Convert a string of hexadecimal digits to octets, the first pair of
**	digits giving the first octet.  The string must fill the area exactly.
**
**  FORMAL PARAMETERS:
**
**	a_hex		The digits
**	a_hexlen	Their number
**	a_bin		Output area
**	a_binsz		Its size: a_hexlen must be twice that
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - wrong length or a character that is
**	not a hexadecimal digit.
**--
*/
int	mh$hex2bin	(
	const	char *		a_hex,
		unsigned	a_hexlen,
		uint8_t *	a_bin,
		unsigned	a_binsz
			)
{
unsigned	l_hi, l_lo;

	if ( a_hexlen != 2 * a_binsz )
		return	STS$K_ERROR;

	for ( unsigned i = 0; i < a_binsz; i++ )
		{
		if ( !(1 & s_mh$xdigit(a_hex[2 * i], &l_hi)) || !(1 & s_mh$xdigit(a_hex[2 * i + 1], &l_lo)) )
			return	STS$K_ERROR;

		a_bin[i] = (uint8_t) ((l_hi << 4) | l_lo);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Format a MAC address the DEC way, XX-XX-XX-XX-XX-XX.
**
**  FORMAL PARAMETERS:
**
**	a_mac		The six octets
**	a_out		Output area, ASCIZ
**	a_outsz		Its size, at least 18
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the area is too small.
**--
*/
int	mh$mac2str	(
	const	uint8_t *	a_mac,
		char *		a_out,
		unsigned	a_outsz
			)
{
char *		l_p = a_out;

	if ( a_outsz < 3 * NISCA$K_SZ_MAC )
		return	STS$K_ERROR;

	for ( int i = 0; i < NISCA$K_SZ_MAC; i++ )
		{
		*l_p++ = s_hexdigits[a_mac[i] >> 4];
		*l_p++ = s_hexdigits[a_mac[i] & 0x0F];
		*l_p++ = (i < NISCA$K_SZ_MAC - 1) ? '-' : '\0';
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The time to measure intervals by: nanoseconds of CLOCK_MONOTONIC.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	The nanoseconds.
**--
*/
uint64_t mh$now_ns	(void)
{
struct timespec	l_ts;

	clock_gettime(CLOCK_MONOTONIC, &l_ts);

	return	((uint64_t) l_ts.tv_sec * 1000000000ULL) + (uint64_t) l_ts.tv_nsec;
}
