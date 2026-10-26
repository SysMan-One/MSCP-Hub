#define	__MODULE__	"MHCC"
#define	__IDENT__	"X01-23"
#define	__REV__		"1.23.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_cc.c
**
**  ABSTRACT:	NISCA channel control: the DX header, the CC message, the
**		HELLO of the server, the table of the nodes seen on the LAN and
**		the state machine of the channel to each of them.
**
**  DESCRIPTION: The server speaks CC V1.3 (98 octets), the version of the
**		satellite boot driver NISCA_BTDRIVER, which PEDRIVER of every
**		VMS node accepts: PEDRIVER compares nothing but MAJOR and tells
**		the version by the length of the message.
**
**		Before the first HELLO the server keeps silent for /LISTEN
**		seconds and learns the nodes from their HELLO: a node that
**		already uses the low word of our SCSSYSTEMID (the DX address,
**		AA-00-04-00-lo-hi - two such nodes cannot be told apart) or
**		our SCSNODE is a reason to stop at once, never to transmit -
**		an identity clash with a VMS node can make it CLUEXIT.
**		The check stays on for the lifetime of the server.
**
**		The channel to a node follows the state machine of PEDRIVER,
**		cut to the states the server goes through:
**
**		  HELLO    CLOSED -> HELLO_DLY; 0.5 - 2 s later our CCSTART
**		  CCSTART  any -> CCSTART_REC, our VERF (OPEN: a restart)
**		  VERF     CCSTART_SENT/REC -> OPEN, our VACK; OPEN - our
**			   VACK (the size and delay probes of PEDRIVER)
**		  VACK     CCSTART_REC -> OPEN
**		  SOLICIT_SRV of a disk we serve
**			   CLOSED, HELLO_DLY, CCSTART_SENT -> SOLICIT_DLY;
**			   0 - 0.5 s later our VERF -> CCSTART_REC; there
**			   again - our VERF again; OPEN - a restart, our VERF
**		  BYE      -> CLOSED
**		  5 s      CCSTART_SENT/REC -> CLOSED
**		  8 s      OPEN, nothing heard from the node -> CLOSED
**
**		A SOLICIT_SRV comes from the boot driver of a satellite, to
**		the group, every 3 s until a server of its system disk
**		answers by VERF: the boot driver makes a channel of nothing
**		else.  Such a channel is marked BOOT: when it opens, the server
**		starts the path itself (MHPPD), and it is kept 30 s without a
**		word, not 8 - the boot driver can be quiet at SYSBOOT>.  The
**		boot driver says BYE at the end of each part it loads and
**		asks again; the runtime PEDRIVER of the satellite comes with
**		HELLO and CCSTART, and a CCSTART on the channel of the boot
**		driver restarts it as an ordinary one.  The pause
**		before the VERF is shorter than the 0 - 2 s of PEDRIVER: the
**		boot driver takes the first VERF and drops the others.
**
**		Every message but HELLO must carry the AUTHORIZE bit and the
**		password hash of the group; one that does not is counted and
**		does not move the channel (PEDRIVER: events BAD_ECO and
**		AUTHORIZE).  Under /PASSIVE, and during the listen period, the
**		machine does not run at all: nothing is ever sent.
**
**		The TR packets of an open channel go to the transport (MHTR);
**		a channel that closes takes its virtual circuit with it.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-11 : /ACK=HOLD is the default.
**		20-OCT-2026	RRL	X01-13 : PIPE_QUOTA 16.
**		24-OCT-2026	RRL	X01-20 : jumbo frames: BUS_BUFFER_SIZE by an MTU up to 9000, JUMBO.
**		25-OCT-2026	RRL	X01-21 : SOLICIT_SRV: the channel of the boot driver of a satellite.
**		26-OCT-2026	RRL	X01-23 : an adapter of a node already known is a channel of that node: its
**						owner keeps the circuit; the circuit goes by the open channel
**						of the largest buffer, with the last one closed; a SOLICIT_SRV
**						closes the other channels of the node.
**
**--
*/

#include	<errno.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>

#include	"mh_def.h"

/*
**  Events of the channel: the first five are the CC message types
*/
enum	{
	MH$K_CE_HELLO		= NISCA$K_CC_HELLO,
	MH$K_CE_BYE		= NISCA$K_CC_BYE,
	MH$K_CE_CCSTART		= NISCA$K_CC_CCSTART,
	MH$K_CE_VERF		= NISCA$K_CC_VERF,
	MH$K_CE_VACK		= NISCA$K_CC_VACK,
	MH$K_CE_HS_TMO,				/* The handshake timer				*/
	MH$K_CE_LISTN_TMO,			/* Nothing heard on an open channel		*/
	MH$K_CE_SOLICIT,			/* SOLICIT_SRV of a disk we serve		*/

	MH$K_CE_NEVENT				/* End-Of-List				*/
	};

/*
**  Actions, after the ones of the state machine of PEDRIVER
*/
enum	{
	MH$K_CA_NONE		= 0,		/* "."  - ignored				*/
	MH$K_CA_DELAY,				/* A31  - RAND_WAIT -> HELLO_DLY			*/
	MH$K_CA_CCSTART,			/* A51  - SEND_CCSTART -> CCSTART_SENT		*/
	MH$K_CA_VERF,				/* A61/A62 - SEND_VERF -> CCSTART_REC		*/
	MH$K_CA_RESTART,			/* A63  - PATH_RESTART + SEND_VERF		*/
	MH$K_CA_OPEN_VACK,			/* A71  - UPDATE, SEND_VACK, OPEN_PATH		*/
	MH$K_CA_OPEN,				/* A72  - UPDATE, OPEN_PATH			*/
	MH$K_CA_VACK,				/* A84  - UPDATE, SEND_VACK (a probe)		*/
	MH$K_CA_UPDATE,				/* A85  - UPDATE				*/
	MH$K_CA_CLOSE,				/* A11/A12/A13/A81 - SHUTDOWN_CH		*/
	MH$K_CA_SOLICIT,			/* A21  - RAND_WAIT -> SOLICIT_DLY		*/
	MH$K_CA_SOL_VERF,			/* A41/A62 - SEND_VERF -> CCSTART_REC, BOOT	*/
	MH$K_CA_SOL_RESTART			/* A42  - PATH_RESTART + SEND_VERF		*/
	};

static	const uint8_t	s_cc_table [MH$K_CE_NEVENT] [MH$K_CH_NSTATE] = {
	/*			CLOSED		CCSTART_SENT		CCSTART_REC		OPEN			HELLO_DLY		SOLICIT_DLY	*/
	[MH$K_CE_HELLO]	    = { MH$K_CA_DELAY,	MH$K_CA_CCSTART,	MH$K_CA_NONE,		MH$K_CA_NONE,		MH$K_CA_NONE,		MH$K_CA_NONE },
	[MH$K_CE_BYE]	    = { MH$K_CA_NONE,	MH$K_CA_CLOSE,		MH$K_CA_CLOSE,		MH$K_CA_CLOSE,		MH$K_CA_CLOSE,		MH$K_CA_CLOSE },
	[MH$K_CE_CCSTART]   = { MH$K_CA_VERF,	MH$K_CA_VERF,		MH$K_CA_VERF,		MH$K_CA_RESTART,	MH$K_CA_VERF,		MH$K_CA_VERF },
	[MH$K_CE_VERF]	    = { MH$K_CA_NONE,	MH$K_CA_OPEN_VACK,	MH$K_CA_OPEN_VACK,	MH$K_CA_VACK,		MH$K_CA_NONE,		MH$K_CA_NONE },
	[MH$K_CE_VACK]	    = { MH$K_CA_NONE,	MH$K_CA_NONE,		MH$K_CA_OPEN,		MH$K_CA_UPDATE,		MH$K_CA_NONE,		MH$K_CA_NONE },
	[MH$K_CE_HS_TMO]    = { MH$K_CA_NONE,	MH$K_CA_CLOSE,		MH$K_CA_CLOSE,		MH$K_CA_NONE,		MH$K_CA_CCSTART,	MH$K_CA_SOL_VERF },
	[MH$K_CE_LISTN_TMO] = { MH$K_CA_NONE,	MH$K_CA_NONE,		MH$K_CA_NONE,		MH$K_CA_CLOSE,		MH$K_CA_NONE,		MH$K_CA_NONE },
	[MH$K_CE_SOLICIT]   = { MH$K_CA_SOLICIT, MH$K_CA_SOLICIT,	MH$K_CA_SOL_VERF,	MH$K_CA_SOL_RESTART,	MH$K_CA_SOLICIT,	MH$K_CA_NONE }
	};

static	const char *	s_cctype [] = { "HELLO", "BYE", "CCSTART", "VERF", "VACK", "DONE", "SOLICIT_SRV" };
static	const char *	s_event [MH$K_CE_NEVENT] = { "HELLO", "BYE", "CCSTART", "VERF", "VACK", "HS_TMO", "LISTN_TMO", "SOLICIT_SRV" };
static	const char *	s_state [MH$K_CH_NSTATE] = { "CLOSED", "CCSTART_SENT", "CCSTART_REC", "OPEN", "HELLO_DLY", "SOLICIT_DLY" };

/*
**  Why a channel is closed, by the event that closes it
*/
static	const char *	s_why [MH$K_CE_NEVENT] = {
	[MH$K_CE_BYE]		= "BYE received, the node is going down",
	[MH$K_CE_CCSTART]	= "restarted by the node",
	[MH$K_CE_HS_TMO]	= "no answer to the handshake in 5 seconds",
	[MH$K_CE_LISTN_TMO]	= "nothing heard from the node for 8 seconds",
	[MH$K_CE_SOLICIT]	= "the boot driver of the node asks for its system disk again"
	};

static	unsigned	s_seed;			/* RAND_R state: the jitter of HELLO and CCSTART	*/

static	int	s_mh$cc_namelen	(const MH$PORT *a_port);
static	int	s_mh$ch_close	(MH$PORT *a_port, MH$NODE *a_node, const char *a_why);
static	int	s_mh$ch_event	(MH$PORT *a_port, MH$NODE *a_node, unsigned a_event, const uint8_t *a_cc, unsigned a_cclen);



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Prepare the CC layer of the port from the configuration: the group and
**	its multicast address (joined on the interface), the node address,
**	the password hash, the names in the form CC carries them, the size of
**	the data field of a block, the incarnation, the listen period; when
**	the ACK owed by the transport goes alone (/ACK).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context, opened by MH$LAN_OPEN
**	a_cfg		The configuration, checked by the caller
**
**  RETURN VALUE:
**	STS$K_SUCCESS; a condition value that has been signalled otherwise.
**--
*/
int	mh$cc_init	(
		MH$PORT *	a_port,
	const	MH$CFG *	a_cfg
			)
{
int		l_status;
unsigned	l_len, l_mtu;

	a_port->group	= (uint16_t) a_cfg->group;
	a_port->sysid	= (uint16_t) a_cfg->sysid;
	a_port->passive	= a_cfg->passive;
	a_port->trace	= a_cfg->trace;

	/*
	**  /ACK=HOLD (the default) | FLUSH
	*/
	a_port->ackmode = MH$K_ACK_HOLD;

	if ( (l_len = $ASCLEN(&a_cfg->ack)) )
		{
		if ( (l_len == 5) && !strncasecmp($ASCPTR(&a_cfg->ack), "FLUSH", 5) )
			a_port->ackmode = MH$K_ACK_FLUSH;
		else if ( (l_len == 4) && !strncasecmp($ASCPTR(&a_cfg->ack), "HOLD", 4) )
			a_port->ackmode = MH$K_ACK_HOLD;
		else	return	$MHMSG(MH$_IVPARAM, $ASCPTR(&a_cfg->ack), "ACK", "FLUSH or HOLD is expected");
		}

	nisca$group_mac(a_port->group, a_port->gmac);

	if ( !(1 & (l_status = mh$lan_join(a_port, a_port->gmac))) )
		return	l_status;

	if ( !(1 & mh$hex2bin($ASCPTR(&a_cfg->auth), $ASCLEN(&a_cfg->auth), a_port->auth, MH$K_SZ_AUTH)) )
		return	$MHMSG(MH$_IVPARAM, $ASCPTR(&a_cfg->auth), "AUTHORIZE", "16 hexadecimal digits are expected");

	/*
	**  NODENAME of CC: ASCIC with a length octet of 6, blank-padded, the
	**  same octets in every message (PEDRIVER drops a channel whose node
	**  name changes - INCOMP_CHNL).
	*/
	a_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memset(a_port->ccname + 1, ' ', NISCA$K_SZ_NODENAME);
	memcpy(a_port->ccname + 1, $ASCPTR(&a_cfg->node), $ASCLEN(&a_cfg->node));

	l_len = (unsigned) $MIN(strlen(a_port->ifname), NISCA$K_SZ_DEVNAME - 1);
	memset(a_port->devname, 0, sizeof(a_port->devname));
	a_port->devname[0] = (uint8_t) l_len;
	memcpy(a_port->devname + 1, a_port->ifname, l_len);

	/*
	**  BUS_BUFFER_SIZE = the data field of the LAN driver less 72:
	**  1498 - 72 = 1426 on an Ethernet, 8926 with jumbo frames of an MTU
	**  of 9000.  PEDRIVER takes the smaller of its own and ours
	**  (CC.NEGOTIATE_BUFFER_SIZE) and probes the path for it; a node on
	**  an adapter without jumbo frames goes on at 1426.
	*/
	l_mtu = (unsigned) $MIN(a_port->mtu, MH$K_LAN_MAXMTU);
	a_port->bufsz = (uint16_t) (l_mtu - NISCA$K_SZ_PAD - MH$K_LAN_OVHD);

	if ( l_mtu > MH$K_LAN_ETHMTU )
		$MHMSG(MH$_JUMBO, a_port->ifname, a_port->mtu, (a_port->mtu > MH$K_LAN_MAXMTU) ? ", held to 9000" : "", a_port->bufsz);

	if ( !(1 & (l_status = mh$vmstime(&a_port->incarn))) )
		return	$MHMSG(MH$_SYSERR, "clock_gettime(CLOCK_REALTIME)", errno, strerror(errno));

	s_seed = (unsigned) a_port->incarn ^ (unsigned) (a_port->incarn >> 32);

	a_port->next_hello = a_port->tick + (uint64_t) a_cfg->listen * MH$K_TICKS_SEC;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Build the DX header of a frame from this port.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_dxdst		Logical destination: the group multicast address, or the
**			node address AA-00-04-00-lo-hi of the peer (NOT its MAC)
**	a_dx		Output: NISCA$K_SZ_DX octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$dx_build	(
	const	MH$PORT *	a_port,
	const	uint8_t *	a_dxdst,
		uint8_t *	a_dx
			)
{
	memcpy(a_dx + NISCA$K_DX_DST_LO, a_dxdst, NISCA$K_SZ_MAC);
	nisca$put16(a_dx + NISCA$K_DX_GROUP, a_port->group);
	nisca$node_mac(a_port->sysid, a_dx + NISCA$K_DX_SRC_LO);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Build a CC message of version 1.3 from this port.  A HELLO carries no
**	password and no channel sequence number; every other message carries
**	the password hash and sets the AUTHORIZE bit (a message without it is
**	a BAD_ECO event at PEDRIVER).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_type		NISCA$K_CC_*
**	a_chseq		LCL_CHAN_SEQ_NUM: of the channel the message goes on,
**			0 in a multicast one
**	a_cc		Output area
**	a_ccsz		Its size, at least NISCA$K_CC_LEN_V13
**	a_len		Output: the length of the message
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the area is too small.
**--
*/
int	mh$cc_build	(
		MH$PORT *	a_port,
		unsigned	a_type,
		uint16_t	a_chseq,
		uint8_t *	a_cc,
		unsigned	a_ccsz,
		unsigned *	a_len
			)
{
uint64_t	l_now = 0;

	if ( a_ccsz < NISCA$K_CC_LEN_V13 )
		return	STS$K_ERROR;

	memset(a_cc, 0, NISCA$K_CC_LEN_V13);

	a_cc[NISCA$K_CC_MSG] = (uint8_t) (NISCA$M_CC_CCFLG | NISCA$M_CC_OK_XMT | (a_type & NISCA$M_CC_TYPE));

	if ( a_type != NISCA$K_CC_HELLO )
		{
		a_cc[NISCA$K_CC_MSG] |= NISCA$M_CC_AUTHORIZE;
		memcpy(a_cc + NISCA$K_CC_AUTHORIZE, a_port->auth, MH$K_SZ_AUTH);
		}

	nisca$put32(a_cc + NISCA$K_CC_MAINT, MH$K_CC_MAINT);
	a_cc[NISCA$K_CC_MINOR] = MH$K_CC_MINOR;
	a_cc[NISCA$K_CC_MAJOR] = NISCA$K_MAJOR;
	memcpy(a_cc + NISCA$K_CC_NODENAME, a_port->ccname, sizeof(a_port->ccname));
	nisca$put32(a_cc + NISCA$K_CC_PORT_FCN, MH$K_CC_PORT_FCN);
	nisca$put32(a_cc + NISCA$K_CC_STA_INFO, MH$K_CC_STA_INFO);
	a_cc[NISCA$K_CC_PIPE_QUOTA] = MH$K_PIPE_QUOTA;
	nisca$put16(a_cc + NISCA$K_CC_LCL_CHSEQ, a_chseq);
	nisca$put16(a_cc + NISCA$K_CC_BUS_BUFSZ, a_port->bufsz);

	mh$vmstime(&l_now);
	nisca$put64(a_cc + NISCA$K_CC_XMT_TIME, l_now);

	memcpy(a_cc + NISCA$K_CC_DEVNAME, a_port->devname, NISCA$K_SZ_DEVNAME);
	memcpy(a_cc + NISCA$K_CC_LAN_HWADDR, a_port->mac, NISCA$K_SZ_MAC);
	a_cc[NISCA$K_CC_NODE_HWTYPE] = MH$K_CC_HWTYPE;

	*a_len = NISCA$K_CC_LEN_V13;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a CC message to the group multicast: a HELLO, or the BYE of a
**	server that stops.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_type		NISCA$K_CC_HELLO or NISCA$K_CC_BYE
**
**  RETURN VALUE:
**	The condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$cc_mcast	(
		MH$PORT *	a_port,
		unsigned	a_type
			)
{
uint8_t		l_data [NISCA$K_SZ_DX + NISCA$K_CC_LEN_V15];
unsigned	l_cclen;

	mh$dx_build(a_port, a_port->gmac, l_data);
	mh$cc_build(a_port, a_type, 0, l_data + NISCA$K_SZ_DX, sizeof(l_data) - NISCA$K_SZ_DX, &l_cclen);

	return	mh$lan_send(a_port, a_port->gmac, l_data, NISCA$K_SZ_DX + l_cclen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a CC message on the channel of a node: the Ethernet destination
**	is its adapter, the DX destination its node address.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_type		NISCA$K_CC_CCSTART, _VERF or _VACK
**
**  RETURN VALUE:
**	The condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$ch_send	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		unsigned	a_type
			)
{
uint8_t		l_data [NISCA$K_SZ_DX + NISCA$K_CC_LEN_V15], l_dxdst [NISCA$K_SZ_MAC];
unsigned	l_cclen;

	nisca$node_mac(a_node->sysid, l_dxdst);
	mh$dx_build(a_port, l_dxdst, l_data);
	mh$cc_build(a_port, a_type, a_node->lcl_chseq, l_data + NISCA$K_SZ_DX, sizeof(l_data) - NISCA$K_SZ_DX, &l_cclen);

	return	mh$lan_send(a_port, a_node->mac, l_data, NISCA$K_SZ_DX + l_cclen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	UPDATE_CHANNEL_INFO: take what the peer tells about its end of the
**	channel out of its CCSTART, VERF or VACK - the window of the circuit is held to
**	its PIPE_QUOTA, a block to its BUS_BUFFER_SIZE, our TR packets carry
**	its LCL_CHAN_SEQ_NUM.
**
**  FORMAL PARAMETERS:
**
**	a_node		The node
**	a_cc		The CC message
**	a_cclen		Its length, at least NISCA$K_CC_LEN_V11
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$ch_update	(
		MH$NODE *	a_node,
	const	uint8_t *	a_cc,
		unsigned	a_cclen
			)
{
	a_node->minor		= a_cc[NISCA$K_CC_MINOR];
	a_node->cclen		= (uint8_t) $MIN(a_cclen, 0xFF);
	a_node->pipe_quota	= a_cc[NISCA$K_CC_PIPE_QUOTA];
	a_node->p_srv		= nisca$get16(a_cc + NISCA$K_CC_P_SRV);
	a_node->r_srv		= nisca$get16(a_cc + NISCA$K_CC_R_SRV);

	if ( a_cclen >= NISCA$K_CC_LCL_CHSEQ + 2 )
		a_node->rmt_chseq = nisca$get16(a_cc + NISCA$K_CC_LCL_CHSEQ);

	if ( a_cclen >= NISCA$K_CC_BUS_BUFSZ + 2 )
		a_node->bufsz = nisca$get16(a_cc + NISCA$K_CC_BUS_BUFSZ);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	SHUTDOWN_CH: close the channel of a node.  The next incarnation of the
**	channel gets the next LCL_CHAN_SEQ_NUM, 0 is skipped: a
**	TR packet of the old one is told by it and dropped.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_why		Why, for the operator: an open channel that closes is
**			reported by CHCLOSE, a handshake that fails under
**			/TRACE only
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$ch_close	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	char *		a_why
			)
{
	/*
	**  The circuit goes with the last channel of the node, not before
	*/
	if ( !mh$cc_vcopen(a_port, $MHOWNER(a_node), a_node) )
		mh$ppd_close(a_port, $MHOWNER(a_node), a_why);

	if ( a_node->state == MH$K_CH_OPEN )
		$MHMSG(MH$_CHCLOSE, a_node->name, a_node->sysid, a_why);
	else if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "Channel to !AZ (!UL) closed in !AZ: !AZ", a_node->name, a_node->sysid, s_state[a_node->state], a_why);

	a_node->state	= MH$K_CH_CLOSED;
	a_node->due	= 0;
	a_node->boot	= 0;

	if ( !++a_node->lcl_chseq )
		a_node->lcl_chseq = 1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Restart the channel of a node whose circuit has failed: close it and
**	send CCSTART at once.  PEDRIVER of the node takes a CCSTART on its
**	open channel for a restart of the path (A63) and starts it over.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_why		Why, for the operator
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
int	mh$cc_restart	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	char *		a_why
			)
{
	s_mh$ch_close(a_port, a_node, a_why);

	if ( a_port->passive || !a_port->announcing )
		return	STS$K_SUCCESS;

	a_node->state	= MH$K_CH_CCSTART_SENT;
	a_node->due	= a_port->tick + MH$K_HS_TMO;

	return	s_mh$ch_send(a_port, a_node, NISCA$K_CC_CCSTART);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	OPEN_PATH: the handshake is over, the channel is open.
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
static	int	s_mh$ch_open	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
	a_node->state	= MH$K_CH_OPEN;
	a_node->due	= 0;
	a_node->opens++;

	$MHMSG(MH$_CHOPEN, a_node->name, a_node->sysid, a_node->minor, a_node->pipe_quota, a_node->bufsz, a_node->p_srv, a_node->r_srv);

	/*
	**  A boot driver waits for the START of its server
	*/
	if ( a_node->boot )
		return	mh$ppd_start_path(a_port, $MHOWNER(a_node));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Run one event through the state machine of the channel of a node.
**	Nothing runs under /PASSIVE or before the listen period is over: the
**	server must not transmit then.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_event		MH$K_CE_*
**	a_cc		The CC message of the event, NULL for a timer
**	a_cclen		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$ch_event	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		unsigned	a_event,
	const	uint8_t *	a_cc,
		unsigned	a_cclen
			)
{
int	l_old = a_node->state, l_status = STS$K_SUCCESS;
unsigned	l_action;

	if ( a_port->passive || !a_port->announcing )
		return	STS$K_SUCCESS;

	if ( (a_event >= MH$K_CE_NEVENT) || ((unsigned) l_old >= MH$K_CH_NSTATE) )
		return	STS$K_SUCCESS;

	if ( MH$K_CA_NONE == (l_action = s_cc_table[a_event][l_old]) )
		return	STS$K_SUCCESS;

	switch ( l_action )
		{
		case	MH$K_CA_DELAY:
			/*
			**  PEM$CC_RAND_WAIT: not all the nodes answer a HELLO at
			**  the same moment
			*/
			a_node->state	= MH$K_CH_HELLO_DLY;
			a_node->due	= a_port->tick + MH$K_DLY_MIN + (uint64_t) (rand_r(&s_seed) % (MH$K_DLY_MAX - MH$K_DLY_MIN + 1));
			break;

		case	MH$K_CA_CCSTART:
			a_node->state	= MH$K_CH_CCSTART_SENT;
			a_node->due	= a_port->tick + MH$K_HS_TMO;
			l_status	= s_mh$ch_send(a_port, a_node, NISCA$K_CC_CCSTART);
			break;

		case	MH$K_CA_RESTART:
			s_mh$ch_close(a_port, a_node, s_why[a_event]);

			/* FALLTHRU */

		case	MH$K_CA_VERF:
			s_mh$ch_update(a_node, a_cc, a_cclen);

			a_node->boot	= 0;			/* A CCSTART: PEDRIVER, not a boot driver	*/
			a_node->state	= MH$K_CH_CCSTART_REC;
			a_node->due	= a_port->tick + MH$K_HS_TMO;
			l_status	= s_mh$ch_send(a_port, a_node, NISCA$K_CC_VERF);
			break;

		case	MH$K_CA_OPEN_VACK:
			s_mh$ch_update(a_node, a_cc, a_cclen);
			l_status = s_mh$ch_send(a_port, a_node, NISCA$K_CC_VACK);
			s_mh$ch_open(a_port, a_node);
			break;

		case	MH$K_CA_OPEN:
			s_mh$ch_update(a_node, a_cc, a_cclen);
			s_mh$ch_open(a_port, a_node);
			break;

		case	MH$K_CA_VACK:
			s_mh$ch_update(a_node, a_cc, a_cclen);
			l_status = s_mh$ch_send(a_port, a_node, NISCA$K_CC_VACK);
			break;

		case	MH$K_CA_UPDATE:
			s_mh$ch_update(a_node, a_cc, a_cclen);
			break;

		case	MH$K_CA_CLOSE:
			s_mh$ch_close(a_port, a_node, s_why[a_event] ? s_why[a_event] : s_event[a_event]);
			break;

		case	MH$K_CA_SOLICIT:
			/*
			**  Our CCSTART, if any, is forgotten: the boot driver
			**  takes nothing but a VERF from a new address
			*/
			s_mh$ch_update(a_node, a_cc, a_cclen);

			a_node->boot	= 1;
			a_node->state	= MH$K_CH_SOLICIT_DLY;
			a_node->due	= a_port->tick + 1 + (uint64_t) (rand_r(&s_seed) % MH$K_SOL_DLY_MAX);
			break;

		case	MH$K_CA_SOL_RESTART:
			s_mh$ch_close(a_port, a_node, s_why[a_event]);

			/* FALLTHRU */

		case	MH$K_CA_SOL_VERF:
			/*
			**  BUS_BUFFER_SIZE and the rest are of the SOLICIT_SRV;
			**  its LCL_CHAN_SEQ_NUM is 0, the VACK brings the real one
			*/
			if ( a_cc )
				s_mh$ch_update(a_node, a_cc, a_cclen);

			a_node->boot	= 1;
			a_node->state	= MH$K_CH_CCSTART_REC;
			a_node->due	= a_port->tick + MH$K_HS_TMO;
			l_status	= s_mh$ch_send(a_port, a_node, NISCA$K_CC_VERF);
			break;
		}

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "Channel to !AZ (!UL): !AZ in !AZ -> !AZ, chseq !UL/!UL", a_node->name, a_node->sysid,
			s_event[a_event], s_state[l_old], s_state[a_node->state], a_node->lcl_chseq, a_node->rmt_chseq);

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The 20 ms clock of the CC layer: the end of the listen period, the
**	HELLO, the timers of the channels.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  IMPLICIT INPUTS/OUTPUTS:
**	TICK, NEXT_HELLO, ANNOUNCING, the table of the nodes.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$cc_tick	(
		MH$PORT *	a_port
			)
{
MH$NODE	*l_node;
char	l_name [NISCA$K_SZ_NODENAME + 1];

	a_port->tick++;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		l_node = &a_port->nodes[i];

		if ( (l_node->state == MH$K_CH_OPEN) && (a_port->tick - l_node->last >= (l_node->boot ? MH$K_BOOT_LSTN_TMO : MH$K_LISTEN_TMO)) )
			s_mh$ch_event(a_port, l_node, MH$K_CE_LISTN_TMO, NULL, 0);
		else if ( l_node->due && (a_port->tick >= l_node->due) )
			s_mh$ch_event(a_port, l_node, MH$K_CE_HS_TMO, NULL, 0);

		/*
		**  The circuit of a node runs while any of its channels is open
		*/
		if ( !l_node->owner && mh$cc_vcopen(a_port, l_node, NULL) )
			{
			mh$ppd_tick(a_port, l_node);
			mh$tr_tick(a_port, l_node);
			}
		}

	if ( a_port->tick < a_port->next_hello )
		return	STS$K_SUCCESS;

	if ( !a_port->announcing )
		{
		a_port->announcing = 1;

		memcpy(l_name, a_port->ccname + 1, NISCA$K_SZ_NODENAME);
		l_name[s_mh$cc_namelen(a_port)] = '\0';

		$MHMSG(MH$_ANNOUNCE, l_name, a_port->passive ? "listens only (/PASSIVE)" : "announced", a_port->ifname, a_port->nnode, a_port->group);

		/*
		**  The nodes heard during the listen period get our CCSTART as
		**  if their HELLO had come in now
		*/
		for ( unsigned i = 0; i < a_port->nnode; i++ )
			{
			if ( a_port->tick - a_port->nodes[i].last < MH$K_LISTEN_TMO )
				s_mh$ch_event(a_port, &a_port->nodes[i], MH$K_CE_HELLO, NULL, 0);
			}
		}

	if ( a_port->passive )
		{
		a_port->next_hello = (uint64_t) -1;
		return	STS$K_SUCCESS;
		}

	a_port->next_hello = a_port->tick + MH$K_HELLO_MIN + (uint64_t) (rand_r(&s_seed) % (MH$K_HELLO_MAX - MH$K_HELLO_MIN + 1));

	return	s_mh$cc_mcast(a_port, NISCA$K_CC_HELLO);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The server stops: HOSTSHUT on the open paths, the channels are
**	closed, and a server that has
**	announced itself says BYE to the group, so that the VMS nodes drop
**	their channels at once instead of in 8 s (PEDRIVER answers BYE on an
**	open channel by a HOSTSHUT to SCS).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
int	mh$cc_shutdown	(
		MH$PORT *	a_port
			)
{
	mh$ppd_shutdown(a_port);

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		if ( a_port->nodes[i].state != MH$K_CH_CLOSED )
			s_mh$ch_close(a_port, &a_port->nodes[i], "the server is stopping");
		}

	if ( a_port->passive || !a_port->announcing )
		return	STS$K_SUCCESS;

	return	s_mh$cc_mcast(a_port, NISCA$K_CC_BYE);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Has a node an open channel: one of the entries of its adapters,
**	the owner's or another, but the one given.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_owner		The owner of the node
**	a_but		An entry not to count, NULL - none
**
**  RETURN VALUE:
**	1 - it has; 0 - not.
**--
*/
int	mh$cc_vcopen	(
	const	MH$PORT *	a_port,
	const	MH$NODE *	a_owner,
	const	MH$NODE *	a_but
			)
{
	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		const	MH$NODE	*l_ch = &a_port->nodes[i];

		if ( (l_ch != a_but) && ($MHOWNER(l_ch) == a_owner) && (l_ch->state == MH$K_CH_OPEN) )
			return	1;
		}

	return	0;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The largest buffer of the open channels of a node: what a chunk of a
**	block transfer may carry.  A channel that has not told its buffer
**	(CC V1.1) is taken as an Ethernet.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_owner		The owner of the node
**
**  RETURN VALUE:
**	BUS_BUFFER_SIZE; that of the owner when no channel is open.
**--
*/
unsigned mh$cc_bufsz	(
	const	MH$PORT *	a_port,
	const	MH$NODE *	a_owner
			)
{
unsigned	l_best = 0, l_sz;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		const	MH$NODE	*l_ch = &a_port->nodes[i];

		if ( ($MHOWNER(l_ch) != a_owner) || (l_ch->state != MH$K_CH_OPEN) )
			continue;

		l_sz = l_ch->bufsz ? l_ch->bufsz : MH$K_LAN_ETHMTU - NISCA$K_SZ_PAD - MH$K_LAN_OVHD;

		if ( l_sz > l_best )
			l_best = l_sz;
		}

	return	l_best ? l_best : a_owner->bufsz;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The channel a packet of the circuit of a node goes by: of the open
**	channels whose buffer takes it, the one of the largest buffer; the
**	channel the circuit used last, while it is as good as any, so that
**	the packets keep to one adapter and their order.  With no channel
**	of the node open, the owner's: the packet goes as it always did, and
**	the circuit sends it again if it is lost.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_owner		The owner of the node; output: XCH
**	a_len		DX header onwards, octets
**
**  RETURN VALUE:
**	The channel; NULL - channels are open, but the packet is larger than
**	the buffer of every one of them.
**--
*/
MH$NODE	*mh$cc_chan	(
		MH$PORT *	a_port,
		MH$NODE *	a_owner,
		unsigned	a_len
			)
{
MH$NODE		*l_best = NULL;
unsigned	l_sz, l_bestsz = 0, l_open = 0;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_ch = &a_port->nodes[i];

		if ( ($MHOWNER(l_ch) != a_owner) || (l_ch->state != MH$K_CH_OPEN) )
			continue;

		l_open++;
		l_sz = l_ch->bufsz ? l_ch->bufsz : MH$K_LAN_ETHMTU - NISCA$K_SZ_PAD - MH$K_LAN_OVHD;

		if ( a_len > l_sz + MH$K_LAN_OVHD )
			continue;

		if ( !l_best || (l_sz > l_bestsz) || ((l_sz == l_bestsz) && (l_ch == a_owner->xch)) )
			{
			l_best	 = l_ch;
			l_bestsz = l_sz;
			}
		}

	if ( !l_best )
		return	l_open ? NULL : a_owner;

	if ( (l_best != a_owner->xch) && a_owner->xch && a_port->trace )
		$LOG_FAO(STS$K_INFO, "Circuit to !AZ (!UL) goes by another adapter now, buffer !UL", a_owner->name, a_owner->sysid, l_bestsz);

	a_owner->xch = l_best;

	return	l_best;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Tie an entry to the node it is an adapter of: another entry of the
**	same system ID and name that owns itself is its owner.  The entries
**	that had this one for their owner are let go first: it is a new node,
**	or the same one by another name.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The entry, its system ID and name set; output: OWNER
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$cc_link	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
	a_node->owner = NULL;
	a_node->xch = NULL;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_ch = &a_port->nodes[i];

		if ( l_ch->owner == a_node )
			l_ch->owner = NULL;

		if ( l_ch->xch == a_node )
			l_ch->xch = NULL;
		}

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_ch = &a_port->nodes[i];

		if ( (l_ch != a_node) && !l_ch->owner && (l_ch->sysid == a_node->sysid) && !strcmp(l_ch->name, a_node->name) )
			{
			a_node->owner = l_ch;
			break;
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Find the node of a source address.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The source MAC of a frame
**
**  RETURN VALUE:
**	The node, NULL - not known.
**--
*/
static	MH$NODE	*s_mh$cc_find	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac
			)
{
	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		if ( !memcmp(a_port->nodes[i].mac, a_mac, NISCA$K_SZ_MAC) )
			return	&a_port->nodes[i];
		}

	return	NULL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Remember the sender of a CC message, and check it against the identity
**	of this server.  A new node takes a free entry, or the one of the
**	node unheard of the longest when it has no channel.  A node that
**	comes with another SCSNODE or system ID than before has its channel
**	closed (INCOMP_CHNL of PEDRIVER) and is checked again.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The source MAC of the frame
**	a_sysid		The DX source: low word of the SCSSYSTEMID
**	a_cc		The CC message
**	a_cclen		Its length
**	a_node		Output: the node, NULL - the table is full
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_IDCLASH or MH$_NAMECLASH (severity F) - the server
**	must stop.
**--
*/
static	int	s_mh$cc_node	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac,
		uint16_t	a_sysid,
	const	uint8_t *	a_cc,
		unsigned	a_cclen,
		MH$NODE **	a_node
			)
{
MH$NODE *	l_node;
char		l_macstr [3 * NISCA$K_SZ_MAC], l_name [NISCA$K_SZ_NODENAME + 1];
unsigned	l_nlen;

	*a_node = NULL;

	/*
	**  ASCIC, trailing blanks dropped
	*/
	l_nlen = $MIN(a_cc[NISCA$K_CC_NODENAME], NISCA$K_SZ_NODENAME);
	memcpy(l_name, a_cc + NISCA$K_CC_NODENAME + 1, l_nlen);

	while ( l_nlen && ((l_name[l_nlen - 1] == ' ') || !l_name[l_nlen - 1]) )
		l_nlen--;

	l_name[l_nlen] = '\0';

	mh$mac2str(a_mac, l_macstr, sizeof(l_macstr));

	if ( (l_node = s_mh$cc_find(a_port, a_mac)) )
		{
		if ( (l_node->sysid == a_sysid) && !strcmp(l_node->name, l_name) )
			goto	l_known;

		if ( l_node->state != MH$K_CH_CLOSED )
			s_mh$ch_close(a_port, l_node, "the node has changed its name or system ID");
		}
	else	{
		if ( a_port->nnode < MH$K_MAXNODE )
			l_node = &a_port->nodes[a_port->nnode++];
		else	{
			for ( unsigned i = 0; i < a_port->nnode; i++ )
				{
				/*
				**  Not the owner of a node whose circuit runs over
				**  another adapter of it
				*/
				if ( (a_port->nodes[i].state == MH$K_CH_CLOSED) && !mh$cc_vcopen(a_port, &a_port->nodes[i], NULL)
					&& (!l_node || (a_port->nodes[i].last < l_node->last)) )
					l_node = &a_port->nodes[i];
				}

			if ( !l_node || (a_port->tick - l_node->last < MH$K_LISTEN_TMO) )
				{
				$MHMSG(MH$_MANYNODES, MH$K_MAXNODE, l_macstr);
				return	STS$K_SUCCESS;
				}
			}

		memset(l_node, 0, sizeof(*l_node));
		memcpy(l_node->mac, a_mac, NISCA$K_SZ_MAC);
		l_node->lcl_chseq = 1;
		}

	l_node->sysid	= a_sysid;
	l_node->minor	= a_cc[NISCA$K_CC_MINOR];
	l_node->cclen	= (uint8_t) $MIN(a_cclen, 0xFF);
	l_node->last	= a_port->tick;		/* Before the clash checks: a node is aged from now	*/
	memcpy(l_node->name, l_name, l_nlen + 1);

	/*
	**  Another adapter of a node we know: its channel serves the circuit
	**  of that node
	*/
	s_mh$cc_link(a_port, l_node);

	$MHMSG(MH$_NEWNODE, l_node->name, l_node->sysid, l_macstr, l_node->minor, a_cclen);

	if ( l_node->sysid == a_port->sysid )
		return	$MHMSG(MH$_IDCLASH, l_node->name, l_macstr, a_port->sysid);

	if ( (l_nlen == (unsigned) s_mh$cc_namelen(a_port)) && !memcmp(l_node->name, a_port->ccname + 1, l_nlen) )
		return	$MHMSG(MH$_NAMECLASH, l_node->name, l_macstr, l_node->sysid);

l_known:
	l_node->last = a_port->tick;

	if ( (a_cc[NISCA$K_CC_MSG] & NISCA$M_CC_TYPE) == NISCA$K_CC_HELLO )
		l_node->hellos++;

	*a_node = l_node;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Length of our SCSNODE without its trailing blanks.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	The length, 1 - 6.
**--
*/
static	int	s_mh$cc_namelen	(
	const	MH$PORT *	a_port
			)
{
int	l_len = NISCA$K_SZ_NODENAME;

	while ( (l_len > 1) && (a_port->ccname[l_len] == ' ') )
		l_len--;

	return	l_len;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in a SOLICIT_SRV, the boot driver of a satellite looking for the
**	server of its system disk: SERVICE is the name of the disk as VMS
**	gives it, _$42$DKA100:.  A disk we serve, writable, drives the
**	channel; another is not answered at all - another server may have
**	it (PEDRIVER: CC_DISK_NOT_SERVED).  A node is told of once a boot,
**	not at every SOLICIT_SRV: it asks every 3 s, and again for each part
**	it loads; a SOLICIT_SRV after 60 s of none, or with another outcome,
**	is told of again.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cc		The CC message, its password checked
**	a_cclen		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$LAN_SEND.
**--
*/
static	int	s_mh$cc_solicit	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_cc,
		unsigned	a_cclen
			)
{
const	uint8_t	*l_svc = a_cc + NISCA$K_CC_SERVICE;
unsigned	l_unit = 0, l_svclen;
int		l_status, l_served;

	if ( a_cclen < NISCA$K_CC_LEN_V12 )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_svclen = $MIN(l_svc[0], NISCA$K_SZ_SERVICE - 1);
	l_status = mh$mscp_check_service(a_port, l_svc, &l_unit);
	l_served = (1 & l_status) ? 1 : (l_status == STS$K_WARN) ? -2 : -1;

	if ( (a_node->svclog != l_served) || (a_port->tick - a_node->sollast >= MH$K_SOL_QUIET) )
		{
		a_node->svclog = l_served;
		a_node->sollogs++;

		$MHMSG(MH$_SOLICIT, a_node->name, a_node->sysid, l_svclen, l_svc + 1, (l_served > 0) ? "served"
			: (l_served == -2) ? "not served: the unit is read only" : "not served here");
		}

	a_node->sollast = a_port->tick;

	if ( l_served < 0 )
		return	STS$K_SUCCESS;

	/*
	**  A node that boots has no runtime channel left: those of its other
	**  adapters are closed - an old one of jumbo frames, open after a crash
	**  without BYE, would carry the loader's blocks in a size it cannot take
	*/
	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		MH$NODE	*l_ch = &a_port->nodes[i];

		if ( (l_ch != a_node) && ($MHOWNER(l_ch) == $MHOWNER(a_node)) && (l_ch->state != MH$K_CH_CLOSED) )
			s_mh$ch_close(a_port, l_ch, "the node boots by another adapter");
		}

	return	s_mh$ch_event(a_port, a_node, MH$K_CE_SOLICIT, a_cc, a_cclen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in one received frame of the ethertype 60-07: the length word
**	and the DX header are checked, a frame of another group or addressed
**	to another node is dropped, as PEDRIVER does (BUS$W_WRONG_PORT);
**	a CC message updates the table of the nodes and, when it passes the
**	password check, drives the channel of its node; a TR packet keeps the
**	channel alive and is otherwise only counted and, under /TRACE, logged.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame, Ethernet header included
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; a condition value of severity F that has been signalled
**	when the server must stop.
**--
*/
int	mh$cc_input	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
const uint8_t	*l_dx, *l_cc, *l_src = a_frame + NISCA$K_SZ_MAC;
uint8_t		l_self [NISCA$K_SZ_MAC];
unsigned	l_dlen, l_cclen, l_type;
uint16_t	l_sysid;
int		l_mcast, l_status;
MH$NODE		*l_node;
char		l_macstr [3 * NISCA$K_SZ_MAC];

	if ( a_len < NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + 1 )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_dlen	= nisca$get16(a_frame + NISCA$K_SZ_ETH);
	l_dx	= a_frame + NISCA$K_SZ_LANHDR;

	if ( (l_dlen <= NISCA$K_SZ_DX) || (l_dlen > a_len - NISCA$K_SZ_LANHDR) )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	if ( nisca$get16(l_dx + NISCA$K_DX_GROUP) != a_port->group )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	nisca$node_mac(a_port->sysid, l_self);

	l_mcast = !memcmp(l_dx + NISCA$K_DX_DST_LO, a_port->gmac, NISCA$K_SZ_MAC);

	if ( !l_mcast && memcmp(l_dx + NISCA$K_DX_DST_LO, l_self, NISCA$K_SZ_MAC) )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_sysid = nisca$get16(l_dx + NISCA$K_DX_SRC_HI);

	if ( !(l_dx[NISCA$K_SZ_DX] & NISCA$M_CC_CCFLG) )
		{
		a_port->rx_tr++;

		/*
		**  A packet of an open channel only (TRANS, A82); any packet
		**  keeps the listen timer of the channel at bay
		*/
		if ( l_mcast || !(l_node = s_mh$cc_find(a_port, l_src)) || (l_node->sysid != l_sysid) || (l_node->state != MH$K_CH_OPEN) )
			{
			a_port->rx_drop++;
			return	STS$K_SUCCESS;
			}

		l_node->last = a_port->tick;

		/*
		**  Of the circuit of the node, whichever adapter of it it came by
		*/
		return	mh$tr_input(a_port, $MHOWNER(l_node), l_node, l_dx + NISCA$K_SZ_DX, l_dlen - NISCA$K_SZ_DX);
		}

	a_port->rx_cc++;

	l_cc	= l_dx + NISCA$K_SZ_DX;
	l_cclen	= l_dlen - NISCA$K_SZ_DX;
	l_type	= l_cc[NISCA$K_CC_MSG] & NISCA$M_CC_TYPE;

	/*
	**  A channel is made of a message of V1.1 length or more only, of
	**  the major version 1
	*/
	if ( (l_cclen < NISCA$K_CC_LEN_V11) || (l_cc[NISCA$K_CC_MAJOR] != NISCA$K_MAJOR) )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "CC !AZ (!XB) from !UL, !AZ, !UL octets", (l_type < $ARRSZ(s_cctype)) ? s_cctype[l_type] : "?",
			l_cc[NISCA$K_CC_MSG], l_sysid, l_mcast ? "multicast" : "to us", l_cclen);

	if ( !(1 & (l_status = s_mh$cc_node(a_port, l_src, l_sysid, l_cc, l_cclen, &l_node))) || !l_node )
		return	l_status;

	/*
	**  HELLO is not checked, by PEDRIVER neither; everything else must
	**  carry the password of the group
	*/
	if ( (l_type != NISCA$K_CC_HELLO) && (!(l_cc[NISCA$K_CC_MSG] & NISCA$M_CC_AUTHORIZE)
		|| memcmp(l_cc + NISCA$K_CC_AUTHORIZE, a_port->auth, MH$K_SZ_AUTH)) )
		{
		a_port->rx_auth++;

		if ( !l_node->authfail )
			{
			l_node->authfail = 1;
			mh$mac2str(l_src, l_macstr, sizeof(l_macstr));
			$MHMSG(MH$_AUTHFAIL, l_node->name, l_node->sysid, l_macstr);
			}

		return	STS$K_SUCCESS;
		}

	if ( l_type == NISCA$K_CC_SOLICIT_SRV )
		return	s_mh$cc_solicit(a_port, l_node, l_cc, l_cclen);

	/*
	**  A CCSTART, VERF or VACK goes to a node address, never to the group
	*/
	if ( l_mcast && (l_type > NISCA$K_CC_BYE) )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	if ( l_type > NISCA$K_CC_VACK )
		return	STS$K_SUCCESS;

	s_mh$ch_event(a_port, l_node, l_type, l_cc, l_cclen);

	return	STS$K_SUCCESS;
}
