#define	__MODULE__	"T_BOOT"
#define	__IDENT__	"X01-21"
#define	__REV__		"1.21.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_boot.c
**
**  ABSTRACT:	Unit tests of the server of the system disk of a satellite:
**		the server stands for MHUB (65533, $42$DKA100 writable,
**		$42$DKA200 read only) and takes in what the boot driver of
**		the satellite SAT1 (1031) sends - SOLICIT_SRV, VACK, STACK,
**		the SCS connection to MSCP$DISK and the first MSCP commands -
**		then the runtime PEDRIVER of SAT1 takes the channel over by
**		CCSTART, and SAT1 boots again.
**
**		The frames of the boot driver are made here as it builds them:
**		CC V1.3 with SERVICE, TR V1.3 (9 octets, 16-bit numbers).
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  25-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		25-OCT-2026	RRL	X01-21 : Initial revision.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>

#include	"mh_def.h"
#include	"mscp.h"

#define	T$K_MAXTX	32

static	uint8_t		s_tx [T$K_MAXTX] [NISCA$K_MAXFRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

/*
**  Offsets in our frames: the CC message; TR V1.5, then the PPC opcode,
**  W LENGTH, W PPD MTYPE, the data or the SCS header and the body
*/
#define	T$K_DX		NISCA$K_SZ_LANHDR
#define	T$K_CC		(NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX)
#define	T$K_TR		T$K_CC
#define	T$K_PAY		(T$K_TR + NISCA$K_TR_LEN_V15)
#define	T$K_PPD		1
#define	T$K_PDATA	(T$K_PPD + 4)		/* START/STACK data			*/
#define	T$K_HDR		(T$K_PPD + 4)		/* ... or the SCS header		*/
#define	T$K_BODY	(T$K_HDR + NISCA$K_SCS_BODY)

#define	T$K_CC_LEN	0x65			/* What BUILD_CC_HEADER of the boot driver makes	*/
#define	T$K_SAT_SYSID	1031
#define	T$K_SAT_CONID	0x00010001U		/* The SRC_CONID of its CON_REQ template	*/

static	const uint8_t	s_satmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x43, 0x01 };
static	const uint8_t	s_hubmac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x02 };
static	const char	s_auth [] = "5A3C9E1700000000";

static	uint16_t	s_seq;			/* SEQ of the next sequenced frame of SAT1	*/



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
	fprintf(stderr, "T_BOOT:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The XMIT hook of the test port: record the frame.
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
**	Run the 20 ms clock of the port.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_ticks		How many ticks
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$run	(
		MH$PORT *	a_port,
		unsigned	a_ticks
			)
{
	for ( unsigned i = 0; i < a_ticks; i++ )
		mh$cc_tick(a_port);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The LAN and DX headers of a frame of SAT1.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context: the group, the server
**	a_f		Output: the frame
**	a_mcast		1 - to the group, 0 - to the server
**	a_dlen		Octets after the length word
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$hdr	(
		MH$PORT *	a_port,
		uint8_t *	a_f,
		int		a_mcast,
		unsigned	a_dlen
			)
{
uint8_t	l_dxdst [NISCA$K_SZ_MAC];

	if ( a_mcast )
		memcpy(l_dxdst, a_port->gmac, NISCA$K_SZ_MAC);
	else	nisca$node_mac(a_port->sysid, l_dxdst);

	memcpy(a_f, a_mcast ? a_port->gmac : s_hubmac, NISCA$K_SZ_MAC);
	memcpy(a_f + NISCA$K_SZ_MAC, s_satmac, NISCA$K_SZ_MAC);
	a_f[12] = 0x60;
	a_f[13] = 0x07;
	nisca$put16(a_f + NISCA$K_SZ_ETH, (uint16_t) a_dlen);

	memcpy(a_f + T$K_DX + NISCA$K_DX_DST_LO, l_dxdst, NISCA$K_SZ_MAC);
	nisca$put16(a_f + T$K_DX + NISCA$K_DX_GROUP, a_port->group);
	nisca$node_mac(T$K_SAT_SYSID, a_f + T$K_DX + NISCA$K_DX_SRC_LO);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a CC message of SAT1, as its boot driver (or, for CCSTART, its
**	runtime PEDRIVER) sends it.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_type		NISCA$K_CC_*
**	a_svc		SERVICE of a SOLICIT_SRV, ASCIZ; NULL - none
**	a_chseq		LCL_CHAN_SEQ_NUM
**	a_auth		1 - the right password, 0 - a wrong one
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
**--
*/
static	int	s_t$cc	(
		MH$PORT *	a_port,
		unsigned	a_type,
	const	char *		a_svc,
		uint16_t	a_chseq,
		int		a_auth
			)
{
uint8_t		l_f [NISCA$K_MAXFRAME] = {0}, *l_cc = l_f + T$K_CC;
int		l_mcast = (a_type == NISCA$K_CC_HELLO) || (a_type == NISCA$K_CC_SOLICIT_SRV);

	s_t$hdr(a_port, l_f, l_mcast, NISCA$K_SZ_DX + T$K_CC_LEN);

	l_cc[NISCA$K_CC_MSG] = (uint8_t) (NISCA$M_CC_CCFLG | NISCA$M_CC_OK_XMT | a_type);

	if ( a_type != NISCA$K_CC_HELLO )
		{
		l_cc[NISCA$K_CC_MSG] |= NISCA$M_CC_AUTHORIZE;
		mh$hex2bin(s_auth, 16, l_cc + NISCA$K_CC_AUTHORIZE, MH$K_SZ_AUTH);

		if ( !a_auth )
			l_cc[NISCA$K_CC_AUTHORIZE] ^= 0xFF;
		}

	nisca$put32(l_cc + NISCA$K_CC_MAINT, 0x80000008U);
	l_cc[NISCA$K_CC_MINOR] = 3;
	l_cc[NISCA$K_CC_MAJOR] = NISCA$K_MAJOR;
	l_cc[NISCA$K_CC_NODENAME] = NISCA$K_SZ_NODENAME;
	memcpy(l_cc + NISCA$K_CC_NODENAME + 1, "SAT1  ", NISCA$K_SZ_NODENAME);
	nisca$put32(l_cc + NISCA$K_CC_PORT_FCN, 0x83FF0180U);
	nisca$put32(l_cc + NISCA$K_CC_STA_INFO, 0x400);
	l_cc[NISCA$K_CC_PIPE_QUOTA] = 8;
	nisca$put16(l_cc + NISCA$K_CC_P_SRV, 3);

	if ( a_svc )
		{
		l_cc[NISCA$K_CC_SERVICE] = (uint8_t) strlen(a_svc);
		memcpy(l_cc + NISCA$K_CC_SERVICE + 1, a_svc, strlen(a_svc));
		}

	nisca$put16(l_cc + NISCA$K_CC_LCL_CHSEQ, a_chseq);
	nisca$put16(l_cc + NISCA$K_CC_BUS_BUFSZ, 1426);
	l_cc[NISCA$K_CC_DEVNAME] = 3;
	memcpy(l_cc + NISCA$K_CC_DEVNAME + 1, "EW?", 3);
	memcpy(l_cc + NISCA$K_CC_LAN_HWADDR, s_satmac, NISCA$K_SZ_MAC);
	l_cc[NISCA$K_CC_NODE_HWTYPE] = 1;

	return	mh$cc_input(a_port, l_f, NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + T$K_CC_LEN);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed a TR packet of SAT1, the header of V1.3: a datagram, or a
**	sequenced message acknowledging everything we have sent.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_opc		NISCA$K_PPC_SNT_DG or _SNT_SEQ
**	a_mtype		PPD MTYPE
**	a_data		What follows MTYPE
**	a_dlen		Its length
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
**--
*/
static	int	s_t$tr	(
		MH$PORT *	a_port,
		unsigned	a_opc,
		unsigned	a_mtype,
	const	uint8_t *	a_data,
		unsigned	a_dlen
			)
{
uint8_t		l_f [NISCA$K_MAXFRAME] = {0}, *l_tr = l_f + T$K_TR, *l_p = l_tr + NISCA$K_TR_LEN_V13;
unsigned	l_len = NISCA$K_SZ_DX + NISCA$K_TR_LEN_V13 + 1 + 4 + a_dlen;
MH$NODE		*l_node = &a_port->nodes[0];

	s_t$hdr(a_port, l_f, 0, l_len);

	l_tr[NISCA$K_TR_FLAG] = NISCA$M_TR_DATA;
	l_tr[NISCA$K_TR_PAD] = 3;
	nisca$put16(l_tr + NISCA$K_TR_RMT_CHSEQ, l_node->lcl_chseq);
	l_tr[NISCA$K_TR_PAD_DLEN] = 3;

	if ( a_opc == NISCA$K_PPC_SNT_SEQ )
		{
		l_tr[NISCA$K_TR_FLAG] |= NISCA$M_TR_SEQ | NISCA$M_TR_ACK;
		nisca$put16(l_tr + NISCA$K_TR_SEQ, s_seq++);
		nisca$put16(l_tr + NISCA$K_TR_ACK, (uint16_t) (l_node->vc->nsu - 1));
		}

	l_p[0] = (uint8_t) a_opc;
	nisca$put16(l_p + T$K_PPD, (uint16_t) (2 + a_dlen));
	nisca$put16(l_p + T$K_PPD + 2, (uint16_t) a_mtype);
	memcpy(l_p + T$K_PDATA, a_data, a_dlen);

	return	mh$cc_input(a_port, l_f, NISCA$K_SZ_LANHDR + l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The START/STACK data of SAT1: SYSTEMID of 2 octets, MAXDG and MAXMSG
**	148, SWTYPE 'VMS ', SWVERS - 'BOOT' of the boot driver, 'V8.3' of
**	VMS.
**
**  FORMAL PARAMETERS:
**
**	a_st		Output: NISCA$K_ST_LEN octets
**	a_swvers	SWVERS, 4 characters
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$sb	(
		uint8_t *	a_st,
	const	char *		a_swvers
			)
{
	memset(a_st, 0, NISCA$K_ST_LEN);
	nisca$put16(a_st + NISCA$K_ST_SYSTEMID, T$K_SAT_SYSID);
	a_st[NISCA$K_ST_PROTOCOL] = 1;
	nisca$put16(a_st + NISCA$K_ST_MAXDG, 148);
	nisca$put16(a_st + NISCA$K_ST_MAXMSG, 148);
	memcpy(a_st + NISCA$K_ST_SWTYPE, "VMS ", 4);
	memcpy(a_st + NISCA$K_ST_SWVERS, a_swvers, 4);
	memcpy(a_st + NISCA$K_ST_HWTYPE, "ALPH", 4);
	memset(a_st + NISCA$K_ST_NODENAME, ' ', MH$K_SZ_SCSNODE);
	memcpy(a_st + NISCA$K_ST_NODENAME, "SAT1", 4);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Feed an SCS message of SAT1.
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
uint8_t	l_h [NISCA$K_SCS_BODY + 128];

	nisca$put16(l_h + NISCA$K_SCS_MTYPE, (uint16_t) a_mtype);
	nisca$put16(l_h + NISCA$K_SCS_CREDIT, (uint16_t) (int16_t) a_credit);
	nisca$put32(l_h + NISCA$K_SCS_DST_CONID, a_dst);
	nisca$put32(l_h + NISCA$K_SCS_SRC_CONID, T$K_SAT_CONID);
	memcpy(l_h + NISCA$K_SCS_BODY, a_body, a_blen);

	return	s_t$tr(a_port, NISCA$K_PPC_SNT_SEQ, NISCA$K_PPD_SCS_MSG, l_h, NISCA$K_SCS_BODY + a_blen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send one MSCP command of the boot driver - CRN 1, CREDIT 1 - and take
**	the end message.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cmd		The command
**	a_len		Its length
**	a_dst		Our CONID
**
**  RETURN VALUE:
**	The end message; NULL - not one frame came out.
**--
*/
static	const uint8_t	*s_t$mscp	(
		MH$PORT *	a_port,
	const	uint8_t *	a_cmd,
		unsigned	a_len,
		uint32_t	a_dst
			)
{
	s_ntx = 0;
	s_t$scs(a_port, NISCA$K_SCS_APPL_MSG, 1, a_dst, a_cmd, a_len);

	return	(s_ntx == 1) ? s_tx[0] + T$K_PAY + T$K_BODY : NULL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is frame A_I of S_TX a CC message of the type to SAT1?
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_i		Index in S_TX
**	a_type		NISCA$K_CC_*
**
**  RETURN VALUE:
**	1 - it is; 0 - it is not.
**--
*/
static	int	s_t$iscc	(
		MH$PORT *	a_port,
		unsigned	a_i,
		unsigned	a_type
			)
{
uint8_t	l_dx [NISCA$K_SZ_MAC];

	nisca$node_mac(T$K_SAT_SYSID, l_dx);

	return	(a_i < s_ntx) && !memcmp(s_tx[a_i], s_satmac, NISCA$K_SZ_MAC) && !memcmp(s_tx[a_i] + T$K_DX, l_dx, NISCA$K_SZ_MAC)
		&& (s_tx[a_i][T$K_CC] == (NISCA$M_CC_CCFLG | NISCA$M_CC_OK_XMT | NISCA$M_CC_AUTHORIZE | a_type))
		&& !memcmp(s_tx[a_i] + T$K_CC + NISCA$K_CC_AUTHORIZE, a_port->auth, MH$K_SZ_AUTH);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The PPD MTYPE of frame A_I of S_TX, a datagram of ours; -1 - not one.
**
**  FORMAL PARAMETERS:
**
**	a_i		Index in S_TX
**
**  RETURN VALUE:
**	The MTYPE, or -1.
**--
*/
static	int	s_t$ppd	(
		unsigned	a_i
			)
{
	if ( (a_i >= s_ntx) || (s_tx[a_i][T$K_CC] & NISCA$M_CC_CCFLG) || (s_tx[a_i][T$K_PAY] != NISCA$K_PPC_SNT_DG) )
		return	-1;

	return	nisca$get16(s_tx[a_i] + T$K_PAY + T$K_PPD + 2);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is $42$DKA100 ONLINE to a host?
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_host		The host number
**
**  RETURN VALUE:
**	1 - it is; 0 - it is not.
**--
*/
static	int	s_t$online	(
	const	MH$PORT *	a_port,
		unsigned	a_host
			)
{
	return	(a_port->units[0].online[a_host / 8] >> (a_host % 8)) & 1;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A port made up as MHUB, announcing, without a socket, serving
**	$42$DKA100 and $42$DKA200 (read only) from the image.
**
**  FORMAL PARAMETERS:
**
**	a_port		Output: the port context
**	a_cfg		The configuration, its units filled in
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_TX is emptied; S_SEQ - the next SEQ of SAT1 is 1.
**
**  RETURN VALUE:
**	The condition value of MH$MSCP_INIT.
**--
*/
static	int	s_t$port	(
		MH$PORT *	a_port,
		MH$CFG *	a_cfg
			)
{
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
	memcpy(a_port->mac, s_hubmac, NISCA$K_SZ_MAC);
	memcpy(a_port->ifname, "mh0", 4);
	nisca$group_mac(a_port->group, a_port->gmac);
	mh$hex2bin(s_auth, 16, a_port->auth, MH$K_SZ_AUTH);

	a_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memcpy(a_port->ccname + 1, "MHUB  ", NISCA$K_SZ_NODENAME);
	a_port->devname[0] = 3;
	memcpy(a_port->devname + 1, "mh0", 3);

	s_ntx = 0;
	s_seq = 1;

	return	mh$mscp_init(a_port, a_cfg);
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
MH$NODE		*l_node;
MH$VC		*l_vc;
uint8_t		l_st [NISCA$K_ST_LEN], l_start [NISCA$K_ST_LEN], l_b [64], l_cmd [36];
char		l_spec [64], l_img [] = "t_boot_XXXXXX", l_img2 [] = "t_boot_XXXXXX";
const uint8_t	*l_e;
uint32_t	l_conid;
uint16_t	l_chseq;
unsigned	l_unit, l_host;
uint32_t	l_logs;
int		l_fd;

	mh$inimsg();

	l_fd = mkstemp(l_img);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 16 * 1024 * 1024), "a sparse image of 16 MB");
	close(l_fd);
	l_fd = mkstemp(l_img2);
	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 16 * 1024 * 1024), "... and another");
	close(l_fd);

	s_cfg.alloclass = 42;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img);
	$ASCLEN(&s_cfg.unit[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[0]), l_spec, strlen(l_spec));
	snprintf(l_spec, sizeof(l_spec), "dka200,%s,RO", l_img2);
	$ASCLEN(&s_cfg.unit[1]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[1]), l_spec, strlen(l_spec));

	$CHECK((1 & s_t$port(l_port, &s_cfg)) && (l_port->nunit == 2), "MHUB serves $42$DKA100 and $42$DKA200");

	/*
	**  The name of the service, as SCS$MSCP_CHECK_SERVICE: with and
	**  without the underscore and the colon, in any case
	*/
	{
	static	const char	*s_good [] = { "_$42$DKA100:", "$42$DKA100", "_$42$dka100", "$42$DKA100:" };
	static	const char	*s_bad [] = { "_$42$DKA10:", "_$42$DKA1000:", "_$43$DKA100:", "", "_MHUB$DKA100:" };
	uint8_t	l_svc [NISCA$K_SZ_SERVICE];

	for ( unsigned i = 0; i < $ARRSZ(s_good); i++ )
		{
		l_svc[0] = (uint8_t) strlen(s_good[i]);
		memcpy(l_svc + 1, s_good[i], l_svc[0]);
		l_unit = 99;
		$CHECK((mh$mscp_check_service(l_port, l_svc, &l_unit) == STS$K_SUCCESS) && !l_unit, s_good[i]);
		}

	for ( unsigned i = 0; i < $ARRSZ(s_bad); i++ )
		{
		l_svc[0] = (uint8_t) strlen(s_bad[i]);
		memcpy(l_svc + 1, s_bad[i], l_svc[0]);
		$CHECK(mh$mscp_check_service(l_port, l_svc, &l_unit) == STS$K_ERROR, s_bad[i]);
		}

	l_svc[0] = 12;
	memcpy(l_svc + 1, "_$42$DKA200:", 12);
	$CHECK((mh$mscp_check_service(l_port, l_svc, &l_unit) == STS$K_WARN) && (l_unit == 1), "_$42$DKA200: read only, WARN");
	}

	/*
	**  SOLICIT_SRV of a disk not ours, of the read-only one, with a wrong
	**  password: no answer, the channel stays closed
	*/
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA300:", 0, 1);
	l_node = &l_port->nodes[0];
	$CHECK((l_port->nnode == 1) && !strcmp(l_node->name, "SAT1") && (l_node->sysid == T$K_SAT_SYSID), "SOLICIT_SRV: SAT1 (1031) is known");
	$CHECK((l_node->state == MH$K_CH_CLOSED) && !l_node->boot && (l_node->svclog == -1) && !s_ntx, "... for $42$DKA300: not ours, no answer");

	s_t$run(l_port, 200);
	$CHECK(!s_ntx, "... nor 4 s later");

	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA200:", 0, 1);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && (l_node->svclog == -2) && !s_ntx, "... for the read-only $42$DKA200: no answer");

	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 0);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && !s_ntx && (l_port->rx_auth == 1), "... with a wrong password: AUTHFAIL, no answer");

	/*
	**  SOLICIT_SRV of $42$DKA100: SOLICIT_DLY, our VERF 0 - 0.5 s later
	*/
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((l_node->state == MH$K_CH_SOLICIT_DLY) && l_node->boot && (l_node->svclog == 1) && !s_ntx, "SOLICIT_SRV of $42$DKA100: SOLICIT_DLY, BOOT");
	$CHECK((l_node->bufsz == 1426) && (l_node->pipe_quota == 8) && (l_node->minor == 3), "... BUS_BUFFER_SIZE 1426, PIPE_QUOTA 8 of the SOLICIT_SRV");

	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((l_node->state == MH$K_CH_SOLICIT_DLY) && !s_ntx, "... again in SOLICIT_DLY: nothing");

	s_t$run(l_port, MH$K_SOL_DLY_MAX + 1);
	$CHECK((s_ntx == 1) && s_t$iscc(l_port, 0, NISCA$K_CC_VERF) && (l_node->state == MH$K_CH_CCSTART_REC),
		"0.5 s at most: our VERF to SAT1 itself, CCSTART_REC");
	$CHECK((s_txlen[0] >= T$K_CC + NISCA$K_CC_LEN_V13) && (nisca$get16(s_tx[0] + T$K_CC + NISCA$K_CC_LCL_CHSEQ) == l_node->lcl_chseq)
		&& (s_tx[0][T$K_CC + NISCA$K_CC_MAJOR] == 1), "... V1.3, our channel sequence number");

	s_ntx = 0;
	l_logs = l_node->sollogs;
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((s_ntx == 1) && s_t$iscc(l_port, 0, NISCA$K_CC_VERF) && (l_node->state == MH$K_CH_CCSTART_REC), "SOLICIT_SRV again: our VERF again");
	$CHECK(l_node->sollogs == l_logs, "... not told of again");

	/*
	**  VACK: the channel is open, our START goes at once
	*/
	s_ntx = 0;
	s_t$cc(l_port, NISCA$K_CC_VACK, NULL, 1, 1);
	l_vc = l_node->vc;
	$CHECK((l_node->state == MH$K_CH_OPEN) && (l_node->rmt_chseq == 1), "VACK: OPEN, its channel sequence number 1");
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_ST_SENT) && (s_ntx == 1) && (s_t$ppd(0) == NISCA$K_PPD_START), "... our START at once, ST_SENT");
	$CHECK((s_ntx == 1) && (nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_RMT_CHSEQ) == 1) && !(s_tx[0][T$K_TR + NISCA$K_TR_FLAG] & NISCA$M_TR_SEQ),
		"... a datagram, RMT_CHAN_SEQ_NUM 1 - the boot driver checks it");
	$CHECK((nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) == NISCA$K_ST_LENGTH)
		&& (nisca$get16(s_tx[0] + T$K_PAY + T$K_PDATA + NISCA$K_ST_SYSTEMID) == 65533)
		&& !memcmp(s_tx[0] + T$K_PAY + T$K_PDATA + NISCA$K_ST_NODENAME, "MHUB    ", 8)
		&& !memcmp(s_tx[0] + T$K_PAY + T$K_PDATA + NISCA$K_ST_SWTYPE, MH$T_PPD_SWTYPE, 4), "... SYSTEMID 65533, MHUB, MSCH");
	memcpy(l_start, s_tx[0] + T$K_PAY + T$K_PDATA, NISCA$K_ST_LEN);

	s_ntx = 0;
	s_t$run(l_port, MH$K_PPD_START_TMO - 1);
	$CHECK(!s_ntx, "no STACK for 0.98 s: nothing yet");
	s_t$run(l_port, 1);
	$CHECK((s_ntx == 1) && (s_t$ppd(0) == NISCA$K_PPD_START), "1 s: our START again");

	/*
	**  STACK of the boot driver: our ACK, the path is open
	*/
	s_ntx = 0;
	s_t$sb(l_st, "BOOT");
	s_t$tr(l_port, NISCA$K_PPC_SNT_DG, NISCA$K_PPD_STACK, l_st, sizeof(l_st));
	$CHECK((s_ntx == 1) && (s_t$ppd(0) == NISCA$K_PPD_ACK) && (l_vc->pbstate == MH$K_PB_OPEN), "STACK: our ACK, OPEN");
	$CHECK(!strcmp(l_vc->swvers, "BOOT") && !strcmp(l_vc->swtype, "VMS") && (l_vc->sysid == T$K_SAT_SYSID) && (l_vc->maxmsg == 148),
		"... the SB: VMS BOOT, 1031, MAXMSG 148");

	s_ntx = 0;
	s_t$run(l_port, 2 * MH$K_PPD_START_TMO);
	$CHECK(!s_ntx, "no more START once open");

	s_t$tr(l_port, NISCA$K_PPC_SNT_DG, NISCA$K_PPD_STACK, l_st, sizeof(l_st));
	$CHECK((s_ntx == 1) && (s_t$ppd(0) == NISCA$K_PPD_ACK) && (l_vc->pbstate == MH$K_PB_OPEN), "STACK again: our ACK was lost, ACK again");

	s_ntx = 0;
	memcpy(l_b, l_st, sizeof(l_st));
	memcpy(l_b + NISCA$K_ST_NODENAME, "SAT2    ", 8);
	l_node->vc->pbstate = MH$K_PB_ST_SENT;
	s_t$tr(l_port, NISCA$K_PPC_SNT_DG, NISCA$K_PPD_STACK, l_b, NISCA$K_ST_LEN);
	$CHECK(!s_ntx && (l_vc->pbstate == MH$K_PB_ST_SENT), "a STACK about another node: ignored");
	l_node->vc->pbstate = MH$K_PB_OPEN;

	/*
	**  The connection of the boot driver: its CON_REQ template, credit 3,
	**  MIN_CR 1; it answers our ACCP_REQ with our credit echoed
	*/
	l_vc->window = l_vc->maxwin;

	memset(l_b, ' ', 52);
	nisca$put16(l_b + NISCA$K_SCS_MIN_CR, 1);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, 0);
	memcpy(l_b + 4, "MSCP$DISK", 9);
	memcpy(l_b + 20, "VMS$DISK_CL_DRVR", 16);
	memcpy(l_b + 36, "V5.0", 4);
	l_b[50] = '+';
	s_ntx = 0;
	s_t$scs(l_port, NISCA$K_SCS_CON_REQ, 3, 0, l_b, 52);
	$CHECK((s_ntx == 2) && (nisca$get16(s_tx[0] + T$K_PAY + T$K_BODY + NISCA$K_SCS_STATUS) == NISCA$K_SCS_STNORMAL)
		&& (nisca$get16(s_tx[1] + T$K_PAY + T$K_HDR + NISCA$K_SCS_MTYPE) == NISCA$K_SCS_ACCP_REQ), "CON_REQ to MSCP$DISK: CON_RSP NORMAL, ACCP_REQ");
	l_conid = nisca$get32(s_tx[1] + T$K_PAY + T$K_HDR + NISCA$K_SCS_SRC_CONID);

	memset(l_b, 0, 4);
	nisca$put16(l_b + NISCA$K_SCS_STATUS, NISCA$K_SCS_STNORMAL);
	s_ntx = 0;
	s_t$scs(l_port, NISCA$K_SCS_ACCP_RSP, MH$K_MSCP_CREDITS, l_conid, l_b, 4);
	$CHECK((l_vc->cdt[0].state == MH$K_CDT_OPEN) && (l_vc->cdt[0].sendcr == 3), "ACCP_RSP: open, 3 credits - not our 16 it sends back");
	l_host = l_vc->cdt[0].host;

	/*
	**  STCON, 32 octets, host timeout 255
	*/
	memset(l_cmd, 0, sizeof(l_cmd));
	nisca$put32(l_cmd + MSCP$K_CMD_REF, 1);
	l_cmd[MSCP$K_OPCODE] = MSCP$K_OP_STCON;
	nisca$put16(l_cmd + 0x0E, 255);
	l_e = s_t$mscp(l_port, l_cmd, 32, l_conid);
	$CHECK(l_e && (l_e[MSCP$K_OPCODE] == (0x80 | MSCP$K_OP_STCON)) && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_SUCC), "STCON: SUCC");
	$CHECK(l_e && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) <= 148), "... not longer than its MAXMSG");

	/*
	**  GUS 0x7FFF by the UNIT_ID of MOP: AVLBL - also while another host
	**  (the VMS node that has the disk mounted) has the unit ONLINE
	*/
	l_port->units[0].online[(l_host + 5) / 8] |= (uint8_t) (1 << ((l_host + 5) % 8));
	l_port->hosts[(l_host + 5) / 8] |= (uint8_t) (1 << ((l_host + 5) % 8));

	memset(l_cmd, 0, sizeof(l_cmd));
	nisca$put32(l_cmd + MSCP$K_CMD_REF, 1);
	nisca$put16(l_cmd + MSCP$K_UNIT, MSCP$K_SLUN_RSVP);
	l_cmd[MSCP$K_OPCODE] = MSCP$K_OP_GTUNT;
	memcpy(l_cmd + MSCP$K_UNIT_ID, l_port->units[0].unitid, 8);
	l_e = s_t$mscp(l_port, l_cmd, 32, l_conid);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_AVLBL) && (nisca$get16(l_e + MSCP$K_UNIT) == 0x4000),
		"GUS 0x7FFF of the UNIT_ID: AVLBL exactly, unit 0x4000, another host ONLINE");
	$CHECK(l_e && (nisca$get16(s_tx[0] + T$K_PAY + T$K_PPD) <= 148), "... not longer than its MAXMSG");

	memset(l_cmd, 0, sizeof(l_cmd));
	nisca$put32(l_cmd + MSCP$K_CMD_REF, 1);
	nisca$put16(l_cmd + MSCP$K_UNIT, 0x4000);
	l_cmd[MSCP$K_OPCODE] = MSCP$K_OP_ONLIN;
	l_e = s_t$mscp(l_port, l_cmd, 36, l_conid);
	$CHECK(l_e && (nisca$get16(l_e + MSCP$K_STATUS) == MSCP$K_ST_SUCC) && s_t$online(l_port, l_host) && s_t$online(l_port, l_host + 5),
		"ONLINE: SUCC, the unit ONLINE to both hosts");

	/*
	**  The boot driver can be quiet at SYSBOOT>: the channel is kept 30 s
	*/
	s_t$run(l_port, MH$K_LISTEN_TMO + MH$K_TICKS_SEC);
	$CHECK((l_node->state == MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_OPEN), "9 s without a word: the channel of a boot driver stays");

	/*
	**  The runtime PEDRIVER of SAT1: CCSTART restarts the channel, which
	**  is no more of a boot driver; it starts the path itself
	*/
	l_chseq = l_node->lcl_chseq;
	s_ntx = 0;
	s_t$cc(l_port, NISCA$K_CC_CCSTART, NULL, 2, 1);
	$CHECK((l_node->state == MH$K_CH_CCSTART_REC) && !l_node->boot && (l_vc->pbstate == MH$K_PB_CLOSED) && (l_node->lcl_chseq != l_chseq),
		"CCSTART of the runtime PEDRIVER: a restart, not BOOT, the path closed");
	$CHECK(!s_t$online(l_port, l_host) && s_t$online(l_port, l_host + 5), "... the host of the boot driver is free, the other stays");
	$CHECK(s_t$iscc(l_port, s_ntx - 1, NISCA$K_CC_VERF), "... our VERF");

	s_ntx = 0;
	s_t$cc(l_port, NISCA$K_CC_VACK, NULL, 2, 1);
	$CHECK((l_node->state == MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_CLOSED) && !s_ntx, "VACK: OPEN, no START of ours");

	s_t$sb(l_st, "V8.3");
	s_t$tr(l_port, NISCA$K_PPC_SNT_DG, NISCA$K_PPD_START, l_st, sizeof(l_st));
	$CHECK((s_ntx == 1) && (s_t$ppd(0) == NISCA$K_PPD_STACK) && (l_vc->pbstate == MH$K_PB_ST_REC), "its START: our STACK");
	$CHECK(!memcmp(s_tx[0] + T$K_PAY + T$K_PDATA, l_start, NISCA$K_ST_CURTIME),
		"... the STACK says of the server what the START did, octet for octet");

	s_t$run(l_port, MH$K_LISTEN_TMO + MH$K_TICKS_SEC);
	$CHECK(l_node->state == MH$K_CH_CLOSED, "9 s without a word: the ordinary channel is closed");

	/*
	**  SAT1 boots again, a minute later: a SOLICIT_SRV on an open channel
	**  restarts it; the boot is told of again
	*/
	s_t$run(l_port, MH$K_SOL_QUIET);
	s_t$cc(l_port, NISCA$K_CC_HELLO, NULL, 0, 1);
	$CHECK(l_node->state == MH$K_CH_HELLO_DLY, "HELLO: HELLO_DLY");
	s_ntx = 0;
	l_logs = l_node->sollogs;
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((l_node->state == MH$K_CH_SOLICIT_DLY) && l_node->boot && !s_ntx, "SOLICIT_SRV in HELLO_DLY: SOLICIT_DLY, BOOT, no CCSTART");
	$CHECK(l_node->sollogs == l_logs + 1, "... a new boot, 60 s after the last SOLICIT_SRV: SOLICIT again");

	s_t$run(l_port, MH$K_SOL_DLY_MAX + 1);
	s_t$cc(l_port, NISCA$K_CC_VACK, NULL, 1, 1);
	$CHECK((l_node->state == MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_ST_SENT), "VERF, VACK: OPEN, our START");

	l_chseq = l_node->lcl_chseq;
	s_ntx = 0;
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((s_ntx == 1) && s_t$iscc(l_port, 0, NISCA$K_CC_VERF) && (l_node->state == MH$K_CH_CCSTART_REC) && l_node->boot
		&& (l_vc->pbstate == MH$K_PB_CLOSED) && (l_node->lcl_chseq != l_chseq), "SOLICIT_SRV on the open channel: a restart, our VERF at once");

	s_t$run(l_port, MH$K_HS_TMO);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && !l_node->boot, "no VACK in 5 s: closed, not BOOT");

	/*
	**  A CCSTART of ours in flight: the boot driver ignores it, its
	**  SOLICIT_SRV takes the channel
	*/
	s_t$cc(l_port, NISCA$K_CC_HELLO, NULL, 0, 1);
	s_t$run(l_port, MH$K_DLY_MAX + 1);
	$CHECK(l_node->state == MH$K_CH_CCSTART_SENT, "HELLO, 2 s: CCSTART_SENT");
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	$CHECK((l_node->state == MH$K_CH_SOLICIT_DLY) && l_node->boot, "SOLICIT_SRV in CCSTART_SENT: SOLICIT_DLY");

	/*
	**  /PASSIVE: never an answer
	*/
	free(l_port->nodes[0].vc);
	memset(l_port->nodes, 0, sizeof(l_port->nodes));
	l_port->nnode	= 0;
	l_port->passive	= 1;
	s_ntx		= 0;
	s_t$cc(l_port, NISCA$K_CC_SOLICIT_SRV, "_$42$DKA100:", 0, 1);
	s_t$run(l_port, 200);
	$CHECK((l_port->nodes[0].state == MH$K_CH_CLOSED) && !s_ntx, "/PASSIVE: no answer to a SOLICIT_SRV");

	unlink(l_img);
	unlink(l_img2);

	printf("T_BOOT: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
