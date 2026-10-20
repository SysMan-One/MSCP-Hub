#define	__MODULE__	"T_WIRE"
#define	__IDENT__	"X01-10"
#define	__REV__		"1.10.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_wire.c
**
**  ABSTRACT:	Unit tests of the wire formats: the frames the server builds
**		are compared octet by octet with reference frames of PEDRIVER of
**		OpenVMS V8.3 (t_frames.h), and those frames are fed to the input side of the server.
**
**		The channel state machine is played against the reference
**		handshake: the server stands for one of the two nodes, the
**		frames of the other one go in, and what comes out must be what
**		PEDRIVER sent in its place.
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

#include	<stdio.h>
#include	<string.h>

#include	"mh_def.h"

#include	"t_frames.h"

static	int	s_fails, s_checks;

/*
**  What the server sent, by the XMIT hook of the port
*/
#define	T$K_MAXTX	8

static	uint8_t		s_tx [T$K_MAXTX] [NISCA$K_MAXFRAME];
static	unsigned	s_txlen [T$K_MAXTX], s_ntx;

#define	$CHECK(cond, what)	s_t$check((cond), (what), __LINE__)



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Count one check, report a failed one.
**
**  FORMAL PARAMETERS:
**
**	a_ok		The outcome
**	a_what		What has been checked
**	a_line		Line of the check
**
**  RETURN VALUE:
**	STS$K_SUCCESS - passed; STS$K_ERROR - failed.
**--
*/
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
	fprintf(stderr, "T_WIRE:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The XMIT hook of the test port: record the frame.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_frame		The frame MH$LAN_SEND has built
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - more frames than T$K_MAXTX.
**--
*/
static	int	s_t$xmit	(
		MH$PORT *	a_port,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
	if ( s_ntx >= T$K_MAXTX )
		return	STS$K_ERROR;

	memcpy(s_tx[s_ntx], a_frame, $MIN(a_len, sizeof(s_tx[0])));
	s_txlen[s_ntx++] = a_len;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A port made up as ALPHA1, without a socket.
**
**  FORMAL PARAMETERS:
**
**	a_port		Output: the port context
**	a_sysid		SCSSYSTEMID to give it
**	a_name		SCSNODE, ASCIZ
**	a_maclo		Last octet of its MAC, 08-00-2B-E5-40-xx
**
**  IMPLICIT INPUTS/OUTPUTS:
**	The frames it sends are recorded in S_TX, which is emptied.
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$port	(
		MH$PORT *	a_port,
		uint16_t	a_sysid,
	const	char *		a_name,
		uint8_t		a_maclo
			)
{
static	const uint8_t	s_mac [NISCA$K_SZ_MAC] = { 0x08, 0x00, 0x2B, 0xE5, 0x40, 0x01 };

	memset(a_port, 0, sizeof(*a_port));

	a_port->sd	= -1;
	a_port->xmit	= s_t$xmit;
	s_ntx		= 0;
	a_port->group	= 4242;
	a_port->sysid	= a_sysid;
	a_port->bufsz	= 1426;
	memcpy(a_port->mac, s_mac, NISCA$K_SZ_MAC);
	a_port->mac[5]	= a_maclo;
	memcpy(a_port->ifname, "EWA", 4);
	nisca$group_mac(a_port->group, a_port->gmac);
	mh$hex2bin("5A3C9E1700000000", 16, a_port->auth, MH$K_SZ_AUTH);

	a_port->ccname[0] = NISCA$K_SZ_NODENAME;
	memset(a_port->ccname + 1, ' ', NISCA$K_SZ_NODENAME);
	memcpy(a_port->ccname + 1, a_name, strlen(a_name));

	a_port->devname[0] = 3;
	memcpy(a_port->devname + 1, "EWA", 3);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Compare a CC message we built with one of PEDRIVER, over the fields
**	both versions carry and in which the server is meant to agree.  The
**	server differs on purpose in MINOR (V1.3), PIPE_QUOTA, P_SRV,
**	DEVICE_TYPE, and of course in TRANSMIT_TIME.
**
**  FORMAL PARAMETERS:
**
**	a_ours		Our message
**	a_theirs	PEDRIVER's
**	a_what		Name of the comparison
**
**  RETURN VALUE:
**	STS$K_SUCCESS - every compared octet agrees; STS$K_ERROR otherwise.
**--
*/
static	int	s_t$cmp_cc	(
	const	uint8_t *	a_ours,
	const	uint8_t *	a_theirs,
	const	char *		a_what
			)
{
char	l_what [128];
int	l_status = STS$K_SUCCESS;

	for ( unsigned i = 0; i < NISCA$K_CC_LEN_V13; i++ )
		{
		if ( (i == NISCA$K_CC_MINOR) || (i == NISCA$K_CC_PIPE_QUOTA) || (i == NISCA$K_CC_P_SRV)
			|| (i == NISCA$K_CC_DEVTYPE) || ((i >= NISCA$K_CC_XMT_TIME) && (i < NISCA$K_CC_XMT_TIME + 8)) )
			continue;

		if ( a_ours[i] == a_theirs[i] )
			continue;

		snprintf(l_what, sizeof(l_what), "%s: CC octet 0x%02X is %02X, PEDRIVER sent %02X", a_what, i, a_ours[i], a_theirs[i]);
		$CHECK(0, l_what);
		l_status = STS$K_ERROR;
		}

	return	l_status;
}


/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Compare a frame the server sent with one of PEDRIVER: the Ethernet
**	header, the DX header, then the CC message by S_T$CMP_CC.  The length
**	word differs - V1.3 against V1.5 - and is checked on its own.
**
**  FORMAL PARAMETERS:
**
**	a_ours		Our frame
**	a_ourlen	Its length
**	a_theirs	PEDRIVER's frame
**	a_what		Name of the comparison
**
**  RETURN VALUE:
**	STS$K_SUCCESS - every compared octet agrees; STS$K_ERROR otherwise.
**--
*/
static	int	s_t$cmp_frame	(
	const	uint8_t *	a_ours,
		unsigned	a_ourlen,
	const	uint8_t *	a_theirs,
	const	char *		a_what
			)
{
char	l_what [128];
int	l_status = STS$K_SUCCESS;

	snprintf(l_what, sizeof(l_what), "%s: Ethernet header", a_what);
	if ( !(1 & $CHECK(!memcmp(a_ours, a_theirs, NISCA$K_SZ_ETH), l_what)) )
		l_status = STS$K_ERROR;

	snprintf(l_what, sizeof(l_what), "%s: length word of a V1.3 message", a_what);
	if ( !(1 & $CHECK((a_ourlen == NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + NISCA$K_CC_LEN_V13)
		&& (nisca$get16(a_ours + NISCA$K_SZ_ETH) == NISCA$K_SZ_DX + NISCA$K_CC_LEN_V13), l_what)) )
		l_status = STS$K_ERROR;

	snprintf(l_what, sizeof(l_what), "%s: DX header", a_what);
	if ( !(1 & $CHECK(!memcmp(a_ours + NISCA$K_SZ_LANHDR, a_theirs + NISCA$K_SZ_LANHDR, NISCA$K_SZ_DX), l_what)) )
		l_status = STS$K_ERROR;

	if ( !(1 & s_t$cmp_cc(a_ours + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX, a_theirs + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX, a_what)) )
		l_status = STS$K_ERROR;

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Run the 20 ms clock of the port.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_ticks		How many ticks
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_t$ticks	(
		MH$PORT *	a_port,
		unsigned	a_ticks
			)
{
	for ( unsigned i = 0; i < a_ticks; i++ )
		mh$cc_tick(a_port);

	return	STS$K_SUCCESS;
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
MH$PORT		l_port;
MH$NODE		*l_node;
uint8_t		l_bye [sizeof(s_hello_alpha2)];
uint8_t		l_dx [NISCA$K_SZ_DX], l_cc [NISCA$K_CC_LEN_V15], l_mac [NISCA$K_SZ_MAC];
uint64_t	l_vms;
unsigned	l_len;
int		l_status;

	mh$inimsg();

	/*
	**  Addresses
	*/
	nisca$group_mac(4242, l_mac);
	$CHECK(!memcmp(l_mac, s_hello_alpha1, NISCA$K_SZ_MAC), "group multicast of 4242 is AB-00-04-01-92-11");

	nisca$node_mac(65534, l_mac);
	$CHECK(!memcmp(l_mac, s_hello_alpha1 + NISCA$K_SZ_LANHDR + NISCA$K_DX_SRC_LO, NISCA$K_SZ_MAC), "node address of 65534");

	$CHECK(nisca$get16(s_hello_alpha1 + NISCA$K_SZ_ETH) == sizeof(s_hello_alpha1) - NISCA$K_SZ_LANHDR, "length word = frame - 16");

	/*
	**  DX and CC built as ALPHA1 against what ALPHA1 sent
	*/
	s_t$port(&l_port, 65534, "ALPHA1", 0x01);

	mh$dx_build(&l_port, l_port.gmac, l_dx);
	$CHECK(!memcmp(l_dx, s_hello_alpha1 + NISCA$K_SZ_LANHDR, NISCA$K_SZ_DX), "DX of a HELLO");

	l_status = mh$cc_build(&l_port, NISCA$K_CC_HELLO, 0, l_cc, sizeof(l_cc), &l_len);
	$CHECK((1 & l_status) && (l_len == NISCA$K_CC_LEN_V13), "HELLO is built, V1.3 length");
	s_t$cmp_cc(l_cc, s_hello_alpha1 + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX, "HELLO");
	$CHECK(l_cc[NISCA$K_CC_MINOR] == MH$K_CC_MINOR, "MINOR 3");

	nisca$node_mac(65533, l_mac);
	mh$dx_build(&l_port, l_mac, l_dx);
	$CHECK(!memcmp(l_dx, s_ccstart_alpha1 + NISCA$K_SZ_LANHDR, NISCA$K_SZ_DX), "DX of a CCSTART to 65533");

	mh$cc_build(&l_port, NISCA$K_CC_CCSTART, 1, l_cc, sizeof(l_cc), &l_len);
	s_t$cmp_cc(l_cc, s_ccstart_alpha1 + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX, "CCSTART");

	/*
	**  Input: ALPHA1's HELLO is a new node of V1.6 / 104 octets; a server
	**  that is configured with the ID or the name of ALPHA1 must stop
	*/
	s_t$port(&l_port, 65200, "MHUB", 0x10);
	l_status = mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	$CHECK((1 & l_status) && (l_port.nnode == 1), "HELLO of ALPHA1 is taken in");
	$CHECK((l_port.nodes[0].sysid == 65534) && !strcmp(l_port.nodes[0].name, "ALPHA1"), "node ALPHA1, 65534");
	$CHECK((l_port.nodes[0].minor == 6) && (l_port.nodes[0].cclen == NISCA$K_CC_LEN_V15), "CC V1.6, 104 octets");

	l_status = mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	$CHECK((1 & l_status) && (l_port.nnode == 1) && (l_port.rx_cc == 2), "the same node is not added twice");

	s_t$port(&l_port, 65534, "MHUB", 0x10);
	l_status = mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	$CHECK(l_status == (int) MH$_IDCLASH, "the system ID of ALPHA1 is a clash");

	s_t$port(&l_port, 65200, "ALPHA1", 0x10);
	l_status = mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	$CHECK(l_status == (int) MH$_NAMECLASH, "the SCSNODE of ALPHA1 is a clash");

	/*
	**  A CCSTART addressed to another node, and a frame of another group,
	**  are none of our business
	*/
	s_t$port(&l_port, 65200, "MHUB", 0x10);
	l_status = mh$cc_input(&l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	$CHECK((1 & l_status) && !l_port.nnode && (l_port.rx_drop == 1), "a CCSTART to 65533 is dropped");

	l_port.group = 4243;
	nisca$group_mac(l_port.group, l_port.gmac);
	l_status = mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	$CHECK((1 & l_status) && !l_port.nnode, "a HELLO of group 4242 is dropped by group 4243");

	/*
	**  The channel, ALPHA1's side of the reference handshake: the HELLO
	**  of ALPHA2 makes our CCSTART 0.5 - 2 s later, the VERF of ALPHA2 our
	**  VACK, and the channel is open
	*/
	s_t$port(&l_port, 65534, "ALPHA1", 0x01);
	l_port.announcing = 1;
	l_port.next_hello = (uint64_t) -1;

	mh$cc_input(&l_port, s_hello_alpha2, sizeof(s_hello_alpha2));
	l_node = &l_port.nodes[0];
	$CHECK((l_port.nnode == 1) && (l_node->state == MH$K_CH_HELLO_DLY) && !s_ntx, "HELLO of a new node: HELLO_DLY, nothing sent yet");

	s_t$ticks(&l_port, MH$K_DLY_MAX);
	$CHECK((s_ntx == 1) && (l_node->state == MH$K_CH_CCSTART_SENT), "our CCSTART is out in 2 s at most");
	s_t$cmp_frame(s_tx[0], s_txlen[0], s_ccstart_alpha1, "CCSTART of the state machine");

	mh$cc_input(&l_port, s_verf_alpha2, sizeof(s_verf_alpha2));
	$CHECK((s_ntx == 2) && (l_node->state == MH$K_CH_OPEN) && (l_node->opens == 1), "VERF: our VACK, the channel is open");
	s_t$cmp_frame(s_tx[1], s_txlen[1], s_vack_alpha1, "VACK of the state machine");
	$CHECK((l_node->rmt_chseq == 1) && (l_node->pipe_quota == 16) && (l_node->bufsz == 1426) && (l_node->p_srv == 7),
		"what ALPHA2 told is taken: chseq 1, pipe quota 16, buffer 1426, services 7");

	mh$cc_input(&l_port, s_verf_alpha2, sizeof(s_verf_alpha2));
	$CHECK((s_ntx == 3) && (s_tx[2][NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX] == 0xB4) && (l_node->state == MH$K_CH_OPEN),
		"a VERF on the open channel (a probe) is answered by a VACK");

	s_t$ticks(&l_port, MH$K_LISTEN_TMO - 1);
	$CHECK(l_node->state == MH$K_CH_OPEN, "the channel stays open for 8 s without a frame");
	s_t$ticks(&l_port, 1);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && (l_node->lcl_chseq == 2), "... and is closed then, the next one is chseq 2");

	/*
	**  ALPHA2's side: the CCSTART of ALPHA1 makes our VERF, the VACK of
	**  ALPHA1 opens the channel; a CCSTART on the open channel restarts it
	*/
	s_t$port(&l_port, 65533, "ALPHA2", 0x02);
	l_port.announcing = 1;
	l_port.next_hello = (uint64_t) -1;

	mh$cc_input(&l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	l_node = &l_port.nodes[0];
	$CHECK((s_ntx == 1) && (l_node->state == MH$K_CH_CCSTART_REC), "CCSTART: our VERF, CCSTART_REC");
	s_t$cmp_frame(s_tx[0], s_txlen[0], s_verf_alpha2, "VERF of the state machine");

	mh$cc_input(&l_port, s_vack_alpha1, sizeof(s_vack_alpha1));
	$CHECK((s_ntx == 1) && (l_node->state == MH$K_CH_OPEN), "VACK: the channel is open, nothing is sent");

	mh$cc_input(&l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	$CHECK((s_ntx == 2) && (l_node->state == MH$K_CH_CCSTART_REC) && (l_node->lcl_chseq == 2)
		&& (nisca$get16(s_tx[1] + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + NISCA$K_CC_LCL_CHSEQ) == 2),
		"CCSTART on the open channel: a restart, our VERF of chseq 2");

	s_t$ticks(&l_port, MH$K_HS_TMO);
	$CHECK((l_node->state == MH$K_CH_CLOSED) && (l_node->lcl_chseq == 3) && (s_ntx == 2), "no VACK in 5 s: closed, nothing sent");

	/*
	**  BYE of ALPHA2 - its HELLO with the BYE type and the password -
	**  closes the open channel
	*/
	memcpy(l_bye, s_hello_alpha2, sizeof(s_hello_alpha2));
	l_bye[NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX] = NISCA$M_CC_CCFLG | NISCA$M_CC_OK_XMT | NISCA$M_CC_AUTHORIZE | NISCA$K_CC_BYE;
	mh$hex2bin("5A3C9E1700000000", 16, l_bye + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + NISCA$K_CC_AUTHORIZE, MH$K_SZ_AUTH);

	s_t$port(&l_port, 65534, "ALPHA1", 0x01);
	l_port.announcing = 1;
	l_port.next_hello = (uint64_t) -1;

	mh$cc_input(&l_port, s_hello_alpha2, sizeof(s_hello_alpha2));
	s_t$ticks(&l_port, MH$K_DLY_MAX);
	mh$cc_input(&l_port, s_verf_alpha2, sizeof(s_verf_alpha2));
	l_node = &l_port.nodes[0];
	mh$cc_input(&l_port, l_bye, sizeof(s_hello_alpha2));
	$CHECK((l_node->state == MH$K_CH_CLOSED) && (s_ntx == 2), "BYE closes the open channel");

	/*
	**  The stop of a server that has announced itself: BYE to the group,
	**  with the password
	*/
	l_status = mh$cc_shutdown(&l_port);
	$CHECK((1 & l_status) && (s_ntx == 3) && !memcmp(s_tx[2], l_port.gmac, NISCA$K_SZ_MAC)
		&& (s_tx[2][NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX] == 0xB1)
		&& !memcmp(s_tx[2] + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + NISCA$K_CC_AUTHORIZE, l_port.auth, MH$K_SZ_AUTH),
		"BYE to the group multicast, AUTHORIZE set, the hash in");

	/*
	**  A wrong password, /PASSIVE, the listen period: nothing is sent
	*/
	s_t$port(&l_port, 65533, "ALPHA2", 0x02);
	l_port.announcing = 1;
	l_port.auth[0] ^= 0x01;
	mh$cc_input(&l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	$CHECK(!s_ntx && (l_port.rx_auth == 1) && (l_port.nodes[0].state == MH$K_CH_CLOSED) && l_port.nodes[0].authfail,
		"a CCSTART with another password: AUTHFAIL, nothing sent");

	s_t$port(&l_port, 65533, "ALPHA2", 0x02);
	l_port.announcing = 1;
	l_port.passive = 1;
	mh$cc_input(&l_port, s_ccstart_alpha1, sizeof(s_ccstart_alpha1));
	mh$cc_input(&l_port, s_hello_alpha1, sizeof(s_hello_alpha1));
	s_t$ticks(&l_port, 10 * MH$K_TICKS_SEC);
	$CHECK(!s_ntx && (l_port.nodes[0].state == MH$K_CH_CLOSED), "/PASSIVE: nothing sent");

	s_t$port(&l_port, 65534, "ALPHA1", 0x01);
	l_port.next_hello = 2 * MH$K_TICKS_SEC;
	mh$cc_input(&l_port, s_hello_alpha2, sizeof(s_hello_alpha2));
	s_t$ticks(&l_port, 2 * MH$K_TICKS_SEC - 1);
	$CHECK(!s_ntx && (l_port.nodes[0].state == MH$K_CH_CLOSED), "the listen period: nothing sent, the node is only remembered");

	mh$cc_input(&l_port, s_hello_alpha2, sizeof(s_hello_alpha2));
	s_t$ticks(&l_port, 1);
	$CHECK((s_ntx == 1) && (s_tx[0][NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX] == 0xA0) && (l_port.nodes[0].state == MH$K_CH_HELLO_DLY),
		"end of the listen period: our HELLO, the node heard before gets our CCSTART");

	s_t$ticks(&l_port, MH$K_DLY_MAX);
	$CHECK((s_ntx >= 2) && (s_tx[1][NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX] == 0xB2), "... and here it is");

	/*
	**  Time: the epoch, and the TRANSMIT_TIME of the reference HELLO is in
	**  OCT-2026
	*/
	mh$unix2vms(0, 0, &l_vms);
	$CHECK(l_vms == 35067168000000000ULL, "VMS time of the UNIX epoch");

	l_vms = nisca$get64(s_hello_alpha1 + NISCA$K_SZ_LANHDR + NISCA$K_SZ_DX + NISCA$K_CC_XMT_TIME);
	l_vms = (l_vms - 35067168000000000ULL) / 10000000ULL;
	$CHECK((l_vms > 1790000000ULL) && (l_vms < 1793000000ULL), "TRANSMIT_TIME of the reference HELLO is in the autumn of 2026");

	/*
	**  /TIMEZONE: a fixed zone moves the time by its offset; a form that is
	**  none of LOCAL, UTC, [+|-]hh[:mm] is refused
	*/
	{
	uint64_t	l_utc, l_msk;
	char		l_tz [32];

	$CHECK((1 & mh$tz_set("UTC")) && (1 & mh$vmstime(&l_utc)), "/TIMEZONE=UTC");
	$CHECK((1 & mh$tz_set("+03:00")) && (1 & mh$vmstime(&l_msk)) && (l_msk - l_utc >= 3ULL * 3600 * 10000000ULL)
		&& (l_msk - l_utc < 3ULL * 3600 * 10000000ULL + 10000000ULL), "/TIMEZONE=+03:00 is 3 hours ahead of UTC");
	mh$tz_name(l_tz, sizeof(l_tz));
	$CHECK(!strcmp(l_tz, "UTC+03:00"), "... and is named UTC+03:00");
	$CHECK((1 & mh$tz_set("-5")) && (1 & mh$tz_set("+05:30")) && (1 & mh$tz_set("local")) && (1 & mh$tz_set("")),
		"-5, +05:30, local, nothing");
	$CHECK(!(1 & mh$tz_set("+15")) && !(1 & mh$tz_set("+3:7")) && !(1 & mh$tz_set("MSK")) && !(1 & mh$tz_set("+03:60")),
		"+15, +3:7, MSK, +03:60 are refused");
	mh$tz_set("LOCAL");
	}

	/*
	**  Conversions
	*/
	$CHECK(!(1 & mh$hex2bin("5A3C9E170000000", 15, l_mac, 8)), "an odd number of digits is refused");
	$CHECK(!(1 & mh$hex2bin("5A3C9E170000000G", 16, l_cc, 8)), "a non-digit is refused");

	printf("T_WIRE: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
