#define	__MODULE__	"MHPPD"
#define	__IDENT__	"X01-10"
#define	__REV__		"1.10.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_ppd.c
**
**  ABSTRACT:	PPD: the path of SCS to a node over its virtual circuit - the
**		START/STACK/ACK handshake, the SB of the node, HOSTSHUT.
**
**  DESCRIPTION: The first open channel of a VMS node makes its PEDRIVER
**		report a path to SCS, and SCSNET starts the handshake with a
**		START datagram.  The server only answers: it
**		takes the SB of the node out of the START, opens the numbers
**		of its transport and sends STACK - the node opens its circuit
**		on our STACK and says ACK.  A lost STACK is no matter: the node
**		sends START every 5 s for as long as its channel is open.
**
**		  START    CLOSED -> ST_REC, our STACK; ST_REC - our STACK
**			   again; OPEN - the node has restarted: the path is
**			   closed and opened again
**		  ACK, STACK, any SCS message
**			   ST_REC -> OPEN
**		  HOSTSHUT the node is going down: closed
**
**		The STACK tells SWTYPE 'MSCH', never 'VMS ': a VMS node runs
**		its connection manager and the watchdog of the circuit only
**		to an SB of SWTYPE 'VMS '.  SWINCARN is the
**		time the server started, the same for its lifetime.
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

static	const char *	s_ppdtype [] = { "START", "STACK", "ACK", "SCS_DG", "SCS_MSG", "ELOG", "HOSTSHUT" };



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Copy a blank-padded text field of the START into an ASCIZ string.
**
**  FORMAL PARAMETERS:
**
**	a_src		The field
**	a_len		Its length
**	a_dst		Output: a_len + 1 octets, trailing blanks dropped
**
**  RETURN VALUE:
**	The length of the string.
**--
*/
static	unsigned	s_mh$ppd_text	(
	const	uint8_t *	a_src,
		unsigned	a_len,
		char *		a_dst
			)
{
	memcpy(a_dst, a_src, a_len);

	while ( a_len && ((a_dst[a_len - 1] == ' ') || !a_dst[a_len - 1]) )
		a_len--;

	a_dst[a_len] = '\0';

	return	a_len;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send our STACK: the START data about the server.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	The condition value of MH$TR_SEND_DG.
**--
*/
static	int	s_mh$ppd_stack	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
uint8_t		l_ppd [NISCA$K_PPD_DATA + NISCA$K_ST_LEN] = {0}, *l_st = l_ppd + NISCA$K_PPD_DATA;
uint64_t	l_now = 0;

	nisca$put16(l_ppd + NISCA$K_PPD_LENGTH, NISCA$K_ST_LENGTH);
	nisca$put16(l_ppd + NISCA$K_PPD_MTYPE, NISCA$K_PPD_STACK);

	nisca$put16(l_st + NISCA$K_ST_SYSTEMID, a_port->sysid);
	l_st[NISCA$K_ST_PROTOCOL] = 1;
	nisca$put16(l_st + NISCA$K_ST_MAXDG, MH$K_PPD_MAXDG);
	nisca$put16(l_st + NISCA$K_ST_MAXMSG, MH$K_PPD_MAXMSG);
	memcpy(l_st + NISCA$K_ST_SWTYPE, MH$T_PPD_SWTYPE, 4);
	memcpy(l_st + NISCA$K_ST_SWVERS, MH$T_PPD_SWVERS, 4);
	nisca$put64(l_st + NISCA$K_ST_SWINCARN, a_port->incarn);
	memcpy(l_st + NISCA$K_ST_HWTYPE, MH$T_PPD_HWTYPE, 4);
	memset(l_st + NISCA$K_ST_NODENAME, ' ', MH$K_SZ_SCSNODE);
	memcpy(l_st + NISCA$K_ST_NODENAME, a_port->ccname + 1, NISCA$K_SZ_NODENAME);

	mh$vmstime(&l_now);
	nisca$put64(l_st + NISCA$K_ST_CURTIME, l_now);

	return	mh$tr_send_dg(a_port, a_node, l_ppd, sizeof(l_ppd));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The path is open: ENTER_PB.
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
static	int	s_mh$ppd_open	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC	*l_vc = a_node->vc;

	l_vc->pbstate = MH$K_PB_OPEN;
	l_vc->opens++;

	$MHMSG(MH$_VCOPEN, a_node->name, a_node->sysid, l_vc->swtype, l_vc->swvers, l_vc->hwtype, l_vc->maxdg, l_vc->maxmsg);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Close the path and the circuit of a node; the SB is kept.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_why		Why, for the operator: an open path that closes is
**			reported by VCCLOSE, a handshake under /TRACE only
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$ppd_close	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	char *		a_why
			)
{
	if ( !a_node->vc || (a_node->vc->pbstate == MH$K_PB_CLOSED) )
		return	STS$K_SUCCESS;

	if ( a_node->vc->pbstate == MH$K_PB_OPEN )
		$MHMSG(MH$_VCCLOSE, a_node->name, a_node->sysid, a_why);
	else if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "Path to !AZ (!UL) closed in the handshake: !AZ", a_node->name, a_node->sysid, a_why);

	return	mh$tr_reset(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in a START: check that it is about the node of the channel,
**	keep its SB, open the transport, answer by STACK.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_st		The START data
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the layers below.
**--
*/
static	int	s_mh$ppd_start	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_st,
		unsigned	a_len
			)
{
MH$VC		*l_vc;
char		l_name [MH$K_SZ_SCSNODE + 1];
uint64_t	l_sysid;
int		l_status;

	if ( a_len < NISCA$K_ST_LEN )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	/*
	**  The SB must be the node of the channel: the low word of its
	**  SYSTEMID is the DX address, the name is the one of its CC
	*/
	l_sysid = nisca$get32(a_st + NISCA$K_ST_SYSTEMID) | ((uint64_t) nisca$get16(a_st + NISCA$K_ST_SYSTEMID + 4) << 32);
	s_mh$ppd_text(a_st + NISCA$K_ST_NODENAME, MH$K_SZ_SCSNODE, l_name);

	if ( ((uint16_t) l_sysid != a_node->sysid) || strcmp(l_name, a_node->name) )
		{
		a_port->rx_drop++;

		return	$LOG_FAO(STS$K_WARN, "START of !AZ (!UL) is about !AZ (!UQ), ignored", a_node->name, a_node->sysid, l_name, l_sysid);
		}

	if ( a_node->vc && (a_node->vc->pbstate == MH$K_PB_ST_REC) )
		return	s_mh$ppd_stack(a_port, a_node);

	if ( a_node->vc && (a_node->vc->pbstate == MH$K_PB_OPEN) )
		mh$ppd_close(a_port, a_node, "restarted by the node (START on the open path)");

	if ( !(1 & (l_status = mh$tr_open(a_port, a_node))) )
		return	l_status;

	l_vc = a_node->vc;
	l_vc->sysid	= l_sysid;
	l_vc->maxdg	= nisca$get16(a_st + NISCA$K_ST_MAXDG);
	l_vc->maxmsg	= nisca$get16(a_st + NISCA$K_ST_MAXMSG);
	l_vc->incarn	= nisca$get64(a_st + NISCA$K_ST_SWINCARN);
	s_mh$ppd_text(a_st + NISCA$K_ST_SWTYPE, 4, l_vc->swtype);
	s_mh$ppd_text(a_st + NISCA$K_ST_SWVERS, 4, l_vc->swvers);
	s_mh$ppd_text(a_st + NISCA$K_ST_HWTYPE, 4, l_vc->hwtype);

	l_vc->pbstate = MH$K_PB_ST_REC;

	return	s_mh$ppd_stack(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in the PPD of a datagram or of a sequenced message.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_opc		The PPC opcode: SNT_DG or SNT_SEQ
**	a_ppd		The PPD: W LENGTH, W MTYPE, data
**	a_len		Its length, as received
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the layers.
**--
*/
int	mh$ppd_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		unsigned	a_opc,
	const	uint8_t *	a_ppd,
		unsigned	a_len
			)
{
unsigned	l_length, l_mtype;
MH$VC		*l_vc = a_node->vc;

	if ( a_len < NISCA$K_PPD_DATA )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_length = nisca$get16(a_ppd + NISCA$K_PPD_LENGTH);
	l_mtype	= nisca$get16(a_ppd + NISCA$K_PPD_MTYPE);

	if ( (l_length < 2) || (l_length + NISCA$K_PPD_MTYPE > a_len) )
		{
		a_port->rx_drop++;
		return	$LOG_FAO(STS$K_WARN, "PPD of !AZ (!UL): LENGTH !UL, !UL octets received, dropped", a_node->name, a_node->sysid, l_length, a_len);
		}

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "PPD !AZ from !AZ (!UL), LENGTH !UL, !AZ", (l_mtype < $ARRSZ(s_ppdtype)) ? s_ppdtype[l_mtype] : "?",
			a_node->name, a_node->sysid, l_length, (a_opc == NISCA$K_PPC_SNT_SEQ) ? "sequenced" : "datagram");

	switch ( l_mtype )
		{
		case	NISCA$K_PPD_START:
			return	s_mh$ppd_start(a_port, a_node, a_ppd + NISCA$K_PPD_DATA, l_length - 2);

		case	NISCA$K_PPD_STACK:
		case	NISCA$K_PPD_ACK:
			if ( l_vc && (l_vc->pbstate == MH$K_PB_ST_REC) )
				return	s_mh$ppd_open(a_port, a_node);

			return	STS$K_SUCCESS;

		case	NISCA$K_PPD_SCS_MSG:
			if ( !l_vc || (l_vc->pbstate == MH$K_PB_CLOSED) )
				return	STS$K_SUCCESS;

			/*
			**  The ACK was lost: the node's first message opens the
			**  path as well
			*/
			if ( l_vc->pbstate == MH$K_PB_ST_REC )
				s_mh$ppd_open(a_port, a_node);

			return	mh$scs_input(a_port, a_node, a_ppd + NISCA$K_PPD_MTYPE, l_length);

		case	NISCA$K_PPD_HOSTSHUT:
			return	mh$ppd_close(a_port, a_node, "HOSTSHUT received, the node is shutting down");

		default:
			/*
			**  SCS datagrams and ELOG: nothing for the server
			*/
			return	STS$K_SUCCESS;
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The server stops: HOSTSHUT on every open path, so that the nodes break
**	their circuits at once.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$ppd_shutdown	(
		MH$PORT *	a_port
			)
{
uint8_t	l_ppd [NISCA$K_PPD_DATA];

	nisca$put16(l_ppd + NISCA$K_PPD_LENGTH, 2);
	nisca$put16(l_ppd + NISCA$K_PPD_MTYPE, NISCA$K_PPD_HOSTSHUT);

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_node = &a_port->nodes[i];

		if ( !l_node->vc || (l_node->vc->pbstate != MH$K_PB_OPEN) )
			continue;

		mh$tr_send_dg(a_port, l_node, l_ppd, sizeof(l_ppd));
		mh$ppd_close(a_port, l_node, "the server is stopping");
		}

	return	STS$K_SUCCESS;
}
