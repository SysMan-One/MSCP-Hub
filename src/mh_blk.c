#define	__MODULE__	"MHBLK"
#define	__IDENT__	"X01-10"
#define	__REV__		"1.10.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_blk.c
**
**  ABSTRACT:	The block transfers of PPC, the way PEDRIVER does them over
**		the LAN: the data of a READ goes to the
**		named buffer of the client in SNT_DAT chunks, the data of a
**		WRITE is asked for by REQ_DAT0 and comes in RET_DAT chunks.
**
**  DESCRIPTION: SEND_DATA (READ): the chunks are made as the window of the
**		transport opens (MH$BLK_CHUNK, called by the pump of MHTR) -
**		one transfer at a time per circuit, as PEDRIVER keeps the one
**		it has begun at the head of its command queue.  A chunk is
**		XCT_ID (our LCONID, the TAG of the command), XCT_LEN = its data,
**		SND = our buffer (TAG) and offset, REC = the name of the client
**		and BOFF + what has gone, the data; the last is SNT_DAT_LP.
**		The client answers it by RET_CNF; only then the END goes - the
**		client UNMAPs its buffer on the END, and a chunk after it would
**		be BUG_CHECK VAXPORT there.
**
**		REQUEST_DATA (WRITE, COMPARE): one REQ_DAT0 for all the data,
**		SND = the name of the client and its BOFF, REC = our TAG at 0;
**		the PEDRIVER of the client answers by itself (its DUDRIVER does
**		not wake up) in RET_DAT chunks of the size of its circuit, the
**		last RET_DAT_LP.  XCT_LEN of a chunk is its own data, not
**		what is left.
**
**		The checks of the client are
**		the reason for the strictness here: a chunk never goes past
**		BCNT, the name is the one of the command.  A chunk that comes
**		for nothing of ours (an old XCT_ID, a name or offset out of
**		place) is dropped and said once by BLKERR - it hurts neither
**		side.
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

#include	<string.h>

#include	"mh_def.h"
#include	"mscp.h"



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Fill the 28 octets of the header of a block transfer.
**
**  FORMAL PARAMETERS:
**
**	a_h		Output: the header
**	a_hrb		The command
**	a_len		XCT_LEN
**	a_sname, a_soff	SND_NAME, SND_BOFF
**	a_rname, a_roff	REC_NAME, REC_BOFF
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$blk_hdr	(
		uint8_t *	a_h,
	const	MH$HRB *	a_hrb,
		uint32_t	a_len,
		uint32_t	a_sname,
		uint32_t	a_soff,
		uint32_t	a_rname,
		uint32_t	a_roff
			)
{
	nisca$put32(a_h + NISCA$K_BLK_XCT_ID, a_hrb->lconid);
	nisca$put32(a_h + NISCA$K_BLK_XCT_ID + 4, a_hrb->tag);
	nisca$put32(a_h + NISCA$K_BLK_XCT_LEN, a_len);
	nisca$put32(a_h + NISCA$K_BLK_SND_NAME, a_sname);
	nisca$put32(a_h + NISCA$K_BLK_SND_BOFF, a_soff);
	nisca$put32(a_h + NISCA$K_BLK_REC_NAME, a_rname);
	nisca$put32(a_h + NISCA$K_BLK_REC_BOFF, a_roff);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	SEND_DATA: the BCNT octets of the buffer of a command go to the buffer
**	of the client.  The transfer waits its turn on the circuit; the chunks
**	are made by MH$BLK_CHUNK.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of MH$TR_KICK.
**--
*/
int	mh$blk_send	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
MH$VC	*l_vc = a_hrb->node->vc;

	a_hrb->state	= MH$K_HRB_SNDAT;
	a_hrb->moved	= 0;
	a_hrb->vcnext	= NULL;

	if ( l_vc->blktail )
		l_vc->blktail->vcnext = a_hrb;
	else	l_vc->blkhead = a_hrb;

	l_vc->blktail = a_hrb;

	return	mh$tr_kick(a_port, a_hrb->node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The next chunk of the block transfer at the head of a circuit: the
**	pump of the transport asks for it when the window has room and no
**	message waits.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_pay		Output: the PPC part, opcode onwards, MH$K_SZ_TRPAY
**	a_len		Output: its length
**
**  RETURN VALUE:
**	1 - a chunk is there; 0 - no transfer waits.
**--
*/
int	mh$blk_chunk	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		uint8_t *	a_pay,
		unsigned *	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$HRB		*l_hrb;
uint32_t	l_max, l_size;
int		l_last;

	if ( !l_vc || !(l_hrb = l_vc->blkhead) )
		return	0;

	/*
	**  The size of the chunk: what the peer takes and what we said we take
	**  (BUS_BUFFER_SIZE of the CC messages); 1426 on an Ethernet
	*/
	l_max = a_port->bufsz;

	if ( a_node->bufsz && (a_node->bufsz < l_max) )
		l_max = a_node->bufsz;

	l_size = $MIN(l_hrb->bcnt - l_hrb->moved, l_max);
	l_last = (l_hrb->moved + l_size == l_hrb->bcnt);

	a_pay[0] = l_last ? NISCA$K_PPC_SNT_DAT_LP : NISCA$K_PPC_SNT_DAT;
	s_mh$blk_hdr(a_pay + 1, l_hrb, l_size, l_hrb->tag, l_hrb->moved, l_hrb->bname, l_hrb->boff + l_hrb->moved);
	memcpy(a_pay + 1 + NISCA$K_BLK_HLEN, l_hrb->buf + l_hrb->moved, l_size);
	*a_len = 1 + NISCA$K_BLK_HLEN + l_size;

	l_hrb->moved += l_size;
	l_hrb->cmd_sts--;
	a_port->blk_snt++;
	a_port->blk_bytes_out += l_size;

	if ( l_last )
		{
		/*
		**  All is out: off the circuit, RET_CNF is awaited
		*/
		if ( !(l_vc->blkhead = l_hrb->vcnext) )
			l_vc->blktail = NULL;

		l_hrb->vcnext	= NULL;
		l_hrb->state	= MH$K_HRB_RETCNF;
		}

	return	1;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	REQUEST_DATA: ask the client for the BCNT octets of its buffer; they
**	come into BUF (FETCH) or BUF2 (CMPHOST) by MH$BLK_INPUT.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of MH$TR_SEND_PPC.
**--
*/
int	mh$blk_request	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
uint8_t	l_pay [1 + NISCA$K_BLK_HLEN];

	a_hrb->state	= MH$K_HRB_REQDAT;
	a_hrb->moved	= 0;

	l_pay[0] = NISCA$K_PPC_REQ_DAT0;
	s_mh$blk_hdr(l_pay + 1, a_hrb, a_hrb->bcnt, a_hrb->bname, a_hrb->boff, a_hrb->tag, 0);

	a_port->blk_req++;

	return	mh$tr_send_ppc(a_port, a_hrb->node, l_pay, sizeof(l_pay));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command ends before its transfer is out (the connection is gone):
**	take it off the circuit.  The chunks already in the transport stay,
**	the client still holds its buffer.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$blk_cancel	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
MH$VC	*l_vc = a_hrb->node ? a_hrb->node->vc : NULL;
MH$HRB	**l_pp, *l_prev = NULL;

	if ( !l_vc || (a_hrb->state != MH$K_HRB_SNDAT) )
		return	STS$K_SUCCESS;

	for ( l_pp = &l_vc->blkhead; *l_pp; l_prev = *l_pp, l_pp = &(*l_pp)->vcnext )
		{
		if ( *l_pp != a_hrb )
			continue;

		*l_pp = a_hrb->vcnext;

		if ( l_vc->blktail == a_hrb )
			l_vc->blktail = l_prev;

		break;
		}

	a_hrb->vcnext = NULL;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Say that a chunk is dropped.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_opc		The PPC opcode
**	a_xct		The XCT_ID: the low and the high longword
**	a_why		Why
**
**  RETURN VALUE:
**	STS$K_SUCCESS: a stray chunk hurts nothing.
**--
*/
static	int	s_mh$blk_bad	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		unsigned	a_opc,
	const	uint8_t *	a_xct,
	const	char *		a_why
			)
{
	a_port->blk_bad++;

	$MHMSG(MH$_BLKERR, (a_opc == NISCA$K_PPC_RET_CNF) ? "RET_CNF" : ((a_opc == NISCA$K_PPC_RET_DAT_LP) ? "RET_DAT_LP" : "RET_DAT"),
		a_node->name, a_node->sysid, nisca$get32(a_xct + 4), nisca$get32(a_xct), a_why);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A block transfer packet of the client, in order: RET_CNF of our
**	SNT_DAT, RET_DAT of our REQ_DAT.  Any other opcode is not for a
**	server that describes no buffers to its peers.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_pay		The PPC part, opcode onwards
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - an opcode the server does not take, the
**	circuit is to fail.
**--
*/
int	mh$blk_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_pay,
		unsigned	a_len
			)
{
const uint8_t	*l_h = a_pay + 1;
unsigned	l_opc = a_pay[0];
uint32_t	l_roff, l_dlen;
MH$HRB		*l_hrb;
uint8_t		*l_into;

	switch ( l_opc )
		{
		case	NISCA$K_PPC_RET_CNF:
			if ( a_len < 1 + NISCA$K_BLK_CNF_LEN )
				return	STS$K_ERROR;

			if ( !(l_hrb = mh$mscp_hrb(a_port, a_node, nisca$get32(l_h), nisca$get32(l_h + 4))) || (l_hrb->state != MH$K_HRB_RETCNF) )
				return	s_mh$blk_bad(a_port, a_node, l_opc, l_h, "no command of ours waits for it");

			a_port->blk_cnf++;

			return	mh$mscp_xferdone(a_port, l_hrb, 1);

		case	NISCA$K_PPC_RET_DAT:
		case	NISCA$K_PPC_RET_DAT_LP:
			if ( a_len < 1 + NISCA$K_BLK_HLEN )
				return	STS$K_ERROR;

			if ( !(l_hrb = mh$mscp_hrb(a_port, a_node, nisca$get32(l_h), nisca$get32(l_h + 4))) || (l_hrb->state != MH$K_HRB_REQDAT) )
				return	s_mh$blk_bad(a_port, a_node, l_opc, l_h, "no command of ours waits for it");

			l_roff	= nisca$get32(l_h + NISCA$K_BLK_REC_BOFF);
			l_dlen	= a_len - 1 - NISCA$K_BLK_HLEN;

			if ( nisca$get32(l_h + NISCA$K_BLK_REC_NAME) != l_hrb->tag )
				return	s_mh$blk_bad(a_port, a_node, l_opc, l_h, "REC_NAME is not the name we gave");

			if ( (l_roff > l_hrb->bcnt) || (l_dlen > l_hrb->bcnt - l_roff) )
				return	s_mh$blk_bad(a_port, a_node, l_opc, l_h, "the data goes past the end of the buffer");

			l_into = (l_hrb->phase == MH$K_PH_CMPHOST) ? l_hrb->buf2 : l_hrb->buf;
			memcpy(l_into + l_roff, l_h + NISCA$K_BLK_HLEN, l_dlen);

			l_hrb->moved += l_dlen;
			l_hrb->cmd_sts--;
			a_port->blk_ret++;
			a_port->blk_bytes_in += l_dlen;

			if ( l_opc != NISCA$K_PPC_RET_DAT_LP )
				return	STS$K_SUCCESS;

			/*
			**  The last: in order, so all the data is there - or the
			**  client has sent less than asked
			*/
			if ( l_hrb->moved != l_hrb->bcnt )
				{
				s_mh$blk_bad(a_port, a_node, l_opc, l_h, "less data than asked for");
				return	mh$mscp_xferdone(a_port, l_hrb, 0);
				}

			return	mh$mscp_xferdone(a_port, l_hrb, 1);
		}

	return	STS$K_ERROR;
}
