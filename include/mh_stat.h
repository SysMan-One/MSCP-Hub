#ifndef	__MHSTAT$H__
#define	__MHSTAT$H__	1

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_stat.h
**
**  ABSTRACT:	The statistics block of the server: what the server publishes in
**		shared memory for MHMON - who it is, the nodes, the units and the
**		counters - and the protocol of reading it.
**
**  DESCRIPTION: The server is the only writer.  Every 100 ms (5 ticks) it
**		makes the block odd (SEQ + 1), copies its state in, and makes
**		it even again (SEQ + 1); a reader copies the block between two
**		reads of SEQ and takes the copy only when SEQ was even and did
**		not change.  Neither side ever waits for the other: the server
**		does not know that anybody reads.
**
**		The block is a POSIX shared memory object /mscphub.<SCSNODE>
**		(/dev/shm/mscphub.MHUB), mode 0640, made anew when the server
**		starts and removed when it stops.  A block left by a server that
**		was killed stays: PID tells a reader whether its writer lives.
**
**		Every field has its size in its type, no pointers: the layout is
**		the interface between two images, VERSION says which one.  A
**		change of the layout raises MH$K_STAT_VERSION.
**
**		Every counter only grows for the life of the server; a reader
**		takes rates as the difference of two copies over the difference
**		of their NOW_NS, the clock of the server.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  22-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		22-OCT-2026	RRL	X01-17 : Initial revision.
**		25-OCT-2026	RRL	X01-21 : SOLICIT_DLY, START_SENT: new values, the layout is the same.
**		12-NOV-2026	CLD	X.01-24ECO03 : IDENT of 16 octets (X.01-24ECO03 did not fit in 8);
**						the layout changes - MH$K_STAT_VERSION 2.
**
**--
*/

#ifndef	__MODULE__
#define	__MODULE__	"MHSTAT"
#endif

#ifndef	__IDENT__
#define	__IDENT__	"X.01-24ECO03"
#endif

#ifndef	__REV__
#define	__REV__		"1.24.3"
#endif

#include	<stdint.h>

#define	MH$T_STAT_MAGIC		"MSCPHUB"	/* 8 octets with the NUL			*/
#define	MH$K_STAT_VERSION	2		/* 2 - X.01-24ECO03: IDENT of 16 octets	*/
#define	MH$T_STAT_PREFIX	"/mscphub."	/* + the SCSNODE: the name of the object	*/
#define	MH$T_STAT_SHMDIR	"/dev/shm"	/* Where Linux keeps the objects		*/
#define	MH$K_STAT_MODE		0640

#define	MH$K_STAT_TICKS		5		/* The block is copied in every 5 ticks: 100 ms	*/
#define	MH$K_STAT_TRIES		1000		/* A reader tries this often for a whole copy	*/

#define	MH$K_STAT_MAXNODE	64		/* = MH$K_MAXNODE				*/
#define	MH$K_STAT_MAXUNIT	8		/* = MH$K_MAXUNIT				*/

/*
**  States of a channel and of a circuit, as the block tells them
*/
enum	{
	MH$K_STCH_CLOSED	= 0,
	MH$K_STCH_CCSTART_SENT,
	MH$K_STCH_CCSTART_REC,
	MH$K_STCH_OPEN,
	MH$K_STCH_HELLO_REC,
	MH$K_STCH_SOLICIT_DLY			/* A SOLICIT_SRV came in, our VERF is due	*/
	};

enum	{
	MH$K_STVC_NONE		= 0,		/* Never started				*/
	MH$K_STVC_CLOSED,
	MH$K_STVC_STACK_SENT,
	MH$K_STVC_OPEN,
	MH$K_STVC_START_SENT			/* Ours, to a boot driver			*/
	};

/*
**  A node seen on the LAN
*/
typedef struct mh_stnode_t {
	char		name [8];		/* SCSNODE, ASCIZ				*/
	char		swtype [8];		/* What its START told: VMS, V8.3, ALPH		*/
	char		swvers [8];
	char		hwtype [8];
	uint8_t		mac [8];		/* Its adapter, 6 octets			*/
	uint16_t	sysid;			/* Low word of its SCSSYSTEMID			*/
	uint8_t		chstate;		/* MH$K_STCH_*					*/
	uint8_t		vcstate;		/* MH$K_STVC_*					*/
	uint8_t		minor;			/* CC V1.n					*/
	uint8_t		nconn;			/* SCS connections open				*/
	uint8_t		nmscp;			/* ... of them to MSCP$DISK			*/
	uint8_t		pad;
	uint32_t	chopens;		/* Times the channel came open			*/
	uint32_t	vcopens;		/* ... the circuit				*/
	uint64_t	ncmd;			/* MSCP commands taken on its connections now	*/
} MH$STNODE;

/*
**  A unit of the MSCP server
*/
typedef struct mh_stunit_t {
	char		name [8];		/* DKA100, ASCIZ				*/
	char		dvname [16];		/* $42$DKA100					*/
	char		media [8];		/* RZ29						*/
	char		path [256];
	uint32_t	size;			/* Blocks of 512				*/
	uint32_t	ssz;			/* The sector of the Linux device		*/
	uint8_t		ro;
	uint8_t		type;			/* 0 - a file, 1 - a block device		*/
	uint8_t		direct;			/* Direct I/O					*/
	uint8_t		nonline;		/* Hosts it is ONLINE to			*/
	uint32_t	current;		/* Commands running now				*/
	uint32_t	waiting;		/* ... waiting for their turn			*/
	uint64_t	online;			/* Bit n: node n of NODES has it ONLINE		*/
	uint64_t	qint;			/* Sum over the ticks of current + waiting	*/
	uint64_t	nread, nwrite;
	uint64_t	rbytes, wbytes;
	uint64_t	latn [2];		/* READ, WRITE: commands			*/
	uint64_t	latsum [2];		/* ... ns from the command to its end		*/
	uint64_t	latbk [2];		/* ... of it in the Linux disk			*/
	uint64_t	latxfer [2];		/* ... of it in block transfers			*/
	uint64_t	nrmw, ncache;		/* Read-modify-writes, sectors from the cache	*/
	uint64_t	nsync, nsyncw;		/* /SYNC=GROUP: fdatasyncs, writes they covered	*/
	uint64_t	nzero;			/* ERASE					*/
} MH$STUNIT;

/*
**  The block
*/
typedef struct mh_stat_t {
	char		magic [8];		/* MH$T_STAT_MAGIC				*/
	uint32_t	version;		/* MH$K_STAT_VERSION				*/
	uint32_t	size;			/* sizeof(MH$STAT)				*/
	uint32_t	seq;			/* Odd - being written				*/
	uint32_t	pid;			/* Of the server				*/

	int64_t		started;		/* When the server started, s of the epoch	*/
	int64_t		wall;			/* When this copy was made, s of the epoch	*/
	uint64_t	now_ns;			/* ... CLOCK_MONOTONIC, ns			*/
	uint64_t	tick;			/* ... the tick of the server			*/
	uint64_t	qticks;			/* Ticks the queues of the units are summed over	*/

	char		ident [16];		/* X.01-24ECO03, ASCIZ				*/
	char		node [8];		/* SCSNODE					*/
	char		ifname [16];
	uint16_t	sysid;
	uint16_t	group;
	uint8_t		alloclass;
	uint8_t		passive;
	uint8_t		sync;			/* MH$K_SYNC_*					*/
	uint8_t		ackmode;		/* MH$K_ACK_*					*/

	/*
	**  The port and the transport
	*/
	uint64_t	rx_frames, rx_bytes, rx_cc, rx_tr, rx_drop, rx_own, rx_auth;
	uint64_t	tx_frames, tx_bytes, tx_err;
	uint64_t	tr_tx, tr_rexmt, tr_rx, tr_dup, tr_ooo, tr_old;
	uint64_t	tr_rsvp, tr_ackpig, tr_ackbare;
	uint64_t	blk_snt, blk_req, blk_ret, blk_cnf, blk_bad, blk_bytes_out, blk_bytes_in;

	/*
	**  The MSCP server
	*/
	uint64_t	bufused;		/* Octets of data buffers			*/
	uint32_t	nhrb;			/* Commands in progress				*/
	uint32_t	tr_maxwin;

	uint32_t	nnode;
	uint32_t	nunit;
	MH$STNODE	nodes [MH$K_STAT_MAXNODE];
	MH$STUNIT	units [MH$K_STAT_MAXUNIT];
} MH$STAT;

int	mh$stat_read	(const MH$STAT *a_blk, MH$STAT *a_copy);
const	MH$STAT *	mh$stat_block	(void);

#endif	/* __MHSTAT$H__ */
