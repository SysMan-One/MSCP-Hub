#define	__MODULE__	"T_MCHAN"
#define	__IDENT__	"X01-23"
#define	__REV__		"1.23.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_mchan.c
**
**  ABSTRACT:	Unit tests of a node of two adapters on the LAN of the server:
**		the server stands for ALPHA2 (65533) and ALPHA1 (65534) has
**		its EWA (08-00-2B-E5-40-01, the reference frames, a buffer of
**		1426) and an EIA (08-00-2B-E5-41-01, the same frames from
**		another address, a buffer of 8100).  The second channel serves
**		the circuit of the first: its packets are taken, ours go by the
**		channel of the larger buffer, with its channel sequence number;
**		the circuit lives while one channel of the node is open.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  26-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		26-OCT-2026	RRL	X01-23 : Initial revision.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>

#include	"mh_def.h"
#include	"t_frames.h"

#define	T$K_MAXTX	16

static	uint8_t		s_tx [T$K_MAXTX] [NISCA$K_MAXFRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;
static	int		s_fails, s_checks;
static	uint8_t		s_lbreq [124];

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)

#define	T$K_CC		(NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX)
#define	T$K_TR		T$K_CC
#define	T$K_PAY15	(T$K_TR + NISCA$K_TR_LEN_V15)
#define	T$K_EIA_CHSEQ	5			/* LCL_CHAN_SEQ_NUM of the EIA's channel, not the EWA's 1	*/

static	const uint8_t	s_ewa [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x01 };
static	const uint8_t	s_eia [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x41, 0x01 };



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
	fprintf(stderr, "T_MCHAN:%d: FAILED: %s\n", a_line, a_what);

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
**	A frame of ALPHA1 sent by one of its adapters: a reference frame with
**	another source address; a CC message may be given another buffer.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_ref		The reference frame
**	a_len		Its length
**	a_mac		The adapter
**	a_bufsz		BUS_BUFFER_SIZE of a CC message, 0 - as it is
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
**--
*/
static	int	s_t$via	(
		MH$PORT *	a_port,
	const	uint8_t *	a_ref,
		unsigned	a_len,
	const	uint8_t *	a_mac,
		uint16_t	a_bufsz
			)
{
uint8_t	l_f [NISCA$K_MAXFRAME];

	memcpy(l_f, a_ref, a_len);
	memcpy(l_f + NISCA$K_SZ_MAC, a_mac, NISCA$K_SZ_MAC);

	/*
	**  The EIA: its own buffer and its own channel sequence number
	*/
	if ( a_bufsz )
		{
		nisca$put16(l_f + T$K_CC + NISCA$K_CC_BUS_BUFSZ, a_bufsz);
		nisca$put16(l_f + T$K_CC + NISCA$K_CC_LCL_CHSEQ, T$K_EIA_CHSEQ);
		}

	return	mh$cc_input(a_port, l_f, a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A sequenced CON_REQ, or a bare ACK, of ALPHA1 by one of its adapters,
**	with the numbers given.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The adapter
**	a_seq		SEQ, 0 - a bare ACK
**	a_ack		ACK
**	a_chseq		RMT_CHAN_SEQ_NUM
**
**  RETURN VALUE:
**	The condition value of MH$CC_INPUT.
**--
*/
static	int	s_t$tr	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac,
		uint16_t	a_seq,
		uint16_t	a_ack,
		uint16_t	a_chseq
			)
{
uint8_t		l_f [sizeof(s_lbreq)], *l_tr = l_f + T$K_TR;
unsigned	l_len = sizeof(s_lbreq);

	memcpy(l_f, s_lbreq, sizeof(s_lbreq));
	memcpy(l_f + NISCA$K_SZ_MAC, a_mac, NISCA$K_SZ_MAC);

	l_tr[NISCA$K_TR_FLAG] = (uint8_t) (a_seq ? (NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK | NISCA$M_TR_RSVP)
		: (NISCA$M_TR_ACK | NISCA$M_TR_FLAG_EXT));
	nisca$put16(l_tr + NISCA$K_TR_ACK, a_ack);
	nisca$put16(l_tr + NISCA$K_TR_SEQ, a_seq);
	nisca$put16(l_tr + NISCA$K_TR_RMT_CHSEQ, a_chseq);
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, a_ack);
	nisca$put32(l_tr + NISCA$K_TR_MSG_SEQ, a_seq);

	if ( !a_seq )
		{
		l_len = T$K_PAY15;
		nisca$put16(l_f + NISCA$K_SZ_ETH, NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15);
		}

	return	mh$cc_input(a_port, l_f, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Run the 20 ms clock; a HELLO of ALPHA1 every second by the adapters
**	given, so that their channels stay open.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_ticks		How many ticks
**	a_ewa		1 - the EWA says HELLO
**	a_eia		1 - the EIA says HELLO
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$run	(
		MH$PORT *	a_port,
		unsigned	a_ticks,
		int		a_ewa,
		int		a_eia
			)
{
	for ( unsigned i = 0; i < a_ticks; i++ )
		{
		if ( !(i % MH$K_TICKS_SEC) )
			{
			if ( a_ewa )
				s_t$via(a_port, s_hello_alpha1, sizeof(s_hello_alpha1), s_ewa, 0);

			if ( a_eia )
				s_t$via(a_port, s_hello_alpha1, sizeof(s_hello_alpha1), s_eia, 8100);
			}

		mh$cc_tick(a_port);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A port made up as ALPHA2 on a LAN of jumbo frames, with ALPHA1 on it
**	by both adapters: the channels of the EWA and of the EIA open, the
**	circuit open over them.
**
**  FORMAL PARAMETERS:
**
**	a_port		Output: the port context; the circuit of an earlier
**			one is freed
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$setup	(
		MH$PORT *	a_port
			)
{
static	const uint8_t	s_mac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x02 };

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		free(a_port->nodes[i].vc);

	memset(a_port, 0, sizeof(*a_port));

	a_port->sd		= -1;
	a_port->xmit		= s_t$xmit;
	a_port->group		= 4242;
	a_port->sysid		= 65533;
	a_port->mtu		= 9000;
	a_port->bufsz		= 9000 - NISCA$K_SZ_PAD - MH$K_LAN_OVHD;
	a_port->announcing	= 1;
	a_port->next_hello	= (uint64_t) -1;
	a_port->incarn		= 0x00BC3C8BC2EC7380ULL;
	memcpy(a_port->mac, s_mac, NISCA$K_SZ_MAC);
	memcpy(a_port->ifname, "EIA", 4);
	nisca$group_mac(a_port->group, a_port->gmac);
	mh$hex2bin("5A3C9E1700000000", 16, a_port->auth, MH$K_SZ_AUTH);
	a_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memcpy(a_port->ccname + 1, "ALPHA2", NISCA$K_SZ_NODENAME);
	a_port->devname[0] = 3;
	memcpy(a_port->devname + 1, "EIA", 3);

	s_t$via(a_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1), s_ewa, 0);
	s_t$via(a_port, s_vack_alpha1, sizeof(s_vack_alpha1), s_ewa, 0);
	s_t$via(a_port, s_start_alpha1, sizeof(s_start_alpha1), s_ewa, 0);
	s_t$via(a_port, s_ack_alpha1, sizeof(s_ack_alpha1), s_ewa, 0);
	s_t$via(a_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1), s_eia, 8100);
	s_t$via(a_port, s_vack_alpha1, sizeof(s_vack_alpha1), s_eia, 8100);

	s_ntx = 0;

	return	STS$K_SUCCESS;
}



int	main	(void)
{
static	MH$PORT	s_port;
static	const uint8_t	s_mac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x02 };
MH$PORT		*l_port = &s_port;
MH$NODE		*l_a1, *l_a1b;
MH$VC		*l_vc;
uint16_t	l_seq, l_our = 0;

	mh$inimsg();

	memcpy(s_lbreq, s_conreq_alpha1, sizeof(s_lbreq));
	memcpy(s_lbreq + T$K_PAY15 + 1 + NISCA$K_PPD_DATA + NISCA$K_SCS_BODY + NISCA$K_SCS_DST_PROC, "VMS$LOOPBACK    ", 16);

	/*
	**  ALPHA2 on a LAN of jumbo frames
	*/
	l_port->sd		= -1;
	l_port->xmit		= s_t$xmit;
	l_port->group		= 4242;
	l_port->sysid		= 65533;
	l_port->mtu		= 9000;
	l_port->bufsz		= 9000 - NISCA$K_SZ_PAD - MH$K_LAN_OVHD;
	l_port->announcing	= 1;
	l_port->next_hello	= (uint64_t) -1;
	l_port->incarn		= 0x00BC3C8BC2EC7380ULL;
	memcpy(l_port->mac, s_mac, NISCA$K_SZ_MAC);
	memcpy(l_port->ifname, "EIA", 4);
	nisca$group_mac(l_port->group, l_port->gmac);
	mh$hex2bin("5A3C9E1700000000", 16, l_port->auth, MH$K_SZ_AUTH);
	l_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memcpy(l_port->ccname + 1, "ALPHA2", NISCA$K_SZ_NODENAME);
	l_port->devname[0] = 3;
	memcpy(l_port->devname + 1, "EIA", 3);

	/*
	**  The channel of the EWA and the circuit over it
	*/
	s_t$via(l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1), s_ewa, 0);
	s_t$via(l_port, s_vack_alpha1, sizeof(s_vack_alpha1), s_ewa, 0);
	s_t$via(l_port, s_start_alpha1, sizeof(s_start_alpha1), s_ewa, 0);
	s_t$via(l_port, s_ack_alpha1, sizeof(s_ack_alpha1), s_ewa, 0);
	l_a1 = &l_port->nodes[0];
	l_vc = l_a1->vc;
	$CHECK((l_port->nnode == 1) && (l_a1->state == MH$K_CH_OPEN) && !l_a1->owner && l_vc && (l_vc->pbstate == MH$K_PB_OPEN),
		"the EWA of ALPHA1: the channel and the circuit are open");
	$CHECK(mh$cc_bufsz(l_port, l_a1) == l_a1->bufsz, "... a block goes in the buffer of the EWA");

	/*
	**  The channel of the EIA: another entry, of the same node, a buffer of 8100
	*/
	s_ntx = 0;
	s_t$via(l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1), s_eia, 8100);
	l_a1b = &l_port->nodes[1];
	$CHECK((l_port->nnode == 2) && (l_a1b->owner == l_a1) && !memcmp(l_a1b->mac, s_eia, 6), "the EIA of ALPHA1: an adapter of the node");
	$CHECK((s_ntx == 1) && !memcmp(s_tx[0], s_eia, 6) && (s_tx[0][T$K_CC] & NISCA$M_CC_TYPE) == NISCA$K_CC_VERF,
		"... its CCSTART: our VERF to the EIA");
	s_t$via(l_port, s_vack_alpha1, sizeof(s_vack_alpha1), s_eia, 8100);
	$CHECK((l_a1b->state == MH$K_CH_OPEN) && (l_a1b->bufsz == 8100) && !l_a1b->vc && (l_a1->vc == l_vc) && (l_vc->pbstate == MH$K_PB_OPEN),
		"... its VACK: the channel is open, no circuit of its own, that of the node goes on");
	$CHECK(mh$cc_bufsz(l_port, l_a1) == 8100, "... a block goes in 8100 now");
	$CHECK((mh$cc_chan(l_port, l_a1, 5000) == l_a1b) && (mh$cc_chan(l_port, l_a1, 100) == l_a1b), "... and the circuit goes by the EIA");

	/*
	**  A CON_REQ by the EIA: taken by the circuit of the node; the answer
	**  goes by the EIA, with the channel sequence number of the EIA
	*/
	s_ntx = 0;
	l_seq = (uint16_t) (l_vc->hsr + 1);
	s_t$tr(l_port, s_eia, l_seq, (uint16_t) l_vc->lar, l_a1b->lcl_chseq);
	$CHECK((l_vc->hsr == l_seq) && (s_ntx == 1), "CON_REQ by the EIA: taken by the circuit, answered");
	$CHECK((s_ntx == 1) && !memcmp(s_tx[0], s_eia, 6) && (l_a1b->rmt_chseq == T$K_EIA_CHSEQ)
		&& (nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_RMT_CHSEQ) == T$K_EIA_CHSEQ),
		"... the answer goes to the EIA, with its channel sequence number, 5");
	l_our = nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_SEQ);

	/*
	**  A packet of another incarnation of the EIA's channel is not taken
	*/
	s_ntx = 0;
	s_t$tr(l_port, s_eia, (uint16_t) (l_seq + 1), (uint16_t) l_vc->lar, (uint16_t) (l_a1b->lcl_chseq + 7));
	$CHECK(!s_ntx && (l_vc->hsr == l_seq) && (l_port->tr_old == 1), "a packet of an old channel of the EIA: dropped");

	/*
	**  ALPHA1 acknowledges by the EWA: a circuit takes its ACKs by any channel
	*/
	s_t$tr(l_port, s_ewa, 0, l_our, l_a1->lcl_chseq);
	$CHECK(!l_vc->nxmt, "the ACK by the EWA: nothing outstanding");

	/*
	**  The EWA falls silent: its channel closes, the circuit stays - the
	**  EIA carries it
	*/
	s_t$run(l_port, MH$K_LISTEN_TMO + MH$K_TICKS_SEC, 0, 1);
	$CHECK((l_a1->state != MH$K_CH_OPEN) && (l_a1b->state == MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_OPEN),
		"the EWA silent: its channel closed, the circuit open over the EIA");

	s_ntx = 0;
	l_seq = (uint16_t) (l_vc->hsr + 1);
	s_t$tr(l_port, s_eia, l_seq, (uint16_t) l_vc->lar, l_a1b->lcl_chseq);
	$CHECK((l_vc->hsr == l_seq) && (s_ntx >= 1) && !memcmp(s_tx[0], s_eia, 6), "... a CON_REQ: answered by the EIA");
	l_our = nisca$get16(s_tx[0] + T$K_TR + NISCA$K_TR_SEQ);
	s_t$tr(l_port, s_eia, 0, l_our, l_a1b->lcl_chseq);
	$CHECK(!l_vc->nxmt, "... its ACK by the EIA: nothing outstanding");

	/*
	**  The EIA falls silent too: the circuit goes with the last channel
	*/
	s_t$run(l_port, MH$K_LISTEN_TMO + MH$K_TICKS_SEC, 0, 0);
	$CHECK((l_a1b->state != MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_CLOSED), "the EIA silent too: the circuit is closed");

	/*
	**  A node of one adapter is as before: a packet that does not fit the
	**  channel goes to the owner all the same
	*/
	$CHECK(mh$cc_chan(l_port, l_a1, 5000) == l_a1, "no channel open: the owner");

	/*
	**  A packet of 5000 octets goes by the EIA; the EIA falls silent with
	**  it unacknowledged.  It fits the EWA no more: it is not sent there,
	**  and the circuit fails at once instead of after 30 retransmissions
	*/
	s_t$setup(l_port);
	l_a1 = &l_port->nodes[0];
	l_a1b = &l_port->nodes[1];
	l_vc = l_a1->vc;
	$CHECK(l_vc && (l_vc->pbstate == MH$K_PB_OPEN) && (l_a1b->state == MH$K_CH_OPEN) && (l_a1b->owner == l_a1), "both channels again, the circuit open");

	{
	uint8_t		l_big [5000] = {0};
	unsigned	l_jumbo = 0, l_ticks = 0;

	nisca$put16(l_big + NISCA$K_PPD_LENGTH, sizeof(l_big) - NISCA$K_PPD_DATA);
	nisca$put16(l_big + NISCA$K_PPD_MTYPE, NISCA$K_PPD_SCS_MSG);
	mh$tr_send_seq(l_port, l_a1, l_big, sizeof(l_big));
	$CHECK((s_ntx == 1) && !memcmp(s_tx[0], s_eia, 6) && (s_txlen[0] > 5000) && (l_vc->nxmt == 1), "5000 octets: by the EIA, unacknowledged");

	/*
	**  The EIA silent; the EWA says HELLO.  Count the ticks from the
	**  close of the EIA to the failure of the circuit
	*/
	for ( unsigned i = 0; (i < 30 * MH$K_TICKS_SEC) && (l_vc->pbstate != MH$K_PB_CLOSED); i++ )
		{
		s_ntx = (s_ntx >= T$K_MAXTX - 2) ? 0 : s_ntx;
		s_t$run(l_port, 1, !(i % MH$K_TICKS_SEC), 0);

		for ( unsigned j = 0; j < s_ntx; j++ )
			l_jumbo += !memcmp(s_tx[j], s_ewa, 6) && (s_txlen[j] > MH$K_LAN_ETHMTU + NISCA$K_SZ_ETH);

		l_ticks += (l_a1b->state != MH$K_CH_OPEN);
		}

	$CHECK((l_a1b->state != MH$K_CH_OPEN) && (l_vc->pbstate == MH$K_PB_CLOSED), "the EIA closed: the circuit failed");
	$CHECK(l_ticks && (l_ticks <= MH$K_TR_RTO_MAX + 2), "... at its next retransmission, not after 30");
	$CHECK(!l_jumbo, "... and no jumbo frame went to the EWA");
	}

	/*
	**  A SOLICIT_SRV by the EWA - ALPHA1 boots, its loader on the EWA - for
	**  a disk of the server: the channel of the EIA, left open, is closed
	*/
	s_t$setup(l_port);
	l_a1 = &l_port->nodes[0];
	l_a1b = &l_port->nodes[1];

	{
	static	MH$CFG	s_cfg;
	char		l_img [] = "t_mchan_XXXXXX", l_spec [64];
	uint8_t		l_f [sizeof(s_hello_alpha1)], *l_cc = l_f + T$K_CC;
	int		l_fd = mkstemp(l_img);

	$CHECK((l_fd >= 0) && !ftruncate(l_fd, 1024 * 1024), "a disk of 1 MB");
	close(l_fd);
	s_cfg.alloclass = 42;
	snprintf(l_spec, sizeof(l_spec), "dka100,%s", l_img);
	$ASCLEN(&s_cfg.unit[0]) = (unsigned char) strlen(l_spec);
	memcpy($ASCPTR(&s_cfg.unit[0]), l_spec, strlen(l_spec));
	$CHECK((1 & mh$mscp_init(l_port, &s_cfg)) && (l_port->nunit == 1), "ALPHA2 serves $42$DKA100");

	memcpy(l_f, s_hello_alpha1, sizeof(l_f));
	l_cc[NISCA$K_CC_MSG] = (uint8_t) ((l_cc[NISCA$K_CC_MSG] & ~NISCA$M_CC_TYPE) | NISCA$K_CC_SOLICIT_SRV | NISCA$M_CC_AUTHORIZE);
	memcpy(l_cc + NISCA$K_CC_AUTHORIZE, l_port->auth, MH$K_SZ_AUTH);
	l_cc[NISCA$K_CC_SERVICE] = 12;
	memcpy(l_cc + NISCA$K_CC_SERVICE + 1, "_$42$DKA100:", 12);

	$CHECK(l_a1b->state == MH$K_CH_OPEN, "the EIA open before the boot");
	mh$cc_input(l_port, l_f, sizeof(l_f));
	$CHECK((l_a1b->state == MH$K_CH_CLOSED) && l_a1->boot, "SOLICIT_SRV by the EWA: the EIA is closed, the EWA boots");
	$CHECK((mh$cc_bufsz(l_port, l_a1) < 8100), "... no block in 8100 for the loader");

	unlink(l_img);
	}

	for ( unsigned i = 0; i < l_port->nnode; i++ )
		free(l_port->nodes[i].vc);

	printf("T_MCHAN: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
