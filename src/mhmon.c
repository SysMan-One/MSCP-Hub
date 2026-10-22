#define	__MODULE__	"MHMON"
#define	__IDENT__	"X01-17"
#define	__REV__		"1.17.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mhmon.c
**
**  ABSTRACT:	MHMON, the monitor of the server, after the MONITOR utility of
**		OpenVMS: the disks and who has them online, the nodes, the
**		counters of the server - on the screen every few seconds, and
**		in files of the T4 style for TLViz and its kin.
**
**  DESCRIPTION: MHMON reads the statistics block the server publishes in
**		shared memory (MH_STAT.H) and never writes to it: the server
**		does not know that it is watched.  It takes a whole copy of the
**		block every /INTERVAL seconds and shows, for every item, the
**		value of the last interval (CUR) and the average, the least and
**		the greatest since it started (AVE, MIN, MAX), as MONITOR does.
**
**		  mhmon [DISK | NODES | SERVER | ALL] [/qualifier...]
**
**		A rate is the difference of two copies over the difference of
**		their times, both taken by the server.  A server that has
**		restarted (another PID or start time) starts the statistics
**		afresh; one that has gone is said on the screen and MHMON waits
**		for it.
**
**		A server is alive when the SEQ of its block moves: MHMON looks at
**		it for 250 ms when it starts, and does not trust the PID.
**
**		The label of a volume is read from the home block of the disk -
**		the file or the device of the unit, opened read only - when the
**		nodes that have it online change, and every minute.
**
**		/RECORD writes the rates in the T4 style CSV of the T4 kit:
**		four rows of header (the node, the date, the time, the column
**		names), a row a sample, the time of the end of the sample first,
**		a field for every column even when it has no value, every row
**		written as it is made.  The columns are fixed when a file is
**		begun; a node or a unit is found by its name.  /RECORD naming a
**		directory makes a file a day there,
**		T4_<node>_<date>_<start>_<end>.CSV - _RUN in place of the end
**		while it is written.
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

#define	_GNU_SOURCE	1			/* O_DIRECT					*/

#include	<errno.h>
#include	<stdarg.h>
#include	<stddef.h>
#include	<stdio.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<signal.h>
#include	<time.h>
#include	<dirent.h>
#include	<sys/mman.h>
#include	<sys/stat.h>

#include	"mh_def.h"
#include	"mh_stat.h"

/*
**  The classes, the items of the disks, the rows of the server
*/
enum	{
	MH$K_MON_DISK		= 0,
	MH$K_MON_NODES,
	MH$K_MON_SERVER,
	MH$K_MON_ALL
	};

enum	{
	MH$K_ITEM_OPRATE	= 0,		/* Commands a second				*/
	MH$K_ITEM_THRUPUT,			/* Kilobytes a second				*/
	MH$K_ITEM_QUEUE,			/* Commands running and waiting, average	*/
	MH$K_ITEM_RESPONSE,			/* Milliseconds from a command to its end	*/

	MH$K_ITEM_N				/* ... and /ITEM=ALL				*/
	};

enum	{
	MH$K_SRV_RXF		= 0,
	MH$K_SRV_TXF,
	MH$K_SRV_RXKB,
	MH$K_SRV_TXKB,
	MH$K_SRV_REXMT,
	MH$K_SRV_DUP,
	MH$K_SRV_OOO,
	MH$K_SRV_DATOUT,
	MH$K_SRV_DATIN,
	MH$K_SRV_CMDS,
	MH$K_SRV_BUFKB,

	MH$K_SRV_N
	};

#define	MH$K_MON_DEFINT		3		/* /INTERVAL, as MONITOR			*/
#define	MH$K_MON_MAXINT		3600
#define	MH$K_MON_LABELAGE	60		/* s: a label is read again			*/
#define	MH$K_MON_SCREEN		8192		/* The text of one screen			*/
#define	MH$K_MON_ROW		4096		/* One row of a T4 file				*/
#define	MH$K_MON_NOVAL		(-1)		/* No value: an empty field, a blank		*/

/*
**  CUR, AVE, MIN, MAX of one item, in hundredths
*/
typedef struct mh_monval_t {
	int64_t		cur;			/* MH$K_MON_NOVAL - none yet			*/
	int64_t		min, max;
	int64_t		sum;
	uint64_t	n;
} MH$MONVAL;

/*
**  The qualifiers
*/
typedef struct mh_moncfg_t {
	int		interval;		/* /INTERVAL					*/
	ASC		item;			/* /ITEM					*/
	ASC		node;			/* /NODE: the SCSNODE of the server		*/
	ASC		record;			/* /RECORD: a file, or a directory		*/
	ASC		ending;			/* /ENDING=hh:mm[:ss]				*/
	int		nodisplay;		/* /NODISPLAY					*/
} MH$MONCFG;

static	MH$MONCFG	s_cfg = { .interval = MH$K_MON_DEFINT };

static	const OPTS	s_opts [] = {
	{ { $ASCINI("interval") },	&s_cfg.interval,	0,		OPTS$K_INT },
	{ { $ASCINI("item") },	&s_cfg.item,		ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("node") },	&s_cfg.node,		ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("record") },	&s_cfg.record,		ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("ending") },	&s_cfg.ending,		ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("nodisplay") },	&s_cfg.nodisplay,	0,		OPTS$K_OPT },

	{ { 0, { 0 } }, NULL, 0, 0 }					/* OPTS_NULL					*/
	};

static	const char	*s_classes [] = { "DISK", "NODES", "SERVER", "ALL" };
static	const char	*s_items [] = { "OPERATION_RATE", "THROUGHPUT", "QUEUE_LENGTH", "RESPONSE_TIME", "ALL" };
static	const char	*s_ititle [MH$K_ITEM_N] = { "I/O Operation Rate", "Throughput (KB/s)", "I/O Request Queue Length",
					"Response Time (ms)" };
static	const char	*s_icol [MH$K_ITEM_N] = { "Op Rate", "KB Rate", "Queue Length", "Response ms" };
static	const char	*s_stitle [MH$K_SRV_N] = { "Frames Received Rate", "Frames Sent Rate", "Kilobytes Received Rate",
					"Kilobytes Sent Rate", "Retransmission Rate", "Duplicate Packet Rate", "Out of Order Rate",
					"Data Sent (KB/s)", "Data Taken (KB/s)", "Commands in Progress", "Data Buffers (KB)" };
static	const char	*s_chst [] = { "closed", "CCSTART", "VERF", "OPEN", "HELLO" };
static	const char	*s_vcst [] = { "-", "closed", "STACK", "OPEN" };
static	const char	*s_months [12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static	volatile sig_atomic_t	s_stop;		/* SIGINT, SIGTERM				*/
static	int		s_class = MH$K_MON_DISK;
static	int		s_item = MH$K_ITEM_OPRATE;	/* MH$K_ITEM_N - all, in turn			*/
static	int		s_tty;			/* The standard output is a terminal		*/
static	time_t		s_ending;		/* /ENDING, 0 - none				*/

static	char		s_shmname [32];
static	const MH$STAT	*s_blk;			/* The block of the server, mapped		*/
static	MH$STAT		s_cur, s_prev;		/* The last two copies				*/
static	int		s_have;			/* 0 - none, 1 - one, 2 - two of one server	*/
static	unsigned	s_shown;		/* Screens shown				*/
static	int		s_hung;			/* The server lives but does not copy in	*/

static	MH$MONVAL	s_disk [MH$K_STAT_MAXUNIT][MH$K_ITEM_N];
static	MH$MONVAL	s_srv [MH$K_SRV_N];
static	int64_t		s_noderate [MH$K_STAT_MAXNODE];	/* Commands a second of a node, CUR	*/
static	char		s_label [MH$K_STAT_MAXUNIT][16];
static	uint64_t	s_labelmask [MH$K_STAT_MAXUNIT];
static	time_t		s_labelat [MH$K_STAT_MAXUNIT];

static	char		s_screen [MH$K_MON_SCREEN];
static	unsigned	s_scrlen;

/*
**  The T4 file and the columns it was begun with
*/
static	int		s_t4fd = -1;
static	int		s_t4dir;		/* /RECORD is a directory: a file a day		*/
static	char		s_t4path [512];
static	struct tm	s_t4start;		/* When the file was begun			*/
static	char		s_t4node [8];
static	unsigned	s_t4nunit, s_t4nnode;
static	char		s_t4units [MH$K_STAT_MAXUNIT][16];
static	char		s_t4nodes [MH$K_STAT_MAXNODE][8];

/*
**  A line of the screen, made by FAO
*/
#define	$MONLINE(a_ctl, ...)	s_mhmon$line(a_ctl, FAO$_NARG(__VA_ARGS__) FAO$_EACH(__VA_ARGS__))



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Add a line to the screen: the FAO control string and its parameters,
**	then a line feed.  A screen that is full takes no more.
**
**  FORMAL PARAMETERS:
**
**	a_ctl		The FAO control string
**	a_prmcnt	The number of the parameters
**	...		The parameters, each a fao_prm_t
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_SCREEN, S_SCRLEN.
**
**  RETURN VALUE:
**	The condition value of UTIL$FAOL.
**--
*/
static	int	s_mhmon$line	(
	const	char *	a_ctl,
		int	a_prmcnt,
			...
			)
{
fao_prm_t	l_prm [16];
fao_desc_t	l_dsc;
unsigned short	l_len = 0;
va_list		l_ap;
int		l_status;

	if ( sizeof(s_screen) - s_scrlen < 256 )
		return	STS$K_ERROR;

	va_start(l_ap, a_prmcnt);

	for ( int i = 0; (i < a_prmcnt) && (i < 16); i++ )
		l_prm[i] = va_arg(l_ap, fao_prm_t);

	va_end(l_ap);

	l_dsc.dsc$w_length = (unsigned short) (sizeof(s_screen) - s_scrlen - 1);
	l_dsc.dsc$b_dtype = l_dsc.dsc$b_class = 0;
	l_dsc.dsc$a_pointer = s_screen + s_scrlen;

	l_status = __util$faol(a_ctl, &l_len, &l_dsc, l_prm, a_prmcnt);
	s_scrlen += l_len;
	s_screen[s_scrlen++] = '\n';

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A value in hundredths as text, right-justified in a field: 12.05, or
**	blanks for no value.
**
**  FORMAL PARAMETERS:
**
**	a_val		The value, MH$K_MON_NOVAL - none
**	a_width		The field, at most 15; 0 - no field, no blanks
**	a_out		Output: the text, zero-terminated, 16 octets
**
**  RETURN VALUE:
**	The text.
**--
*/
static	char *	s_mhmon$num	(
		int64_t		a_val,
		unsigned	a_width,
		char *		a_out
			)
{
char		l_tmp [32];
unsigned short	l_len = 0;
fao_desc_t	l_dsc = $FAO_DESC(l_tmp);

	if ( a_val < 0 )
		l_len = 0;
	else	__util$fao("!UQ.!2ZL", &l_len, &l_dsc, (fao_prm_t) (a_val / 100), (fao_prm_t) (a_val % 100));

	if ( l_len > 15 )
		l_len = 15;

	if ( a_width > 15 )
		a_width = 15;

	if ( l_len >= a_width )
		{
		memcpy(a_out, l_tmp, l_len);
		a_out[l_len] = '\0';
		return	a_out;
		}

	memset(a_out, ' ', a_width - l_len);
	memcpy(a_out + a_width - l_len, l_tmp, l_len);
	a_out[a_width] = '\0';

	return	a_out;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A time as OpenVMS writes it: 22-OCT-2026 14:30:12, the date alone,
**	or hh:mm.
**
**  FORMAL PARAMETERS:
**
**	a_tm		The local time
**	a_what		0 - date and time, 1 - the date, 2 - hh:mm
**	a_out		Output: zero-terminated, 32 octets
**
**  RETURN VALUE:
**	The text.
**--
*/
static	char *	s_mhmon$time	(
	const	struct tm *	a_tm,
		int		a_what,
		char *		a_out
			)
{
unsigned short	l_len = 0;
fao_desc_t	l_dsc = { 31, 0, 0, a_out };

	if ( a_what == 2 )
		__util$fao("!2ZL:!2ZL", &l_len, &l_dsc, (fao_prm_t) a_tm->tm_hour, (fao_prm_t) a_tm->tm_min);
	else if ( a_what == 1 )
		__util$fao("!2ZL-!AZ-!4ZL", &l_len, &l_dsc, (fao_prm_t) a_tm->tm_mday, (fao_prm_t) s_months[a_tm->tm_mon],
			(fao_prm_t) (a_tm->tm_year + 1900));
	else	__util$fao("!2ZL-!AZ-!4ZL !2ZL:!2ZL:!2ZL", &l_len, &l_dsc, (fao_prm_t) a_tm->tm_mday,
			(fao_prm_t) s_months[a_tm->tm_mon], (fao_prm_t) (a_tm->tm_year + 1900), (fao_prm_t) a_tm->tm_hour,
			(fao_prm_t) a_tm->tm_min, (fao_prm_t) a_tm->tm_sec);

	a_out[l_len] = '\0';

	return	a_out;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is the server of a block alive: does SEQ move?  The server copies in
**	every 100 ms, two and a half of which are waited for; a block left by
**	a server that was killed, or a server that hangs, keeps its SEQ.  This
**	does not trust the PID, which another process may have taken since.
**
**  FORMAL PARAMETERS:
**
**	a_name		The name of the block: /mscphub.MHUB
**
**  RETURN VALUE:
**	STS$K_SUCCESS - SEQ has moved; STS$K_ERROR - it has not, or there is no
**	such block.
**--
*/
static	int	s_mhmon$alive	(
	const	char *	a_name
			)
{
const	MH$STAT	*l_blk;
struct timespec	l_ts = { 0, (MH$K_STAT_TICKS * MH$K_TICK_MS * 1000000L * 5) / 2 };
uint32_t	l_seq;
int		l_fd, l_alive;

	if ( 0 > (l_fd = shm_open(a_name, O_RDONLY | O_CLOEXEC, 0)) )
		return	STS$K_ERROR;

	l_blk = mmap(NULL, offsetof(MH$STAT, started), PROT_READ, MAP_SHARED, l_fd, 0);
	close(l_fd);

	if ( l_blk == MAP_FAILED )
		return	STS$K_ERROR;

	l_seq = __atomic_load_n(&l_blk->seq, __ATOMIC_ACQUIRE);
	nanosleep(&l_ts, NULL);
	l_alive = (__atomic_load_n(&l_blk->seq, __ATOMIC_ACQUIRE) != l_seq);
	munmap((void *) l_blk, offsetof(MH$STAT, started));

	return	l_alive ? STS$K_SUCCESS : STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Find the block of the server: /mscphub.<node> of /NODE, or the only
**	one whose server is alive.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT OUTPUTS:
**	S_SHMNAME.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_NOSERVER, MH$_MANYSERVERS (signalled).
**--
*/
static	int	s_mhmon$find	(void)
{
DIR		*l_dir;
struct dirent	*l_de;
unsigned	l_n = 0, l_seen = 0;
size_t		l_plen = sizeof(MH$T_STAT_PREFIX) - 2;	/* "mscphub."					*/
char		l_suffix [64];

	if ( $ASCLEN(&s_cfg.node) )
		{
		if ( $ASCLEN(&s_cfg.node) > 6 )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.node), "NODE", "at most 6 characters");

		memcpy(s_shmname, MH$T_STAT_PREFIX, sizeof(MH$T_STAT_PREFIX) - 1);

		for ( unsigned i = 0; i < $ASCLEN(&s_cfg.node); i++ )
			{
			char	l_c = $ASCPTR(&s_cfg.node)[i];

			s_shmname[sizeof(MH$T_STAT_PREFIX) - 1 + i] = ((l_c >= 'a') && (l_c <= 'z')) ? (char) (l_c - 'a' + 'A') : l_c;
			}

		s_shmname[sizeof(MH$T_STAT_PREFIX) - 1 + $ASCLEN(&s_cfg.node)] = '\0';

		return	STS$K_SUCCESS;
		}

	if ( !(l_dir = opendir(MH$T_STAT_SHMDIR)) )
		return	$MHMSG(MH$_NOSERVER, "");

	while ( (l_de = readdir(l_dir)) )
		{
		if ( strncmp(l_de->d_name, MH$T_STAT_PREFIX + 1, l_plen) || !l_de->d_name[l_plen] || (strlen(l_de->d_name) - l_plen > 6) )
			continue;

		/*
		**  Only a block whose SEQ moves: one left by a server that was
		**  killed, or by a /NODE no longer used, is not counted
		*/
		{
		char	l_name [64];

		l_name[0] = '/';
		memcpy(l_name + 1, l_de->d_name, strlen(l_de->d_name) + 1);

		l_seen++;

		if ( !(1 & s_mhmon$alive(l_name)) )
			continue;
		}

		if ( !l_n++ )
			memcpy(l_suffix, l_de->d_name + l_plen, strlen(l_de->d_name + l_plen) + 1);
		}

	closedir(l_dir);

	if ( !l_n )
		return	$MHMSG(MH$_NOSERVER, l_seen ? ", or it does not respond" : "");

	if ( l_n > 1 )
		return	$MHMSG(MH$_MANYSERVERS, l_n);

	memcpy(s_shmname, MH$T_STAT_PREFIX, sizeof(MH$T_STAT_PREFIX) - 1);
	memcpy(s_shmname + sizeof(MH$T_STAT_PREFIX) - 1, l_suffix, strlen(l_suffix) + 1);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Map the block of the server, read only.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_SHMNAME, S_BLK.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - there is none (the server does not run);
**	MH$_STATVERS (signalled) - a block of another layout.
**--
*/
static	int	s_mhmon$attach	(void)
{
struct stat	l_st;
const MH$STAT	*l_blk;
int		l_fd;

	if ( 0 > (l_fd = shm_open(s_shmname, O_RDONLY | O_CLOEXEC, 0)) )
		return	STS$K_ERROR;

	if ( fstat(l_fd, &l_st) || (l_st.st_size < (off_t) offsetof(MH$STAT, ident)) )
		{
		close(l_fd);
		return	STS$K_ERROR;
		}

	l_blk = mmap(NULL, (size_t) l_st.st_size, PROT_READ, MAP_SHARED, l_fd, 0);
	close(l_fd);

	if ( l_blk == MAP_FAILED )
		return	STS$K_ERROR;

	if ( memcmp(l_blk->magic, MH$T_STAT_MAGIC, sizeof(l_blk->magic)) )
		{
		munmap((void *) l_blk, (size_t) l_st.st_size);
		return	STS$K_ERROR;
		}

	if ( (l_blk->version != MH$K_STAT_VERSION) || (l_st.st_size != (off_t) sizeof(MH$STAT)) )
		{
		uint32_t	l_vers = l_blk->version;

		munmap((void *) l_blk, (size_t) l_st.st_size);
		return	$MHMSG(MH$_STATVERS, s_shmname, l_vers, MH$K_STAT_VERSION);
		}

	s_blk = l_blk;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Start the statistics afresh: no copy, no CUR, AVE, MIN, MAX.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mhmon$reset	(void)
{
	s_have = 0;

	for ( int u = 0; u < MH$K_STAT_MAXUNIT; u++ )
		{
		for ( int i = 0; i < MH$K_ITEM_N; i++ )
			s_disk[u][i] = (MH$MONVAL) { .cur = MH$K_MON_NOVAL, .min = INT64_MAX };

		s_labelat[u] = 0;
		}

	for ( int i = 0; i < MH$K_SRV_N; i++ )
		s_srv[i] = (MH$MONVAL) { .cur = MH$K_MON_NOVAL, .min = INT64_MAX };

	for ( int i = 0; i < MH$K_STAT_MAXNODE; i++ )
		s_noderate[i] = MH$K_MON_NOVAL;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take the copy of an interval: map the block if it is not, take a
**	whole copy, see that its server lives, start afresh when the server
**	is another one.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_BLK, S_CUR, S_PREV, S_HAVE.
**
**  RETURN VALUE:
**	STS$K_SUCCESS - a copy is in S_CUR; STS$K_ERROR - no server;
**	MH$_STATVERS (signalled, severity E).
**--
*/
static	int	s_mhmon$sample	(void)
{
MH$STAT		l_copy;
int		l_status;

	if ( !s_blk && !(1 & (l_status = s_mhmon$attach())) )
		{
		s_have = 0;
		return	l_status;
		}

	if ( !(1 & mh$stat_read(s_blk, &l_copy)) || (kill((pid_t) l_copy.pid, 0) && (errno == ESRCH)) )
		{
		/*
		**  The server is gone: its block may have been removed and a new
		**  one made, so it is mapped again the next time
		*/
		munmap((void *) s_blk, sizeof(MH$STAT));
		s_blk = NULL;
		s_have = 0;
		return	STS$K_ERROR;
		}

	if ( s_have && ((l_copy.pid != s_cur.pid) || (l_copy.started != s_cur.started)) )
		s_mhmon$reset();

	/*
	**  A server that lives but has not copied in for a second does not
	**  respond: no value is shown for it, rather than the last one again
	*/
	if ( s_have && (l_copy.now_ns == s_cur.now_ns) )
		{
		s_hung = 1;
		return	STS$K_ERROR;
		}

	s_hung = 0;

	s_prev = s_cur;
	s_cur = l_copy;

	if ( s_have < 2 )
		s_have++;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a value of an interval into its CUR, AVE, MIN, MAX.
**
**  FORMAL PARAMETERS:
**
**	a_mv		The item
**	a_val		The value, hundredths
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$take	(
		MH$MONVAL *	a_mv,
		int64_t		a_val
			)
{
	a_mv->cur = a_val;
	a_mv->sum += a_val;
	a_mv->n++;

	if ( a_val < a_mv->min )
		a_mv->min = a_val;

	if ( a_val > a_mv->max )
		a_mv->max = a_val;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A rate in hundredths a second: the difference of a counter over the
**	time between the two copies.  A counter that went back (a node that
**	connected anew) is no rate.
**
**  FORMAL PARAMETERS:
**
**	a_now, a_then	The counter in the two copies
**	a_dt		The time between them, ns
**	a_div		The counter is divided by it (1024 for kilobytes)
**
**  RETURN VALUE:
**	The rate; 0 - none.
**--
*/
static	int64_t	s_mhmon$rate	(
		uint64_t	a_now,
		uint64_t	a_then,
		uint64_t	a_dt,
		uint64_t	a_div
			)
{
	if ( (a_now < a_then) || !a_dt )
		return	0;

	return	(int64_t) ((__uint128_t) (a_now - a_then) * 100 * 1000000000ULL / a_div / a_dt);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The values of an interval, out of the last two copies: of every unit
**	the four items, of the server its rows, of every node its commands.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_CUR, S_PREV; S_DISK, S_SRV, S_NODERATE.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no two copies of one server yet.
**--
*/
static	int	s_mhmon$compute	(void)
{
uint64_t	l_dt, l_dq, l_n, l_sum;

	if ( (s_have < 2) || (s_cur.now_ns <= s_prev.now_ns) )
		return	STS$K_ERROR;

	l_dt = s_cur.now_ns - s_prev.now_ns;

	for ( unsigned u = 0; (u < s_cur.nunit) && (u < s_prev.nunit); u++ )
		{
		const	MH$STUNIT	*l_c = &s_cur.units[u], *l_p = &s_prev.units[u];

		s_mhmon$take(&s_disk[u][MH$K_ITEM_OPRATE], s_mhmon$rate(l_c->nread + l_c->nwrite, l_p->nread + l_p->nwrite, l_dt, 1));
		s_mhmon$take(&s_disk[u][MH$K_ITEM_THRUPUT], s_mhmon$rate(l_c->rbytes + l_c->wbytes, l_p->rbytes + l_p->wbytes, l_dt, 1024));

		l_dq = s_cur.qticks - s_prev.qticks;
		s_mhmon$take(&s_disk[u][MH$K_ITEM_QUEUE], l_dq ? (int64_t) ((l_c->qint - l_p->qint) * 100 / l_dq) : 0);

		l_n = (l_c->latn[0] + l_c->latn[1]) - (l_p->latn[0] + l_p->latn[1]);
		l_sum = (l_c->latsum[0] + l_c->latsum[1]) - (l_p->latsum[0] + l_p->latsum[1]);
		s_mhmon$take(&s_disk[u][MH$K_ITEM_RESPONSE], l_n ? (int64_t) (l_sum / l_n / 10000) : 0);
		}

	s_mhmon$take(&s_srv[MH$K_SRV_RXF], s_mhmon$rate(s_cur.rx_frames, s_prev.rx_frames, l_dt, 1));
	s_mhmon$take(&s_srv[MH$K_SRV_TXF], s_mhmon$rate(s_cur.tx_frames, s_prev.tx_frames, l_dt, 1));
	s_mhmon$take(&s_srv[MH$K_SRV_RXKB], s_mhmon$rate(s_cur.rx_bytes, s_prev.rx_bytes, l_dt, 1024));
	s_mhmon$take(&s_srv[MH$K_SRV_TXKB], s_mhmon$rate(s_cur.tx_bytes, s_prev.tx_bytes, l_dt, 1024));
	s_mhmon$take(&s_srv[MH$K_SRV_REXMT], s_mhmon$rate(s_cur.tr_rexmt, s_prev.tr_rexmt, l_dt, 1));
	s_mhmon$take(&s_srv[MH$K_SRV_DUP], s_mhmon$rate(s_cur.tr_dup, s_prev.tr_dup, l_dt, 1));
	s_mhmon$take(&s_srv[MH$K_SRV_OOO], s_mhmon$rate(s_cur.tr_ooo, s_prev.tr_ooo, l_dt, 1));
	s_mhmon$take(&s_srv[MH$K_SRV_DATOUT], s_mhmon$rate(s_cur.blk_bytes_out, s_prev.blk_bytes_out, l_dt, 1024));
	s_mhmon$take(&s_srv[MH$K_SRV_DATIN], s_mhmon$rate(s_cur.blk_bytes_in, s_prev.blk_bytes_in, l_dt, 1024));
	s_mhmon$take(&s_srv[MH$K_SRV_CMDS], (int64_t) s_cur.nhrb * 100);
	s_mhmon$take(&s_srv[MH$K_SRV_BUFKB], (int64_t) (s_cur.bufused / 1024) * 100);

	for ( unsigned i = 0; i < s_cur.nnode; i++ )
		s_noderate[i] = ((i < s_prev.nnode) && !strncmp(s_cur.nodes[i].name, s_prev.nodes[i].name, 8))
			? s_mhmon$rate(s_cur.nodes[i].ncmd, s_prev.nodes[i].ncmd, l_dt, 1) : MH$K_MON_NOVAL;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The label of the volume on a unit: the home block of Files-11 (ODS-2,
**	ODS-5) at block 1 has DECFILE11 at offset 0x1F0 and the volume name,
**	12 characters, at 0x1D8.  Read again when the nodes that have the
**	unit online change, and every minute; the file or the device is
**	opened read only, never for exclusive use.
**
**  FORMAL PARAMETERS:
**
**	a_u		The index of the unit
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_CUR; S_LABEL, S_LABELMASK, S_LABELAT.
**
**  RETURN VALUE:
**	The label; empty - none (not Files-11, not readable).
**--
*/
static	const char *	s_mhmon$label	(
		unsigned	a_u
			)
{
const	MH$STUNIT	*l_u = &s_cur.units[a_u];
time_t		l_now = time(NULL);
uint8_t		*l_buf = NULL;
int		l_fd, l_n;

	if ( s_labelat[a_u] && (s_labelmask[a_u] == l_u->online) && (l_now - s_labelat[a_u] < MH$K_MON_LABELAGE) )
		return	s_label[a_u];

	s_labelat[a_u] = l_now;
	s_labelmask[a_u] = l_u->online;
	s_label[a_u][0] = '\0';

	/*
	**  Direct I/O sees what the server has written with it; a file system
	**  without it takes the page cache, as the server does
	*/
	if ( 0 > (l_fd = open(l_u->path, O_RDONLY | O_DIRECT | O_CLOEXEC)) && (0 > (l_fd = open(l_u->path, O_RDONLY | O_CLOEXEC))) )
		return	s_label[a_u];

	if ( posix_memalign((void **) &l_buf, 4096, 4096) )
		{
		close(l_fd);
		return	s_label[a_u];
		}

	l_n = (int) pread(l_fd, l_buf, 4096, 0);

	if ( (l_n < 0) && (errno == EINVAL) )
		{
		close(l_fd);

		if ( 0 <= (l_fd = open(l_u->path, O_RDONLY | O_CLOEXEC)) )
			l_n = (int) pread(l_fd, l_buf, 4096, 0);
		}

	if ( l_fd >= 0 )
		close(l_fd);

	if ( (l_n >= 1024) && !memcmp(l_buf + 512 + 0x1F0, "DECFILE11", 9) )
		{
		int	l_len = 12;

		memcpy(s_label[a_u], l_buf + 512 + 0x1D8, 12);

		while ( (l_len > 0) && ((s_label[a_u][l_len - 1] == ' ') || !s_label[a_u][l_len - 1]) )
			l_len--;

		s_label[a_u][l_len] = '\0';
		}

	free(l_buf);

	return	s_label[a_u];
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The names of the nodes a unit is online to: ALPHA1,ALPHA2.
**
**  FORMAL PARAMETERS:
**
**	a_mask		Bit n - node n of the copy
**	a_out		Output, zero-terminated
**	a_outsz		Its size
**
**  RETURN VALUE:
**	The text; "-" for none.
**--
*/
static	char *	s_mhmon$online	(
		uint64_t	a_mask,
		char *		a_out,
		unsigned	a_outsz
			)
{
unsigned	l_len = 0, l_nlen;

	for ( unsigned i = 0; i < s_cur.nnode; i++ )
		{
		if ( !(a_mask & (1ULL << i)) )
			continue;

		l_nlen = (unsigned) strnlen(s_cur.nodes[i].name, 8);

		if ( l_len + l_nlen + 2 >= a_outsz )
			break;

		if ( l_len )
			a_out[l_len++] = ',';

		memcpy(a_out + l_len, s_cur.nodes[i].name, l_nlen);
		l_len += l_nlen;
		}

	if ( !l_len )
		a_out[l_len++] = '-';

	a_out[l_len] = '\0';

	return	a_out;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The head of a screen, as MONITOR has it: the utility, the class and
**	the server, the time.
**
**  FORMAL PARAMETERS:
**
**	a_title		The title of the class
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$head	(
	const	char *	a_title
			)
{
struct tm	l_tm;
time_t		l_now = time(NULL);
char		l_time [32];
int		l_pad;

	localtime_r(&l_now, &l_tm);

	$MONLINE("!28* MSCPHUB Monitor Utility");
	l_pad = 40 - (int) strlen(a_title) / 2;
	$MONLINE("!#* !AZ", l_pad > 0 ? l_pad : 0, a_title);
	l_pad = 40 - (10 + (int) strnlen(s_cur.node, 8)) / 2;
	$MONLINE("!#* on server !AZ", l_pad > 0 ? l_pad : 0, s_cur.node[0] ? s_cur.node : "-");
	$MONLINE("!29* !AZ", s_mhmon$time(&l_tm, 0, l_time));
	$MONLINE("");
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The screen of the disks: an item, CUR, AVE, MIN and MAX of every
**	unit, its label and the nodes that have it online.
**
**  FORMAL PARAMETERS:
**
**	a_item		MH$K_ITEM_*
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$disk	(
		int		a_item
			)
{
char		l_on [24], l_cur [16], l_ave [16], l_min [16], l_max [16];

	s_mhmon$head("DISK I/O STATISTICS");
	$MONLINE("!46<!AZ!>     CUR      AVE      MIN      MAX", s_ititle[a_item]);
	$MONLINE("");

	for ( unsigned u = 0; u < s_cur.nunit; u++ )
		{
		const	MH$MONVAL	*l_mv = &s_disk[u][a_item];

		$MONLINE("!12<!AZ!> !12<!AZ!> !20<!AZ!>!AZ !AZ !AZ !AZ", s_cur.units[u].dvname, s_mhmon$label(u),
			s_mhmon$online(s_cur.units[u].online, l_on, sizeof(l_on)), s_mhmon$num(l_mv->cur, 8, l_cur),
			s_mhmon$num(l_mv->n ? l_mv->sum / (int64_t) l_mv->n : MH$K_MON_NOVAL, 8, l_ave),
			s_mhmon$num(l_mv->n ? l_mv->min : MH$K_MON_NOVAL, 8, l_min), s_mhmon$num(l_mv->n ? l_mv->max : MH$K_MON_NOVAL, 8, l_max));
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The screen of the nodes: every node the server knows, its channel
**	and circuit, its connections, the units online to it and the MSCP
**	commands it sends a second.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$nodes	(void)
{
char		l_rate [16];
unsigned	l_disks;

	s_mhmon$head("NODES OF THE SERVER");
	$MONLINE("Node    System ID  Channel  Circuit  Opens  Connections  Disks  Commands Rate");
	$MONLINE("");

	for ( unsigned i = 0; i < s_cur.nnode; i++ )
		{
		const	MH$STNODE	*l_n = &s_cur.nodes[i];

		l_disks = 0;

		for ( unsigned u = 0; u < s_cur.nunit; u++ )
			l_disks += !!(s_cur.units[u].online & (1ULL << i));

		$MONLINE("!8<!AZ!>!9UL  !9<!AZ!>!9<!AZ!>!5UL  !11UL  !5UL  !AZ", l_n->name, l_n->sysid,
			s_chst[l_n->chstate < 5 ? l_n->chstate : 0], s_vcst[l_n->vcstate < 4 ? l_n->vcstate : 0], l_n->chopens,
			l_n->nconn, l_disks, s_mhmon$num(s_noderate[i], 13, l_rate));
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The screen of the server: frames, the transport, the data, the
**	commands in progress, with CUR, AVE, MIN, MAX.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$server	(void)
{
char		l_cur [16], l_ave [16], l_min [16], l_max [16];

	s_mhmon$head("SERVER STATISTICS");
	$MONLINE("!46<MSCPHUB !AZ on !AZ!>     CUR      AVE      MIN      MAX", s_cur.ident, s_cur.ifname);
	$MONLINE("");

	for ( int i = 0; i < MH$K_SRV_N; i++ )
		{
		const	MH$MONVAL	*l_mv = &s_srv[i];

		$MONLINE("!46<!AZ!>!AZ !AZ !AZ !AZ", s_stitle[i], s_mhmon$num(l_mv->cur, 8, l_cur),
			s_mhmon$num(l_mv->n ? l_mv->sum / (int64_t) l_mv->n : MH$K_MON_NOVAL, 8, l_ave),
			s_mhmon$num(l_mv->n ? l_mv->min : MH$K_MON_NOVAL, 8, l_min), s_mhmon$num(l_mv->n ? l_mv->max : MH$K_MON_NOVAL, 8, l_max));
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Show a screen: the class of the command, or the next one of ALL; the
**	item of /ITEM, or the next one of ALL.  On a terminal the screen is
**	cleared first; otherwise the screens follow one another.
**
**  FORMAL PARAMETERS:
**
**	a_up		The server is there
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mhmon$show	(
		int		a_up
			)
{
int	l_class = (s_class == MH$K_MON_ALL) ? (int) (s_shown % 3) : s_class;
int	l_item = (s_item == MH$K_ITEM_N) ? (int) ((s_shown / ((s_class == MH$K_MON_ALL) ? 3 : 1)) % MH$K_ITEM_N) : s_item;
ssize_t	l_rc;

	s_scrlen = 0;

	if ( s_tty )
		{
		memcpy(s_screen, "\033[H\033[J", 6);
		s_scrlen = 6;
		}

	if ( !a_up )
		{
		s_mhmon$head(s_hung ? "SERVER NOT RESPONDING" : "NO SERVER");

		if ( s_hung )
			$MONLINE("The MSCPHUB server !AZ (PID !UL) runs but has not updated its counters; waiting for it.",
				s_cur.node, s_cur.pid);
		else	$MONLINE("No MSCPHUB server runs (!AZ); waiting for it.", s_shmname);
		}
	else if ( l_class == MH$K_MON_DISK )
		s_mhmon$disk(l_item);
	else if ( l_class == MH$K_MON_NODES )
		s_mhmon$nodes();
	else	s_mhmon$server();

	if ( !s_tty )
		$MONLINE("");

	l_rc = write(STDOUT_FILENO, s_screen, s_scrlen);
	s_shown++;

	return	(l_rc == (ssize_t) s_scrlen) ? STS$K_SUCCESS : STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The name of a T4 file of a day: T4_<node>_<d><MON><yyyy>_<hhmm>_<end>.CSV,
**	the form of the files of the T4 kit, e.g. T4_MHUB_22OCT2026_1430_RUN.CSV.
**
**  FORMAL PARAMETERS:
**
**	a_end		The time of the end, hhmm; NULL - RUN, it is being written
**	a_out		Output: the full path, zero-terminated
**	a_outsz		Its size
**
**  IMPLICIT INPUTS:
**	S_CFG.RECORD, S_T4NODE, S_T4START.
**
**  RETURN VALUE:
**	The path.
**--
*/
static	char *	s_mhmon$t4name	(
	const	char *	a_end,
		char *	a_out,
		unsigned	a_outsz
			)
{
unsigned short	l_len = 0;
fao_desc_t	l_dsc = { (unsigned short) (a_outsz - 1), 0, 0, a_out };

	__util$fao("!AZ/T4_!AZ_!UL!AZ!4ZL_!2ZL!2ZL_!AZ.CSV", &l_len, &l_dsc, (fao_prm_t) $ASCPTR(&s_cfg.record), (fao_prm_t) s_t4node,
		(fao_prm_t) s_t4start.tm_mday, (fao_prm_t) s_months[s_t4start.tm_mon], (fao_prm_t) (s_t4start.tm_year + 1900),
		(fao_prm_t) s_t4start.tm_hour, (fao_prm_t) s_t4start.tm_min, (fao_prm_t) (a_end ? a_end : "RUN"));

	a_out[l_len] = '\0';

	return	a_out;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Add a field to a row of a T4 file: a comma, then the text.
**
**  FORMAL PARAMETERS:
**
**	a_row, a_len	The row and its length so far
**	a_txt		The text; a comma in it would make two fields and is
**			taken out
**	a_prefix	[MSCPHUB] goes first: a column name
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$field	(
		char *		a_row,
		unsigned *	a_len,
	const	char *		a_txt,
		int		a_prefix
			)
{
static	const char	s_pfx [] = "[MSCPHUB]";

	if ( *a_len + 64 + strlen(a_txt) >= MH$K_MON_ROW )
		return;

	if ( *a_len )
		a_row[(*a_len)++] = ',';

	if ( a_prefix )
		{
		memcpy(a_row + *a_len, s_pfx, sizeof(s_pfx) - 1);
		*a_len += sizeof(s_pfx) - 1;
		}

	for ( ; *a_txt; a_txt++ )
		if ( *a_txt != ',' )
			a_row[(*a_len)++] = *a_txt;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Begin a T4 file: the columns are those of the server now - its rows,
**	four items of every unit, the commands of every node - and the four
**	rows of the header go out at once.
**
**  FORMAL PARAMETERS:
**
**	a_now		The time it is begun
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_CUR; S_T4FD, S_T4PATH, S_T4START, the columns.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR (signalled, severity F) - it cannot be made.
**--
*/
static	int	s_mhmon$t4open	(
		time_t		a_now
			)
{
char		l_row [MH$K_MON_ROW], l_txt [64], l_time [32];
unsigned	l_len = 0;
unsigned short	l_flen;
fao_desc_t	l_dsc;

	localtime_r(&a_now, &s_t4start);
	memcpy(s_t4node, s_cur.node, sizeof(s_t4node));

	if ( s_t4dir )
		s_mhmon$t4name(NULL, s_t4path, sizeof(s_t4path));
	else	{
		memcpy(s_t4path, $ASCPTR(&s_cfg.record), $ASCLEN(&s_cfg.record));
		s_t4path[$ASCLEN(&s_cfg.record)] = '\0';
		}

	if ( 0 > (s_t4fd = open(s_t4path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644)) )
		return	$MHMSG(MH$_SYSERR, "open(/RECORD)", errno, strerror(errno));

	/*
	**  The columns, fixed for the life of the file
	*/
	s_t4nunit = s_cur.nunit;
	s_t4nnode = s_cur.nnode;

	for ( unsigned u = 0; u < s_t4nunit; u++ )
		memcpy(s_t4units[u], s_cur.units[u].dvname, sizeof(s_t4units[u]));

	for ( unsigned i = 0; i < s_t4nnode; i++ )
		memcpy(s_t4nodes[i], s_cur.nodes[i].name, sizeof(s_t4nodes[i]));

	/*
	**  The node and comments; the date; the time; the names
	*/
	s_mhmon$field(l_row, &l_len, s_t4node, 0);
	s_mhmon$field(l_row, &l_len, "MSCPHUB MHMON " __IDENT__, 0);
	l_row[l_len++] = '\n';
	l_flen = (unsigned short) strlen(s_mhmon$time(&s_t4start, 1, l_time));
	memcpy(l_row + l_len, l_time, l_flen);
	l_len += l_flen;
	l_row[l_len++] = '\n';
	l_flen = (unsigned short) strlen(s_mhmon$time(&s_t4start, 2, l_time));
	memcpy(l_row + l_len, l_time, l_flen);
	l_len += l_flen;
	l_row[l_len++] = '\n';

	if ( (ssize_t) l_len != write(s_t4fd, l_row, l_len) )
		return	$MHMSG(MH$_SYSERR, "write(/RECORD)", errno, strerror(errno));

	l_len = 0;
	s_mhmon$field(l_row, &l_len, "Sample Time", 1);

	for ( int i = 0; i < MH$K_SRV_N; i++ )
		s_mhmon$field(l_row, &l_len, s_stitle[i], 1);

	for ( unsigned u = 0; u < s_t4nunit; u++ )
		for ( int i = 0; i < MH$K_ITEM_N; i++ )
			{
			l_dsc = (fao_desc_t) $FAO_DESC(l_txt);
			l_flen = 0;
			__util$fao("!AZ !AZ", &l_flen, &l_dsc, (fao_prm_t) s_t4units[u], (fao_prm_t) s_icol[i]);
			l_txt[l_flen] = '\0';
			s_mhmon$field(l_row, &l_len, l_txt, 1);
			}

	for ( unsigned i = 0; i < s_t4nnode; i++ )
		{
		l_dsc = (fao_desc_t) $FAO_DESC(l_txt);
		l_flen = 0;
		__util$fao("!AZ Commands Rate", &l_flen, &l_dsc, (fao_prm_t) s_t4nodes[i]);
		l_txt[l_flen] = '\0';
		s_mhmon$field(l_row, &l_len, l_txt, 1);
		}

	l_row[l_len++] = '\n';

	if ( (ssize_t) l_len != write(s_t4fd, l_row, l_len) )
		return	$MHMSG(MH$_SYSERR, "write(/RECORD)", errno, strerror(errno));

	return	$MHMSG(MH$_RECORD, s_cfg.interval, s_t4path);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	End a T4 file; the one of a day gets the time of its end in its name.
**
**  FORMAL PARAMETERS:
**
**	a_now		The time of the last sample
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mhmon$t4close	(
		time_t		a_now
			)
{
char		l_final [512], l_end [8];
struct tm	l_tm;
unsigned short	l_len = 0;
fao_desc_t	l_dsc = $FAO_DESC(l_end);

	if ( s_t4fd < 0 )
		return	STS$K_SUCCESS;

	close(s_t4fd);
	s_t4fd = -1;

	if ( !s_t4dir )
		return	STS$K_SUCCESS;

	localtime_r(&a_now, &l_tm);

	/*
	**  A file that ends past the midnight of its day ends at 2400
	*/
	if ( (l_tm.tm_yday != s_t4start.tm_yday) || (l_tm.tm_year != s_t4start.tm_year) )
		__util$fao("2400", &l_len, &l_dsc);
	else	__util$fao("!2ZL!2ZL", &l_len, &l_dsc, (fao_prm_t) l_tm.tm_hour, (fao_prm_t) l_tm.tm_min);

	l_end[l_len] = '\0';

	if ( rename(s_t4path, s_mhmon$t4name(l_end, l_final, sizeof(l_final))) )
		$MHMSG(MH$_SYSERR, "rename(/RECORD)", errno, strerror(errno));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Write the row of a sample to the T4 file, beginning the file - and
**	in a directory a new one every day - first.  The time is the end of
**	the sample in the OpenVMS form; a field with no value is empty.
**
**  FORMAL PARAMETERS:
**
**	a_now		The time of the sample
**	a_up		The server is there: without it every field is empty
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_SYSERR (signalled, severity F).
**--
*/
static	int	s_mhmon$t4row	(
		time_t		a_now,
		int		a_up
			)
{
char		l_row [MH$K_MON_ROW], l_num [16], l_time [32];
unsigned	l_len = 0;
struct tm	l_tm;
int		l_status;

	localtime_r(&a_now, &l_tm);

	if ( (s_t4fd >= 0) && s_t4dir && ((l_tm.tm_yday != s_t4start.tm_yday) || (l_tm.tm_year != s_t4start.tm_year)) )
		s_mhmon$t4close(a_now);

	if ( s_t4fd < 0 )
		{
		if ( !a_up )
			return	STS$K_SUCCESS;

		if ( !(1 & (l_status = s_mhmon$t4open(a_now))) )
			return	l_status;
		}

	s_mhmon$field(l_row, &l_len, s_mhmon$time(&l_tm, 0, l_time), 0);

	for ( int i = 0; i < MH$K_SRV_N; i++ )
		s_mhmon$field(l_row, &l_len, a_up ? s_mhmon$num(s_srv[i].cur, 0, l_num) : "", 0);

	for ( unsigned c = 0; c < s_t4nunit; c++ )
		{
		int	l_u = -1;

		for ( unsigned u = 0; a_up && (u < s_cur.nunit); u++ )
			if ( !strncmp(s_cur.units[u].dvname, s_t4units[c], sizeof(s_t4units[c])) )
				l_u = (int) u;

		for ( int i = 0; i < MH$K_ITEM_N; i++ )
			s_mhmon$field(l_row, &l_len, (l_u >= 0) ? s_mhmon$num(s_disk[l_u][i].cur, 0, l_num) : "", 0);
		}

	for ( unsigned c = 0; c < s_t4nnode; c++ )
		{
		int	l_n = -1;

		for ( unsigned i = 0; a_up && (i < s_cur.nnode); i++ )
			if ( !strncmp(s_cur.nodes[i].name, s_t4nodes[c], sizeof(s_t4nodes[c])) )
				l_n = (int) i;

		s_mhmon$field(l_row, &l_len, (l_n >= 0) ? s_mhmon$num(s_noderate[l_n], 0, l_num) : "", 0);
		}

	l_row[l_len++] = '\n';

	if ( (ssize_t) l_len != write(s_t4fd, l_row, l_len) )
		return	$MHMSG(MH$_SYSERR, "write(/RECORD)", errno, strerror(errno));

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	SIGINT, SIGTERM: stop after the screen or the row of now.
**
**  FORMAL PARAMETERS:
**
**	a_sig		The signal (the signature of a handler is the system's)
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_mhmon$sig	(
		int		a_sig
			)
{
	s_stop = 1;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Check the qualifiers and the class, and work out /ENDING: hh:mm[:ss]
**	of today, or of tomorrow when it is past.
**
**  FORMAL PARAMETERS:
**
**	a_class		The class word of the command, NULL - DISK
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_CFG; S_CLASS, S_ITEM, S_ENDING, S_T4DIR.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_IVPARAM (signalled, severity F).
**--
*/
static	int	s_mhmon$check	(
	const	char *	a_class
			)
{
struct stat	l_st;
struct tm	l_tm;
time_t		l_now = time(NULL);
unsigned	l_h = 0, l_m = 0, l_s = 0;
int		l_n;
size_t		l_len;

	if ( a_class )
		{
		l_len = strlen(a_class);

		for ( s_class = 0; s_class <= MH$K_MON_ALL; s_class++ )
			if ( l_len && !strncasecmp(a_class, s_classes[s_class], l_len) )
				break;

		if ( s_class > MH$K_MON_ALL )
			return	$MHMSG(MH$_IVPARAM, a_class, "MHMON", "DISK, NODES, SERVER or ALL is expected");
		}

	if ( $ASCLEN(&s_cfg.item) )
		{
		for ( s_item = 0; s_item <= MH$K_ITEM_N; s_item++ )
			if ( !strncasecmp($ASCPTR(&s_cfg.item), s_items[s_item], $ASCLEN(&s_cfg.item)) )
				break;

		if ( s_item > MH$K_ITEM_N )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.item), "ITEM",
				"OPERATION_RATE, THROUGHPUT, QUEUE_LENGTH, RESPONSE_TIME or ALL is expected");
		}

	if ( (s_cfg.interval < 1) || (s_cfg.interval > MH$K_MON_MAXINT) )
		return	$MHMSG(MH$_IVPARAM, "", "INTERVAL", "1 - 3600 seconds is expected");

	if ( s_cfg.nodisplay && !$ASCLEN(&s_cfg.record) )
		return	$MHMSG(MH$_IVPARAM, "", "NODISPLAY", "/RECORD is needed with it");

	if ( $ASCLEN(&s_cfg.record) )
		{
		s_t4dir = !stat($ASCPTR(&s_cfg.record), &l_st) && S_ISDIR(l_st.st_mode);

		/*
		**  A directory with a slash at its end: the names of the files
		**  get one of their own
		*/
		while ( s_t4dir && ($ASCLEN(&s_cfg.record) > 1) && ($ASCPTR(&s_cfg.record)[$ASCLEN(&s_cfg.record) - 1] == '/') )
			$ASCPTR(&s_cfg.record)[--$ASCLEN(&s_cfg.record)] = '\0';
		}

	if ( $ASCLEN(&s_cfg.ending) )
		{
		l_n = sscanf($ASCPTR(&s_cfg.ending), "%u:%u:%u", &l_h, &l_m, &l_s);

		if ( (l_n < 2) || (l_h > 23) || (l_m > 59) || (l_s > 59) )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.ending), "ENDING", "hh:mm[:ss] is expected");

		localtime_r(&l_now, &l_tm);
		l_tm.tm_hour = (int) l_h;
		l_tm.tm_min = (int) l_m;
		l_tm.tm_sec = (int) l_s;
		l_tm.tm_isdst = -1;

		if ( (s_ending = mktime(&l_tm)) <= l_now )
			{
			l_tm.tm_mday++;
			l_tm.tm_isdst = -1;
			s_ending = mktime(&l_tm);
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The image: /HELP is answered first; the class word is taken out of
**	the command line and the rest are the qualifiers.  Then a sample
**	every /INTERVAL seconds, on the boundaries of the interval of the
**	clock, until a signal or /ENDING.
**
**  FORMAL PARAMETERS:
**
**	a_argc, a_argv	The command line
**
**  RETURN VALUE:
**	0 - ended by a signal or /ENDING; 1 - a condition of severity E or F.
**	(The signature of MAIN is the system's.)
**--
*/
int	main	(
		int		a_argc,
		char **		a_argv
			)
{
char			*l_argv [64], l_topic [256];
const char		*l_class = NULL;
int			l_argc = 1, l_up, l_status, l_waited = 0;
struct sigaction	l_sa = { .sa_handler = s_mhmon$sig };
struct timespec		l_ts;
time_t			l_now, l_next, l_last = 0;
unsigned short		l_tlen;
fao_desc_t		l_tdsc;

	mh$inimsg();

	/*
	**  /HELP[=topic]: the topics of MHMON hang below MHMON in the library
	**  of the server
	*/
	for ( int i = 1; i < a_argc; i++ )
		{
		const char *l_p = a_argv[i];

		l_p += (l_p[0] == '-' && l_p[1] == '-') ? 2 : ((l_p[0] == '/') ? 1 : 0);

		if ( (l_p != a_argv[i]) && !strncasecmp(l_p, "HELP", 4) && ((l_p[4] == '\0') || (l_p[4] == '=')) )
			{
			l_tdsc = (fao_desc_t) $FAO_DESC(l_topic);
			l_tlen = 0;
			__util$fao("MHMON !AZ", &l_tlen, &l_tdsc, (fao_prm_t) (l_p[4] ? l_p + 5 : ""));
			l_topic[l_tlen] = '\0';

			return	(1 & mh$help(l_topic)) ? 0 : 1;
			}
		}

	l_argv[0] = a_argv[0];

	for ( int i = 1; (i < a_argc) && (l_argc < (int) $ARRSZ(l_argv) - 1); i++ )
		{
		if ( (a_argv[i][0] != '/') && (a_argv[i][0] != '-') && !l_class )
			l_class = a_argv[i];
		else	l_argv[l_argc++] = a_argv[i];
		}

	l_argv[l_argc] = NULL;

	if ( !(1 & __util$getparams(l_argc, l_argv, s_opts)) )
		return	1;

	if ( !(1 & s_mhmon$check(l_class)) || !(1 & s_mhmon$find()) )
		return	1;

	sigaction(SIGINT, &l_sa, NULL);
	sigaction(SIGTERM, &l_sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	s_tty = isatty(STDOUT_FILENO);
	s_mhmon$reset();

	/*
	**  No server at the start is an error at a terminal; a recording
	**  waits for it, as a collector started with the system must
	*/
	/*
	**  The server of /NODE must be alive too: its SEQ moves
	*/
	if ( (1 & (l_status = s_mhmon$sample())) && $ASCLEN(&s_cfg.node) && !(1 & s_mhmon$alive(s_shmname)) )
		l_status = STS$K_ERROR;

	if ( !(1 & l_status) )
		{
		if ( l_status == MH$_STATVERS )
			return	1;

		if ( !$ASCLEN(&s_cfg.record) )
			{
			$MHMSG(MH$_NOSERVER, ", or it does not respond");
			return	1;
			}

		$MHMSG(MH$_NOSERVER, ", waiting for it");
		l_waited = 1;
		}

	while ( !s_stop )
		{
		/*
		**  The next boundary of the interval of the clock: the rows of a
		**  recording every minute fall on the minute
		*/
		clock_gettime(CLOCK_REALTIME, &l_ts);
		l_now = l_ts.tv_sec;
		l_next = (l_now / s_cfg.interval + 1) * s_cfg.interval;

		/*
		**  TIME(2) runs on the coarse clock and may be a tick behind the
		**  boundary just slept to: a boundary is never taken twice
		*/
		if ( l_next <= l_last )
			l_next = l_last + s_cfg.interval;

		l_last = l_next;

		if ( s_ending && (l_next > s_ending) )
			break;

		l_ts.tv_sec = l_next;
		l_ts.tv_nsec = 0;

		if ( clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &l_ts, NULL) && s_stop )
			break;

		l_up = 1 & s_mhmon$sample();

		if ( l_up && l_waited )
			l_waited = 0;

		if ( l_up )
			s_mhmon$compute();

		if ( !s_cfg.nodisplay && ((s_have == 2) || !l_up) )
			s_mhmon$show(l_up);

		if ( $ASCLEN(&s_cfg.record) && (s_have == 2 || !l_up) && !(1 & (l_status = s_mhmon$t4row(l_next, l_up))) )
			return	1;
		}

	s_mhmon$t4close(time(NULL));

	return	0;
}
