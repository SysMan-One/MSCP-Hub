#define	__MODULE__	"MSCPHUB"
#define	__IDENT__	"X01-11"
#define	__REV__		"1.11.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mscphub.c
**
**  ABSTRACT:	The head module: the parameters, the start-up checks and the
**		event loop of the server.
**
**  DESCRIPTION: The server makes local files and block devices of a Linux host
**		visible to OpenVMS nodes on the same LAN as MSCP disks, the
**		way an HSC makes its disks visible on a CI: it is an SCS node
**		with the MSCP$DISK system application and without a connection
**		manager, so it is never a member of a VMScluster and votes in
**		none.
**
**		The parameters come from the command line and from the file
**		/CONFIG names, one per line, in the form of the STARLET
**		library:
**
**		    /INTERFACE=mh0
**		    /NODE=MHUB
**		    /SYSTEMID=65200
**		    /GROUP=4242
**		    /AUTHORIZE=5A3C9E1700000000
**		    ! a comment
**
**		The protocol runs in one thread on EPOLL: the LAN socket, a
**		20 ms TIMERFD (the TR tick of NISCA), the EVENTFD of the
**		backend (the io_uring that does the I/O of the units) and a
**		SIGNALFD - SIGINT and SIGTERM stop the server, SIGUSR1 logs
**		how long it has run, the nodes and the counters.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-11 : /ACK=HOLD is the default.
**
**--
*/

#include	<errno.h>
#include	<string.h>
#include	<strings.h>
#include	<signal.h>
#include	<unistd.h>
#include	<sys/epoll.h>
#include	<sys/timerfd.h>
#include	<sys/signalfd.h>
#include	<time.h>

#include	"mh_def.h"

static	MH$CFG		s_cfg = {
	.listen	= MH$K_DEF_LISTEN
	};

static	MH$PORT		s_port;
static	time_t		s_start;		/* When the server started, for UPTIME		*/

static	const OPTS	s_opts [] = {
	{ { $ASCINI("config") },	&s_cfg.conf,	ASC$K_SZ,	OPTS$K_CONF },
	{ { $ASCINI("logfile") },	&s_cfg.logfile,	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("interface") },	&s_cfg.ifname,	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("node") },	&s_cfg.node,	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("systemid") },	&s_cfg.sysid,	0,		OPTS$K_INT },
	{ { $ASCINI("group") },	&s_cfg.group,	0,		OPTS$K_INT },
	{ { $ASCINI("authorize") },	&s_cfg.auth,	ASC$K_SZ,	OPTS$K_PWD },
	{ { $ASCINI("listen") },	&s_cfg.listen,	0,		OPTS$K_INT },
	{ { $ASCINI("passive") },	&s_cfg.passive,	0,		OPTS$K_OPT },
	{ { $ASCINI("trace") },	&s_cfg.trace,	0,		OPTS$K_OPT },
	{ { $ASCINI("alloclass") },	&s_cfg.alloclass, 0,		OPTS$K_INT },
	{ { $ASCINI("unsafe") },	&s_cfg.unsafe,	0,		OPTS$K_OPT },
	{ { $ASCINI("sync") },	&s_cfg.sync,	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("credits") },	&s_cfg.credits,	0,		OPTS$K_INT },
	{ { $ASCINI("timezone") },	&s_cfg.timezone, ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("ack") },	&s_cfg.ack,	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit0") },	&s_cfg.unit[0],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit1") },	&s_cfg.unit[1],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit2") },	&s_cfg.unit[2],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit3") },	&s_cfg.unit[3],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit4") },	&s_cfg.unit[4],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit5") },	&s_cfg.unit[5],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit6") },	&s_cfg.unit[6],	ASC$K_SZ,	OPTS$K_STR },
	{ { $ASCINI("unit7") },	&s_cfg.unit[7],	ASC$K_SZ,	OPTS$K_STR },

	{ { 0, { 0 } }, NULL, 0, 0 }					/* OPTS_NULL, braced for -Wmissing-braces	*/
	};

enum	{
	MH$K_EV_LAN	= 1,
	MH$K_EV_TIMER,
	MH$K_EV_SIGNAL,
	MH$K_EV_BK
	};



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Check the configuration and bring it to the form the protocol needs:
**	the SCSNODE in upper case, the mandatory parameters present, every
**	value in its range.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_CFG.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; a condition value of severity F that has been signalled
**	otherwise.
**--
*/
static	int	s_mh$check_cfg	(void)
{
char	l_val [32];

	if ( !$ASCLEN(&s_cfg.ifname) )
		return	$MHMSG(MH$_NOPARAM, "INTERFACE");

	if ( !$ASCLEN(&s_cfg.node) )
		return	$MHMSG(MH$_NOPARAM, "NODE");

	if ( !s_cfg.sysid )
		return	$MHMSG(MH$_NOPARAM, "SYSTEMID");

	if ( !s_cfg.group )
		return	$MHMSG(MH$_NOPARAM, "GROUP");

	if ( !$ASCLEN(&s_cfg.auth) )
		return	$MHMSG(MH$_NOPARAM, "AUTHORIZE");

	/*
	**  SCSNODE: 1 - 6 characters, A-Z, 0-9, $, _ ; upper case
	*/
	if ( $ASCLEN(&s_cfg.node) > NISCA$K_SZ_NODENAME )
		return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.node), "NODE", "at most 6 characters");

	for ( unsigned i = 0; i < $ASCLEN(&s_cfg.node); i++ )
		{
		char	l_c = $ASCPTR(&s_cfg.node)[i];

		if ( (l_c >= 'a') && (l_c <= 'z') )
			$ASCPTR(&s_cfg.node)[i] = l_c = (char) (l_c - 'a' + 'A');

		if ( !(((l_c >= 'A') && (l_c <= 'Z')) || ((l_c >= '0') && (l_c <= '9')) || (l_c == '$') || (l_c == '_')) )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.node), "NODE", "only A-Z, 0-9, $ and _ are allowed");
		}

	/*
	**  SCSSYSTEMID: v1 keeps it to the low word, which is all the DX
	**  address carries; SCSSYSTEMIDH is 0
	*/
	if ( (s_cfg.sysid < 1) || (s_cfg.sysid > 0xFFFF) )
		{
		__util$uint32_2_dec((uint32_t) s_cfg.sysid, l_val, sizeof(l_val), NULL);
		return	$MHMSG(MH$_IVPARAM, l_val, "SYSTEMID", "1 - 65535 is expected");
		}

	if ( (s_cfg.group < 1) || (s_cfg.group > NISCA$K_GROUP_MAX) )
		{
		__util$uint32_2_dec((uint32_t) s_cfg.group, l_val, sizeof(l_val), NULL);
		return	$MHMSG(MH$_IVPARAM, l_val, "GROUP", "1 - 65279 is expected");
		}

	if ( (s_cfg.alloclass < 0) || (s_cfg.alloclass > 255) )
		{
		__util$uint32_2_dec((uint32_t) s_cfg.alloclass, l_val, sizeof(l_val), NULL);
		return	$MHMSG(MH$_IVPARAM, l_val, "ALLOCLASS", "0 - 255 is expected");
		}

	if ( s_cfg.credits && ((s_cfg.credits < 2) || (s_cfg.credits > MH$K_MSCP_MAXCRED)) )
		{
		__util$uint32_2_dec((uint32_t) s_cfg.credits, l_val, sizeof(l_val), NULL);
		return	$MHMSG(MH$_IVPARAM, l_val, "CREDITS", "2 - 64 is expected");
		}

	if ( !(1 & mh$tz_set($ASCPTR(&s_cfg.timezone))) )
		return	$MHMSG(MH$_IVPARAM, $ASCPTR(&s_cfg.timezone), "TIMEZONE", "LOCAL, UTC or [+|-]hh[:mm] is expected");

	if ( (s_cfg.listen < 1) || (s_cfg.listen > 60) )
		{
		__util$uint32_2_dec((uint32_t) s_cfg.listen, l_val, sizeof(l_val), NULL);
		return	$MHMSG(MH$_IVPARAM, l_val, "LISTEN", "1 - 60 seconds is expected");
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Report what the server is doing, the SHOW of it: how long it has run,
**	every node it knows - the channel, the virtual circuit, the open SCS
**	connections - and the counters of the port, the transport and its
**	ACKs, the block transfers, the MSCP server and the units.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS:
**	S_PORT, S_CFG, S_START.
**
**  RETURN VALUE:
**	The condition value of the last message.
**--
*/
static	int	s_mh$stats	(void)
{
static	const char	*s_chstate [MH$K_CH_NSTATE] = { "closed", "CCSTART sent", "CCSTART taken", "OPEN", "HELLO taken" };
static	const char	*s_pbstate [MH$K_PB_NSTATE] = { "closed", "STACK sent", "OPEN" };
char			l_tz [32];
uint64_t		l_up, l_bare;
unsigned		l_ncon;

	l_up = (uint64_t) (time(NULL) - s_start);
	mh$tz_name(l_tz, sizeof(l_tz));
	$MHMSG(MH$_UPTIME, __IDENT__, $ASCPTR(&s_cfg.node), (unsigned) (l_up / 86400), (unsigned) ((l_up / 3600) % 24),
		(unsigned) ((l_up / 60) % 60), (unsigned) (l_up % 60), l_tz);

	for ( unsigned i = 0; i < s_port.nnode; i++ )
		{
		MH$NODE	*l_node = &s_port.nodes[i];

		l_ncon = 0;

		if ( l_node->vc )
			for ( unsigned j = 0; j < MH$K_MAXCDT; j++ )
				l_ncon += (l_node->vc->cdt[j].state == MH$K_CDT_OPEN);

		$MHMSG(MH$_NODESTATS, l_node->name, l_node->sysid, s_chstate[l_node->state], l_node->opens,
			l_node->vc ? s_pbstate[l_node->vc->pbstate] : "never started", l_node->vc ? l_node->vc->opens : 0, l_ncon);
		}

	$MHMSG(MH$_STATS, s_port.rx_frames, s_port.rx_cc, s_port.rx_tr, s_port.rx_drop, s_port.rx_own, s_port.rx_auth,
		s_port.tx_frames, s_port.tx_err);

	$MHMSG(MH$_TRSTATS, s_port.tr_tx, s_port.tr_rexmt, s_port.tr_rx, s_port.tr_dup, s_port.tr_ooo, s_port.tr_old, s_port.tr_maxwin);

	l_bare = 0;

	for ( unsigned i = 0; i < MH$K_ACKW_N; i++ )
		l_bare += s_port.tr_ackbare[i];

	$MHMSG(MH$_ACKSTATS, (s_port.ackmode == MH$K_ACK_HOLD) ? "HOLD" : "FLUSH", s_port.tr_rsvp, s_port.tr_ackpig, l_bare,
		s_port.tr_ackbare[MH$K_ACKW_RSVP], s_port.tr_ackbare[MH$K_ACKW_DUP], s_port.tr_ackbare[MH$K_ACKW_BATCH], MH$K_TR_ACKBATCH,
		s_port.tr_ackbare[MH$K_ACKW_FLUSH], s_port.tr_ackbare[MH$K_ACKW_TICK]);

	$MHMSG(MH$_BLKSTATS, s_port.blk_snt, s_port.blk_bytes_out, s_port.blk_cnf, s_port.blk_req, s_port.blk_ret, s_port.blk_bytes_in,
		s_port.blk_bad);

	return	mh$mscp_stats(&s_port);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The event loop: frames, the 20 ms tick, the signals.  It returns when
**	a signal stops the server or when a layer reports a condition of
**	severity F.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT INPUTS/OUTPUTS:
**	S_PORT.
**
**  RETURN VALUE:
**	STS$K_SUCCESS - stopped by a signal; the condition value of severity F
**	otherwise.
**--
*/
static	int	s_mh$loop	(void)
{
struct epoll_event		l_ev, l_evs [8];
struct itimerspec		l_its = {0};
struct signalfd_siginfo		l_si;
sigset_t			l_sigs;
uint8_t				l_frame [NISCA$K_MAXFRAME + 64];
uint64_t			l_expired;
unsigned			l_len;
int				l_ep, l_tfd, l_sfd, l_n, l_status = STS$K_SUCCESS;

	sigemptyset(&l_sigs);
	sigaddset(&l_sigs, SIGINT);
	sigaddset(&l_sigs, SIGTERM);
	sigaddset(&l_sigs, SIGUSR1);

	if ( sigprocmask(SIG_BLOCK, &l_sigs, NULL) )
		return	$MHMSG(MH$_SYSERR, "sigprocmask()", errno, strerror(errno));

	if ( 0 > (l_sfd = signalfd(-1, &l_sigs, SFD_NONBLOCK | SFD_CLOEXEC)) )
		return	$MHMSG(MH$_SYSERR, "signalfd()", errno, strerror(errno));

	if ( 0 > (l_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) )
		return	$MHMSG(MH$_SYSERR, "timerfd_create()", errno, strerror(errno));

	l_its.it_interval.tv_nsec	= MH$K_TICK_MS * 1000000L;
	l_its.it_value.tv_nsec		= MH$K_TICK_MS * 1000000L;

	if ( timerfd_settime(l_tfd, 0, &l_its, NULL) )
		return	$MHMSG(MH$_SYSERR, "timerfd_settime()", errno, strerror(errno));

	if ( 0 > (l_ep = epoll_create1(EPOLL_CLOEXEC)) )
		return	$MHMSG(MH$_SYSERR, "epoll_create1()", errno, strerror(errno));

	l_ev.events = EPOLLIN;

	l_ev.data.u32 = MH$K_EV_LAN;
	if ( epoll_ctl(l_ep, EPOLL_CTL_ADD, s_port.sd, &l_ev) )
		return	$MHMSG(MH$_SYSERR, "epoll_ctl(LAN)", errno, strerror(errno));

	l_ev.data.u32 = MH$K_EV_TIMER;
	if ( epoll_ctl(l_ep, EPOLL_CTL_ADD, l_tfd, &l_ev) )
		return	$MHMSG(MH$_SYSERR, "epoll_ctl(TIMER)", errno, strerror(errno));

	l_ev.data.u32 = MH$K_EV_SIGNAL;
	if ( epoll_ctl(l_ep, EPOLL_CTL_ADD, l_sfd, &l_ev) )
		return	$MHMSG(MH$_SYSERR, "epoll_ctl(SIGNAL)", errno, strerror(errno));

	/*
	**  The backend, when units are served
	*/
	l_ev.data.u32 = MH$K_EV_BK;
	if ( (mh$bk_fd() >= 0) && epoll_ctl(l_ep, EPOLL_CTL_ADD, mh$bk_fd(), &l_ev) )
		return	$MHMSG(MH$_SYSERR, "epoll_ctl(BACKEND)", errno, strerror(errno));

	for ( ;; )
		{
		if ( 0 > (l_n = epoll_wait(l_ep, l_evs, (int) $ARRSZ(l_evs), -1)) )
			{
			if ( errno == EINTR )
				continue;

			l_status = $MHMSG(MH$_SYSERR, "epoll_wait()", errno, strerror(errno));
			break;
			}

		for ( int i = 0; i < l_n; i++ )
			{
			switch ( l_evs[i].data.u32 )
				{
				case	MH$K_EV_LAN:
					/*
					**  Drain the socket; a condition of severity F
					**  from the layers above stops the server
					*/
					for ( ;; )
						{
						if ( !(1 & mh$lan_recv(&s_port, l_frame, sizeof(l_frame), &l_len)) || !l_len )
							break;

						l_status = mh$cc_input(&s_port, l_frame, l_len);

						if ( $DEFSEV(l_status) == STS$K_FATAL )
							goto	l_done;
						}

					/*
					**  Everything that came is taken: the ACKs owed go now
					*/
					mh$tr_flush(&s_port);

					break;

				case	MH$K_EV_TIMER:
					if ( sizeof(l_expired) != read(l_tfd, &l_expired, sizeof(l_expired)) )
						break;

					/*
					**  A stall of the process is not caught up tick by
					**  tick beyond a second: the protocol times are
					**  coarse anyway
					*/
					if ( l_expired > MH$K_TICKS_SEC )
						l_expired = MH$K_TICKS_SEC;

					for ( uint64_t j = 0; j < l_expired; j++ )
						mh$cc_tick(&s_port);

					mh$bk_tick(&s_port);

					break;

				case	MH$K_EV_BK:
					mh$bk_reap(&s_port);
					break;

				case	MH$K_EV_SIGNAL:
					if ( sizeof(l_si) != read(l_sfd, &l_si, sizeof(l_si)) )
						break;

					if ( l_si.ssi_signo == SIGUSR1 )
						{
						s_mh$stats();
						break;
						}

					$MHMSG(MH$_SHUTDOWN, l_si.ssi_signo);
					l_status = STS$K_SUCCESS;
					goto	l_done;
				}
			}
		}

l_done:
	close(l_ep);
	close(l_tfd);
	close(l_sfd);

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Entry point: the parameters, the checks, the port, the loop.
**
**  FORMAL PARAMETERS:
**
**	a_argc		Argument count
**	a_argv		Arguments: /NAME=value or --NAME=value
**
**  RETURN VALUE:
**	0 - stopped by a signal; 1 - a fatal condition has been signalled.
**--
*/
int	main	(
		int		a_argc,
		char **		a_argv
		)
{
char	l_macstr [3 * NISCA$K_SZ_MAC];
int	l_status;

	mh$inimsg();

	/*
	**  /HELP[=topic] is answered before anything else, the parameters
	**  that may come with it are not looked at
	*/
	for ( int i = 1; i < a_argc; i++ )
		{
		const char *l_p = a_argv[i];

		l_p += (l_p[0] == '-' && l_p[1] == '-') ? 2 : ((l_p[0] == '/') ? 1 : 0);

		if ( (l_p != a_argv[i]) && !strncasecmp(l_p, "HELP", 4) && ((l_p[4] == '\0') || (l_p[4] == '=')) )
			return	(1 & mh$help(l_p[4] ? l_p + 5 : "")) ? 0 : 1;
		}

	if ( !(1 & __util$getparams(a_argc, a_argv, s_opts)) )
		return	1;

	if ( $ASCLEN(&s_cfg.logfile) && !(1 & __util$deflog($ASCPTR(&s_cfg.logfile), NULL)) )
		return	1;

	if ( !(1 & s_mh$check_cfg()) )
		return	1;

	if ( !(1 & mh$lan_open(&s_port, $ASCPTR(&s_cfg.ifname))) )
		return	1;

	if ( !(1 & mh$cc_init(&s_port, &s_cfg)) )
		return	1;

	if ( !(1 & mh$mscp_init(&s_port, &s_cfg)) )
		return	1;

	mh$mac2str(s_port.mac, l_macstr, sizeof(l_macstr));

	$MHMSG(MH$_STARTUP, __IDENT__, $ASCPTR(&s_cfg.node), s_cfg.sysid, s_cfg.group, s_port.ifname, l_macstr);
	$MHMSG(MH$_LISTEN, s_cfg.listen);

	s_start = time(NULL);

	l_status = s_mh$loop();

	mh$cc_shutdown(&s_port);
	s_mh$stats();
	mh$lan_close(&s_port);

	return	(1 & l_status) ? 0 : 1;
}
