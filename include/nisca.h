#ifndef	__NISCA$H__
#define	__NISCA$H__	1

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	nisca.h
**
**  ABSTRACT:	The wire formats of NISCA, the protocol PEDRIVER speaks over
**		the LAN: the frame, the DX header, the channel control (CC)
**		message, the transport (TR) header and the PPC opcodes, with
**		the little-endian accessors every module uses on them.
**
**  DESCRIPTION: A NISCA frame on an Ethernet looks so:
**
**		+0   6	destination MAC (a channel's adapter, or the group multicast)
**		+6   6	source MAC (our adapter)
**		+12  2	type 60-07
**		+14  2	length of what follows, little-endian (DEC "padding")
**		+16 14	DX: logical destination and source node address, group
**		+30  n	CC (bit 7 of its first octet is set) or TR + PPC
**
**		Every offset below is relative to the start of its own header.
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

#ifndef	__MODULE__
#define	__MODULE__	"NISCA"
#endif

#ifndef	__IDENT__
#define	__IDENT__	"X01-10"
#endif

#ifndef	__REV__
#define	__REV__		"1.10.0"
#endif

#include	<stdint.h>
#include	<string.h>

/*
**  The frame
*/
#define	NISCA$K_ETHERTYPE	0x6007		/* 60-07, in network order on the wire		*/
#define	NISCA$K_SZ_MAC		6
#define	NISCA$K_SZ_ETH		14		/* Destination, source, type			*/
#define	NISCA$K_SZ_PAD		2		/* The length word that follows the type	*/
#define	NISCA$K_SZ_LANHDR	(NISCA$K_SZ_ETH + NISCA$K_SZ_PAD)
#define	NISCA$K_MAXFRAME	1514		/* Without the FCS				*/
#define	NISCA$K_MINFRAME	60		/* Shorter frames are padded with zeros		*/
#define	NISCA$K_MAXDATA		(NISCA$K_MAXFRAME - NISCA$K_SZ_LANHDR)	/* 1498 - CSMACD$C_MAX_PKTSIZE_PAD	*/

/*
**  DX header, NISCA$C_DX_LNG_HDR
*/
#define	NISCA$K_SZ_DX		14
#define	NISCA$K_DX_DST_LO	0x00		/* L							*/
#define	NISCA$K_DX_DST_HI	0x04		/* W							*/
#define	NISCA$K_DX_GROUP	0x06		/* W							*/
#define	NISCA$K_DX_SRC_LO	0x08		/* L							*/
#define	NISCA$K_DX_SRC_HI	0x0C		/* W							*/

#define	NISCA$K_NODE_LO		0x000400AAU	/* AA-00-04-00-lo-hi: lo/hi = low word of SCSSYSTEMID	*/
#define	NISCA$K_GROUP_LO	0x010400ABU	/* AB-00-04-01-xx-yy: yyxx = 0x0100 + group		*/
#define	NISCA$K_GROUP_BASE	0x0100
#define	NISCA$K_GROUP_MAX	0xFEFF		/* Group 0 is not allowed				*/

/*
**  CC - channel control message, NISCA$C_CC_LNG_HDR.  The length of the
**  message tells its version: PEDRIVER compares nothing but MAJOR.
*/
#define	NISCA$K_CC_MSG		0x00		/* B: type, AUTHORIZE, OK_XMT, NO_XMT, CCFLG		*/
#define	NISCA$K_CC_REASON	0x01		/* B							*/
#define	NISCA$K_CC_MAINT	0x02		/* L: port type 8 + "multi-rails"			*/
#define	NISCA$K_CC_MINOR	0x06		/* B							*/
#define	NISCA$K_CC_MAJOR	0x07		/* B							*/
#define	NISCA$K_CC_ECO		0x08		/* W							*/
#define	NISCA$K_CC_NODENAME	0x0A		/* 8: ASCIC, length octet 6, blank-padded		*/
#define	NISCA$K_CC_PORT_FCN	0x12		/* L							*/
#define	NISCA$K_CC_STA_INFO	0x16		/* L							*/
#define	NISCA$K_CC_RST_PORT	0x1A		/* 6							*/
#define	NISCA$K_CC_PIPE_QUOTA	0x21		/* B: receive ring of the adapter, <= 31		*/
#define	NISCA$K_CC_P_SRV	0x22		/* W: services offered				*/
#define	NISCA$K_CC_R_SRV	0x24		/* W: services required				*/
#define	NISCA$K_CC_AUTHORIZE	0x26		/* Q: the cluster password hash, zero in a HELLO	*/
#define	NISCA$K_CC_SERVICE	0x2E		/* 16: ASCIC, SOLICIT_SRV only			*/
#define	NISCA$K_CC_LCL_CHSEQ	0x3E		/* W: incarnation of the channel			*/
#define	NISCA$K_CC_BUS_BUFSZ	0x40		/* W: largest data field of a block			*/
#define	NISCA$K_CC_XMT_TIME	0x42		/* Q							*/
#define	NISCA$K_CC_DEVNAME	0x4A		/* 16: ASCIC name of the adapter			*/
#define	NISCA$K_CC_LAN_HWADDR	0x5A		/* 6							*/
#define	NISCA$K_CC_DEVTYPE	0x60		/* B							*/
#define	NISCA$K_CC_NODE_HWTYPE	0x61		/* B: 0 VAX, 1 Alpha, 2 FIB, 3 IA64			*/
#define	NISCA$K_CC_TR_RCVCACHE	0x62		/* W							*/
#define	NISCA$K_CC_LOAD_CLASS	0x64		/* L							*/

#define	NISCA$K_CC_LEN_V11	0x2E
#define	NISCA$K_CC_LEN_V12	0x3E
#define	NISCA$K_CC_LEN_V13	0x62
#define	NISCA$K_CC_LEN_V15	0x68

#define	NISCA$K_SZ_NODENAME	6		/* SCSNODE as the CC message carries it		*/
#define	NISCA$K_SZ_SERVICE	16
#define	NISCA$K_SZ_DEVNAME	16

#define	NISCA$M_CC_TYPE		0x0F
#define	NISCA$M_CC_AUTHORIZE	0x10		/* The AUTHORIZE field is valid				*/
#define	NISCA$M_CC_OK_XMT	0x20		/* You may send to me through this adapter		*/
#define	NISCA$M_CC_NO_XMT	0x40
#define	NISCA$M_CC_CCFLG	0x80		/* This is a CC message, not a TR one			*/

enum	{
	NISCA$K_CC_HELLO	= 0,		/* Multicast, every 1.5 - 3 s				*/
	NISCA$K_CC_BYE,				/* Multicast, last gasp					*/
	NISCA$K_CC_CCSTART,
	NISCA$K_CC_VERF,
	NISCA$K_CC_VACK,
	NISCA$K_CC_DONE,			/* "Not yet implemented"				*/
	NISCA$K_CC_SOLICIT_SRV			/* A booting satellite looking for its system disk	*/
	};

#define	NISCA$K_MAJOR		1

/*
**  TR - transport header.  TR_PAD tells the version of every packet:
**  >= 3 - V1.3+ (channel sequence number), >= 0x0F - V1.4+ (32-bit numbers).
*/
#define	NISCA$K_TR_FLAG		0x00		/* B							*/
#define	NISCA$K_TR_PAD		0x01		/* B: the PPC opcode is at 6 + TR_PAD			*/
#define	NISCA$K_TR_ACK		0x02		/* W							*/
#define	NISCA$K_TR_SEQ		0x04		/* W							*/
#define	NISCA$K_TR_RMT_CHSEQ	0x06		/* W: V1.3						*/
#define	NISCA$K_TR_PAD_DLEN	0x08		/* B: V1.3						*/
#define	NISCA$K_TR_ACK_SEQ	0x0A		/* L: V1.4						*/
#define	NISCA$K_TR_MSG_SEQ	0x0E		/* L: V1.4						*/
#define	NISCA$K_TR_LAST_ACC	0x12		/* L: V1.5						*/
#define	NISCA$K_TR_FLAG_EXT	0x16		/* W: V1.5						*/

#define	NISCA$K_TR_PPC_BASE	6

#define	NISCA$K_TR_LEN_V10	6
#define	NISCA$K_TR_LEN_V13	9		/* TR_PAD = TR_PAD_DATA_LEN = 3			*/
#define	NISCA$K_TR_LEN_V14	0x15		/* From it on the numbers are longwords		*/
#define	NISCA$K_TR_LEN_V15	0x19
#define	NISCA$K_TR_PAD_DLEN_V15	0x12		/* TR_PAD_DATA_LEN of V1.5, as PEDRIVER V8.3 sends it	*/

#define	NISCA$M_TR_DATA		0x01
#define	NISCA$M_TR_SEQ		0x02
#define	NISCA$M_TR_NAK		0x04
#define	NISCA$M_TR_ACK		0x08
#define	NISCA$M_TR_RSVP		0x10
#define	NISCA$M_TR_REXMT	0x20
#define	NISCA$M_TR_FLAG_EXT	0x40

/*
**  PPC opcodes, NISCA$B_PPC_OPC
*/
enum	{
	NISCA$K_PPC_SNT_DG	= 0,
	NISCA$K_PPC_SNT_SEQ	= 2,
	NISCA$K_PPC_SNT_DAT	= 4,
	NISCA$K_PPC_SNT_DAT_LP	= 5,
	NISCA$K_PPC_REQ_DAT0	= 8,
	NISCA$K_PPC_REQ_DAT1	= 9,
	NISCA$K_PPC_REQ_DAT2	= 10,
	NISCA$K_PPC_RET_DAT	= 12,
	NISCA$K_PPC_RET_DAT_LP	= 13,
	NISCA$K_PPC_RET_CNF	= 15,
	NISCA$K_PPC_SNT_DATWM	= 20,
	NISCA$K_PPC_SNT_DATWM_LP = 21
	};

/*
**  A block transfer, after the PPC opcode:
**
**	SNT_DAT, RET_DAT	XCT_ID, XCT_LEN = the data of this chunk, the
**				names and offsets of the chunk, the data
**	REQ_DAT0/1/2		the header alone, XCT_LEN = all the data asked
**	RET_CNF			XCT_ID alone, 8 octets
**
**  SND is always the side the data comes from; the receiver checks only
**  REC_NAME and REC_BOFF + the length of the data against its buffer
**  - a wrong one is BUG_CHECK VAXPORT there.
*/
#define	NISCA$K_BLK_XCT_ID	0x00		/* Q: LCONID + RSPID of the side that asks	*/
#define	NISCA$K_BLK_XCT_LEN	0x08		/* L							*/
#define	NISCA$K_BLK_SND_NAME	0x0C		/* L							*/
#define	NISCA$K_BLK_SND_BOFF	0x10		/* L							*/
#define	NISCA$K_BLK_REC_NAME	0x14		/* L							*/
#define	NISCA$K_BLK_REC_BOFF	0x18		/* L							*/
#define	NISCA$K_BLK_HLEN	0x1C		/* 28: PEMPPC$C_BLK_XFER_HLEN			*/
#define	NISCA$K_BLK_CNF_LEN	8		/* RET_CNF					*/

/*
**  The buffer descriptor of a peer, as a SYSAP passes it on (CIBHAN, 12
**  octets; MSCP: the BUFFER field of READ/WRITE)
*/
#define	NISCA$K_BD_BOFF		0x00		/* L: offset in the buffer, MAP puts 0	*/
#define	NISCA$K_BD_BNAME	0x04		/* L: the name				*/
#define	NISCA$K_BD_RCONID	0x08		/* L: CONID of the side that moves the data	*/
#define	NISCA$K_SZ_BD		12

/*
**  PPD: after the PPC opcode of SNT_DG / SNT_SEQ - W LENGTH (octets that
**  follow it), W MTYPE, then the data
*/
#define	NISCA$K_PPD_LENGTH	0x00
#define	NISCA$K_PPD_MTYPE	0x02
#define	NISCA$K_PPD_DATA	0x04

enum	{
	NISCA$K_PPD_START	= 0,
	NISCA$K_PPD_STACK,
	NISCA$K_PPD_ACK,
	NISCA$K_PPD_SCS_DG,
	NISCA$K_PPD_SCS_MSG,
	NISCA$K_PPD_ELOG,
	NISCA$K_PPD_HOSTSHUT
	};

/*
**  START/STACK data, from NISCA$K_PPD_DATA; LENGTH = 62
*/
#define	NISCA$K_ST_SYSTEMID	0x00		/* 6							*/
#define	NISCA$K_ST_PROTOCOL	0x06		/* B: 1							*/
#define	NISCA$K_ST_MAXDG	0x08		/* W							*/
#define	NISCA$K_ST_MAXMSG	0x0A		/* W							*/
#define	NISCA$K_ST_SWTYPE	0x0C		/* 4							*/
#define	NISCA$K_ST_SWVERS	0x10		/* 4							*/
#define	NISCA$K_ST_SWINCARN	0x14		/* Q							*/
#define	NISCA$K_ST_HWTYPE	0x1C		/* 4							*/
#define	NISCA$K_ST_HWVERS	0x20		/* 12							*/
#define	NISCA$K_ST_NODENAME	0x2C		/* 8, blank-padded					*/
#define	NISCA$K_ST_CURTIME	0x34		/* Q							*/
#define	NISCA$K_ST_LEN		0x3C		/* 60 octets of data				*/
#define	NISCA$K_ST_LENGTH	(2 + NISCA$K_ST_LEN)	/* LENGTH: MTYPE + data = 62			*/

/*
**  SCS header, from NISCA$K_PPD_DATA of an SCS_MSG / SCS_DG
*/
#define	NISCA$K_SCS_MTYPE	0x00		/* W							*/
#define	NISCA$K_SCS_CREDIT	0x02		/* W, signed						*/
#define	NISCA$K_SCS_DST_CONID	0x04		/* L							*/
#define	NISCA$K_SCS_SRC_CONID	0x08		/* L							*/
#define	NISCA$K_SCS_BODY	0x0C
#define	NISCA$K_SCS_OVHD	14		/* PPD MTYPE + the SCS header: LENGTH = 14 + body	*/

enum	{
	NISCA$K_SCS_CON_REQ	= 0,
	NISCA$K_SCS_CON_RSP,
	NISCA$K_SCS_ACCP_REQ,
	NISCA$K_SCS_ACCP_RSP,
	NISCA$K_SCS_REJ_REQ,
	NISCA$K_SCS_REJ_RSP,
	NISCA$K_SCS_DISC_REQ,
	NISCA$K_SCS_DISC_RSP,
	NISCA$K_SCS_CR_REQ,
	NISCA$K_SCS_CR_RSP,
	NISCA$K_SCS_APPL_MSG,
	NISCA$K_SCS_APPL_DG
	};

#define	NISCA$K_SCS_MIN_CR	0x00		/* Body: W							*/
#define	NISCA$K_SCS_STATUS	0x02		/* Body: W							*/
#define	NISCA$K_SCS_DST_PROC	0x04		/* Body of CON_REQ: 16					*/
#define	NISCA$K_SCS_CON_RSP_LEN	22		/* LENGTH of a CON_RSP					*/

#define	NISCA$K_SCS_STNORMAL	1
#define	NISCA$K_SCS_STNOMAT	10		/* No listener: also the answer to VMS$LOOPBACK	*/

/*
**  Little-endian accessors: NISCA, SCS and MSCP are all little-endian, the
**  fields are not aligned, so they are moved octet-wise.
*/
static inline uint16_t	nisca$get16	(const uint8_t *a_p)	{ return (uint16_t) (a_p[0] | (a_p[1] << 8)); }
static inline uint32_t	nisca$get32	(const uint8_t *a_p)	{ return (uint32_t) nisca$get16(a_p) | ((uint32_t) nisca$get16(a_p + 2) << 16); }
static inline uint64_t	nisca$get64	(const uint8_t *a_p)	{ return (uint64_t) nisca$get32(a_p) | ((uint64_t) nisca$get32(a_p + 4) << 32); }

static inline void	nisca$put16	(uint8_t *a_p, uint16_t a_v)	{ a_p[0] = (uint8_t) a_v; a_p[1] = (uint8_t) (a_v >> 8); }
static inline void	nisca$put32	(uint8_t *a_p, uint32_t a_v)	{ nisca$put16(a_p, (uint16_t) a_v); nisca$put16(a_p + 2, (uint16_t) (a_v >> 16)); }
static inline void	nisca$put64	(uint8_t *a_p, uint64_t a_v)	{ nisca$put32(a_p, (uint32_t) a_v); nisca$put32(a_p + 4, (uint32_t) (a_v >> 32)); }

/*
**  The MAC address of the group multicast and of a node, in the order the
**  octets go on the wire.
*/
static inline void	nisca$group_mac	(
		uint16_t	a_group,
		uint8_t *	a_mac
			)
{
	nisca$put32(a_mac, NISCA$K_GROUP_LO);
	nisca$put16(a_mac + 4, (uint16_t) (NISCA$K_GROUP_BASE + a_group));
}

static inline void	nisca$node_mac	(
		uint16_t	a_sysid_lo,
		uint8_t *	a_mac
			)
{
	nisca$put32(a_mac, NISCA$K_NODE_LO);
	nisca$put16(a_mac + 4, a_sysid_lo);
}

#endif	/* __NISCA$H__ */
