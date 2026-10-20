#define	__MODULE__	"MHSCS"
#define	__IDENT__	"X01-10"
#define	__REV__		"1.10.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_scs.c
**
**  ABSTRACT:	SCS, the session layer: the connections (CDT) over an open
**		path, their credits, and the directory service SCS$DIRECTORY.
**
**  DESCRIPTION: The server only accepts connections, it never asks for one
**		(the acceptor half):
**
**		  CON_REQ  to a listener -> CON_RSP NORMAL, CON_REC; then our
**			   ACCP_REQ -> ACCP_SENT; the peer's ACCP_RSP -> OPEN.
**			   To anything else -> CON_RSP STNOMAT (no listener):
**			   VMS$LOOPBACK of the watchdog gets this answer too.
**		  DISC_REQ -> DISC_RSP, then our own DISC_REQ; the CDT is free
**			   on its DISC_RSP.
**		  CR_REQ   -> CR_RSP with the credit taken.
**
**		Listeners: SCS$DIRECTORY (credit 8, as VMS) and, when units are
**		served, MSCP$DISK (credit 16).  A response is built in a copy of
**		the request: MTYPE + 1, CREDIT 0 (CR_RSP: the credit), CONIDs
**		swapped (SCS$SEND_RSP); STATUS is never 0 - that breaks the
**		circuit at the peer (§9.6).  Of the requests of the server, one
**		at a time is out on a path (§9.3): the next goes when the
**		response of the last comes.
**
**		Credits (§10): an APPL_MSG we send spends one of the credits
**		the peer gave (CREDIT of every message it sends adds to them);
**		without a credit it waits in the queue of the CDT.  An APPL_MSG
**		we take frees a buffer of ours: its credit goes back on our
**		next message to the peer.
**
**		SCS$DIRECTORY answers in place of the lookup, 36 octets: status
**		at +0, the name echoed at +4, PRINFO at +20.  MSCP$DISK is present (16 blanks) when units are served;
**		MSCP$TAPE and VMS$VAXcluster are "NOT PRESENT HERE", so that the
**		process poller of the VMS node stops asking for them (§6.5);
**		anything else is SS$_NOSUCHOBJ.
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

static	const char *	s_scstype [] = { "CON_REQ", "CON_RSP", "ACCP_REQ", "ACCP_RSP", "REJ_REQ", "REJ_RSP",
					 "DISC_REQ", "DISC_RSP", "CR_REQ", "CR_RSP", "APPL_MSG", "APPL_DG" };

#define	MH$K_SZ_PROCNAM		16
#define	MH$K_DIR_MSGLEN		36		/* A lookup and its answer			*/
#define	MH$K_DIR_NOSUCHOBJ	0x20A4		/* SS$_NOSUCHOBJ				*/
#define	MH$K_SCS_STNORS		0x12		/* No resources					*/
#define	MH$K_SCS_LENGTH_REQ	66		/* CON_REQ, ACCP_REQ				*/
#define	MH$K_SCS_LENGTH_DISC	18		/* DISC_REQ					*/

/*
**  Offsets in the PPD of an SCS message we build: W LENGTH, W PPD MTYPE,
**  the SCS header, the body
*/
#define	MH$K_SM_HDR		(NISCA$K_PPD_MTYPE + 2)
#define	MH$K_SM_BODY		(MH$K_SM_HDR + NISCA$K_SCS_BODY)

static	const char	s_dirname [MH$K_SZ_PROCNAM + 1] = "SCS$DIRECTORY   ";
static	const char	s_mscpname [MH$K_SZ_PROCNAM + 1] = "MSCP$DISK       ";
static	const char	s_notpresent [MH$K_SZ_PROCNAM + 1] = "NOT PRESENT HERE";

static	int	s_mh$scs_kick	(MH$PORT *a_port, MH$NODE *a_node);



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Copy a blank-padded 16-octet SYSAP name into an ASCIZ string.
**
**  FORMAL PARAMETERS:
**
**	a_src		The name in the message
**	a_dst		Output: 17 octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$scs_name	(
	const	uint8_t *	a_src,
		char *		a_dst
			)
{
unsigned	l_len = MH$K_SZ_PROCNAM;

	memcpy(a_dst, a_src, l_len);

	while ( l_len && ((a_dst[l_len - 1] == ' ') || !a_dst[l_len - 1]) )
		l_len--;

	a_dst[l_len] = '\0';

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Fill the PPD LENGTH and MTYPE and the SCS header of a message.
**
**  FORMAL PARAMETERS:
**
**	a_msg		The message: W LENGTH, W PPD MTYPE, SCS header ...
**	a_length	LENGTH: 14 + body
**	a_mtype		NISCA$K_SCS_*
**	a_credit	CREDIT
**	a_dst		DST_CONID
**	a_src		SRC_CONID
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$scs_hdr	(
		uint8_t *	a_msg,
		unsigned	a_length,
		unsigned	a_mtype,
		int		a_credit,
		uint32_t	a_dst,
		uint32_t	a_src
			)
{
uint8_t	*l_h = a_msg + MH$K_SM_HDR;

	nisca$put16(a_msg + NISCA$K_PPD_LENGTH, (uint16_t) a_length);
	nisca$put16(a_msg + NISCA$K_PPD_MTYPE, NISCA$K_PPD_SCS_MSG);
	nisca$put16(l_h + NISCA$K_SCS_MTYPE, (uint16_t) a_mtype);
	nisca$put16(l_h + NISCA$K_SCS_CREDIT, (uint16_t) (int16_t) a_credit);
	nisca$put32(l_h + NISCA$K_SCS_DST_CONID, a_dst);
	nisca$put32(l_h + NISCA$K_SCS_SRC_CONID, a_src);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Answer a request of the peer, the way SCS$SEND_RSP does: in a copy of
**	the request, MTYPE + 1, the CONIDs swapped, the credit and the status
**	given, the length of the response.  The octets after STATUS keep what
**	the request had there.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_req		The request from the PPD MTYPE on
**	a_reqlen	Its LENGTH
**	a_length	LENGTH of the response
**	a_credit	CREDIT of the response
**	a_status	STATUS of the response, 0 - none (CR_RSP, DISC_RSP)
**
**  RETURN VALUE:
**	The condition value of MH$TR_SEND_SEQ.
**--
*/
static	int	s_mh$scs_rsp	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_req,
		unsigned	a_reqlen,
		unsigned	a_length,
		int		a_credit,
		unsigned	a_status
			)
{
uint8_t		l_rsp [NISCA$K_PPD_MTYPE + MH$K_SCS_LENGTH_REQ] = {0};
const uint8_t	*l_rh = a_req + 2;

	memcpy(l_rsp + NISCA$K_PPD_MTYPE, a_req, $MIN(a_reqlen, a_length));

	s_mh$scs_hdr(l_rsp, a_length, nisca$get16(l_rh + NISCA$K_SCS_MTYPE) + 1, a_credit,
		nisca$get32(l_rh + NISCA$K_SCS_SRC_CONID), nisca$get32(l_rh + NISCA$K_SCS_DST_CONID));

	if ( a_status )
		{
		nisca$put16(l_rsp + MH$K_SM_BODY + NISCA$K_SCS_MIN_CR, 0);
		nisca$put16(l_rsp + MH$K_SM_BODY + NISCA$K_SCS_STATUS, (uint16_t) a_status);
		}

	return	mh$tr_send_seq(a_port, a_node, l_rsp, NISCA$K_PPD_MTYPE + a_length);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Free a connection; the MSCP server forgets its host.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_why		Why, for the operator; NULL - say nothing (the circuit
**			is gone, VCCLOSE has said it)
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$scs_free	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
	const	char *		a_why
			)
{
	if ( a_cdt->sysap == MH$K_SYSAP_MSCP )
		mh$mscp_disconnect(a_port, a_node, a_cdt);

	if ( a_why && ((a_cdt->sysap == MH$K_SYSAP_MSCP) || a_port->trace) )
		$MHMSG(MH$_CONCLOSE, a_cdt->rproc, a_node->name, a_node->sysid, a_why);

	memset(a_cdt, 0, sizeof(*a_cdt));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The circuit of a node is gone: every connection on it is freed.
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
int	mh$scs_reset	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC	*l_vc = a_node->vc;

	if ( !l_vc )
		return	STS$K_SUCCESS;

	for ( unsigned i = 0; i < MH$K_MAXCDT; i++ )
		{
		if ( l_vc->cdt[i].state != MH$K_CDT_FREE )
			s_mh$scs_free(a_port, a_node, &l_vc->cdt[i], NULL);
		}

	l_vc->reqcdt = 0;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send the request of ours that is due, if none is out: ACCP_REQ of a
**	connection that has just come, DISC_REQ of one that goes.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$TR_SEND_SEQ.
**--
*/
static	int	s_mh$scs_kick	(
		MH$PORT *	a_port,
		MH$NODE *	a_node
			)
{
MH$VC		*l_vc = a_node->vc;
MH$CDT		*l_cdt;
uint8_t		l_req [NISCA$K_PPD_MTYPE + MH$K_SCS_LENGTH_REQ];
uint8_t		*l_b = l_req + MH$K_SM_BODY;
const char	*l_name;

	if ( l_vc->reqcdt )
		return	STS$K_SUCCESS;

	for ( unsigned i = 0; i < MH$K_MAXCDT; i++ )
		{
		l_cdt = &l_vc->cdt[i];

		if ( l_cdt->state == MH$K_CDT_CON_REC )
			{
			/*
			**  ACCP_REQ: our grant of credit, our MIN_CR, the names
			**  turned round, 16 octets of data
			*/
			int	l_dir = (l_cdt->sysap == MH$K_SYSAP_DIR);

			memset(l_req, 0, sizeof(l_req));
			s_mh$scs_hdr(l_req, MH$K_SCS_LENGTH_REQ, NISCA$K_SCS_ACCP_REQ, l_dir ? MH$K_DIR_CREDITS : a_port->credits,
				l_cdt->rconid, l_cdt->lconid);

			nisca$put16(l_b + NISCA$K_SCS_MIN_CR, l_dir ? 0 : 1);
			memset(l_b + NISCA$K_SCS_DST_PROC, ' ', 3 * MH$K_SZ_PROCNAM);
			memcpy(l_b + NISCA$K_SCS_DST_PROC, l_cdt->rproc, strlen(l_cdt->rproc));

			l_name = l_dir ? s_dirname : s_mscpname;
			memcpy(l_b + NISCA$K_SCS_DST_PROC + MH$K_SZ_PROCNAM, l_name, MH$K_SZ_PROCNAM);

			if ( !l_dir )
				memset(l_b + NISCA$K_SCS_DST_PROC + 2 * MH$K_SZ_PROCNAM, 0, MH$K_SZ_PROCNAM);

			l_cdt->state	= MH$K_CDT_ACCP_SENT;
			l_vc->reqcdt	= (int) i + 1;

			return	mh$tr_send_seq(a_port, a_node, l_req, NISCA$K_PPD_MTYPE + MH$K_SCS_LENGTH_REQ);
			}

		if ( (l_cdt->state == MH$K_CDT_DISC_REC) || (l_cdt->state == MH$K_CDT_DISC_SENT) )
			{
			memset(l_req, 0, sizeof(l_req));
			s_mh$scs_hdr(l_req, MH$K_SCS_LENGTH_DISC, NISCA$K_SCS_DISC_REQ, 0, l_cdt->rconid, l_cdt->lconid);
			nisca$put16(l_b + NISCA$K_SCS_STATUS, NISCA$K_SCS_STNORMAL);

			l_cdt->state	= MH$K_CDT_DISC_MTCH;
			l_vc->reqcdt	= (int) i + 1;

			return	mh$tr_send_seq(a_port, a_node, l_req, NISCA$K_PPD_MTYPE + MH$K_SCS_LENGTH_DISC);
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send what waits for a send credit on a connection.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$TR_SEND_SEQ.
**--
*/
static	int	s_mh$scs_flush	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt
			)
{
uint8_t		l_msg [MH$K_SM_BODY + MH$K_SZ_APPL];
MH$APPLMSG	*l_m;
int		l_status = STS$K_SUCCESS;

	while ( a_cdt->nq && (a_cdt->sendcr > 0) && (a_cdt->state == MH$K_CDT_OPEN) )
		{
		l_m = &a_cdt->q[a_cdt->qhead];
		a_cdt->qhead = (a_cdt->qhead + 1) % MH$K_CDT_XMTQ;
		a_cdt->nq--;
		a_cdt->sendcr--;

		s_mh$scs_hdr(l_msg, NISCA$K_SCS_OVHD + l_m->len, NISCA$K_SCS_APPL_MSG, a_cdt->pendrec, a_cdt->rconid, a_cdt->lconid);
		a_cdt->pendrec = 0;
		memcpy(l_msg + MH$K_SM_BODY, l_m->data, l_m->len);

		l_status = mh$tr_send_seq(a_port, a_node, l_msg, MH$K_SM_BODY + l_m->len);
		}

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send an application message on an open connection: an MSCP end
**	message, an answer of the directory.  It waits in the queue of the
**	connection while the peer has given no credit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_msg		The data of the SYSAP
**	a_len		Its length, at most MH$K_SZ_APPL
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - too long, or the queue is full.
**--
*/
int	mh$scs_send	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
	const	uint8_t *	a_msg,
		unsigned	a_len
			)
{
MH$APPLMSG	*l_m;

	if ( a_len > MH$K_SZ_APPL )
		return	STS$K_ERROR;

	if ( a_cdt->nq >= MH$K_CDT_XMTQ )
		return	$LOG_FAO(STS$K_ERROR, "Connection !AZ of !AZ (!UL): !UL messages wait for credit, one is lost", a_cdt->rproc,
			a_node->name, a_node->sysid, a_cdt->nq);

	l_m = &a_cdt->q[(a_cdt->qhead + a_cdt->nq++) % MH$K_CDT_XMTQ];
	l_m->len = (uint16_t) a_len;
	memcpy(l_m->data, a_msg, a_len);

	return	s_mh$scs_flush(a_port, a_node, a_cdt);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The server ends a connection by itself: an MSCP server that holds an
**	END back, for one.  Our DISC_REQ goes when the path lets it.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_why		Why, for the operator
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$TR_SEND_SEQ.
**--
*/
int	mh$scs_disconnect	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
	const	char *		a_why
			)
{
	if ( a_cdt->state != MH$K_CDT_OPEN )
		return	STS$K_SUCCESS;

	$MHMSG(MH$_CONCLOSE, a_cdt->rproc, a_node->name, a_node->sysid, a_why);

	if ( a_cdt->sysap == MH$K_SYSAP_MSCP )
		mh$mscp_disconnect(a_port, a_node, a_cdt);

	a_cdt->sysap	= 0;
	a_cdt->nq	= 0;
	a_cdt->state	= MH$K_CDT_DISC_SENT;

	return	s_mh$scs_kick(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A lookup of SCS$DIRECTORY: answer it in place.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_msg		The lookup
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of MH$SCS_SEND.
**--
*/
static	int	s_mh$dir_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
	const	uint8_t *	a_msg,
		unsigned	a_len
			)
{
uint8_t	l_ans [MH$K_DIR_MSGLEN] = {0};
char	l_name [MH$K_SZ_PROCNAM + 1];
const char *l_info = NULL;

	memcpy(l_ans, a_msg, $MIN(a_len, sizeof(l_ans)));
	s_mh$scs_name(l_ans + 4, l_name);

	if ( !strcmp(l_name, "MSCP$DISK") && a_port->nunit )
		l_info = "                ";
	else if ( !strcmp(l_name, "MSCP$DISK") || !strcmp(l_name, "MSCP$TAPE") || !strcmp(l_name, "VMS$VAXcluster") )
		l_info = s_notpresent;

	if ( l_info )
		{
		nisca$put16(l_ans, 1);
		memcpy(l_ans + 20, l_info, MH$K_SZ_PROCNAM);
		}
	else	nisca$put16(l_ans, MH$K_DIR_NOSUCHOBJ);

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "Directory lookup of !AZ (!UL) for !AZ: !AZ", a_node->name, a_node->sysid, l_name,
			l_info ? l_info : "no such object");

	return	mh$scs_send(a_port, a_node, a_cdt, l_ans, sizeof(l_ans));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A CON_REQ: a new connection to a listener of ours, or a refusal.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_req		The request from the PPD MTYPE on
**	a_len		Its LENGTH
**
**  RETURN VALUE:
**	The condition value of MH$TR_SEND_SEQ.
**--
*/
static	int	s_mh$scs_conreq	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_req,
		unsigned	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$CDT		*l_cdt = NULL;
const uint8_t	*l_h = a_req + 2, *l_b = l_h + NISCA$K_SCS_BODY;
char		l_dst [MH$K_SZ_PROCNAM + 1] = "?", l_src [MH$K_SZ_PROCNAM + 1] = "?";
int		l_sysap = 0, l_status;
unsigned	l_idx = 0;

	if ( a_len >= MH$K_SCS_LENGTH_REQ )
		{
		s_mh$scs_name(l_b + NISCA$K_SCS_DST_PROC, l_dst);
		s_mh$scs_name(l_b + NISCA$K_SCS_DST_PROC + MH$K_SZ_PROCNAM, l_src);

		if ( !memcmp(l_b + NISCA$K_SCS_DST_PROC, s_dirname, MH$K_SZ_PROCNAM) )
			l_sysap = MH$K_SYSAP_DIR;
		else if ( !memcmp(l_b + NISCA$K_SCS_DST_PROC, s_mscpname, MH$K_SZ_PROCNAM) && a_port->nunit )
			l_sysap = MH$K_SYSAP_MSCP;
		}

	if ( !l_sysap )
		{
		if ( a_port->trace )
			$LOG_FAO(STS$K_INFO, "CON_REQ of !AZ (!UL) from !AZ to !AZ refused: no listener", a_node->name, a_node->sysid, l_src, l_dst);

		return	s_mh$scs_rsp(a_port, a_node, a_req, a_len, NISCA$K_SCS_CON_RSP_LEN, 0, NISCA$K_SCS_STNOMAT);
		}

	for ( l_idx = 0; l_idx < MH$K_MAXCDT; l_idx++ )
		{
		if ( l_vc->cdt[l_idx].state == MH$K_CDT_FREE )
			{
			l_cdt = &l_vc->cdt[l_idx];
			break;
			}
		}

	if ( !l_cdt )
		{
		$LOG_FAO(STS$K_WARN, "CON_REQ of !AZ (!UL) from !AZ to !AZ refused: !UL connections already", a_node->name, a_node->sysid,
			l_src, l_dst, MH$K_MAXCDT);

		return	s_mh$scs_rsp(a_port, a_node, a_req, a_len, NISCA$K_SCS_CON_RSP_LEN, 0, MH$K_SCS_STNORS);
		}

	memset(l_cdt, 0, sizeof(*l_cdt));
	l_cdt->state	= MH$K_CDT_CON_REC;
	l_cdt->sysap	= l_sysap;
	l_cdt->host	= -1;

	if ( !++l_vc->cdtgen )
		l_vc->cdtgen = 1;

	l_cdt->lconid	= (l_idx + 1) | ((uint32_t) l_vc->cdtgen << 16);
	l_cdt->rconid	= nisca$get32(l_h + NISCA$K_SCS_SRC_CONID);
	l_cdt->sendcr	= (int16_t) nisca$get16(l_h + NISCA$K_SCS_CREDIT);
	l_cdt->minsend	= nisca$get16(l_b + NISCA$K_SCS_MIN_CR);
	memcpy(l_cdt->rproc, l_src, sizeof(l_cdt->rproc));

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "CON_REQ of !AZ (!UL) from !AZ to !AZ: CONID !XL, credit !SL, data '!AF'", a_node->name, a_node->sysid,
			l_src, l_dst, l_cdt->lconid, l_cdt->sendcr, MH$K_SZ_PROCNAM, l_b + NISCA$K_SCS_DST_PROC + 2 * MH$K_SZ_PROCNAM);

	if ( !(1 & (l_status = s_mh$scs_rsp(a_port, a_node, a_req, a_len, NISCA$K_SCS_CON_RSP_LEN, 0, NISCA$K_SCS_STNORMAL))) )
		return	l_status;

	return	s_mh$scs_kick(a_port, a_node);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in an SCS message of an open path.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_scs		The message from the PPD MTYPE on: W MTYPE (4), then the
**			SCS header and body
**	a_len		LENGTH of the PPD: the octets from a_scs on
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the layers.
**--
*/
int	mh$scs_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
	const	uint8_t *	a_scs,
		unsigned	a_len
			)
{
MH$VC		*l_vc = a_node->vc;
MH$CDT		*l_cdt;
const uint8_t	*l_hdr = a_scs + 2, *l_body = l_hdr + NISCA$K_SCS_BODY;
unsigned	l_mtype, l_idx;
uint32_t	l_dst;
int16_t		l_credit;
int		l_status = STS$K_SUCCESS;

	if ( a_len < NISCA$K_SCS_OVHD )
		{
		a_port->rx_drop++;
		return	STS$K_SUCCESS;
		}

	l_mtype	= nisca$get16(l_hdr + NISCA$K_SCS_MTYPE);
	l_credit = (int16_t) nisca$get16(l_hdr + NISCA$K_SCS_CREDIT);
	l_dst	= nisca$get32(l_hdr + NISCA$K_SCS_DST_CONID);

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "SCS !AZ from !AZ (!UL), CONID !XL -> !XL, credit !SW, !UL octets", (l_mtype < $ARRSZ(s_scstype)) ? s_scstype[l_mtype] : "?",
			a_node->name, a_node->sysid, nisca$get32(l_hdr + NISCA$K_SCS_SRC_CONID), l_dst, l_credit, a_len - NISCA$K_SCS_OVHD);

	if ( l_mtype == NISCA$K_SCS_CON_REQ )
		return	s_mh$scs_conreq(a_port, a_node, a_scs, a_len);

	/*
	**  Everything else is of a connection of ours; one of no connection is
	**  dropped (VMS would break the circuit - the server does not)
	*/
	l_idx = (l_dst & 0xFFFF) - 1;
	l_cdt = (l_idx < MH$K_MAXCDT) ? &l_vc->cdt[l_idx] : NULL;

	if ( !l_cdt || (l_cdt->state == MH$K_CDT_FREE) || (l_cdt->lconid != l_dst) )
		{
		/*
		**  A DISC_REQ of a connection we have freed still wants its answer
		*/
		if ( l_mtype == NISCA$K_SCS_DISC_REQ )
			return	s_mh$scs_rsp(a_port, a_node, a_scs, a_len, NISCA$K_SCS_OVHD, 0, 0);

		a_port->rx_drop++;
		return	$LOG_FAO(STS$K_WARN, "SCS !AZ of !AZ (!UL) to CONID !XL: no such connection, dropped",
			(l_mtype < $ARRSZ(s_scstype)) ? s_scstype[l_mtype] : "?", a_node->name, a_node->sysid, l_dst);
		}

	if ( (l_credit > 0) && (l_mtype != NISCA$K_SCS_CR_RSP) )
		l_cdt->sendcr += l_credit;

	switch ( l_mtype )
		{
		case	NISCA$K_SCS_ACCP_RSP:
			if ( l_cdt->state != MH$K_CDT_ACCP_SENT )
				break;

			l_vc->reqcdt = 0;

			if ( (a_len < NISCA$K_SCS_OVHD + 4) || (nisca$get16(l_body + NISCA$K_SCS_STATUS) != NISCA$K_SCS_STNORMAL) )
				{
				s_mh$scs_free(a_port, a_node, l_cdt, "the peer did not take our ACCP_REQ");
				break;
				}

			l_cdt->state = MH$K_CDT_OPEN;

			if ( (l_cdt->sysap == MH$K_SYSAP_MSCP) || a_port->trace )
				$MHMSG(MH$_CONOPEN, l_cdt->rproc, a_node->name, a_node->sysid, (l_cdt->sysap == MH$K_SYSAP_DIR) ? "SCS$DIRECTORY" : "MSCP$DISK",
					l_cdt->sendcr, (l_cdt->sysap == MH$K_SYSAP_DIR) ? MH$K_DIR_CREDITS : a_port->credits);

			if ( l_cdt->sysap == MH$K_SYSAP_MSCP )
				l_status = mh$mscp_connect(a_port, a_node, l_cdt);

			break;

		case	NISCA$K_SCS_APPL_MSG:
			if ( l_cdt->state != MH$K_CDT_OPEN )
				break;

			/*
			**  The buffer it came in is ours again: the credit goes back
			**  on our next message
			*/
			l_cdt->pendrec++;

			if ( l_cdt->sysap == MH$K_SYSAP_DIR )
				l_status = s_mh$dir_input(a_port, a_node, l_cdt, l_body, a_len - NISCA$K_SCS_OVHD);
			else if ( l_cdt->sysap == MH$K_SYSAP_MSCP )
				l_status = mh$mscp_input(a_port, a_node, l_cdt, l_body, a_len - NISCA$K_SCS_OVHD);

			break;

		case	NISCA$K_SCS_CR_REQ:
			l_status = s_mh$scs_rsp(a_port, a_node, a_scs, a_len, NISCA$K_SCS_OVHD, l_credit, 0);
			break;

		case	NISCA$K_SCS_CR_RSP:
			break;

		case	NISCA$K_SCS_DISC_REQ:
			l_status = s_mh$scs_rsp(a_port, a_node, a_scs, a_len, NISCA$K_SCS_OVHD, 0, 0);

			if ( l_cdt->state == MH$K_CDT_DISC_ACK )
				{
				s_mh$scs_free(a_port, a_node, l_cdt, NULL);
				break;
				}

			if ( l_cdt->state == MH$K_CDT_DISC_MTCH )
				{
				l_cdt->peerdisc = 1;
				break;
				}

			if ( l_cdt->sysap == MH$K_SYSAP_MSCP )
				{
				$MHMSG(MH$_CONCLOSE, l_cdt->rproc, a_node->name, a_node->sysid, "disconnected by the client");
				mh$mscp_disconnect(a_port, a_node, l_cdt);
				l_cdt->sysap = 0;
				}

			l_cdt->peerdisc	= 1;
			l_cdt->nq	= 0;
			l_cdt->state	= MH$K_CDT_DISC_REC;
			break;

		case	NISCA$K_SCS_DISC_RSP:
			if ( l_cdt->state != MH$K_CDT_DISC_MTCH )
				break;

			l_vc->reqcdt = 0;

			if ( l_cdt->peerdisc )
				s_mh$scs_free(a_port, a_node, l_cdt, NULL);
			else	l_cdt->state = MH$K_CDT_DISC_ACK;

			break;

		default:
			a_port->rx_drop++;
			$LOG_FAO(STS$K_WARN, "SCS !AZ of !AZ (!UL) on CONID !XL: not expected, dropped",
				(l_mtype < $ARRSZ(s_scstype)) ? s_scstype[l_mtype] : "?", a_node->name, a_node->sysid, l_dst);
			break;
		}

	if ( l_cdt->state == MH$K_CDT_OPEN )
		s_mh$scs_flush(a_port, a_node, l_cdt);

	s_mh$scs_kick(a_port, a_node);

	return	l_status;
}
