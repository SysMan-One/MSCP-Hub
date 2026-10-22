#define	__MODULE__	"MHSTAT"
#define	__IDENT__	"X01-17"
#define	__REV__		"1.17.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_stat.c
**
**  ABSTRACT:	The statistics block: the state and the counters of the server,
**		published in shared memory for MHMON.
**
**  DESCRIPTION: The block is made when the server starts and copied in on
**		tenth of a second, as MH_STAT.H describes: SEQ odd, the copy, SEQ
**		even.  The copy is a few kilobytes 10 times a second, and nothing in it
**		ever waits - the protocol thread does not know whether anybody
**		reads.
**
**		A block that cannot be made is no reason to stop: the server
**		says NOSTAT and serves its disks without it.
**
**		The queue of a unit is summed on every tick (QINT), so that
**		MHMON takes the average length of the queue over its interval,
**		not what it happened to be at the moment it looked.
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

#include	<errno.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<time.h>
#include	<sched.h>
#include	<sys/mman.h>
#include	<sys/stat.h>

#include	"mh_def.h"
#include	"mh_stat.h"

_Static_assert(MH$K_STAT_MAXNODE == MH$K_MAXNODE, "the block must hold every node");
_Static_assert(MH$K_STAT_MAXUNIT == MH$K_MAXUNIT, "the block must hold every unit");
_Static_assert(MH$K_STAT_MAXNODE <= 64, "ONLINE of a unit is a mask of 64 nodes");

static	MH$STAT	*s_stat;			/* The block, NULL - not published	*/
static	char	s_name [32];			/* Its name: /mscphub.MHUB		*/
static	uint64_t s_qticks;			/* Ticks the queues have been summed	*/



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make the statistics block: create the shared memory object, give it
**	its size, map it, and put in what does not change - who the server
**	is.  A block left by an earlier server of the same name is made anew.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cfg		The configuration
**	a_ident		The IDENT of the product
**
**  IMPLICIT OUTPUTS:
**	S_STAT, S_NAME.
**
**  RETURN VALUE:
**	STS$K_SUCCESS - published; MH$_NOSTAT (severity W, signalled) - the
**	server runs without it.
**--
*/
int	mh$stat_open	(
		MH$PORT *	a_port,
	const	MH$CFG *	a_cfg,
	const	char *	a_ident
			)
{
MH$STAT	*l_stat;
int	l_fd, l_err;

	if ( sizeof(MH$T_STAT_PREFIX) + $ASCLEN(&a_cfg->node) > sizeof(s_name) )
		return	$MHMSG(MH$_NOSTAT, "the name of the block", ENAMETOOLONG, strerror(ENAMETOOLONG));

	memcpy(s_name, MH$T_STAT_PREFIX, sizeof(MH$T_STAT_PREFIX) - 1);
	memcpy(s_name + sizeof(MH$T_STAT_PREFIX) - 1, $ASCPTR(&a_cfg->node), $ASCLEN(&a_cfg->node));
	s_name[sizeof(MH$T_STAT_PREFIX) - 1 + $ASCLEN(&a_cfg->node)] = '\0';

	if ( 0 > (l_fd = shm_open(s_name, O_CREAT | O_RDWR | O_CLOEXEC, MH$K_STAT_MODE)) )
		return	$MHMSG(MH$_NOSTAT, "shm_open()", errno, strerror(errno));

	/*
	**  The mode of O_CREAT is cut by the umask, and an old object keeps
	**  the mode it had: both are set here
	*/
	if ( fchmod(l_fd, MH$K_STAT_MODE) || ftruncate(l_fd, 0) || ftruncate(l_fd, sizeof(MH$STAT)) )
		{
		l_err = errno;
		close(l_fd);
		shm_unlink(s_name);
		return	$MHMSG(MH$_NOSTAT, "ftruncate()", l_err, strerror(l_err));
		}

	l_stat = mmap(NULL, sizeof(MH$STAT), PROT_READ | PROT_WRITE, MAP_SHARED, l_fd, 0);
	l_err = errno;
	close(l_fd);

	if ( l_stat == MAP_FAILED )
		{
		shm_unlink(s_name);
		return	$MHMSG(MH$_NOSTAT, "mmap()", l_err, strerror(l_err));
		}

	memcpy(l_stat->magic, MH$T_STAT_MAGIC, sizeof(l_stat->magic));
	l_stat->version	= MH$K_STAT_VERSION;
	l_stat->size	= sizeof(MH$STAT);
	l_stat->pid	= (uint32_t) getpid();
	l_stat->started	= (int64_t) time(NULL);

	strncpy(l_stat->ident, a_ident, sizeof(l_stat->ident) - 1);
	strncpy(l_stat->node, $ASCPTR(&a_cfg->node), sizeof(l_stat->node) - 1);
	strncpy(l_stat->ifname, a_port->ifname, sizeof(l_stat->ifname) - 1);
	l_stat->sysid	= a_port->sysid;
	l_stat->group	= a_port->group;
	l_stat->alloclass = a_port->alloclass;
	l_stat->passive	= (uint8_t) a_port->passive;
	l_stat->sync	= (uint8_t) a_port->sync;
	l_stat->ackmode	= (uint8_t) a_port->ackmode;

	s_stat = l_stat;
	s_qticks = MH$K_STAT_TICKS - 1;		/* The first tick copies			*/

	return	mh$stat_tick(a_port);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Copy a node into the block: its identity, the state of its channel
**	and circuit, its connections and the MSCP commands they have taken.
**
**  FORMAL PARAMETERS:
**
**	a_node		The node
**	a_st		Its entry in the block
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$stat_node	(
	const	MH$NODE *	a_node,
		MH$STNODE *	a_st
			)
{
const	MH$VC	*l_vc = a_node->vc;

	memcpy(a_st->name, a_node->name, sizeof(a_node->name));
	memcpy(a_st->mac, a_node->mac, NISCA$K_SZ_MAC);
	a_st->sysid	= a_node->sysid;
	a_st->chstate	= (uint8_t) a_node->state;
	a_st->minor	= a_node->minor;
	a_st->chopens	= a_node->opens;
	a_st->nconn	= 0;
	a_st->nmscp	= 0;
	a_st->ncmd	= 0;

	if ( !l_vc )
		{
		a_st->vcstate	= MH$K_STVC_NONE;
		a_st->vcopens	= 0;
		return	STS$K_SUCCESS;
		}

	a_st->vcstate	= (uint8_t) (MH$K_STVC_CLOSED + l_vc->pbstate);
	a_st->vcopens	= l_vc->opens;
	memcpy(a_st->swtype, l_vc->swtype, sizeof(l_vc->swtype));
	memcpy(a_st->swvers, l_vc->swvers, sizeof(l_vc->swvers));
	memcpy(a_st->hwtype, l_vc->hwtype, sizeof(l_vc->hwtype));

	for ( int j = 0; j < MH$K_MAXCDT; j++ )
		{
		const	MH$CDT	*l_cdt = &l_vc->cdt[j];

		if ( l_cdt->state != MH$K_CDT_OPEN )
			continue;

		a_st->nconn++;

		if ( l_cdt->sysap == MH$K_SYSAP_MSCP )
			{
			a_st->nmscp++;
			a_st->ncmd += l_cdt->ncmd;
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Copy a unit into the block: what it is, its queue, its counters, and
**	which nodes have it ONLINE - the hosts of the unit are the MSCP
**	connections of the nodes, so a host is found back to its node here.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_unit		The unit
**	a_st		Its entry in the block
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$stat_unit	(
	const	MH$PORT *	a_port,
	const	MH$UNIT *	a_unit,
		MH$STUNIT *	a_st
			)
{
const	MH$HRB	*l_hrb;
uint32_t	l_wait = 0;

	memcpy(a_st->name, a_unit->name, sizeof(a_st->name));
	memcpy(a_st->dvname, a_unit->dvname, sizeof(a_st->dvname));
	memcpy(a_st->media, a_unit->ddr, sizeof(a_st->media));
	memcpy(a_st->path, a_unit->path, sizeof(a_st->path));
	a_st->size	= a_unit->size;
	a_st->ssz	= a_unit->dev.ssz;
	a_st->ro	= (uint8_t) a_unit->ro;
	a_st->type	= (uint8_t) a_unit->dev.type;
	a_st->direct	= (uint8_t) a_unit->dev.direct;

	for ( l_hrb = a_unit->qhead; l_hrb; l_hrb = l_hrb->next )
		l_wait++;

	a_st->current	= a_unit->current;
	a_st->waiting	= l_wait;
	a_st->qint	= a_unit->qint;

	a_st->online	= 0;
	a_st->nonline	= 0;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		{
		const	MH$VC	*l_vc = a_port->nodes[i].vc;

		if ( !l_vc )
			continue;

		for ( int j = 0; j < MH$K_MAXCDT; j++ )
			{
			const	MH$CDT	*l_cdt = &l_vc->cdt[j];

			if ( (l_cdt->state != MH$K_CDT_OPEN) || (l_cdt->sysap != MH$K_SYSAP_MSCP) || (l_cdt->host < 0) )
				continue;

			if ( (a_unit->online[l_cdt->host / 8] & (1U << (l_cdt->host % 8))) && !(a_st->online & (1ULL << i)) )
				{
				a_st->online |= 1ULL << i;
				a_st->nonline++;
				}
			}
		}

	a_st->nread	= a_unit->nread;
	a_st->nwrite	= a_unit->nwrite;
	a_st->rbytes	= a_unit->rbytes;
	a_st->wbytes	= a_unit->wbytes;

	for ( int k = 0; k < 2; k++ )
		{
		a_st->latn[k]	= a_unit->lat[k].n;
		a_st->latsum[k]	= a_unit->lat[k].sum;
		a_st->latbk[k]	= a_unit->lat[k].bk;
		a_st->latxfer[k] = a_unit->lat[k].xfer;
		}

	a_st->nrmw	= a_unit->dev.nrmw;
	a_st->ncache	= a_unit->dev.ncache;
	a_st->nsync	= a_unit->dev.nsync;
	a_st->nsyncw	= a_unit->dev.nsyncw;
	a_st->nzero	= a_unit->dev.nzero;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The tick of the statistics: the queue of every unit is summed, and,
**	when the block is published, the state of the server is copied in -
**	SEQ odd while the copy is made, even when it is whole.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_STAT; QINT of the units.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$stat_tick	(
		MH$PORT *	a_port
			)
{
MH$STAT		*l_stat = s_stat;
struct timespec	l_ts;
uint64_t	l_bare = 0;

	for ( unsigned i = 0; i < a_port->nunit; i++ )
		{
		MH$UNIT	*l_unit = &a_port->units[i];
		const	MH$HRB	*l_hrb;

		l_unit->qint += l_unit->current;

		for ( l_hrb = l_unit->qhead; l_hrb; l_hrb = l_hrb->next )
			l_unit->qint++;
		}

	s_qticks++;

	/*
	**  The block is copied in every MH$K_STAT_TICKS ticks, 100 ms
	*/
	if ( !l_stat || (s_qticks % MH$K_STAT_TICKS) )
		return	STS$K_SUCCESS;

	__atomic_store_n(&l_stat->seq, l_stat->seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	clock_gettime(CLOCK_MONOTONIC, &l_ts);
	l_stat->now_ns	= (uint64_t) l_ts.tv_sec * 1000000000ULL + (uint64_t) l_ts.tv_nsec;
	l_stat->wall	= (int64_t) time(NULL);
	l_stat->tick	= a_port->tick;
	l_stat->qticks	= s_qticks;

	l_stat->rx_frames = a_port->rx_frames;
	l_stat->rx_bytes = a_port->rx_bytes;
	l_stat->rx_cc	= a_port->rx_cc;
	l_stat->rx_tr	= a_port->rx_tr;
	l_stat->rx_drop	= a_port->rx_drop;
	l_stat->rx_own	= a_port->rx_own;
	l_stat->rx_auth	= a_port->rx_auth;
	l_stat->tx_frames = a_port->tx_frames;
	l_stat->tx_bytes = a_port->tx_bytes;
	l_stat->tx_err	= a_port->tx_err;
	l_stat->tr_tx	= a_port->tr_tx;
	l_stat->tr_rexmt = a_port->tr_rexmt;
	l_stat->tr_rx	= a_port->tr_rx;
	l_stat->tr_dup	= a_port->tr_dup;
	l_stat->tr_ooo	= a_port->tr_ooo;
	l_stat->tr_old	= a_port->tr_old;
	l_stat->tr_rsvp	= a_port->tr_rsvp;
	l_stat->tr_ackpig = a_port->tr_ackpig;

	for ( int i = 0; i < MH$K_ACKW_N; i++ )
		l_bare += a_port->tr_ackbare[i];

	l_stat->tr_ackbare = l_bare;
	l_stat->blk_snt	= a_port->blk_snt;
	l_stat->blk_req	= a_port->blk_req;
	l_stat->blk_ret	= a_port->blk_ret;
	l_stat->blk_cnf	= a_port->blk_cnf;
	l_stat->blk_bad	= a_port->blk_bad;
	l_stat->blk_bytes_out = a_port->blk_bytes_out;
	l_stat->blk_bytes_in = a_port->blk_bytes_in;
	l_stat->bufused	= a_port->bufused;
	l_stat->nhrb	= a_port->nhrb;
	l_stat->tr_maxwin = a_port->tr_maxwin;

	l_stat->nnode	= a_port->nnode;

	for ( unsigned i = 0; i < a_port->nnode; i++ )
		s_mh$stat_node(&a_port->nodes[i], &l_stat->nodes[i]);

	l_stat->nunit	= a_port->nunit;

	for ( unsigned i = 0; i < a_port->nunit; i++ )
		s_mh$stat_unit(a_port, &a_port->units[i], &l_stat->units[i]);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&l_stat->seq, l_stat->seq + 1, __ATOMIC_RELEASE);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a whole copy of a statistics block: copy it between two reads of
**	SEQ, and again while the writer was in the middle of it.  The writer
**	is never waited for; a copy is taken in at most a few tries, as the
**	writer copies in for some microseconds every 100 ms.
**
**  FORMAL PARAMETERS:
**
**	a_blk		The block: of this process, or mapped from the server
**	a_copy		Output: the copy
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no whole copy in MH$K_STAT_TRIES tries,
**	or the block is not of this layout (MAGIC, VERSION, SIZE).
**--
*/
int	mh$stat_read	(
	const	MH$STAT *	a_blk,
		MH$STAT *	a_copy
			)
{
uint32_t	l_seq1, l_seq2;

	for ( int i = 0; i < MH$K_STAT_TRIES; i++ )
		{
		l_seq1 = __atomic_load_n(&a_blk->seq, __ATOMIC_ACQUIRE);

		if ( l_seq1 & 1 )
			{
			sched_yield();
			continue;
			}

		memcpy(a_copy, (const void *) a_blk, sizeof(MH$STAT));
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		l_seq2 = __atomic_load_n(&a_blk->seq, __ATOMIC_ACQUIRE);

		if ( l_seq1 != l_seq2 )
			continue;

		if ( memcmp(a_copy->magic, MH$T_STAT_MAGIC, sizeof(a_copy->magic)) || (a_copy->version != MH$K_STAT_VERSION)
			|| (a_copy->size != sizeof(MH$STAT)) || (a_copy->nnode > MH$K_STAT_MAXNODE) || (a_copy->nunit > MH$K_STAT_MAXUNIT) )
			return	STS$K_ERROR;

		return	STS$K_SUCCESS;
		}

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The block of this server, for a reader in the process: the thread of
**	Modbus.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	The block; NULL - not published.
**--
*/
const	MH$STAT *	mh$stat_block	(void)
{
	return	s_stat;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Withdraw the block: unmap it and remove the object, so that MHMON
**	finds no server rather than a server that does not move.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_STAT, S_NAME.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$stat_close	(void)
{
	if ( !s_stat )
		return	STS$K_SUCCESS;

	munmap(s_stat, sizeof(MH$STAT));
	s_stat = NULL;
	shm_unlink(s_name);

	return	STS$K_SUCCESS;
}
