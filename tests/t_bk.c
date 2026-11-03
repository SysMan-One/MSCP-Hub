#define	__MODULE__	"T_BK"
#define	__IDENT__	"X01-15"
#define	__REV__		"1.15.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_bk.c
**
**  ABSTRACT:	Unit tests of the backend (MHBK) by itself: io_uring, the
**		read-modify-write of a disk of 4096-octet sectors and its
**		ranges of sectors, ERASE, group commit, a read past the end of
**		a file.  A file taken as a disk of 4096-octet sectors (SECTOR=
**		of a unit) stands for a 4K block device: the same steps, with
**		the page cache under them instead of O_DIRECT.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  18-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-11ECO1 : the wait for the requests is 30 s,
**						not 5: an SD card under load is that slow.
**		20-OCT-2026	RRL	X01-12 : RMW of both edges, of one edge.
**		20-OCT-2026	RRL	X01-13 : the sectors an RMW reads, the ones last written.
**		20-OCT-2026	RRL	X01-14 : BKSTATS: a model of a cache of the last 2, 16, 128,
**						512 sectors written.
**		20-OCT-2026	RRL	X01-14 : a cache of the 128 sectors last written: an RMW
**						reads only the edges it does not find there.
**		21-OCT-2026	RRL	X01-15 : ZERO of a file up to 1 MB: zeros written, not
**						fallocate (unwritten extents); BKSTATS counts ZERO.
**		21-OCT-2026	RRL	X01-15 : a file O_DIRECT; odd byte counts, NODIRECT.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<fcntl.h>
#include	<unistd.h>
#include	<sys/ioctl.h>
#include	<linux/fs.h>
#include	<linux/fiemap.h>

#include	"mh_def.h"

#define	T$K_IMGSZ	(1024 * 1024)
#define	T$K_NREQ	32

static	int		s_fails, s_checks;
static	unsigned	s_given;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)



static	int	s_t$check	(
		int		a_ok,
	const	char *		a_what,
		int		a_line
			)
{
	s_checks++;

	if ( a_ok )
		return	STS$K_SUCCESS;

	s_fails++;
	fprintf(stderr, "T_BK:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**  A request is given back: counted
*/
static	int	s_t$done	(
		MH$PORT *	a_port,
		MH$BKREQ *	a_req
			)
{
	(void) a_port;
	(void) a_req;

	s_given++;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Hand a request to the backend.
**--
*/
static	int	s_t$submit	(
		MH$BKREQ *	a_req,
		MH$BKDEV *	a_dev,
		int		a_op,
		int		a_sync,
		uint64_t	a_off,
		uint32_t	a_len,
		uint8_t *	a_buf
			)
{
	memset(a_req, 0, sizeof(*a_req));
	a_req->op	= a_op;
	a_req->dev	= a_dev;
	a_req->sync	= a_sync;
	a_req->off	= a_off;
	a_req->len	= a_len;
	a_req->buf	= a_buf;
	a_req->cb	= s_t$done;

	return	mh$bk_submit(a_req);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Wait until N requests in all have been given back, up to 30 s - a
**	slow medium (an SD card under load) takes many seconds to sync.
**--
*/
static	int	s_t$wait	(
		MH$PORT *	a_port,
		unsigned	a_n
			)
{
	for ( int i = 0; (i < 3000) && (s_given < a_n); i++ )
		mh$bk_wait(a_port, 10);

	return	s_given >= a_n;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is any of a range of a file unwritten (allocated, reads as zeros:
**	fallocate)?  A file system without FIEMAP (tmpfs) says no.
**
**  RETURN VALUE:
**	1 - an unwritten extent is there; 0 - none.
**--
*/
static	int	s_t$unwritten	(
		int		a_fd,
		uint64_t	a_off,
		uint64_t	a_len
			)
{
struct	{
	struct fiemap		map;
	struct fiemap_extent	ext [32];
	} l_fm;

	memset(&l_fm, 0, sizeof(l_fm));
	l_fm.map.fm_start		= a_off;
	l_fm.map.fm_length		= a_len;
	l_fm.map.fm_flags		= FIEMAP_FLAG_SYNC;
	l_fm.map.fm_extent_count	= 32;

	if ( ioctl(a_fd, FS_IOC_FIEMAP, &l_fm.map) )
		return	0;

	for ( unsigned i = 0; i < l_fm.map.fm_mapped_extents; i++ )
		{
		if ( l_fm.ext[i].fe_flags & FIEMAP_EXTENT_UNWRITTEN )
			return	1;
		}

	return	0;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The tests, see the ABSTRACT.
**
**  RETURN VALUE:
**	0 - every check passed; 1 - some failed.
**--
*/
int	main	(void)
{
static	MH$PORT		s_port;
static	MH$BKDEV	s_d512, s_d4k, s_dodd;
static	MH$BKREQ	s_req [T$K_NREQ];
static	uint8_t		s_img [T$K_IMGSZ], s_want [T$K_IMGSZ], s_got [T$K_IMGSZ];
uint8_t			*l_buf [T$K_NREQ];
char			l_img [] = "t_bk_XXXXXX", l_img4 [] = "t_bk_XXXXXX", l_imgo [] = "t_bk_XXXXXX";
uint64_t		l_size;
unsigned		l_n;
int			l_fd, l_ok;

	mh$inimsg();

	for ( unsigned i = 0; i < T$K_IMGSZ; i++ )
		s_img[i] = (uint8_t) (i * 13 + 5 + (i >> 9));

	for ( unsigned i = 0; i < T$K_NREQ; i++ )
		l_buf[i] = aligned_alloc(4096, 8192);

	l_fd = mkstemp(l_img);
	$CHECK((l_fd >= 0) && (T$K_IMGSZ == pwrite(l_fd, s_img, T$K_IMGSZ, 0)), "an image of 1 MB, 512");
	close(l_fd);
	l_fd = mkstemp(l_img4);
	$CHECK((l_fd >= 0) && (T$K_IMGSZ == pwrite(l_fd, s_img, T$K_IMGSZ, 0)), "an image of 1 MB, 4096");
	close(l_fd);

	$CHECK(1 & mh$bk_init(), "io_uring started");
	$CHECK((1 & mh$bk_open(&s_d512, l_img, 0, 0, &l_size)) && (l_size == T$K_IMGSZ) && (s_d512.ssz == 512) && (s_d512.type == MH$K_DEV_FILE),
		"a file of 512-octet sectors");
	$CHECK((1 & mh$bk_open(&s_d4k, l_img4, 0, 4096, &l_size)) && (s_d4k.ssz == 4096), "a file taken as a disk of 4096-octet sectors");

	/*
	**  512: a WRITE of /SYNC=DSYNC, a READ that runs past the end of the file
	*/
	memset(l_buf[0], 0x5A, 4096);
	s_t$submit(&s_req[0], &s_d512, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 8192, 4096, l_buf[0]);
	l_fd = open(l_img, O_RDONLY);
	$CHECK(s_t$wait(&s_port, 1) && !s_req[0].err && (4096 == pread(l_fd, s_got, 4096, 8192)) && !memcmp(s_got, l_buf[0], 4096),
		"WRITE 4096 at 8192: on the file");
	memset(l_buf[1], 0xEE, 1024);
	s_t$submit(&s_req[1], &s_d512, MH$K_BK_READ, 0, T$K_IMGSZ - 512, 1024, l_buf[1]);
	memset(s_want, 0, 1024);
	memcpy(s_want, s_img + T$K_IMGSZ - 512, 512);
	$CHECK(s_t$wait(&s_port, 2) && !s_req[1].err && !memcmp(l_buf[1], s_want, 1024), "READ past the end of the file: the data, then zeros");

	/*
	**  4096: eight WRITEs of 512 into one sector at once, then one of 1024
	**  over the last block of it and the first of the next - every one an
	**  RMW, every one after the first waits for the range; the last one
	**  given wins where they overlap
	*/
	memcpy(s_want, s_img, T$K_IMGSZ);
	s_given = 0;

	for ( unsigned i = 0; i < 8; i++ )
		{
		memset(l_buf[i], (int) (0x10 + i), 512);
		memcpy(s_want + i * 512, l_buf[i], 512);
		s_t$submit(&s_req[i], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, i * 512, 512, l_buf[i]);
		}

	memset(l_buf[8], 0x77, 1024);
	memcpy(s_want + 3584, l_buf[8], 1024);
	s_t$submit(&s_req[8], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 3584, 1024, l_buf[8]);

	l_ok = s_t$wait(&s_port, 9);

	for ( unsigned i = 0; i < 9; i++ )
		l_ok = l_ok && !s_req[i].err;

	l_fd = open(l_img4, O_RDONLY);
	$CHECK(l_ok && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0)) && !memcmp(s_got, s_want, T$K_IMGSZ),
		"4096: 9 WRITEs into the same sectors at once, none lost");
	$CHECK((s_d4k.nrmw == 9) && (s_d4k.nlockwait == 8) && !s_d4k.locks && !s_d4k.lwhead, "... 9 RMW, 8 waited for the range, nothing held");
	$CHECK((s_d4k.nedge == 10) && (s_d4k.nedgehit[0] == 8) && (s_d4k.nedgehit[3] == 8),
		"... they read 10 sectors, 8 of them among the last 2 and the last 512 written");
	$CHECK(s_d4k.ncache == 8, "... 8 of the 10 sectors came out of the cache, not the disk");

	/*
	**  4096: a READ not on whole sectors; a WRITE on whole sectors goes
	**  straight; ERASE not on whole sectors, ERASE on whole sectors
	*/
	s_given = 0;
	s_t$submit(&s_req[0], &s_d4k, MH$K_BK_READ, 0, 1536, 5000 - 1536 + 4, l_buf[0]);
	$CHECK(s_t$wait(&s_port, 1) && !s_req[0].err && !memcmp(l_buf[0], s_want + 1536, 5000 - 1536 + 4), "4096: READ at 1536 of 3468 octets");

	memset(l_buf[1], 0x99, 8192);
	memcpy(s_want + 16384, l_buf[1], 8192);
	s_t$submit(&s_req[1], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 16384, 8192, l_buf[1]);
	$CHECK(s_t$wait(&s_port, 2) && !s_req[1].err && (s_d4k.nrmw == 9), "4096: WRITE of two whole sectors, no RMW");

	memset(s_want + 4608, 0, 1024);
	s_t$submit(&s_req[2], &s_d4k, MH$K_BK_ZERO, MH$K_SYNC_DSYNC, 4608, 1024, NULL);
	memset(s_want + 32768, 0, 8192);
	s_t$submit(&s_req[3], &s_d4k, MH$K_BK_ZERO, MH$K_SYNC_DSYNC, 32768, 8192, NULL);
	$CHECK(s_t$wait(&s_port, 4) && !s_req[2].err && !s_req[3].err && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0))
		&& !memcmp(s_got, s_want, T$K_IMGSZ) && (s_d4k.nrmw == 10), "4096: ERASE of 1024 at 4608 (RMW), of 8192 at 32768 (zeros written)");

	/*
	**  4096: the reads of the first and of the last sector go at once - a
	**  WRITE over the end of one sector, a whole one and the start of a
	**  third; one with the first sector alone, one with the last alone
	*/
	s_given = 0;
	memset(l_buf[4], 0x31, 5632);
	memcpy(s_want + 52736, l_buf[4], 5632);
	s_t$submit(&s_req[4], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 52736, 5632, l_buf[4]);
	memset(l_buf[5], 0x32, 3072);
	memcpy(s_want + 66560, l_buf[5], 3072);
	s_t$submit(&s_req[5], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 66560, 3072, l_buf[5]);
	memset(l_buf[6], 0x33, 1024);
	memcpy(s_want + 73728, l_buf[6], 1024);
	s_t$submit(&s_req[6], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 73728, 1024, l_buf[6]);
	$CHECK(s_t$wait(&s_port, 3) && !s_req[4].err && !s_req[5].err && !s_req[6].err && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0))
		&& !memcmp(s_got, s_want, T$K_IMGSZ) && (s_d4k.nrmw == 13), "4096: RMW of both edges at once, of the first, of the last");

	/*
	**  4096, the cache: a write of a whole sector puts its new data there,
	**  an RMW over it after takes that; an ERASE drops a
	**  sector, an RMW over it after reads the zeros from the disk
	*/
	s_given = 0;
	memset(l_buf[0], 0x41, 4096);
	memcpy(s_want + 81920, l_buf[0], 4096);
	s_t$submit(&s_req[0], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 81920, 4096, l_buf[0]);
	l_ok = s_t$wait(&s_port, 1) && !s_req[0].err;
	l_size = s_d4k.ncache;
	memset(l_buf[1], 0x42, 512);
	memcpy(s_want + 81920 + 1024, l_buf[1], 512);
	s_t$submit(&s_req[1], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 81920 + 1024, 512, l_buf[1]);
	l_ok = l_ok && s_t$wait(&s_port, 2) && !s_req[1].err && (s_d4k.ncache == l_size + 1);
	$CHECK(l_ok && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0)) && !memcmp(s_got, s_want, T$K_IMGSZ),
		"4096 cache: a whole sector written, an RMW over it takes it from the cache, the data is right");

	memset(l_buf[2], 0x43, 512);
	memcpy(s_want + 90112 + 512, l_buf[2], 512);
	s_t$submit(&s_req[2], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 90112 + 512, 512, l_buf[2]);
	l_ok = s_t$wait(&s_port, 3) && !s_req[2].err;
	memset(s_want + 90112, 0, 8192);
	s_t$submit(&s_req[3], &s_d4k, MH$K_BK_ZERO, MH$K_SYNC_DSYNC, 90112, 8192, NULL);
	l_ok = l_ok && s_t$wait(&s_port, 4) && !s_req[3].err;
	l_size = s_d4k.ncache;
	memset(l_buf[4], 0x44, 512);
	memcpy(s_want + 90112 + 1024, l_buf[4], 512);
	s_t$submit(&s_req[4], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 90112 + 1024, 512, l_buf[4]);
	l_ok = l_ok && s_t$wait(&s_port, 5) && !s_req[4].err && (s_d4k.ncache == l_size);
	$CHECK(l_ok && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0)) && !memcmp(s_got, s_want, T$K_IMGSZ),
		"4096 cache: an ERASE drops a sector, an RMW over it after reads the disk, no stale data");

	/*
	**  4096, the cache is full: 128 sectors more written one by one push
	**  out the oldest - an RMW over 81920 reads the disk, over the last of
	**  them takes the cache
	*/
	s_given = 0;
	l_ok = 1;

	for ( unsigned i = 0; l_ok && (i < MH$K_BK_CACHE); i++ )
		{
		memset(l_buf[0], (int) (0x60 + (i & 0x1F)), 4096);
		memcpy(s_want + 524288 + i * 4096, l_buf[0], 4096);
		s_t$submit(&s_req[0], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 524288 + i * 4096, 4096, l_buf[0]);
		l_ok = s_t$wait(&s_port, i + 1) && !s_req[0].err;
		}

	l_size = s_d4k.ncache;
	memset(l_buf[1], 0x45, 512);
	memcpy(s_want + 81920 + 2048, l_buf[1], 512);
	s_t$submit(&s_req[1], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 81920 + 2048, 512, l_buf[1]);
	l_ok = l_ok && s_t$wait(&s_port, MH$K_BK_CACHE + 1) && !s_req[1].err && (s_d4k.ncache == l_size);
	memset(l_buf[2], 0x46, 512);
	memcpy(s_want + T$K_IMGSZ - 1024, l_buf[2], 512);
	s_t$submit(&s_req[2], &s_d4k, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, T$K_IMGSZ - 1024, 512, l_buf[2]);
	l_ok = l_ok && s_t$wait(&s_port, MH$K_BK_CACHE + 2) && !s_req[2].err && (s_d4k.ncache == l_size + 1);
	$CHECK(l_ok && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0)) && !memcmp(s_got, s_want, T$K_IMGSZ),
		"4096 cache: full, the oldest sector is pushed out, the newest is kept, the data is right");
	close(l_fd);

	/*
	**  Group commit: 24 WRITEs at once, a few fdatasync for all of them.
	**  The data and the counters must be right every time; that the writes
	**  were grouped depends on the speed of the file system - a fdatasync
	**  that returns at once (a RAM disk, a container) leaves nothing to
	**  group - so it must happen in one of 5 attempts.
	*/
	unsigned	l_try, l_n0, l_w0;

	l_ok = 1;
	l_n = 24;

	for ( l_try = 0; l_try < 5 && l_ok && l_n >= 24; l_try++ )
		{
		s_given = 0;
		memcpy(s_want, s_img, T$K_IMGSZ);
		l_n0 = (unsigned) s_d512.nsync;
		l_w0 = (unsigned) s_d512.nsyncw;

		for ( unsigned i = 0; i < 24; i++ )
			{
			memset(l_buf[i], (int) (0xA0 + i), 4096);
			s_t$submit(&s_req[i], &s_d512, MH$K_BK_WRITE, MH$K_SYNC_GROUP, 65536 + i * 4096, 4096, l_buf[i]);
			}

		l_ok = s_t$wait(&s_port, 24);
		l_fd = open(l_img, O_RDONLY);

		for ( unsigned i = 0; i < 24; i++ )
			l_ok = l_ok && !s_req[i].err && (4096 == pread(l_fd, s_got, 4096, 65536 + i * 4096)) && !memcmp(s_got, l_buf[i], 4096);

		close(l_fd);
		l_n = (unsigned) s_d512.nsync - l_n0;
		l_ok = l_ok && ((unsigned) s_d512.nsyncw - l_w0 == 24) && (l_n >= 1);
		}

	$CHECK(l_ok && (l_n < 24) && !s_d512.gbusy && !s_d512.gpend && !s_d512.ginfl,
		"GROUP: 24 WRITEs, every one on the file, fewer fdatasync than writes");
	printf("T_BK: group commit - %u fdatasync for 24 writes (attempt %u)\n", l_n, l_try);

	/*
	**  A file: O_DIRECT if its file system takes it.  A byte count not a
	**  multiple of 512 goes through the page cache, and a direct READ
	**  after it sees its data; a file not of whole blocks - NODIRECT
	*/
	printf("T_BK: the file of 512 - %s\n", s_d512.direct ? "O_DIRECT" : "the page cache");
	s_given = 0;
	memset(l_buf[0], 0xD7, 700);
	s_t$submit(&s_req[0], &s_d512, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 196608, 700, l_buf[0]);
	l_ok = s_t$wait(&s_port, 1) && !s_req[0].err;
	s_t$submit(&s_req[1], &s_d512, MH$K_BK_READ, 0, 196608, 1024, l_buf[1]);
	s_t$submit(&s_req[2], &s_d512, MH$K_BK_READ, 0, 196608 + 600, 100, l_buf[2]);
	memcpy(s_want, s_img + 196608, 1024);
	memset(s_want, 0xD7, 700);
	$CHECK(l_ok && s_t$wait(&s_port, 3) && !s_req[1].err && !s_req[2].err && !memcmp(l_buf[1], s_want, 1024) && !memcmp(l_buf[2], s_want + 600, 100),
		"a WRITE of 700 octets, a READ of 1024 over it sees it, a READ of 100");

	l_fd = mkstemp(l_imgo);
	$CHECK((l_fd >= 0) && (1000 == pwrite(l_fd, s_img, 1000, 0)), "a file of 1000 octets");
	close(l_fd);
	s_given = 0;
	l_ok = (1 & mh$bk_open(&s_dodd, l_imgo, 0, 0, &l_size)) && (l_size == 1000) && !s_dodd.direct && (s_dodd.bfd == s_dodd.fd);
	s_t$submit(&s_req[0], &s_dodd, MH$K_BK_READ, 0, 0, 512, l_buf[0]);
	$CHECK(l_ok && s_t$wait(&s_port, 1) && !s_req[0].err && !memcmp(l_buf[0], s_img, 512), "a file not of whole blocks: the page cache (NODIRECT), READ");
	unlink(l_imgo);

	/*
	**  ERASE of a file: up to MH$K_BK_ZFILLMAX zeros are written, so no
	**  unwritten extent is left behind; a bigger one is fallocate
	*/
	s_given = 0;
	memset(l_buf[0], 0xC5, 8192);
	s_t$submit(&s_req[0], &s_d512, MH$K_BK_WRITE, MH$K_SYNC_DSYNC, 262144, 8192, l_buf[0]);
	l_ok = s_t$wait(&s_port, 1) && !s_req[0].err;
	s_t$submit(&s_req[1], &s_d512, MH$K_BK_ZERO, MH$K_SYNC_DSYNC, 262144 + 512, 4096, NULL);
	l_ok = l_ok && s_t$wait(&s_port, 2) && !s_req[1].err;
	memset(s_want, 0xC5, 8192);
	memset(s_want + 512, 0, 4096);
	l_fd = open(l_img, O_RDONLY);
	$CHECK(l_ok && (8192 == pread(l_fd, s_got, 8192, 262144)) && !memcmp(s_got, s_want, 8192) && (s_d512.nzero == 1) && (s_d512.nzfill == 1)
		&& !s_t$unwritten(l_fd, 262144, 8192), "ERASE of 4096 of a file: zeros written, no unwritten extent");

	s_t$submit(&s_req[2], &s_d512, MH$K_BK_ZERO, MH$K_SYNC_DSYNC, 0, MH$K_BK_ZFILLMAX + 512, NULL);
	memset(s_want, 0, T$K_IMGSZ);
	$CHECK(s_t$wait(&s_port, 3) && !s_req[2].err && (T$K_IMGSZ == pread(l_fd, s_got, T$K_IMGSZ, 0)) && !memcmp(s_got, s_want, T$K_IMGSZ)
		&& (s_d512.nzero == 2) && (s_d512.nzfill == 1), "ERASE over MH$K_BK_ZFILLMAX of a file: fallocate, zeros");
	close(l_fd);

	unlink(l_img);
	unlink(l_img4);

	for ( unsigned i = 0; i < T$K_NREQ; i++ )
		free(l_buf[i]);

	printf("T_BK: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
