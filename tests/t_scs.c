#define	__MODULE__	"T_SCS"
#define	__IDENT__	"X01-10"
#define	__REV__		"1.10.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_scs.c
**
**  ABSTRACT:	Unit tests of the session layer and of the MSCP server.
**
**		SCS$DIRECTORY: the server stands for ALPHA2 and takes in the
**		reference lookups of ALPHA1;
**		its CON_RSP, ACCP_REQ, answers, CR_RSP, DISC_RSP and DISC_REQ
**		must be those of ALPHA2, but for our CONID.
**
**		MSCP$DISK: a connection of a client made up after DUDRIVER:
**		STCON, the walk of the units, GUS of unit 0 and
**		of 0x7FFF, ONLINE, AVAILABLE, the checks of a command, the send
**		credits, DISCONNECT.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
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

#define	T$K_MAXTX	32

static	uint8_t		s_tx [T$K_MAXTX] [NISCA$K_MAXFRAME];
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
#define	T$K_PPD		1			/* From the opcode: W LENGTH			*/
#define	T$K_HDR		(T$K_PPD + 4)		/* ... the SCS header				*/
#define	T$K_BODY	(T$K_HDR + NISCA$K_SCS_BODY)

#define	T$K_CONID_A2	0x5A570006U		/* ALPHA2's CONID of the directory connection	*/
#define	T$K_CONID_A1	0xBD2F0006U		/* ALPHA1's					*/
#define	T$K_CONID_US	0x00010001U		/* Ours: the first CDT, generation 1		*/

static	uint16_t	s_seq;			/* SEQ of the next frame of ALPHA1		*/



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
	fprintf(stderr, "T_SCS:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The XMIT hook of the test port: record the frame - but a bare ACK,
**	which the RSVP of the reference frames asks for (T_VC tests them).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame MH$LAN_SEND has built
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - more frames than T$K_MAXTX.
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
**	A port made up as ALPHA2, its channel and its path to ALPHA1 open by
**	the reference frames.
**
**  FORMAL PARAMETERS:
**
**	a_port		Output: the port context
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_TX is emptied; S_SEQ - the next SEQ of ALPHA1 is 1.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$port	(
		MH$PORT *	a_port
			)
{
static	const uint8_t	s_mac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x02 };

	if ( a_port->nnode && a_port->nodes[0].vc )
		free(a_port->nodes[0].vc);

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

	/*
	**  Every frame fed acknowledges all we have sent: the window may as well
	**  be open from the start (T_VC tests its growth)
	*/
	a_port->nodes[0].vc->window = a_port->nodes[0].vc->maxwin;

	s_ntx = 0;
	s_seq = 1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a sequenced frame of ALPHA1: renumbered in order, acknowledging
**	everything we have sent, its DST_CONID made ours when asked.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		A frame of ALPHA1, TR V1.5
**	a_len		Its length
**	a_dst		DST_CONID to put in, 0 - leave it
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
**--
*/
static	int	s_t$feed	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len,
		uint32_t	a_dst
			)
{
uint8_t		l_f [NISCA$K_MAXFRAME];
uint8_t		*l_tr = l_f + T$K_TR;
uint16_t	l_ack = (uint16_t) (a_port->nodes[0].vc->nsu - 1);

	memcpy(l_f, a_frame, a_len);

	nisca$put16(l_tr + NISCA$K_TR_SEQ, s_seq);
	nisca$put32(l_tr + NISCA$K_TR_MSG_SEQ, s_seq);
	nisca$put16(l_tr + NISCA$K_TR_ACK, l_ack);
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, l_ack);
	s_seq++;

	if ( a_dst )
		nisca$put32(l_f + T$K_PAY15 + T$K_HDR + NISCA$K_SCS_DST_CONID, a_dst);

	return	mh$cc_input(a_port, l_f, a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed an SCS message of ALPHA1 made up from nothing: a client of
**	MSCP$DISK.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mtype		NISCA$K_SCS_*
**	a_credit	CREDIT
**	a_dst		DST_CONID
**	a_body		The body
**	a_blen		Its length
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
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
uint8_t		l_f [NISCA$K_MAXFRAME];
uint8_t		*l_p = l_f + T$K_PAY15;
unsigned	l_len = T$K_PAY15 + T$K_BODY + a_blen;

	memcpy(l_f, s_conreq_alpha1, T$K_PAY15);
	nisca$put16(l_f + NISCA$K_SZ_ETH, (uint16_t) (l_len - NISCA$K_SZ_LANHDR));
	l_f[T$K_TR + NISCA$K_TR_FLAG] = NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK | NISCA$M_TR_FLAG_EXT;	/* No RSVP: no bare ACK	*/

	l_p[0] = NISCA$K_PPC_SNT_SEQ;
	nisca$put16(l_p + T$K_PPD, (uint16_t) (NISCA$K_SCS_OVHD + a_blen));
	nisca$put16(l_p + T$K_PPD + 2, NISCA$K_PPD_SCS_MSG);
	nisca$put16(l_p + T$K_HDR + NISCA$K_SCS_MTYPE, (uint16_t) a_mtype);
	nisca$put16(l_p + T$K_HDR + NISCA$K_SCS_CREDIT, (uint16_t) (int16_t) a_credit);
	nisca$put32(l_p + T$K_HDR + NISCA$K_SCS_DST_CONID, a_dst);
	nisca$put32(l_p + T$K_HDR + NISCA$K_SCS_SRC_CONID, T$K_CONID_A1);
	memcpy(l_p + T$K_BODY, a_body, a_blen);

	return	s_t$feed(a_port, l_f, l_len, 0);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Compare an SCS message we sent with one of ALPHA2, from LENGTH on;
**	SRC_CONID - ours - is not compared.
**
**  FORMAL PARAMETERS:
**
**	a_i		Index in S_TX
**	a_ref		The frame of ALPHA2
**	a_what		Name of the comparison
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - they differ.
**--
*/
static	int	s_t$cmp	(
		unsigned	a_i,
	const	uint8_t *	a_ref,
	const	char *		a_what
			)
{
const uint8_t	*l_o = s_tx[a_i] + T$K_PAY, *l_r = a_ref + T$K_PAY15;
unsigned	l_len = 2 + nisca$get16(l_r + T$K_PPD), l_src = T$K_HDR + NISCA$K_SCS_SRC_CONID;
char		l_what [128];
int		l_ok;

	l_ok = (a_i < s_ntx) && (l_o[0] == l_r[0]) && !memcmp(l_o + T$K_PPD, l_r + T$K_PPD, l_src - T$K_PPD)
		&& !memcmp(l_o + l_src + 4, l_r + l_src + 4, l_len - (l_src + 4 - T$K_PPD))
		&& (nisca$get32(l_o + l_src) == T$K_CONID_US);

	snprintf(l_what, sizeof(l_what), "%s: as ALPHA2 sent it, our CONID", a_what);

	return	$CHECK(l_ok, l_what);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The MSCP end message in a frame we sent.
**
**  FORMAL PARAMETERS:
**
**	a_i		Index in S_TX
**
**  RETURN VALUE:
**	The end message; it is in S_TX[a_i] from T$K_PAY + T$K_BODY on.
**--
*/
static	const uint8_t	*s_t$end	(
		unsigned	a_i
			)
{
	return	s_tx[a_i] + T$K_PAY + T$K_BODY;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command of DUDRIVER: CMD_REF, unit, opcode, modifier, the rest 0.
**
**  FORMAL PARAMETERS:
**
**	a_cmd		Output: the command
**	a_len		Its length
**	a_ref		CMD_REF
**	a_unit		UNIT
**	a_op		OPCODE
**	a_mod		MODIFIER
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$cmd	(
		uint8_t *	a_cmd,
		unsigned	a_len,
		uint32_t	a_ref,
		uint16_t	a_unit,
		uint8_t		a_op,
		uint16_t	a_mod
			)
{
	memset(a_cmd, 0, a_len);
	nisca$put32(a_cmd + MSCP$K_CMD_REF, a_ref);
	nisca$put16(a_cmd + MSCP$K_UNIT, a_unit);
	a_cmd[MSCP$K_OPCODE] = a_op;
	nisca$put16(a_cmd + MSCP$K_MODIFIER, a_mod);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send one MSCP command and take the one end message it must make.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cmd		The command
**	a_len		Its length
**	a_credit	CREDIT on the APPL_MSG
**
**  RETURN VALUE:
**	The end message; NULL - not one frame came out.
**--
*/
static	const uint8_t	*s_t$mscp	(
		MH$PORT *	a_port,
	const	uint8_t *	a_cmd,
		unsigned	a_len,
		int		a_credit
			)
{
	s_ntx = 0;
	s_t$scs(a_port, NISCA$K_SCS_APPL_MSG, a_credit, T$K_CONID_US, a_cmd, a_len);

	return	(s_ntx == 1) ? s_t$end(0) : NULL;
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
MH$PORT		*l_port = &s_port;
MH$VC		*l_vc;
MH$CDT		*l_cdt;
uint8_t		l_b [64], l_cmd [36];
char		l_spec [64];
const uint8_t	*l_e;
char		l_img [] = "t_scs_XXXXXX";
int		l_fd;

	mh$inimsg();

	/*
	**  SCS$DIRECTORY, as ALPHA2 answered: no units, so MSCP$DISK is
	**  NOT PRESENT HERE there and here
	*/
	s_t$port(l_port);
	l_vc = l_port->nodes[0].vc;
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_OPEN), "the path to ALPHA1 is open");

	s_t$feed(l_port, s_conreq_alpha1, sizeof(s_conreq_alpha1), 0);
	$CHECK(s_ntx == 2, "CON_REQ to SCS$DIRECTORY: CON_RSP and ACCP_REQ");
	$CHECK(!memcmp(s_tx[0] + T$K_PAY, s_conrsp_alpha2 + T$K_PAY15, 1 + 2 + NISCA$K_SCS_CON_RSP_LEN), "CON_RSP: octet for octet as ALPHA2's");
	s_t$cmp(1, s_dir_accpreq_a2, "ACCP_REQ, credit 8, the names turned round, 16 blanks");
	l_cdt = &l_vc->cdt[0];
	$CHECK((l_cdt->state == MH$K_CDT_ACCP_SENT) && (l_cdt->lconid == T$K_CONID_US) && (l_cdt->sendcr == 8), "ACCP_SENT, credit 8 from ALPHA1");

	s_ntx = 0;
	s_t$feed(l_port, s_dir_accprsp_a1, sizeof(s_dir_accprsp_a1), T$K_CONID_US);
	$CHECK((l_cdt->state == MH$K_CDT_OPEN) && !s_ntx, "ACCP_RSP: the connection is open");

	s_t$feed(l_port, s_dir_look_tape_a1, sizeof(s_dir_look_tape_a1), T$K_CONID_US);
	s_t$feed(l_port, s_dir_look_disk_a1, sizeof(s_dir_look_disk_a1), T$K_CONID_US);
	s_t$feed(l_port, s_dir_look_disk_a1, sizeof(s_dir_look_disk_a1), T$K_CONID_US);
	s_t$feed(l_port, s_dir_look_vaxc_a1, sizeof(s_dir_look_vaxc_a1), T$K_CONID_US);
	$CHECK((s_ntx == 4) && (l_cdt->sendcr == 4) && !l_cdt->pendrec, "4 lookups, 4 answers, each spends a credit and gives one back");
	s_t$cmp(0, s_dir_ans_tape_a2, "answer for MSCP$TAPE");
	s_t$cmp(1, s_dir_ans_disk_a2, "answer for MSCP$DISK");
	s_t$cmp(2, s_dir_ans_disk_a2, "answer for MSCP$DISK again");
	$CHECK((nisca$get16(s_tx[3] + T$K_PAY + T$K_BODY) == 1) && !memcmp(s_tx[3] + T$K_PAY + T$K_BODY + 4, "VMS$VAXcluster  NOT PRESENT HERE", 32),
		"answer for VMS$VAXcluster: NOT PRESENT HERE, the connection manager keeps away");

	s_ntx = 0;
	s_t$feed(l_port, s_dir_crreq_a1, sizeof(s_dir_crreq_a1), T$K_CONID_US);
	$CHECK((s_ntx == 1) && (l_cdt->sendcr == 8), "CR_REQ of 4: CR_RSP, the credit taken");
	s_t$cmp(0, s_dir_crrsp_a2, "CR_RSP of 4");

	s_ntx = 0;
	s_t$feed(l_port, s_dir_discreq_a1, sizeof(s_dir_discreq_a1), T$K_CONID_US);
	$CHECK((s_ntx == 2) && (l_cdt->state == MH$K_CDT_DISC_MTCH), "DISC_REQ: DISC_RSP and our own DISC_REQ");
	s_t$cmp(0, s_dir_discrsp_a2, "DISC_RSP");
	s_t$cmp(1, s_dir_discreq_a2, "our DISC_REQ");

	s_ntx = 0;
	s_t$feed(l_port, s_dir_discrsp_a1, sizeof(s_dir_discrsp_a1), T$K_CONID_US);
	$CHECK((l_cdt->state == MH$K_CDT_FREE) && !s_ntx && !l_vc->reqcdt, "DISC_RSP: the connection is free");

	s_t$feed(l_port, s_dir_look_tape_a1, sizeof(s_dir_look_tape_a1), T$K_CONID_US);
	$CHECK(!s_ntx, "a lookup on the freed connection is dropped");

	/*
	**  MSCP$DISK: one unit, $42$DKA100, 100 MB
	*/
	l_fd = mkstemp(l_img);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 100 * 1024 * 1024), "a sparse image of 100 MB");
	close(l_fd);

	s_t$port(l_port);
	l_vc = l_port->nodes[0].vc;
	s_cfg.alloclass = 42;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img);
	$ASCLEN(&s_cfg.unit[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[0]), l_spec, strlen(l_spec));
	$CHECK((1 & mh$mscp_init(l_port, &s_cfg)) && (l_port->nunit == 1) && (l_port->units[0].size == 204800)
		&& !memcmp(l_port->units[0].unitid, "\x2A\x00\x00\x00\x64\x00\x61\x11", 8), "unit $42$DKA100: 204800 blocks, UNIT_ID 2A 64 1161");

	/*
	**  CON_REQ of DUDRIVER: credit 10, MIN_CR 2, "V5.0 ... + "
	*/
	memset(l_b, ' ', 52);
	nisca$put16(l_b + NISCA$K_SCS_MIN_CR, 2);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	memcpy(l_b + 4, "MSCP$DISK", 9);
	memcpy(l_b + 20, "VMS$DISK_CL_DRVR", 16);
	memcpy(l_b + 36, "V5.0", 4);
	l_b[50] = '+';
	s_t$scs(l_port, NISCA$K_SCS_CON_REQ, 10, 0, l_b, 52);
	l_cdt = &l_vc->cdt[0];
	$CHECK((s_ntx == 2) && (nisca$get16(s_tx[0] + T$K_PAY + T$K_BODY + NISCA$K_SCS_STATUS) == 1), "CON_REQ to MSCP$DISK: CON_RSP NORMAL");
	$CHECK((nisca$get16(s_tx[1] + T$K_PAY + T$K_HDR + NISCA$K_SCS_MTYPE) == NISCA$K_SCS_ACCP_REQ)
		&& (nisca$get16(s_tx[1] + T$K_PAY + T$K_HDR + NISCA$K_SCS_CREDIT) == MH$K_MSCP_CREDITS)
		&& (nisca$get32(s_tx[1] + T$K_PAY + T$K_HDR + NISCA$K_SCS_DST_CONID) == T$K_CONID_A1)
		&& (nisca$get16(s_tx[1] + T$K_PAY + T$K_BODY + NISCA$K_SCS_MIN_CR) == 1)
		&& !memcmp(s_tx[1] + T$K_PAY + T$K_BODY + 4, "VMS$DISK_CL_DRVRMSCP$DISK       ", 32), "ACCP_REQ: credit 16, MIN_CR 1, the names turned round");

	s_ntx = 0;
	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	s_t$scs(l_port, NISCA$K_SCS_ACCP_RSP, 0, T$K_CONID_US, l_b, 4);
	$CHECK((l_cdt->state == MH$K_CDT_OPEN) && (l_cdt->host == 0) && (l_cdt->sendcr == 10), "ACCP_RSP: open, host 0, 10 credits to send");

	/*
	**  STCON #1, 34 octets: host flags ATTN|MISC|THIS, no host timeout
	*/
	s_t$cmd(l_cmd, 34, 0x00010001, 0, MSCP$K_OP_STCON, 0);
	nisca$put16(l_cmd + MSCP$K_CNT_FLGS, 0x00D0);
	l_e = s_t$mscp(l_port, l_cmd, 34, 1);
	$CHECK(l_e && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_SCS_OVHD + 32), "STCON: an end message of 32 octets");
	$CHECK(l_e && (l_e[MSCP$K_OPCODE] == 0x84) && !nisca$get16(l_e + MSCP$K_STATUS) && (l_e[MSCP$K_UNIT] == 42)
		&& (nisca$get16(l_e + MSCP$K_CNT_FLGS) == 0x2004) && (nisca$get16(l_e + MSCP$K_CNT_TMO) == 20) && (l_e[MSCP$K_CNT_HVR] == 5)
		&& !memcmp(l_e + MSCP$K_CNT_ID, "\xFD\xFF\x00\x00\x00\x00\x04\x01", 8) && (nisca$get32(l_e + MSCP$K_MAXBCNT) == 0x20000),
		"STCON: SUCC, CNT_ALCS 42, MLTHS|LOAD, 20 s, HVR 5, CNT_ID 65533 EMULA, 128 KB");
	$CHECK(nisca$get16(s_tx[0] + T$K_PAY + T$K_HDR + NISCA$K_SCS_CREDIT) == 1, "... and it gives back the credit of the command");

	s_t$cmd(l_cmd, 34, 0x00020001, 0, MSCP$K_OP_STCON, 0);
	nisca$put16(l_cmd + MSCP$K_VERSION, 1);
	l_e = s_t$mscp(l_port, l_cmd, 34, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == 0x0C01) && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_SCS_OVHD + 36),
		"STCON of version 1: ICMD on +0C, 36 octets");

	/*
	**  The walk: unit 1 NXUNT -> 0x4000, AVLBL; 0x4001 NXUNT -> the end
	*/
	s_t$cmd(l_cmd, 28, 0x00030001, 1, MSCP$K_OP_GTUNT, MSCP$M_MD_NXUNT);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_SCS_OVHD + 52) && (nisca$get16(l_e + MSCP$K_UNIT) == 0x4000)
		&& (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_AVLBL), "GUS 1 NXUNT: unit 0x4000, AVLBL, 52 octets");
	$CHECK(l_e && !memcmp(l_e + MSCP$K_UNIT_ID, l_port->units[0].unitid, 8) && (nisca$get32(l_e + MSCP$K_MEDIA_ID) == MH$K_MEDIA_RZ29)
		&& (nisca$get16(l_e + MSCP$K_TRACK) == 255) && (nisca$get16(l_e + MSCP$K_GROUP) == 255) && (nisca$get16(l_e + MSCP$K_CYLINDER) == 1)
		&& (nisca$get16(l_e + MSCP$K_LOAD_AVAIL) == MH$K_MSCP_LOAD), "... UNIT_ID, RZ29, 255/255/1, LOAD_AVAIL");

	s_t$cmd(l_cmd, 28, 0x00040001, 0x4001, MSCP$K_OP_GTUNT, MSCP$M_MD_NXUNT);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && !nisca$get16(l_e + MSCP$K_UNIT) && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_OFFLN), "GUS 0x4001 NXUNT: the end, UNIT 0, clean OFFLN");

	s_t$cmd(l_cmd, 28, 0x00050001, 0, MSCP$K_OP_GTUNT, 0);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && !nisca$get16(l_e + MSCP$K_UNIT) && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_OFFLN), "GUS unit 0, the sign of life: OFFLN");

	/*
	**  0x7FFF: by UNIT_ID; not found - OFFLN, 0x7FFF left
	*/
	s_t$cmd(l_cmd, 28, 0x00060001, MSCP$K_SLUN_RSVP, MSCP$K_OP_GTUNT, 0);
	memcpy(l_cmd + MSCP$K_UNIT_ID, l_port->units[0].unitid, 8);
	l_e = s_t$mscp(l_port, l_cmd, 28, 3);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_UNIT) == 0x4000) && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_AVLBL), "GUS 0x7FFF of our UNIT_ID: 0x4000");

	l_cmd[MSCP$K_UNIT_ID + 4] = 0x65;
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_UNIT) == MSCP$K_SLUN_RSVP) && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_OFFLN), "GUS 0x7FFF of DKA101: OFFLN");

	/*
	**  ONLINE, ONLINE again, GUS, AVAILABLE
	*/
	s_t$cmd(l_cmd, 36, 0x00070001, 0x4000, MSCP$K_OP_ONLIN, 0);
	l_e = s_t$mscp(l_port, l_cmd, 36, 1);
	$CHECK(l_e && !nisca$get16(l_e + MSCP$K_STATUS) && (nisca$get32(l_e + MSCP$K_UNT_SIZE) == 204800)
		&& (nisca$get32(l_e + MSCP$K_MEDIA_ID) == MH$K_MEDIA_RZ29) && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_SCS_OVHD + 44),
		"ONLINE: SUCC, 204800 blocks, RZ29, 44 octets");
	l_e = s_t$mscp(l_port, l_cmd, 36, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_ALONL), "ONLINE again: SUCC + ALONL");

	s_t$cmd(l_cmd, 28, 0x00080001, 0x4000, MSCP$K_OP_GTUNT, 0);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_SUCC), "GUS 0x4000 of an ONLINE unit: SUCC");

	s_t$cmd(l_cmd, 12, 0x00090001, 0x4000, MSCP$K_OP_AVAIL, 0);
	l_e = s_t$mscp(l_port, l_cmd, 12, 1);
	s_t$cmd(l_cmd, 28, 0x000A0001, 0x4000, MSCP$K_OP_GTUNT, 0);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_AVLBL), "AVAILABLE, then GUS: AVLBL");

	/*
	**  The checks of a command; READ of a unit not ONLINE (T_BLK moves data)
	*/
	s_t$cmd(l_cmd, 32, 0x000B0001, 0x4000, MSCP$K_OP_READ, 0);
	nisca$put32(l_cmd + MSCP$K_BYTE_CNT, 512);
	l_e = s_t$mscp(l_port, l_cmd, 32, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_AVLBL) && (l_e[MSCP$K_OPCODE] == 0xA1)
		&& (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_SCS_OVHD + 32), "READ of a unit not ONLINE: AVLBL, 32 octets");

	s_t$cmd(l_cmd, 12, 0x000B0002, 0x4000, MSCP$K_OP_REPLC, 0);
	l_e = s_t$mscp(l_port, l_cmd, 32, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == 0x0801), "REPLACE: ICMD on the opcode");

	s_t$cmd(l_cmd, 28, 0x000C0001, 0x4000, MSCP$K_OP_GTUNT, 0);
	l_cmd[MSCP$K_FLAGS] = 1;
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == 0x0901), "FLAGS not 0: ICMD on +09");

	s_t$cmd(l_cmd, 28, 0x000D0001, 0x4000, MSCP$K_OP_GTUNT, 0x0004);
	l_e = s_t$mscp(l_port, l_cmd, 28, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == 0x0A01), "a modifier GUS does not take: ICMD on +0A");

	s_t$cmd(l_cmd, 8, 0x000E0001, 0x4000, MSCP$K_OP_GTUNT, 0);
	l_e = s_t$mscp(l_port, l_cmd, 8, 1);
	$CHECK(!l_e && !s_ntx, "a packet shorter than 12: dropped");

	s_t$cmd(l_cmd, 12, 0x000F0001, 0x4000, MSCP$K_OP_ONLIN, 0);
	l_e = s_t$mscp(l_port, l_cmd, 12, 1);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == 0x0001), "ONLINE of 12 octets: ICMD on the length");

	/*
	**  Credits: every command gave one back, the client keeps its 10 + 3;
	**  with none left an end waits, the credit of the next command lets it go
	*/
	$CHECK(l_cdt->sendcr == 13, "a credit back with every command, 2 more once, 1 of a dropped packet: 13 to send");

	l_cdt->sendcr = 0;
	s_t$cmd(l_cmd, 28, 0x00100001, 0, MSCP$K_OP_GTUNT, 0);
	l_e = s_t$mscp(l_port, l_cmd, 28, 0);
	$CHECK(!l_e && !s_ntx && (l_cdt->nq == 1), "no credit: the end waits");

	s_t$cmd(l_cmd, 28, 0x00110001, 0, MSCP$K_OP_GTUNT, 0);
	s_ntx = 0;
	s_t$scs(l_port, NISCA$K_SCS_APPL_MSG, 2, T$K_CONID_US, l_cmd, 28);
	$CHECK((s_ntx == 2) && !l_cdt->nq && (nisca$get32(s_t$end(0) + MSCP$K_CMD_REF) == 0x00100001)
		&& (nisca$get32(s_t$end(1) + MSCP$K_CMD_REF) == 0x00110001), "a credit of 2: both ends go, in order");

	/*
	**  The client disconnects: the host number is free, the unit forgets it
	*/
	s_t$cmd(l_cmd, 36, 0x00120001, 0x4000, MSCP$K_OP_ONLIN, 0);
	s_t$mscp(l_port, l_cmd, 36, 4);
	$CHECK(l_port->units[0].online[0] & 1, "ONLINE to host 0");

	s_ntx = 0;
	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 1);
	s_t$scs(l_port, NISCA$K_SCS_DISC_REQ, 0, T$K_CONID_US, l_b, 4);
	$CHECK((s_ntx == 2) && !(l_port->units[0].online[0] & 1) && !(l_port->hosts[0] & 1) && (l_cdt->state == MH$K_CDT_DISC_MTCH),
		"DISC_REQ: DISC_RSP, our DISC_REQ, the unit and the host number free");

	s_t$scs(l_port, NISCA$K_SCS_DISC_RSP, 0, T$K_CONID_US, l_b, 0);
	$CHECK(l_cdt->state == MH$K_CDT_FREE, "DISC_RSP: the connection is free");

	/*
	**  A CON_REQ to MSCP$DISK when no unit is served is refused
	*/
	l_port->nunit = 0;
	memset(l_b, ' ', 52);
	memcpy(l_b + 4, "MSCP$DISK", 9);
	s_ntx = 0;
	s_t$scs(l_port, NISCA$K_SCS_CON_REQ, 10, 0, l_b, 52);
	$CHECK((s_ntx == 1) && (nisca$get16(s_tx[0] + T$K_PAY + T$K_BODY + NISCA$K_SCS_STATUS) == NISCA$K_SCS_STNOMAT), "no units: MSCP$DISK, STNOMAT");

	unlink(l_img);

	printf("T_SCS: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
