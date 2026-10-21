#define	__MODULE__	"MHBK"
#define	__IDENT__	"X01-14"
#define	__REV__		"1.14.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_bk.c
**
**  ABSTRACT:	The backend: the I/O of the units, by io_uring, so that the
**		protocol thread never waits for a disk.
**
**  DESCRIPTION: The protocol thread hands a request (MH$BKREQ, a part of the
**		command it is of) to MH$BK_SUBMIT; it goes to the ring as one
**		or a few steps.  The ring writes its eventfd when a step is
**		over; the protocol thread reads it in its EPOLL loop and calls
**		MH$BK_REAP, which takes the next step or gives the request back
**		to the MSCP server (MH$MSCP_IODONE).  Everything here runs in
**		the protocol thread - no locks.
**
**		  READ	read to the end; past the end of a file - zeros
**		  WRITE	written to the end, made durable as /SYNC says:
**			  DSYNC	every write RWF_DSYNC
**			  GROUP	the writes done while an fdatasync of the
**				device is in flight wait for the next one, then
**				end together - one fdatasync for many
**			  NONE	/UNSAFE: the page cache, for tests only
**		  ZERO	fallocate(ZERO_RANGE | KEEP_SIZE), zeros written where
**			the device does not have it; never DISCARD
**
**		A device whose sector is over 512 octets (a 4K disk) takes a
**		request that is not on whole sectors through a bounce buffer
**		of the whole sectors: a READ reads them and copies the middle
**		out, a WRITE or ZERO reads the first and the last sector,
**		puts its data between and writes them all (read-modify-write).
**		Every WRITE and ZERO of such a device holds its range of
**		sectors: one that meets a range held waits, in order, so two
**		writes of the blocks of one sector never lose each other.
**
**		A request longer than MH$K_BK_STUCK seconds in the backend is
**		reported (IOSTUCK) and cancelled; if the cancel does not take,
**		the unit waits for it - nothing of a newer command may overtake
**		a write on the disk.
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
**		20-OCT-2026	RRL	X01-12 : MISALIGN - a block device that does not
**						start on a physical sector.
**		20-OCT-2026	RRL	X01-13 : BKSTATS: the sectors an RMW reads, the ones the
**						last write of the disk had.
**		20-OCT-2026	RRL	X01-14 : BKSTATS: a model of a cache of the last 2, 16, 128,
**						512 sectors written.
**		20-OCT-2026	RRL	X01-14 : a cache of the 128 sectors last written: an RMW
**						reads only the edges it does not find there.
**
**--
*/

#define	_GNU_SOURCE	1

#include	<errno.h>
#include	<fcntl.h>
#include	<poll.h>
#include	<stdio.h>
#include	<string.h>
#include	<stdlib.h>
#include	<time.h>
#include	<unistd.h>
#include	<liburing.h>
#include	<linux/fs.h>
#include	<sys/eventfd.h>
#include	<sys/file.h>
#include	<sys/ioctl.h>
#include	<sys/stat.h>

#include	"mh_def.h"

/*
**  The steps of a request
*/
enum	{
	MH$K_BKS_IO		= 1,		/* The data: read, write, fallocate		*/
	MH$K_BKS_EDGES,				/* RMW: reading the first and the last sector	*/
	MH$K_BKS_ZFILL,				/* ZERO without fallocate: zeros written	*/
	MH$K_BKS_DSYNC,				/* fdatasync after a ZERO of /SYNC=DSYNC	*/
	MH$K_BKS_GROUP				/* Done, waiting for the fdatasync of a group	*/
	};

/*
**  The ring: one per process - the tests make the port over and over
*/
static	struct	{
	struct io_uring	ring;
	int		efd;			/* -1 - not started				*/
	MH$BKREQ	*dhead, *dtail;		/* Over, for MH$MSCP_IODONE			*/
	MH$BKREQ	*active;		/* In the backend, for IOSTUCK			*/
	uint64_t	lastcheck;
	uint64_t	ngiven;			/* Given back to MH$MSCP_IODONE			*/
} s_bk = { .efd = -1 };

static	uint8_t	s_zeros [MH$K_BK_ZEROSZ] __attribute__ ((aligned (4096)));

static	int	s_mh$bk_step	(MH$BKREQ *a_req);
static	int	s_mh$bk_start	(MH$BKREQ *a_req);
static	int	s_mh$bk_lruhit	(MH$BKDEV *a_dev, uint64_t a_sec);
static	int	s_mh$bk_cget	(MH$BKDEV *a_dev, uint64_t a_sec, uint8_t *a_dst);
static	int	s_mh$bk_cput	(MH$BKDEV *a_dev, uint64_t a_sec, const uint8_t *a_src);
static	int	s_mh$bk_cdrop	(MH$BKDEV *a_dev, uint64_t a_off, uint64_t a_len);
static	int	s_mh$bk_lruput	(MH$BKDEV *a_dev, uint64_t a_sec);



/*
**  Seconds of CLOCK_MONOTONIC
*/
static	uint64_t	s_mh$bk_now	(void)
{
struct timespec	l_ts;

	clock_gettime(CLOCK_MONOTONIC, &l_ts);

	return	(uint64_t) l_ts.tv_sec;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A request is over: off the list of the active ones, its range given
**	up, on the list of the done ones.  MH$BK_REAP gives it back.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request; ERR is set
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_unlock	(MH$BKREQ *a_req);

static	int	s_mh$bk_finish	(
		MH$BKREQ *	a_req
			)
{
	for ( MH$BKREQ **l_pp = &s_bk.active; *l_pp; l_pp = &(*l_pp)->anext )
		{
		if ( *l_pp == a_req )
			{
			*l_pp = a_req->anext;
			break;
			}
		}

	/*
	**  A write that failed: what is on disk under it is not known
	*/
	if ( a_req->err && (a_req->op != MH$K_BK_READ) && (a_req->op != MH$K_BK_SYNC) && (a_req->dev->ssz > 512) )
		s_mh$bk_cdrop(a_req->dev, a_req->aoff, a_req->alen);

	s_mh$bk_unlock(a_req);

	free(a_req->bounce);
	a_req->bounce	= NULL;
	a_req->next	= NULL;

	if ( s_bk.dtail )
		s_bk.dtail->next = a_req;
	else	s_bk.dhead = a_req;

	s_bk.dtail = a_req;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A free entry of the submission queue; a full queue is submitted to
**	make room.
**
**  FORMAL PARAMETERS:
**
**	a_data		The user data of the entry: the request, NULL - none
**
**  RETURN VALUE:
**	The entry; NULL - the ring takes nothing.
**--
*/
static	struct io_uring_sqe *	s_mh$bk_sqe	(
		void *		a_data
			)
{
struct io_uring_sqe	*l_sqe;

	if ( !(l_sqe = io_uring_get_sqe(&s_bk.ring)) )
		{
		io_uring_submit(&s_bk.ring);

		if ( !(l_sqe = io_uring_get_sqe(&s_bk.ring)) )
			return	NULL;
		}

	io_uring_sqe_set_data(l_sqe, a_data);

	return	l_sqe;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Does a range of sectors meet the one of a request?
**
**  FORMAL PARAMETERS:
**
**	a_a, a_b	The requests
**
**  RETURN VALUE:
**	1 - they meet; 0 - not.
**--
*/
static	int	s_mh$bk_meet	(
	const	MH$BKREQ *	a_a,
	const	MH$BKREQ *	a_b
			)
{
	return	(a_a->aoff < a_b->aoff + a_b->alen) && (a_b->aoff < a_a->aoff + a_a->alen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Give up the range of a request, and start the ones that waited for
**	it and now meet no range held - in their order: one that meets an
**	earlier one still waiting waits behind it.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_unlock	(
		MH$BKREQ *	a_req
			)
{
MH$BKDEV	*l_dev = a_req->dev;
MH$BKREQ	**l_pp, *l_w, *l_prev = NULL, *l_next;

	if ( !a_req->locked )
		return	STS$K_SUCCESS;

	a_req->locked = 0;

	for ( l_pp = &l_dev->locks; *l_pp; l_pp = &(*l_pp)->next )
		{
		if ( *l_pp == a_req )
			{
			*l_pp = a_req->next;
			break;
			}
		}

	for ( l_w = l_dev->lwhead; l_w; l_w = l_next )
		{
		int	l_free = 1;

		l_next = l_w->next;

		for ( MH$BKREQ *l_h = l_dev->locks; l_free && l_h; l_h = l_h->next )
			l_free = !s_mh$bk_meet(l_w, l_h);

		for ( MH$BKREQ *l_e = l_dev->lwhead; l_free && (l_e != l_w); l_e = l_e->next )
			l_free = !s_mh$bk_meet(l_w, l_e);

		if ( !l_free )
			{
			l_prev = l_w;
			continue;
			}

		/*
		**  Off the waiting list, onto the held one, and go
		*/
		if ( l_prev )
			l_prev->next = l_next;
		else	l_dev->lwhead = l_next;

		if ( l_dev->lwtail == l_w )
			l_dev->lwtail = l_prev;

		l_w->locked	= 1;
		l_w->next	= l_dev->locks;
		l_dev->locks	= l_w;

		if ( !(1 & s_mh$bk_start(l_w)) )
			{
			l_w->err = EIO;
			s_mh$bk_finish(l_w);
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Group commit: when no fdatasync of the device is in flight and some
**	writes wait for one, send it; it covers all of them.
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the ring takes nothing, the writes end
**	with EIO.
**--
*/
static	int	s_mh$bk_group	(
		MH$BKDEV *	a_dev
			)
{
struct io_uring_sqe	*l_sqe;
MH$BKREQ		*l_req, *l_next;

	if ( a_dev->gbusy || !a_dev->gpend )
		return	STS$K_SUCCESS;

	if ( !(l_sqe = s_mh$bk_sqe(&a_dev->gsync)) )
		{
		for ( l_req = a_dev->gpend, a_dev->gpend = NULL; l_req; l_req = l_next )
			{
			l_next = l_req->next;
			l_req->err = EIO;
			s_mh$bk_finish(l_req);
			}

		return	STS$K_ERROR;
		}

	io_uring_prep_fsync(l_sqe, a_dev->fd, IORING_FSYNC_DATASYNC);

	a_dev->ginfl	= a_dev->gpend;
	a_dev->gpend	= NULL;
	a_dev->gbusy	= 1;
	a_dev->nsync++;

	for ( l_req = a_dev->ginfl; l_req; l_req = l_req->next )
		a_dev->nsyncw++;

	io_uring_submit(&s_bk.ring);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The data of a WRITE or ZERO is written: give up the range, then make
**	it durable as /SYNC says.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the step.
**--
*/
static	int	s_mh$bk_written	(
		MH$BKREQ *	a_req
			)
{
MH$BKDEV	*l_dev = a_req->dev;

	/*
	**  The first and the last sector of this write: a cache would hold
	**  them now (BKSTATS)
	*/
	if ( l_dev->ssz > 512 )
		{
		s_mh$bk_lruput(l_dev, (a_req->off + a_req->len - 1) / l_dev->ssz);
		s_mh$bk_lruput(l_dev, a_req->off / l_dev->ssz);

		/*
		**  The cache: what the write covers is dropped, its first and
		**  last whole sectors go in - out of the bounce buffer of an RMW,
		**  or out of the data of a write on whole sectors.  Before the
		**  range is given up, so no RMW of these sectors runs meanwhile
		*/
		s_mh$bk_cdrop(l_dev, a_req->aoff, a_req->alen);

		if ( a_req->bounce )
			{
			if ( a_req->alen > l_dev->ssz )
				s_mh$bk_cput(l_dev, (a_req->aoff + a_req->alen) / l_dev->ssz - 1, a_req->bounce + a_req->alen - l_dev->ssz);

			s_mh$bk_cput(l_dev, a_req->aoff / l_dev->ssz, a_req->bounce);
			}
		else if ( a_req->op == MH$K_BK_WRITE )
			{
			if ( a_req->len > l_dev->ssz )
				s_mh$bk_cput(l_dev, (a_req->off + a_req->len) / l_dev->ssz - 1, a_req->buf + a_req->len - l_dev->ssz);

			s_mh$bk_cput(l_dev, a_req->off / l_dev->ssz, a_req->buf);
			}
		}

	s_mh$bk_unlock(a_req);

	switch ( a_req->sync )
		{
		case	MH$K_SYNC_GROUP:
			a_req->stage	= MH$K_BKS_GROUP;
			a_req->next	= l_dev->gpend;
			l_dev->gpend	= a_req;

			return	s_mh$bk_group(l_dev);

		case	MH$K_SYNC_DSYNC:
			/*
			**  The writes of a WRITE went RWF_DSYNC; fallocate and the
			**  zeros of a ZERO did not
			*/
			if ( a_req->op == MH$K_BK_ZERO )
				{
				a_req->stage = MH$K_BKS_DSYNC;

				return	s_mh$bk_step(a_req);
				}

			break;
		}

	return	s_mh$bk_finish(a_req);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put the step a request is at to the ring.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request: STAGE, DONE
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the ring takes nothing.
**--
*/
static	int	s_mh$bk_step	(
		MH$BKREQ *	a_req
			)
{
struct io_uring_sqe	*l_sqe;
MH$BKDEV		*l_dev = a_req->dev;
uint32_t		l_n;

	if ( !(l_sqe = s_mh$bk_sqe(a_req)) )
		return	STS$K_ERROR;

	switch ( a_req->stage )
		{
		case	MH$K_BKS_ZFILL:
			l_n = $MIN(a_req->len - a_req->done, (uint32_t) sizeof(s_zeros));
			io_uring_prep_write(l_sqe, l_dev->fd, s_zeros, l_n, a_req->off + a_req->done);
			break;

		case	MH$K_BKS_DSYNC:
			io_uring_prep_fsync(l_sqe, l_dev->fd, IORING_FSYNC_DATASYNC);
			break;

		case	MH$K_BKS_IO:
			if ( a_req->bounce )
				{
				/*
				**  The whole sectors, from or to the bounce buffer
				*/
				if ( a_req->op == MH$K_BK_READ )
					io_uring_prep_read(l_sqe, l_dev->fd, a_req->bounce + a_req->done, a_req->alen - a_req->done,
						a_req->aoff + a_req->done);
				else	io_uring_prep_write(l_sqe, l_dev->fd, a_req->bounce + a_req->done, a_req->alen - a_req->done,
						a_req->aoff + a_req->done);
				}
			else if ( a_req->op == MH$K_BK_READ )
				io_uring_prep_read(l_sqe, l_dev->fd, a_req->buf + a_req->done, a_req->len - a_req->done, a_req->off + a_req->done);
			else if ( a_req->op == MH$K_BK_WRITE )
				io_uring_prep_write(l_sqe, l_dev->fd, a_req->buf + a_req->done, a_req->len - a_req->done, a_req->off + a_req->done);
			else	io_uring_prep_fallocate(l_sqe, l_dev->fd, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, a_req->off, a_req->len);

			if ( (a_req->op == MH$K_BK_WRITE) && (a_req->sync == MH$K_SYNC_DSYNC) )
				l_sqe->rw_flags = RWF_DSYNC;

			break;
		}

	io_uring_submit(&s_bk.ring);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a sector out of the cache of the sectors last written.
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**	a_sec		The sector, in sectors of the device
**	a_dst		Where its data goes, a sector long
**
**  RETURN VALUE:
**	1 - it was there, copied; 0 - not.
**--
*/
static	int	s_mh$bk_cget	(
		MH$BKDEV *	a_dev,
		uint64_t	a_sec,
		uint8_t *	a_dst
			)
{
	if ( !a_dev->cdata )
		return	0;

	for ( unsigned i = 0; i < MH$K_BK_CACHE; i++ )
		{
		if ( !a_dev->cstamp[i] || (a_dev->csec[i] != a_sec) )
			continue;

		memcpy(a_dst, a_dev->cdata + (size_t) i * a_dev->ssz, a_dev->ssz);
		a_dev->cstamp[i] = ++a_dev->cclock;
		a_dev->ncache++;

		return	1;
		}

	return	0;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put a sector just written into the cache: in place of itself, of a
**	free entry or of the oldest one.  Without the memory for the cache
**	nothing is kept.
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**	a_sec		The sector, in sectors of the device
**	a_src		Its data, as on disk now
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_cput	(
		MH$BKDEV *	a_dev,
		uint64_t	a_sec,
	const	uint8_t *	a_src
			)
{
unsigned	l_at = 0;

	if ( !a_dev->cdata && !(a_dev->cdata = aligned_alloc(4096, (size_t) MH$K_BK_CACHE * a_dev->ssz)) )
		return	STS$K_SUCCESS;

	for ( unsigned i = 0; i < MH$K_BK_CACHE; i++ )
		{
		if ( a_dev->cstamp[i] && (a_dev->csec[i] == a_sec) )
			{
			l_at = i;
			break;
			}

		if ( a_dev->cstamp[i] < a_dev->cstamp[l_at] )
			l_at = i;
		}

	memcpy(a_dev->cdata + (size_t) l_at * a_dev->ssz, a_src, a_dev->ssz);
	a_dev->csec[l_at]	= a_sec;
	a_dev->cstamp[l_at]	= ++a_dev->cclock;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Drop from the cache every sector a range of octets touches.
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**	a_off		The start of the range, octets
**	a_len		Its length, octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_cdrop	(
		MH$BKDEV *	a_dev,
		uint64_t	a_off,
		uint64_t	a_len
			)
{
uint64_t	l_first = a_off / a_dev->ssz, l_last = (a_off + a_len - 1) / a_dev->ssz;

	if ( !a_len )
		return	STS$K_SUCCESS;

	for ( unsigned i = 0; i < MH$K_BK_CACHE; i++ )
		if ( a_dev->cstamp[i] && (a_dev->csec[i] >= l_first) && (a_dev->csec[i] <= l_last) )
			a_dev->cstamp[i] = 0;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The model of a cache of the sectors last written (BKSTATS): a sector
**	an RMW reads - is it among the latest 2, 16, 128, 512 written?
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**	a_sec		The sector, in sectors of the device
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_lruhit	(
		MH$BKDEV *	a_dev,
		uint64_t	a_sec
			)
{
static	const unsigned	s_size [MH$K_BK_LRUN] = { 2, 16, 128, MH$K_BK_LRU };

	a_dev->nedge++;

	for ( unsigned i = 0; i < a_dev->nlru; i++ )
		{
		if ( a_dev->lru[i] != a_sec )
			continue;

		for ( unsigned j = 0; j < MH$K_BK_LRUN; j++ )
			if ( i < s_size[j] )
				a_dev->nedgehit[j]++;

		break;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A sector has been written: the head of the list of the model, out of
**	where it was, the last one off when the list is full.
**
**  FORMAL PARAMETERS:
**
**	a_dev		The device
**	a_sec		The sector, in sectors of the device
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bk_lruput	(
		MH$BKDEV *	a_dev,
		uint64_t	a_sec
			)
{
unsigned	l_at = a_dev->nlru;

	for ( unsigned i = 0; i < a_dev->nlru; i++ )
		{
		if ( a_dev->lru[i] == a_sec )
			{
			l_at = i;
			break;
			}
		}

	if ( l_at == MH$K_BK_LRU )
		l_at--;
	else if ( l_at == a_dev->nlru )
		a_dev->nlru++;

	memmove(a_dev->lru + 1, a_dev->lru, l_at * sizeof(a_dev->lru[0]));
	a_dev->lru[0] = a_sec;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put in the ring the read of what is left of an edge sector of an
**	RMW: 0 - the first one, 1 - the last one.  Its completion carries
**	the request with the edge in the low bit.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request, stage EDGES
**	a_edge		0, 1
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the ring takes nothing.
**--
*/
static	int	s_mh$bk_edge	(
		MH$BKREQ *	a_req,
		unsigned	a_edge
			)
{
struct io_uring_sqe	*l_sqe;
MH$BKDEV		*l_dev = a_req->dev;
uint32_t		l_at = (a_edge ? a_req->alen - l_dev->ssz : 0) + a_req->edone[a_edge];

	if ( !(l_sqe = s_mh$bk_sqe((void *) ((uintptr_t) a_req | a_edge))) )
		return	STS$K_ERROR;

	io_uring_prep_read(l_sqe, l_dev->fd, a_req->bounce + l_at, l_dev->ssz - a_req->edone[a_edge], a_req->aoff + l_at);
	a_req->einfl++;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A read of an edge sector of an RMW is over.  The rest of it is asked
**	for again; when both reads are in, the data goes into the bounce
**	buffer and the whole sectors are written.  An error waits for the
**	other read, so that the request is not given back under the ring.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request, stage EDGES
**	a_edge		0 - the first sector, 1 - the last one
**	a_res		The result of the read: octets, 0, -errno
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the next step.
**--
*/
static	int	s_mh$bk_edgedone	(
		MH$BKREQ *	a_req,
		unsigned	a_edge,
		int		a_res
			)
{
MH$BKDEV	*l_dev = a_req->dev;

	a_req->einfl--;

	if ( (a_res == -EINTR) || (a_res == -EAGAIN) )
		{
		if ( !a_req->eerr && (1 & s_mh$bk_edge(a_req, a_edge)) )
			{
			io_uring_submit(&s_bk.ring);

			return	STS$K_SUCCESS;
			}

		a_res = -EIO;
		}

	if ( a_res < 0 )
		{
		if ( !a_req->eerr )
			a_req->eerr = ((a_res == -ECANCELED) && a_req->stuck) ? ETIMEDOUT : -a_res;
		}
	else	{
		/*
		**  A sector past the end of a file reads as zeros
		*/
		if ( !a_res )
			{
			memset(a_req->bounce + (a_edge ? a_req->alen - l_dev->ssz : 0) + a_req->edone[a_edge], 0,
				l_dev->ssz - a_req->edone[a_edge]);
			a_res = (int) (l_dev->ssz - a_req->edone[a_edge]);
			}

		if ( ((a_req->edone[a_edge] += (uint32_t) a_res) < l_dev->ssz) && !a_req->eerr )
			{
			if ( 1 & s_mh$bk_edge(a_req, a_edge) )
				{
				io_uring_submit(&s_bk.ring);

				return	STS$K_SUCCESS;
				}

			a_req->eerr = EIO;
			}
		}

	if ( a_req->einfl )
		return	STS$K_SUCCESS;

	if ( a_req->eerr )
		{
		a_req->err = a_req->eerr;

		return	s_mh$bk_finish(a_req);
		}

	a_req->stage	= MH$K_BKS_IO;
	a_req->done	= 0;

	if ( a_req->op == MH$K_BK_WRITE )
		memcpy(a_req->bounce + (a_req->off - a_req->aoff), a_req->buf, a_req->len);
	else	memset(a_req->bounce + (a_req->off - a_req->aoff), 0, a_req->len);

	return	s_mh$bk_step(a_req);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The first step of a request that may go: the bounce buffer of a
**	request not on whole sectors, and what is read first.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request; its range is held if it needs one
**
**  RETURN VALUE:
**	The condition value of S_MH$BK_STEP; STS$K_ERROR - no memory.
**--
*/
static	int	s_mh$bk_start	(
		MH$BKREQ *	a_req
			)
{
MH$BKDEV	*l_dev = a_req->dev;
int		l_head, l_tail;

	a_req->done	= 0;
	a_req->stage	= MH$K_BKS_IO;

	if ( (l_dev->ssz > 512) && ((a_req->off | a_req->len) & (l_dev->ssz - 1)) )
		{
		if ( !(a_req->bounce = aligned_alloc(4096, $MAX(a_req->alen, 4096U))) )
			return	STS$K_ERROR;

		if ( a_req->op != MH$K_BK_READ )
			{
			l_dev->nrmw++;

			/*
			**  The first sector when the data begins in it, the last
			**  one when the data ends in it - both reads at once; one
			**  read when they are the same sector; the data goes in
			**  after both
			*/
			l_head = (a_req->off & (l_dev->ssz - 1)) != 0;
			l_tail = ((a_req->off + a_req->len) & (l_dev->ssz - 1)) && !(l_head && (a_req->alen == l_dev->ssz));

			/*
			**  What a cache of the sectors last written would have
			**  saved of these reads (BKSTATS)
			*/
			for ( unsigned e = 0; e < 2; e++ )
				{
				if ( e ? l_tail : l_head )
					s_mh$bk_lruhit(l_dev, (e ? a_req->off + a_req->len - 1 : a_req->off) / l_dev->ssz);
				}

			/*
			**  The edges the cache has are not read
			*/
			if ( l_head && s_mh$bk_cget(l_dev, a_req->aoff / l_dev->ssz, a_req->bounce) )
				l_head = 0;

			if ( l_tail && s_mh$bk_cget(l_dev, (a_req->aoff + a_req->alen) / l_dev->ssz - 1, a_req->bounce + a_req->alen - l_dev->ssz) )
				l_tail = 0;

			if ( l_head || l_tail )
				{
				a_req->stage		= MH$K_BKS_EDGES;
				a_req->edone[0]		= a_req->edone[1] = 0;
				a_req->einfl		= a_req->eerr = 0;

				if ( (l_head && !(1 & s_mh$bk_edge(a_req, 0))) || (l_tail && !(1 & s_mh$bk_edge(a_req, 1))) )
					{
					/*
					**  A read already in the ring ends the request
					**  with the error when it comes back
					*/
					if ( !a_req->einfl )
						return	STS$K_ERROR;

					a_req->eerr = EIO;
					}

				io_uring_submit(&s_bk.ring);

				return	STS$K_SUCCESS;
				}
			}
		}

	if ( (a_req->stage == MH$K_BKS_IO) && a_req->bounce && (a_req->op != MH$K_BK_READ) )
		{
		if ( a_req->op == MH$K_BK_WRITE )
			memcpy(a_req->bounce + (a_req->off - a_req->aoff), a_req->buf, a_req->len);
		else	memset(a_req->bounce + (a_req->off - a_req->aoff), 0, a_req->len);
		}

	return	s_mh$bk_step(a_req);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A step is over (a completion of the ring): the next step, the same
**	one again for what is left, or the end of the request.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request
**	a_res		The result of the step: octets, 0, -errno
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of the next step.
**--
*/
static	int	s_mh$bk_done	(
		MH$BKREQ *	a_req,
		int		a_res
			)
{
uint32_t	l_want;

	if ( (a_res == -EINTR) || (a_res == -EAGAIN) )
		return	s_mh$bk_step(a_req);

	if ( a_res < 0 )
		{
		/*
		**  No ZERO_RANGE here: zeros are written instead
		*/
		if ( (a_req->stage == MH$K_BKS_IO) && (a_req->op == MH$K_BK_ZERO) && !a_req->bounce
			&& ((a_res == -EOPNOTSUPP) || (a_res == -EINVAL) || (a_res == -ENODEV)) )
			{
			a_req->stage	= MH$K_BKS_ZFILL;
			a_req->done	= 0;

			return	s_mh$bk_step(a_req);
			}

		a_req->err = ((a_res == -ECANCELED) && a_req->stuck) ? ETIMEDOUT : -a_res;

		return	s_mh$bk_finish(a_req);
		}

	switch ( a_req->stage )
		{
		case	MH$K_BKS_ZFILL:
			if ( (a_req->done += (uint32_t) a_res) < a_req->len )
				return	s_mh$bk_step(a_req);

			return	s_mh$bk_written(a_req);

		case	MH$K_BKS_DSYNC:
			a_req->err = 0;

			return	s_mh$bk_finish(a_req);

		case	MH$K_BKS_IO:
			if ( (a_req->op == MH$K_BK_ZERO) && !a_req->bounce )
				return	s_mh$bk_written(a_req);

			l_want = a_req->bounce ? a_req->alen : a_req->len;

			/*
			**  Past the end of a file: the unit is as big as the file
			**  was at the start, the rest reads as zeros
			*/
			if ( !a_res )
				{
				if ( a_req->op != MH$K_BK_READ )
					{
					a_req->err = EIO;

					return	s_mh$bk_finish(a_req);
					}

				memset((a_req->bounce ? a_req->bounce : a_req->buf) + a_req->done, 0, l_want - a_req->done);
				a_res = (int) (l_want - a_req->done);
				}

			if ( (a_req->done += (uint32_t) a_res) < l_want )
				return	s_mh$bk_step(a_req);

			if ( a_req->op != MH$K_BK_READ )
				return	s_mh$bk_written(a_req);

			if ( a_req->bounce )
				memcpy(a_req->buf, a_req->bounce + (a_req->off - a_req->aoff), a_req->len);

			a_req->err = 0;

			return	s_mh$bk_finish(a_req);
		}

	a_req->err = EINVAL;

	return	s_mh$bk_finish(a_req);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Start the ring, once: the eventfd it writes on every completion.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR - signalled.
**--
*/
int	mh$bk_init	(void)
{
struct io_uring_params	l_par = { .flags = IORING_SETUP_CQSIZE, .cq_entries = 2 * MH$K_BK_RING };
int			l_rc;

	if ( s_bk.efd >= 0 )
		return	STS$K_SUCCESS;

	if ( (l_rc = io_uring_queue_init_params(MH$K_BK_RING, &s_bk.ring, &l_par)) )
		return	$MHMSG(MH$_SYSERR, "io_uring_queue_init(), io_uring is off in this kernel?", -l_rc, strerror(-l_rc));

	if ( 0 > (s_bk.efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) )
		return	$MHMSG(MH$_SYSERR, "eventfd()", errno, strerror(errno));

	if ( (l_rc = io_uring_register_eventfd(&s_bk.ring, s_bk.efd)) )
		return	$MHMSG(MH$_SYSERR, "io_uring_register_eventfd()", -l_rc, strerror(-l_rc));

	s_bk.lastcheck = s_mh$bk_now();

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Signal SYSERR of a call on the file or the device of a unit, the
**	path named: "open(/srv/vms/dka100.img)".
**
**  FORMAL PARAMETERS:
**
**	a_buf, a_bufsz	A buffer for the text
**	a_call		The call
**	a_path		The file or the device
**
**  IMPLICIT INPUTS:
**	ERRNO.
**
**  RETURN VALUE:
**	MH$_SYSERR.
**--
*/
static	int	s_mh$bk_syserr	(
		char *		a_buf,
		unsigned	a_bufsz,
	const	char *		a_call,
	const	char *		a_path
			)
{
int	l_errno = errno;

	snprintf(a_buf, a_bufsz, "%s(%s)", a_call, a_path);

	return	$MHMSG(MH$_SYSERR, a_buf, l_errno, strerror(l_errno));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Open the file or the device of a unit and learn its size and sector.
**	A file: the page cache, flock() against a second server.  A block
**	device: O_DIRECT, O_EXCL - a device mounted on the host or opened by
**	a second server is refused (DEVBUSY).
**
**  FORMAL PARAMETERS:
**
**	a_dev		Output: the device
**	a_path		The file or the device
**	a_ro		Read only
**	a_ssz		The sector of a file, for tests: 0 - 512
**	a_size		Output: the size, octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR, MH$_DEVBUSY - signalled.
**--
*/
int	mh$bk_open	(
		MH$BKDEV *	a_dev,
	const	char *		a_path,
		int		a_ro,
		uint32_t	a_ssz,
		uint64_t *	a_size
			)
{
struct stat	l_st;
int		l_ssz = 512, l_aoff = 0;
unsigned	l_pssz = 0;
char		l_what [256 + 64];

	memset(a_dev, 0, sizeof(*a_dev));
	a_dev->fd	= -1;
	a_dev->gsync.op	= MH$K_BK_SYNC;
	a_dev->gsync.dev = a_dev;

	if ( stat(a_path, &l_st) )
		return	s_mh$bk_syserr(l_what, sizeof(l_what), "stat", a_path);

	if ( S_ISBLK(l_st.st_mode) )
		{
		a_dev->type = MH$K_DEV_BLK;

		if ( 0 > (a_dev->fd = open(a_path, (a_ro ? O_RDONLY : O_RDWR) | O_DIRECT | O_EXCL | O_CLOEXEC)) )
			{
			if ( errno == EBUSY )
				return	$MHMSG(MH$_DEVBUSY, a_path);

			return	s_mh$bk_syserr(l_what, sizeof(l_what), "open", a_path);
			}

		if ( ioctl(a_dev->fd, BLKGETSIZE64, a_size) || ioctl(a_dev->fd, BLKSSZGET, &l_ssz) )
			return	s_mh$bk_syserr(l_what, sizeof(l_what), "ioctl(BLKGETSIZE64, BLKSSZGET)", a_path);

		a_dev->ssz = (uint32_t) l_ssz;

		/*
		**  A partition or a volume that does not start on a physical
		**  sector (BLKALIGNOFF: -1 - it cannot be aligned at all) makes
		**  every write a read-modify-write, here or in the disk itself,
		**  even on a volume of the node with a cluster of 8 or 16
		*/
		if ( !ioctl(a_dev->fd, BLKPBSZGET, &l_pssz) && !ioctl(a_dev->fd, BLKALIGNOFF, &l_aoff) && l_aoff )
			$MHMSG(MH$_MISALIGN, a_path, l_aoff, l_pssz);
		}
	else	{
		a_dev->type = MH$K_DEV_FILE;

		if ( 0 > (a_dev->fd = open(a_path, (a_ro ? O_RDONLY : O_RDWR) | O_CLOEXEC)) )
			return	s_mh$bk_syserr(l_what, sizeof(l_what), "open", a_path);

		if ( flock(a_dev->fd, (a_ro ? LOCK_SH : LOCK_EX) | LOCK_NB) )
			return	s_mh$bk_syserr(l_what, sizeof(l_what), "flock - another server has the file?", a_path);

		*a_size		= (uint64_t) l_st.st_size;
		a_dev->ssz	= a_ssz ? a_ssz : 512;
		}

	/*
	**  512 ... 4096 and a power of two: the bounce buffers and the buffers
	**  of the commands are aligned to 4096
	*/
	if ( (a_dev->ssz < 512) || (a_dev->ssz > 4096) || (a_dev->ssz & (a_dev->ssz - 1)) )
		return	$MHMSG(MH$_SYSERR, "BLKSSZGET, a sector of 512 ... 4096 octets is expected", EINVAL, strerror(EINVAL));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The eventfd the protocol thread waits on.
**
**  RETURN VALUE:
**	The descriptor; -1 - the ring is not started.
**--
*/
int	mh$bk_fd	(void)
{
	return	s_bk.efd;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Hand a request to the backend.  A WRITE or ZERO of a device of
**	sectors over 512 octets first takes its range of sectors, or waits
**	for it.
**
**  FORMAL PARAMETERS:
**
**	a_req		The request: OP, DEV, SYNC, OFF, LEN, BUF, CTX
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the ring is not started or takes nothing.
**--
*/
int	mh$bk_submit	(
		MH$BKREQ *	a_req
			)
{
MH$BKDEV	*l_dev = a_req->dev;

	if ( s_bk.efd < 0 )
		return	STS$K_ERROR;

	a_req->next	= NULL;
	a_req->err	= 0;
	a_req->locked	= a_req->stuck = 0;
	a_req->bounce	= NULL;
	a_req->t0	= s_mh$bk_now();
	a_req->aoff	= a_req->off & ~((uint64_t) l_dev->ssz - 1);
	a_req->alen	= (uint32_t) (((a_req->off + a_req->len + l_dev->ssz - 1) & ~((uint64_t) l_dev->ssz - 1)) - a_req->aoff);

	a_req->anext	= s_bk.active;
	s_bk.active	= a_req;

	if ( (l_dev->ssz > 512) && (a_req->op != MH$K_BK_READ) )
		{
		for ( MH$BKREQ *l_h = l_dev->locks; l_h; l_h = l_h->next )
			{
			if ( s_mh$bk_meet(a_req, l_h) )
				goto	l_wait;
			}

		for ( MH$BKREQ *l_w = l_dev->lwhead; l_w; l_w = l_w->next )
			{
			if ( s_mh$bk_meet(a_req, l_w) )
				goto	l_wait;
			}

		a_req->locked	= 1;
		a_req->next	= l_dev->locks;
		l_dev->locks	= a_req;
		}

	if ( !(1 & s_mh$bk_start(a_req)) )
		{
		s_mh$bk_unlock(a_req);

		for ( MH$BKREQ **l_pp = &s_bk.active; *l_pp; l_pp = &(*l_pp)->anext )
			{
			if ( *l_pp == a_req )
				{
				*l_pp = a_req->anext;
				break;
				}
			}

		free(a_req->bounce);
		a_req->bounce = NULL;

		return	STS$K_ERROR;
		}

	return	STS$K_SUCCESS;

l_wait:
	l_dev->nlockwait++;

	if ( l_dev->lwtail )
		l_dev->lwtail->next = a_req;
	else	l_dev->lwhead = a_req;

	l_dev->lwtail = a_req;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take the completions of the ring: the next steps, the fdatasync of
**	the groups, and give the requests that are over back to the MSCP
**	server.  MH$MSCP_IODONE may submit again; that is taken in the same
**	call.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$bk_reap	(
		MH$PORT *	a_port
			)
{
struct io_uring_cqe	*l_cqe;
MH$BKREQ		*l_req, *l_next;
MH$BKDEV		*l_dev;
uint64_t		l_cnt;
uintptr_t		l_tag;
int			l_res;

	if ( s_bk.efd < 0 )
		return	STS$K_SUCCESS;

	if ( sizeof(l_cnt) != read(s_bk.efd, &l_cnt, sizeof(l_cnt)) )
		l_cnt = 0;

	for ( ;; )
		{
		while ( !io_uring_peek_cqe(&s_bk.ring, &l_cqe) )
			{
			l_tag = (uintptr_t) io_uring_cqe_get_data(l_cqe);
			l_req = (MH$BKREQ *) (l_tag & ~(uintptr_t) 1);
			l_res = l_cqe->res;
			io_uring_cqe_seen(&s_bk.ring, l_cqe);

			/*
			**  A cancel of ours: its own completion says nothing
			*/
			if ( !l_req )
				continue;

			/*
			**  A read of an edge sector of an RMW: the edge in the low bit
			*/
			if ( l_req->stage == MH$K_BKS_EDGES )
				{
				s_mh$bk_edgedone(l_req, (unsigned) (l_tag & 1), l_res);
				continue;
				}

			if ( l_req->op != MH$K_BK_SYNC )
				{
				s_mh$bk_done(l_req, l_res);
				continue;
				}

			/*
			**  The fdatasync of a group: every write it covers ends
			**  with its result; an error is not tried again (Linux
			**  forgets it once told)
			*/
			l_dev = l_req->dev;

			for ( l_req = l_dev->ginfl, l_dev->ginfl = NULL; l_req; l_req = l_next )
				{
				l_next = l_req->next;
				l_req->err = (l_res < 0) ? -l_res : 0;
				s_mh$bk_finish(l_req);
				}

			l_dev->gbusy = 0;
			s_mh$bk_group(l_dev);
			}

		if ( !(l_req = s_bk.dhead) )
			break;

		s_bk.dhead = s_bk.dtail = NULL;

		for ( ; l_req; l_req = l_next )
			{
			l_next = l_req->next;
			s_bk.ngiven++;

			if ( l_req->cb )
				l_req->cb(a_port, l_req);
			else	mh$mscp_iodone(a_port, (MH$HRB *) l_req->ctx);
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Once a second, from the tick of the protocol: a request longer than
**	MH$K_BK_STUCK seconds in the backend is reported and cancelled, once.
**	A cancel that takes ends it with ETIMEDOUT (status DRIVE); one that
**	does not (a write the kernel is in) leaves it - the unit waits.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$bk_tick	(
		MH$PORT *	a_port
			)
{
struct io_uring_sqe	*l_sqe;
uint64_t		l_now = s_mh$bk_now();
MH$HRB			*l_hrb;

	if ( (s_bk.efd < 0) || (l_now == s_bk.lastcheck) )
		return	STS$K_SUCCESS;

	s_bk.lastcheck = l_now;

	for ( MH$BKREQ *l_req = s_bk.active; l_req; l_req = l_req->anext )
		{
		if ( l_req->stuck || (l_now - l_req->t0 < MH$K_BK_STUCK) )
			continue;

		l_req->stuck = 1;

		if ( (l_hrb = l_req->cb ? NULL : (MH$HRB *) l_req->ctx) )
			$MHMSG(MH$_IOSTUCK, mh$mscp_opname(l_hrb), l_hrb->unit->dvname, l_hrb->lbn, (unsigned) (l_now - l_req->t0));
		else	$MHMSG(MH$_IOSTUCK, "I/O", "-", (unsigned) (l_req->off / 512), (unsigned) (l_now - l_req->t0));

		/*
		**  The reads of the edges of an RMW go with the edge in the low
		**  bit of the request
		*/
		for ( unsigned e = 0; e < ((l_req->stage == MH$K_BKS_EDGES) ? 2U : 1U); e++ )
			if ( (l_sqe = s_mh$bk_sqe(NULL)) )
				io_uring_prep_cancel(l_sqe, (void *) ((uintptr_t) l_req | e), 0);

		io_uring_submit(&s_bk.ring);
		}

	(void) a_port;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Wait until the backend gives a request back to the MSCP server - a
**	request may take a few steps: the tests, which have no EPOLL loop.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_ms		How long at most, ms
**
**  RETURN VALUE:
**	STS$K_SUCCESS - something has been given back; STS$K_WARN - nothing.
**--
*/
int	mh$bk_wait	(
		MH$PORT *	a_port,
		int		a_ms
			)
{
struct pollfd	l_pfd = { .fd = s_bk.efd, .events = POLLIN };
uint64_t	l_given = s_bk.ngiven;

	if ( s_bk.efd < 0 )
		return	STS$K_WARN;

	for ( int l_left = a_ms; l_given == s_bk.ngiven; l_left -= 10 )
		{
		if ( l_left <= 0 )
			return	STS$K_WARN;

		if ( 1 == poll(&l_pfd, 1, 10) )
			mh$bk_reap(a_port);
		}

	return	STS$K_SUCCESS;
}
