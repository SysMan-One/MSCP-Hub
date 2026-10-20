#ifndef	__MHDEF$H__
#define	__MHDEF$H__	1

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_def.h
**
**  ABSTRACT:	Public definitions shared by the modules of the MSCPHUB
**		facility: the condition values, the configuration, the
**		context of the LAN port and of the nodes seen on it, and the
**		prototypes of the MH$ routines.
**
**  DESCRIPTION: The server is one process; the protocol, from the LAN
**		frame up to MSCP, runs in one thread on an EPOLL loop
**		(MSCPHUB.C), the way PEDRIVER runs in one fork thread per
**		port.  The layers, bottom up:
**
**		    MHLAN  - the AF_PACKET socket: frames in and out
**		    MHMSCP - the MSCP server: the commands, the units, the hosts
**		    MHBK   - the backend: the I/O of the units, in threads
**		    MHSCS  - SCS: the connections, credits, SCS$DIRECTORY
**		    MHBLK  - PPC block transfers: SNT_DAT, REQ_DAT, RET_DAT
**		    MHPPD  - PPD: the START/STACK/ACK handshake, HOSTSHUT
**		    MHTR   - NISCA transport: the virtual circuit
**		    MHCC   - NISCA channel control: HELLO, the nodes seen,
**			     the channel state machine
**		    MHUTL  - the message catalogue, VMS time, conversions
**
**		The server announces itself, listens, and refuses to run when
**		its identity clashes with a node already on the LAN.  The
**		channel to every node is opened by the CCSTART/VERF/VACK
**		handshake and kept by HELLO; over it go the virtual circuit and
**		the path of SCS (START/STACK/ACK), the SCS connections to
**		SCS$DIRECTORY and MSCP$DISK, and the data of the units over the
**		block transfers of PPC.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-12 : the first and the last sector of an RMW
**						are read at once.
**
**--
*/

#ifndef	__MODULE__
#define	__MODULE__	"MHDEF"
#endif

#ifndef	__IDENT__
#define	__IDENT__	"X01-12"
#endif

#ifndef	__REV__
#define	__REV__		"1.12.0"
#endif

#ifndef	__FAC__
#define	__FAC__		"MSCPHUB"
#endif

#include	<stdint.h>
#include	<stddef.h>

#include	"utility_routines.h"
#include	"starlet.h"

#include	"nisca.h"

#ifdef	__cplusplus
extern "C" {
#endif

/*
**  Facility number, see $DEFFAC/$DEFMSG/$DEFSEV in UTILITY_ROUTINES.H.
*/
#define	MH$K_FACNO		0x0121

#define	$MHSTS(msg, sev)	( $DEFFAC(MH$K_FACNO) | $DEFMSG(msg) | $DEFSEV(sev) )

enum	{
	MH$K_MSG_NORMAL		= 1,		/* Normal successful completion		*/
	MH$K_MSG_STARTUP,			/* Who we are, where			*/
	MH$K_MSG_LISTEN,			/* Silent period before the first HELLO	*/
	MH$K_MSG_ANNOUNCE,			/* The first HELLO is going out		*/
	MH$K_MSG_SHUTDOWN,			/* Stopped by a signal			*/
	MH$K_MSG_NOPARAM,			/* A mandatory parameter is missing	*/
	MH$K_MSG_IVPARAM,			/* Illegal value of a parameter		*/
	MH$K_MSG_SYSERR,			/* A system service failed		*/
	MH$K_MSG_NEWNODE,			/* A node seen on the LAN for the first time	*/
	MH$K_MSG_IDCLASH,			/* A node uses our system ID low word	*/
	MH$K_MSG_NAMECLASH,			/* A node uses our SCSNODE		*/
	MH$K_MSG_MANYNODES,			/* The table of the nodes is full	*/
	MH$K_MSG_STATS,				/* Counters, on SIGUSR1			*/
	MH$K_MSG_NOTOPIC,			/* /HELP names a topic the library has not	*/
	MH$K_MSG_CHOPEN,			/* The channel to a node is open		*/
	MH$K_MSG_CHCLOSE,			/* The channel to a node is closed		*/
	MH$K_MSG_AUTHFAIL,			/* A CC message with a wrong password		*/
	MH$K_MSG_VCOPEN,			/* The virtual circuit to a node is open	*/
	MH$K_MSG_VCCLOSE,			/* ... is closed				*/
	MH$K_MSG_TRSTATS,			/* Counters of the transport, on SIGUSR1	*/
	MH$K_MSG_CONOPEN,			/* An SCS connection is open			*/
	MH$K_MSG_CONCLOSE,			/* ... is closed				*/
	MH$K_MSG_UNITCFG,			/* A unit is served				*/
	MH$K_MSG_MSCPERR,			/* An END that could crash the client is held	*/
	MH$K_MSG_IOERR,				/* The backend failed a command			*/
	MH$K_MSG_BLKERR,			/* A block transfer chunk that matches nothing	*/
	MH$K_MSG_BLKSTATS,			/* Counters of the block transfers, on SIGUSR1	*/
	MH$K_MSG_UNSAFE,			/* /UNSAFE: no fdatasync			*/
	MH$K_MSG_MSCPSTATS,			/* Commands in progress, buffers, on SIGUSR1	*/
	MH$K_MSG_UNITSTATS,			/* Counters of a unit, on SIGUSR1		*/
	MH$K_MSG_BKSTATS,			/* ... of its device: RMW, ranges, group commit	*/
	MH$K_MSG_IOSTUCK,			/* A request is long in the backend		*/
	MH$K_MSG_DEVBUSY,			/* A block device is in use by someone else	*/
	MH$K_MSG_NODESTATS,			/* A node: its channel, circuit, connections	*/
	MH$K_MSG_UPTIME,			/* How long the server has run, on SIGUSR1	*/
	MH$K_MSG_ACKSTATS,			/* The ACKs sent, by why they went, on SIGUSR1	*/
	MH$K_MSG_LATSTATS,			/* The time of the commands of a unit, on SIGUSR1	*/

	MH$K_MSG_MAX				/* End-Of-List				*/
	};

#define	MH$_NORMAL		$MHSTS(MH$K_MSG_NORMAL,		STS$K_SUCCESS)
#define	MH$_STARTUP		$MHSTS(MH$K_MSG_STARTUP,	STS$K_INFO)
#define	MH$_LISTEN		$MHSTS(MH$K_MSG_LISTEN,		STS$K_INFO)
#define	MH$_ANNOUNCE		$MHSTS(MH$K_MSG_ANNOUNCE,	STS$K_INFO)
#define	MH$_SHUTDOWN		$MHSTS(MH$K_MSG_SHUTDOWN,	STS$K_INFO)
#define	MH$_NOPARAM		$MHSTS(MH$K_MSG_NOPARAM,	STS$K_FATAL)
#define	MH$_IVPARAM		$MHSTS(MH$K_MSG_IVPARAM,	STS$K_FATAL)
#define	MH$_SYSERR		$MHSTS(MH$K_MSG_SYSERR,		STS$K_FATAL)
#define	MH$_NEWNODE		$MHSTS(MH$K_MSG_NEWNODE,	STS$K_INFO)
#define	MH$_IDCLASH		$MHSTS(MH$K_MSG_IDCLASH,	STS$K_FATAL)
#define	MH$_NAMECLASH		$MHSTS(MH$K_MSG_NAMECLASH,	STS$K_FATAL)
#define	MH$_MANYNODES		$MHSTS(MH$K_MSG_MANYNODES,	STS$K_WARN)
#define	MH$_STATS		$MHSTS(MH$K_MSG_STATS,		STS$K_INFO)
#define	MH$_NOTOPIC		$MHSTS(MH$K_MSG_NOTOPIC,	STS$K_ERROR)
#define	MH$_CHOPEN		$MHSTS(MH$K_MSG_CHOPEN,		STS$K_INFO)
#define	MH$_CHCLOSE		$MHSTS(MH$K_MSG_CHCLOSE,	STS$K_WARN)
#define	MH$_AUTHFAIL		$MHSTS(MH$K_MSG_AUTHFAIL,	STS$K_WARN)
#define	MH$_VCOPEN		$MHSTS(MH$K_MSG_VCOPEN,		STS$K_INFO)
#define	MH$_VCCLOSE		$MHSTS(MH$K_MSG_VCCLOSE,	STS$K_WARN)
#define	MH$_TRSTATS		$MHSTS(MH$K_MSG_TRSTATS,	STS$K_INFO)
#define	MH$_CONOPEN		$MHSTS(MH$K_MSG_CONOPEN,	STS$K_INFO)
#define	MH$_CONCLOSE		$MHSTS(MH$K_MSG_CONCLOSE,	STS$K_INFO)
#define	MH$_UNITCFG		$MHSTS(MH$K_MSG_UNITCFG,	STS$K_INFO)
#define	MH$_MSCPERR		$MHSTS(MH$K_MSG_MSCPERR,	STS$K_ERROR)
#define	MH$_IOERR		$MHSTS(MH$K_MSG_IOERR,		STS$K_ERROR)
#define	MH$_BLKERR		$MHSTS(MH$K_MSG_BLKERR,		STS$K_WARN)
#define	MH$_BLKSTATS		$MHSTS(MH$K_MSG_BLKSTATS,	STS$K_INFO)
#define	MH$_UNSAFE		$MHSTS(MH$K_MSG_UNSAFE,		STS$K_WARN)
#define	MH$_MSCPSTATS		$MHSTS(MH$K_MSG_MSCPSTATS,	STS$K_INFO)
#define	MH$_UNITSTATS		$MHSTS(MH$K_MSG_UNITSTATS,	STS$K_INFO)
#define	MH$_BKSTATS		$MHSTS(MH$K_MSG_BKSTATS,	STS$K_INFO)
#define	MH$_IOSTUCK		$MHSTS(MH$K_MSG_IOSTUCK,	STS$K_WARN)
#define	MH$_DEVBUSY		$MHSTS(MH$K_MSG_DEVBUSY,	STS$K_FATAL)
#define	MH$_NODESTATS		$MHSTS(MH$K_MSG_NODESTATS,	STS$K_INFO)
#define	MH$_UPTIME		$MHSTS(MH$K_MSG_UPTIME,		STS$K_INFO)
#define	MH$_ACKSTATS		$MHSTS(MH$K_MSG_ACKSTATS,	STS$K_INFO)
#define	MH$_LATSTATS		$MHSTS(MH$K_MSG_LATSTATS,	STS$K_INFO)

/*
**  Sizes, limits and the timing of the protocol
*/
#define	MH$K_SZ_SCSNODE		8		/* SCSNODE as PPD START carries it, blank-padded	*/
#define	MH$K_SZ_AUTH		8		/* The cluster password hash			*/
#define	MH$K_SZ_IFNAME		16
#define	MH$K_MAXNODE		64		/* Nodes remembered from their HELLO		*/

#define	MH$K_TICK_MS		20		/* The TR tick, NISCA$C_TICKS_PER_SEC = 50	*/
#define	MH$K_TICKS_SEC		(1000 / MH$K_TICK_MS)
#define	MH$K_HELLO_MIN		76		/* HELLO every 1.52 ... 3 s, in ticks		*/
#define	MH$K_HELLO_MAX		150
#define	MH$K_DEF_LISTEN		4		/* Seconds of silence before the first HELLO:	*/
						/* longer than one HELLO interval of a VMS node	*/
#define	MH$K_LISTEN_TMO		(8 * MH$K_TICKS_SEC)	/* An open channel unheard of this long is closed	*/
#define	MH$K_HS_TMO		(5 * MH$K_TICKS_SEC)	/* The handshake of a channel must end in this	*/
#define	MH$K_DLY_MIN		25		/* HELLO of a new node to our CCSTART: 0.5 ... 2 s	*/
#define	MH$K_DLY_MAX		100

#define	MH$K_LAN_OVHD		72		/* PEDRIVER: data field = DLL size - 72		*/
#define	MH$K_PIPE_QUOTA		8		/* As the satellite's boot driver		*/

/*
**  The transport
*/
#define	MH$K_TR_WINDOW		8		/* Our transmit window at most			*/
#define	MH$K_TR_RCVCACHE	32		/* Receive cache: HSR+2 ... HSR+32		*/
#define	MH$K_TR_XMTQ		64		/* Messages waiting for the window; the chunks	*/
						/* of a block transfer are made when it opens	*/
#define	MH$K_TR_RTO		25		/* 500 ms: first retransmission timeout		*/
#define	MH$K_TR_RTO_MAX		150		/* 3 s, NISCA$C_MAX_SEQ_TMO			*/
#define	MH$K_TR_MAXREXMT	30		/* Timeouts of the head in a row to give up	*/
#define	MH$K_TR_ACKBATCH	4		/* /ACK=HOLD: packets taken that are ACKed at once	*/
#define	MH$K_SZ_TRPAY		(NISCA$K_MAXDATA - NISCA$K_SZ_DX - NISCA$K_TR_LEN_V15)	/* PPC opcode onwards	*/

/*
**  PPD START/STACK of the server.  SWTYPE is not 'VMS ':
**  a VMS node keeps its connection manager and its watchdog off us.
*/
#define	MH$K_PPD_MAXDG		576
#define	MH$K_PPD_MAXMSG		216
#define	MH$T_PPD_SWTYPE		"MSCH"
#define	MH$T_PPD_SWVERS		"X010"
#define	MH$T_PPD_HWTYPE		"LNX "

/*
**  SCS: the connections of a circuit
*/
#define	MH$K_MAXCDT		8		/* Connections on one circuit			*/
#define	MH$K_SZ_APPL		64		/* An application message we queue: END, answer	*/
#define	MH$K_CDT_XMTQ		16		/* ... waiting for a send credit		*/
#define	MH$K_DIR_CREDITS	8		/* SCS$DIRECTORY, as VMS			*/
#define	MH$K_MSCP_CREDITS	16		/* MSCP$DISK: the default			*/
#define	MH$K_MSCP_MAXCRED	64		/* ... /CREDITS at most				*/

/*
**  The MSCP server
*/
#define	MH$K_MAXUNIT		8		/* /UNIT0 ... /UNIT7				*/
#define	MH$K_MAXHOST		256		/* Host numbers of the connections		*/
#define	MH$K_MSCP_CNT_TMO	20		/* Controller timeout, s: the GUS of the client	*/
#define	MH$K_MSCP_MAXBCNT	0x20000		/* 128 KB, MAXBCNT and DMAXBCNT			*/
#define	MH$K_MSCP_LOAD		340		/* LOAD_AVAIL, as a VMS server of MSCP_LOAD 1	*/
#define	MH$K_MEDIA_RZ29		0x22E5A01DU	/* MEDIA_ID: D0 D1 = D K, RZ 29			*/
#define	MH$K_MEDIA_RRD42	0x22E5222AU	/* ... RRD 42, the CD-ROM of a unit RO		*/
#define	MH$K_MAXHRB		512		/* Commands in progress, all the hosts		*/
#define	MH$K_BUFPOOL		(16 * 1024 * 1024)	/* Data buffers at most; more wait (BUF_WAIT)	*/
#define	MH$K_BK_RING		1024		/* Entries of the io_uring			*/
#define	MH$K_BK_STUCK		15		/* s: a request longer in the backend - IOSTUCK	*/
#define	MH$K_BK_ZEROSZ		65536		/* Zeros written a piece at a time		*/

/*
**  What the CC messages of the server carry
*/
#define	MH$K_CC_MAINT		0x80000008U
#define	MH$K_CC_PORT_FCN	0x83FF0180U
#define	MH$K_CC_STA_INFO	0x00000400U
#define	MH$K_CC_MINOR		3		/* CC V1.3, as NISCA_BTDRIVER			*/
#define	MH$K_CC_HWTYPE		1		/* NODE_HW_TYPE: Alpha (display only)		*/

/*
**  The configuration, from the command line and the /CONFIG file
*/
typedef struct mh_cfg_t {
	ASC		conf;			/* /CONFIG					*/
	ASC		logfile;		/* /LOGFILE					*/
	ASC		ifname;			/* /INTERFACE					*/
	ASC		node;			/* /NODE: SCSNODE, 1 - 6 characters		*/
	ASC		auth;			/* /AUTHORIZE: the hash, 16 hex digits		*/
	int		sysid;			/* /SYSTEMID: SCSSYSTEMID, 1 - 65535 in v1	*/
	int		group;			/* /GROUP: cluster group number			*/
	int		listen;			/* /LISTEN: seconds before the first HELLO	*/
	int		passive;		/* /PASSIVE: never transmit			*/
	int		trace;			/* /TRACE					*/
	int		alloclass;		/* /ALLOCLASS: CNT_ALCS, 0 - 255		*/
	int		unsafe;			/* /UNSAFE: a WRITE ends before the data is on disk	*/
	ASC		sync;			/* /SYNC=DSYNC | GROUP | NONE			*/
	int		credits;		/* /CREDITS: of MSCP$DISK, 0 - the default	*/
	ASC		timezone;		/* /TIMEZONE=LOCAL | UTC | [+|-]hh[:mm]		*/
	ASC		ack;			/* /ACK=FLUSH | HOLD				*/
	ASC		unit [MH$K_MAXUNIT];	/* /UNITn=name,path[,RO][,MEDIA=m][,SECTOR=n]	*/
} MH$CFG;

/*
**  States of a channel, the subset of CH$W_STATE of PEDRIVER the server
**  goes through (the SOLICIT_* states are of the satellite boot service,
**  not here)
*/
enum	{
	MH$K_CH_CLOSED		= 0,
	MH$K_CH_CCSTART_SENT,			/* Our CCSTART is out, a VERF is awaited	*/
	MH$K_CH_CCSTART_REC,			/* Our VERF is out, a VACK is awaited		*/
	MH$K_CH_OPEN,
	MH$K_CH_HELLO_DLY,			/* A HELLO came in, our CCSTART is due		*/

	MH$K_CH_NSTATE				/* End-Of-List				*/
	};

/*
**  States of the path, PB$W_STATE: the server
**  answers the START of the VMS node, it never starts a path itself
*/
enum	{
	MH$K_PB_CLOSED		= 0,
	MH$K_PB_ST_REC,				/* Our STACK is out, an ACK is awaited		*/
	MH$K_PB_OPEN,

	MH$K_PB_NSTATE				/* End-Of-List				*/
	};

/*
**  States of an SCS connection, the subset of CDT$W_STATE the server goes
**  through as the acceptor
*/
enum	{
	MH$K_CDT_FREE		= 0,
	MH$K_CDT_CON_REC,			/* CON_RSP sent, our ACCP_REQ is due		*/
	MH$K_CDT_ACCP_SENT,			/* ... sent, an ACCP_RSP is awaited		*/
	MH$K_CDT_OPEN,
	MH$K_CDT_DISC_REC,			/* The peer's DISC_REQ answered, ours is due	*/
	MH$K_CDT_DISC_MTCH,			/* Ours sent, its DISC_RSP is awaited		*/
	MH$K_CDT_DISC_ACK,			/* Ours acknowledged, the peer's DISC_REQ is awaited	*/
	MH$K_CDT_DISC_SENT,			/* We disconnect: our DISC_REQ is due		*/

	MH$K_CDT_NSTATE				/* End-Of-List				*/
	};

enum	{
	MH$K_SYSAP_DIR		= 1,		/* SCS$DIRECTORY				*/
	MH$K_SYSAP_MSCP				/* MSCP$DISK					*/
	};

typedef struct mh_applmsg_t {
	uint16_t	len;
	uint8_t		data [MH$K_SZ_APPL];
} MH$APPLMSG;

/*
**  An SCS connection: CDT
*/
typedef struct mh_cdt_t {
	int		state;			/* MH$K_CDT_*					*/
	int		sysap;			/* MH$K_SYSAP_*					*/
	uint32_t	lconid;			/* Ours: index + 1, generation in the high word	*/
	uint32_t	rconid;			/* The peer's					*/
	int32_t		sendcr;			/* Messages we may send				*/
	uint16_t	pendrec;		/* Receive credits to give back			*/
	uint16_t	minsend;		/* MIN_CR of the CON_REQ			*/
	char		rproc [17];		/* The SYSAP of the peer, ASCIZ			*/
	int		peerdisc;		/* The peer's DISC_REQ has been answered	*/

	unsigned	nq, qhead;		/* Waiting for a send credit			*/
	MH$APPLMSG	q [MH$K_CDT_XMTQ];

	/*
	**  MSCP: the host this connection is
	*/
	int		host;			/* 0 - 255, -1 - none				*/
	uint16_t	hst_flgs, hst_tmo;
	uint32_t	ncmd;			/* Commands taken				*/
} MH$CDT;

/*
**  An I/O request to the backend (MHBK): made by the protocol thread, done
**  by io_uring, given back through the eventfd of the ring - all of it in
**  the protocol thread
*/
enum	{
	MH$K_BK_READ		= 1,
	MH$K_BK_WRITE,				/* ... made durable as /SYNC says		*/
	MH$K_BK_ZERO,				/* fallocate(ZERO_RANGE), or zeros written	*/
	MH$K_BK_SYNC				/* fdatasync of a group (MHBK only)		*/
	};

/*
**  /SYNC: when a WRITE or ERASE is on the medium
*/
enum	{
	MH$K_SYNC_DSYNC		= 0,		/* Every write RWF_DSYNC, ERASE fdatasync	*/
	MH$K_SYNC_GROUP,			/* One fdatasync for the writes done meanwhile	*/
	MH$K_SYNC_NONE				/* /UNSAFE: the page cache, for tests only	*/
	};

/*
**  /ACK: when the ACK owed for the packets taken goes alone
*/
enum	{
	MH$K_ACK_FLUSH		= 0,		/* When the LAN socket is drained		*/
	MH$K_ACK_HOLD				/* With the next packet, or on the 20 ms tick	*/
	};

/*
**  Why a bare ACK has gone, for ACKSTATS
*/
enum	{
	MH$K_ACKW_RSVP		= 0,		/* The peer asked for it			*/
	MH$K_ACKW_DUP,				/* A duplicate came				*/
	MH$K_ACKW_BATCH,			/* /ACK=HOLD: MH$K_TR_ACKBATCH packets owed	*/
	MH$K_ACKW_FLUSH,			/* /ACK=FLUSH: the socket is drained		*/
	MH$K_ACKW_TICK,				/* The 20 ms tick				*/

	MH$K_ACKW_N				/* End-Of-List				*/
	};

/*
**  The time of the commands that move data, by kind, for LATSTATS: from the
**  command to its END, and the steps in the backend and in the block
**  transfers in it - what is left is the queue of the unit and the server.
**  Nanoseconds of CLOCK_MONOTONIC.
*/
enum	{
	MH$K_LAT_READ		= 0,
	MH$K_LAT_WRITE,

	MH$K_LAT_N				/* End-Of-List				*/
	};

#define	MH$K_LAT_NHIST		6		/* Under 0.1, 0.3, 1, 3, 10 ms, longer		*/

typedef struct mh_lat_t {
	uint64_t	n;			/* Commands					*/
	uint64_t	sum, max;		/* From the command to the END			*/
	uint64_t	bk, xfer;		/* ... of it in the backend, in block transfers	*/
	uint64_t	hist [MH$K_LAT_NHIST];
} MH$LAT;

/*
**  Kinds of a unit
*/
enum	{
	MH$K_DEV_FILE		= 0,		/* A file: the page cache, flock()		*/
	MH$K_DEV_BLK				/* A block device: O_DIRECT, O_EXCL		*/
	};

struct mh_bkdev_t;
struct mh_port_t;

typedef struct mh_bkreq_t {
	struct mh_bkreq_t *next;		/* Lists of MHBK: done, waiting, group		*/
	struct mh_bkreq_t *anext;		/* The list of the ones in the backend		*/
	int		op;			/* MH$K_BK_*					*/
	struct mh_bkdev_t *dev;
	int		sync;			/* MH$K_SYNC_*					*/
	uint64_t	off;
	uint32_t	len;
	uint8_t *	buf;
	int		err;			/* Output: errno, 0 - done			*/
	void *		ctx;			/* The command it is of: MH$HRB			*/
	int		(*cb) (struct mh_port_t *, struct mh_bkreq_t *);	/* Given back here, NULL - MH$MSCP_IODONE	*/

	/*
	**  MHBK: the step, what the step has moved, the range of whole sectors
	**  of a device of sectors over 512 octets and its bounce buffer, when
	**  it went in (for IOSTUCK); of an RMW: what is read of the first and
	**  of the last sector, the reads in the ring, the first error of them
	*/
	int		stage, locked, stuck;
	uint32_t	done;
	uint32_t	edone [2];
	int		einfl, eerr;
	uint64_t	aoff;
	uint32_t	alen;
	uint8_t *	bounce;
	uint64_t	t0;
} MH$BKREQ;

/*
**  The device under a unit: what MHBK keeps of it
*/
typedef struct mh_bkdev_t {
	int		fd;			/* -1 - not open				*/
	int		type;			/* MH$K_DEV_*					*/
	uint32_t	ssz;			/* The sector: 512, 4096; a bigger one is RMW	*/

	/*
	**  The writes that hold a range of sectors and the ones that wait for
	**  it (a sector over 512 octets: read-modify-write)
	*/
	MH$BKREQ *	locks;
	MH$BKREQ *	lwhead;
	MH$BKREQ *	lwtail;

	/*
	**  Group commit: the writes done since the fdatasync in flight, the
	**  ones it covers
	*/
	MH$BKREQ *	gpend;
	MH$BKREQ *	ginfl;
	int		gbusy;
	MH$BKREQ	gsync;

	uint64_t	nrmw, nlockwait, nsync, nsyncw;
} MH$BKDEV;

/*
**  States of a command in progress: the step it waits for
*/
enum	{
	MH$K_HRB_NEW		= 0,
	MH$K_HRB_SEQ_WAIT,			/* The order of the unit holds it		*/
	MH$K_HRB_BUF_WAIT,			/* No buffer memory				*/
	MH$K_HRB_DRV_WAIT,			/* The backend has it				*/
	MH$K_HRB_SNDAT,				/* Its data goes to the client			*/
	MH$K_HRB_RETCNF,			/* ... sent, RET_CNF is awaited			*/
	MH$K_HRB_REQDAT,			/* The data of the client is asked, RET_DAT comes	*/

	MH$K_HRB_NSTATE				/* End-Of-List				*/
	};

/*
**  The steps of a command that moves data: READ = disk, out (, back, compare);
**  WRITE = in, disk (, disk, compare); COMPARE = in, disk, compare
*/
enum	{
	MH$K_PH_NONE		= 0,
	MH$K_PH_READ,				/* The disk is read into BUF			*/
	MH$K_PH_SEND,				/* BUF goes to the client			*/
	MH$K_PH_FETCH,				/* The client's data comes into BUF		*/
	MH$K_PH_WRITE,				/* BUF is written				*/
	MH$K_PH_ZERO,				/* ERASE					*/
	MH$K_PH_CMPDISK,			/* The disk is read into BUF2, to compare	*/
	MH$K_PH_CMPHOST				/* The client's data comes into BUF2		*/
	};

struct mh_node_t;
struct mh_cdt_t;
struct mh_unit_t;

/*
**  A command in progress of the MSCP server: HRB.  It is the identity of
**  its block transfers too - TAG goes in the XCT_ID and as the name of our
**  buffer.
*/
typedef struct mh_hrb_t {
	struct mh_hrb_t	*next;			/* The queue of the unit, or of BUF_WAIT	*/
	struct mh_hrb_t	*vcnext;		/* The block transfers of the circuit		*/
	int		state;			/* MH$K_HRB_*					*/
	int		phase;			/* MH$K_PH_*					*/
	unsigned	slot;			/* Index in the table of the port		*/
	uint32_t	tag;			/* Generation << 9 | slot			*/

	struct mh_node_t *node;
	struct mh_cdt_t	*cdt;
	uint32_t	lconid;			/* The connection is still the one it came on	*/
	struct mh_unit_t *unit;

	uint8_t		cmd [36];		/* The command: the end is built of it		*/
	uint8_t		op;
	uint16_t	mod;
	uint32_t	cmd_ref, bcnt, lbn;
	uint32_t	boff, bname;		/* The buffer of the client			*/
	uint32_t	cmd_sts;		/* GET COMMAND STATUS: down by 1 a step		*/

	uint8_t *	buf;			/* The data, BCNT rounded up to 4096		*/
	uint8_t *	buf2;			/* ... to compare with				*/
	uint32_t	bufsz;
	uint32_t	moved;			/* Octets sent or taken in this step		*/
	uint32_t	done;			/* BYTE_CNT of the end				*/

	int		aborted;		/* ABORT has come for it			*/
	int		orphan;			/* The connection is gone, the backend has it	*/
	int		cmpfail;		/* COMPARE: the data differ			*/

	uint64_t	t_cmd;			/* When the command came, ns			*/
	uint64_t	t_step;			/* When its step in the backend or a transfer began	*/
	uint64_t	t_bk, t_xfer;		/* The time of its steps so far			*/

	MH$BKREQ	bk;
} MH$HRB;

/*
**  A packet of the transport: from the PPC opcode on
*/
typedef struct mh_trpkt_t {
	uint16_t	len;
	uint32_t	seq;			/* Its number; the receive cache: of the slot	*/
	int		valid;			/* ... it holds one				*/
	uint8_t		data [MH$K_SZ_TRPAY];
} MH$TRPKT;

/*
**  The virtual circuit to a node and the SCS system of it, SB.  Allocated at the first START of the node, reset - not
**  freed - when the circuit closes.
*/
typedef struct mh_vc_t {
	int		pbstate;		/* MH$K_PB_*					*/
	uint32_t	gen;			/* ++ at every reset: a loop over the cache stops	*/

	uint32_t	nsu;			/* Next sequence number to use, 32 bits (TR V1.4)	*/
	uint32_t	lar;			/* Last ACK received				*/
	uint32_t	hsr;			/* Highest in order received: our ACK		*/
	unsigned	window, maxwin;
	unsigned	rto, retries;
	uint64_t	due;			/* Tick the head of the transmit cache times out	*/
	int		sack;			/* ACK owed: 1 - on the next tick, 2 - now	*/
	int		recover;		/* A timeout: a partial ACK resends the head	*/
	unsigned	nowed;			/* Packets taken since our last ACK		*/

	unsigned	nxmt;			/* Sent and not acknowledged: LAR+1 ...	*/
	MH$TRPKT	xmt [MH$K_TR_WINDOW];
	unsigned	nq, qhead;		/* Waiting for the window			*/
	MH$TRPKT	q [MH$K_TR_XMTQ];
	MH$TRPKT	rcv [MH$K_TR_RCVCACHE];	/* Out of order, slot = SEQ % size		*/

	/*
	**  What the START of the node told: its SB
	*/
	uint64_t	sysid;			/* 48 bits					*/
	uint16_t	maxdg, maxmsg;
	char		swtype [5], swvers [5], hwtype [5];
	uint64_t	incarn;
	uint32_t	opens;

	/*
	**  The connections: one request of ours is out at a time
	*/
	MH$CDT		cdt [MH$K_MAXCDT];
	uint16_t	cdtgen;
	int		reqcdt;			/* Index + 1 of the CDT our request is of	*/

	/*
	**  Block transfers waiting to go: the chunks of the head are made as
	**  the window opens, one transfer after the other (PEDRIVER requeues
	**  the one it has begun at the head of its command queue)
	*/
	MH$HRB *	blkhead;
	MH$HRB *	blktail;
} MH$VC;

/*
**  A node seen on the LAN, and our channel to it: the server has one
**  adapter, so a node has one channel.  An entry is kept when the node
**  falls silent - the channel sequence number must not start over, the
**  peer puts it in every TR packet it sends us.
*/
typedef struct mh_node_t {
	uint8_t		mac [NISCA$K_SZ_MAC];	/* Its adapter					*/
	uint16_t	sysid;			/* DX source: low word of its SCSSYSTEMID	*/
	uint8_t		minor;			/* CC version it speaks				*/
	uint8_t		cclen;
	char		name [NISCA$K_SZ_NODENAME + 1];	/* ASCIZ, trailing blanks dropped	*/
	uint32_t	hellos;
	uint64_t	last;			/* Tick of the last frame			*/

	int		state;			/* MH$K_CH_*					*/
	uint64_t	due;			/* Tick of the handshake timer, 0 - not running	*/
	uint16_t	lcl_chseq;		/* LCL_CHAN_SEQ_NUM of our CC, never 0		*/
	uint16_t	rmt_chseq;		/* ... of the peer: RMT_CHAN_SEQ_NUM of our TR	*/
	uint8_t		pipe_quota;		/* What the peer told in its CCSTART/VERF/VACK	*/
	uint16_t	bufsz;			/* BUS_BUFFER_SIZE, 0 - not told (V1.1)		*/
	uint16_t	p_srv, r_srv;
	uint32_t	opens;			/* Times the channel came open			*/
	int		authfail;		/* AUTHFAIL has been reported for this node	*/

	MH$VC *		vc;			/* NULL - never had a START			*/
} MH$NODE;

/*
**  A unit of the MSCP server
*/
typedef struct mh_unit_t {
	char		name [8];		/* DKA100, ASCIZ				*/
	char		path [256];
	int		ro;
	uint32_t	size;			/* UNT_SIZE: blocks of 512			*/
	uint8_t		unitid [8];		/* UNIT_ID: allocation class, number, name	*/
	uint32_t	media;			/* MEDIA_ID					*/
	uint32_t	volser;
	uint8_t		online [MH$K_MAXHOST / 8];	/* The hosts it is ONLINE for		*/
	char		ddr [8];		/* The name of the medium: RZ29, ASCIZ (GTUNM)	*/
	char		dvname [16];		/* $42$DKA100, MHUB$DKA100 with class 0, ASCIZ	*/
	MH$BKDEV	dev;			/* The file or the device			*/

	/*
	**  The order of the commands: how many run, a sequential
	**  one runs, the ones that wait for their turn
	*/
	unsigned	current;
	int		seq;
	MH$HRB *	qhead;
	MH$HRB *	qtail;

	uint64_t	nread, nwrite, rbytes, wbytes;
	MH$LAT		lat [MH$K_LAT_N];
} MH$UNIT;

/*
**  The LAN port: one adapter
*/
typedef struct mh_port_t {
	int		sd;			/* AF_PACKET socket				*/
	int		ifindex;
	int		mtu;
	char		ifname [MH$K_SZ_IFNAME];
	uint8_t		mac [NISCA$K_SZ_MAC];	/* Our adapter					*/
	uint8_t		gmac [NISCA$K_SZ_MAC];	/* The group multicast				*/
	uint16_t	group;
	uint16_t	sysid;			/* Low word of SCSSYSTEMID: the DX address	*/
	uint8_t		auth [MH$K_SZ_AUTH];
	uint8_t		ccname [1 + NISCA$K_SZ_NODENAME];	/* ASCIC, blank-padded		*/
	uint8_t		devname [NISCA$K_SZ_DEVNAME];	/* ASCIC name of the adapter		*/
	uint16_t	bufsz;			/* BUS_BUFFER_SIZE				*/
	uint64_t	incarn;			/* SWINCARN: VMS time we started		*/
	int		passive;
	int		trace;

	uint64_t	tick;			/* The 20 ms clock				*/
	uint64_t	next_hello;
	int		announcing;		/* The listen period is over			*/

	unsigned	nnode;
	MH$NODE		nodes [MH$K_MAXNODE];

	uint64_t	rx_frames, rx_bytes, rx_cc, rx_tr, rx_drop, rx_own, rx_auth;
	uint64_t	tx_frames, tx_bytes, tx_err;
	uint64_t	tr_tx, tr_rexmt, tr_rx, tr_dup, tr_ooo, tr_old;
	int		ackmode;		/* MH$K_ACK_*					*/
	uint64_t	tr_rsvp;		/* Packets taken that asked for an ACK		*/
	uint64_t	tr_ackpig;		/* ACKs owed paid by a packet of ours		*/
	uint64_t	tr_ackbare [MH$K_ACKW_N];	/* ... by a bare ACK, by why		*/

	/*
	**  The MSCP server
	*/
	uint8_t		alloclass;
	unsigned	nunit;
	MH$UNIT		units [MH$K_MAXUNIT];
	uint8_t		hosts [MH$K_MAXHOST / 8];	/* Host numbers in use			*/
	unsigned	hostseed;
	int		sync;			/* MH$K_SYNC_*					*/
	uint16_t	credits;		/* We give a client of MSCP$DISK		*/

	MH$HRB *	hrb [MH$K_MAXHRB];	/* Commands in progress, by slot		*/
	unsigned	nhrb, hrbseed;
	uint32_t	hrbgen;
	uint64_t	bufused;		/* Octets of data buffers			*/
	MH$HRB *	bwhead;			/* Waiting for buffer memory			*/
	MH$HRB *	bwtail;

	uint64_t	blk_snt, blk_req, blk_ret, blk_cnf, blk_bad, blk_bytes_out, blk_bytes_in;
	unsigned	tr_maxwin;		/* The widest the window has been		*/

	/*
	**  The tests take the frames here instead of the socket
	*/
	int		(*xmit) (struct mh_port_t *a_port, const uint8_t *a_frame, unsigned a_len);
} MH$PORT;

/*
**  MHUTL.C
*/
int	mh$inimsg	(void);
int	mh$vmstime	(uint64_t *a_vmstime);
int	mh$tz_set	(const char *a_spec);
int	mh$tz_name	(char *a_buf, unsigned a_bufsz);
int	mh$unix2vms	(int64_t a_sec, int64_t a_nsec, uint64_t *a_vmstime);
int	mh$hex2bin	(const char *a_hex, unsigned a_hexlen, uint8_t *a_bin, unsigned a_binsz);
int	mh$mac2str	(const uint8_t *a_mac, char *a_out, unsigned a_outsz);
uint64_t mh$now_ns	(void);

#define	$MHMSG(a_sts, ...)	$PUTMSG_FAO((a_sts), ##__VA_ARGS__)

/*
**  MHHLP.C
*/
int	mh$help		(const char *a_topic);

/*
**  MHLAN.C
*/
int	mh$lan_open	(MH$PORT *a_port, const char *a_ifname);
int	mh$lan_join	(MH$PORT *a_port, const uint8_t *a_mac);
int	mh$lan_close	(MH$PORT *a_port);
int	mh$lan_send	(MH$PORT *a_port, const uint8_t *a_dst, const uint8_t *a_data, unsigned a_len);
int	mh$lan_recv	(MH$PORT *a_port, uint8_t *a_frame, unsigned a_frmsz, unsigned *a_len);

/*
**  MHCC.C
*/
int	mh$cc_init	(MH$PORT *a_port, const MH$CFG *a_cfg);
int	mh$cc_build	(MH$PORT *a_port, unsigned a_type, uint16_t a_chseq, uint8_t *a_cc, unsigned a_ccsz, unsigned *a_len);
int	mh$cc_tick	(MH$PORT *a_port);
int	mh$cc_shutdown	(MH$PORT *a_port);
int	mh$cc_input	(MH$PORT *a_port, const uint8_t *a_frame, unsigned a_len);
int	mh$dx_build	(const MH$PORT *a_port, const uint8_t *a_dxdst, uint8_t *a_dx);
int	mh$cc_restart	(MH$PORT *a_port, MH$NODE *a_node, const char *a_why);

/*
**  MHTR.C
*/
int	mh$tr_open	(MH$PORT *a_port, MH$NODE *a_node);
int	mh$tr_reset	(MH$PORT *a_port, MH$NODE *a_node);
int	mh$tr_input	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_tr, unsigned a_len);
int	mh$tr_tick	(MH$PORT *a_port, MH$NODE *a_node);
int	mh$tr_flush	(MH$PORT *a_port);
int	mh$tr_send_dg	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_ppd, unsigned a_len);
int	mh$tr_send_seq	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_ppd, unsigned a_len);
int	mh$tr_send_ppc	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_pay, unsigned a_len);
int	mh$tr_kick	(MH$PORT *a_port, MH$NODE *a_node);

/*
**  MHBLK.C
*/
int	mh$blk_send	(MH$PORT *a_port, MH$HRB *a_hrb);
int	mh$blk_request	(MH$PORT *a_port, MH$HRB *a_hrb);
int	mh$blk_chunk	(MH$PORT *a_port, MH$NODE *a_node, uint8_t *a_pay, unsigned *a_len);
int	mh$blk_input	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_pay, unsigned a_len);
int	mh$blk_cancel	(MH$PORT *a_port, MH$HRB *a_hrb);

/*
**  MHPPD.C
*/
int	mh$ppd_input	(MH$PORT *a_port, MH$NODE *a_node, unsigned a_opc, const uint8_t *a_ppd, unsigned a_len);
int	mh$ppd_close	(MH$PORT *a_port, MH$NODE *a_node, const char *a_why);
int	mh$ppd_shutdown	(MH$PORT *a_port);

/*
**  MHSCS.C
*/
int	mh$scs_input	(MH$PORT *a_port, MH$NODE *a_node, const uint8_t *a_scs, unsigned a_len);
int	mh$scs_reset	(MH$PORT *a_port, MH$NODE *a_node);
int	mh$scs_send	(MH$PORT *a_port, MH$NODE *a_node, MH$CDT *a_cdt, const uint8_t *a_msg, unsigned a_len);
int	mh$scs_disconnect (MH$PORT *a_port, MH$NODE *a_node, MH$CDT *a_cdt, const char *a_why);

/*
**  MHMSCP.C
*/
int	mh$mscp_init	(MH$PORT *a_port, const MH$CFG *a_cfg);
int	mh$mscp_connect	(MH$PORT *a_port, MH$NODE *a_node, MH$CDT *a_cdt);
int	mh$mscp_disconnect (MH$PORT *a_port, MH$NODE *a_node, MH$CDT *a_cdt);
int	mh$mscp_stats	(MH$PORT *a_port);
const char *mh$mscp_opname (const MH$HRB *a_hrb);
int	mh$mscp_input	(MH$PORT *a_port, MH$NODE *a_node, MH$CDT *a_cdt, const uint8_t *a_cmd, unsigned a_len);
int	mh$mscp_xferdone (MH$PORT *a_port, MH$HRB *a_hrb, int a_ok);
int	mh$mscp_iodone	(MH$PORT *a_port, MH$HRB *a_hrb);
MH$HRB *mh$mscp_hrb	(MH$PORT *a_port, MH$NODE *a_node, uint32_t a_lconid, uint32_t a_tag);

/*
**  MHBK.C
*/
int	mh$bk_init	(void);
int	mh$bk_open	(MH$BKDEV *a_dev, const char *a_path, int a_ro, uint32_t a_ssz, uint64_t *a_size);
int	mh$bk_fd	(void);
int	mh$bk_submit	(MH$BKREQ *a_req);
int	mh$bk_reap	(MH$PORT *a_port);
int	mh$bk_wait	(MH$PORT *a_port, int a_ms);
int	mh$bk_tick	(MH$PORT *a_port);

#ifdef	__cplusplus
}
#endif

#endif	/* __MHDEF$H__ */
