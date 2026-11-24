#define	__MODULE__	"T_TFTP"
#define	__IDENT__	"X.01-25"
#define	__REV__		"1.25.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_tftp.c
**
**  ABSTRACT:	Unit tests of the BOOTP and TFTP server of the I64 satellites:
**		the server stands for MHUB3 (192.168.42.5/24, gateway .1,
**		$44$DKA100 the system disk of the satellite I64S1,
**		08-00-2B-E5-44-10, 192.168.42.10, root SYS10) and takes in
**		what the firmware and the loader of I64S1 send - ARP, BOOTP and
**		DHCP, TFTP read requests with and without options, the ACKs and
**		the ERRORs - and what others send.  The answers are checked
**		octet by octet: the headers and their checksums, the boot file,
**		the options, the blocks of a file, the records of a file of
**		variable length as a stream, the access of the files, the
**		block number past 65535, the packets sent again.
**
**		The disk is the volume of T_VOL.H with the files of an I64 root
**		put in [SYS10.SYSEXE].
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  24-NOV-2026
**
**  MODIFICATION HISTORY:
**
**		24-NOV-2026	CLD	X.01-25 : Initial revision.
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

#define	T$K_MAXTX	8
#define	T$K_HUBIP	0xC0A82A05U		/* 192.168.42.5					*/
#define	T$K_SATIP	0xC0A82A0AU		/* 192.168.42.10				*/
#define	T$K_GWIP	0xC0A82A01U		/* 192.168.42.1					*/
#define	T$K_CPORT	1234			/* The port of the client			*/
#define	T$K_WRAPBLK	8			/* WRAP.EXE: blocks of 8 octets ...		*/
#define	T$K_WRAPN	65540			/* ... so many, past 65535			*/
#define	T$K_WRAPLBN	2000

static	uint8_t		s_tx [T$K_MAXTX] [MH$K_SZ_FRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

static	const uint8_t	s_satmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x44, 0x10 };
static	const uint8_t	s_alphamac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x43, 0x01 };	/* A satellite of MOP	*/
static	const uint8_t	s_othmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x44, 0x99 };
static	const uint8_t	s_hubmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x03 };
static	const uint8_t	s_bcast [NISCA$K_SZ_MAC] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static	const char	s_auth [] = "5A3C9E1700000000";



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
	fprintf(stderr, "T_TFTP:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



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
**  Big-endian octets and the Internet checksum, the test's own
*/
static	uint16_t	s_g16	(const uint8_t *a_p)		{ return (uint16_t) ((a_p[0] << 8) | a_p[1]); }
static	uint32_t	s_g32	(const uint8_t *a_p)		{ return ((uint32_t) s_g16(a_p) << 16) | s_g16(a_p + 2); }
static	void		s_p16	(uint8_t *a_p, uint16_t a_v)	{ a_p[0] = (uint8_t) (a_v >> 8); a_p[1] = (uint8_t) a_v; }
static	void		s_p32	(uint8_t *a_p, uint32_t a_v)	{ s_p16(a_p, (uint16_t) (a_v >> 16)); s_p16(a_p + 2, (uint16_t) a_v); }

static	uint16_t	s_t$sum	(
		uint32_t	a_sum,
	const	uint8_t *	a_p,
		unsigned	a_len
			)
{
	for ( ; a_len > 1; a_p += 2, a_len -= 2 )
		a_sum += s_g16(a_p);

	if ( a_len )
		a_sum += (uint32_t) a_p[0] << 8;

	while ( a_sum >> 16 )
		a_sum = (a_sum & 0xFFFF) + (a_sum >> 16);

	return	(uint16_t) ~a_sum;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A UDP datagram of a client into the server: Ethernet, IPv4 (its
**	checksum right), UDP (no checksum, as a client may send).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_src		The adapter of the client
**	a_dst		The adapter it is sent to
**	a_sip		The source address
**	a_dip		The destination address
**	a_sport		The source port
**	a_dport		The destination port
**	a_data		The data
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of MH$TFTP_INPUT.
**--
*/
static	int	s_t$udp	(
		MH$PORT *	a_port,
	const	uint8_t *	a_src,
	const	uint8_t *	a_dst,
		uint32_t	a_sip,
		uint32_t	a_dip,
		uint16_t	a_sport,
		uint16_t	a_dport,
	const	void *		a_data,
		unsigned	a_len
			)
{
static	uint8_t	s_f [2048];
uint8_t		*l_ip = s_f + 14, *l_u = l_ip + 20;
unsigned	l_n = 14 + 20 + 8 + a_len;

	memset(s_f, 0, sizeof(s_f));
	memcpy(s_f, a_dst, 6);
	memcpy(s_f + 6, a_src, 6);
	s_p16(s_f + 12, 0x0800);
	l_ip[0] = 0x45;
	s_p16(l_ip + 2, (uint16_t) (20 + 8 + a_len));
	l_ip[8] = 64;
	l_ip[9] = 17;
	s_p32(l_ip + 12, a_sip);
	s_p32(l_ip + 16, a_dip);
	s_p16(l_ip + 10, s_t$sum(0, l_ip, 20));
	s_p16(l_u, a_sport);
	s_p16(l_u + 2, a_dport);
	s_p16(l_u + 4, (uint16_t) (8 + a_len));
	memcpy(l_u + 8, a_data, a_len);

	return	mh$tftp_input(a_port, MH$K_IP_IP, s_f, (l_n < 60) ? 60 : l_n);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A frame the server has sent, a UDP datagram: its Ethernet and IPv4
**	headers and both checksums checked; the data and the ports given.
**
**  FORMAL PARAMETERS:
**
**	a_i		Which frame
**	a_dmac		The adapter it must go to
**	a_dip		The address it must go to
**	a_sport		Output: its source port
**	a_dport		Output: its destination port
**	a_len		Output: the length of the data
**
**  RETURN VALUE:
**	The data; NULL - no such frame, or it is not right.
**--
*/
static	const uint8_t *	s_t$reply	(
		unsigned	a_i,
	const	uint8_t *	a_dmac,
		uint32_t	a_dip,
		uint16_t *	a_sport,
		uint16_t *	a_dport,
		unsigned *	a_len
			)
{
const	uint8_t	*l_f = s_tx[a_i], *l_ip = l_f + 14, *l_u = l_ip + 20;
unsigned	l_ulen;
uint32_t	l_ps;

	if ( (a_i >= s_ntx) || (s_txlen[a_i] < 60) || memcmp(l_f, a_dmac, 6) || memcmp(l_f + 6, s_hubmac, 6) || (s_g16(l_f + 12) != 0x0800)
		|| (l_ip[0] != 0x45) || (l_ip[9] != 17) || s_t$sum(0, l_ip, 20) || (s_g32(l_ip + 12) != T$K_HUBIP) || (s_g32(l_ip + 16) != a_dip) )
		return	NULL;

	l_ulen	= s_g16(l_u + 4);
	l_ps	= (T$K_HUBIP >> 16) + (T$K_HUBIP & 0xFFFF) + (a_dip >> 16) + (a_dip & 0xFFFF) + 17 + l_ulen;

	if ( (s_g16(l_ip + 2) != 20 + l_ulen) || (14 + 20 + l_ulen > s_txlen[a_i]) || !s_g16(l_u + 6) || s_t$sum(l_ps, l_u, l_ulen) )
		return	NULL;

	*a_sport = s_g16(l_u);
	*a_dport = s_g16(l_u + 2);
	*a_len	 = l_ulen - 8;

	return	l_u + 8;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A TFTP request of I64S1 from its port, the options given as pairs.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_op		TFTP_RRQ, TFTP_WRQ
**	a_name		The file
**	a_opts		name, value, ... NULL
**
**  RETURN VALUE:
**	The condition value of MH$TFTP_INPUT.
**--
*/
static	int	s_t$rq	(
		MH$PORT *	a_port,
		unsigned	a_op,
	const	char *		a_name,
	const	char **		a_opts
			)
{
uint8_t		l_p [512];
unsigned	l_n = 2;

	s_p16(l_p, (uint16_t) a_op);
	l_n += (unsigned) sprintf((char *) l_p + l_n, "%s", a_name) + 1;
	l_n += (unsigned) sprintf((char *) l_p + l_n, "octet") + 1;

	for ( ; a_opts && *a_opts; a_opts++ )
		l_n += (unsigned) sprintf((char *) l_p + l_n, "%s", *a_opts) + 1;

	s_ntx = 0;

	return	s_t$udp(a_port, s_satmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT, 69, l_p, l_n);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	An ACK, or an ERROR, of I64S1 to the port of a transfer.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sport		The port of the transfer
**	a_op		TFTP_ACK, TFTP_ERROR
**	a_num		The block, the error code
**
**  RETURN VALUE:
**	The condition value of MH$TFTP_INPUT.
**--
*/
static	int	s_t$ack	(
		MH$PORT *	a_port,
		uint16_t	a_sport,
		unsigned	a_op,
		unsigned	a_num
			)
{
uint8_t	l_p [8] = {0};

	s_p16(l_p, (uint16_t) a_op);
	s_p16(l_p + 2, (uint16_t) a_num);
	s_ntx = 0;

	return	s_t$udp(a_port, s_satmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT, a_sport, l_p, (a_op == 5) ? 5 : 4);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read a file over TFTP as a client does: the request, the OACK (then
**	ACK 0), every DATA acknowledged, its octets kept.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_name		The file
**	a_opts		The options of the request
**	a_out		Output: the octets
**	a_outsz		... at most
**	a_len		Output: how many
**	a_oack		Output: the OACK, if one came; NULL - not wanted
**	a_blocks	Output: the DATA packets taken
**
**  RETURN VALUE:
**	1 - the file is read: the last DATA was shorter than the block;
**	0 - an ERROR, or a packet not right.
**--
*/
static	int	s_t$get	(
		MH$PORT *	a_port,
	const	char *		a_name,
	const	char **		a_opts,
		uint8_t *	a_out,
		unsigned	a_outsz,
		unsigned *	a_len,
		char *		a_oack,
		unsigned *	a_blocks
			)
{
const	uint8_t	*l_d;
uint16_t	l_sp, l_dp, l_port;
unsigned	l_n, l_blk = 512, l_want = 1;

	*a_len = *a_blocks = 0;
	s_t$rq(a_port, 1, a_name, a_opts);

	if ( !(l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n)) || (s_ntx != 1) || (l_dp != T$K_CPORT) || (l_sp < 49152) )
		return	0;

	l_port = l_sp;

	if ( s_g16(l_d) == 6 )
		{
		if ( a_oack )
			{
			memcpy(a_oack, l_d + 2, l_n - 2);
			a_oack[l_n - 2] = '\0';
			}

		for ( const char *l_o = (const char *) l_d + 2; l_o < (const char *) l_d + l_n; l_o += strlen(l_o) + 1 )
			if ( !strcmp(l_o, "blksize") )
				l_blk = (unsigned) atoi(l_o + 8);

		s_t$ack(a_port, l_port, 4, 0);

		if ( !(l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n)) || (l_sp != l_port) )
			return	0;
		}

	for ( ;; )
		{
		if ( (s_g16(l_d) != 3) || (s_g16(l_d + 2) != (uint16_t) l_want) || (l_n - 4 > l_blk) || (*a_len + l_n - 4 > a_outsz) )
			return	0;

		memcpy(a_out + *a_len, l_d + 4, l_n - 4);
		*a_len += l_n - 4;
		(*a_blocks)++;
		s_t$ack(a_port, l_port, 4, l_want);

		if ( l_n - 4 < l_blk )
			return	!s_ntx;

		l_want++;

		if ( !(l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n)) || (s_ntx != 1) || (l_sp != l_port) )
			return	0;
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A file of [SYS10.SYSEXE]: its header (FID n,1, its first block at
**	LBN), its record attributes, its protection, an ACE of an
**	identifier, and its directory record.
**
**  FORMAL PARAMETERS:
**
**	a_dir		The directory block; a_off: where the record goes, output: after it
**	a_name		NAME.TYPE
**	a_num		File number
**	a_lbn		The first block
**	a_blocks	Blocks
**	a_size		Octets to the end of file
**	a_rtype		FAT record type
**	a_rattrib	... record attributes
**	a_vfc		... size of the VFC area
**	a_prot		FILEPROT
**	a_ace		The identifier of an ACE of read, 0 - no ACL
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$file	(
		uint8_t *	a_dir,
		unsigned *	a_off,
	const	char *		a_name,
		uint32_t	a_num,
		uint32_t	a_lbn,
		uint32_t	a_blocks,
		uint32_t	a_size,
		uint8_t		a_rtype,
		uint8_t		a_rattrib,
		uint8_t		a_vfc,
		uint16_t	a_prot,
		uint32_t	a_ace
			)
{
uint16_t	l_map [4];
uint8_t		*l_h = $BLK(T$K_HDRLBN + a_num - 1);
unsigned	l_n = 0;

	for ( uint32_t l_b = 0; l_b < a_blocks; l_b += 256 )
		{
		s_t$fm1(l_map + l_n, a_lbn + l_b, ((a_blocks - l_b) < 256) ? a_blocks - l_b : 256);
		l_n += 2;

		if ( l_n >= $ARRSZ(l_map) )
			break;
		}

	if ( a_blocks > 512 )				/* WRAP.EXE: one pointer of format 2		*/
		{
		l_map[0] = (uint16_t) (0x8000 | (a_blocks - 1));
		l_map[1] = (uint16_t) a_lbn;
		l_map[2] = (uint16_t) (a_lbn >> 16);
		l_n = 3;
		}

	s_t$hdr(a_num, 1, 0, 0, a_size / 512 + 1, (uint16_t) (a_size % 512), l_map, l_n);
	l_h[0x14]	= a_rtype;
	l_h[0x15]	= a_rattrib;
	l_h[0x23]	= a_vfc;
	nisca$put16(l_h + 0x40, a_prot);

	if ( a_ace )
		{
		l_h[0x02] = 0x70;				/* ACOFFSET, RSOFFSET: words			*/
		l_h[0x03] = 0x78;
		l_h[0xE0] = 12;					/* An ACE of an identifier, read		*/
		l_h[0xE1] = 1;
		nisca$put32(l_h + 0xE4, 0x09);
		nisca$put32(l_h + 0xE8, a_ace);
		}

	s_t$cksum(l_h, 0x1FE);
	s_t$dirent(a_dir, a_off, 0, a_name, a_num, 1, 0, 0);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The files of an I64 root in [SYS10.SYSEXE], next to ALPHAVMSSYS.PAR
**	of T_VOL.H: a file of 3 blocks and 100 octets, one of 2 whole blocks,
**	a parameter file of no WORLD access but the ACE of
**	VMS$SATELLITE_ACCESS, one of no access, files of variable length -
**	one whose last record crosses a block, one of NOSPAN, one of VFC -
**	and one of 65540 blocks of 8 octets.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$i64	(void)
{
uint8_t		*l_d = $BLK(204), *l_b;
unsigned	l_off = 0;

	memset(l_d, 0, 512);
	s_t$dirent(l_d, &l_off, 0, "ALPHAVMSSYS.PAR", 24, 1, 0, 0);

	s_t$file(l_d, &l_off, "BIG.EXE", 40, 1100, 4, 3 * 512 + 100, 1, 0, 0, 0xEE00, 0);
	for ( unsigned i = 0; i < 4 * 512; i++ )
		$BLK(1100)[i] = (uint8_t) (i * 7 + 1);

	s_t$file(l_d, &l_off, "WHOLE.EXE", 41, 1110, 2, 1024, 1, 0, 0, 0xEE00, 0);
	for ( unsigned i = 0; i < 1024; i++ )
		$BLK(1110)[i] = (uint8_t) (i * 3);

	s_t$file(l_d, &l_off, "IA64VMSSYS.PAR", 42, 1120, 1, 512, 1, 0, 0, 0xFF00, 0x8000000AU);
	memset($BLK(1120), 0x5A, 512);
	memcpy($BLK(1120) + 100, "\007SCSNODE\0\0\0\0\0\0\0\0I64S1   ", 24);

	s_t$file(l_d, &l_off, "SYSUAF.DAT", 43, 1130, 1, 512, 1, 0, 0, 0xFF00, 0x8000000BU);
	memset($BLK(1130), 0x55, 512);

	/*
	**  TEXT.DAT, VAR: ABC, an empty one, HELLO, and one across VBN 1 and 2
	*/
	s_t$file(l_d, &l_off, "TEXT.DAT", 44, 1140, 2, 512 + 14, 2, 0x02, 0, 0xEE00, 0);
	l_b = $BLK(1140);
	nisca$put16(l_b, 3);				/* ABC and its pad				*/
	memcpy(l_b + 2, "ABC", 3);
	nisca$put16(l_b + 6, 0);			/* An empty record				*/
	nisca$put16(l_b + 8, 5);			/* HELLO and its pad				*/
	memcpy(l_b + 10, "HELLO", 5);
	nisca$put16(l_b + 16, 472);			/* 472 octets of zeros				*/
	nisca$put16(l_b + 490, 34);			/* ... and one from octet 490 to 525		*/
	memcpy(l_b + 492, "A RECORD ACROSS THE END OF A BLOCK", 34);

	/*
	**  NOSPAN.DAT: ONE in VBN 1 then FFFF, TWO in VBN 2
	*/
	s_t$file(l_d, &l_off, "NOSPAN.DAT", 45, 1150, 2, 512 + 6, 2, 0x08 | 0x02, 0, 0xEE00, 0);
	l_b = $BLK(1150);
	nisca$put16(l_b, 3);
	memcpy(l_b + 2, "ONE", 3);
	nisca$put16(l_b + 6, 0xFFFF);
	nisca$put16(l_b + 512, 3);
	memcpy(l_b + 514, "TWO", 3);

	/*
	**  VFC.DAT: a record of 2 octets of control and XYZ
	*/
	s_t$file(l_d, &l_off, "VFC.DAT", 46, 1160, 1, 8, 3, 0x02, 2, 0xEE00, 0);
	l_b = $BLK(1160);
	nisca$put16(l_b, 5);
	l_b[2] = 1;
	l_b[3] = 0x8D;
	memcpy(l_b + 4, "XYZ", 3);

	s_t$file(l_d, &l_off, "WRAP.EXE", 47, T$K_WRAPLBN, (T$K_WRAPN * T$K_WRAPBLK + 511) / 512, T$K_WRAPN * T$K_WRAPBLK, 1, 0, 0,
		0xEE00, 0);
	for ( unsigned i = 0; i < T$K_WRAPN * T$K_WRAPBLK; i++ )
		$BLK(T$K_WRAPLBN)[i] = (uint8_t) (i / T$K_WRAPBLK);
}



static	void	s_t$cfg	(
		ASC *		a_asc,
	const	char *		a_val
			)
{
	$ASCLEN(a_asc) = (unsigned char) strlen(a_val);
	memcpy($ASCPTR(a_asc), a_val, strlen(a_val));
}



int	main	(void)
{
static	MH$PORT	s_port;
static	MH$CFG	s_cfg;
static	uint8_t	s_out [T$K_WRAPN * T$K_WRAPBLK + 16];
MH$PORT		*l_port = &s_port;
MH$MOPSAT	*l_sat;
const	uint8_t	*l_d;
uint8_t		l_b [600], l_arp [60];
uint16_t	l_sp, l_dp;
uint16_t	l_xport;
uint32_t	l_fails;
unsigned	l_n, l_len, l_blocks;
char		l_spec [96], l_img_name [] = "t_tftp_XXXXXX", l_oack [80];
int		l_fd, l_ok;

	mh$inimsg();

	s_t$mkvol();
	s_t$i64();

	l_fd = mkstemp(l_img_name);
	$CHECK((l_fd >= 0) && (write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol)), "the volume of I64S1 is written");
	close(l_fd);

	s_t$cfg(&s_cfg.node, "MHUB3");
	s_cfg.sysid	= 65220;
	s_cfg.group	= 4242;
	s_cfg.alloclass	= 44;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img_name);
	s_t$cfg(&s_cfg.unit[0], l_spec);

	l_port->sd	= -1;
	l_port->xmit	= s_t$xmit;
	l_port->group	= 4242;
	l_port->sysid	= 65220;
	l_port->tick	= 1000;
	l_port->mtu	= 1500;
	memcpy(l_port->mac, s_hubmac, NISCA$K_SZ_MAC);
	memcpy(l_port->ifname, "mh3", 4);
	mh$hex2bin(s_auth, 16, l_port->auth, MH$K_SZ_AUTH);

	$CHECK((1 & mh$mscp_init(l_port, &s_cfg)) && (l_port->nunit == 1), "MHUB3 serves $44$DKA100");

	/*
	**  IP= of /SATELLITEn: what is not an address is refused
	*/
	{
	static	const char	*s_bad [] = {
		"08-00-2B-E5-44-10,DKA100,SYS10,IP=192.168.42",		/* Three octets				*/
		"08-00-2B-E5-44-10,DKA100,SYS10,IP=192.168.42.256",	/* Out of range				*/
		"08-00-2B-E5-44-10,DKA100,SYS10,IP=192.168.42.10x",	/* Not a number				*/
		"08-00-2B-E5-44-10,DKA100,SYS10,IP= 192.168.42.10",	/* A blank				*/
		"08-00-2B-E5-44-10,DKA100,SYS10,IP=0.0.0.0",		/* None					*/
		"08-00-2B-E5-44-10,DKA100,SYS10,IP=192.168.42.10,SIZE=1000"	/* Both				*/
		};

	for ( unsigned i = 0; i < $ARRSZ(s_bad); i++ )
		{
		memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
		s_t$cfg(&s_cfg.sat[0], s_bad[i]);
		$CHECK(!(1 & mh$mop_init(l_port, &s_cfg)), s_bad[i]);
		}
	}

	/*
	**  /IPADDRESS and /GATEWAY against the satellites
	*/
	{
	static	const struct { const char *sat, *ip, *gw, *why; } s_bad [] = {
		{ "IP=192.168.42.10",	"",			"",		"no /IPADDRESS" },
		{ "IP=192.168.43.10",	"192.168.42.5/24",	"",		"a satellite out of the network" },
		{ "IP=192.168.42.5",	"192.168.42.5/24",	"",		"a satellite of our address" },
		{ "IP=192.168.42.255",	"192.168.42.5/24",	"",		"a satellite of the broadcast" },
		{ "IP=192.168.42.1",	"192.168.42.5/24",	"192.168.42.1",	"a satellite of the gateway" },
		{ "IP=192.168.42.10",	"192.168.42.5/33",	"",		"a prefix too long" },
		{ "IP=192.168.42.10",	"192.168.42.0/24",	"",		"/IPADDRESS of the network" },
		{ "IP=192.168.42.10",	"192.168.42.5/24",	"192.168.43.1",	"a gateway out of the network" },
		{ "IP=192.168.42.10",	"192.168.42.5/24",	"192.168.42.5",	"a gateway of our address" }
		};

	for ( unsigned i = 0; i < $ARRSZ(s_bad); i++ )
		{
		memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
		snprintf(l_spec, sizeof(l_spec), "08-00-2B-E5-44-10,DKA100,SYS10,%s", s_bad[i].sat);
		s_t$cfg(&s_cfg.sat[0], l_spec);
		s_t$cfg(&s_cfg.ipaddr, s_bad[i].ip);
		s_t$cfg(&s_cfg.gateway, s_bad[i].gw);
		$CHECK((1 & mh$mop_init(l_port, &s_cfg)) && !(1 & mh$tftp_init(l_port, &s_cfg)), s_bad[i].why);
		}

	memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
	s_t$cfg(&s_cfg.sat[0], "08-00-2B-E5-44-10,DKA100,SYS10,IP=192.168.42.10");
	s_t$cfg(&s_cfg.sat[1], "08-00-2B-E5-44-11,DKA100,SYS11,IP=192.168.42.10");
	s_t$cfg(&s_cfg.ipaddr, "192.168.42.5/24");
	s_t$cfg(&s_cfg.gateway, "");
	$CHECK((1 & mh$mop_init(l_port, &s_cfg)) && !(1 & mh$tftp_init(l_port, &s_cfg)), "two satellites of one address are refused");
	}

	/*
	**  I64S1 of IP=, SAT1 of MOP beside it
	*/
	memset(&s_cfg.sat, 0, sizeof(s_cfg.sat));
	s_t$cfg(&s_cfg.sat[0], "08:00:2b:e5:44:10,dka100,[SYS10.],IP=192.168.42.10");
	s_t$cfg(&s_cfg.sat[1], "08-00-2B-E5-43-01,DKA100,SYS11");
	s_t$cfg(&s_cfg.ipaddr, "192.168.42.5");
	s_t$cfg(&s_cfg.gateway, "192.168.42.1");

	$CHECK((1 & mh$mop_init(l_port, &s_cfg)) && (1 & mh$tftp_init(l_port, &s_cfg)) && (l_port->nsat == 2),
		"I64S1 of IP= and SAT1 of MOP; /IPADDRESS of no prefix, /GATEWAY");
	l_sat = &l_port->sats[0];
	$CHECK((l_sat->ip == T$K_SATIP) && !l_port->sats[1].ip && !strcmp(l_sat->root, "SYS10") && (l_port->ipaddr == T$K_HUBIP)
		&& (l_port->ipmask == 0xFFFFFF00U) && (l_port->gateway == T$K_GWIP), "... 192.168.42.10, SYS10; us .5/24, gateway .1");

	/*
	**  ARP
	*/
	memset(l_arp, 0, sizeof(l_arp));
	memcpy(l_arp, s_bcast, 6);
	memcpy(l_arp + 6, s_satmac, 6);
	s_p16(l_arp + 12, 0x0806);
	memcpy(l_arp + 14, "\x00\x01\x08\x00\x06\x04\x00\x01", 8);
	memcpy(l_arp + 22, s_satmac, 6);
	s_p32(l_arp + 28, T$K_SATIP);
	s_p32(l_arp + 38, T$K_HUBIP);
	s_ntx = 0;
	mh$tftp_input(l_port, MH$K_IP_ARP, l_arp, 60);
	$CHECK((s_ntx == 1) && (s_txlen[0] == 60) && !memcmp(s_tx[0], s_satmac, 6) && !memcmp(s_tx[0] + 6, s_hubmac, 6)
		&& (s_g16(s_tx[0] + 12) == 0x0806) && !memcmp(s_tx[0] + 14, "\x00\x01\x08\x00\x06\x04\x00\x02", 8)
		&& !memcmp(s_tx[0] + 22, s_hubmac, 6) && (s_g32(s_tx[0] + 28) == T$K_HUBIP) && !memcmp(s_tx[0] + 32, s_satmac, 6)
		&& (s_g32(s_tx[0] + 38) == T$K_SATIP), "ARP of 192.168.42.5: the reply, our adapter, to the asker");

	l_port->passive = 1;
	s_ntx = 0;
	mh$tftp_input(l_port, MH$K_IP_ARP, l_arp, 60);
	$CHECK(!s_ntx, "... /PASSIVE: no reply");
	l_port->passive = 0;

	s_p32(l_arp + 38, T$K_GWIP);
	s_ntx = 0;
	mh$tftp_input(l_port, MH$K_IP_ARP, l_arp, 60);
	$CHECK(!s_ntx, "ARP of another address: no reply");

	/*
	**  BOOTP: the reply to I64S1 at its address
	*/
	memset(l_b, 0, sizeof(l_b));
	l_b[0] = 1;
	l_b[1] = 1;
	l_b[2] = 6;
	memcpy(l_b + 4, "\x8E\xD3\x60\xF4", 4);
	memcpy(l_b + 28, s_satmac, 6);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_ntx == 1) && (l_sp == 67) && (l_dp == 68) && (l_n == 300) && (l_d[0] == 2) && !memcmp(l_d + 4, l_b + 4, 4)
		&& (s_g32(l_d + 16) == T$K_SATIP) && (s_g32(l_d + 20) == T$K_HUBIP) && !memcmp(l_d + 28, s_satmac, 6)
		&& !strcmp((const char *) l_d + 44, "MHUB3"), "BOOTREQUEST of I64S1: BOOTREPLY to its adapter and 192.168.42.10, from us");
	$CHECK(l_d && !strcmp((const char *) l_d + 108, "$44$DKA100:[SYS10.SYSCOMMON.SYSEXE]VMS_LOADER.EFI"),
		"... the boot file: $44$DKA100:[SYS10.SYSCOMMON.SYSEXE]VMS_LOADER.EFI");
	$CHECK(l_d && !memcmp(l_d + 236, "\x63\x82\x53\x63\x01\x04\xFF\xFF\xFF\x00\x03\x04\xC0\xA8\x2A\x01\xFF", 17),
		"... the options: the mask, the gateway, no DHCP");
	$CHECK(l_sat->bootps == 1, "... counted");

	/*
	**  DHCP: DISCOVER of a broadcast - OFFER to all; REQUEST - ACK; of
	**  another address - NAK
	*/
	memcpy(l_b + 236, "\x63\x82\x53\x63\x35\x01\x01\x00\x37\x02\x01\x03\xFF", 13);
	l_b[10] = 0x80;
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	l_d = s_t$reply(0, s_bcast, 0xFFFFFFFFU, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g32(l_d + 16) == T$K_SATIP) && (l_d[10] == 0x80)
		&& !memcmp(l_d + 236, "\x63\x82\x53\x63\x35\x01\x02\x36\x04\xC0\xA8\x2A\x05\x33\x04\xFF\xFF\xFF\xFF\x01\x04\xFF\xFF\xFF\x00", 25),
		"DHCPDISCOVER of a broadcast: an OFFER to all, the server ID, the lease for ever, the mask");

	memcpy(l_b + 236, "\x63\x82\x53\x63\x35\x01\x03\x32\x04\xC0\xA8\x2A\x0A\xFF", 14);
	l_b[10] = 0;
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g32(l_d + 16) == T$K_SATIP) && !memcmp(l_d + 236, "\x63\x82\x53\x63\x35\x01\x05", 7),
		"DHCPREQUEST of 192.168.42.10: an ACK to its adapter");

	memcpy(l_b + 236, "\x63\x82\x53\x63\x35\x01\x03\x32\x04\xC0\xA8\x2A\x0B\xFF", 14);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	l_d = s_t$reply(0, s_bcast, 0xFFFFFFFFU, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && !s_g32(l_d + 16) && !l_d[108] && !memcmp(l_d + 236, "\x63\x82\x53\x63\x35\x01\x06\x36\x04\xC0\xA8\x2A\x05\xFF", 14)
		&& (l_sat->tfails == 1), "DHCPREQUEST of another address: a NAK to all, no address, no file; told");

	memcpy(l_b + 236, "\x63\x82\x53\x63\x35\x01\x03\x36\x04\xC0\xA8\x2A\x09\xFF", 14);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	$CHECK(!s_ntx, "DHCPREQUEST for the OFFER of another server (option 54): not answered");

	memcpy(l_b + 236, "\x63\x82\x53\x63\x35\x01\x07\xFF", 8);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	$CHECK(!s_ntx, "DHCPRELEASE: not answered");

	memcpy(l_b + 236, "\x63\x82\x53\x63\xFF", 5);
	memcpy(l_b + 28, s_othmac, 6);
	s_ntx = 0;
	s_t$udp(l_port, s_othmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	$CHECK(!s_ntx, "BOOTREQUEST of another adapter: not answered");

	memcpy(l_b + 28, s_alphamac, 6);
	s_ntx = 0;
	s_t$udp(l_port, s_alphamac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	$CHECK(!s_ntx, "... of SAT1, a satellite of MOP: not answered");

	memcpy(l_b + 28, s_satmac, 6);
	s_p32(l_b + 24, T$K_GWIP);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_bcast, 0, 0xFFFFFFFFU, 68, 67, l_b, 300);
	$CHECK(!s_ntx, "... passed on by a relay agent (giaddr): not answered");

	/*
	**  TFTP of BIG.EXE, no options: blocks of 512, the last of 100
	*/
	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == 3 * 512 + 100) && (l_blocks == 4) && !memcmp(s_out, $BLK(1100), l_len),
		"RRQ of BIG.EXE: 4 blocks of 512, the last of 100, the octets of the file");
	$CHECK((l_sat->tfiles == 1) && (l_sat->toctets == 1636) && !l_port->xfer[0].busy, "... counted, the transfer over");

	/*
	**  ... tsize and blksize 1024: the OACK, then 2 blocks
	*/
	{
	const	char	*l_o [] = { "tsize", "0", "blksize", "1024", NULL };

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", l_o, s_out, sizeof(s_out), &l_len, l_oack, &l_blocks);
	$CHECK(l_ok && (l_len == 1636) && (l_blocks == 2) && !memcmp(s_out, $BLK(1100), l_len) && !strcmp(l_oack, "tsize")
		&& !strcmp(l_oack + 6, "1636") && !strcmp(l_oack + 11, "blksize") && !strcmp(l_oack + 19, "1024"),
		"... tsize 0, blksize 1024: OACK tsize 1636, blksize 1024; 2 blocks");
	}

	/*
	**  ... blksize past what a frame holds: 1468; timeout 3
	*/
	{
	const	char	*l_o [] = { "blksize", "9000", "timeout", "3", NULL };

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", l_o, s_out, sizeof(s_out), &l_len, l_oack, &l_blocks);
	$CHECK(l_ok && (l_len == 1636) && (l_blocks == 2) && !strcmp(l_oack + 8, "1468") && !strcmp(l_oack + 13, "timeout")
		&& !strcmp(l_oack + 21, "3"), "... blksize 9000 on an MTU of 1500: 1468; timeout 3");
	}

	/*
	**  ... every option seven times: each taken once, the OACK whole
	*/
	{
	const	char	*l_o [] = { "blksize", "1468", "timeout", "255", "blksize", "1468", "timeout", "255", "blksize", "1468",
				"timeout", "255", "blksize", "1468", NULL };

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", l_o, s_out, sizeof(s_out), &l_len, l_oack, &l_blocks);
	$CHECK(l_ok && (l_len == 1636) && (l_blocks == 2) && !strcmp(l_oack, "blksize") && !strcmp(l_oack + 13, "timeout")
		&& !strcmp(l_oack + 21, "255") && !l_oack[25], "options again and again: blksize and timeout once each");
	}

	/*
	**  ... the size only: the OACK, then the client's ERROR ends it
	*/
	{
	const	char	*l_o [] = { "tsize", "0", NULL };
	uint32_t	l_files = l_sat->tfiles;

	s_t$rq(l_port, 1, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", l_o);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 6) && !memcmp(l_d + 2, "tsize\0" "1636", 11), "RRQ of tsize 0: the OACK");
	s_t$ack(l_port, l_sp, 5, 8);
	$CHECK(!s_ntx && !l_port->xfer[0].busy && (l_sat->tfiles == l_files), "... the client's ERROR 8: the transfer over, no file counted");
	}

	/*
	**  WHOLE.EXE, of 2 whole blocks: an empty third one
	*/
	l_ok = s_t$get(l_port, "_$44$DKA100:<SYS10.SYSEXE>WHOLE.EXE;1", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == 1024) && (l_blocks == 3) && !memcmp(s_out, $BLK(1110), 1024),
		"_$44$DKA100:<SYS10.SYSEXE>WHOLE.EXE;1: 2 blocks of 512 and an empty one");

	/*
	**  The access: the ACE of VMS$SATELLITE_ACCESS reads, another does not
	*/
	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]IA64VMSSYS.PAR", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == 512) && (s_out[0] == 0x5A), "IA64VMSSYS.PAR, no WORLD read, the ACE of VMS$SATELLITE_ACCESS: read");
	$CHECK(!strcmp(l_sat->name, "I64S1"), "... the satellite is I64S1 of its SCSNODE from then on");

	l_fails = l_sat->tfails;
	s_t$rq(l_port, 1, "$44$DKA100:[SYS10.SYSEXE]SYSUAF.DAT", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (l_sp == 69) && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 2) && (l_sat->tfails == l_fails + 1),
		"SYSUAF.DAT, no WORLD read, an ACE of another identifier: ERROR 2 from 69, refused");

	/*
	**  The records of variable length as a stream
	*/
	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]TEXT.DAT", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	s_out[l_ok ? l_len : 0] = '\0';
	$CHECK(l_ok && !memcmp(s_out, "ABC\n\nHELLO\n", 11) && (l_len == 11 + 472 + 1 + 35) && (s_out[11 + 472] == '\n')
		&& !memcmp(s_out + l_len - 35, "A RECORD ACROSS THE END OF A BLOCK\n", 35),
		"TEXT.DAT, VAR: ABC, an empty record, HELLO, one across a block - each ended by a line feed");

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]NOSPAN.DAT", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == 8) && !memcmp(s_out, "ONE\nTWO\n", 8), "NOSPAN.DAT: the FFFF ends the records of a block");

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]VFC.DAT", NULL, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == 4) && !memcmp(s_out, "XYZ\n", 4), "VFC.DAT: the control area left out");

	/*
	**  What is not there, not ours, not right
	*/
	s_t$rq(l_port, 1, "$44$DKA100:[SYS10.SYSEXE]NONE.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (l_sp == 69) && (l_dp == T$K_CPORT) && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 1) && (l_sat->tmiss == 1),
		"NONE.EXE: ERROR 1, file not found; counted");

	s_t$rq(l_port, 1, "$44$DKA200:[SYS10.SYSEXE]BIG.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 2), "another unit: ERROR 2");

	s_t$rq(l_port, 1, "[SYS10.SYSEXE]BIG.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 2), "no unit: ERROR 2");

	s_t$rq(l_port, 2, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 2), "WRQ: ERROR 2, read only");

	{
	uint8_t	l_p [64] = { 0, 1 };

	memcpy(l_p + 2, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE\0mail", 37);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT, 69, l_p, 2 + 38);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 4), "mode mail: ERROR 4");

	memcpy(l_p + 2, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE\0netascii", 42);
	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT, 69, l_p, 2 + 42);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 4), "mode netascii: ERROR 4, octet only");

	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_hubmac, T$K_GWIP, T$K_HUBIP, T$K_CPORT, 69, l_p, 2 + 33);
	$CHECK(!s_ntx, "an RRQ of I64S1's adapter from another address: not answered");

	s_ntx = 0;
	s_t$udp(l_port, s_othmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT, 69, l_p, 2 + 33);
	$CHECK(!s_ntx, "... of another adapter: not answered");

	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_hubmac, T$K_SATIP, T$K_GWIP, T$K_CPORT, 69, l_p, 2 + 33);
	$CHECK(!s_ntx, "... to another address: not answered");
	}

	/*
	**  The packets lost: the DATA again in a second, five times, then the
	**  transfer is cut; a duplicate ACK is not answered; the RRQ again
	**  gives the first block again
	*/
	s_t$rq(l_port, 1, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_xport, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d) == 3) && (s_g16(l_d + 2) == 1), "RRQ of BIG.EXE: DATA 1");

	s_t$rq(l_port, 1, "$44$DKA100:[SYS10.SYSEXE]BIG.EXE", NULL);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (l_sp == l_xport) && (s_g16(l_d + 2) == 1), "... the RRQ again: DATA 1 again from the same port");

	s_t$ack(l_port, (uint16_t) l_xport, 4, 1);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (s_g16(l_d + 2) == 2), "... ACK 1: DATA 2");

	s_t$ack(l_port, (uint16_t) l_xport, 4, 1);
	$CHECK(!s_ntx, "... ACK 1 again: nothing");

	{
	uint8_t	l_p [4] = { 0, 4, 0, 2 };

	s_ntx = 0;
	s_t$udp(l_port, s_satmac, s_hubmac, T$K_SATIP, T$K_HUBIP, T$K_CPORT + 1, (uint16_t) l_xport, l_p, 4);
	l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);
	$CHECK(l_d && (l_sp == l_xport) && (l_dp == T$K_CPORT + 1) && (s_g16(l_d) == 5) && (s_g16(l_d + 2) == 5),
		"... an ACK from another port: ERROR 5, unknown transfer ID");
	}

	s_ntx = 0;
	l_port->tick += MH$K_TFTP_RETRY - 1;
	mh$tftp_tick(l_port);
	$CHECK(!s_ntx, "... no ACK 2: nothing before a second");

	for ( unsigned i = 0; i < MH$K_TFTP_TRIES; i++ )
		{
		s_ntx = 0;
		l_port->tick += MH$K_TFTP_RETRY;
		mh$tftp_tick(l_port);
		l_d = s_t$reply(0, s_satmac, T$K_SATIP, &l_sp, &l_dp, &l_n);

		if ( !(1 & $CHECK(l_d && (s_g16(l_d + 2) == 2), "... DATA 2 again every second")) )
			break;
		}

	s_ntx = 0;
	l_port->tick += MH$K_TFTP_RETRY;
	mh$tftp_tick(l_port);
	$CHECK(!s_ntx && !l_port->xfer[0].busy && !l_port->xfer[1].busy, "... after five: the transfer is cut");

	/*
	**  The block number past 65535: 65540 blocks of 8 octets
	*/
	{
	const	char	*l_o [] = { "blksize", "8", NULL };

	l_ok = s_t$get(l_port, "$44$DKA100:[SYS10.SYSEXE]WRAP.EXE", l_o, s_out, sizeof(s_out), &l_len, NULL, &l_blocks);
	$CHECK(l_ok && (l_len == T$K_WRAPN * T$K_WRAPBLK) && (l_blocks == T$K_WRAPN + 1) && !memcmp(s_out, $BLK(T$K_WRAPLBN), l_len),
		"WRAP.EXE in blocks of 8: 65541 blocks, the number past 65535 goes on from 0");
	}

	/*
	**  The boot is told when TFTP is quiet; the counters
	*/
	$CHECK(l_sat->bheard && l_sat->bfiles, "a boot is going on");
	l_port->tick += MH$K_TFTP_QUIET;
	mh$tftp_tick(l_port);
	$CHECK(!l_sat->bheard && !l_sat->bfiles && !l_sat->boctets, "... told after 10 seconds of quiet, begun anew");
	$CHECK(1 & mh$tftp_stats(l_port), "TFTPSTATS");

	unlink(l_img_name);

	printf("T_TFTP: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
