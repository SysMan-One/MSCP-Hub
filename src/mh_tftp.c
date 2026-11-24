#define	__MODULE__	"MHTFTP"
#define	__IDENT__	"X.01-25"
#define	__REV__		"1.25.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_tftp.c
**
**  ABSTRACT:	The BOOTP and TFTP server of the diskless I64 satellites: the
**		firmware of a satellite (a LAN entry of the Boot Manager, the
**		memory-disk flag 0x200000) is given its address and the name of
**		its loader by BOOTP, and the loader and every file of its
**		memory disk by TFTP, all of them read from the system disk of
**		the satellite that the server serves - so that a satellite
**		needs no OpenVMS node with TCP/IP Services to boot.
**
**  DESCRIPTION: IPv4 and UDP are the server's own, on the adapter of the port
**		by AF_PACKET as the frames of NISCA and MOP are - the address
**		of /IPADDRESS is no address of Linux.  What is answered:
**
**		    ARP          - a request for /IPADDRESS;
**		    BOOTP, DHCP  - a request of an adapter of /SATELLITEn with
**		                   IP=: BOOTREPLY with its address, the mask, the
**		                   gateway, siaddr - us, and the boot file
**		                   unit:[root.SYSCOMMON.SYSEXE]VMS_LOADER.EFI, the
**		                   full cluster name of the unit (IPB refuses
**		                   another, BADSATDEV); to a DHCPDISCOVER an OFFER,
**		                   to a DHCPREQUEST an ACK (a NAK if it asks for
**		                   another address); the rest of DHCP is not;
**		    TFTP         - a read request of that satellite, from its
**		                   address: the file named as OpenVMS names it,
**		                   unit:[dir...]NAME.TYPE[;version], on the
**		                   satellite's own unit, the highest version.
**		                   Options blksize, tsize and timeout (RFC 2347 -
**		                   2349); the block number past 65535 goes on from
**		                   0.  A write request is refused.
**
**		A file is read whole, by MHODS, when it is asked for, and sent
**		from memory: as it is on the disk up to its end of file, the
**		records of a file of variable length (VAR, VFC) as a stream,
**		each one ended by a line feed - the way TCP/IP Services sends
**		it.  It is read only if TCP/IP Services would read it for its
**		TFTP account: the protection gives WORLD read, or an ACE gives
**		read to the identifier VMS$SATELLITE_ACCESS (%X8000000A), the
**		one CLUSTER_CONFIG_LAN puts on IA64VMSSYS.PAR and
**		CLUSTER_AUTHORIZE.DAT.  A file not found is answered with TFTP
**		error 1: the loader asks in the root first, then in
**		VMS$COMMON.
**
**		A boot is told once its TFTP has been quiet for 10 seconds: the
**		files and octets read, the files not found.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  24-NOV-2026
**
**  MODIFICATION HISTORY:
**
**		24-NOV-2026	CLD	X.01-25 : Initial version.
**
**--
*/

#include	<errno.h>
#include	<stdio.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>

#include	"mh_def.h"

/*
**  Ethernet, ARP, IPv4, UDP: the offsets in a frame
*/
#define	ETH$K_SZ		14
#define	ETH$W_TYPE		12

#define	ARP$K_SZ		28
#define	ARP$W_OP		6
#define	ARP$T_SHA		8
#define	ARP$L_SPA		14
#define	ARP$T_THA		18
#define	ARP$L_TPA		24

#define	IP$K_SZ			20		/* No options in what we send			*/
#define	IP$B_VHL		0
#define	IP$W_LEN		2
#define	IP$W_FRAG		6
#define	IP$B_TTL		8
#define	IP$B_PROTO		9
#define	IP$W_CKSUM		10
#define	IP$L_SRC		12
#define	IP$L_DST		16
#define	IP$K_UDP		17
#define	IP$M_FRAG		0x3FFF		/* More fragments, the offset			*/

#define	UDP$K_SZ		8
#define	UDP$W_SPORT		0
#define	UDP$W_DPORT		2
#define	UDP$W_LEN		4
#define	UDP$W_CKSUM		6

/*
**  BOOTP (RFC 951) and the DHCP that a PXE client speaks (RFC 2131, 2132)
*/
#define	BOOTP$B_OP		0
#define	BOOTP$B_HTYPE		1
#define	BOOTP$B_HLEN		2
#define	BOOTP$L_XID		4
#define	BOOTP$W_FLAGS		10
#define	BOOTP$L_CIADDR		12
#define	BOOTP$L_YIADDR		16
#define	BOOTP$L_SIADDR		20
#define	BOOTP$L_GIADDR		24
#define	BOOTP$T_CHADDR		28
#define	BOOTP$T_SNAME		44
#define	BOOTP$T_FILE		108
#define	BOOTP$T_VEND		236
#define	BOOTP$K_SZ		300		/* With the 64 octets of the vendor area	*/
#define	BOOTP$K_FILESZ		128
#define	BOOTP$K_SNAMESZ		64
#define	BOOTP$M_BROADCAST	0x8000
#define	BOOTP$K_COOKIE		0x63825363U

enum	{
	DHCP$K_PAD		= 0,
	DHCP$K_MASK		= 1,
	DHCP$K_ROUTER		= 3,
	DHCP$K_REQADDR		= 50,
	DHCP$K_LEASE		= 51,
	DHCP$K_MSGTYPE		= 53,
	DHCP$K_SERVERID		= 54,
	DHCP$K_END		= 255
	};

enum	{
	DHCP$K_DISCOVER		= 1,
	DHCP$K_OFFER,
	DHCP$K_REQUEST,
	DHCP$K_DECLINE,
	DHCP$K_ACK,
	DHCP$K_NAK
	};

/*
**  TFTP (RFC 1350): the opcodes, the errors we send
*/
enum	{
	TFTP$K_RRQ		= 1,
	TFTP$K_WRQ,
	TFTP$K_DATA,
	TFTP$K_ACK,
	TFTP$K_ERROR,
	TFTP$K_OACK
	};

enum	{
	TFTP$K_ENOTDEF		= 0,
	TFTP$K_ENOTFOUND,
	TFTP$K_EACCESS,
	TFTP$K_EDISKFULL,
	TFTP$K_EBADOP,
	TFTP$K_EUNKTID
	};

#define	MH$K_TFTP_BLK		512		/* A block of the volume			*/
#define	MH$K_PAR_FIELD		16		/* A parameter file: the counted name of a parameter in it	*/
#define	TFTP$K_MINBLK		8		/* blksize, RFC 2348				*/
#define	TFTP$K_MAXBLK		65464

/*
**  The record attributes and the protection in a file header, FH2 and FAT,
**  that the server looks at
*/
#define	FAT$K_UDF		0
#define	FAT$K_FIX		1
#define	FAT$K_VAR		2
#define	FAT$K_VFC		3
#define	FAT$M_NOSPAN		0x08		/* FAT$B_RATTRIB: records do not cross blocks	*/


static	const uint8_t	s_bcast [NISCA$K_SZ_MAC] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static	char		s_hubname [NISCA$K_SZ_NODENAME + 1];	/* SCSNODE of the server: BOOTP sname		*/
static	uint8_t		s_frame [MH$K_SZ_FRAME];	/* What we send: one at a time			*/



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take an IPv4 address in dotted decimal, a.b.c.d, with a prefix length
**	after it, /nn, if one may be.
**
**  FORMAL PARAMETERS:
**
**	a_str		The text, ASCIZ
**	a_addr		Output: the address, host order
**	a_plen		Output: the prefix length, -1 - none; NULL - none may be
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - not an address.
**--
*/
static	int	s_mh$ip_addr	(
	const	char *		a_str,
		uint32_t *	a_addr,
		int *		a_plen
			)
{
const	char	*l_p = a_str;
char		*l_end;
unsigned long	l_v;
uint32_t	l_a = 0;

	for ( int i = 0; i < 4; i++ )
		{
		if ( (*l_p < '0') || (*l_p > '9') )
			return	STS$K_ERROR;

		l_v = strtoul(l_p, &l_end, 10);

		if ( (l_v > 255) || (l_end - l_p > 3) || ((i < 3) && (*l_end != '.')) )
			return	STS$K_ERROR;

		l_a = (l_a << 8) | (uint32_t) l_v;
		l_p = l_end + (i < 3);
		}

	if ( a_plen )
		*a_plen = -1;

	if ( *l_p == '/' )
		{
		if ( !a_plen || (l_p[1] < '0') || (l_p[1] > '9') || ((l_v = strtoul(l_p + 1, &l_end, 10)), *l_end) || (l_v < 8) || (l_v > 30) )
			return	STS$K_ERROR;

		*a_plen = (int) l_v;
		}
	else if ( *l_p )
		return	STS$K_ERROR;

	*a_addr = l_a;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	An IPv4 address as text.
**
**  FORMAL PARAMETERS:
**
**	a_addr		The address, host order
**	a_out		Output area, 16 octets at least
**
**  RETURN VALUE:
**	A_OUT.
**--
*/
static	char *	s_mh$ip_str	(
		uint32_t	a_addr,
		char *		a_out
			)
{
	snprintf(a_out, 16, "%u.%u.%u.%u", a_addr >> 24, (a_addr >> 16) & 0xFF, (a_addr >> 8) & 0xFF, a_addr & 0xFF);

	return	a_out;
}



/*
**  The octets of the wire are big-endian here, unlike NISCA's
*/
static inline uint16_t	s_get16	(const uint8_t *a_p)	{ return (uint16_t) ((a_p[0] << 8) | a_p[1]); }
static inline uint32_t	s_get32	(const uint8_t *a_p)	{ return ((uint32_t) s_get16(a_p) << 16) | s_get16(a_p + 2); }
static inline void	s_put16	(uint8_t *a_p, uint16_t a_v)	{ a_p[0] = (uint8_t) (a_v >> 8); a_p[1] = (uint8_t) a_v; }
static inline void	s_put32	(uint8_t *a_p, uint32_t a_v)	{ s_put16(a_p, (uint16_t) (a_v >> 16)); s_put16(a_p + 2, (uint16_t) a_v); }



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The Internet checksum: the one's complement sum of 16-bit words, an
**	odd octet at the end padded with zero.
**
**  FORMAL PARAMETERS:
**
**	a_sum		The sum so far, not folded
**	a_p		The octets
**	a_len		... how many
**
**  RETURN VALUE:
**	The sum, not folded and not complemented.
**--
*/
static	uint32_t	s_mh$ip_sum	(
		uint32_t	a_sum,
	const	uint8_t *	a_p,
		unsigned	a_len
			)
{
	for ( ; a_len > 1; a_p += 2, a_len -= 2 )
		a_sum += s_get16(a_p);

	if ( a_len )
		a_sum += (uint32_t) a_p[0] << 8;

	return	a_sum;
}

static	uint16_t	s_mh$ip_fold	(
		uint32_t	a_sum
			)
{
	while ( a_sum >> 16 )
		a_sum = (a_sum & 0xFFFF) + (a_sum >> 16);

	return	(uint16_t) ~a_sum;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The checksum of a UDP datagram: the pseudo header, the header, the data.
**
**  FORMAL PARAMETERS:
**
**	a_src		The source address, host order
**	a_dst		The destination address
**	a_udp		The datagram, its checksum field as it is
**	a_len		Its length
**
**  RETURN VALUE:
**	The checksum to put; 0 is sent as 0xFFFF (RFC 768).
**--
*/
static	uint16_t	s_mh$udp_sum	(
		uint32_t	a_src,
		uint32_t	a_dst,
	const	uint8_t *	a_udp,
		unsigned	a_len
			)
{
uint32_t	l_sum;

	l_sum = (a_src >> 16) + (a_src & 0xFFFF) + (a_dst >> 16) + (a_dst & 0xFFFF) + IP$K_UDP + a_len;

	return	s_mh$ip_fold(s_mh$ip_sum(l_sum, a_udp, a_len));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a UDP datagram from /IPADDRESS: the Ethernet, IPv4 and UDP
**	headers around the data, which the caller has put at S_FRAME + 42.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The destination adapter
**	a_dst		The destination address, host order
**	a_sport		The source port
**	a_dport		The destination port
**	a_len		Octets of the data
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_FRAME; IP_TX of the port.
**
**  RETURN VALUE:
**	The condition value of MH$LAN_IP_SEND.
**--
*/
static	int	s_mh$udp_send	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac,
		uint32_t	a_dst,
		uint16_t	a_sport,
		uint16_t	a_dport,
		unsigned	a_len
			)
{
uint8_t		*l_ip = s_frame + ETH$K_SZ, *l_udp = l_ip + IP$K_SZ;
unsigned	l_len = ETH$K_SZ + IP$K_SZ + UDP$K_SZ + a_len;
uint16_t	l_sum;

	memcpy(s_frame, a_mac, NISCA$K_SZ_MAC);
	memcpy(s_frame + NISCA$K_SZ_MAC, a_port->mac, NISCA$K_SZ_MAC);
	s_put16(s_frame + ETH$W_TYPE, MH$K_ETH_IP);

	memset(l_ip, 0, IP$K_SZ);
	l_ip[IP$B_VHL]	= 0x45;
	s_put16(l_ip + IP$W_LEN, (uint16_t) (IP$K_SZ + UDP$K_SZ + a_len));
	s_put16(l_ip + IP$W_FRAG, 0x4000);	/* Don't fragment			*/
	l_ip[IP$B_TTL]	= 64;
	l_ip[IP$B_PROTO] = IP$K_UDP;
	s_put32(l_ip + IP$L_SRC, a_port->ipaddr);
	s_put32(l_ip + IP$L_DST, a_dst);
	s_put16(l_ip + IP$W_CKSUM, s_mh$ip_fold(s_mh$ip_sum(0, l_ip, IP$K_SZ)));

	s_put16(l_udp + UDP$W_SPORT, a_sport);
	s_put16(l_udp + UDP$W_DPORT, a_dport);
	s_put16(l_udp + UDP$W_LEN, (uint16_t) (UDP$K_SZ + a_len));
	s_put16(l_udp + UDP$W_CKSUM, 0);
	l_sum = s_mh$udp_sum(a_port->ipaddr, a_dst, l_udp, UDP$K_SZ + a_len);
	s_put16(l_udp + UDP$W_CKSUM, l_sum ? l_sum : 0xFFFF);

	if ( l_len < NISCA$K_MINFRAME )
		{
		memset(s_frame + l_len, 0, NISCA$K_MINFRAME - l_len);
		l_len = NISCA$K_MINFRAME;
		}

	a_port->ip_tx++;

	return	mh$lan_ip_send(a_port, MH$K_IP_IP, s_frame, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take /IPADDRESS and /GATEWAY and check the satellites of IP=: their
**	addresses in the network of /IPADDRESS, none twice, none ours.  The
**	satellites are told.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context, the satellites taken by MH$MOP_INIT;
**			output: IPADDR, IPMASK, GATEWAY, the transfers free
**	a_cfg		The configuration
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_IVPARAM or MH$_NOPARAM has been signalled otherwise.
**--
*/
int	mh$tftp_init	(
		MH$PORT *	a_port,
	const	MH$CFG *	a_cfg
			)
{
char		l_spec [ASC$K_SZ + 1], l_mac [3 * NISCA$K_SZ_MAC], l_a [16], l_b [16];
unsigned	l_nip = 0;
int		l_plen;

	memcpy(s_hubname, $ASCPTR(&a_cfg->node), $ASCLEN(&a_cfg->node));
	s_hubname[$ASCLEN(&a_cfg->node)] = '\0';

	a_port->ipaddr = a_port->ipmask = a_port->gateway = 0;

	for ( unsigned i = 0; i < MH$K_IP_NSOCK; i++ )
		a_port->ipsd[i] = -1;

	memset(a_port->xfer, 0, sizeof(a_port->xfer));

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		l_nip += (a_port->sats[i].ip != 0);

	if ( $ASCLEN(&a_cfg->ipaddr) )
		{
		memcpy(l_spec, $ASCPTR(&a_cfg->ipaddr), $ASCLEN(&a_cfg->ipaddr));
		l_spec[$ASCLEN(&a_cfg->ipaddr)] = '\0';

		if ( !(1 & s_mh$ip_addr(l_spec, &a_port->ipaddr, &l_plen)) )
			return	$MHMSG(MH$_IVPARAM, l_spec, "IPADDRESS", "an address and its prefix length, 192.168.42.5/24, is expected");

		l_plen = (l_plen < 0) ? 24 : l_plen;
		a_port->ipmask = ~0U << (32 - l_plen);

		if ( !(a_port->ipaddr & ~a_port->ipmask) || ((a_port->ipaddr & ~a_port->ipmask) == ~a_port->ipmask) )
			return	$MHMSG(MH$_IVPARAM, l_spec, "IPADDRESS", "the address of the network or its broadcast");
		}

	if ( $ASCLEN(&a_cfg->gateway) )
		{
		memcpy(l_spec, $ASCPTR(&a_cfg->gateway), $ASCLEN(&a_cfg->gateway));
		l_spec[$ASCLEN(&a_cfg->gateway)] = '\0';

		if ( !a_port->ipaddr )
			return	$MHMSG(MH$_IVPARAM, l_spec, "GATEWAY", "given without /IPADDRESS");

		if ( !(1 & s_mh$ip_addr(l_spec, &a_port->gateway, NULL))
			|| ((a_port->gateway & a_port->ipmask) != (a_port->ipaddr & a_port->ipmask)) || (a_port->gateway == a_port->ipaddr) )
			return	$MHMSG(MH$_IVPARAM, l_spec, "GATEWAY", "an address in the network of /IPADDRESS, not its own, is expected");
		}

	if ( !l_nip )
		return	STS$K_SUCCESS;

	if ( !a_port->ipaddr )
		return	$MHMSG(MH$_NOPARAM, "IPADDRESS", "a satellite of IP= is loaded by BOOTP and TFTP from it");

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		{
		MH$MOPSAT *l_sat = &a_port->sats[i];

		if ( !l_sat->ip )
			continue;

		s_mh$ip_str(l_sat->ip, l_a);

		if ( ((l_sat->ip & a_port->ipmask) != (a_port->ipaddr & a_port->ipmask)) || (l_sat->ip == a_port->ipaddr)
			|| (l_sat->ip == a_port->gateway) || !(l_sat->ip & ~a_port->ipmask) || ((l_sat->ip & ~a_port->ipmask) == ~a_port->ipmask) )
			return	$MHMSG(MH$_IVPARAM, l_a, "SATELLITE", "IP= must be a free address of a host in the network of /IPADDRESS");

		for ( unsigned j = 0; j < i; j++ )
			if ( a_port->sats[j].ip == l_sat->ip )
				return	$MHMSG(MH$_IVPARAM, l_a, "SATELLITE", "the address of another satellite");

		mh$mac2str(l_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_SATIPCFG, l_mac, l_a, a_port->units[l_sat->unit].dvname, l_sat->root);
		}

	$MHMSG(MH$_IPCFG, s_mh$ip_str(a_port->ipaddr, l_a), __builtin_popcount(a_port->ipmask), a_port->ifname,
		a_port->gateway ? s_mh$ip_str(a_port->gateway, l_b) : "none");

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A name of the wire for the logs: what is not printable becomes ?, the
**	rest is cut at the size of the area.
**
**  FORMAL PARAMETERS:
**
**	a_in		The name, ASCIZ
**	a_out		Output area
**	a_outsz		... its size
**
**  RETURN VALUE:
**	A_OUT.
**--
*/
static	char *	s_mh$tftp_clean	(
	const	char *		a_in,
		char *		a_out,
		unsigned	a_outsz
			)
{
unsigned	l_n = 0;

	for ( ; *a_in && (l_n + 1 < a_outsz); a_in++ )
		a_out[l_n++] = ((*a_in >= ' ') && (*a_in < 0x7F)) ? *a_in : '?';

	a_out[l_n] = '\0';

	return	a_out;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A failure of a satellite: counted, told - the failures of one
**	satellite not more often than once a minute; what came of the wire
**	is told printable.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_file		The file, or what was asked
**	a_why		What has failed
**
**  RETURN VALUE:
**	MH$_TFTPFAIL, told or not.
**--
*/
static	int	s_mh$tftp_fail	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	char *		a_file,
	const	char *		a_why
			)
{
char	l_mac [3 * NISCA$K_SZ_MAC], l_file [128], l_why [256];

	a_sat->tfails++;

	if ( !a_sat->tfaillog || (a_port->tick - a_sat->tfaillog >= MH$K_MOP_FAILLOG) )
		{
		mh$mac2str(a_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_TFTPFAIL, l_mac, a_sat->name, s_mh$tftp_clean(a_file, l_file, sizeof(l_file)), s_mh$tftp_clean(a_why, l_why, sizeof(l_why)));
		a_sat->tfaillog = a_port->tick;
		}

	return	MH$_TFTPFAIL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	An ARP request for /IPADDRESS: our adapter is the answer.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of MH$LAN_IP_SEND.
**--
*/
static	int	s_mh$arp_input	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
static	const uint8_t	s_arphdr [6] = { 0x00, 0x01, 0x08, 0x00, 0x06, 0x04 };	/* Ethernet, IPv4, 6, 4		*/
const	uint8_t	*l_a = a_frame + ETH$K_SZ;
uint8_t		*l_r = s_frame + ETH$K_SZ;

	if ( (a_len < ETH$K_SZ + ARP$K_SZ) || memcmp(l_a, s_arphdr, sizeof(s_arphdr)) || (s_get16(l_a + ARP$W_OP) != 1)
		|| (s_get32(l_a + ARP$L_TPA) != a_port->ipaddr) )
		return	STS$K_SUCCESS;

	a_port->arp_rx++;

	memcpy(s_frame, l_a + ARP$T_SHA, NISCA$K_SZ_MAC);
	memcpy(s_frame + NISCA$K_SZ_MAC, a_port->mac, NISCA$K_SZ_MAC);
	s_put16(s_frame + ETH$W_TYPE, MH$K_ETH_ARP);

	memcpy(l_r, s_arphdr, sizeof(s_arphdr));
	s_put16(l_r + ARP$W_OP, 2);
	memcpy(l_r + ARP$T_SHA, a_port->mac, NISCA$K_SZ_MAC);
	s_put32(l_r + ARP$L_SPA, a_port->ipaddr);
	memcpy(l_r + ARP$T_THA, l_a + ARP$T_SHA, NISCA$K_SZ_MAC);
	memcpy(l_r + ARP$L_TPA, l_a + ARP$L_SPA, 4);
	memset(l_r + ARP$K_SZ, 0, NISCA$K_MINFRAME - ETH$K_SZ - ARP$K_SZ);

	return	mh$lan_ip_send(a_port, MH$K_IP_ARP, s_frame, NISCA$K_MINFRAME);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A BOOTP or DHCP request of a satellite: its address and its boot
**	file.  A request of another adapter, one a relay agent passes on, one
**	that names another server (option 54), and DHCP but DISCOVER and
**	REQUEST, are not ours.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite of the adapter the request came from
**	a_b		The BOOTP message
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of S_MH$UDP_SEND.
**--
*/
static	int	s_mh$bootp_input	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	uint8_t *	a_b,
		unsigned	a_len
			)
{
uint8_t		*l_r = s_frame + ETH$K_SZ + IP$K_SZ + UDP$K_SZ;
unsigned	l_n, l_type = 0, l_reply;
uint32_t	l_req = 0, l_sid = 0;
char		l_mac [3 * NISCA$K_SZ_MAC], l_a [16];
int		l_status, l_bcast;

	if ( (a_len < BOOTP$T_VEND) || (a_b[BOOTP$B_OP] != 1) || (a_b[BOOTP$B_HTYPE] != 1) || (a_b[BOOTP$B_HLEN] != NISCA$K_SZ_MAC)
		|| memcmp(a_b + BOOTP$T_CHADDR, a_sat->mac, NISCA$K_SZ_MAC) || s_get32(a_b + BOOTP$L_GIADDR) )
		return	STS$K_SUCCESS;

	/*
	**  DHCP: the type of the message, the address asked for
	*/
	if ( (a_len >= BOOTP$T_VEND + 4) && (s_get32(a_b + BOOTP$T_VEND) == BOOTP$K_COOKIE) )
		for ( unsigned l_o = BOOTP$T_VEND + 4; (l_o < a_len) && (a_b[l_o] != DHCP$K_END); l_o += 2 + a_b[l_o + 1] )
			{
			if ( a_b[l_o] == DHCP$K_PAD )
				{
				l_o--;
				continue;
				}

			if ( (l_o + 2 > a_len) || (l_o + 2 + a_b[l_o + 1] > a_len) )
				break;

			if ( (a_b[l_o] == DHCP$K_MSGTYPE) && (a_b[l_o + 1] == 1) )
				l_type = a_b[l_o + 2];
			else if ( (a_b[l_o] == DHCP$K_REQADDR) && (a_b[l_o + 1] == 4) )
				l_req = s_get32(a_b + l_o + 2);
			else if ( (a_b[l_o] == DHCP$K_SERVERID) && (a_b[l_o + 1] == 4) )
				l_sid = s_get32(a_b + l_o + 2);
			}

	/*
	**  A REQUEST for the OFFER of another server: the client has chosen it
	*/
	if ( l_sid && (l_sid != a_port->ipaddr) )
		return	STS$K_SUCCESS;

	switch ( l_type )
		{
		case	0:
			l_reply = 0;
			break;

		case	DHCP$K_DISCOVER:
			l_reply = DHCP$K_OFFER;
			break;

		case	DHCP$K_REQUEST:
			if ( !l_req )
				l_req = s_get32(a_b + BOOTP$L_CIADDR);

			l_reply = (l_req && (l_req != a_sat->ip)) ? DHCP$K_NAK : DHCP$K_ACK;
			break;

		default:
			return	STS$K_SUCCESS;
		}

	/*
	**  The reply: what the request carries back, the address, us, the
	**  boot file; the options of the mask, the gateway, DHCP's own
	*/
	memset(l_r, 0, BOOTP$K_SZ);
	l_r[BOOTP$B_OP]		= 2;
	l_r[BOOTP$B_HTYPE]	= 1;
	l_r[BOOTP$B_HLEN]	= NISCA$K_SZ_MAC;
	memcpy(l_r + BOOTP$L_XID, a_b + BOOTP$L_XID, 4);
	memcpy(l_r + BOOTP$W_FLAGS, a_b + BOOTP$W_FLAGS, 2);
	memcpy(l_r + BOOTP$L_CIADDR, a_b + BOOTP$L_CIADDR, 4);
	memcpy(l_r + BOOTP$T_CHADDR, a_sat->mac, NISCA$K_SZ_MAC);

	if ( l_reply != DHCP$K_NAK )
		{
		s_put32(l_r + BOOTP$L_YIADDR, a_sat->ip);
		s_put32(l_r + BOOTP$L_SIADDR, a_port->ipaddr);
		snprintf((char *) l_r + BOOTP$T_SNAME, BOOTP$K_SNAMESZ, "%s", s_hubname);
		snprintf((char *) l_r + BOOTP$T_FILE, BOOTP$K_FILESZ, "%s:[%s.SYSCOMMON.SYSEXE]VMS_LOADER.EFI",
			a_port->units[a_sat->unit].dvname, a_sat->root);
		}

	l_n = BOOTP$T_VEND;
	s_put32(l_r + l_n, BOOTP$K_COOKIE);
	l_n += 4;

	if ( l_reply )
		{
		l_r[l_n++] = DHCP$K_MSGTYPE;
		l_r[l_n++] = 1;
		l_r[l_n++] = (uint8_t) l_reply;
		l_r[l_n++] = DHCP$K_SERVERID;
		l_r[l_n++] = 4;
		s_put32(l_r + l_n, a_port->ipaddr);
		l_n += 4;
		}

	if ( l_reply != DHCP$K_NAK )
		{
		if ( l_reply )
			{
			l_r[l_n++] = DHCP$K_LEASE;
			l_r[l_n++] = 4;
			s_put32(l_r + l_n, 0xFFFFFFFFU);	/* For ever: the address is the satellite's	*/
			l_n += 4;
			}

		l_r[l_n++] = DHCP$K_MASK;
		l_r[l_n++] = 4;
		s_put32(l_r + l_n, a_port->ipmask);
		l_n += 4;

		if ( a_port->gateway )
			{
			l_r[l_n++] = DHCP$K_ROUTER;
			l_r[l_n++] = 4;
			s_put32(l_r + l_n, a_port->gateway);
			l_n += 4;
			}
		}

	l_r[l_n++] = DHCP$K_END;
	l_n = (l_n < BOOTP$K_SZ) ? BOOTP$K_SZ : l_n;

	/*
	**  The client has no address yet: the reply goes to its adapter and to
	**  the address it is given, or to all when it asks for a broadcast
	**  (and a NAK always)
	*/
	l_bcast = (s_get16(a_b + BOOTP$W_FLAGS) & BOOTP$M_BROADCAST) || (l_reply == DHCP$K_NAK);

	a_sat->bootps++;

	if ( !(1 & (l_status = s_mh$udp_send(a_port, l_bcast ? s_bcast : a_sat->mac, l_bcast ? 0xFFFFFFFFU : a_sat->ip, MH$K_BOOTPS,
		MH$K_BOOTPC, l_n))) )
		return	l_status;

	/*
	**  Told once a boot: by the BOOTREPLY or the ACK, not by the OFFER
	**  before the ACK
	*/
	if ( l_reply == DHCP$K_NAK )
		return	s_mh$tftp_fail(a_port, a_sat, s_mh$ip_str(l_req, l_a), "DHCPREQUEST of another address, answered with a NAK");

	if ( l_reply != DHCP$K_OFFER )
		{
		mh$mac2str(a_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_BOOTP, l_mac, a_sat->name, l_type ? "DHCP" : "BOOTP", s_mh$ip_str(a_sat->ip, l_a), (char *) l_r + BOOTP$T_FILE);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a TFTP ERROR to a client: from the port of its transfer, or from
**	69 to a request.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_mac		The adapter of the client
**	a_ip		Its address
**	a_sport		Our port
**	a_cport		Its port
**	a_code		TFTP$K_E*
**	a_text		The message of the error
**
**  RETURN VALUE:
**	The condition value of S_MH$UDP_SEND.
**--
*/
static	int	s_mh$tftp_error	(
		MH$PORT *	a_port,
	const	uint8_t *	a_mac,
		uint32_t	a_ip,
		uint16_t	a_sport,
		uint16_t	a_cport,
		unsigned	a_code,
	const	char *		a_text
			)
{
uint8_t		*l_t = s_frame + ETH$K_SZ + IP$K_SZ + UDP$K_SZ;
unsigned	l_len;

	s_put16(l_t, TFTP$K_ERROR);
	s_put16(l_t + 2, (uint16_t) a_code);
	l_len = 4 + (unsigned) snprintf((char *) l_t + 4, 128, "%s", a_text) + 1;

	return	s_mh$udp_send(a_port, a_mac, a_ip, a_sport, a_cport, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	May the TFTP account of TCP/IP Services read the file: WORLD read in
**	its protection, or an ACE of the identifier VMS$SATELLITE_ACCESS
**	with read in its ACL.
**
**  FORMAL PARAMETERS:
**
**	a_file		The file: FILEPROT, its ACL
**
**  RETURN VALUE:
**	1 - it may; 0 - not.
**--
*/
static	int	s_mh$tftp_mayread	(
	const	MH$ODSFILE *	a_file
			)
{
	if ( !(a_file->fileprot & 0x1000) )
		return	1;

	return	a_file->satacl;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The records of a file of variable length (VAR, VFC) as a stream: the
**	data of each record and a line feed; the control area of a VFC
**	record is left out.  A record of the count FFFF, or one in the end of
**	a block that no record fits in (NOSPAN), ends the records of a block.
**
**  FORMAL PARAMETERS:
**
**	a_file		The file: RTYPE, RATTRIB, VFCSIZE
**	a_raw		Its octets, to the end of file
**	a_len		... how many
**	a_out		Output: the stream, A_LEN octets at most (a record of
**			one octet and its pad becomes two)
**	a_outlen	Output: its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - a record runs past the end of file.
**--
*/
static	int	s_mh$tftp_stream	(
	const	MH$ODSFILE *	a_file,
	const	uint8_t *	a_raw,
		uint64_t	a_len,
		uint8_t *	a_out,
		uint64_t *	a_outlen
			)
{
uint64_t	l_at = 0, l_n = 0;
unsigned	l_cnt, l_vfc = (a_file->rtype == FAT$K_VFC) ? (a_file->vfcsize ? a_file->vfcsize : 2) : 0;

	while ( l_at + 2 <= a_len )
		{
		l_cnt = nisca$get16(a_raw + l_at);

		if ( (l_cnt == 0xFFFF) && (a_file->rattrib & FAT$M_NOSPAN) )
			{
			l_at = (l_at / MH$K_TFTP_BLK + 1) * MH$K_TFTP_BLK;
			continue;
			}

		if ( (l_cnt == 0xFFFF) || (l_at + 2 + l_cnt > a_len) || (l_cnt < l_vfc) )
			return	STS$K_ERROR;

		memcpy(a_out + l_n, a_raw + l_at + 2 + l_vfc, l_cnt - l_vfc);
		l_n += l_cnt - l_vfc;
		a_out[l_n++] = '\n';
		l_at += 2 + l_cnt + (l_cnt & 1);
		}

	*a_outlen = l_n;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read a file a satellite asks for: the name of OpenVMS taken apart -
**	the unit must be the satellite's own, the version is not looked at -
**	the file found by MHODS, its access checked, its octets read, its
**	records made a stream if they are of variable length.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_name		The name it asks for, ASCIZ
**	a_data		Output: the octets, malloc()ed; NULL on a failure
**	a_len		Output: how many
**	a_code		Output: the TFTP error, on a failure
**	a_why		Output: what has failed, A_WHYSZ octets
**	a_whysz		... its size
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - no such file; STS$K_ERROR - it may not
**	or cannot be read.
**--
*/
static	int	s_mh$tftp_read	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	char *		a_name,
		uint8_t **	a_data,
		uint64_t *	a_len,
		unsigned *	a_code,
		char *		a_why,
		unsigned	a_whysz
			)
{
const	MH$UNIT	*l_unit = &a_port->units[a_sat->unit];
static	MH$ODSFILE	s_file;
MH$ODS		l_ods;
char		l_path [MH$K_TFTP_NAMESZ];
const	char	*l_p, *l_dev;
uint8_t		*l_raw = NULL, *l_out = NULL;
uint64_t	l_n;
unsigned	l_dlen, l_plen;
int		l_status;

	*a_data	= NULL;
	*a_len	= 0;
	*a_code	= TFTP$K_EACCESS;

	/*
	**  unit:[dir...]NAME.TYPE;version - the unit may come with _, the
	**  brackets may be <>
	*/
	if ( !(l_p = strchr(a_name, ':')) )
		{
		snprintf(a_why, a_whysz, "no unit in the name");
		return	STS$K_ERROR;
		}

	l_dev	= a_name + (*a_name == '_');
	l_dlen	= (unsigned) (l_p - l_dev);
	l_p++;

	if ( (strlen(l_unit->dvname) != l_dlen) || strncasecmp(l_unit->dvname, l_dev, l_dlen) )
		{
		snprintf(a_why, a_whysz, "not the unit of the satellite, %s", l_unit->dvname);
		return	STS$K_ERROR;
		}

	l_plen = (unsigned) strcspn(l_p, ";");

	if ( (l_plen >= sizeof(l_path)) || ((*l_p != '[') && (*l_p != '<')) )
		{
		snprintf(a_why, a_whysz, "no directory, or too long");
		return	STS$K_ERROR;
		}

	memcpy(l_path, l_p, l_plen);
	l_path[l_plen] = '\0';

	if ( !(1 & mh$ods_open(&l_ods, l_unit->dev.fd, (uint64_t) l_unit->size * 512)) )
		{
		snprintf(a_why, a_whysz, "unit %s: %s", l_unit->dvname, l_ods.why);
		mh$ods_close(&l_ods);
		*a_code = TFTP$K_ENOTDEF;

		return	STS$K_ERROR;
		}

	do	{
		if ( !(1 & (l_status = mh$ods_lookup(&l_ods, l_path, &s_file))) )
			{
			snprintf(a_why, a_whysz, "%s", l_ods.why);
			*a_code = (l_status == STS$K_WARN) ? TFTP$K_ENOTFOUND : TFTP$K_ENOTDEF;
			break;
			}

		if ( !s_mh$tftp_mayread(&s_file) )
			{
			snprintf(a_why, a_whysz, "no WORLD read, no ACE of VMS$SATELLITE_ACCESS");
			l_status = STS$K_ERROR;
			break;
			}

		if ( s_file.size > MH$K_TFTP_MAXFILE )
			{
			snprintf(a_why, a_whysz, "%llu octets, more than %u", (unsigned long long) s_file.size, MH$K_TFTP_MAXFILE);
			l_status = STS$K_ERROR;
			break;
			}

		l_n = (s_file.size + MH$K_TFTP_BLK - 1) / MH$K_TFTP_BLK;

		if ( !(l_raw = malloc(l_n ? l_n * MH$K_TFTP_BLK : 1)) )
			{
			snprintf(a_why, a_whysz, "no memory for %llu octets", (unsigned long long) s_file.size);
			*a_code = TFTP$K_ENOTDEF;
			l_status = STS$K_ERROR;
			break;
			}

		if ( l_n && !(1 & (l_status = mh$ods_read(&l_ods, &s_file, 1, (uint32_t) l_n, l_raw))) )
			{
			snprintf(a_why, a_whysz, "%s", l_ods.why);
			*a_code = TFTP$K_ENOTDEF;
			break;
			}

		if ( (s_file.rtype == FAT$K_VAR) || (s_file.rtype == FAT$K_VFC) )
			{
			if ( !(l_out = malloc(s_file.size + 1)) )
				{
				snprintf(a_why, a_whysz, "no memory for %llu octets", (unsigned long long) s_file.size);
				*a_code = TFTP$K_ENOTDEF;
				l_status = STS$K_ERROR;
				break;
				}

			if ( !(1 & (l_status = s_mh$tftp_stream(&s_file, l_raw, s_file.size, l_out, a_len))) )
				{
				snprintf(a_why, a_whysz, "a record runs past the end of file");
				*a_code = TFTP$K_ENOTDEF;
				break;
				}

			*a_data = l_out;
			l_out = NULL;
			}
		else	{
			*a_data	= l_raw;
			*a_len	= s_file.size;
			l_raw	= NULL;
			}

		l_status = STS$K_SUCCESS;
		}
	while ( 0 );

	free(l_raw);
	free(l_out);
	mh$ods_close(&l_ods);

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The name of a satellite, from its parameter file as it is sent: the
**	value of SCSNODE, a counted name in a field of 16 and 8 characters
**	after it - the logs name the satellite by it from then on.
**
**  FORMAL PARAMETERS:
**
**	a_sat		The satellite; output: NAME
**	a_name		The file sent
**	a_data		Its octets
**	a_len		... how many
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mh$tftp_name	(
		MH$MOPSAT *	a_sat,
	const	char *		a_name,
	const	uint8_t *	a_data,
		uint64_t	a_len
			)
{
static	const char	s_key [] = "\007SCSNODE", s_par [] = "]IA64VMSSYS.PAR";
size_t		l_nlen = strcspn(a_name, ";");
unsigned	l_n;

	if ( (l_nlen < sizeof(s_par) - 1) || strncasecmp(a_name + l_nlen - (sizeof(s_par) - 1), s_par, sizeof(s_par) - 1) )
		return;

	for ( uint64_t i = 0; i + MH$K_PAR_FIELD + 8 <= a_len; i++ )
		if ( !memcmp(a_data + i, s_key, sizeof(s_key) - 1) )
			{
			for ( l_n = 0; (l_n < 8) && (a_data[i + MH$K_PAR_FIELD + l_n] > ' ') && (a_data[i + MH$K_PAR_FIELD + l_n] < 0x7F); l_n++ )
				a_sat->name[l_n] = (char) a_data[i + MH$K_PAR_FIELD + l_n];

			if ( l_n )
				a_sat->name[l_n] = '\0';

			return;
			}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send what is next of a transfer, or again: the OACK, or the DATA of
**	the block BLOCK - the last one shorter than BLKSIZE, empty when the
**	file is of whole blocks.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_x		The transfer
**
**  RETURN VALUE:
**	The condition value of S_MH$UDP_SEND.
**--
*/
static	int	s_mh$tftp_send	(
		MH$PORT *	a_port,
		MH$TFTPXFER *	a_x
			)
{
uint8_t		*l_t = s_frame + ETH$K_SZ + IP$K_SZ + UDP$K_SZ;
uint64_t	l_off;
unsigned	l_len;

	a_x->due = a_port->tick + a_x->timeout;

	if ( !a_x->block )
		{
		s_put16(l_t, TFTP$K_OACK);
		memcpy(l_t + 2, a_x->oack, a_x->oacklen);

		return	s_mh$udp_send(a_port, a_x->mac, a_x->ip, a_x->sport, a_x->cport, 2 + a_x->oacklen);
		}

	l_off = (uint64_t) (a_x->block - 1) * a_x->blksize;
	l_len = (l_off >= a_x->len) ? 0 : (unsigned) (((a_x->len - l_off) < a_x->blksize) ? (a_x->len - l_off) : a_x->blksize);

	s_put16(l_t, TFTP$K_DATA);
	s_put16(l_t + 2, (uint16_t) a_x->block);
	memcpy(l_t + 4, a_x->data + l_off, l_len);

	return	s_mh$udp_send(a_port, a_x->mac, a_x->ip, a_x->sport, a_x->cport, 4 + l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A transfer is over: its data let go, the file counted to the boot.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_x		The transfer
**	a_done		1 - the last block is acknowledged
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mh$tftp_end	(
		MH$PORT *	a_port,
		MH$TFTPXFER *	a_x,
		int		a_done
			)
{
MH$MOPSAT	*l_sat = &a_port->sats[a_x->sat];

	if ( a_done )
		{
		l_sat->tfiles++;
		l_sat->toctets	+= a_x->len;
		l_sat->bfiles++;
		l_sat->boctets	+= a_x->len;
		}

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "TFTP of !AZ to !AZ: !AZ, !UQ octets in !UL blocks of !UL", a_x->file, l_sat->name,
			a_done ? "done" : "cut", a_x->len, a_x->block, a_x->blksize);

	free(a_x->data);
	memset(a_x, 0, sizeof(*a_x));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put an option taken to the OACK of a transfer: its name and its
**	value, each ASCIZ.
**
**  FORMAL PARAMETERS:
**
**	a_x		The transfer: OACK, OACKLEN
**	a_name		The option
**	a_val		Its value
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no room, nothing put.
**--
*/
static	int	s_mh$tftp_opt	(
		MH$TFTPXFER *	a_x,
	const	char *		a_name,
		uint64_t	a_val
			)
{
int	l_n;

	l_n = snprintf(a_x->oack + a_x->oacklen, sizeof(a_x->oack) - a_x->oacklen, "%s%c%llu", a_name, 0, (unsigned long long) a_val);

	if ( (l_n < 0) || ((unsigned) l_n + 1 > sizeof(a_x->oack) - a_x->oacklen) )
		{
		a_x->oack[a_x->oacklen] = '\0';
		return	STS$K_ERROR;
		}

	a_x->oacklen += (unsigned) l_n + 1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A read request of a satellite: the options, the file, a transfer from
**	a port of ours - the OACK first if an option is taken, the first block
**	otherwise.  A request again from the port of a transfer is its first
**	packet lost: what was sent goes again.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_mac		Its adapter
**	a_cport		Its port
**	a_t		The request: the opcode onwards
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of what was sent.
**--
*/
static	int	s_mh$tftp_rrq	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	uint8_t *	a_mac,
		uint16_t	a_cport,
	const	uint8_t *	a_t,
		unsigned	a_len
			)
{
const	char	*l_s[16], *l_e = (const char *) a_t + a_len, *l_p = (const char *) a_t + 2;
char		l_why [sizeof(((MH$ODS *) 0)->why) + 96], l_name [MH$K_TFTP_NAMESZ];
MH$TFTPXFER	*l_x = NULL, *l_old = NULL;
unsigned	l_ns = 0, l_code, l_blksize = MH$K_TFTP_DEFBLK, l_maxblk, l_tmo = 0;
uint8_t		*l_data;
uint64_t	l_len;
int		l_tsize = 0, l_blk = 0, l_status;

	/*
	**  The strings: the name, the mode, the options and their values
	*/
	while ( (l_p < l_e) && (l_ns < $ARRSZ(l_s)) )
		{
		const char *l_z = memchr(l_p, '\0', (size_t) (l_e - l_p));

		if ( !l_z )
			break;

		l_s[l_ns++] = l_p;
		l_p = l_z + 1;
		}

	if ( (l_ns < 2) || !*l_s[0] )
		return	s_mh$tftp_error(a_port, a_mac, a_sat->ip, MH$K_TFTP_PORT, a_cport, TFTP$K_EBADOP, "malformed request");

	s_mh$tftp_clean(l_s[0], l_name, sizeof(l_name));

	for ( unsigned i = 0; i < MH$K_TFTP_MAXXFER; i++ )
		if ( a_port->xfer[i].busy && (a_port->xfer[i].ip == a_sat->ip) && (a_port->xfer[i].cport == a_cport) )
			{
			if ( !strcmp(a_port->xfer[i].file, l_name) && (a_port->xfer[i].block <= 1) )
				{
				a_port->xfer[i].resent++;
				a_sat->trexmt++;

				return	s_mh$tftp_send(a_port, &a_port->xfer[i]);
				}

			s_mh$tftp_end(a_port, &a_port->xfer[i], 0);
			}

	/*
	**  octet only: what the loader asks for; netascii would want its line
	**  ends made CR LF, and nothing of a boot asks for it
	*/
	if ( strcasecmp(l_s[1], "octet") )
		return	s_mh$tftp_error(a_port, a_mac, a_sat->ip, MH$K_TFTP_PORT, a_cport, TFTP$K_EBADOP, "mode not octet");

	/*
	**  The file
	*/
	if ( !(1 & (l_status = s_mh$tftp_read(a_port, a_sat, l_s[0], &l_data, &l_len, &l_code, l_why, sizeof(l_why)))) )
		{
		if ( l_status == STS$K_WARN )
			{
			a_sat->tmiss++;
			a_sat->bmiss++;

			if ( a_port->trace )
				$LOG_FAO(STS$K_INFO, "TFTP of !AZ to !AZ: not found", l_name, a_sat->name);
			}
		else	s_mh$tftp_fail(a_port, a_sat, l_name, l_why);

		return	s_mh$tftp_error(a_port, a_mac, a_sat->ip, MH$K_TFTP_PORT, a_cport, l_code,
			(l_status == STS$K_WARN) ? "file not found" : "access violation");
		}

	s_mh$tftp_name(a_sat, l_s[0], l_data, l_len);

	/*
	**  A transfer: a free one, or the one longest unheard of
	*/
	for ( unsigned i = 0; i < MH$K_TFTP_MAXXFER; i++ )
		{
		if ( !a_port->xfer[i].busy )
			{
			l_x = &a_port->xfer[i];
			break;
			}

		if ( !l_old || (a_port->xfer[i].heard < l_old->heard) )
			l_old = &a_port->xfer[i];
		}

	if ( !l_x )
		{
		s_mh$tftp_end(a_port, l_old, 0);
		l_x = l_old;
		}

	l_x->busy	= 1;
	l_x->sat	= (unsigned) (a_sat - a_port->sats);
	l_x->ip		= a_sat->ip;
	l_x->cport	= a_cport;
	memcpy(l_x->mac, a_mac, NISCA$K_SZ_MAC);
	l_x->data	= l_data;
	l_x->len	= l_len;
	l_x->heard	= a_port->tick;
	memcpy(l_x->file, l_name, sizeof(l_x->file));

	/*
	**  Our port: one no transfer has
	*/
	for ( int l_busy = 1; l_busy; )
		{
		l_x->sport = (uint16_t) (MH$K_TFTP_PORT0 + (a_port->portseed++ % MH$K_TFTP_NPORT));
		l_busy = 0;

		for ( unsigned i = 0; i < MH$K_TFTP_MAXXFER; i++ )
			if ( (&a_port->xfer[i] != l_x) && a_port->xfer[i].busy && (a_port->xfer[i].sport == l_x->sport) )
				l_busy = 1;
		}

	/*
	**  The options we take, each once: the block size up to what a frame
	**  holds, the size of the file, the timeout
	*/
	l_maxblk = (unsigned) a_port->mtu - IP$K_SZ - UDP$K_SZ - 4;
	l_maxblk = (l_maxblk < MH$K_TFTP_MAXBLK) ? l_maxblk : MH$K_TFTP_MAXBLK;

	for ( unsigned i = 2; i + 1 < l_ns; i += 2 )
		{
		unsigned long	l_v;
		char		*l_end;

		l_v = strtoul(l_s[i + 1], &l_end, 10);

		if ( *l_end || !*l_s[i + 1] )
			continue;

		if ( !strcasecmp(l_s[i], "blksize") && !l_blk && (l_v >= TFTP$K_MINBLK) && (l_v <= TFTP$K_MAXBLK) )
			{
			l_blk = 1;
			l_v = (l_v < l_maxblk) ? l_v : l_maxblk;

			if ( 1 & s_mh$tftp_opt(l_x, "blksize", l_v) )
				l_blksize = (unsigned) l_v;
			}
		else if ( !strcasecmp(l_s[i], "tsize") && !l_tsize )
			{
			l_tsize = 1;
			s_mh$tftp_opt(l_x, "tsize", l_len);
			}
		else if ( !strcasecmp(l_s[i], "timeout") && !l_tmo && (l_v >= 1) && (l_v <= 255) && (1 & s_mh$tftp_opt(l_x, "timeout", l_v)) )
			l_tmo = (unsigned) l_v;
		}

	l_x->blksize	= l_blksize;
	l_x->timeout	= l_tmo ? l_tmo * MH$K_TICKS_SEC : MH$K_TFTP_RETRY;
	l_x->block	= l_x->oacklen ? 0 : 1;

	if ( !a_sat->bheard )
		a_sat->bbegun = a_port->tick;

	a_sat->bheard = a_port->tick;

	return	s_mh$tftp_send(a_port, l_x);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A packet of a client to the port of its transfer: the ACK of the
**	block sent - the next one goes, or the transfer is over; an ERROR -
**	the client ends it (the firmware does, after the OACK, when it only
**	wants the size).  An ACK of another block is a duplicate, not answered.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_x		The transfer
**	a_t		The packet: the opcode onwards
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of S_MH$TFTP_SEND.
**--
*/
static	int	s_mh$tftp_ack	(
		MH$PORT *	a_port,
		MH$TFTPXFER *	a_x,
	const	uint8_t *	a_t,
		unsigned	a_len
			)
{
MH$MOPSAT	*l_sat = &a_port->sats[a_x->sat];
uint64_t	l_sent;

	if ( a_len < 4 )
		return	STS$K_SUCCESS;

	a_x->heard	= a_port->tick;
	l_sat->bheard	= a_port->tick;

	if ( s_get16(a_t) == TFTP$K_ERROR )
		{
		s_mh$tftp_end(a_port, a_x, 0);
		return	STS$K_SUCCESS;
		}

	if ( (s_get16(a_t) != TFTP$K_ACK) || (s_get16(a_t + 2) != (uint16_t) a_x->block) )
		return	STS$K_SUCCESS;

	/*
	**  The block acknowledged was the last: shorter than a whole one
	*/
	l_sent = (uint64_t) a_x->block * a_x->blksize;

	if ( a_x->block && (l_sent > a_x->len) )
		{
		s_mh$tftp_end(a_port, a_x, 1);
		return	STS$K_SUCCESS;
		}

	a_x->block++;
	a_x->tries = 0;

	return	s_mh$tftp_send(a_port, a_x);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	An IPv4 frame: a UDP datagram to us - its header and the checksum of
**	it right, not a fragment - from a satellite of IP=: BOOTP to 67 from any
**	address (the client has none yet), TFTP to 69 and to the ports of the
**	transfers from the satellite's own address.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of what was sent.
**--
*/
static	int	s_mh$ip_input	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
const	uint8_t	*l_ip = a_frame + ETH$K_SZ, *l_udp, *l_d;
MH$MOPSAT	*l_sat = NULL;
unsigned	l_hl, l_iplen, l_ulen;
uint32_t	l_src, l_dst;
uint16_t	l_sport, l_dport;

	if ( a_len < ETH$K_SZ + IP$K_SZ + UDP$K_SZ )
		return	STS$K_SUCCESS;

	l_hl	= 4u * (l_ip[IP$B_VHL] & 0x0F);
	l_iplen	= s_get16(l_ip + IP$W_LEN);

	if ( ((l_ip[IP$B_VHL] >> 4) != 4) || (l_hl < IP$K_SZ) || (l_iplen < l_hl + UDP$K_SZ) || (ETH$K_SZ + l_iplen > a_len)
		|| (l_ip[IP$B_PROTO] != IP$K_UDP) || (s_get16(l_ip + IP$W_FRAG) & IP$M_FRAG) )
		return	STS$K_SUCCESS;

	l_src	= s_get32(l_ip + IP$L_SRC);
	l_dst	= s_get32(l_ip + IP$L_DST);
	l_udp	= l_ip + l_hl;
	l_ulen	= s_get16(l_udp + UDP$W_LEN);
	l_sport	= s_get16(l_udp + UDP$W_SPORT);
	l_dport	= s_get16(l_udp + UDP$W_DPORT);

	if ( (l_dst != a_port->ipaddr) && (l_dport != MH$K_BOOTPS) )
		return	STS$K_SUCCESS;

	/*
	**  The checksum of the IP header is looked at, the one of UDP is not:
	**  a frame of the stack of this host (a veth, a TAP) comes to AF_PACKET
	**  with it not filled in yet - the Ethernet CRC has covered the frame
	*/
	if ( (l_ulen < UDP$K_SZ) || (l_ulen > l_iplen - l_hl) || s_mh$ip_fold(s_mh$ip_sum(0, l_ip, l_hl)) )
		{
		a_port->ip_bad++;
		return	STS$K_SUCCESS;
		}

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		if ( a_port->sats[i].ip && !memcmp(a_port->sats[i].mac, a_frame + NISCA$K_SZ_MAC, NISCA$K_SZ_MAC) )
			{
			l_sat = &a_port->sats[i];
			break;
			}

	if ( !l_sat )
		{
		a_port->ip_other++;
		return	STS$K_SUCCESS;
		}

	a_port->ip_rx++;
	l_d	= l_udp + UDP$K_SZ;
	l_ulen	-= UDP$K_SZ;

	if ( l_dport == MH$K_BOOTPS )
		return	((l_sport == MH$K_BOOTPC) && ((l_dst == a_port->ipaddr) || (l_dst == 0xFFFFFFFFU)))
			? s_mh$bootp_input(a_port, l_sat, l_d, l_ulen) : STS$K_SUCCESS;

	if ( (l_src != l_sat->ip) || (l_ulen < 2) )
		return	STS$K_SUCCESS;

	if ( l_dport == MH$K_TFTP_PORT )
		{
		switch ( s_get16(l_d) )
			{
			case	TFTP$K_RRQ:
				return	s_mh$tftp_rrq(a_port, l_sat, a_frame + NISCA$K_SZ_MAC, l_sport, l_d, l_ulen);

			case	TFTP$K_WRQ:
				return	s_mh$tftp_error(a_port, a_frame + NISCA$K_SZ_MAC, l_src, MH$K_TFTP_PORT, l_sport, TFTP$K_EACCESS,
					"the server is read only");
			}

		return	STS$K_SUCCESS;
		}

	for ( unsigned i = 0; i < MH$K_TFTP_MAXXFER; i++ )
		{
		MH$TFTPXFER *l_x = &a_port->xfer[i];

		if ( l_x->busy && (l_x->sport == l_dport) && (l_x->ip == l_src) )
			{
			if ( l_x->cport == l_sport )
				return	s_mh$tftp_ack(a_port, l_x, l_d, l_ulen);

			return	s_mh$tftp_error(a_port, a_frame + NISCA$K_SZ_MAC, l_src, l_dport, l_sport, TFTP$K_EUNKTID, "unknown transfer ID");
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A frame of the IP sockets: ARP or IPv4.  A server of /PASSIVE answers
**	nothing.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sock		The socket: MH$K_IP_IP, MH$K_IP_ARP
**	a_frame		The frame
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of what was sent.
**--
*/
int	mh$tftp_input	(
		MH$PORT *	a_port,
		unsigned	a_sock,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
	if ( a_port->passive )
		return	STS$K_SUCCESS;

	if ( (a_len < ETH$K_SZ) || (s_get16(a_frame + ETH$W_TYPE) != ((a_sock == MH$K_IP_ARP) ? MH$K_ETH_ARP : MH$K_ETH_IP)) )
		return	STS$K_SUCCESS;

	return	(a_sock == MH$K_IP_ARP) ? s_mh$arp_input(a_port, a_frame, a_len) : s_mh$ip_input(a_port, a_frame, a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The tick of the server: a packet unacknowledged in its timeout goes
**	again, five times; a boot whose TFTP has been quiet for 10 seconds is
**	told.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$tftp_tick	(
		MH$PORT *	a_port
			)
{
char	l_mac [3 * NISCA$K_SZ_MAC];

	for ( unsigned i = 0; i < MH$K_TFTP_MAXXFER; i++ )
		{
		MH$TFTPXFER *l_x = &a_port->xfer[i];

		if ( !l_x->busy || (a_port->tick < l_x->due) )
			continue;

		if ( ++l_x->tries > MH$K_TFTP_TRIES )
			{
			s_mh$tftp_end(a_port, l_x, 0);
			continue;
			}

		l_x->resent++;
		a_port->sats[l_x->sat].trexmt++;
		s_mh$tftp_send(a_port, l_x);
		}

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		{
		MH$MOPSAT *l_sat = &a_port->sats[i];

		if ( !l_sat->bheard || (a_port->tick - l_sat->bheard < MH$K_TFTP_QUIET) )
			continue;

		mh$mac2str(l_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_TFTPLOAD, l_mac, l_sat->name, l_sat->bfiles, l_sat->boctets,
			(unsigned) ((l_sat->bheard - l_sat->bbegun) / MH$K_TICKS_SEC), l_sat->bmiss);
		l_sat->bheard = l_sat->bbegun = 0;
		l_sat->bfiles = l_sat->bmiss = 0;
		l_sat->boctets = 0;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The counters of every satellite of IP=, on SIGUSR1.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	The condition value of the last message, STS$K_SUCCESS if none.
**--
*/
int	mh$tftp_stats	(
		MH$PORT *	a_port
			)
{
char	l_mac [3 * NISCA$K_SZ_MAC];
int	l_status = STS$K_SUCCESS;

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		{
		const	MH$MOPSAT *l_sat = &a_port->sats[i];

		if ( !l_sat->ip )
			continue;

		mh$mac2str(l_sat->mac, l_mac, sizeof(l_mac));
		l_status = $MHMSG(MH$_TFTPSTATS, l_mac, l_sat->name, l_sat->bootps, l_sat->tfiles, l_sat->toctets, l_sat->tmiss,
			l_sat->tfails, l_sat->trexmt);
		}

	return	l_status;
}
