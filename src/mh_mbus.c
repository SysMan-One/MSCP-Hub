#define	__MODULE__	"MHMBUS"
#define	__IDENT__	"X01-17"
#define	__REV__		"1.17.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_mbus.c
**
**  ABSTRACT:	The counters of the server over Modbus/TCP, for a SCADA, a
**		PLC panel or any Modbus client: a thread of its own that serves
**		the statistics block as input registers.
**
**  DESCRIPTION: The protocol thread publishes the statistics block every
**		100 ms (MHSTAT).  This thread takes a whole copy of it with the
**		reader of MHSTAT for every request and answers out of the copy;
**		it shares nothing else with the protocol thread, and neither
**		waits for the other.  A slow or hung client holds up this thread
**		only.
**
**		Functions:
**
**		  03, 04  Read Holding / Input Registers - the same map
**		  2B/0E   Read Device Identification, basic: vendor, product,
**			  version
**
**		anything else is answered ILLEGAL FUNCTION; an address out of
**		the map ILLEGAL DATA ADDRESS.  Every Unit Identifier is
**		answered.  Nothing can be written.
**
**		The map, registers of 16 bits; a counter of 64 bits is four
**		registers, the high word first; text is two characters a
**		register, the first in the high octet:
**
**		     0 ...  999  the server
**		  1000 ... 1799  unit n at 1000 + 100 * n
**		  2000 ... 5199  node n at 2000 + 50 * n
**
**		The definitions of the frames follow those of MBUS-GW-T2R of the
**		same author (t2r_modbus.h): the MODBUS Application Protocol
**		Specification V1.1b3 and the Messaging on TCP/IP Implementation
**		Guide V1.0b.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  22-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		22-OCT-2026	RRL	X01-17 : Initial revision.
**
**--
*/

#define	_GNU_SOURCE	1			/* accept4()					*/

#include	<errno.h>
#include	<string.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<signal.h>
#include	<pthread.h>
#include	<poll.h>
#include	<netdb.h>
#include	<sys/socket.h>
#include	<sys/eventfd.h>
#include	<netinet/in.h>
#include	<netinet/tcp.h>
#include	<arpa/inet.h>

#include	"mh_def.h"
#include	"mh_stat.h"

#define	MODBUS$SZ_MBAPHDR	7		/* Transaction, protocol, length, unit		*/
#define	MODBUS$SZ_MAXADU	260		/* MBAP + the largest PDU			*/
#define	MODBUS$K_MAXWORDS	125		/* FC03/FC04 at most				*/

#define	MODBUS$K_FC_RDHOLD	0x03
#define	MODBUS$K_FC_RDINPUT	0x04
#define	MODBUS$K_FC_MEI		0x2B
#define	MODBUS$K_MEI_DEVID	0x0E

#define	MODBUS$K_EXC_FUNC	0x01		/* ILLEGAL FUNCTION				*/
#define	MODBUS$K_EXC_ADDR	0x02		/* ILLEGAL DATA ADDRESS				*/
#define	MODBUS$K_EXC_VALUE	0x03		/* ILLEGAL DATA VALUE				*/
#define	MODBUS$K_EXC_FAIL	0x04		/* SERVER DEVICE FAILURE: no block		*/

#define	MH$K_MB_CLIENTS		8		/* Clients at a time				*/
#define	MH$K_MB_MAPVERS		1		/* Register 0: the version of the map		*/
#define	MH$K_MB_UNIT0		1000		/* Unit n at 1000 + 100 * n			*/
#define	MH$K_MB_UNITSZ		100
#define	MH$K_MB_NODE0		2000		/* Node n at 2000 + 50 * n			*/
#define	MH$K_MB_NODESZ		50
#define	MH$K_MB_NREG		(MH$K_MB_NODE0 + MH$K_STAT_MAXNODE * MH$K_MB_NODESZ)

typedef struct mh_mbcli_t {
	int		fd;			/* -1 - free					*/
	unsigned	len;			/* Octets of the request taken so far		*/
	uint8_t		buf [MODBUS$SZ_MAXADU];
} MH$MBCLI;

static	int		s_lsd = -1;		/* The listening socket				*/
static	int		s_efd = -1;		/* EVENTFD: the thread is to stop		*/
static	pthread_t	s_thread;
static	int		s_running;
static	MH$MBCLI	s_cli [MH$K_MB_CLIENTS];
static	MH$STAT		s_copy;			/* The copy a request is answered from		*/
static	uint16_t	s_reg [MH$K_MB_NREG];	/* ... laid out as registers			*/



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put a value of 16, 32 or 64 bits, or a text, into the registers.
**
**  FORMAL PARAMETERS:
**
**	a_at		The first register
**	a_val		The value
**	a_txt, a_n	The text and the registers it takes
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mh$mb_put32	(unsigned a_at, uint32_t a_val)
{
	s_reg[a_at]	= (uint16_t) (a_val >> 16);
	s_reg[a_at + 1]	= (uint16_t) a_val;
}

static	void	s_mh$mb_put64	(unsigned a_at, uint64_t a_val)
{
	s_mh$mb_put32(a_at, (uint32_t) (a_val >> 32));
	s_mh$mb_put32(a_at + 2, (uint32_t) a_val);
}

static	void	s_mh$mb_puttxt	(unsigned a_at, const char *a_txt, unsigned a_n)
{
unsigned	l_len = (unsigned) strnlen(a_txt, 2 * a_n);

	for ( unsigned i = 0; i < a_n; i++ )
		s_reg[a_at + i] = (uint16_t) ((((2 * i) < l_len) ? (uint8_t) a_txt[2 * i] : 0) << 8
				| (((2 * i + 1) < l_len) ? (uint8_t) a_txt[2 * i + 1] : 0));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Lay a copy of the block out as the registers of the map.  The map is
**	the interface: the offsets here are the ones of the manuals
**	(MSCPHUB User's Guide, Appendix D), and a change of them raises
**	MH$K_MB_MAPVERS.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_COPY, S_REG.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$mb_layout	(void)
{
const	MH$STAT	*l_st = &s_copy;

	memset(s_reg, 0, sizeof(s_reg));

	/*
	**  The server
	*/
	s_reg[0] = MH$K_MB_MAPVERS;
	s_reg[1] = (uint16_t) l_st->nunit;
	s_reg[2] = (uint16_t) l_st->nnode;
	s_reg[3] = l_st->sysid;
	s_reg[4] = l_st->group;
	s_reg[5] = l_st->alloclass;
	s_reg[6] = l_st->sync;
	s_reg[7] = l_st->ackmode;
	s_mh$mb_put32(8, (uint32_t) (l_st->wall - l_st->started));
	s_mh$mb_puttxt(10, l_st->ident, 4);
	s_mh$mb_puttxt(14, l_st->node, 4);
	s_mh$mb_puttxt(18, l_st->ifname, 8);

	s_mh$mb_put64(100, l_st->rx_frames);
	s_mh$mb_put64(104, l_st->tx_frames);
	s_mh$mb_put64(108, l_st->rx_bytes);
	s_mh$mb_put64(112, l_st->tx_bytes);
	s_mh$mb_put64(116, l_st->rx_drop);
	s_mh$mb_put64(120, l_st->rx_auth);
	s_mh$mb_put64(124, l_st->tx_err);
	s_mh$mb_put64(128, l_st->tr_tx);
	s_mh$mb_put64(132, l_st->tr_rx);
	s_mh$mb_put64(136, l_st->tr_rexmt);
	s_mh$mb_put64(140, l_st->tr_dup);
	s_mh$mb_put64(144, l_st->tr_ooo);
	s_mh$mb_put64(148, l_st->tr_ackbare);
	s_mh$mb_put64(152, l_st->blk_snt);
	s_mh$mb_put64(156, l_st->blk_bytes_out);
	s_mh$mb_put64(160, l_st->blk_ret);
	s_mh$mb_put64(164, l_st->blk_bytes_in);
	s_mh$mb_put64(168, l_st->bufused);
	s_mh$mb_put32(172, l_st->nhrb);

	/*
	**  The units
	*/
	for ( unsigned n = 0; n < l_st->nunit; n++ )
		{
		const	MH$STUNIT *l_u = &l_st->units[n];
		unsigned	l_at = MH$K_MB_UNIT0 + MH$K_MB_UNITSZ * n;

		s_mh$mb_puttxt(l_at, l_u->dvname, 8);
		s_mh$mb_puttxt(l_at + 8, l_u->media, 4);
		s_reg[l_at + 12] = (uint16_t) (1 | (l_u->ro << 1) | (l_u->type << 2) | (l_u->direct << 3));
		s_reg[l_at + 13] = (uint16_t) l_u->ssz;
		s_mh$mb_put32(l_at + 14, l_u->size);
		s_reg[l_at + 16] = l_u->nonline;
		s_reg[l_at + 17] = (uint16_t) l_u->current;
		s_reg[l_at + 18] = (uint16_t) l_u->waiting;
		s_mh$mb_put64(l_at + 20, l_u->online);
		s_mh$mb_put64(l_at + 24, l_u->nread);
		s_mh$mb_put64(l_at + 28, l_u->nwrite);
		s_mh$mb_put64(l_at + 32, l_u->rbytes);
		s_mh$mb_put64(l_at + 36, l_u->wbytes);
		s_mh$mb_put64(l_at + 40, l_u->latn[0]);
		s_mh$mb_put64(l_at + 44, l_u->latsum[0] / 1000);
		s_mh$mb_put64(l_at + 48, l_u->latn[1]);
		s_mh$mb_put64(l_at + 52, l_u->latsum[1] / 1000);
		s_mh$mb_put64(l_at + 56, l_u->qint);
		s_mh$mb_put64(l_at + 60, l_st->qticks);
		s_mh$mb_put64(l_at + 64, l_u->nrmw);
		s_mh$mb_put64(l_at + 68, l_u->nzero);
		}

	/*
	**  The nodes
	*/
	for ( unsigned n = 0; n < l_st->nnode; n++ )
		{
		const	MH$STNODE *l_n = &l_st->nodes[n];
		unsigned	l_at = MH$K_MB_NODE0 + MH$K_MB_NODESZ * n;

		s_mh$mb_puttxt(l_at, l_n->name, 4);
		s_reg[l_at + 4] = l_n->sysid;
		s_reg[l_at + 5] = l_n->chstate;
		s_reg[l_at + 6] = l_n->vcstate;
		s_reg[l_at + 7] = l_n->nconn;
		s_reg[l_at + 8] = l_n->nmscp;
		s_mh$mb_put32(l_at + 10, l_n->chopens);
		s_mh$mb_put32(l_at + 12, l_n->vcopens);
		s_mh$mb_put64(l_at + 16, l_n->ncmd);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Answer one request: build the response PDU in place of the request.
**
**  FORMAL PARAMETERS:
**
**	a_pdu		The PDU of the request (function code onwards); the
**			response is put here
**	a_len		Its length
**	a_rlen		Output: the length of the response PDU
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$mbus_answer	(
		uint8_t *	a_pdu,
		unsigned	a_len,
		unsigned *	a_rlen
			)
{
const	MH$STAT	*l_blk = mh$stat_block();
uint8_t		l_fc = a_pdu[0], l_exc = 0;
unsigned	l_start, l_qty;
static	const char	*s_obj [3] = { "StarLet Squad", "MSCPHUB", __IDENT__ };

	switch ( l_fc )
		{
		case	MODBUS$K_FC_RDHOLD:
		case	MODBUS$K_FC_RDINPUT:
			if ( a_len != 5 )
				{
				l_exc = MODBUS$K_EXC_VALUE;
				break;
				}

			l_start	= (unsigned) (a_pdu[1] << 8 | a_pdu[2]);
			l_qty	= (unsigned) (a_pdu[3] << 8 | a_pdu[4]);

			if ( !l_qty || (l_qty > MODBUS$K_MAXWORDS) )
				{
				l_exc = MODBUS$K_EXC_VALUE;
				break;
				}

			if ( l_start + l_qty > MH$K_MB_NREG )
				{
				l_exc = MODBUS$K_EXC_ADDR;
				break;
				}

			if ( !l_blk || !(1 & mh$stat_read(l_blk, &s_copy)) )
				{
				l_exc = MODBUS$K_EXC_FAIL;
				break;
				}

			s_mh$mb_layout();

			a_pdu[1] = (uint8_t) (2 * l_qty);

			for ( unsigned i = 0; i < l_qty; i++ )
				{
				a_pdu[2 + 2 * i] = (uint8_t) (s_reg[l_start + i] >> 8);
				a_pdu[3 + 2 * i] = (uint8_t) s_reg[l_start + i];
				}

			*a_rlen = 2 + 2 * l_qty;
			return	STS$K_SUCCESS;

		case	MODBUS$K_FC_MEI:
			if ( (a_len != 4) || (a_pdu[1] != MODBUS$K_MEI_DEVID) || !a_pdu[2] || (a_pdu[2] > 4) )
				{
				l_exc = (a_len >= 2) && (a_pdu[1] != MODBUS$K_MEI_DEVID) ? MODBUS$K_EXC_FUNC : MODBUS$K_EXC_VALUE;
				break;
				}

			/*
			**  Basic identification, every object in one answer
			*/
			a_pdu[2] = 0x01;		/* Read device ID code: basic		*/
			a_pdu[3] = 0x01;		/* Conformity: basic, stream access	*/
			a_pdu[4] = 0x00;		/* More follows: no			*/
			a_pdu[5] = 0x00;		/* Next object				*/
			a_pdu[6] = 3;			/* Number of objects			*/
			*a_rlen = 7;

			for ( uint8_t i = 0; i < 3; i++ )
				{
				uint8_t	l_olen = (uint8_t) strlen(s_obj[i]);

				a_pdu[(*a_rlen)++] = i;
				a_pdu[(*a_rlen)++] = l_olen;
				memcpy(a_pdu + *a_rlen, s_obj[i], l_olen);
				*a_rlen += l_olen;
				}

			return	STS$K_SUCCESS;

		default:
			l_exc = MODBUS$K_EXC_FUNC;
		}

	a_pdu[0] = (uint8_t) (l_fc | 0x80);
	a_pdu[1] = l_exc;
	*a_rlen = 2;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take what a client has sent, answer every whole request in it, and
**	close the connection on an end, an error or a frame that is not
**	Modbus/TCP.
**
**  FORMAL PARAMETERS:
**
**	a_cli		The client
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the connection is closed.
**--
*/
static	int	s_mh$mb_client	(
		MH$MBCLI *	a_cli
			)
{
ssize_t		l_got;
unsigned	l_adulen, l_rlen;
uint8_t		l_rsp [MODBUS$SZ_MAXADU];

	if ( 0 >= (l_got = recv(a_cli->fd, a_cli->buf + a_cli->len, sizeof(a_cli->buf) - a_cli->len, 0)) )
		{
		if ( (l_got < 0) && ((errno == EAGAIN) || (errno == EINTR)) )
			return	STS$K_SUCCESS;

		close(a_cli->fd);
		a_cli->fd = -1;
		return	STS$K_ERROR;
		}

	a_cli->len += (unsigned) l_got;

	while ( a_cli->len >= MODBUS$SZ_MBAPHDR )
		{
		/*
		**  The protocol identifier is 0; the length counts the unit
		**  and the PDU
		*/
		l_adulen = 6 + (unsigned) (a_cli->buf[4] << 8 | a_cli->buf[5]);

		if ( (a_cli->buf[2] | a_cli->buf[3]) || (l_adulen < MODBUS$SZ_MBAPHDR + 1) || (l_adulen > sizeof(a_cli->buf)) )
			{
			close(a_cli->fd);
			a_cli->fd = -1;
			return	STS$K_ERROR;
			}

		if ( a_cli->len < l_adulen )
			break;

		memcpy(l_rsp, a_cli->buf, l_adulen);
		mh$mbus_answer(l_rsp + MODBUS$SZ_MBAPHDR, l_adulen - MODBUS$SZ_MBAPHDR, &l_rlen);

		l_rsp[4] = (uint8_t) ((l_rlen + 1) >> 8);
		l_rsp[5] = (uint8_t) (l_rlen + 1);

		if ( (ssize_t) (MODBUS$SZ_MBAPHDR + l_rlen) != send(a_cli->fd, l_rsp, MODBUS$SZ_MBAPHDR + l_rlen, MSG_NOSIGNAL) )
			{
			close(a_cli->fd);
			a_cli->fd = -1;
			return	STS$K_ERROR;
			}

		memmove(a_cli->buf, a_cli->buf + l_adulen, a_cli->len - l_adulen);
		a_cli->len -= l_adulen;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The thread: POLL on the listening socket, the clients and the EVENTFD
**	that stops it.  A client beyond MH$K_MB_CLIENTS is closed at once.
**
**  FORMAL PARAMETERS:
**
**	a_arg		Not used
**
**  RETURN VALUE:
**	NULL.  (The signature of a thread is the system's.)
**--
*/
static	void *	s_mh$mb_thread	(
		void *		a_arg
			)
{
struct pollfd	l_pfd [2 + MH$K_MB_CLIENTS];
int		l_n, l_fd;

	for ( ;; )
		{
		l_pfd[0].fd = s_efd;
		l_pfd[0].events = POLLIN;
		l_pfd[1].fd = s_lsd;
		l_pfd[1].events = POLLIN;

		for ( int i = 0; i < MH$K_MB_CLIENTS; i++ )
			{
			l_pfd[2 + i].fd = s_cli[i].fd;
			l_pfd[2 + i].events = POLLIN;
			l_pfd[2 + i].revents = 0;
			}

		if ( 0 > (l_n = poll(l_pfd, 2 + MH$K_MB_CLIENTS, -1)) )
			{
			if ( errno == EINTR )
				continue;

			$MHMSG(MH$_MBUSERR, "poll()", errno, strerror(errno));
			break;
			}

		if ( l_pfd[0].revents )
			break;

		if ( l_pfd[1].revents & POLLIN )
			{
			if ( 0 <= (l_fd = accept4(s_lsd, NULL, NULL, SOCK_CLOEXEC)) )
				{
				int	i;

				for ( i = 0; (i < MH$K_MB_CLIENTS) && (s_cli[i].fd >= 0); i++ );

				if ( i < MH$K_MB_CLIENTS )
					{
					s_cli[i].fd = l_fd;
					s_cli[i].len = 0;
					}
				else	close(l_fd);
				}
			}

		for ( int i = 0; i < MH$K_MB_CLIENTS; i++ )
			if ( (s_cli[i].fd >= 0) && (l_pfd[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) )
				s_mh$mb_client(&s_cli[i]);
		}

	for ( int i = 0; i < MH$K_MB_CLIENTS; i++ )
		if ( s_cli[i].fd >= 0 )
			{
			close(s_cli[i].fd);
			s_cli[i].fd = -1;
			}

	return	NULL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Start serving the counters over Modbus/TCP: bind and listen here, so
**	that a wrong address or a port in use is said at once, then start the
**	thread with every signal blocked - the signals are the protocol
**	thread's.
**
**  FORMAL PARAMETERS:
**
**	a_spec		/MODBUS: [address:]port, or the port alone; an empty one
**			- not served
**
**  RETURN VALUE:
**	STS$K_SUCCESS - served, or not asked for; MH$_IVPARAM (severity F,
**	signalled) - a wrong /MODBUS; MH$_MBUSERR (severity W, signalled) -
**	not served, the server goes on.
**--
*/
int	mh$mbus_start	(
	const	char *	a_spec
			)
{
struct addrinfo	l_hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV };
struct addrinfo	*l_ai = NULL;
char		l_host [64], l_port [8];
const char	*l_colon;
sigset_t	l_all, l_old;
int		l_on = 1, l_rc;
unsigned long	l_num;
char		*l_end;

	for ( int i = 0; i < MH$K_MB_CLIENTS; i++ )
		s_cli[i].fd = -1;

	if ( !a_spec || !*a_spec )
		return	STS$K_SUCCESS;

	/*
	**  [address:]port; an IPv6 address is the part before the last colon
	*/
	l_host[0] = '\0';

	if ( (l_colon = strrchr(a_spec, ':')) )
		{
		if ( (size_t) (l_colon - a_spec) >= sizeof(l_host) )
			return	$MHMSG(MH$_IVPARAM, a_spec, "MODBUS", "[address:]port is expected");

		memcpy(l_host, a_spec, (size_t) (l_colon - a_spec));
		l_host[l_colon - a_spec] = '\0';
		l_colon++;
		}
	else	l_colon = a_spec;

	l_num = strtoul(l_colon, &l_end, 10);

	if ( *l_end || !l_num || (l_num > 65535) )
		return	$MHMSG(MH$_IVPARAM, a_spec, "MODBUS", "[address:]port is expected");

	memcpy(l_port, l_colon, (size_t) (l_end - l_colon));
	l_port[l_end - l_colon] = '\0';

	if ( (l_rc = getaddrinfo(l_host[0] ? l_host : NULL, l_port, &l_hints, &l_ai)) || !l_ai )
		return	$MHMSG(MH$_IVPARAM, a_spec, "MODBUS", "[address:]port is expected");

	if ( 0 > (s_lsd = socket(l_ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0)) )
		{
		freeaddrinfo(l_ai);
		return	$MHMSG(MH$_MBUSERR, "socket()", errno, strerror(errno));
		}

	setsockopt(s_lsd, SOL_SOCKET, SO_REUSEADDR, &l_on, sizeof(l_on));

	if ( bind(s_lsd, l_ai->ai_addr, l_ai->ai_addrlen) || listen(s_lsd, MH$K_MB_CLIENTS) )
		{
		l_rc = errno;
		freeaddrinfo(l_ai);
		close(s_lsd);
		s_lsd = -1;
		return	$MHMSG(MH$_MBUSERR, "bind()", l_rc, strerror(l_rc));
		}

	freeaddrinfo(l_ai);

	if ( 0 > (s_efd = eventfd(0, EFD_CLOEXEC)) )
		{
		l_rc = errno;
		close(s_lsd);
		s_lsd = -1;
		return	$MHMSG(MH$_MBUSERR, "eventfd()", l_rc, strerror(l_rc));
		}

	sigfillset(&l_all);
	pthread_sigmask(SIG_BLOCK, &l_all, &l_old);
	l_rc = pthread_create(&s_thread, NULL, s_mh$mb_thread, NULL);
	pthread_sigmask(SIG_SETMASK, &l_old, NULL);

	if ( l_rc )
		{
		close(s_lsd);
		close(s_efd);
		s_lsd = s_efd = -1;
		return	$MHMSG(MH$_MBUSERR, "pthread_create()", l_rc, strerror(l_rc));
		}

	s_running = 1;

	return	$MHMSG(MH$_MODBUS, l_host[0] ? l_host : "every address", (unsigned) l_num);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Stop the thread and close its sockets.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$mbus_stop	(void)
{
uint64_t	l_one = 1;

	if ( !s_running )
		return	STS$K_SUCCESS;

	if ( sizeof(l_one) != write(s_efd, &l_one, sizeof(l_one)) )
		$LOG_FAO(STS$K_WARN, "write(EVENTFD) of the Modbus thread failed, errno=!SL", errno);

	pthread_join(s_thread, NULL);
	close(s_lsd);
	close(s_efd);
	s_lsd = s_efd = -1;
	s_running = 0;

	return	STS$K_SUCCESS;
}
