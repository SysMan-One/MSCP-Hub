#define	__MODULE__	"MHLAN"
#define	__IDENT__	"X01-20"
#define	__REV__		"1.20.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_lan.c
**
**  ABSTRACT:	The LAN port: an AF_PACKET socket bound to one interface and to
**		the ethertype 60-07, the group multicast, and the frames in and
**		out.
**
**  DESCRIPTION: The socket is SOCK_RAW: the Ethernet header is ours to build,
**		and so is the length word of the DEC padding that follows it -
**		PEDRIVER opens its LAN port with NMA$C_PCLI_PAD,
**		so every NISCA frame carries the length of its data in
**		little-endian right after the type; the frame is 16 + that
**		length, then zeros up to the Ethernet minimum.
**
**		The MAC address of the interface is not changed: the node
**		address of NISCA (AA-00-04-00-lo-hi) lives in the DX header,
**		not in the Ethernet one.
**
**		Frames we have sent come back to an AF_PACKET socket as
**		PACKET_OUTGOING, or - on the loopback - as ordinary frames with
**		our source address; both are dropped here and counted.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		24-OCT-2026	RRL	X01-20 : a frame up to an MTU of 9000.
**
**--
*/

#include	<errno.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<sys/socket.h>
#include	<sys/ioctl.h>
#include	<net/if.h>
#include	<arpa/inet.h>
#include	<linux/if_packet.h>
#include	<linux/if_ether.h>

#include	"mh_def.h"

static	int	s_mh$lan_fail	(MH$PORT *a_port, const char *a_what);



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Open the port: an AF_PACKET socket for the ethertype 60-07, bound to
**	the interface, non-blocking; the MAC address and the MTU of the
**	interface are taken into the port context.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context, SD is set
**	a_ifname	Name of the interface, ASCIZ
**
**  IMPLICIT INPUTS/OUTPUTS:
**	The process must hold CAP_NET_RAW.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR has been signalled otherwise.
**--
*/
int	mh$lan_open	(
		MH$PORT *	a_port,
	const	char *		a_ifname
			)
{
struct ifreq		l_ifr = {0};
struct sockaddr_ll	l_sll = {0};
size_t			l_len;

	a_port->sd = -1;

	if ( !(l_len = strnlen(a_ifname, IFNAMSIZ)) || (l_len >= IFNAMSIZ) )
		return	$MHMSG(MH$_IVPARAM, a_ifname, "INTERFACE", "the name is empty or too long");

	memcpy(a_port->ifname, a_ifname, l_len + 1);
	memcpy(l_ifr.ifr_name, a_ifname, l_len + 1);

	if ( 0 > (a_port->sd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(NISCA$K_ETHERTYPE))) )
		return	$MHMSG(MH$_SYSERR, "socket(AF_PACKET)", errno, strerror(errno));

	if ( ioctl(a_port->sd, SIOCGIFINDEX, &l_ifr) )
		return	s_mh$lan_fail(a_port, "ioctl(SIOCGIFINDEX)");

	a_port->ifindex = l_ifr.ifr_ifindex;

	if ( ioctl(a_port->sd, SIOCGIFHWADDR, &l_ifr) )
		return	s_mh$lan_fail(a_port, "ioctl(SIOCGIFHWADDR)");

	memcpy(a_port->mac, l_ifr.ifr_hwaddr.sa_data, NISCA$K_SZ_MAC);

	if ( ioctl(a_port->sd, SIOCGIFMTU, &l_ifr) )
		return	s_mh$lan_fail(a_port, "ioctl(SIOCGIFMTU)");

	a_port->mtu = l_ifr.ifr_mtu;

	l_sll.sll_family	= AF_PACKET;
	l_sll.sll_protocol	= htons(NISCA$K_ETHERTYPE);
	l_sll.sll_ifindex	= a_port->ifindex;

	if ( bind(a_port->sd, (struct sockaddr *) &l_sll, sizeof(l_sll)) )
		return	s_mh$lan_fail(a_port, "bind(AF_PACKET)");

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Signal a failed system service of the opening sequence and close the
**	socket.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_what		Name of the service
**
**  RETURN VALUE:
**	The condition value MH$_SYSERR has been signalled with.
**--
*/
static	int	s_mh$lan_fail	(
		MH$PORT *	a_port,
	const	char *		a_what
			)
{
int	l_errno = errno, l_status;

	l_status = $MHMSG(MH$_SYSERR, a_what, l_errno, strerror(l_errno));

	close(a_port->sd);
	a_port->sd = -1;

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Have the interface accept the frames sent to a multicast address.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The multicast address
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR has been signalled otherwise.
**--
*/
int	mh$lan_join	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac
			)
{
struct packet_mreq	l_mr = {0};

	l_mr.mr_ifindex	= a_port->ifindex;
	l_mr.mr_type	= PACKET_MR_MULTICAST;
	l_mr.mr_alen	= NISCA$K_SZ_MAC;
	memcpy(l_mr.mr_address, a_mac, NISCA$K_SZ_MAC);

	if ( setsockopt(a_port->sd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &l_mr, sizeof(l_mr)) )
		return	$MHMSG(MH$_SYSERR, "setsockopt(PACKET_ADD_MEMBERSHIP)", errno, strerror(errno));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Close the port.  The memberships go with the socket.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$lan_close	(
		MH$PORT *	a_port
			)
{
	if ( a_port->sd >= 0 )
		close(a_port->sd);

	a_port->sd = -1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send one NISCA frame: the Ethernet header and the length word are put
**	in front of the data (DX header onwards), the frame is padded with
**	zeros to the Ethernet minimum.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_dst		Destination MAC: an adapter, or the group multicast
**	a_data		DX header and what follows it
**	a_len		Its length, at most MH$K_SZ_MAXDATA
**
**  IMPLICIT INPUTS/OUTPUTS:
**	The TX counters of the port; XMIT of the port, when set, takes the
**	frame instead of the socket.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - too long, or SEND(2) failed (logged with
**	ERRNO, a full TX queue is not signalled to the operator: the protocol
**	retransmits).
**--
*/
int	mh$lan_send	(
		MH$PORT *	a_port,
	const	uint8_t *	a_dst,
	const	uint8_t *	a_data,
		unsigned	a_len
			)
{
uint8_t		l_frame [MH$K_SZ_FRAME];
unsigned	l_frmlen = NISCA$K_SZ_LANHDR + a_len;
ssize_t		l_sent;

	if ( a_len > MH$K_SZ_MAXDATA )
		return	$LOG_FAO(STS$K_ERROR, "Frame data of !UL octets is longer than !UL", a_len, MH$K_SZ_MAXDATA);

	memcpy(l_frame, a_dst, NISCA$K_SZ_MAC);
	memcpy(l_frame + NISCA$K_SZ_MAC, a_port->mac, NISCA$K_SZ_MAC);
	l_frame[12] = (uint8_t) (NISCA$K_ETHERTYPE >> 8);
	l_frame[13] = (uint8_t) NISCA$K_ETHERTYPE;
	nisca$put16(l_frame + NISCA$K_SZ_ETH, (uint16_t) a_len);
	memcpy(l_frame + NISCA$K_SZ_LANHDR, a_data, a_len);

	if ( l_frmlen < NISCA$K_MINFRAME )
		{
		memset(l_frame + l_frmlen, 0, NISCA$K_MINFRAME - l_frmlen);
		l_frmlen = NISCA$K_MINFRAME;
		}

	if ( a_port->xmit )
		{
		a_port->tx_frames++;
		a_port->tx_bytes += l_frmlen;

		return	a_port->xmit(a_port, l_frame, l_frmlen);
		}

	if ( 0 > (l_sent = send(a_port->sd, l_frame, l_frmlen, 0)) )
		{
		a_port->tx_err++;

		return	$LOG_FAO(STS$K_ERROR, "send(!UL octets) on !AZ failed, errno=!SL", l_frmlen, a_port->ifname, errno);
		}

	a_port->tx_frames++;
	a_port->tx_bytes += (uint64_t) l_sent;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Receive one frame, if there is one.  Our own frames, which an
**	AF_PACKET socket sees as PACKET_OUTGOING, are dropped and counted.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		Output area for the whole frame, Ethernet header included
**	a_frmsz		Its size
**	a_len		Output: the length of the frame, 0 - nothing has been received
**
**  IMPLICIT INPUTS/OUTPUTS:
**	The RX counters of the port.
**
**  RETURN VALUE:
**	STS$K_SUCCESS - a frame is in, or there is none (a_len is 0);
**	STS$K_ERROR - RECVFROM(2) failed, logged with ERRNO.
**--
*/
int	mh$lan_recv	(
		MH$PORT *	a_port,
		uint8_t *	a_frame,
		unsigned	a_frmsz,
		unsigned *	a_len
			)
{
struct sockaddr_ll	l_sll;
socklen_t		l_slen;
ssize_t			l_got;

	*a_len = 0;

	for ( ;; )
		{
		l_slen = sizeof(l_sll);

		if ( 0 > (l_got = recvfrom(a_port->sd, a_frame, a_frmsz, MSG_TRUNC, (struct sockaddr *) &l_sll, &l_slen)) )
			{
			if ( (errno == EAGAIN) || (errno == EWOULDBLOCK) )
				return	STS$K_SUCCESS;

			if ( errno == EINTR )
				continue;

			return	$LOG_FAO(STS$K_ERROR, "recvfrom() on !AZ failed, errno=!SL", a_port->ifname, errno);
			}

		/*
		**  Our own frames: PACKET_OUTGOING on a real interface; on the
		**  loopback, and wherever a bridge reflects them, they come in
		**  as ordinary ones, so the source address is checked as well
		*/
		if ( (l_sll.sll_pkttype == PACKET_OUTGOING)
			|| ((l_got >= NISCA$K_SZ_ETH) && !memcmp(a_frame + NISCA$K_SZ_MAC, a_port->mac, NISCA$K_SZ_MAC)) )
			{
			a_port->rx_own++;
			continue;
			}

		if ( (size_t) l_got > a_frmsz )
			{
			a_port->rx_drop++;
			$LOG_FAO(STS$K_WARN, "Frame of !SL octets on !AZ is longer than !UL, dropped", l_got, a_port->ifname, a_frmsz);
			continue;
			}

		a_port->rx_frames++;
		a_port->rx_bytes += (uint64_t) l_got;
		*a_len = (unsigned) l_got;

		return	STS$K_SUCCESS;
		}
}
