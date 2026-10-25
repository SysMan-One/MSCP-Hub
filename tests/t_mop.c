#define	__MODULE__	"T_MOP"
#define	__IDENT__	"X01-22"
#define	__REV__		"1.22.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_mop.c
**
**  ABSTRACT:	Unit tests of the MOP server: the server stands for MHUB3
**		(65220, allocation class 44, $44$DKA100 the system disk of the
**		satellite SAT1, 65000, root SYS10) and takes in what the console
**		of SAT1 sends - REQUEST PROGRAM by multicast and to us, REQUEST
**		MEMORY LOAD - in the frames of MOP V4 and V3.  The load is
**		checked octet by octet: the section of the image, the
**		parameters right after it, the PARAMETER LOAD; and the timers,
**		the requests not ours, the parameter files that do not do.
**
**		The disk is the volume of T_VOL.H; its APB.EXE gets a header of
**		an Alpha image, its ALPHAVMSSYS.PAR the parameters of SAT1.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  25-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		25-OCT-2026	RRL	X01-22 : Initial revision.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<fcntl.h>
#include	<unistd.h>

#include	"mh_def.h"
#include	"t_vol.h"

#define	T$K_MAXTX	16
#define	T$K_SECSIZE	1948			/* The section of the image: not of whole messages	*/
#define	T$K_SIZE	1000			/* SIZE= of the satellite			*/

static	uint8_t		s_tx [T$K_MAXTX] [MH$K_MOP_FRAMESZ];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

static	const uint8_t	s_satmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x43, 0x01 };
static	const uint8_t	s_othmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x43, 0x09 };
static	const uint8_t	s_hubmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x03 };
static	const uint8_t	s_lanmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x41, 0x01 };	/* LANCP of ALPHA1	*/
static	const uint8_t	s_mopmc [NISCA$K_SZ_MAC] = { 0xAB, 0x00, 0x00, 0x01, 0x00, 0x00 };
static	const uint8_t	s_snap [8] = { 0xAA, 0xAA, 0x03, 0x08, 0x00, 0x2B, 0x60, 0x01 };
static	const char	s_auth [] = "5A3C9E1700000000";



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Count one check, report a failed one.
**
**  FORMAL PARAMETERS:
**
**	a_ok		The outcome
**	a_what		What has been checked
**	a_line		Line of the check
**
**  RETURN VALUE:
**	STS$K_SUCCESS - passed; STS$K_ERROR - failed.
**--
*/
static	int	s_t$check	(
		int		a_ok,
	const	char *		a_what,
		int		a_line
			)
{
	s_checks++;

	if ( a_ok )
		return	STS$K_SUCCESS;

	s_fails++;
	fprintf(stderr, "T_MOP:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The socket of the port, for the tests: the frames are kept here.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no room.
**--
*/
static	int	s_t$xmit	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
	if ( s_ntx >= T$K_MAXTX )
		return	STS$K_ERROR;

	memcpy(s_tx[s_ntx], a_frame, (a_len < sizeof(s_tx[0])) ? a_len : sizeof(s_tx[0]));
	s_txlen[s_ntx++] = a_len;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A message of the console of a satellite, in a frame of V3 or V4, into
**	the MOP server.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_fmt		MH$K_MOP_V3, MH$K_MOP_V4
**	a_src		The adapter of the satellite
**	a_dst		Where it is sent: the multicast of MOP, or us
**	a_msg		The message
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of MH$MOP_INPUT.
**--
*/
static	int	s_t$mop	(
		MH$PORT *	a_port,
		unsigned	a_fmt,
	const	uint8_t *	a_src,
	const	uint8_t *	a_dst,
	const	uint8_t *	a_msg,
		unsigned	a_len
			)
{
uint8_t		l_f [128] = {0};
unsigned	l_n;

	memcpy(l_f, a_dst, 6);
	memcpy(l_f + 6, a_src, 6);

	if ( a_fmt == MH$K_MOP_V3 )
		{
		l_f[12] = 0x60;
		l_f[13] = 0x01;
		nisca$put16(l_f + 14, (uint16_t) a_len);
		memcpy(l_f + 16, a_msg, a_len);
		l_n = 16 + a_len;
		}
	else	{
		l_f[12] = 0;
		l_f[13] = (uint8_t) (8 + a_len);
		memcpy(l_f + 14, s_snap, 8);
		memcpy(l_f + 22, a_msg, a_len);
		l_n = 22 + a_len;
		}

	return	mh$mop_input(a_port, a_fmt, l_f, (l_n < 60) ? 60 : l_n);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	REQUEST PROGRAM of the console: an operating system, no software ID
**	(BOOT EWA0), or the type and the ID given.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_fmt		MH$K_MOP_V3, MH$K_MOP_V4
**	a_src		The adapter of the satellite
**	a_mcast		1 - by multicast; 0 - to us
**	a_pgm		Program type
**	a_softid	Software ID: its length octet
**
**  RETURN VALUE:
**	The condition value of MH$MOP_INPUT.
**--
*/
static	int	s_t$reqpgm	(
		MH$PORT *	a_port,
		unsigned	a_fmt,
	const	uint8_t *	a_src,
		int		a_mcast,
		uint8_t		a_pgm,
		uint8_t		a_softid
			)
{
uint8_t	l_m [10] = { 8, 0x25, 1, a_pgm, a_softid, 'X', 'Y', 0, 0, 0 };

	return	s_t$mop(a_port, a_fmt, a_src, a_mcast ? s_mopmc : s_hubmac, l_m, (a_softid && (a_softid < 0x80)) ? 5 + a_softid : 5);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	REQUEST MEMORY LOAD of the console: the number it wants.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_fmt		MH$K_MOP_V3, MH$K_MOP_V4
**	a_num		The load number
**
**  RETURN VALUE:
**	The condition value of MH$MOP_INPUT.
**--
*/
static	int	s_t$reqmem	(
		MH$PORT *	a_port,
		unsigned	a_fmt,
		uint8_t		a_num
			)
{
uint8_t	l_m [3] = { 10, a_num, 0 };

	return	s_t$mop(a_port, a_fmt, s_satmac, s_hubmac, l_m, sizeof(l_m));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The MOP message of a frame we sent, in its format; the frame is
**	checked to be one to SAT1, of us.
**
**  FORMAL PARAMETERS:
**
**	a_i		The frame: its index
**	a_fmt		The format expected
**	a_len		Output: the length of the message
**
**  RETURN VALUE:
**	The message; NULL - not a frame of that format to SAT1.
**--
*/
static	const uint8_t *	s_t$msg	(
		unsigned	a_i,
		unsigned	a_fmt,
		unsigned *	a_len
			)
{
const	uint8_t	*l_f = s_tx[a_i];

	if ( (a_i >= s_ntx) || memcmp(l_f, s_satmac, 6) || memcmp(l_f + 6, s_hubmac, 6) || (s_txlen[a_i] < 60) )
		return	NULL;

	if ( a_fmt == MH$K_MOP_V3 )
		{
		if ( (l_f[12] != 0x60) || (l_f[13] != 0x01) )
			return	NULL;

		*a_len = nisca$get16(l_f + 14);

		return	l_f + 16;
		}

	if ( memcmp(l_f + 14, s_snap, 8) )
		return	NULL;

	*a_len = (unsigned) ((l_f[12] << 8) | l_f[13]) - 8;

	return	l_f + 22;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Give APB.EXE of the volume the header of an Alpha image - one block,
**	one section of T$K_SECSIZE octets from VBN 2 - and ALPHAVMSSYS.PAR the
**	parameters of SAT1, in the order SYSGEN has them, other parameters
**	between them.
**
**  FORMAL PARAMETERS:
**
**	a_vaxc		VAXCLUSTER of the file
**
**  IMPLICIT OUTPUTS:
**	S_VOL.
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$sat	(
		uint32_t	a_vaxc
			)
{
static	const struct { const char *name; uint8_t len; const char *val; } s_par [] = {
	{ "STARTUP_P1",		4, "MIN " },
	{ "SCSSYSTEMID",	4, "\xE8\xFD\0\0" },
	{ "SCSNODE",		8, "SAT1    " },
	{ "VAXCLUSTER",		4, NULL },
	{ "NISCS_CONV_BOOT",	4, "\1\0\0\0" },
	{ "VOTES",		4, "\0\0\0\0" },
	{ "NISCS_PORT_SERV",	4, "\x11\0\0\0" }
	};
uint8_t		*l_h = $BLK(400), *l_p = $BLK(300);
unsigned	l_off = 0, l_n;

	memset(l_h, 0, 512);
	nisca$put32(l_h + 0x00, 3);			/* EIHD: MAJORID of an Alpha image		*/
	nisca$put32(l_h + 0x0C, 0x120);			/* ISDOFF					*/
	nisca$put32(l_h + 0x4C, 1);			/* HDRBLKCNT					*/
	nisca$put32(l_h + 0x120 + 0x00, 1);		/* EISD: MAJORID, MINORID, EISDSIZE		*/
	nisca$put32(l_h + 0x120 + 0x04, 1);
	nisca$put32(l_h + 0x120 + 0x08, 36);
	nisca$put32(l_h + 0x120 + 0x0C, T$K_SECSIZE);	/* SECSIZE					*/
	nisca$put32(l_h + 0x120 + 0x1C, 2);		/* VBN						*/

	memset(l_p, 0, 1024);

	for ( unsigned i = 0; i < $ARRSZ(s_par); i++ )
		{
		l_n = (unsigned) strlen(s_par[i].name);
		l_p[l_off] = (uint8_t) l_n;
		memcpy(l_p + l_off + 1, s_par[i].name, l_n);

		if ( s_par[i].val )
			memcpy(l_p + l_off + 16, s_par[i].val, s_par[i].len);
		else	nisca$put32(l_p + l_off + 16, a_vaxc);

		l_off += 16 + s_par[i].len;
		}
}



int	main	(void)
{
static	MH$PORT	s_port;
static	MH$CFG	s_cfg;
MH$PORT		*l_port = &s_port;
MH$MOPSAT	*l_sat;
const	uint8_t	*l_m, *l_img;
unsigned	l_len, l_off, l_ok;
char		l_spec [96], l_img_name [] = "t_mop_XXXXXX", l_img2 [] = "t_mop_XXXXXX";
int		l_fd;
uint8_t		l_exp [128];
unsigned	l_elen;

	mh$inimsg();

	s_t$mkvol();
	s_t$sat(2);

	l_fd = mkstemp(l_img_name);
	$CHECK((l_fd >= 0) && (write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol)), "the volume of SAT1 is written");
	close(l_fd);

	/*
	**  MHUB3: $44$DKA100 the volume
	*/
	memcpy($ASCPTR(&s_cfg.node), "MHUB3", 5);
	$ASCLEN(&s_cfg.node) = 5;
	s_cfg.sysid	= 65220;
	s_cfg.group	= 4242;
	s_cfg.alloclass	= 44;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img_name);
	$ASCLEN(&s_cfg.unit[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[0]), l_spec, strlen(l_spec));
	l_fd = mkstemp(l_img2);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 1024 * 1024), "... and a read-only disk of 1 MB");
	close(l_fd);
	snprintf(l_spec, sizeof(l_spec), "dka200,%s,RO", l_img2);
	$ASCLEN(&s_cfg.unit[1]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[1]), l_spec, strlen(l_spec));

	l_port->sd	= -1;
	l_port->xmit	= s_t$xmit;
	l_port->group	= 4242;
	l_port->sysid	= 65220;
	l_port->tick	= 1000;
	memcpy(l_port->mac, s_hubmac, NISCA$K_SZ_MAC);
	memcpy(l_port->ifname, "mh3", 4);
	mh$hex2bin(s_auth, 16, l_port->auth, MH$K_SZ_AUTH);

	$CHECK((1 & mh$mscp_init(l_port, &s_cfg)) && (l_port->nunit == 2), "MHUB3 serves $44$DKA100 and $44$DKA200");

	/*
	**  The satellites of the configuration: what is not right is refused
	*/
	{
	static	const char	*s_bad [] = {
		"08-00-2B-E5-43-01,DKA100",			/* No root					*/
		"08-00-2B-E5-43,DKA100,SYS10",			/* Not a MAC					*/
		"AB-00-00-01-00-00,DKA100,SYS10",		/* A multicast					*/
		"08-00-2B-E5-43-01,DKA300,SYS10",		/* Not a unit of ours				*/
		"08-00-2B-E5-43-01,DKA200,SYS10",		/* Read only					*/
		"08-00-2B-E5-43-01,DKA100,SYS 10",		/* A root of a blank				*/
		"08-00-2B-E5-43-01,DKA100,SYS10,SIZE=100",	/* Too small					*/
		"08-00-2B-E5-43-01,DKA100,SYS10,SIZE=1500",	/* Too big					*/
		"08-00-2B-E5-43-01,DKA100,SYS10,FILE=X",	/* Not SIZE					*/
		"08-00-2B-E5-43-01,DKA100,SYS10,SIZE=1000,X"	/* One field too many				*/
		};

	for ( unsigned i = 0; i < $ARRSZ(s_bad); i++ )
		{
		memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
		$ASCLEN(&s_cfg.sat[0]) = (unsigned char) strlen(s_bad[i]);
		memcpy($ASCPTR(&s_cfg.sat[0]), s_bad[i], strlen(s_bad[i]));
		$CHECK(!(1 & mh$mop_init(l_port, &s_cfg)), s_bad[i]);
		}

	memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
	snprintf(l_spec, sizeof(l_spec), "08-00-2B-E5-43-01,DKA100,SYS10");
	$ASCLEN(&s_cfg.sat[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.sat[0]), l_spec, strlen(l_spec));
	$ASCLEN(&s_cfg.sat[1]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.sat[1]), l_spec, strlen(l_spec));
	$CHECK(!(1 & mh$mop_init(l_port, &s_cfg)), "two satellites of one adapter are refused");
	}

	memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
	snprintf(l_spec, sizeof(l_spec), "08:00:2b:e5:43:01,dka100,<sys10.>,SIZE=%u", T$K_SIZE);
	$ASCLEN(&s_cfg.sat[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.sat[0]), l_spec, strlen(l_spec));

	l_sat = &l_port->sats[0];
	$CHECK((1 & mh$mop_init(l_port, &s_cfg)) && (l_port->nsat == 1), "SAT1: 08:00:2b:e5:43:01, dka100, <sys10.>");
	$CHECK(!memcmp(l_sat->mac, s_satmac, 6) && !l_sat->unit && !strcmp(l_sat->root, "SYS10") && (l_sat->size == T$K_SIZE),
		"... is 08-00-2B-E5-43-01, DKA100, SYS10, SIZE 1000");

	/*
	**  REQUEST PROGRAM by multicast, V4: ASSISTANCE VOLUNTEER to SAT1
	*/
	s_ntx = 0;
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 1, 2, 0);
	l_m = s_t$msg(0, MH$K_MOP_V4, &l_len);
	$CHECK((s_ntx == 1) && l_m && (l_len == 1) && (l_m[0] == 3), "multicast REQUEST PROGRAM, V4: ASSISTANCE VOLUNTEER, V4, to SAT1");
	$CHECK((s_txlen[0] == 60) && (s_tx[0][12] == 0) && (s_tx[0][13] == 9), "... 802.3 length 9, padded to 60");
	$CHECK((l_sat->state == MH$K_MOP_VOLUNTEER) && (l_sat->imglen == T$K_SECSIZE) && !strcmp(l_sat->name, "SAT1"),
		"... the image is ready, 1948 octets; the satellite is SAT1 of its PAR");

	/*
	**  ... to another MOP server, which the console chose: not ours
	*/
	{
	uint8_t	l_m [5] = { 8, 0x25, 1, 2, 0 };

	s_ntx = 0;
	s_t$mop(l_port, MH$K_MOP_V4, s_satmac, s_lanmac, l_m, sizeof(l_m));
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_VOLUNTEER), "REQUEST PROGRAM to another MOP server: no answer, nothing changed");
	}

	/*
	**  ... to us: MEMORY LOAD 0 at once, at 0, 1000 octets of VBN 2 on
	*/
	s_ntx = 0;
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 0, 2, 0);
	l_m = s_t$msg(0, MH$K_MOP_V4, &l_len);
	$CHECK((s_ntx == 1) && l_m && (l_len == 6 + T$K_SIZE) && (l_m[0] == 2) && (l_m[1] == 0) && !nisca$get32(l_m + 2),
		"REQUEST PROGRAM to us: MEMORY LOAD 0 at address 0, 1000 octets");

	l_ok = 1;

	for ( unsigned i = 0; i < T$K_SIZE; i++ )
		l_ok &= (l_m[6 + i] == ((i < 512) ? 0xA1 : 0xA2));

	$CHECK(l_ok, "... VBN 2 and 3 of APB.EXE: the header block is skipped");

	/*
	**  The console asks for 0 again: the same frame
	*/
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 0);
	$CHECK((s_ntx == 1) && (s_txlen[0] == l_sat->framelen) && !memcmp(s_tx[0], l_sat->frame, l_sat->framelen) && (l_sat->nrexmt == 1),
		"REQUEST MEMORY LOAD 0 again: MEMORY LOAD 0 again");

	/*
	**  1: the rest of the section, from 1000
	*/
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 1);
	l_m = s_t$msg(0, MH$K_MOP_V4, &l_len);
	$CHECK(l_m && (l_len == 6 + T$K_SECSIZE - T$K_SIZE) && (l_m[1] == 1) && (nisca$get32(l_m + 2) == T$K_SIZE),
		"REQUEST MEMORY LOAD 1: MEMORY LOAD 1 at 1000, 948 octets");

	l_img = l_m + 6;
	l_ok = 1;

	for ( unsigned i = T$K_SIZE; i < T$K_SECSIZE; i++ )
		l_ok &= (l_img[i - T$K_SIZE] == ((i < 1024) ? 0xA2 : ((i < 1536) ? 0xA3 : 0xA4)));

	$CHECK(l_ok, "... VBN 3 to 5 of APB.EXE, across its two extents");

	/*
	**  A REQUEST MEMORY LOAD to another MOP server, in the middle of ours
	*/
	{
	uint8_t	l_m [3] = { 10, 2, 0 };

	s_ntx = 0;
	s_t$mop(l_port, MH$K_MOP_V4, s_satmac, s_lanmac, l_m, sizeof(l_m));
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_IMAGE) && (l_sat->loadnum == 1) && (l_sat->offset == T$K_SIZE),
		"REQUEST MEMORY LOAD 2 to another MOP server: ignored");
	}

	/*
	**  2: the parameters, at the address the section ends at - in the
	**  order of the file for those of the PAR
	*/
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 2);
	l_m = s_t$msg(0, MH$K_MOP_V4, &l_len);
	$CHECK(l_m && (l_m[0] == 2) && (l_m[1] == 2) && (nisca$get32(l_m + 2) == T$K_SECSIZE), "MEMORY LOAD 2 at 1948: the parameters");

	l_elen = 0;
#define	$ITEM(code, data, len)	do { nisca$put16(l_exp + l_elen, code); nisca$put16(l_exp + l_elen + 2, (uint16_t) ((len) + 4)); \
				memcpy(l_exp + l_elen + 4, data, len); l_elen += (len) + 4; } while ( 0 )

	$ITEM(2, "<SYS10.>", 8);
	$ITEM(8, "MHUB3", 5);
	$ITEM(0x0B, "\x0C_$44$DKA100:\0\0\0", 16);
	$ITEM(1, "\x2C\0\0\0\x64\0\x61\x11", 8);		/* Class 44, unit 100, D K A			*/
	$ITEM(9, "\xC4\xFE\0\0\0\0", 6);			/* 65220					*/
	$ITEM(5, "\xE8\xFD\0\0", 4);				/* The order of the PAR: SCSSYSTEMID first	*/
	$ITEM(4, "SAT1    ", 8);
	$ITEM(7, "\1", 1);
	$ITEM(6, "\x11\0\0\0", 4);
	$ITEM(3, "\x92\x10\0\0", 4);				/* 4242						*/
	$ITEM(0x0A, "\x5A\x3C\x9E\x17\0\0\0\0", 8);
	memset(l_exp + l_elen, 0, 4);
	l_elen += 4;

	$CHECK(l_m && (l_len == 6 + l_elen) && !memcmp(l_m + 6, l_exp, l_elen), "... the parameters of SAT1, item by item");

	/*
	**  3: the PARAMETER LOAD WITH TRANSFER ADDRESS
	*/
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 3);
	l_m = s_t$msg(0, MH$K_MOP_V4, &l_len);
	$CHECK(l_m && (l_m[0] == 20) && (l_m[1] == 3), "REQUEST MEMORY LOAD 3: PARAMETER LOAD WITH TRANSFER ADDRESS 3");

	l_off = 2;
	$CHECK(l_m && (l_m[l_off] == 1) && (l_m[l_off + 1] == 4) && !memcmp(l_m + l_off + 2, "SAT1", 4), "... client name SAT1");
	l_off += 6;
	$CHECK(l_m && (l_m[l_off] == 2) && (l_m[l_off + 1] == 2) && (nisca$get16(l_m + l_off + 2) == 65000), "... client address 65000");
	l_off += 4;
	$CHECK(l_m && (l_m[l_off] == 3) && (l_m[l_off + 1] == 5) && !memcmp(l_m + l_off + 2, "MHUB3", 5), "... host name MHUB3");
	l_off += 7;
	$CHECK(l_m && (l_m[l_off] == 4) && (l_m[l_off + 1] == 2) && (nisca$get16(l_m + l_off + 2) == 65220), "... host address 65220");
	l_off += 4;
	$CHECK(l_m && (l_m[l_off] == 5) && (l_m[l_off + 1] == 10) && (l_m[l_off + 2] == 20), "... the time, of the 21st century");
	l_off += 12;
	$CHECK(l_m && !l_m[l_off] && !nisca$get32(l_m + l_off + 1) && (l_len == l_off + 5), "... the end, transfer address 0");

	/*
	**  4: the satellite is loaded
	*/
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 4);
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_IDLE) && (l_sat->loads == 1) && !l_sat->img, "REQUEST MEMORY LOAD 4: loaded, nothing sent");

	/*
	**  V3, straight to us (a console that knows us): the load from 0 in
	**  frames of V3; unanswered, it goes again every second; silent for
	**  60 seconds, it is ended
	*/
	s_ntx = 0;
	s_t$reqpgm(l_port, MH$K_MOP_V3, s_satmac, 0, 2, 0);
	l_m = s_t$msg(0, MH$K_MOP_V3, &l_len);
	$CHECK(l_m && (l_len == 6 + T$K_SIZE) && (l_m[0] == 2) && !l_m[1] && (s_txlen[0] == 16 + 6 + T$K_SIZE),
		"V3, to us: MEMORY LOAD 0 by 60-01 with the length word");

	s_ntx = 0;
	l_port->tick += MH$K_MOP_RETRY - 1;
	mh$mop_tick(l_port);
	$CHECK(!s_ntx, "... nothing again before a second");
	l_port->tick++;
	mh$mop_tick(l_port);
	$CHECK((s_ntx == 1) && !memcmp(s_tx[0], l_sat->frame, l_sat->framelen), "... a second later it goes again");

	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V4, 1);
	$CHECK(!s_ntx && (l_sat->offset == 0), "... a REQUEST MEMORY LOAD of V4 is not of this load");

	l_port->tick += MH$K_MOP_TMO;
	mh$mop_tick(l_port);
	$CHECK((l_sat->state == MH$K_MOP_IDLE) && (l_sat->fails == 1), "... silent for 60 s: the load is ended, a failure");

	/*
	**  A number out of order ends the load
	*/
	s_t$reqpgm(l_port, MH$K_MOP_V3, s_satmac, 0, 2, 0);
	s_ntx = 0;
	s_t$reqmem(l_port, MH$K_MOP_V3, 5);
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_IDLE) && (l_sat->fails == 2), "REQUEST MEMORY LOAD 5 after 0: the load is ended");

	/*
	**  Not ours: another adapter, a file asked for by name, a secondary
	**  loader; a REQUEST MEMORY LOAD out of a load
	*/
	s_ntx = 0;
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_othmac, 1, 2, 0);
	$CHECK(!s_ntx && (l_port->mop_other == 1), "an adapter not of a satellite: no answer");
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 1, 2, 2);
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_IDLE), "BOOT -FILE (software ID XY): no answer");
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 1, 0, 0);
	$CHECK(!s_ntx, "a secondary loader: no answer");
	s_t$reqmem(l_port, MH$K_MOP_V4, 1);
	$CHECK(!s_ntx, "REQUEST MEMORY LOAD with no load: no answer");

	/*
	**  A parameter file of no cluster: no volunteer
	*/
	s_t$sat(0);
	l_fd = open(l_img_name, O_WRONLY);
	$CHECK((l_fd >= 0) && (pwrite(l_fd, s_vol, sizeof(s_vol), 0) == (ssize_t) sizeof(s_vol)), "VAXCLUSTER 0 in the PAR of SAT1");
	close(l_fd);
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 1, 2, 0);
	$CHECK(!s_ntx && (l_sat->state == MH$K_MOP_IDLE) && (l_sat->fails == 3), "... a multicast REQUEST PROGRAM: no volunteer, a failure");

	/*
	**  No such root
	*/
	memcpy(l_sat->root, "SYS11", 6);
	s_t$reqpgm(l_port, MH$K_MOP_V4, s_satmac, 1, 2, 0);
	$CHECK(!s_ntx && (l_sat->fails == 4), "a root that is not there: no volunteer, a failure");

	unlink(l_img_name);
	unlink(l_img2);

	printf("T_MOP: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
