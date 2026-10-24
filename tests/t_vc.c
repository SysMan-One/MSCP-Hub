#define	__MODULE__	"T_VC"
#define	__IDENT__	"X01-20"
#define	__REV__		"1.20.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_vc.c
**
**  ABSTRACT:	Unit tests of the virtual circuit and of the path: the server
**		stands for ALPHA2 (65533) and takes in what
**		ALPHA1 sent it - CCSTART, VACK, START, ACK, CON_REQ - then
**		the transport is played under loss: retransmission, duplicate,
**		out of order, an old channel, 30 timeouts, HOSTSHUT, restart.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		24-OCT-2026	RRL	X01-20 : a circuit put back is put back with the data of its packets.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>

#include	"mh_def.h"

#include	"t_frames.h"

#define	T$K_MAXTX	16

static	uint8_t		s_tx [T$K_MAXTX] [NISCA$K_MAXFRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;

/*
**  The reference CON_REQ sent to VMS$LOOPBACK, as the watchdog does:
**  no listener, the answer is CON_RSP / STNOMAT (made in MAIN)
*/
static	uint8_t		s_lbreq [124];

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

/*
**  Offsets in a frame we send: TR V1.3, then the PPC part
*/
#define	T$K_TR		(NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX)
#define	T$K_PAY		(T$K_TR + NISCA$K_TR_LEN_V15)	/* Our frames: the header of V1.5	*/

/*
**  ... and in a frame of ALPHA1: TR V1.5
*/
#define	T$K_PAY15	(T$K_TR + NISCA$K_TR_LEN_V15)



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
	fprintf(stderr, "T_VC:%d: FAILED: %s\n", a_line, a_what);

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
**	A port made up as ALPHA2, announcing, without a socket, with the
**	channel to ALPHA1 open by the reference CCSTART and VACK.
**
**  FORMAL PARAMETERS:
**
**	a_port		Output: the port context
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_TX is emptied.
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

	s_ntx = 0;

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
static	int	s_t$ticks	(
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
**	A sequenced frame of ALPHA1 (TR V1.5) made out of a reference one
**	with other numbers.
**
**  FORMAL PARAMETERS:
**
**	a_src		The reference frame
**	a_len		Its length
**	a_flags		TR flags
**	a_seq		SEQ, both the 16- and the 32-bit field
**	a_ack		ACK, ditto
**	a_chseq		RMT_CHAN_SEQ_NUM
**	a_dst		Output: the frame, a_len octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$frame	(
	const	uint8_t *	a_src,
		unsigned	a_len,
		unsigned	a_flags,
		uint16_t	a_seq,
		uint16_t	a_ack,
		uint16_t	a_chseq,
		uint8_t *	a_dst
			)
{
uint8_t	*l_tr = a_dst + T$K_TR;

	memcpy(a_dst, a_src, a_len);

	l_tr[NISCA$K_TR_FLAG] = (uint8_t) a_flags;
	nisca$put16(l_tr + NISCA$K_TR_ACK, a_ack);
	nisca$put16(l_tr + NISCA$K_TR_SEQ, a_seq);
	nisca$put16(l_tr + NISCA$K_TR_RMT_CHSEQ, a_chseq);
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, a_ack);
	nisca$put32(l_tr + NISCA$K_TR_MSG_SEQ, a_seq);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Check a sequenced frame we sent: TR V1.3 with the numbers expected,
**	then CON_RSP / STNOMAT against the CON_RSP of ALPHA2 (all but STATUS).
**
**  FORMAL PARAMETERS:
**
**	a_i		Index in S_TX
**	a_flags		TR flags expected
**	a_seq		SEQ expected
**	a_ack		ACK expected
**	a_what		Name of the check
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - some check failed.
**--
*/
static	int	s_t$conrsp	(
		unsigned	a_i,
		unsigned	a_flags,
		uint16_t	a_seq,
		uint16_t	a_ack,
	const	char *		a_what
			)
{
const uint8_t	*l_f = s_tx[a_i], *l_tr = l_f + T$K_TR;
char		l_what [160];
int		l_ok;
unsigned	l_status = 1 + NISCA$K_PPD_DATA + NISCA$K_SCS_BODY + NISCA$K_SCS_STATUS;	/* In the PPC part	*/

	l_ok = (a_i < s_ntx) && !memcmp(l_f, s_conrsp_alpha2, NISCA$K_SZ_ETH)
		&& !memcmp(l_f + NISCA$K_SZ_LANHDR, s_conrsp_alpha2 + NISCA$K_SZ_LANHDR, NISCA$K_SZ_DX)
		&& (s_txlen[a_i] == T$K_PAY + 1 + 2 + NISCA$K_SCS_CON_RSP_LEN)
		&& (nisca$get16(l_f + NISCA$K_SZ_ETH) == NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15 + 1 + 2 + NISCA$K_SCS_CON_RSP_LEN)
		&& (l_tr[NISCA$K_TR_FLAG] == a_flags) && (l_tr[NISCA$K_TR_PAD] == 0x13) && (l_tr[NISCA$K_TR_PAD_DLEN] == 0x12)
		&& (nisca$get16(l_tr + NISCA$K_TR_SEQ) == a_seq) && (nisca$get16(l_tr + NISCA$K_TR_ACK) == a_ack)
		&& (nisca$get32(l_tr + NISCA$K_TR_MSG_SEQ) == a_seq) && (nisca$get32(l_tr + NISCA$K_TR_ACK_SEQ) == a_ack)
		&& !nisca$get32(l_tr + NISCA$K_TR_LAST_ACC) && !nisca$get16(l_tr + NISCA$K_TR_FLAG_EXT)
		&& (nisca$get16(l_tr + NISCA$K_TR_RMT_CHSEQ) == 1)
		&& !memcmp(l_f + T$K_PAY, s_conrsp_alpha2 + T$K_PAY15, l_status)
		&& (nisca$get16(l_f + T$K_PAY + l_status) == NISCA$K_SCS_STNOMAT)
		&& !memcmp(l_f + T$K_PAY + l_status + 2, "VMS$", 4);

	snprintf(l_what, sizeof(l_what), "%s: CON_RSP/STNOMAT, TR flags %02X seq %u ack %u", a_what, a_flags, a_seq, a_ack);

	return	$CHECK(l_ok, l_what);
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
MH$PORT		*l_port = &s_port;
MH$NODE		*l_node;
MH$VC		*l_vc;
uint8_t		l_f [256], *l_st, *l_tr;
unsigned	l_n;

	mh$inimsg();

	memcpy(s_lbreq, s_conreq_alpha1, sizeof(s_lbreq));
	memcpy(s_lbreq + T$K_PAY15 + 1 + NISCA$K_PPD_DATA + NISCA$K_SCS_BODY + NISCA$K_SCS_DST_PROC, "VMS$LOOPBACK    ", 16);

	/*
	**  The channel is open; a sequenced message and an ACK
	**  before START are of no circuit and are dropped
	*/
	s_t$port(l_port);
	l_node = &l_port->nodes[0];
	$CHECK((l_port->nnode == 1) && (l_node->state == MH$K_CH_OPEN) && (l_node->rmt_chseq == 1), "the channel to ALPHA1 is open");

	mh$cc_input(l_port, s_lbreq, sizeof(s_lbreq));
	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	$CHECK(!s_ntx && (!l_node->vc || (l_node->vc->pbstate == MH$K_PB_CLOSED)), "CON_REQ and ACK before START: nothing");

	/*
	**  START: the SB of ALPHA1 is taken, STACK goes out
	*/
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	l_vc = l_node->vc;
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_ST_REC) && (s_ntx == 1), "START: our STACK, ST_REC");
	$CHECK(l_vc && (l_vc->sysid == 65534) && !strcmp(l_vc->swtype, "VMS") && !strcmp(l_vc->swvers, "V8.3") && !strcmp(l_vc->hwtype, "ALPH")
		&& (l_vc->maxdg == 576) && (l_vc->maxmsg == 216), "the SB of ALPHA1: 65534, VMS V8.3, ALPH, 576/216");

	l_tr = s_tx[0] + T$K_TR;
	l_st = s_tx[0] + T$K_PAY + 1 + NISCA$K_PPD_DATA;
	$CHECK(!memcmp(s_tx[0], s_conrsp_alpha2, NISCA$K_SZ_ETH) && !memcmp(s_tx[0] + NISCA$K_SZ_LANHDR, s_conrsp_alpha2 + NISCA$K_SZ_LANHDR, NISCA$K_SZ_DX),
		"STACK: Ethernet and DX headers to ALPHA1");
	$CHECK((l_tr[NISCA$K_TR_FLAG] == NISCA$M_TR_DATA) && (l_tr[NISCA$K_TR_PAD] == 0x13) && (l_tr[NISCA$K_TR_PAD_DLEN] == 0x12)
		&& (nisca$get16(l_tr + NISCA$K_TR_RMT_CHSEQ) == 1), "STACK: a TR V1.5 datagram on channel 1");
	$CHECK((s_tx[0][T$K_PAY] == NISCA$K_PPC_SNT_DG) && !memcmp(s_tx[0] + T$K_PAY + 1, s_start_alpha1 + T$K_PAY15 + 1, 2)
		&& (nisca$get16(s_tx[0] + T$K_PAY + 1 + NISCA$K_PPD_MTYPE) == NISCA$K_PPD_STACK), "STACK: SNT_DG, LENGTH 62 as START, MTYPE 1");
	$CHECK((nisca$get16(l_st + NISCA$K_ST_SYSTEMID) == 65533) && (l_st[NISCA$K_ST_PROTOCOL] == 1)
		&& !memcmp(l_st + NISCA$K_ST_MAXDG, s_start_alpha1 + T$K_PAY15 + 1 + NISCA$K_PPD_DATA + NISCA$K_ST_MAXDG, 4)
		&& !memcmp(l_st + NISCA$K_ST_SWTYPE, "MSCHX010", 8) && !memcmp(l_st + NISCA$K_ST_HWTYPE, "LNX ", 4)
		&& !memcmp(l_st + NISCA$K_ST_NODENAME, "ALPHA2  ", 8) && (nisca$get64(l_st + NISCA$K_ST_SWINCARN) == l_port->incarn)
		&& nisca$get64(l_st + NISCA$K_ST_CURTIME), "STACK data: 65533, 576/216, MSCH X010, LNX, ALPHA2, our incarnation, the time");

	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	$CHECK((s_ntx == 2) && (l_vc->pbstate == MH$K_PB_ST_REC) && (l_vc->nsu == 1), "START again in ST_REC: STACK again, numbers kept");

	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	$CHECK((l_vc->pbstate == MH$K_PB_OPEN) && (s_ntx == 2), "ACK: the path is open");

	/*
	**  The CON_REQ of ALPHA1 (SEQ 1, RSVP): CON_RSP / STNOMAT with the
	**  CONIDs swapped, SEQ 1, ACK 1, RSVP
	*/
	s_ntx = 0;
	mh$cc_input(l_port, s_lbreq, sizeof(s_lbreq));
	$CHECK(s_ntx == 1, "CON_REQ: one frame out");
	s_t$conrsp(0, 0x1B, 1, 1, "first answer");
	$CHECK((l_vc->hsr == 1) && (l_vc->nxmt == 1) && !l_vc->sack, "HSR 1, one packet unacknowledged, the ACK went with it");

	/*
	**  No ACK: the head goes again after 500 ms, REXMT and RSVP
	*/
	s_t$ticks(l_port, MH$K_TR_RTO - 1);
	$CHECK(s_ntx == 1, "nothing again before 500 ms");
	s_t$ticks(l_port, 1);
	s_t$conrsp(1, 0x3B, 1, 1, "retransmission");
	$CHECK(l_port->tr_rexmt == 1, "one retransmission counted");

	/*
	**  The CON_REQ again (its sender retransmits): a duplicate, a bare ACK
	**  at once, no second answer
	*/
	mh$cc_input(l_port, s_lbreq, sizeof(s_lbreq));
	$CHECK((s_ntx == 3) && (nisca$get16(s_tx[2] + NISCA$K_SZ_ETH) == NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15) && (s_tx[2][T$K_TR + NISCA$K_TR_FLAG] == NISCA$M_TR_ACK)
		&& (nisca$get16(s_tx[2] + T$K_TR + NISCA$K_TR_ACK) == 1) && (l_port->tr_dup == 1), "duplicate: a bare ACK 1, no answer");

	/*
	**  ALPHA1 acknowledges our SEQ 1 - a bare ACK of V1.5
	*/
	s_t$frame(s_lbreq, T$K_PAY15, NISCA$M_TR_ACK | NISCA$M_TR_FLAG_EXT, 0x4148, 1, 1, l_f);
	nisca$put16(l_f + NISCA$K_SZ_ETH, NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15);
	mh$cc_input(l_port, l_f, T$K_PAY15);
	$CHECK(!l_vc->nxmt && !l_vc->due && (l_vc->lar == 1) && (l_vc->window == 2), "ACK 1: nothing outstanding, window 2");

	/*
	**  An ACK owed (a packet without RSVP, nothing to answer it with) goes
	**  when the frames that came are taken - not on the next tick - and once
	*/
	s_ntx = 0;
	l_vc->sack = 1;
	mh$tr_flush(l_port);
	$CHECK((s_ntx == 1) && (s_tx[0][T$K_TR + NISCA$K_TR_FLAG] == NISCA$M_TR_ACK) && (nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_ACK) == 1)
		&& !l_vc->sack, "the ACK owed: a bare ACK 1 at once");
	mh$tr_flush(l_port);
	$CHECK(s_ntx == 1, "... and not twice");

	/*
	**  /ACK=HOLD: the ACK owed for a packet without RSVP waits - the drain
	**  of the socket sends nothing, the tick does; MH$K_TR_ACKBATCH packets
	**  owed are acknowledged at once, RSVP is at once as ever, and an
	**  answer of ours carries the ACK.  The packets are ahead of HSR, so
	**  they are cached and nothing answers them.  The circuit and the
	**  counters the checks below look at are put back afterwards.
	*/
	{
	static	MH$VC	s_hold;
	static	uint8_t	s_holdpool [MH$K_TR_NPKT * MH$K_SZ_TRPAY];
	uint64_t	l_ooo = l_port->tr_ooo, l_n0, l_n1;
	unsigned	l_seq;

	memcpy(&s_hold, l_vc, sizeof(s_hold));
	memcpy(s_holdpool, l_vc->pool, MH$K_TR_NPKT * l_vc->paysz);
	l_port->ackmode = MH$K_ACK_HOLD;

	s_ntx = 0;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 3, 1, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	mh$tr_flush(l_port);
	$CHECK(!s_ntx && (l_vc->sack == 1) && (l_vc->nowed == 1), "HOLD: a packet without RSVP is owed, the drain sends nothing");

	s_t$ticks(l_port, 1);
	$CHECK((s_ntx == 1) && (s_tx[0][T$K_TR + NISCA$K_TR_FLAG] == NISCA$M_TR_ACK) && (nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_ACK) == 1)
		&& !l_vc->sack && !l_vc->nowed && (l_port->tr_ackbare[MH$K_ACKW_TICK] == 1), "... the tick sends it: a bare ACK 1");

	s_ntx = 0;

	for ( l_seq = 4; l_seq < 4 + MH$K_TR_ACKBATCH; l_seq++ )
		{
		s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, (uint16_t) l_seq, 1, 1, l_f);
		mh$cc_input(l_port, l_f, sizeof(s_lbreq));
		$CHECK(s_ntx == (l_seq == 3 + MH$K_TR_ACKBATCH), "HOLD: a batch of packets owed is acknowledged at its last one");
		}

	$CHECK((l_port->tr_ackbare[MH$K_ACKW_BATCH] == 1) && !l_vc->nowed, "... one bare ACK for the batch");

	s_ntx	= 0;
	l_n0	= l_port->tr_ackbare[MH$K_ACKW_RSVP];
	l_n1	= l_port->tr_rsvp;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x5B, (uint16_t) l_seq, 1, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	$CHECK((s_ntx == 1) && (l_port->tr_ackbare[MH$K_ACKW_RSVP] == l_n0 + 1) && (l_port->tr_rsvp == l_n1 + 1), "HOLD: RSVP is acknowledged at once");

	/*
	**  SEQ 2 without RSVP: delivered, the CON_RSP of it carries ACK 2
	*/
	memcpy(l_vc, &s_hold, sizeof(s_hold));
	memcpy(l_vc->pool, s_holdpool, MH$K_TR_NPKT * l_vc->paysz);
	s_ntx	= 0;
	l_n0	= l_port->tr_ackpig;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 2, 1, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	mh$tr_flush(l_port);
	$CHECK((s_ntx == 1) && (nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_ACK) == 2) && !l_vc->sack && (l_port->tr_ackpig == l_n0 + 1),
		"HOLD: the answer pays the ACK owed, no bare ACK");

	memcpy(l_vc, &s_hold, sizeof(s_hold));
	memcpy(l_vc->pool, s_holdpool, MH$K_TR_NPKT * l_vc->paysz);
	l_port->ackmode	= MH$K_ACK_FLUSH;
	l_port->tr_ooo	= l_ooo;
	}

	/*
	**  SEQ 3 before SEQ 2: cached, then both answered in order
	*/
	s_ntx = 0;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 3, 1, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	$CHECK(!s_ntx && (l_vc->hsr == 1) && (l_port->tr_ooo == 1), "SEQ 3 before 2: cached, nothing delivered");

	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 2, 1, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	$CHECK((s_ntx == 2) && (l_vc->hsr == 3), "SEQ 2: SEQ 2 and 3 delivered, HSR 3");
	s_t$conrsp(0, 0x1B, 2, 2, "answer to SEQ 2, the queue empty behind it");
	s_t$conrsp(1, 0x1B, 3, 3, "answer to SEQ 3, the last of the burst");

	/*
	**  The numbers are longwords: across 65535 both ways, the words of the
	**  header wrap, MSG_SEQ and ACK_SEQ go on.  A header of V1.3 (words
	**  only) is taken as the nearest longword.  The circuit is put back as
	**  it was afterwards: SEQ 2 and 3 stay unacknowledged for the tests of
	**  the retransmission below.
	*/
	{
	static	MH$VC	s_save;
	static	uint8_t	s_savepool [MH$K_TR_NPKT * MH$K_SZ_TRPAY];
	const uint8_t	*l_tr;
	unsigned	l_len13 = sizeof(s_lbreq) - (NISCA$K_TR_LEN_V15 - NISCA$K_TR_LEN_V13);

	memcpy(&s_save, l_vc, sizeof(s_save));
	memcpy(s_savepool, l_vc->pool, MH$K_TR_NPKT * l_vc->paysz);
	l_vc->nxmt	= 0;
	l_vc->due	= 0;
	l_vc->hsr	= 0xFFFE;
	l_vc->lar	= 0xFFFE;
	l_vc->nsu	= 0xFFFF;

	s_ntx = 0;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 0xFFFF, 0xFFFE, 1, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	s_t$conrsp(0, 0x1B, 0xFFFF, 0xFFFF, "SEQ 65535: answered with SEQ 65535");

	s_ntx = 0;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x4B, 0, 0xFFFF, 1, l_f);
	nisca$put32(l_f + T$K_TR + NISCA$K_TR_MSG_SEQ, 0x10000);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	l_tr = s_tx[0] + T$K_TR;
	$CHECK((s_ntx == 1) && (l_vc->hsr == 0x10000) && (l_vc->lar == 0xFFFF) && (l_vc->nxmt == 1)
		&& !nisca$get16(l_tr + NISCA$K_TR_SEQ) && (nisca$get32(l_tr + NISCA$K_TR_MSG_SEQ) == 0x10000)
		&& !nisca$get16(l_tr + NISCA$K_TR_ACK) && (nisca$get32(l_tr + NISCA$K_TR_ACK_SEQ) == 0x10000),
		"SEQ 65536: HSR 65536, our answer SEQ 65536 - words 0, longwords 65536");

	/*
	**  The same request in a header of V1.3: the PPC part moved down, the
	**  words 1 and 0 are 65537 and 65536
	*/
	s_ntx = 0;
	memcpy(l_f, s_lbreq, T$K_TR);
	l_f[T$K_TR + NISCA$K_TR_FLAG]	= 0x1B & ~NISCA$M_TR_REXMT;
	l_f[T$K_TR + NISCA$K_TR_PAD]	= NISCA$K_TR_LEN_V13 - NISCA$K_TR_LEN_V10;
	nisca$put16(l_f + T$K_TR + NISCA$K_TR_ACK, 0);
	nisca$put16(l_f + T$K_TR + NISCA$K_TR_SEQ, 1);
	nisca$put16(l_f + T$K_TR + NISCA$K_TR_RMT_CHSEQ, 1);
	l_f[T$K_TR + NISCA$K_TR_PAD_DLEN] = NISCA$K_TR_LEN_V13 - NISCA$K_TR_LEN_V10;
	memcpy(l_f + T$K_TR + NISCA$K_TR_LEN_V13, s_lbreq + T$K_PAY15, sizeof(s_lbreq) - T$K_PAY15);
	nisca$put16(l_f + NISCA$K_SZ_ETH, (uint16_t) (nisca$get16(s_lbreq + NISCA$K_SZ_ETH) - (NISCA$K_TR_LEN_V15 - NISCA$K_TR_LEN_V13)));
	mh$cc_input(l_port, l_f, l_len13);
	$CHECK((s_ntx == 1) && (l_vc->hsr == 0x10001) && (l_vc->lar == 0x10000) && (l_vc->nxmt == 1)
		&& (nisca$get32(s_tx[0] + T$K_TR + NISCA$K_TR_MSG_SEQ) == 0x10001), "V1.3 header, words 1/0: SEQ 65537, ACK 65536, answered with 65537");

	memcpy(l_vc, &s_save, sizeof(s_save));
	memcpy(l_vc->pool, s_savepool, MH$K_TR_NPKT * l_vc->paysz);
	}

	/*
	**  A packet of another incarnation of the channel is dropped
	*/
	s_ntx = 0;
	s_t$frame(s_lbreq, sizeof(s_lbreq), 0x5B, 4, 3, 7, l_f);
	mh$cc_input(l_port, l_f, sizeof(s_lbreq));
	$CHECK(!s_ntx && (l_vc->hsr == 3) && (l_port->tr_old == 1), "RMT_CHAN_SEQ_NUM 7: dropped");

	/*
	**  Nothing acknowledges SEQ 2 and 3; HELLOs keep the channel: after 30
	**  timeouts the circuit fails and the channel is restarted by CCSTART
	*/
	for ( l_n = 0; (l_n < 200 * MH$K_TICKS_SEC) && (l_vc->pbstate == MH$K_PB_OPEN); l_n++ )
		{
		s_ntx = 0;

		if ( !(l_n % MH$K_TICKS_SEC) )
			mh$cc_input(l_port, s_hello_alpha1, sizeof(s_hello_alpha1));

		mh$cc_tick(l_port);
		}

	$CHECK((l_vc->pbstate == MH$K_PB_CLOSED) && (l_port->tr_rexmt == 1 + MH$K_TR_MAXREXMT), "30 retransmissions, then the circuit is closed");
	$CHECK((l_node->state == MH$K_CH_CCSTART_SENT) && (l_node->lcl_chseq == 2) && (s_tx[s_ntx - 1][T$K_TR] == 0xB2),
		"... and the channel restarted: CCSTART of chseq 2");
	$CHECK((l_n > 60 * MH$K_TICKS_SEC) && (l_n < 100 * MH$K_TICKS_SEC), "... in 1 - 1.5 minutes (3 s each at most)");

	/*
	**  START on the open path: the node has restarted - closed and opened
	**  again; HOSTSHUT closes the path
	*/
	s_t$port(l_port);
	l_node = &l_port->nodes[0];
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	l_vc = l_node->vc;
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_OPEN), "a new port: the path is open again");

	mh$cc_input(l_port, s_lbreq, sizeof(s_lbreq));
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	$CHECK((l_vc->pbstate == MH$K_PB_ST_REC) && (l_vc->nsu == 1) && !l_vc->nxmt && (l_vc->opens == 1), "START on the open path: reset, ST_REC");

	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	memcpy(l_f, s_ack_alpha1, sizeof(s_ack_alpha1));
	nisca$put16(l_f + T$K_PAY15 + 1 + NISCA$K_PPD_MTYPE, NISCA$K_PPD_HOSTSHUT);
	mh$cc_input(l_port, l_f, sizeof(s_ack_alpha1));
	$CHECK((l_vc->pbstate == MH$K_PB_CLOSED) && (l_vc->opens == 2), "HOSTSHUT of the node: the path is closed");

	/*
	**  The listen timeout of the channel takes the circuit with it
	*/
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	s_t$ticks(l_port, MH$K_LISTEN_TMO);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && (l_vc->pbstate == MH$K_PB_CLOSED), "8 s of silence: channel and circuit closed");

	/*
	**  The stop of the server: HOSTSHUT on the open path, then BYE
	*/
	s_t$port(l_port);
	l_node = &l_port->nodes[0];
	mh$cc_input(l_port, s_start_alpha1, sizeof(s_start_alpha1));
	mh$cc_input(l_port, s_ack_alpha1, sizeof(s_ack_alpha1));
	s_ntx = 0;
	mh$cc_shutdown(l_port);
	$CHECK((s_ntx == 2) && (s_tx[0][T$K_PAY] == NISCA$K_PPC_SNT_DG) && (nisca$get16(s_tx[0] + T$K_PAY + 1) == 2)
		&& (nisca$get16(s_tx[0] + T$K_PAY + 1 + NISCA$K_PPD_MTYPE) == NISCA$K_PPD_HOSTSHUT)
		&& (s_tx[1][T$K_TR] == 0xB1), "stop: HOSTSHUT datagram, LENGTH 2, then BYE");
	$CHECK(l_node->vc->pbstate == MH$K_PB_CLOSED, "... and the path is closed");

	printf("T_VC: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
