#define	__MODULE__	"MHTR"
#define	__IDENT__	"X01-20"
#define	__REV__		"1.20.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_tr.c
**
**  ABSTRACT:	NISCA transport: the virtual circuit to a node over its open
**		channel - sequence numbers, the cumulative ACK, the transmit
**		window and its retransmission, the cache of the packets that
**		come out of order, and the PPC opcode of every packet.
**
**  DESCRIPTION: The server sends the TR header of V1.5, the way PEDRIVER V8.3
**		lays it out: the numbers in words for V1.3 and in longwords
**		for V1.4 on, then the PPC opcode.
**		It takes in any version, by TR_PAD: the opcode is at 6 + PAD;
**		the 16-bit ACK and SEQ at +2 and +4 are kept by V1.4+ senders
**		too, as the low words of their 32-bit numbers.
**
**		Receive:
**
**		  - a packet of another incarnation of the channel (its
**		    RMT_CHAN_SEQ_NUM is not our LCL_CHAN_SEQ_NUM) is dropped;
**		  - before the circuit is open only a datagram is taken;
**		  - ACK (when the flag says so) frees what it covers;
**		  - SEQ (when the flag says so): the next one is delivered and
**		    the cache drained after it, a duplicate is dropped and
**		    answered by an ACK at once, one up to 32 ahead is cached.
**
**		Transmit: at most min(PIPE_QUOTA of the peer, 16) packets are
**		out unacknowledged; the rest wait in a queue.  Every packet
**		carries ACK = HSR; RSVP asks for an ACK at once and is set on
**		the last packet of a burst, on a full window and, on a window
**		of 8 or more, on its half, as PEDRIVER does - the peer
**		would otherwise hold its ACK up to 100 ms.  An ACK we owe goes
**		on the next packet of ours, or alone: at once for RSVP and
**		duplicates; with /ACK=FLUSH when the LAN socket is drained,
**		with /ACK=HOLD (the default) when MH$K_TR_ACKBATCH packets are
**		owed or on the next 20 ms tick.  The head that times out (500 ms, then
**		doubled up to 3 s) is sent again with REXMT and RSVP, the
**		window drops to 1 and grows by one with every ACK; 30 timeouts
**		in a row fail the circuit, which restarts the channel.  After a
**		timeout, an ACK that leaves packets out sends the new head again
**		at once: with the cache of the peer the next hole is there, and
**		waiting 500 ms for every lost chunk of a 128 KB transfer is what
**		would make a lossy LAN unusable.
**
**		The window is filled from the queue of messages first, then
**		with the chunks of the block transfer at the head of the
**		circuit (MHBLK) - they are made only when there is room, so a
**		READ of 128 KB does not need 92 places in the queue.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-11 : /ACK=HOLD is the default.
**		20-OCT-2026	RRL	X01-13 : 16 packets in flight, RSVP at half the window.
**		24-OCT-2026	RRL	X01-20 : jumbo frames: the packets of a circuit are sized by the MTU.
**
**--
*/

#include	<errno.h>
#include	<string.h>
#include	<stdlib.h>

#include	"mh_def.h"



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Build and send one TR packet on the channel of a node: DX, TR V1.5,
**	then the PPC part.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_flags		NISCA$M_TR_*
**	a_seq		SEQ field
**	a_pay		The PPC part: opcode onwards, NULL - a bare ACK
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$tr_xmit	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		unsigned	a_flags,
		uint32_t	a_seq,
	const	uint8_t *	a_pay,
		unsigned	a_len
			)
{
uint8_t		l_data [MH$K_SZ_MAXDATA], l_dxdst [NISCA$K_SZ_MAC], *l_tr = l_data + NISCA$K_SZ_DX;
MH$VC		*l_vc = a_node->vc;

	nisca$node_mac(a_node->sysid, l_dxdst);
	mh$dx_build(a_port, l_dxdst, l_data);

	/*
	**  An ACK goes on every packet but a datagram, and pays what we owe
	*/
	if ( a_flags & NISCA$M_TR_ACK )
		{
		if ( a_len && l_vc->sack )
			a_port->tr_ackpig++;

		l_vc->sack	= 0;
		l_vc->nowed	= 0;
		}

	/*
	**  The header of V1.5, the way PEDRIVER V8.3 lays it out: the numbers
	**  in words for V1.3 and in longwords for V1.4 on; LAST_ACCEPTED is
	**  not given (no FLAG_EXT)
	*/
	memset(l_tr, 0, NISCA$K_TR_LEN_V15);
	l_tr[NISCA$K_TR_FLAG]	= (uint8_t) a_flags;
	l_tr[NISCA$K_TR_PAD]	= NISCA$K_TR_LEN_V15 - NISCA$K_TR_LEN_V10;
	nisca$put16(l_tr + NISCA$K_TR_ACK, (a_flags & NISCA$M_TR_ACK) ? (uint16_t) l_vc->hsr : 0);
	nisca$put16(l_tr + NISCA$K_TR_SEQ, (uint16_t) a_seq);
	nisca$put16(l_tr + NISCA$K_TR_RMT_CHSEQ, a_node->rmt_chseq);
	l_tr[NISCA$K_TR_PAD_DLEN] = NISCA$K_TR_PAD_DLEN_V15;
	nisca$put32(l_tr + NISCA$K_TR_ACK_SEQ, (a_flags & NISCA$M_TR_ACK) ? l_vc->hsr : 0);
	nisca$put32(l_tr + NISCA$K_TR_MSG_SEQ, a_seq);

	if ( a_len )
		memcpy(l_tr + NISCA$K_TR_LEN_V15, a_pay, a_len);

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "TR to !AZ (!UL): flags !XB seq !UL ack !UL, !UL octets", a_node->name, a_node->sysid,
			a_flags, a_seq, (a_flags & NISCA$M_TR_ACK) ? l_vc->hsr : 0, a_len);

	return	mh$lan_send(a_port, a_node->mac, l_data, NISCA$K_SZ_DX + NISCA$K_TR_LEN_V15 + a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send the ACK owed alone, counted by why.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_why		MH$K_ACKW_*
**
**  RETURN VALUE:
**	The condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$tr_bare	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		int		a_why
			)
{
	a_port->tr_ackbare[a_why]++;

	return	s_mh$tr_xmit(a_port, a_node, NISCA$M_TR_ACK, 0, NULL, 0);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Open the transport of a node: allocate its circuit the first time,
**	start the numbers over (TR_SEQ_OPEN: 1 is the first, HSR = LAR = 0),
**	empty the caches.  The window is held to the PIPE_QUOTA the peer told
**	in its CC messages.
**
**	The buffers of the packets are allocated with the circuit, PAYSZ
**	octets each: what a frame of the MTU of the port carries after the DX
**	and TR headers (an MTU over 9000 is held to it, none - a test - is
**	1500).  162 KB for a circuit on an Ethernet, 1 MB with jumbo frames.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  IMPLICIT INPUTS:
**	MTU of the port.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR - no memory.
**--
*/
int	mh$tr_open	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC		*l_vc;
unsigned	l_mtu, l_n = 0;

	if ( !a_node->vc )
		{
		l_mtu = (a_port->mtu > 0) ? (unsigned) $MIN(a_port->mtu, MH$K_LAN_MAXMTU) : MH$K_LAN_ETHMTU;

		if ( !(l_vc = calloc(1, sizeof(MH$VC))) )
			return	$MHMSG(MH$_SYSERR, "calloc(MH$VC)", errno, strerror(errno));

		l_vc->paysz = l_mtu - NISCA$K_SZ_PAD - NISCA$K_SZ_DX - NISCA$K_TR_LEN_V15;

		if ( !(l_vc->pool = malloc((size_t) MH$K_TR_NPKT * l_vc->paysz)) )
			{
			free(l_vc);
			return	$MHMSG(MH$_SYSERR, "malloc(the packets of a circuit)", errno, strerror(errno));
			}

		for ( unsigned i = 0; i < MH$K_TR_WINDOW; i++ )
			l_vc->xmt[i].data = l_vc->pool + (size_t) (l_n++) * l_vc->paysz;

		for ( unsigned i = 0; i < MH$K_TR_XMTQ; i++ )
			l_vc->q[i].data = l_vc->pool + (size_t) (l_n++) * l_vc->paysz;

		for ( unsigned i = 0; i < MH$K_TR_RCVCACHE; i++ )
			l_vc->rcv[i].data = l_vc->pool + (size_t) (l_n++) * l_vc->paysz;

		a_node->vc = l_vc;
		}

	mh$tr_reset(a_port, a_node);

	a_node->vc->maxwin = $MAX(1, $MIN(a_node->pipe_quota, MH$K_TR_WINDOW));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Forget the state of the transport of a node: the numbers, both caches,
**	the queue.  The path state goes to CLOSED; the SB is kept.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$tr_reset	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC	*l_vc = a_node->vc;

	if ( !l_vc )
		return	STS$K_SUCCESS;

	mh$scs_reset(a_port, a_node);

	l_vc->pbstate	= MH$K_PB_CLOSED;
	l_vc->gen++;
	l_vc->nsu	= 1;
	l_vc->lar	= l_vc->hsr = 0;
	l_vc->window	= 1;
	l_vc->rto	= MH$K_TR_RTO;
	l_vc->retries	= 0;
	l_vc->due	= 0;
	l_vc->sack	= 0;
	l_vc->nowed	= 0;
	l_vc->recover	= 0;
	l_vc->nxmt	= l_vc->nq = l_vc->qhead = 0;
	l_vc->blkhead	= l_vc->blktail = NULL;

	for ( unsigned i = 0; i < MH$K_TR_RCVCACHE; i++ )
		l_vc->rcv[i].valid = 0;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send what the window lets out: the queue of messages, then the chunks
**	of the block transfer at the head of the circuit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$tr_pump	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC		*l_vc = a_node->vc;
MH$TRPKT	*l_pkt;
unsigned	l_flags;
int		l_status = STS$K_SUCCESS;

unsigned	l_len;

	while ( l_vc->nxmt < l_vc->window )
		{
		l_pkt = &l_vc->xmt[l_vc->nxmt];

		if ( l_vc->nq )
			{
			l_pkt->len = l_vc->q[l_vc->qhead].len;
			memcpy(l_pkt->data, l_vc->q[l_vc->qhead].data, l_pkt->len);
			l_vc->qhead = (l_vc->qhead + 1) % MH$K_TR_XMTQ;
			l_vc->nq--;
			}
		else if ( mh$blk_chunk(a_port, a_node, l_pkt->data, &l_len) )
			l_pkt->len = (uint16_t) l_len;
		else	break;

		l_vc->nxmt++;
		l_pkt->seq = l_vc->nsu++;

		l_flags = NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK;

		/*
		**  RSVP on the last of a burst, on a full window and on the half of
		**  a wide one: the ACK of the first half comes back while the
		**  second is out
		*/
		if ( (!l_vc->nq && !l_vc->blkhead) || (l_vc->nxmt == l_vc->window) || ((l_vc->window >= 8) && (l_vc->nxmt == l_vc->window / 2)) )
			l_flags |= NISCA$M_TR_RSVP;

		if ( l_vc->nxmt == 1 )
			l_vc->due = a_port->tick + l_vc->rto;

		a_port->tr_tx++;

		l_status = s_mh$tr_xmit(a_port, a_node, l_flags, l_pkt->seq, l_pkt->data, l_pkt->len);
		}

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Queue a sequenced packet: the PPC part as it is, opcode onwards - a
**	REQ_DAT, for one.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node, its path not CLOSED
**	a_pay		The PPC part
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no circuit, too long or the queue is full.
**--
*/
int	mh$tr_send_ppc	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_pay,
		unsigned	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$TRPKT	*l_pkt;

	if ( !l_vc || (l_vc->pbstate == MH$K_PB_CLOSED) || (a_len > l_vc->paysz) )
		return	STS$K_ERROR;

	if ( l_vc->nq >= MH$K_TR_XMTQ )
		return	$LOG_FAO(STS$K_ERROR, "Transmit queue of !AZ (!UL) is full, a message is lost", a_node->name, a_node->sysid);

	l_pkt = &l_vc->q[(l_vc->qhead + l_vc->nq++) % MH$K_TR_XMTQ];
	memcpy(l_pkt->data, a_pay, a_len);
	l_pkt->len = (uint16_t) a_len;

	return	s_mh$tr_pump(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Let the pump look again: a block transfer has come to the circuit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
int	mh$tr_kick	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
	if ( !a_node->vc || (a_node->vc->pbstate == MH$K_PB_CLOSED) )
		return	STS$K_SUCCESS;

	return	s_mh$tr_pump(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Queue a sequenced message: SNT_SEQ and the PPD that follows it.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node, its path not CLOSED
**	a_ppd		The PPD: W LENGTH, W MTYPE, data
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no circuit, too long or the queue is full.
**--
*/
int	mh$tr_send_seq	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_ppd,
		unsigned	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$TRPKT	*l_pkt;

	if ( !l_vc || (l_vc->pbstate == MH$K_PB_CLOSED) || (a_len + 1 > l_vc->paysz) )
		return	STS$K_ERROR;

	if ( l_vc->nq >= MH$K_TR_XMTQ )
		return	$LOG_FAO(STS$K_ERROR, "Transmit queue of !AZ (!UL) is full, a message is lost", a_node->name, a_node->sysid);

	l_pkt = &l_vc->q[(l_vc->qhead + l_vc->nq++) % MH$K_TR_XMTQ];
	l_pkt->data[0] = NISCA$K_PPC_SNT_SEQ;
	memcpy(l_pkt->data + 1, a_ppd, a_len);
	l_pkt->len = (uint16_t) (a_len + 1);

	return	s_mh$tr_pump(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a datagram: SNT_DG and the PPD that follows it, at once, without
**	a sequence number or an ACK.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node, its circuit allocated
**	a_ppd		The PPD: W LENGTH, W MTYPE, data
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of MH$LAN_SEND; STS$K_ERROR - too long.
**--
*/
int	mh$tr_send_dg	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_ppd,
		unsigned	a_len
			)
{
uint8_t	l_pay [MH$K_SZ_TRPAY];

	if ( !a_node->vc || (a_len + 1 > a_node->vc->paysz) )
		return	STS$K_ERROR;

	l_pay[0] = NISCA$K_PPC_SNT_DG;
	memcpy(l_pay + 1, a_ppd, a_len);

	return	s_mh$tr_xmit(a_port, a_node, NISCA$M_TR_DATA, 0, l_pay, a_len + 1);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The circuit fails: the transport cannot go on (30 timeouts, a PPC
**	opcode the server does not take).  The channel is restarted, as
**	PEDRIVER does - the peer answers a CCSTART on its open channel by a
**	new path, and its START comes again.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_why		Why, for the operator
**
**  RETURN VALUE:
**	The condition value of MH$CC_RESTART.
**--
*/
static	int	s_mh$tr_fail	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	char *		a_why
			)
{
	mh$ppd_close(a_port, a_node, a_why);

	return	mh$cc_restart(a_port, a_node, a_why);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in the ACK of a packet from the peer.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_ack		The ACK field
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$tr_ack	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		uint32_t	a_ack
			)
{
MH$VC	*l_vc = a_node->vc;
int32_t	l_new = (int32_t) (a_ack - l_vc->lar);
uint8_t	*l_free [MH$K_TR_WINDOW];

	/*
	**  Seen before, or never sent (NAKs are not implemented, by PEDRIVER
	**  neither)
	*/
	if ( (l_new <= 0) || ((unsigned) l_new > l_vc->nxmt) )
		return	STS$K_SUCCESS;

	/*
	**  The slots acknowledged give their buffers to the tail the move
	**  leaves behind: no two slots share one
	*/
	for ( int i = 0; i < l_new; i++ )
		l_free[i] = l_vc->xmt[i].data;

	l_vc->nxmt -= (unsigned) l_new;
	memmove(l_vc->xmt, l_vc->xmt + l_new, l_vc->nxmt * sizeof(l_vc->xmt[0]));

	for ( int i = 0; i < l_new; i++ )
		l_vc->xmt[l_vc->nxmt + (unsigned) i].data = l_free[i];

	l_vc->lar	= a_ack;
	l_vc->retries	= 0;
	l_vc->rto	= MH$K_TR_RTO;
	l_vc->due	= l_vc->nxmt ? a_port->tick + l_vc->rto : 0;

	if ( l_vc->window < l_vc->maxwin )
		l_vc->window++;

	if ( l_vc->window > a_port->tr_maxwin )
		a_port->tr_maxwin = l_vc->window;

	/*
	**  Recovering from a loss: what is still out after this ACK starts at
	**  the next hole - send it again now, not after another timeout
	*/
	if ( l_vc->recover && l_vc->nxmt )
		{
		a_port->tr_rexmt++;

		s_mh$tr_xmit(a_port, a_node, NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK | NISCA$M_TR_RSVP | NISCA$M_TR_REXMT,
			l_vc->xmt[0].seq, l_vc->xmt[0].data, l_vc->xmt[0].len);
		}
	else	l_vc->recover = 0;

	return	s_mh$tr_pump(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Deliver the next packet in order to PPC: SNT_SEQ to PPD, RET_DAT and
**	RET_CNF to MHBLK; any other opcode fails the circuit - the server
**	describes no buffers, so no SNT_DAT or REQ_DAT can be addressed to it.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_pay		The PPC part, opcode onwards
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the layer above.
**--
*/
static	int	s_mh$tr_deliver	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_pay,
		unsigned	a_len
			)
{
	a_port->tr_rx++;

	if ( a_pay[0] == NISCA$K_PPC_SNT_SEQ )
		return	mh$ppd_input(a_port, a_node, a_pay[0], a_pay + 1, a_len - 1);

	if ( !(1 & mh$blk_input(a_port, a_node, a_pay, a_len)) )
		return	s_mh$tr_fail(a_port, a_node, "a block transfer the server does not take");

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in a TR packet of a node whose channel is open.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_tr		The TR header and what follows it
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the layers above.
**--
*/
int	mh$tr_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_tr,
		unsigned	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$TRPKT	*l_slot;
unsigned	l_flags, l_pad, l_paylen;
uint32_t	l_seq, l_ack;
int32_t		l_dist;
uint32_t	l_gen;
int		l_why;
const uint8_t	*l_pay;
int		l_status = STS$K_SUCCESS;

	if ( a_len < NISCA$K_TR_LEN_V10 )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_flags	= a_tr[NISCA$K_TR_FLAG];
	l_pad	= a_tr[NISCA$K_TR_PAD];

	if ( (NISCA$K_TR_PPC_BASE + l_pad > a_len) || ((l_flags & NISCA$M_TR_DATA) && (NISCA$K_TR_PPC_BASE + l_pad >= a_len)) )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	/*
	**  A packet of an older incarnation of the channel
	*/
	if ( (l_pad >= NISCA$K_TR_LEN_V13 - NISCA$K_TR_LEN_V10) && (nisca$get16(a_tr + NISCA$K_TR_RMT_CHSEQ) != a_node->lcl_chseq) )
		{
		a_port->tr_old++;
		return	STS$K_SUCCESS;
		}

	l_pay	= a_tr + NISCA$K_TR_PPC_BASE + l_pad;
	l_paylen = a_len - NISCA$K_TR_PPC_BASE - l_pad;

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "TR from !AZ (!UL): flags !XB seq !UW ack !UW, !UL octets", a_node->name, a_node->sysid,
			l_flags, nisca$get16(a_tr + NISCA$K_TR_SEQ), nisca$get16(a_tr + NISCA$K_TR_ACK), l_paylen);

	/*
	**  Before the circuit is open: datagrams only
	*/
	if ( !l_vc || (l_vc->pbstate == MH$K_PB_CLOSED) )
		{
		if ( (l_flags & (NISCA$M_TR_SEQ | NISCA$M_TR_ACK)) || !(l_flags & NISCA$M_TR_DATA) )
			{
			a_port->rx_drop++;
			return	STS$K_SUCCESS;
			}

		return	mh$ppd_input(a_port, a_node, l_pay[0], l_pay + 1, l_paylen - 1);
		}

	l_gen = l_vc->gen;

	/*
	**  The numbers: the longwords from V1.4 on; the words of an older
	**  header are taken as the nearest longword to where we are
	*/
	if ( l_pad >= NISCA$K_TR_LEN_V14 - NISCA$K_TR_LEN_V10 )
		{
		l_seq = nisca$get32(a_tr + NISCA$K_TR_MSG_SEQ);
		l_ack = nisca$get32(a_tr + NISCA$K_TR_ACK_SEQ);
		}
	else	{
		l_seq = l_vc->hsr + (uint32_t) (int32_t) (int16_t) (nisca$get16(a_tr + NISCA$K_TR_SEQ) - (uint16_t) l_vc->hsr);
		l_ack = l_vc->lar + (uint32_t) (int32_t) (int16_t) (nisca$get16(a_tr + NISCA$K_TR_ACK) - (uint16_t) l_vc->lar);
		}

	if ( l_flags & NISCA$M_TR_ACK )
		{
		if ( !(1 & (l_status = s_mh$tr_ack(a_port, a_node, l_ack))) || (l_vc->gen != l_gen) )
			return	l_status;
		}

	if ( !(l_flags & NISCA$M_TR_SEQ) )
		{
		if ( l_flags & NISCA$M_TR_DATA )
			return	mh$ppd_input(a_port, a_node, l_pay[0], l_pay + 1, l_paylen - 1);

		return	STS$K_SUCCESS;
		}

	/*
	**  The ACK it is owed: at once when asked; with /ACK=HOLD at once for
	**  a batch, otherwise with a packet of ours or on the tick
	*/
	l_vc->nowed++;
	l_why = MH$K_ACKW_RSVP;

	if ( l_flags & NISCA$M_TR_RSVP )
		{
		a_port->tr_rsvp++;
		l_vc->sack = 2;
		}
	else if ( (a_port->ackmode == MH$K_ACK_HOLD) && (l_vc->nowed >= MH$K_TR_ACKBATCH) )
		{
		l_vc->sack = 2;
		l_why = MH$K_ACKW_BATCH;
		}
	else if ( !l_vc->sack )
		l_vc->sack = 1;

	l_dist = (int32_t) (l_seq - l_vc->hsr);

	if ( l_dist <= 0 )
		{
		/*
		**  Seen and delivered before: an ACK at once helps to stop the
		**  duplication
		*/
		a_port->tr_dup++;
		l_vc->sack = 2;
		l_why = MH$K_ACKW_DUP;
		}
	else if ( l_dist == 1 )
		{
		l_vc->hsr = l_seq;
		l_status = s_mh$tr_deliver(a_port, a_node, l_pay, l_paylen);

		/*
		**  The cache, in order after it
		*/
		while ( (l_vc->gen == l_gen) && (l_slot = &l_vc->rcv[(l_vc->hsr + 1) % MH$K_TR_RCVCACHE])->valid
			&& (l_slot->seq == l_vc->hsr + 1) )
			{
			l_slot->valid = 0;
			l_vc->hsr++;
			l_status = s_mh$tr_deliver(a_port, a_node, l_slot->data, l_slot->len);
			}
		}
	else if ( l_dist <= MH$K_TR_RCVCACHE )
		{
		l_slot = &l_vc->rcv[l_seq % MH$K_TR_RCVCACHE];
		l_slot->valid	= 1;
		l_slot->seq	= l_seq;
		l_slot->len	= (uint16_t) $MIN(l_paylen, l_vc->paysz);
		memcpy(l_slot->data, l_pay, l_slot->len);

		a_port->tr_ooo++;
		}
	else	a_port->rx_drop++;

	if ( (l_vc->gen == l_gen) && (l_vc->sack == 2) )
		s_mh$tr_bare(a_port, a_node, l_why);

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The 20 ms clock of the transport of a node: the ACK we owe, the
**	timeout of the head of the transmit cache.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
int	mh$tr_tick	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC	*l_vc = a_node->vc;

	if ( !l_vc || (l_vc->pbstate == MH$K_PB_CLOSED) )
		return	STS$K_SUCCESS;

	if ( l_vc->sack )
		s_mh$tr_bare(a_port, a_node, MH$K_ACKW_TICK);

	if ( !l_vc->nxmt || (a_port->tick < l_vc->due) )
		return	STS$K_SUCCESS;

	if ( ++l_vc->retries > MH$K_TR_MAXREXMT )
		return	s_mh$tr_fail(a_port, a_node, "no acknowledgement after 30 retransmissions");

	l_vc->window	= 1;
	l_vc->recover	= 1;
	l_vc->rto	= $MIN(2 * l_vc->rto, MH$K_TR_RTO_MAX);
	l_vc->due	= a_port->tick + l_vc->rto;

	a_port->tr_rexmt++;

	return	s_mh$tr_xmit(a_port, a_node, NISCA$M_TR_DATA | NISCA$M_TR_SEQ | NISCA$M_TR_ACK | NISCA$M_TR_RSVP | NISCA$M_TR_REXMT,
			l_vc->xmt[0].seq, l_vc->xmt[0].data, l_vc->xmt[0].len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Pay the ACKs owed, once the frames that came in together have all
**	been taken: one ACK for the lot, at once - /ACK=FLUSH.  With
**	/ACK=HOLD nothing is done here: the ACK owed waits for a packet of
**	ours (the END of a command, the data of a READ), for a batch of
**	MH$K_TR_ACKBATCH or for the tick.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
int	mh$tr_flush	(
		MH$PORT *	a_port
			)
{
int	l_status = STS$K_SUCCESS;

	if ( a_port->ackmode == MH$K_ACK_HOLD )
		return	STS$K_SUCCESS;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_node = &a_port->nodes[i];

		if ( (l_node->state == MH$K_CH_OPEN) && l_node->vc && (l_node->vc->pbstate != MH$K_PB_CLOSED) && l_node->vc->sack )
			l_status = s_mh$tr_bare(a_port, l_node, MH$K_ACKW_FLUSH);
		}

	return	l_status;
}
