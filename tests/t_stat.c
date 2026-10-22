#define	__MODULE__	"T_STAT"
#define	__IDENT__	"X01-17"
#define	__REV__		"1.17.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_stat.c
**
**  ABSTRACT:	Unit tests of the statistics block (MHSTAT) and of the
**		answers of Modbus/TCP (MHMBUS): the block is made, copied in
**		every 100 ms, read whole while it is written from another
**		thread, refused when of another layout, removed; the queue of
**		a unit is summed every tick; the registers and the exceptions of
**		Modbus.
**
**		t_stat HOLD seconds - publish a block of a server TSnnnn whose
**		unit does 1000 reads a second, for MHMON to be tried on
**		(t4.sh).
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

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<strings.h>
#include	<unistd.h>
#include	<pthread.h>
#include	<time.h>
#include	<sys/stat.h>

#include	"mh_def.h"
#include	"mh_stat.h"

static	int		s_fails, s_checks;
static	MH$PORT		s_port;
static	MH$CFG		s_cfg;
static	volatile int	s_writing;

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
	fprintf(stderr, "T_STAT:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**  A server of one unit and one node, its name TSnnnn after the PID, so
**  that tests run at once do not share a block
*/
static	int	s_t$server	(void)
{
char	l_node [8];

	snprintf(l_node, sizeof(l_node), "TS%04u", (unsigned) getpid() % 10000);
	$ASCLEN(&s_cfg.node) = (unsigned char) strlen(l_node);
	memcpy($ASCPTR(&s_cfg.node), l_node, strlen(l_node) + 1);

	strcpy(s_port.ifname, "lo");
	s_port.sysid	= 4321;
	s_port.group	= 77;
	s_port.alloclass = 42;
	s_port.nunit	= 1;
	strcpy(s_port.units[0].name, "DKA100");
	strcpy(s_port.units[0].dvname, "$42$DKA100");
	strcpy(s_port.units[0].ddr, "RZ29");
	strcpy(s_port.units[0].path, "/dev/null");
	s_port.units[0].size = 2048;
	s_port.units[0].dev.ssz = 512;
	s_port.nnode	= 1;
	strcpy(s_port.nodes[0].name, "ALPHA1");
	s_port.nodes[0].sysid = 65534;
	s_port.nodes[0].state = MH$K_CH_OPEN;

	return	STS$K_SUCCESS;
}



/*
**  The writer: RX and TX frames always equal, so that a copy taken in the
**  middle of a write shows them apart
*/
static	void *	s_t$writer	(
		void *		a_arg
			)
{
	(void) a_arg;

	while ( s_writing )
		{
		s_port.rx_frames++;
		s_port.tx_frames = s_port.rx_frames;
		s_port.units[0].nread = s_port.rx_frames;
		mh$stat_tick(&s_port);
		}

	return	NULL;
}



/*
**  HOLD: a block for MHMON, 1000 reads a second, 100 every 100 ms
*/
static	int	s_t$hold	(
		int		a_secs
			)
{
struct timespec	l_ts = { 0, 20 * 1000000 }, l_t0, l_now;
uint64_t	l_ns;

	s_t$server();

	if ( !(1 & mh$stat_open(&s_port, &s_cfg, __IDENT__)) )
		return	1;

	printf("%s\n", $ASCPTR(&s_cfg.node));
	fflush(stdout);

	/*
	**  The counters follow the clock, not the loop: 1000 reads a second
	**  however slowly the loop goes round on a busy computer
	*/
	clock_gettime(CLOCK_MONOTONIC, &l_t0);

	do	{
		clock_gettime(CLOCK_MONOTONIC, &l_now);
		l_ns = (uint64_t) (l_now.tv_sec - l_t0.tv_sec) * 1000000000ULL + (uint64_t) l_now.tv_nsec - (uint64_t) l_t0.tv_nsec;
		s_port.units[0].nread = l_ns / 1000000;
		s_port.units[0].rbytes = s_port.units[0].nread * 512;
		s_port.rx_frames = l_ns / 10000000;
		s_port.tick++;
		mh$stat_tick(&s_port);
		nanosleep(&l_ts, NULL);
		} while ( l_ns < (uint64_t) a_secs * 1000000000ULL );

	mh$stat_close();

	return	0;
}



int	main	(
		int		a_argc,
		char **		a_argv
			)
{
MH$STAT		l_copy, *l_fake;
struct stat	l_st;
char		l_path [320];
pthread_t	l_thr;
uint8_t		l_pdu [260];
unsigned	l_rlen, l_torn = 0;
uint64_t	l_qint;

	mh$inimsg();

	if ( (a_argc == 3) && !strcasecmp(a_argv[1], "HOLD") )
		return	s_t$hold(atoi(a_argv[2]));

	s_t$server();

	/*
	**  Made, with its mode, and whole at once
	*/
	$CHECK(1 & mh$stat_open(&s_port, &s_cfg, "X01-17"), "the block is made");
	snprintf(l_path, sizeof(l_path), MH$T_STAT_SHMDIR MH$T_STAT_PREFIX "%s", $ASCPTR(&s_cfg.node));
	$CHECK(!stat(l_path, &l_st) && ((l_st.st_mode & 0777) == MH$K_STAT_MODE), "/dev/shm/mscphub.<node>, mode 0640");
	$CHECK(1 & mh$stat_read(mh$stat_block(), &l_copy), "a whole copy");
	$CHECK(!strcmp(l_copy.node, $ASCPTR(&s_cfg.node)) && !strcmp(l_copy.ident, "X01-17") && (l_copy.sysid == 4321),
		"who the server is");
	$CHECK((l_copy.nunit == 1) && !strcmp(l_copy.units[0].dvname, "$42$DKA100") && (l_copy.units[0].size == 2048), "the unit");
	$CHECK((l_copy.nnode == 1) && !strcmp(l_copy.nodes[0].name, "ALPHA1") && (l_copy.nodes[0].chstate == MH$K_STCH_OPEN)
		&& (l_copy.nodes[0].vcstate == MH$K_STVC_NONE), "the node");
	$CHECK(l_copy.pid == (uint32_t) getpid(), "the PID of the server");

	/*
	**  Copied in every 5 ticks, not every tick
	*/
	s_port.rx_frames = 1000;

	for ( int i = 0; i < MH$K_STAT_TICKS - 1; i++ )
		mh$stat_tick(&s_port);

	mh$stat_read(mh$stat_block(), &l_copy);
	$CHECK(l_copy.rx_frames != 1000, "not copied before 100 ms");
	mh$stat_tick(&s_port);
	mh$stat_read(mh$stat_block(), &l_copy);
	$CHECK(l_copy.rx_frames == 1000, "copied at 100 ms");

	/*
	**  The queue: 2 running and 1 waiting, 10 ticks - 30
	*/
	{
	MH$HRB	l_hrb = { 0 };

	l_qint = s_port.units[0].qint;
	s_port.units[0].current = 2;
	s_port.units[0].qhead = &l_hrb;

	for ( int i = 0; i < 10; i++ )
		mh$stat_tick(&s_port);

	$CHECK(s_port.units[0].qint - l_qint == 30, "the queue summed every tick");
	s_port.units[0].current = 0;
	s_port.units[0].qhead = NULL;
	}

	/*
	**  Never a torn copy, while another thread writes as fast as it can
	*/
	s_port.tx_frames = s_port.units[0].nread = s_port.rx_frames;

	for ( int i = 0; i < MH$K_STAT_TICKS; i++ )
		mh$stat_tick(&s_port);

	s_writing = 1;
	pthread_create(&l_thr, NULL, s_t$writer, NULL);

	for ( int i = 0; i < 5000; i++ )
		if ( (1 & mh$stat_read(mh$stat_block(), &l_copy)) && ((l_copy.rx_frames != l_copy.tx_frames)
			|| (l_copy.units[0].nread != l_copy.rx_frames)) )
			{
			if ( !l_torn )
				fprintf(stderr, "T_STAT: torn: seq %u rx %llu tx %llu nread %llu\n", l_copy.seq, (unsigned long long) l_copy.rx_frames,
					(unsigned long long) l_copy.tx_frames, (unsigned long long) l_copy.units[0].nread);

			l_torn++;
			}

	s_writing = 0;
	pthread_join(l_thr, NULL);
	$CHECK(!l_torn, "no torn copy under a writer");

	/*
	**  A block of another layout is refused
	*/
	l_fake = malloc(sizeof(MH$STAT));
	memcpy(l_fake, mh$stat_block(), sizeof(MH$STAT));
	l_fake->version = MH$K_STAT_VERSION + 1;
	$CHECK(!(1 & mh$stat_read(l_fake, &l_copy)), "another version refused");
	l_fake->version = MH$K_STAT_VERSION;
	l_fake->magic[0] = 'X';
	$CHECK(!(1 & mh$stat_read(l_fake, &l_copy)), "another magic refused");
	free(l_fake);

	/*
	**  Modbus: the map, the device, the exceptions
	*/
	mh$stat_tick(&s_port);

	for ( int i = 0; i < MH$K_STAT_TICKS; i++ )
		mh$stat_tick(&s_port);

	memcpy(l_pdu, (uint8_t []) { 0x04, 0x00, 0x00, 0x00, 0x12 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 2 + 36) && (l_pdu[1] == 36), "FC04: 18 registers");
	$CHECK((l_pdu[2] == 0) && (l_pdu[3] == 1) && (l_pdu[5] == 1) && (l_pdu[7] == 1), "map 1, one unit, one node");
	$CHECK(((l_pdu[8] << 8) | l_pdu[9]) == 4321, "the system ID");
	$CHECK(!memcmp(l_pdu + 2 + 2 * 10, "X01-17", 6), "the IDENT, two characters a register");

	memcpy(l_pdu, (uint8_t []) { 0x03, 0x03, 0xE8, 0x00, 0x08 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 18) && !memcmp(l_pdu + 2, "$42$DKA100", 10), "FC03 at 1000: the name of unit 0");

	memcpy(l_pdu, (uint8_t []) { 0x04, 0x07, 0xD0, 0x00, 0x04 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 10) && !memcmp(l_pdu + 2, "ALPHA1", 6), "at 2000: the name of node 0");

	memcpy(l_pdu, (uint8_t []) { 0x04, 0xFF, 0x00, 0x00, 0x02 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 2) && (l_pdu[0] == 0x84) && (l_pdu[1] == 0x02), "out of the map: ILLEGAL DATA ADDRESS");

	memcpy(l_pdu, (uint8_t []) { 0x04, 0x00, 0x00, 0x00, 0x00 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 2) && (l_pdu[0] == 0x84) && (l_pdu[1] == 0x03), "no register: ILLEGAL DATA VALUE");

	memcpy(l_pdu, (uint8_t []) { 0x04, 0x00, 0x00, 0x00, 0x7E }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 2) && (l_pdu[1] == 0x03), "126 registers: ILLEGAL DATA VALUE");

	memcpy(l_pdu, (uint8_t []) { 0x06, 0x00, 0x00, 0x00, 0x01 }, 5);
	mh$mbus_answer(l_pdu, 5, &l_rlen);
	$CHECK((l_rlen == 2) && (l_pdu[0] == 0x86) && (l_pdu[1] == 0x01), "a write: ILLEGAL FUNCTION");

	memcpy(l_pdu, (uint8_t []) { 0x2B, 0x0E, 0x01, 0x00 }, 4);
	mh$mbus_answer(l_pdu, 4, &l_rlen);
	$CHECK((l_pdu[6] == 3) && !memcmp(l_pdu + 9, "StarLet Squad", 13), "2B/0E: three objects, the vendor first");

	/*
	**  Removed
	*/
	mh$stat_close();
	$CHECK(stat(l_path, &l_st) && !mh$stat_block(), "the block is removed");

	printf("T_STAT: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
