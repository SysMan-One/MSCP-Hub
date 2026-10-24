#define	__MODULE__	"T_BLK"
#define	__IDENT__	"X01-20"
#define	__REV__		"1.20.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_blk.c
**
**  ABSTRACT:	Unit tests of the data of the MSCP server: READ, WRITE,
**		COMPARE HOST DATA, ERASE over the block transfers of PPC, the
**		order of the commands on a unit, ABORT, GET COMMAND STATUS, a
**		connection that goes while the disk has a command.
**
**		The oracle is a reference block transfer of PEDRIVER: the
**		chunks of our READ must be laid out as the SNT_DAT of ALPHA1
**		there (XCT_LEN 1426, 1426, 875; the offsets; the name of the
**		buffer), our REQ_DAT0 as its REQ_DAT0 of 568 octets; the
**		RET_DAT_LP and the RET_CNF ALPHA2 answered with are fed back,
**		XCT_ID and our name put in.  The server stands for ALPHA2 of
**		T_SCS, the client is made up after DUDRIVER.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-13 : the window opens to 16, RSVP at its half.
**		22-OCT-2026	RRL	X01-19ECO1 : the backend is waited for up to 30 s, as in T_BK: a
**						WRITE to an SD card of a busy small board takes more than 2 s.
**		24-OCT-2026	RRL	X01-20 : jumbo frames: a circuit on an MTU of 9000, READ of 128 KB in 15 chunks,
**						WRITE of 128 KB in 15, one of them out of order.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<fcntl.h>
#include	<unistd.h>

#include	"mh_def.h"
#include	"mscp.h"

#include	"t_frames.h"

#define	T$K_MAXTX	128

static	uint8_t		s_tx [T$K_MAXTX] [MH$K_SZ_FRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

/*
**  Offsets: of our frames (TR V1.3) and of those of ALPHA1/2 (TR V1.5) -
**  the PPC opcode, then W LENGTH, W PPD MTYPE, the SCS header, the body
*/
#define	T$K_TR		(NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX)
#define	T$K_PAY		(T$K_TR + NISCA$K_TR_LEN_V15)	/* Our frames: the header of V1.5	*/
#define	T$K_PAY15	(T$K_TR + NISCA$K_TR_LEN_V15)
#define	T$K_PPD		1
#define	T$K_HDR		(T$K_PPD + 4)
#define	T$K_BODY	(T$K_HDR + NISCA$K_SCS_BODY)

#define	T$K_CONID_A1	0xBD2F0006U		/* The client's CONID				*/
#define	T$K_CONID_US	0x00010001U		/* Ours: the first CDT, generation 1		*/
#define	T$K_BNAME	0xC8200803U		/* The buffer of the client, as in the reference	*/

static	uint16_t	s_seq;



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
	fprintf(stderr, "T_BLK:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The XMIT hook: record the frame, but a bare ACK.
**--
*/
static	int	s_t$xmit	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
	if ( nisca$get16(a_frame + NISCA$K_SZ_ETH) == NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15 )
		return	STS$K_SUCCESS;

	if ( s_ntx >= T$K_MAXTX )
		return	STS$K_ERROR;

	memcpy(s_tx[s_ntx], a_frame, $MIN(a_len, sizeof(s_tx[0])));
	s_txlen[s_ntx++] = a_len;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A port made up as ALPHA2, its path to ALPHA1 open, the
**	window open from the start.
**--
*/
static	int	s_t$port	(
		MH$PORT *	a_port
			)
{
static	const uint8_t	s_mac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x02 };

	memset(a_port, 0, sizeof(*a_port));

	a_port->sd		= -1;
	a_port->xmit		= s_t$xmit;
	a_port->group		= 4242;
	a_port->sysid		= 65533;
	a_port->bufsz		= 1426;
	a_port->announcing	= 1;
	a_port->next_hello	= (uint64_t) -1;
	a_port->incarn		= 0x00BC3C8BC2EC7380ULL;
	memcpy(a_port->mac, s_mac, NISCA$K_SZ_MAC);
	memcpy(a_port->ifname, "EWA", 4);
	nisca$group_mac(a_port->group, a_port->gmac);
	mh$hex2bin("5A3C9E1700000000", 16, a_port->auth, MH$K_SZ_AUTH);

	a_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memcpy(a_port->ccname + 1, "ALPHA2", NISCA$K_SZ_NODENAME);
	a_port->devname[0] = 3;
	memcpy(a_port->devname + 1, "EWA", 3);

	mh$cc_input(a_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	mh$cc_input(a_port, s_vack_alpha1, sizeof(s_vack_alpha1));
	mh$cc_input(a_port, s_start_alpha1, sizeof(s_start_alpha1));
	mh$cc_input(a_port, s_ack_alpha1, sizeof(s_ack_alpha1));

	a_port->nodes[0].vc->window = a_port->nodes[0].vc->maxwin;

	s_ntx = 0;
	s_seq = 1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a sequenced frame of ALPHA1, renumbered, acknowledging all we
**	have sent.
**--
*/
static	int	s_t$feed	(
		MH$PORT *	a_port,
		uint8_t *	a_frame,
		unsigned	a_len
			)
{
uint8_t		*l_tr = a_frame + T$K_TR;
uint16_t	l_ack = (uint16_t) (a_port->nodes[0].vc->nsu - 1);

	nisca$put16(l_tr + NISCA$K_TR_SEQ, s_seq);
	nisca$put32(l_tr + NISCA$K_TR_MSG_SEQ, s_seq);
	nisca$put16(l_tr + NISCA$K_TR_ACK, l_ack);
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, l_ack);
	s_seq++;

	return	mh$cc_input(a_port, a_frame, a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a PPC packet of ALPHA1: the TR V1.5 header of its CON_REQ, no
**	RSVP, then the PPC part given.
**--
*/
static	int	s_t$ppc	(
		MH$PORT *	a_port,
	const	uint8_t *	a_pay,
		unsigned	a_len
			)
{
uint8_t		l_f [MH$K_SZ_FRAME];
unsigned	l_len = T$K_PAY15 + a_len;

	memcpy(l_f, s_conreq_alpha1, T$K_PAY15);
	nisca$put16(l_f + NISCA$K_SZ_ETH, (uint16_t) (l_len - NISCA$K_SZ_LANHDR));
	l_f[T$K_TR + NISCA$K_TR_FLAG] = NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK | NISCA$M_TR_FLAG_EXT;
	memcpy(l_f + T$K_PAY15, a_pay, a_len);

	return	s_t$feed(a_port, l_f, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a bare ACK of ALPHA1 of all we have sent: the window opens.
**--
*/
static	int	s_t$ack	(
		MH$PORT *	a_port
			)
{
uint8_t		l_f [NISCA$K_MAXFRAME];
uint8_t		*l_tr = l_f + T$K_TR;
uint16_t	l_ack = (uint16_t) (a_port->nodes[0].vc->nsu - 1);

	memcpy(l_f, s_conreq_alpha1, T$K_PAY15);
	nisca$put16(l_f + NISCA$K_SZ_ETH, (uint16_t) (T$K_PAY15 - NISCA$K_SZ_LANHDR));
	l_tr[NISCA$K_TR_FLAG] = NISCA$M_TR_ACK | NISCA$M_TR_FLAG_EXT;
	nisca$put16(l_tr + NISCA$K_TR_ACK, l_ack);
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, l_ack);

	return	mh$cc_input(a_port, l_f, T$K_PAY15);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed an SCS message of ALPHA1 made up from nothing.
**--
*/
static	int	s_t$scs	(
		MH$PORT *	a_port,
		unsigned	a_mtype,
		int		a_credit,
		uint32_t	a_dst,
	const	uint8_t *	a_body,
		unsigned	a_blen
			)
{
uint8_t		l_p [MH$K_SZ_TRPAY];

	l_p[0] = NISCA$K_PPC_SNT_SEQ;
	nisca$put16(l_p + T$K_PPD, (uint16_t) (NISCA$K_SCS_OVHD + a_blen));
	nisca$put16(l_p + T$K_PPD + 2, NISCA$K_PPD_SCS_MSG);
	nisca$put16(l_p + T$K_HDR + NISCA$K_SCS_MTYPE, (uint16_t) a_mtype);
	nisca$put16(l_p + T$K_HDR + NISCA$K_SCS_CREDIT, (uint16_t) (int16_t) a_credit);
	nisca$put32(l_p + T$K_HDR + NISCA$K_SCS_DST_CONID, a_dst);
	nisca$put32(l_p + T$K_HDR + NISCA$K_SCS_SRC_CONID, T$K_CONID_A1);
	memcpy(l_p + T$K_BODY, a_body, a_blen);

	return	s_t$ppc(a_port, l_p, T$K_BODY + a_blen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command of DUDRIVER; READ, WRITE and the like get BYTE_CNT, the
**	buffer of the client and LBN.
**--
*/
static	int	s_t$cmd	(
		uint8_t *	a_cmd,
		uint32_t	a_ref,
		uint16_t	a_unit,
		uint8_t		a_op,
		uint16_t	a_mod,
		uint32_t	a_bcnt,
		uint32_t	a_lbn
			)
{
	memset(a_cmd, 0, 36);
	nisca$put32(a_cmd + MSCP$K_CMD_REF, a_ref);
	nisca$put16(a_cmd + MSCP$K_UNIT, a_unit);
	a_cmd[MSCP$K_OPCODE] = a_op;
	nisca$put16(a_cmd + MSCP$K_MODIFIER, a_mod);
	nisca$put32(a_cmd + MSCP$K_BYTE_CNT, a_bcnt);
	nisca$put32(a_cmd + MSCP$K_BUFFER + NISCA$K_BD_BOFF, 0);
	nisca$put32(a_cmd + MSCP$K_BUFFER + NISCA$K_BD_BNAME, T$K_BNAME);
	nisca$put32(a_cmd + MSCP$K_BUFFER + NISCA$K_BD_RCONID, T$K_CONID_US);
	nisca$put32(a_cmd + MSCP$K_LBN, a_lbn);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send an MSCP command of 36 octets.
**--
*/
static	int	s_t$mscp	(
		MH$PORT *	a_port,
	const	uint8_t *	a_cmd
			)
{
	return	s_t$scs(a_port, NISCA$K_SCS_APPL_MSG, 1, T$K_CONID_US, a_cmd, 36);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	What a frame we sent is: its PPC part, the opcode, an end message.
**--
*/
static	const uint8_t	*s_t$pay	(unsigned a_i)	{ return s_tx[a_i] + T$K_PAY; }
static	unsigned	s_t$opc		(unsigned a_i)	{ return s_tx[a_i][T$K_PAY]; }
static	const uint8_t	*s_t$end	(unsigned a_i)	{ return s_tx[a_i] + T$K_PAY + T$K_BODY; }
static	unsigned	s_t$endlen	(unsigned a_i)	{ return nisca$get16(s_tx[a_i] + T$K_PAY + T$K_PPD) - NISCA$K_SCS_OVHD; }

static	int	s_t$isend	(
		unsigned	a_i,
		uint32_t	a_ref,
		unsigned	a_status
			)
{
	return	(a_i < s_ntx) && (s_t$opc(a_i) == NISCA$K_PPC_SNT_SEQ) && (nisca$get32(s_t$end(a_i) + MSCP$K_CMD_REF) == a_ref)
		&& (nisca$get16(s_t$end(a_i) + MSCP$K_STATUS) == a_status);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Wait for the backend, up to 30 s: a small board under load is slow.
**--
*/
static	int	s_t$disk	(
		MH$PORT *	a_port
			)
{
	return	mh$bk_wait(a_port, 30000);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The answer of the client to our block transfer: the PPC part of a
**	reference frame of ALPHA2, our XCT_ID and REC_NAME put in.
**--
*/
static	int	s_t$answer	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len,
	const	uint8_t *	a_ours
			)
{
uint8_t		l_p [MH$K_SZ_TRPAY];
unsigned	l_len = a_len - T$K_PAY15;

	memcpy(l_p, a_frame + T$K_PAY15, l_len);
	memcpy(l_p + 1 + NISCA$K_BLK_XCT_ID, a_ours + 1 + NISCA$K_BLK_XCT_ID, 8);

	if ( l_len >= 1 + NISCA$K_BLK_HLEN )
		memcpy(l_p + 1 + NISCA$K_BLK_REC_NAME, a_ours + 1 + NISCA$K_BLK_REC_NAME, 4);

	return	s_t$ppc(a_port, l_p, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	RET_DAT chunks of a client of a given size, for the data of a REQ_DAT
**	we sent.
**--
*/
static	int	s_t$retdat	(
		MH$PORT *	a_port,
	const	uint8_t *	a_req,
	const	uint8_t *	a_data,
		uint32_t	a_len,
		uint32_t	a_chunk
			)
{
uint8_t		l_p [MH$K_SZ_TRPAY];
uint32_t	l_n;

	for ( uint32_t l_off = 0; l_off < a_len; l_off += l_n )
		{
		l_n = $MIN(a_chunk, a_len - l_off);

		memcpy(l_p, a_req, 1 + NISCA$K_BLK_HLEN);
		l_p[0] = (l_off + l_n == a_len) ? NISCA$K_PPC_RET_DAT_LP : NISCA$K_PPC_RET_DAT;
		nisca$put32(l_p + 1 + NISCA$K_BLK_XCT_LEN, l_n);
		nisca$put32(l_p + 1 + NISCA$K_BLK_SND_BOFF, l_off);
		nisca$put32(l_p + 1 + NISCA$K_BLK_REC_BOFF, l_off);
		memcpy(l_p + 1 + NISCA$K_BLK_HLEN, a_data + l_off, l_n);
		s_t$ppc(a_port, l_p, 1 + NISCA$K_BLK_HLEN + l_n);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The tests, see the ABSTRACT.
**
**  RETURN VALUE:
**	0 - every check passed; 1 - some failed.
**--
*/
int	main	(void)
{
static	MH$PORT	s_port;
static	MH$CFG	s_cfg;
static	uint8_t	s_img [256 * 512], s_got [MH$K_MSCP_MAXBCNT], s_data [8192];
MH$PORT		*l_port = &s_port;
MH$VC		*l_vc;
MH$CDT		*l_cdt;
uint8_t		l_b [64], l_cmd [36], l_req [1 + NISCA$K_BLK_HLEN];
char		l_spec [64], l_img [] = "t_blk_XXXXXX", l_img2 [] = "t_blk_XXXXXX";
const uint8_t	*l_p;
uint32_t	l_off, l_tag;
unsigned	l_n, l_ok;
int		l_fd;

	mh$inimsg();

	/*
	**  Two units: $42$DKA100 of 100 MB with a pattern in the first 256
	**  blocks, $42$DKA200 read only
	*/
	for ( unsigned i = 0; i < sizeof(s_img); i++ )
		s_img[i] = (uint8_t) (i * 7 + 3 + (i >> 9));

	l_fd = mkstemp(l_img);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 100 * 1024 * 1024) && (sizeof(s_img) == pwrite(l_fd, s_img, sizeof(s_img), 0)), "an image of 100 MB");
	close(l_fd);
	l_fd = mkstemp(l_img2);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 1024 * 1024), "an image of 1 MB");
	close(l_fd);

	s_t$port(l_port);
	l_vc = l_port->nodes[0].vc;
	s_cfg.alloclass = 42;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img);
	$ASCLEN(&s_cfg.unit[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[0]), l_spec, strlen(l_spec));
	snprintf(l_spec, sizeof(l_spec), "dka200,%s,RO", l_img2);
	$ASCLEN(&s_cfg.unit[1]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[1]), l_spec, strlen(l_spec));
	$CHECK((1 & mh$mscp_init(l_port, &s_cfg)) && (l_port->nunit == 2) && (l_port->units[0].dev.fd >= 0) && (mh$bk_fd() >= 0),
		"two units, opened, the backend started");
	$CHECK((l_port->units[0].media == MH$K_MEDIA_RZ29) && !strcmp(l_port->units[0].ddr, "RZ29") && (l_port->units[1].media == MH$K_MEDIA_RRD42)
		&& !strcmp(l_port->units[1].ddr, "RRD42"), "the media: RZ29, RRD42 for the one read only");

	/*
	**  The connection of DUDRIVER, STCON, ONLINE of both units
	*/
	memset(l_b, ' ', 52);
	nisca$put16(l_b + NISCA$K_SCS_MIN_CR, 2);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	memcpy(l_b + 4, "MSCP$DISK", 9);
	memcpy(l_b + 20, "VMS$DISK_CL_DRVR", 16);
	s_t$scs(l_port, NISCA$K_SCS_CON_REQ, 10, 0, l_b, 52);
	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	s_t$scs(l_port, NISCA$K_SCS_ACCP_RSP, 0, T$K_CONID_US, l_b, 4);
	l_cdt = &l_vc->cdt[0];
	$CHECK((l_cdt->state == MH$K_CDT_OPEN) && (l_cdt->host == 0), "the connection is open, host 0");

	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00010001, 0x4000, MSCP$K_OP_ONLIN, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x00020001, 0x4001, MSCP$K_OP_ONLIN, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	$CHECK((s_ntx == 2) && s_t$isend(0, 0x00010001, 0) && s_t$isend(1, 0x00020001, 0), "ONLINE of both units: SUCC");

	/*
	**  READ of 3727 octets at LBN 10: the reference chunks of ALPHA1, then
	**  RET_CNF, then - not before - the END
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00030001, 0x4000, MSCP$K_OP_READ, 0, 3727, 10);
	s_t$mscp(l_port, l_cmd);
	$CHECK(!s_ntx && (l_port->nhrb == 1), "READ: nothing out while the disk has it");
	s_t$disk(l_port);
	$CHECK(s_ntx == 3, "READ of 3727: three chunks");

	for ( unsigned i = 0, l_e0 [] = { 0, 1, 2 }; i < 3; i++ )
		{
		static	const uint8_t	*s_e0 [] = { s_e0_sntdat0_a1, s_e0_sntdat1_a1, s_e0_sntdat2_a1 };
		const uint8_t		*l_r = s_e0[l_e0[i]] + T$K_PAY15;
		char			l_what [96];

		l_p = s_t$pay(i);
		snprintf(l_what, sizeof(l_what), "READ chunk %u: opcode, XCT_LEN, SND_BOFF, REC_NAME, REC_BOFF as in the reference", i + 1);
		$CHECK((l_p[0] == l_r[0]) && !memcmp(l_p + 1 + NISCA$K_BLK_XCT_LEN, l_r + 1 + NISCA$K_BLK_XCT_LEN, 4)
			&& !memcmp(l_p + 1 + NISCA$K_BLK_SND_BOFF, l_r + 1 + NISCA$K_BLK_SND_BOFF, 4)
			&& !memcmp(l_p + 1 + NISCA$K_BLK_REC_NAME, l_r + 1 + NISCA$K_BLK_REC_NAME, 8), l_what);

		l_off = nisca$get32(l_p + 1 + NISCA$K_BLK_SND_BOFF);
		l_n = nisca$get32(l_p + 1 + NISCA$K_BLK_XCT_LEN);
		snprintf(l_what, sizeof(l_what), "READ chunk %u: our XCT_ID and name, the data of LBN 10, the frame of %u", i + 1, 16 + 14 + NISCA$K_TR_LEN_V15 + 1 + 28 + l_n);
		$CHECK((nisca$get32(l_p + 1) == T$K_CONID_US) && (nisca$get32(l_p + 5) == nisca$get32(l_p + 1 + NISCA$K_BLK_SND_NAME))
			&& !memcmp(l_p + 1 + NISCA$K_BLK_HLEN, s_img + 10 * 512 + l_off, l_n) && (s_txlen[i] == 16 + 14 + NISCA$K_TR_LEN_V15 + 1 + 28 + l_n), l_what);
		}

	$CHECK(s_tx[2][T$K_TR + NISCA$K_TR_FLAG] & NISCA$M_TR_RSVP, "the last chunk asks for an ACK at once");

	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	$CHECK((s_ntx == 1) && s_t$isend(0, 0x00030001, MSCP$K_ST_SUCC) && (s_t$end(0)[MSCP$K_OPCODE] == 0xA1) && (s_t$endlen(0) == 32)
		&& (nisca$get32(s_t$end(0) + MSCP$K_BYTE_CNT) == 3727) && (nisca$get32(s_t$end(0) + MSCP$K_LBN) == 10),
		"the reference RET_CNF: the END, SUCC, 32 octets, BYTE_CNT 3727, FRST_BAD = LBN");
	$CHECK(!l_port->nhrb && !l_port->bufused && (l_port->blk_snt == 3) && (l_port->blk_cnf == 1), "the command and its buffer are free");

	/*
	**  WRITE of 568 octets at LBN 20: REQ_DAT0 as ALPHA1's reference one; the
	**  RET_DAT_LP of ALPHA2 brings the data; the END when it is on disk
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00040001, 0x4000, MSCP$K_OP_WRITE, 0, 568, 20);
	s_t$mscp(l_port, l_cmd);
	l_p = s_t$pay(0);
	$CHECK((s_ntx == 1) && (s_txlen[0] == 16 + 14 + NISCA$K_TR_LEN_V15 + 1 + 28) && (l_p[0] == NISCA$K_PPC_REQ_DAT0)
		&& !memcmp(l_p + 1 + NISCA$K_BLK_XCT_LEN, s_e0_reqdat_a1 + T$K_PAY15 + 1 + NISCA$K_BLK_XCT_LEN, 12)
		&& !nisca$get32(l_p + 1 + NISCA$K_BLK_REC_BOFF) && (nisca$get32(l_p + 1 + NISCA$K_BLK_REC_NAME) == nisca$get32(l_p + 5)),
		"WRITE of 568: REQ_DAT0 as the reference - 568 of C8200803 at 0, into our name at 0");
	memcpy(l_req, l_p, sizeof(l_req));

	s_ntx = 0;
	s_t$answer(l_port, s_e0_retdat_a2, sizeof(s_e0_retdat_a2), l_req);
	$CHECK(!s_ntx, "RET_DAT_LP: nothing out while the disk has it");
	s_t$disk(l_port);
	$CHECK((s_ntx == 1) && s_t$isend(0, 0x00040001, MSCP$K_ST_SUCC) && (s_t$endlen(0) == 36) && (nisca$get32(s_t$end(0) + MSCP$K_BYTE_CNT) == 568),
		"WRITE: the END, SUCC, 36 octets, BYTE_CNT 568");

	l_fd = open(l_img, O_RDONLY);
	$CHECK((l_fd >= 0) && (568 == pread(l_fd, s_got, 568, 20 * 512)) && !memcmp(s_got, s_e0_retdat_a2 + T$K_PAY15 + 1 + NISCA$K_BLK_HLEN, 568)
		&& (512 == pread(l_fd, s_got + 568, 512, 20 * 512 + 568)) && !memcmp(s_got + 568, s_img + 20 * 512 + 568, 512),
		"the 568 reference octets are in the image at LBN 20, the rest of the block is as it was");
	close(l_fd);

	/*
	**  The time of both: one command each, its steps within it
	*/
	{
	const MH$LAT	*l_rd = &l_port->units[0].lat[MH$K_LAT_READ], *l_wr = &l_port->units[0].lat[MH$K_LAT_WRITE];
	uint64_t	l_hist = 0;

	for ( unsigned i = 0; i < MH$K_LAT_NHIST; i++ )
		l_hist += l_rd->hist[i] + l_wr->hist[i];

	$CHECK((l_rd->n == 1) && l_rd->bk && l_rd->xfer && (l_rd->sum >= l_rd->bk + l_rd->xfer) && (l_rd->max == l_rd->sum),
		"LATSTATS: the READ counted, backend and transfer within it");
	$CHECK((l_wr->n == 1) && l_wr->bk && l_wr->xfer && (l_wr->sum >= l_wr->bk + l_wr->xfer) && (l_hist == 2),
		"... the WRITE too, both in the histogram");
	}

	/*
	**  COMPARE HOST DATA of the same: SUCC; one octet changed: COMP
	*/
	memcpy(s_data, s_e0_retdat_a2 + T$K_PAY15 + 1 + NISCA$K_BLK_HLEN, 568);

	for ( int l_bad = 0; l_bad < 2; l_bad++ )
		{
		s_ntx = 0;
		s_t$cmd(l_cmd, 0x00050001 + l_bad, 0x4000, MSCP$K_OP_COMP, 0, 568, 20);
		s_t$mscp(l_port, l_cmd);
		memcpy(l_req, s_t$pay(0), sizeof(l_req));
		s_data[100] ^= (uint8_t) l_bad;
		s_ntx = 0;
		s_t$retdat(l_port, l_req, s_data, 568, 1426);
		s_t$disk(l_port);
		$CHECK((s_ntx == 1) && s_t$isend(0, 0x00050001 + l_bad, l_bad ? MSCP$K_ST_COMP : MSCP$K_ST_SUCC) && (s_t$endlen(0) == 32),
			l_bad ? "COMPARE of other data: COMP (0x0007)" : "COMPARE of the same data: SUCC");
		}

	/*
	**  WRITE of 4096 in the chunks of a satellite (1424): three of them
	*/
	for ( unsigned i = 0; i < 4096; i++ )
		s_data[i] = (uint8_t) (0xA5 ^ i);

	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00070001, 0x4000, MSCP$K_OP_WRITE, 0, 4096, 30);
	s_t$mscp(l_port, l_cmd);
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$retdat(l_port, l_req, s_data, 4096, 1424);
	s_t$disk(l_port);
	l_fd = open(l_img, O_RDONLY);
	$CHECK(s_t$isend(0, 0x00070001, MSCP$K_ST_SUCC) && (l_fd >= 0) && (4096 == pread(l_fd, s_got, 4096, 30 * 512)) && !memcmp(s_got, s_data, 4096),
		"WRITE of 4096 in chunks of 1424: SUCC, the data in the image");
	close(l_fd);

	/*
	**  WRITE with COMP: written, read back, compared
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00080001, 0x4000, MSCP$K_OP_WRITE, MSCP$M_MD_COMP, 1024, 40);
	s_t$mscp(l_port, l_cmd);
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$retdat(l_port, l_req, s_data, 1024, 1426);
	s_t$disk(l_port);

	if ( !s_ntx )
		s_t$disk(l_port);

	$CHECK((s_ntx == 1) && s_t$isend(0, 0x00080001, MSCP$K_ST_SUCC), "WRITE with COMP: written and compared, SUCC");

	/*
	**  READ with COMP: the data goes, comes back by REQ_DAT, compared
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x00090001, 0x4000, MSCP$K_OP_READ, MSCP$M_MD_COMP, 1024, 40);
	s_t$mscp(l_port, l_cmd);
	s_t$disk(l_port);
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	$CHECK((s_ntx == 1) && (s_t$opc(0) == NISCA$K_PPC_SNT_DAT_LP), "READ with COMP of 1024: one chunk");
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	$CHECK((s_ntx == 1) && (s_t$opc(0) == NISCA$K_PPC_REQ_DAT0), "... RET_CNF: the data is asked back");
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$retdat(l_port, l_req, s_data, 1024, 1426);
	$CHECK((s_ntx == 1) && s_t$isend(0, 0x00090001, MSCP$K_ST_SUCC) && (nisca$get32(s_t$end(0) + MSCP$K_BYTE_CNT) == 1024),
		"... it is the same: SUCC, BYTE_CNT 1024");

	/*
	**  The checks of a command that moves data, in their order
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000A0001, 0x4001, MSCP$K_OP_WRITE, 0, 512, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000A0002, 0x4000, MSCP$K_OP_READ, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000A0003, 0x4000, MSCP$K_OP_READ, 0, 1024, 204799);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000A0004, 0x4000, MSCP$K_OP_READ, 0, 512, 204800);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000A0005, 0x4003, MSCP$K_OP_READ, 0, 512, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000A0006, 0x4000, MSCP$K_OP_ERASE, 0x0004, 512, 0);
	s_t$mscp(l_port, l_cmd);
	$CHECK((s_ntx == 6) && s_t$isend(0, 0x000A0001, MSCP$K_ST_WRTPR), "WRITE of a read only unit: WRTPR");
	$CHECK(s_t$isend(1, 0x000A0002, MSCP$K_ST_SUCC) && !nisca$get32(s_t$end(1) + MSCP$K_BYTE_CNT), "READ of 0 octets: SUCC at once");
	$CHECK(s_t$isend(2, 0x000A0003, 0x0C01) && (s_t$endlen(2) == 36), "READ of 2 blocks from the last: ICMD on BYTE_CNT, 36 octets");
	$CHECK(s_t$isend(3, 0x000A0004, 0x0C01), "READ past the end: ICMD on BYTE_CNT");
	$CHECK(s_t$isend(4, 0x000A0005, MSCP$K_ST_OFFLN), "READ of no unit: OFFLN");
	$CHECK(s_t$isend(5, 0x000A0006, 0x0A01), "ERASE with a modifier it does not take: ICMD on +0A");
	$CHECK(!l_port->nhrb, "... no command left in progress");

	/*
	**  CMD_REF with 0xFFFF in its high word - a live RSPID of the client may
	**  have it: its END goes
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0xFFFF0123, 0x4000, MSCP$K_OP_READ, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	$CHECK((s_ntx == 1) && s_t$isend(0, 0xFFFF0123, MSCP$K_ST_SUCC) && (l_cdt->state == MH$K_CDT_OPEN), "CMD_REF FFFF0123: the END goes, the connection stays");

	/*
	**  ERASE of 2 blocks at LBN 30
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000B0001, 0x4000, MSCP$K_OP_ERASE, 0, 1024, 30);
	s_t$mscp(l_port, l_cmd);
	s_t$disk(l_port);
	l_fd = open(l_img, O_RDONLY);
	memset(s_data, 0, 1024);
	$CHECK(s_t$isend(0, 0x000B0001, MSCP$K_ST_SUCC) && (s_t$endlen(0) == 36) && (l_fd >= 0) && (1536 == pread(l_fd, s_got, 1536, 30 * 512))
		&& !memcmp(s_got, s_data, 1024) && (s_got[1024] == (uint8_t) (0xA5 ^ 1024)), "ERASE of 2 blocks: SUCC, 36 octets, zeros there, not past");
	close(l_fd);

	/*
	**  The order of a unit: READ in progress, ONLINE waits, a WRITE after it
	**  waits too; ABORT of the WRITE ends it ABRTD at once; GTCMD of the
	**  READ shows it moving; then all goes in order
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000C0001, 0x4000, MSCP$K_OP_READ, 0, 512, 1);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000C0002, 0x4000, MSCP$K_OP_ONLIN, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000C0003, 0x4000, MSCP$K_OP_WRITE, 0, 512, 1);
	s_t$mscp(l_port, l_cmd);
	$CHECK(!s_ntx && (l_port->nhrb == 3) && (l_port->units[0].current == 1) && l_port->units[0].qhead, "READ runs, ONLINE and WRITE wait");

	s_t$cmd(l_cmd, 0x000C0004, 0x4000, MSCP$K_OP_ABORT, 0, 0, 0);
	nisca$put32(l_cmd + MSCP$K_OUT_REF, 0x000C0003);
	s_t$mscp(l_port, l_cmd);
	$CHECK((s_ntx == 2) && s_t$isend(0, 0x000C0003, MSCP$K_ST_ABRTD) && s_t$isend(1, 0x000C0004, MSCP$K_ST_SUCC) && (s_t$endlen(1) == 16),
		"ABORT of the waiting WRITE: it ends ABRTD, then ABORT SUCC");

	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000C0005, 0x4000, MSCP$K_OP_GTCMD, 0, 0, 0);
	nisca$put32(l_cmd + MSCP$K_OUT_REF, 0x000C0001);
	s_t$mscp(l_port, l_cmd);
	l_n = nisca$get32(s_t$end(0) + MSCP$K_CMD_STS);
	$CHECK(s_t$isend(0, 0x000C0005, MSCP$K_ST_SUCC) && (l_n < MSCP$K_CMD_STS_INIT) && (l_n > 0x80000000U) && (s_t$endlen(0) == 20),
		"GTCMD of the READ: CMD_STS below -2, it moves");
	s_ntx = 0;
	nisca$put32(l_cmd + MSCP$K_OUT_REF, 0x12345678);
	s_t$mscp(l_port, l_cmd);
	$CHECK(s_t$isend(0, 0x000C0005, MSCP$K_ST_SUCC) && !nisca$get32(s_t$end(0) + MSCP$K_CMD_STS), "GTCMD of no command: 0");

	s_ntx = 0;
	s_t$disk(l_port);
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	$CHECK((s_ntx == 1) && (s_t$opc(0) == NISCA$K_PPC_SNT_DAT_LP), "the READ goes on: its chunk");
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	$CHECK((s_ntx == 2) && s_t$isend(0, 0x000C0001, MSCP$K_ST_SUCC) && s_t$isend(1, 0x000C0002, MSCP$K_ST_ALONL),
		"its RET_CNF: the END of READ, then that of the ONLINE it held back");
	$CHECK(!l_port->nhrb && !l_port->units[0].current && !l_port->units[0].seq && !l_port->units[0].qhead, "the unit is idle");

	/*
	**  READ of 128 KB: 92 chunks through a window of 8, none lost
	*/
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000D0001, 0x4000, MSCP$K_OP_READ, 0, MH$K_MSCP_MAXBCNT, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$disk(l_port);

	for ( unsigned l_last = 0; s_ntx != l_last; )
		{
		l_last = s_ntx;
		s_t$ack(l_port);
		}

	l_ok = (s_ntx == 92);
	l_tag = nisca$get32(s_t$pay(0) + 5);

	/*
	**  The image as the WRITEs above have left it
	*/
	l_fd = open(l_img, O_RDONLY);
	l_ok = l_ok && (l_fd >= 0) && (MH$K_MSCP_MAXBCNT == pread(l_fd, s_got, MH$K_MSCP_MAXBCNT, 0));
	close(l_fd);

	for ( unsigned i = 0, l_want = 0; l_ok && (i < s_ntx); i++ )
		{
		l_p = s_t$pay(i);
		l_off = nisca$get32(l_p + 1 + NISCA$K_BLK_REC_BOFF);
		l_n = nisca$get32(l_p + 1 + NISCA$K_BLK_XCT_LEN);
		l_ok = (l_off == l_want) && (nisca$get32(l_p + 5) == l_tag) && (l_p[0] == ((i == 91) ? NISCA$K_PPC_SNT_DAT_LP : NISCA$K_PPC_SNT_DAT))
			&& (l_n == ((i == 91) ? 131072 - 91 * 1426 : 1426));

		l_ok = l_ok && !memcmp(l_p + 1 + NISCA$K_BLK_HLEN, s_got + l_off, l_n);

		l_want += l_n;
		}

	$CHECK(l_ok, "READ of 128 KB: 92 chunks, 91 x 1426 + 1306, in order, the data of the image");
	$CHECK(l_port->tr_maxwin == 16, "the window opened to 16");
	$CHECK(!(s_tx[6][T$K_TR] & NISCA$M_TR_RSVP) && (s_tx[7][T$K_TR] & NISCA$M_TR_RSVP) && (s_tx[15][T$K_TR] & NISCA$M_TR_RSVP),
		"... RSVP on the 8th chunk, the half of the window, and on the 16th");
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	$CHECK(s_t$isend(0, 0x000D0001, MSCP$K_ST_SUCC) && (nisca$get32(s_t$end(0) + MSCP$K_BYTE_CNT) == MH$K_MSCP_MAXBCNT), "... RET_CNF: SUCC, 128 KB");

	/*
	**  Chunks that match nothing: dropped, the circuit stays
	*/
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	s_t$retdat(l_port, l_req, s_data, 512, 1426);
	$CHECK(!s_ntx && (l_port->blk_bad == 2) && (l_vc->pbstate == MH$K_PB_OPEN), "a stray RET_CNF and RET_DAT: dropped, BLKERR, the path stays");

	/*
	**  The client goes while the disk has a READ: no END, nothing sent,
	**  the command is freed when the backend gives it back
	*/
	s_t$cmd(l_cmd, 0x000E0001, 0x4000, MSCP$K_OP_READ, 0, 65536, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$cmd(l_cmd, 0x000E0002, 0x4000, MSCP$K_OP_AVAIL, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	s_ntx = 0;
	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	s_t$scs(l_port, NISCA$K_SCS_DISC_REQ, 0, T$K_CONID_US, l_b, 4);
	$CHECK((s_ntx == 2) && (l_port->nhrb == 1) && (l_port->units[0].current == 1) && !l_port->units[0].qhead,
		"DISC_REQ: the waiting AVAILABLE is gone, the READ an orphan that holds the unit");
	s_ntx = 0;
	s_t$disk(l_port);
	$CHECK(!s_ntx && !l_port->nhrb && !l_port->bufused && !l_port->units[0].current, "the READ comes back from the disk: freed, nothing sent, the unit free");

	/*
	**  Jumbo frames: the circuit made again on an MTU of 9000, the node
	**  takes 8926 as well.  READ of 128 KB: 15 chunks, 14 x 8926 + 6108.
	*/
	mh$tr_reset(l_port, &l_port->nodes[0]);
	free(l_vc->pool);
	free(l_vc);
	l_port->nodes[0].vc	= NULL;
	l_port->mtu		= 9000;
	l_port->bufsz		= 9000 - NISCA$K_SZ_PAD - MH$K_LAN_OVHD;
	l_port->nodes[0].lcl_chseq = 0;		/* The restart makes it 1 again: the frames of ALPHA1 are of channel 1	*/

	mh$cc_input(l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	mh$cc_input(l_port, s_vack_alpha1, sizeof(s_vack_alpha1));
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));

	l_port->nodes[0].bufsz = 8926;
	l_vc = l_port->nodes[0].vc;
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_OPEN) && (l_vc->paysz == 9000 - NISCA$K_SZ_PAD - NISCA$K_SZ_DX - NISCA$K_TR_LEN_V15),
		"jumbo: the circuit is open again, its packets sized by the MTU of 9000");
	l_vc->window = l_vc->maxwin;
	s_seq = 1;

	memset(l_b, ' ', 52);
	nisca$put16(l_b + NISCA$K_SCS_MIN_CR, 2);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	memcpy(l_b + 4, "MSCP$DISK", 9);
	memcpy(l_b + 20, "VMS$DISK_CL_DRVR", 16);
	s_t$scs(l_port, NISCA$K_SCS_CON_REQ, 10, 0, l_b, 52);
	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	s_t$scs(l_port, NISCA$K_SCS_ACCP_RSP, 0, T$K_CONID_US, l_b, 4);
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000F0001, 0x4000, MSCP$K_OP_ONLIN, 0, 0, 0);
	s_t$mscp(l_port, l_cmd);
	$CHECK((l_vc->cdt[0].state == MH$K_CDT_OPEN) && (s_ntx == 1) && s_t$isend(0, 0x000F0001, 0), "jumbo: connected again, ONLINE: SUCC");

	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000F0002, 0x4000, MSCP$K_OP_READ, 0, MH$K_MSCP_MAXBCNT, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$disk(l_port);

	for ( unsigned l_last = 0; s_ntx != l_last; )
		{
		l_last = s_ntx;
		s_t$ack(l_port);
		}

	l_ok = (s_ntx == 15);
	l_tag = nisca$get32(s_t$pay(0) + 5);

	l_fd = open(l_img, O_RDONLY);
	l_ok = l_ok && (l_fd >= 0) && (MH$K_MSCP_MAXBCNT == pread(l_fd, s_got, MH$K_MSCP_MAXBCNT, 0));
	close(l_fd);

	for ( unsigned i = 0, l_want = 0; l_ok && (i < s_ntx); i++ )
		{
		l_p = s_t$pay(i);
		l_off = nisca$get32(l_p + 1 + NISCA$K_BLK_REC_BOFF);
		l_n = nisca$get32(l_p + 1 + NISCA$K_BLK_XCT_LEN);
		l_ok = (l_off == l_want) && (nisca$get32(l_p + 5) == l_tag) && (l_p[0] == ((i == 14) ? NISCA$K_PPC_SNT_DAT_LP : NISCA$K_PPC_SNT_DAT))
			&& (l_n == ((i == 14) ? 131072 - 14 * 8926 : 8926)) && (s_txlen[i] <= MH$K_SZ_FRAME);

		l_ok = l_ok && !memcmp(l_p + 1 + NISCA$K_BLK_HLEN, s_got + l_off, l_n);

		l_want += l_n;
		}

	$CHECK(l_ok, "jumbo: READ of 128 KB in 15 chunks, 14 x 8926 + 6108, in order, the data of the image");
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);
	$CHECK(s_t$isend(0, 0x000F0002, MSCP$K_ST_SUCC) && (nisca$get32(s_t$end(0) + MSCP$K_BYTE_CNT) == MH$K_MSCP_MAXBCNT), "jumbo: RET_CNF, SUCC, 128 KB");

	/*
	**  WRITE of 128 KB at LBN 1000 in chunks of 8926 from the node, the
	**  third before the second: it waits in the receive cache, full size
	*/
	{
	static	uint8_t	s_big [MH$K_MSCP_MAXBCNT];
	uint8_t		l_p [MH$K_SZ_TRPAY];
	uint64_t	l_ooo = l_port->tr_ooo;
	uint16_t	l_seq0;
	static	const unsigned	s_order [15] = { 0, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };

	for ( unsigned i = 0; i < sizeof(s_big); i++ )
		s_big[i] = (uint8_t) (0x3C ^ (i * 13) ^ (i >> 11));

	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000F0004, 0x4000, MSCP$K_OP_WRITE, 0, MH$K_MSCP_MAXBCNT, 1000);
	s_t$mscp(l_port, l_cmd);
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	l_seq0 = s_seq;
	s_ntx = 0;

	for ( unsigned k = 0; k < 15; k++ )
		{
		unsigned	i = s_order[k];
		uint32_t	l_o = i * 8926, l_c = $MIN(8926, MH$K_MSCP_MAXBCNT - l_o);

		memcpy(l_p, l_req, 1 + NISCA$K_BLK_HLEN);
		l_p[0] = (i == 14) ? NISCA$K_PPC_RET_DAT_LP : NISCA$K_PPC_RET_DAT;
		nisca$put32(l_p + 1 + NISCA$K_BLK_XCT_LEN, l_c);
		nisca$put32(l_p + 1 + NISCA$K_BLK_SND_BOFF, l_o);
		nisca$put32(l_p + 1 + NISCA$K_BLK_REC_BOFF, l_o);
		memcpy(l_p + 1 + NISCA$K_BLK_HLEN, s_big + l_o, l_c);
		s_seq = (uint16_t) (l_seq0 + i);
		s_t$ppc(l_port, l_p, 1 + NISCA$K_BLK_HLEN + l_c);
		}

	s_seq = (uint16_t) (l_seq0 + 15);
	s_t$disk(l_port);

	l_fd = open(l_img, O_RDONLY);
	$CHECK(s_t$isend(0, 0x000F0004, MSCP$K_ST_SUCC) && (l_port->tr_ooo == l_ooo + 1) && (l_fd >= 0)
		&& (MH$K_MSCP_MAXBCNT == pread(l_fd, s_got, MH$K_MSCP_MAXBCNT, 1000 * 512)) && !memcmp(s_got, s_big, MH$K_MSCP_MAXBCNT),
		"jumbo: WRITE of 128 KB in 15 chunks of 8926, one out of order: SUCC, the data in the image");
	close(l_fd);
	}

	/*
	**  A node without jumbo frames on the same port: the chunks stay 1426
	*/
	l_port->nodes[0].bufsz = 1426;
	s_ntx = 0;
	s_t$cmd(l_cmd, 0x000F0003, 0x4000, MSCP$K_OP_READ, 0, 4096, 0);
	s_t$mscp(l_port, l_cmd);
	s_t$disk(l_port);

	for ( unsigned l_last = 0; s_ntx != l_last; )
		{
		l_last = s_ntx;
		s_t$ack(l_port);
		}

	$CHECK((s_ntx == 3) && (nisca$get32(s_t$pay(0) + 1 + NISCA$K_BLK_XCT_LEN) == 1426) && (nisca$get32(s_t$pay(2) + 1 + NISCA$K_BLK_XCT_LEN) == 4096 - 2 * 1426),
		"jumbo port, a node of 1426: READ of 4 KB in 1426 + 1426 + 1244");
	memcpy(l_req, s_t$pay(0), sizeof(l_req));
	s_ntx = 0;
	s_t$answer(l_port, s_e0_retcnf_a2, sizeof(s_e0_retcnf_a2), l_req);

	unlink(l_img);
	unlink(l_img2);

	printf("T_BLK: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
