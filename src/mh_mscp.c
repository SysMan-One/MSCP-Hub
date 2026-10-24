#define	__MODULE__	"MHMSCP"
#define	__IDENT__	"X01-21"
#define	__REV__		"1.21.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_mscp.c
**
**  ABSTRACT:	The MSCP server: the units, the hosts (one per connection of
**		a client DUDRIVER), the check of a command, the order of the
**		commands on a unit and the commands themselves - SET
**		CONTROLLER CHARACTERISTICS, GET UNIT STATUS, ONLINE, AVAILABLE,
**		READ, WRITE, ERASE, COMPARE HOST DATA, ABORT, GET COMMAND STATUS
**		and the small ones.
**
**  DESCRIPTION: A command is checked in this order:
**		length, opcode, FLAGS, MODIFIER, unit;
**		a bad one is answered ICMD with the offset of the bad field in
**		the high octet of STATUS, 36 octets.  The end message is built
**		in a copy of the command, its length from END_PKT_LEN.
**
**		The immediate ones (opcode / 8 = 0 or 6: ABORT, GTCMD, GUS,
**		STCON, GTUNM, TERCO) are answered at once.  Every other one
**		becomes a command in progress (MH$HRB) and keeps the order of
**		its unit: a sequential one (ONLINE, AVAILABLE,
**		STUNT, DTACP) waits until nothing else runs on the unit and
**		holds back what comes after it; the others (READ, WRITE, ERASE,
**		COMPARE, FLUSH, CMPCD) run side by side.  ONLINE and AVAILABLE
**		are so the barriers the client counts on.
**
**		The steps of the commands that move data (the
**		checks, in this order: OFFLN, AVLBL, WRTPR, BYTE_CNT = 0 -> SUCC,
**		past the end -> ICMD on +0C):
**
**		  READ	 the disk into a buffer (MHBK) -> SEND_DATA to the buffer
**			 of the client (MHBLK) -> RET_CNF -> END
**		  WRITE	 REQUEST_DATA -> RET_DAT_LP -> the disk, fdatasync ->
**			 END: SUCC is said only of the data on the medium
**		  COMP	 REQUEST_DATA -> the disk into a second buffer, the two
**			 compared -> SUCC or COMP (0x0007) - honestly: a copy of
**			 a shadow set takes SUCC for "nothing to write"
**		  ERASE	 fallocate(ZERO_RANGE), fdatasync -> END
**
**		The modifier COMP on READ asks the data back from the client
**		after it has gone and compares; on WRITE it reads the disk back
**		after the write.  BYTE_CNT of the END is what has been moved.
**		Data buffers are taken from a pool of 16 MB; a command that
**		finds it empty waits (BUF_WAIT), as with the VMS server.
**
**		ABORT: a command waiting for its turn or for memory ends at once
**		with ABRTD; one in progress is marked and ends ABRTD at its next
**		step, before anything is written.  GET COMMAND STATUS: CMD_STS
**		starts at -2 and goes down by one at every step and every chunk
**		of data, so the client sees that the command moves.
**
**		An end message that a VMS client would take for a reason to
**		crash is never sent: a unit
**		number with no SLUN bit or of
**		0x7FFF in an answer of GUS, a geometry with a zero low octet,
**		a controller letter out of 1 .. 26 in UNIT_ID.  It is held,
**		MSCPERR is signalled and the connection is ended.
**
**		Units: /UNITn=name,path[,RO] - name DKA100 and the like (the
**		letters D K of the name are those of the medium, RZ29); the
**		size is that of the file at the start, in blocks of 512.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  17-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		20-OCT-2026	RRL	X01-10 : Initial public version.
**		20-OCT-2026	RRL	X01-13 : BKSTATS: the sectors an RMW reads.
**		20-OCT-2026	RRL	X01-14 : BKSTATS: a model of a cache of the last 2, 16, 128,
**						512 sectors written.
**		20-OCT-2026	RRL	X01-14 : a cache of the 128 sectors last written: an RMW
**						reads only the edges it does not find there.
**		21-OCT-2026	RRL	X01-15 : ZERO of a file up to 1 MB: zeros written, not
**						fallocate (unwritten extents); BKSTATS counts ZERO.
**		25-OCT-2026	RRL	X01-21 : MH$MSCP_CHECK_SERVICE: the system disk a satellite asks for.
**
**--
*/

#include	<errno.h>
#include	<fcntl.h>
#include	<stdio.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<sys/file.h>
#include	<sys/stat.h>

#include	"mh_def.h"
#include	"mscp.h"

static	int	s_mh$hrb_purge	(MH$PORT *a_port, MH$CDT *a_cdt);

/*
**  Per opcode: the length of the command (0 - not taken), of its end
**  message, and the modifiers it may carry (COM_PKT_LEN, END_PKT_LEN,
**  the modifiers)
*/
typedef struct mh_opdef_t {
	uint8_t		cmdlen, endlen;
	uint16_t	mods;
	const char *	name;
} MH$OPDEF;

static	const MH$OPDEF	s_opdef [MSCP$K_OP_MAX] = {
	[MSCP$K_OP_ABORT]	= { 16, 16, 0x0000, "ABORT" },
	[MSCP$K_OP_GTCMD]	= { 20, 20, 0x0000, "GET COMMAND STATUS" },
	[MSCP$K_OP_GTUNT]	= { 12, 52, 0x2001, "GET UNIT STATUS" },
	[MSCP$K_OP_STCON]	= { 28, 32, 0x0000, "SET CONTROLLER CHARACTERISTICS" },
	[MSCP$K_OP_GTUNM]	= { 12, 44, 0x0000, "GET UNIT NAME" },
	[MSCP$K_OP_AVAIL]	= { 12, 12, 0x2003, "AVAILABLE" },
	[MSCP$K_OP_ONLIN]	= { 36, 44, 0x2017, "ONLINE" },
	[MSCP$K_OP_STUNT]	= { 36, 44, 0x2014, "SET UNIT CHARACTERISTICS" },
	[MSCP$K_OP_DTACP]	= { 12, 12, 0x0000, "DETERMINE ACCESS PATHS" },
	[MSCP$K_OP_CMPCD]	= { 32, 32, 0xAB80, "COMPARE CONTROLLER DATA" },
	[MSCP$K_OP_ERASE]	= { 32, 36, 0xB1F8, "ERASE" },
	[MSCP$K_OP_FLUSH]	= { 32, 32, 0xA383, "FLUSH" },
	[MSCP$K_OP_COMP]	= { 32, 32, 0xAB80, "COMPARE HOST DATA" },
	[MSCP$K_OP_READ]	= { 32, 32, 0xEB80, "READ" },
	[MSCP$K_OP_WRITE]	= { 32, 36, 0xF3F8, "WRITE" },
	[MSCP$K_OP_TERCO]	= { 36, 36, 0x0000, "TERMINATE CLASS DRIVER CONNECTION" }
	};

/*
**  The answer of GET UNIT STATUS of a unit: a fixed geometry - neither TRACK nor GROUP x CYLINDER may have a zero low octet
*/
#define	MH$K_GEO_TRACK		255
#define	MH$K_GEO_GROUP		255
#define	MH$K_GEO_CYL		1



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	MEDIA_ID of a medium named like RZ29 or RRD42 for a unit named DKcnnnn:
**	D0 D1 - the first two letters of the unit name, then the two or
**	three letters and the number of the medium.
**
**  FORMAL PARAMETERS:
**
**	a_unit		The unit name, ASCIZ, upper case
**	a_media		The name of the medium, ASCIZ, upper case
**	a_id		Output: MEDIA_ID
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - not 2 or 3 letters and 0 ... 127.
**--
*/
static	int	s_mh$mscp_media	(
	const	char *		a_unit,
	const	char *		a_media,
		uint32_t *	a_id
			)
{
unsigned	l_nlet, l_num, l_ndig;
uint32_t	l_id;

	for ( l_nlet = 0; (a_media[l_nlet] >= 'A') && (a_media[l_nlet] <= 'Z'); l_nlet++ )
		;

	l_ndig = (unsigned) strspn(a_media + l_nlet, "0123456789");

	if ( (l_nlet < 2) || (l_nlet > 3) || !l_ndig || (l_ndig > 3) || a_media[l_nlet + l_ndig] || ((l_num = (unsigned) atoi(a_media + l_nlet)) > 127) )
		return	STS$K_ERROR;

	l_id = ((uint32_t) (a_unit[0] - '@') << 27) | ((uint32_t) (a_unit[1] - '@') << 22) | ((uint32_t) (a_media[0] - '@') << 17)
		| ((uint32_t) (a_media[1] - '@') << 12) | l_num;

	if ( l_nlet == 3 )
		l_id |= (uint32_t) (a_media[2] - '@') << 7;

	*a_id = l_id;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make the units of the configuration:
**
**		/UNITn=name,path[,RO][,MEDIA=medium][,SECTOR=octets]
**
**	The name is of the form DKA100 - two letters, the controller letter,
**	a number up to 9999.  The path is a file (its size at the start is
**	the size of the unit) or a block device; MHBK opens and locks it.
**	RO: read only, the medium is RRD42 (a CD-ROM) unless MEDIA says else;
**	otherwise RZ29.  SECTOR: a file taken as a disk of sectors of that
**	size (tests of the RMW of 4K disks).  Then the ring of the backend is
**	started.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cfg		The configuration
**
**  RETURN VALUE:
**	STS$K_SUCCESS; a condition value of severity F that has been signalled.
**--
*/
int	mh$mscp_init	(
		MH$PORT *	a_port,
	const	MH$CFG *	a_cfg
			)
{
MH$UNIT		*l_unit;
char		l_spec [ASC$K_SZ + 1], *l_path, *l_opt, *l_next, l_what [16], l_kind [64], l_media [8], l_dvname [16];
unsigned	l_num, l_len, l_ssz;
uint64_t	l_size;
int		l_status, l_ro;

	a_port->alloclass = (uint8_t) a_cfg->alloclass;
	a_port->credits	= (uint16_t) (a_cfg->credits ? a_cfg->credits : MH$K_MSCP_CREDITS);

	/*
	**  /SYNC; /UNSAFE is /SYNC=NONE
	*/
	a_port->sync = a_cfg->unsafe ? MH$K_SYNC_NONE : MH$K_SYNC_DSYNC;

	if ( (l_len = $ASCLEN(&a_cfg->sync)) )
		{
		if ( (l_len == 5) && !strncasecmp($ASCPTR(&a_cfg->sync), "DSYNC", 5) )
			a_port->sync = MH$K_SYNC_DSYNC;
		else if ( (l_len == 5) && !strncasecmp($ASCPTR(&a_cfg->sync), "GROUP", 5) )
			a_port->sync = MH$K_SYNC_GROUP;
		else if ( (l_len == 4) && !strncasecmp($ASCPTR(&a_cfg->sync), "NONE", 4) )
			a_port->sync = MH$K_SYNC_NONE;
		else	return	$MHMSG(MH$_IVPARAM, $ASCPTR(&a_cfg->sync), "SYNC", "DSYNC, GROUP or NONE is expected");

		if ( a_cfg->unsafe && (a_port->sync != MH$K_SYNC_NONE) )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&a_cfg->sync), "SYNC", "/UNSAFE is /SYNC=NONE");
		}

	for ( unsigned i = 0; i < MH$K_MAXUNIT; i++ )
		{
		if ( !(l_len = $ASCLEN(&a_cfg->unit[i])) )
			continue;

		snprintf(l_what, sizeof(l_what), "UNIT%u", i);
		memcpy(l_spec, $ASCPTR(&a_cfg->unit[i]), l_len);
		l_spec[l_len] = '\0';

		if ( !(l_path = strchr(l_spec, ',')) )
			return	$MHMSG(MH$_IVPARAM, l_spec, l_what, "name,path[,RO][,MEDIA=m][,SECTOR=n] is expected");

		*l_path++ = '\0';

		if ( (l_opt = strchr(l_path, ',')) )
			*l_opt++ = '\0';

		l_unit = &a_port->units[a_port->nunit];
		memset(l_unit, 0, sizeof(*l_unit));
		l_unit->dev.fd = -1;

		for ( char *l_p = l_spec; *l_p; l_p++ )
			{
			if ( (*l_p >= 'a') && (*l_p <= 'z') )
				*l_p = (char) (*l_p - 'a' + 'A');
			}

		/*
		**  DK, the controller letter, the number; the letters D K are
		**  those of the medium - another pair would be MEDOFL at the
		**  client
		*/
		if ( (strlen(l_spec) < 4) || (strlen(l_spec) > 7) || memcmp(l_spec, "DK", 2) || (l_spec[2] < 'A') || (l_spec[2] > 'Z')
			|| (strspn(l_spec + 3, "0123456789") != strlen(l_spec + 3)) || ((l_num = (unsigned) atoi(l_spec + 3)) > 9999) )
			return	$MHMSG(MH$_IVPARAM, l_spec, l_what, "a name DKcnnnn (c - A ... Z, nnnn - 0 ... 9999) is expected");

		for ( unsigned j = 0; j < a_port->nunit; j++ )
			{
			if ( !strcmp(a_port->units[j].name, l_spec) )
				return	$MHMSG(MH$_IVPARAM, l_spec, l_what, "the name is given to another unit already");
			}

		/*
		**  The options, in any order
		*/
		l_ro = 0;
		l_ssz = 0;
		l_media[0] = '\0';

		for ( ; l_opt && *l_opt; l_opt = l_next )
			{
			if ( (l_next = strchr(l_opt, ',')) )
				*l_next++ = '\0';

			for ( char *l_p = l_opt; *l_p; l_p++ )
				{
				if ( (*l_p >= 'a') && (*l_p <= 'z') )
					*l_p = (char) (*l_p - 'a' + 'A');
				}

			if ( !strcmp(l_opt, "RO") )
				l_ro = 1;
			else if ( !strncmp(l_opt, "MEDIA=", 6) && (strlen(l_opt + 6) < sizeof(l_media)) )
				strcpy(l_media, l_opt + 6);
			else if ( !strncmp(l_opt, "SECTOR=", 7) && (strspn(l_opt + 7, "0123456789") == strlen(l_opt + 7)) && l_opt[7] )
				l_ssz = (unsigned) atoi(l_opt + 7);
			else	return	$MHMSG(MH$_IVPARAM, l_opt, l_what, "RO, MEDIA=medium or SECTOR=octets is expected after the path");
			}

		if ( !l_media[0] )
			strcpy(l_media, l_ro ? "RRD42" : "RZ29");

		if ( !(1 & s_mh$mscp_media(l_spec, l_media, &l_unit->media)) )
			return	$MHMSG(MH$_IVPARAM, l_media, l_what, "a medium of 2 or 3 letters and 0 ... 127 is expected: RZ29, RRD42");

		if ( l_ssz && ((l_ssz < 512) || (l_ssz > 4096) || (l_ssz & (l_ssz - 1))) )
			return	$MHMSG(MH$_IVPARAM, "SECTOR", l_what, "a SECTOR of 512, 1024, 2048 or 4096 is expected");

		if ( !(1 & (l_status = mh$bk_open(&l_unit->dev, l_path, l_ro, l_ssz, &l_size))) )
			return	l_status;

		if ( (l_size < 512) || (l_size / 512 > 0xFFFFFFFFULL) )
			return	$MHMSG(MH$_IVPARAM, l_path, l_what, "a file or a device of 512 octets ... 2 TB is expected");

		if ( l_ssz && (l_unit->dev.type == MH$K_DEV_BLK) )
			return	$MHMSG(MH$_IVPARAM, l_path, l_what, "SECTOR is for a file; a device tells its own");

		memcpy(l_unit->name, l_spec, strlen(l_spec) + 1);
		snprintf(l_unit->path, sizeof(l_unit->path), "%s", l_path);
		strcpy(l_unit->ddr, l_media);

		/*
		**  The name the nodes give it: $42$DKA100; MHUB$DKA100 with class 0
		*/
		if ( a_port->alloclass )
			snprintf(l_dvname, sizeof(l_dvname), "$%u$%.7s", a_port->alloclass, l_spec);
		else	snprintf(l_dvname, sizeof(l_dvname), "%.6s$%.7s", $ASCPTR(&a_cfg->node), l_spec);

		memcpy(l_unit->dvname, l_dvname, sizeof(l_unit->dvname));
		l_unit->ro	= l_ro;
		l_unit->size	= (uint32_t) (l_size / 512);
		l_unit->volser	= 0x4D480000U | l_num;

		/*
		**  UNIT_ID: allocation class, number, the packed name
		*/
		nisca$put32(l_unit->unitid, a_port->alloclass);
		nisca$put16(l_unit->unitid + 4, (uint16_t) l_num);
		nisca$put16(l_unit->unitid + 6, (uint16_t) (((l_spec[0] - 0x40) << 10) | ((l_spec[1] - 0x40) << 5) | (l_spec[2] - 0x40)));

		snprintf(l_kind, sizeof(l_kind), ", %s%s, sector %u, %s", (l_unit->dev.type == MH$K_DEV_BLK) ? "a block device" : "a file",
			l_ro ? ", read only" : "", l_unit->dev.ssz, l_media);

		$MHMSG(MH$_UNITCFG, l_unit->dvname, l_unit->path, l_unit->size, l_kind, 0x4000 | a_port->nunit);

		a_port->nunit++;
		}

	if ( !a_port->nunit )
		return	STS$K_SUCCESS;

	if ( a_port->sync == MH$K_SYNC_NONE )
		$MHMSG(MH$_UNSAFE);

	return	mh$bk_init();
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is the disk a booting satellite asks for one of ours (as
**	SCS$MSCP_CHECK_SERVICE of VMS)?  The SERVICE of its SOLICIT_SRV is the
**	name of the disk as $GETDVI DVI$_ALLDEVNAM gives it: _$42$DKA100:, or
**	_MHUB$DKA100: with the allocation class 0.  The underscore and the
**	colon may be there or not, the case does not matter.  A system disk
**	must be writable: a read-only unit is not served.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_svc		SERVICE: ASCIC, 16 octets at most
**	a_unit		Output: the index of the unit
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - the unit is read only; STS$K_ERROR - no
**	such unit.
**--
*/
int	mh$mscp_check_service	(
	const	MH$PORT *	a_port,
	const	uint8_t *	a_svc,
		unsigned *	a_unit
			)
{
const	char	*l_name = (const char *) a_svc + 1;
unsigned	l_len = $MIN(a_svc[0], NISCA$K_SZ_SERVICE - 1);

	if ( l_len && (l_name[0] == '_') )
		{
		l_name++;
		l_len--;
		}

	if ( l_len && (l_name[l_len - 1] == ':') )
		l_len--;

	for ( unsigned i = 0; i < a_port->nunit; i++ )
		{
		const	MH$UNIT	*l_unit = &a_port->units[i];

		if ( (strlen(l_unit->dvname) != l_len) || strncasecmp(l_unit->dvname, l_name, l_len) )
			continue;

		*a_unit = i;

		return	l_unit->ro ? STS$K_WARN : STS$K_SUCCESS;
		}

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A client has connected: give it a host number, the first free one
**	from a seed that moves on (the VMS server, X-71: a number is not
**	reused at once).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no number free, the connection is ended.
**--
*/
int	mh$mscp_connect	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt
			)
{
	for ( unsigned i = 0; i < MH$K_MAXHOST; i++ )
		{
		unsigned	l_h = (a_port->hostseed + i) % MH$K_MAXHOST;

		if ( a_port->hosts[l_h / 8] & (1U << (l_h % 8)) )
			continue;

		a_port->hosts[l_h / 8] |= (uint8_t) (1U << (l_h % 8));
		a_port->hostseed = l_h + 1;
		a_cdt->host = (int) l_h;

		if ( a_port->trace )
			$LOG_FAO(STS$K_INFO, "MSCP host !UL is !AZ of !AZ (!UL)", l_h, a_cdt->rproc, a_node->name, a_node->sysid);

		return	STS$K_SUCCESS;
		}

	a_cdt->host = -1;

	return	mh$scs_disconnect(a_port, a_node, a_cdt, "no free host number");
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A client is gone: its commands end without an END (the client will
**	issue them again on a new connection) - the ones the
**	backend has are left to it and freed when they come back; every unit
**	forgets it was ONLINE to it (an internal AVAILABLE), the
**	host number is free.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$mscp_disconnect	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt
			)
{
unsigned	l_h;

	if ( a_cdt->host < 0 )
		return	STS$K_SUCCESS;

	l_h = (unsigned) a_cdt->host;

	s_mh$hrb_purge(a_port, a_cdt);

	for ( unsigned i = 0; i < a_port->nunit; i++ )
		a_port->units[i].online[l_h / 8] &= (uint8_t) ~(1U << (l_h % 8));

	a_port->hosts[l_h / 8] &= (uint8_t) ~(1U << (l_h % 8));
	a_cdt->host = -1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The name of the command of an HRB, for the messages of MHBK.
**
**  FORMAL PARAMETERS:
**
**	a_hrb		The command
**
**  RETURN VALUE:
**	The name: READ, WRITE, ...
**--
*/
const	char *	mh$mscp_opname	(
	const	MH$HRB *	a_hrb
			)
{
	return	s_opdef[a_hrb->op].name;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Report the state of the server, on SIGUSR1: the commands in progress,
**	the buffers, and for every unit its hosts, what runs and waits on it,
**	the counters.  With no client every count of what is in progress is
**	0 - a leak shows here after a connection has gone.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	The condition value of the last message.
**--
*/
int	mh$mscp_stats	(
		MH$PORT *	a_port
			)
{
static	const char	*s_latname [MH$K_LAT_N] = { "READ", "WRITE" };
int		l_status;
unsigned	l_hosts, l_wait;

	l_status = $MHMSG(MH$_MSCPSTATS, a_port->nhrb, a_port->bufused);

	for ( unsigned i = 0; i < a_port->nunit; i++ )
		{
		MH$UNIT	*l_unit = &a_port->units[i];

		l_hosts = l_wait = 0;

		for ( unsigned j = 0; j < sizeof(l_unit->online); j++ )
			l_hosts += (unsigned) __builtin_popcount(l_unit->online[j]);

		for ( MH$HRB *l_hrb = l_unit->qhead; l_hrb; l_hrb = l_hrb->next )
			l_wait++;

		l_status = $MHMSG(MH$_UNITSTATS, l_unit->dvname, l_hosts, l_unit->current, l_wait,
			l_unit->nread, l_unit->rbytes, l_unit->nwrite, l_unit->wbytes);

		l_status = $MHMSG(MH$_BKSTATS, l_unit->dvname, l_unit->dev.ssz, l_unit->dev.nrmw, l_unit->dev.nedge, l_unit->dev.ncache, l_unit->dev.nedgehit[0],
			l_unit->dev.nedgehit[1], l_unit->dev.nedgehit[2], l_unit->dev.nedgehit[3], l_unit->dev.nlockwait, l_unit->dev.nsync,
			l_unit->dev.nsyncw, l_unit->dev.nzero, l_unit->dev.nzfill);

		for ( unsigned j = 0; j < MH$K_LAT_N; j++ )
			{
			const MH$LAT	*l_lat = &l_unit->lat[j];
			uint64_t	l_rest;

			if ( !l_lat->n )
				continue;

			/*
			**  Not $MIN: it takes ints, the sums of nanoseconds are not
			*/
			l_rest = (l_lat->sum > l_lat->bk + l_lat->xfer) ? l_lat->sum - l_lat->bk - l_lat->xfer : 0;

			l_status = $MHMSG(MH$_LATSTATS, l_unit->dvname, s_latname[j], l_lat->n, l_lat->sum / l_lat->n / 1000,
				l_lat->bk / l_lat->n / 1000, l_lat->xfer / l_lat->n / 1000,
				l_rest / l_lat->n / 1000, l_lat->max / 1000,
				l_lat->hist[0], l_lat->hist[1], l_lat->hist[2], l_lat->hist[3], l_lat->hist[4], l_lat->hist[5]);
			}
		}

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The unit of a unit number: a SLUN of ours.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_unitno	UNIT of the command
**
**  RETURN VALUE:
**	The unit; NULL - none.
**--
*/
static	MH$UNIT	*s_mh$mscp_unit	(
		MH$PORT *	a_port,
		uint16_t	a_unitno
			)
{
	if ( !(a_unitno & MSCP$M_UNIT_SLUN) || (a_unitno == MSCP$K_SLUN_RSVP) || ((a_unitno & ~MSCP$M_UNIT_SLUN) >= a_port->nunit) )
		return	NULL;

	return	&a_port->units[a_unitno & ~MSCP$M_UNIT_SLUN];
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is the unit ONLINE to the host of a connection?
**
**  FORMAL PARAMETERS:
**
**	a_unit		The unit
**	a_cdt		The connection
**
**  RETURN VALUE:
**	1 - yes; 0 - no.
**--
*/
static	int	s_mh$mscp_isonline	(
	const	MH$UNIT *	a_unit,
	const	MH$CDT *	a_cdt
			)
{
	if ( a_cdt->host < 0 )
		return	0;

	return	!!(a_unit->online[a_cdt->host / 8] & (1U << (a_cdt->host % 8)));
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Fill the end message of GET UNIT STATUS about a unit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cdt		The connection
**	a_idx		Index of the unit
**	a_end		The end message, 52 octets
**
**  RETURN VALUE:
**	The STATUS: SUCC - ONLINE to this host, AVLBL - not.
**--
*/
static	unsigned	s_mh$mscp_gus	(
		MH$PORT *	a_port,
		MH$CDT *	a_cdt,
		unsigned	a_idx,
		uint8_t *	a_end
			)
{
MH$UNIT	*l_unit = &a_port->units[a_idx];

	nisca$put16(a_end + MSCP$K_UNIT, (uint16_t) (MSCP$M_UNIT_SLUN | a_idx));
	nisca$put16(a_end + MSCP$K_MULT_UNT, (uint16_t) (a_idx << 8));
	nisca$put16(a_end + MSCP$K_UNT_FLGS, l_unit->ro ? MSCP$M_UF_WRTPH : 0);
	memset(a_end + 0x10, 0, 4);
	memcpy(a_end + MSCP$K_UNIT_ID, l_unit->unitid, 8);
	nisca$put32(a_end + MSCP$K_MEDIA_ID, l_unit->media);
	nisca$put32(a_end + MSCP$K_DMAXBCNT, MH$K_MSCP_MAXBCNT);
	nisca$put16(a_end + MSCP$K_TRACK, MH$K_GEO_TRACK);
	nisca$put16(a_end + MSCP$K_GROUP, MH$K_GEO_GROUP);
	nisca$put16(a_end + MSCP$K_CYLINDER, MH$K_GEO_CYL);
	a_end[MSCP$K_UNIT_SVR] = 1;
	a_end[MSCP$K_UNIT_HVR] = 1;
	nisca$put16(a_end + MSCP$K_RCT_SIZE, 0);
	a_end[MSCP$K_RBNS] = 0;
	a_end[MSCP$K_RCT_CPYS] = 0;

	return	s_mh$mscp_isonline(l_unit, a_cdt) ? MSCP$K_ST_SUCC : MSCP$K_ST_AVLBL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Fill the end message of ONLINE or SET UNIT CHARACTERISTICS (COPY_CHAR).
**
**  FORMAL PARAMETERS:
**
**	a_unit		The unit
**	a_idx		Its index
**	a_end		The end message, 44 octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$mscp_char	(
		MH$UNIT *	a_unit,
		unsigned	a_idx,
		uint8_t *	a_end
			)
{
	nisca$put16(a_end + MSCP$K_MULT_UNT, (uint16_t) (a_idx << 8));
	nisca$put16(a_end + MSCP$K_UNT_FLGS, a_unit->ro ? MSCP$M_UF_WRTPH : 0);
	memcpy(a_end + MSCP$K_UNIT_ID, a_unit->unitid, 8);
	nisca$put32(a_end + MSCP$K_MEDIA_ID, a_unit->media);
	nisca$put16(a_end + MSCP$K_SHDW_UNT, 0);
	nisca$put16(a_end + MSCP$K_SHDW_STS, 0);
	nisca$put32(a_end + MSCP$K_UNT_SIZE, a_unit->size);
	nisca$put32(a_end + MSCP$K_VOL_SER, a_unit->volser);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The last look at an end message before it goes: nothing a VMS client
**	takes for a reason to BUG_CHECK.
**
**  FORMAL PARAMETERS:
**
**	a_end		The end message
**	a_len		Its length
**
**  RETURN VALUE:
**	NULL - it may go; what is wrong with it otherwise.
**--
*/
static	const char *	s_mh$mscp_check	(
	const	uint8_t *	a_end,
		unsigned	a_len
			)
{
unsigned	l_op = a_end[MSCP$K_OPCODE] & ~MSCP$M_OP_END, l_st = nisca$get16(a_end + MSCP$K_STATUS) & MSCP$K_ST_MASK;
uint16_t	l_unit = nisca$get16(a_end + MSCP$K_UNIT);

	/*
	**  CMD_REF is not looked at: it is the RSPID of the client, echoed;
	**  0xFFFF in its high word is a BUG_CHECK only when the RSPID is stale
	**  - a live one may well have that sequence
	**  number.  An END only ever goes for a
	**  command of the open connection.
	*/
	if ( l_op != MSCP$K_OP_GTUNT )
		return	NULL;

	/*
	**  0x7FFF stays in an OFFLN answer only: the client checks the SLUN
	**  of a SUCC or AVLBL one
	*/
	if ( (l_st == MSCP$K_ST_SUCC) || (l_st == MSCP$K_ST_AVLBL) )
		{
		if ( !(l_unit & MSCP$M_UNIT_SLUN) || (l_unit == MSCP$K_SLUN_RSVP) || (l_unit & 0x8000) )
			return	"a unit number with no SLUN bit, or 0x7FFF (MSCPCLASS)";

		if ( !(nisca$get16(a_end + MSCP$K_TRACK) & 0xFF)
			|| !((nisca$get16(a_end + MSCP$K_GROUP) * nisca$get16(a_end + MSCP$K_CYLINDER)) & 0xFF) )
			return	"a zero low octet of TRACK or GROUP x CYLINDER (DISKCLASS)";

		if ( !(a_end[MSCP$K_UNIT_ID + 6] & 0x1F) || ((a_end[MSCP$K_UNIT_ID + 6] & 0x1F) > 26) )
			return	"a controller letter out of 1 ... 26 (MSCPCLASS)";
		}
	else if ( !l_unit && (l_st == MSCP$K_ST_OFFLN) && (nisca$get16(a_end + MSCP$K_STATUS) != MSCP$K_ST_OFFLN) )
		return	"the end of the walk with an OFFLN subcode (MSCPCLASS)";

	return	NULL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Check an end message and send it on the connection.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_end		The end message
**	a_len		Its length
**	a_name		Name of the command, for MSCPERR
**
**  RETURN VALUE:
**	The condition value of MH$SCS_SEND or of MH$SCS_DISCONNECT.
**--
*/
static	int	s_mh$mscp_reply	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
		uint8_t *	a_end,
		unsigned	a_len,
	const	char *		a_name
			)
{
const char	*l_bad;

	a_end[MSCP$K_FLAGS] = 0;

	if ( (l_bad = s_mh$mscp_check(a_end, a_len)) )
		{
		$MHMSG(MH$_MSCPERR, a_name, a_cdt->rproc, a_node->name, a_node->sysid, l_bad);

		return	mh$scs_disconnect(a_port, a_node, a_cdt, "an end message was held back, see MSCPERR");
		}

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "MSCP end !XB to !AZ (!UL): CMD_REF !XL, unit !XW, status !XW, !UL octets", a_end[MSCP$K_OPCODE], a_node->name,
			a_node->sysid, nisca$get32(a_end + MSCP$K_CMD_REF), nisca$get16(a_end + MSCP$K_UNIT), nisca$get16(a_end + MSCP$K_STATUS), a_len);

	return	mh$scs_send(a_port, a_node, a_cdt, a_end, a_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is a command sequential (opcode / 8 = 1 or 3)?
**
**  FORMAL PARAMETERS:
**
**	a_op		The opcode
**
**  RETURN VALUE:
**	1 - yes; 0 - no.
**--
*/
static	int	s_mh$mscp_isseq	(
		unsigned	a_op
			)
{
	return	((a_op >> 3) == 1) || ((a_op >> 3) == 3);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is the connection a command came on still the one it came on?
**
**  FORMAL PARAMETERS:
**
**	a_hrb		The command
**
**  RETURN VALUE:
**	1 - yes; 0 - it is gone.
**--
*/
static	int	s_mh$hrb_live	(
	const	MH$HRB *	a_hrb
			)
{
	return	!a_hrb->orphan && (a_hrb->cdt->state == MH$K_CDT_OPEN) && (a_hrb->cdt->lconid == a_hrb->lconid);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command in progress, out of its slot in the port: XCT_ID of a block
**	transfer is (LCONID, TAG).
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node the packet came from
**	a_lconid	The low longword of the XCT_ID
**	a_tag		The high one
**
**  RETURN VALUE:
**	The command; NULL - none of these.
**--
*/
MH$HRB	*mh$mscp_hrb	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		uint32_t	a_lconid,
		uint32_t	a_tag
			)
{
MH$HRB	*l_hrb = a_port->hrb[a_tag % MH$K_MAXHRB];

	if ( !l_hrb || (l_hrb->tag != a_tag) || (l_hrb->lconid != a_lconid) || (l_hrb->node != a_node) || !s_mh$hrb_live(l_hrb) )
		return	NULL;

	return	l_hrb;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A new command in progress, in a free slot.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	The command, zeroed but for its slot, tag and CMD_STS; NULL - the table
**	is full or no memory.
**--
*/
static	MH$HRB	*s_mh$hrb_alloc	(
		MH$PORT *	a_port
			)
{
MH$HRB		*l_hrb;
unsigned	l_slot;

	if ( a_port->nhrb >= MH$K_MAXHRB )
		return	NULL;

	for ( unsigned i = 0; i < MH$K_MAXHRB; i++ )
		{
		l_slot = (a_port->hrbseed + i) % MH$K_MAXHRB;

		if ( a_port->hrb[l_slot] )
			continue;

		if ( !(l_hrb = calloc(1, sizeof(MH$HRB))) )
			return	NULL;

		/*
		**  The tag changes with every command in a slot: a chunk of an
		**  old transfer matches nothing
		*/
		a_port->hrbgen++;
		l_hrb->slot	= l_slot;
		l_hrb->tag	= (a_port->hrbgen * MH$K_MAXHRB) + l_slot;
		l_hrb->cmd_sts	= MSCP$K_CMD_STS_INIT;
		l_hrb->t_cmd	= mh$now_ns();
		l_hrb->bk.dev	= NULL;
		l_hrb->bk.ctx	= l_hrb;

		a_port->hrb[l_slot] = l_hrb;
		a_port->hrbseed = l_slot + 1;
		a_port->nhrb++;

		return	l_hrb;
		}

	return	NULL;
}



static	int	s_mh$bufwait_run (MH$PORT *a_port);

/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Free a command in progress and its buffers; the ones that wait for
**	memory may go now.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$hrb_free	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
int	l_had = (a_hrb->buf != NULL);

	if ( a_hrb->buf )
		{
		free(a_hrb->buf);
		a_port->bufused -= a_hrb->bufsz;
		}

	if ( a_hrb->buf2 )
		{
		free(a_hrb->buf2);
		a_port->bufused -= a_hrb->bufsz;
		}

	a_port->hrb[a_hrb->slot] = NULL;
	a_port->nhrb--;
	free(a_hrb);

	return	l_had ? s_mh$bufwait_run(a_port) : STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a command out of a singly linked queue.
**
**  FORMAL PARAMETERS:
**
**	a_head, a_tail	The queue
**	a_hrb		The command
**
**  RETURN VALUE:
**	1 - it was there; 0 - not.
**--
*/
static	int	s_mh$hrb_unq	(
		MH$HRB **	a_head,
		MH$HRB **	a_tail,
		MH$HRB *	a_hrb
			)
{
MH$HRB	**l_pp, *l_prev = NULL;

	for ( l_pp = a_head; *l_pp; l_prev = *l_pp, l_pp = &(*l_pp)->next )
		{
		if ( *l_pp != a_hrb )
			continue;

		*l_pp = a_hrb->next;

		if ( *a_tail == a_hrb )
			*a_tail = l_prev;

		a_hrb->next = NULL;

		return	1;
		}

	return	0;
}



static	int	s_mh$unit_unblock (MH$PORT *a_port, MH$UNIT *a_unit);

/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A step of a command in the backend or in a block transfer is over:
**	its time goes to what the command has spent there.
**
**  FORMAL PARAMETERS:
**
**	a_hrb		The command, T_STEP set when the step began
**	a_acc		Output: T_BK or T_XFER of it, added to
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$hrb_lap	(
		MH$HRB *	a_hrb,
		uint64_t *	a_acc
			)
{
	if ( a_hrb->t_step )
		*a_acc += mh$now_ns() - a_hrb->t_step;

	a_hrb->t_step = 0;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A READ or a WRITE ends: its time from the command to the END, and the
**	time of its steps, go to the counters of the unit (LATSTATS).
**
**  FORMAL PARAMETERS:
**
**	a_hrb		The command
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$hrb_count	(
		MH$HRB *	a_hrb
			)
{
static	const uint64_t	s_bounds [MH$K_LAT_NHIST - 1] = { 100000, 300000, 1000000, 3000000, 10000000 };
MH$LAT		*l_lat;
uint64_t	l_ns;
unsigned	l_b;

	if ( a_hrb->op == MSCP$K_OP_READ )
		l_lat = &a_hrb->unit->lat[MH$K_LAT_READ];
	else if ( a_hrb->op == MSCP$K_OP_WRITE )
		l_lat = &a_hrb->unit->lat[MH$K_LAT_WRITE];
	else	return	STS$K_SUCCESS;

	l_ns = mh$now_ns() - a_hrb->t_cmd;

	l_lat->n++;
	l_lat->sum	+= l_ns;
	l_lat->bk	+= a_hrb->t_bk;
	l_lat->xfer	+= a_hrb->t_xfer;

	if ( l_ns > l_lat->max )
		l_lat->max = l_ns;

	for ( l_b = 0; (l_b < MH$K_LAT_NHIST - 1) && (l_ns >= s_bounds[l_b]); l_b++ )
		;

	l_lat->hist[l_b]++;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command is over: its END goes (when its connection is still there),
**	it stops counting on its unit, the ones it held back may run.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**	a_end		The end message, built; NULL - none is sent
**	a_len		Its length
**
**  RETURN VALUE:
**	The condition value of S_MH$MSCP_REPLY; STS$K_SUCCESS.
**--
*/
static	int	s_mh$hrb_finish	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb,
		uint8_t *	a_end,
		unsigned	a_len
			)
{
MH$UNIT	*l_unit = a_hrb->unit;
int	l_status = STS$K_SUCCESS, l_counted = (a_hrb->state != MH$K_HRB_SEQ_WAIT), l_live = a_end && s_mh$hrb_live(a_hrb);

	/*
	**  An END held back ends the connection: the purge that comes with it
	**  must leave this command to us
	*/
	a_hrb->orphan = 1;

	if ( l_live )
		{
		s_mh$hrb_count(a_hrb);
		l_status = s_mh$mscp_reply(a_port, a_hrb->node, a_hrb->cdt, a_end, a_len, s_opdef[a_hrb->op].name);
		}

	if ( l_counted && l_unit->current )
		{
		l_unit->current--;

		if ( s_mh$mscp_isseq(a_hrb->op) )
			l_unit->seq = 0;
		}

	s_mh$hrb_free(a_port, a_hrb);
	s_mh$unit_unblock(a_port, l_unit);

	return	l_status;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	End a command that moves data: the END is the command with its
**	opcode | 0x80, STATUS, BYTE_CNT = what has been moved; FRST_BAD stays
**	as LBN, HRN of WRITE and ERASE as it came.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**	a_status	STATUS
**
**  RETURN VALUE:
**	The condition value of S_MH$HRB_FINISH.
**--
*/
static	int	s_mh$hrb_end	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb,
		unsigned	a_status
			)
{
uint8_t		l_end [MH$K_SZ_APPL] = {0};
unsigned	l_len = s_opdef[a_hrb->op].endlen;

	memcpy(l_end, a_hrb->cmd, sizeof(a_hrb->cmd));

	if ( (a_status & MSCP$K_ST_MASK) == MSCP$K_ST_ICMD )
		l_len = MSCP$K_MXCMDLEN;

	l_end[MSCP$K_OPCODE] = (uint8_t) (a_hrb->op | MSCP$M_OP_END);
	nisca$put16(l_end + MSCP$K_STATUS, (uint16_t) a_status);

	if ( (a_hrb->op == MSCP$K_OP_READ) || (a_hrb->op == MSCP$K_OP_WRITE) || (a_hrb->op == MSCP$K_OP_COMP) || (a_hrb->op == MSCP$K_OP_ERASE) )
		nisca$put32(l_end + MSCP$K_BYTE_CNT, a_hrb->done);

	return	s_mh$hrb_finish(a_port, a_hrb, l_end, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Give a command to the backend.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**	a_phase		MH$K_PH_READ, _WRITE, _ZERO, _CMPDISK
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of S_MH$HRB_END when the backend
**	cannot take it.
**--
*/
static	int	s_mh$hrb_io	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb,
		int		a_phase
			)
{
MH$BKREQ	*l_bk = &a_hrb->bk;

	a_hrb->t_step	= mh$now_ns();
	a_hrb->phase	= a_phase;
	a_hrb->state	= MH$K_HRB_DRV_WAIT;
	a_hrb->cmd_sts--;

	l_bk->dev	= &a_hrb->unit->dev;
	l_bk->off	= (uint64_t) a_hrb->lbn * 512;
	l_bk->len	= a_hrb->bcnt;
	l_bk->sync	= 0;
	l_bk->buf	= a_hrb->buf;

	switch ( a_phase )
		{
		case	MH$K_PH_READ:
			l_bk->op = MH$K_BK_READ;
			break;

		case	MH$K_PH_CMPDISK:
			l_bk->op = MH$K_BK_READ;
			l_bk->buf = a_hrb->buf2;
			break;

		case	MH$K_PH_WRITE:
			l_bk->op = MH$K_BK_WRITE;
			l_bk->sync = a_port->sync;
			break;

		case	MH$K_PH_ZERO:
			l_bk->op = MH$K_BK_ZERO;
			l_bk->sync = a_port->sync;
			break;
		}

	if ( !(1 & mh$bk_submit(l_bk)) )
		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_CNTLR);

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The data of a command can move: its buffers are there.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of the first step.
**--
*/
static	int	s_mh$hrb_go	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
	switch ( a_hrb->op )
		{
		case	MSCP$K_OP_READ:
			return	s_mh$hrb_io(a_port, a_hrb, MH$K_PH_READ);

		case	MSCP$K_OP_ERASE:
			return	s_mh$hrb_io(a_port, a_hrb, MH$K_PH_ZERO);

		default:
			/*
			**  WRITE, COMPARE: the data of the client first
			*/
			a_hrb->phase	= MH$K_PH_FETCH;
			a_hrb->t_step	= mh$now_ns();
			a_hrb->cmd_sts--;

			return	mh$blk_request(a_port, a_hrb);
		}
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take the buffers of a command from the pool; when the pool is empty
**	the command waits (BUF_WAIT) - but one alone always gets them.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	1 - the buffers are there; 0 - it waits.
**--
*/
static	int	s_mh$hrb_buffers	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
uint32_t	l_sz = (a_hrb->bcnt + 4095) & ~4095U;
int		l_two = (a_hrb->op == MSCP$K_OP_COMP) || (a_hrb->mod & MSCP$M_MD_COMP);

	if ( a_port->bufused && (a_port->bufused + (l_two ? 2ULL : 1ULL) * l_sz > MH$K_BUFPOOL) )
		return	0;

	if ( !(a_hrb->buf = aligned_alloc(4096, l_sz)) )
		return	0;

	if ( l_two && !(a_hrb->buf2 = aligned_alloc(4096, l_sz)) )
		{
		free(a_hrb->buf);
		a_hrb->buf = NULL;

		return	0;
		}

	a_hrb->bufsz	= l_sz;
	a_port->bufused	+= (l_two ? 2ULL : 1ULL) * l_sz;

	return	1;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Memory is back: the commands that wait for it go, in order, as long as
**	there is enough.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$bufwait_run	(
		MH$PORT *	a_port
			)
{
MH$HRB	*l_hrb;

	while ( (l_hrb = a_port->bwhead) && s_mh$hrb_buffers(a_port, l_hrb) )
		{
		s_mh$hrb_unq(&a_port->bwhead, &a_port->bwtail, l_hrb);
		s_mh$hrb_go(a_port, l_hrb);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The first look at READ, WRITE, ERASE, COMPARE when its turn has come,
**	in the order of the VMS server.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of the step taken.
**--
*/
static	int	s_mh$hrb_data	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
MH$UNIT	*l_unit = a_hrb->unit;

	if ( !s_mh$mscp_isonline(l_unit, a_hrb->cdt) )
		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_AVLBL);

	if ( l_unit->ro && ((a_hrb->op == MSCP$K_OP_WRITE) || (a_hrb->op == MSCP$K_OP_ERASE)) )
		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_WRTPR);

	if ( !a_hrb->bcnt )
		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_SUCC);

	if ( (a_hrb->lbn >= l_unit->size) || ((a_hrb->bcnt - 1) / 512 >= l_unit->size - a_hrb->lbn) || (a_hrb->bcnt > MH$K_MSCP_MAXBCNT) )
		return	s_mh$hrb_end(a_port, a_hrb, $MSCP_ICMD(MSCP$K_BYTE_CNT));

	if ( a_hrb->op == MSCP$K_OP_ERASE )
		return	s_mh$hrb_go(a_port, a_hrb);

	if ( !s_mh$hrb_buffers(a_port, a_hrb) )
		{
		a_hrb->state = MH$K_HRB_BUF_WAIT;
		a_hrb->next = NULL;

		if ( a_port->bwtail )
			a_port->bwtail->next = a_hrb;
		else	a_port->bwhead = a_hrb;

		a_port->bwtail = a_hrb;

		return	STS$K_SUCCESS;
		}

	return	s_mh$hrb_go(a_port, a_hrb);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Run a command that is not immediate, its turn on the unit has come:
**	the sequential ones and FLUSH, CMPCD are done at once, the ones that
**	move data begin their steps.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of the end or of the first step.
**--
*/
static	int	s_mh$hrb_start	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
uint8_t		l_end [MH$K_SZ_APPL] = {0};
unsigned	l_status = MSCP$K_ST_SUCC, l_len = s_opdef[a_hrb->op].endlen, l_idx = (unsigned) (a_hrb->unit - a_port->units);
MH$UNIT		*l_unit = a_hrb->unit;
MH$CDT		*l_cdt = a_hrb->cdt;

	a_hrb->state = MH$K_HRB_NEW;

	switch ( a_hrb->op )
		{
		case	MSCP$K_OP_READ:
		case	MSCP$K_OP_WRITE:
		case	MSCP$K_OP_COMP:
		case	MSCP$K_OP_ERASE:
			return	s_mh$hrb_data(a_port, a_hrb);
		}

	memcpy(l_end, a_hrb->cmd, sizeof(a_hrb->cmd));

	switch ( a_hrb->op )
		{
		case	MSCP$K_OP_ONLIN:
			if ( l_cdt->host < 0 )
				{
				l_status = MSCP$K_ST_OFFLN;
				break;
				}

			if ( nisca$get16(a_hrb->cmd + MSCP$K_UNT_FLGS) & (MSCP$M_UF_SSMEM | MSCP$M_UF_SSMST) )
				{
				l_status = $MSCP_ICMD(MSCP$K_UNT_FLGS);
				l_len = MSCP$K_MXCMDLEN;
				break;
				}

			if ( s_mh$mscp_isonline(l_unit, l_cdt) )
				l_status = MSCP$K_ST_ALONL;

			l_unit->online[l_cdt->host / 8] |= (uint8_t) (1U << (l_cdt->host % 8));
			s_mh$mscp_char(l_unit, l_idx, l_end);
			break;

		case	MSCP$K_OP_STUNT:
			if ( !s_mh$mscp_isonline(l_unit, l_cdt) )
				{
				l_status = MSCP$K_ST_OFFLN;
				break;
				}

			s_mh$mscp_char(l_unit, l_idx, l_end);
			break;

		case	MSCP$K_OP_AVAIL:
			if ( l_cdt->host >= 0 )
				l_unit->online[l_cdt->host / 8] &= (uint8_t) ~(1U << (l_cdt->host % 8));

			break;

		default:
			/*
			**  DTACP, CMPCD, FLUSH: nothing to do - the data is on disk
			**  before the END of every WRITE
			*/
			break;
		}

	l_end[MSCP$K_OPCODE] = (uint8_t) (a_hrb->op | MSCP$M_OP_END);
	nisca$put16(l_end + MSCP$K_STATUS, (uint16_t) l_status);

	return	s_mh$hrb_finish(a_port, a_hrb, l_end, l_len);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command that is not immediate has come: it runs now or waits for
**	its turn.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of S_MH$HRB_START; STS$K_SUCCESS - it waits.
**--
*/
static	int	s_mh$hrb_submit	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
MH$UNIT	*l_unit = a_hrb->unit;
int	l_run;

	if ( s_mh$mscp_isseq(a_hrb->op) )
		l_run = !l_unit->current && !l_unit->qhead;
	else	l_run = !l_unit->seq && !l_unit->qhead;

	if ( !l_run )
		{
		a_hrb->state = MH$K_HRB_SEQ_WAIT;
		a_hrb->next = NULL;

		if ( l_unit->qtail )
			l_unit->qtail->next = a_hrb;
		else	l_unit->qhead = a_hrb;

		l_unit->qtail = a_hrb;

		return	STS$K_SUCCESS;
		}

	l_unit->current++;

	if ( s_mh$mscp_isseq(a_hrb->op) )
		l_unit->seq = 1;

	return	s_mh$hrb_start(a_port, a_hrb);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A command of a unit is over: the ones that wait may run - every
**	non-sequential one at the head, a sequential one only when nothing
**	runs (UNBLOCK).  A command that ends at once calls this
**	again from inside: the outer loop does the work.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_unit		The unit; every unit is looked at, NULL will do
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$unit_unblock	(
		MH$PORT *	a_port,
		MH$UNIT *	a_unit
			)
{
static	int	s_busy;
MH$HRB		*l_hrb;

	if ( s_busy )
		return	STS$K_SUCCESS;

	s_busy = 1;

	for ( int l_again = 1; l_again; )
		{
		l_again = 0;

		for ( unsigned i = 0; i < a_port->nunit; i++ )
			{
			MH$UNIT	*l_unit = &a_port->units[i];

			while ( !l_unit->seq && (l_hrb = l_unit->qhead) )
				{
				if ( s_mh$mscp_isseq(l_hrb->op) && l_unit->current )
					break;

				s_mh$hrb_unq(&l_unit->qhead, &l_unit->qtail, l_hrb);
				l_unit->current++;

				if ( s_mh$mscp_isseq(l_hrb->op) )
					l_unit->seq = 1;

				s_mh$hrb_start(a_port, l_hrb);
				l_again = 1;
				}
			}
		}

	s_busy = 0;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A connection is gone: its commands end without an END.  The ones in
**	the backend are left there as orphans and freed when they come back -
**	the threads still use their buffers - and hold their unit till then,
**	so that nothing of a new connection overtakes them on the disk.  A
**	request the backend never gives back so holds its unit for ever.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cdt		The connection
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
static	int	s_mh$hrb_purge	(
		MH$PORT *	a_port,
		MH$CDT *	a_cdt
			)
{
MH$HRB	*l_hrb;

	for ( unsigned i = 0; i < MH$K_MAXHRB; i++ )
		{
		if ( !(l_hrb = a_port->hrb[i]) || l_hrb->orphan || (l_hrb->cdt != a_cdt) || (l_hrb->lconid != a_cdt->lconid) )
			continue;

		switch ( l_hrb->state )
			{
			case	MH$K_HRB_SEQ_WAIT:
				s_mh$hrb_unq(&l_hrb->unit->qhead, &l_hrb->unit->qtail, l_hrb);
				break;

			case	MH$K_HRB_BUF_WAIT:
				s_mh$hrb_unq(&a_port->bwhead, &a_port->bwtail, l_hrb);
				break;

			case	MH$K_HRB_SNDAT:
				mh$blk_cancel(a_port, l_hrb);
				break;
			}

		/*
		**  One the disk has goes on counting on its unit until it comes
		**  back: a WRITE of the old connection must not land after what
		**  the client sends again once it has reconnected
		*/
		if ( l_hrb->state == MH$K_HRB_DRV_WAIT )
			{
			l_hrb->orphan = 1;
			continue;
			}

		if ( (l_hrb->state != MH$K_HRB_SEQ_WAIT) && l_hrb->unit->current )
			{
			l_hrb->unit->current--;

			if ( s_mh$mscp_isseq(l_hrb->op) )
				l_hrb->unit->seq = 0;
			}

		s_mh$hrb_free(a_port, l_hrb);
		}

	return	s_mh$unit_unblock(a_port, NULL);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Compare the two buffers of a command.
**
**  FORMAL PARAMETERS:
**
**	a_hrb		The command
**
**  RETURN VALUE:
**	The STATUS: SUCC - the same; COMP - they differ.
**--
*/
static	unsigned	s_mh$hrb_compare	(
	const	MH$HRB *	a_hrb
			)
{
	return	memcmp(a_hrb->buf, a_hrb->buf2, a_hrb->bcnt) ? MSCP$K_ST_COMP : MSCP$K_ST_SUCC;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The backend has done a step of a command (MHBK, in the protocol
**	thread): the next step, or the END.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**
**  RETURN VALUE:
**	The condition value of the next step.
**--
*/
int	mh$mscp_iodone	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb
			)
{
MH$UNIT	*l_unit = a_hrb->unit;
int	l_err = a_hrb->bk.err;

	/*
	**  The connection went while the disk had it: nothing to tell anyone,
	**  the unit is free of it now (S_MH$HRB_PURGE)
	*/
	if ( a_hrb->orphan )
		{
		if ( l_unit->current )
			{
			l_unit->current--;

			if ( s_mh$mscp_isseq(a_hrb->op) )
				l_unit->seq = 0;
			}

		s_mh$hrb_free(a_port, a_hrb);

		return	s_mh$unit_unblock(a_port, l_unit);
		}

	a_hrb->cmd_sts--;
	s_mh$hrb_lap(a_hrb, &a_hrb->t_bk);

	if ( l_err )
		{
		$MHMSG(MH$_IOERR, s_opdef[a_hrb->op].name, l_unit->dvname, a_hrb->lbn, a_hrb->bcnt, l_err, strerror(l_err));

		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_DRIVE);
		}

	switch ( a_hrb->phase )
		{
		case	MH$K_PH_READ:
			l_unit->nread++;
			l_unit->rbytes += a_hrb->bcnt;

			if ( a_hrb->aborted )
				return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_ABRTD);

			a_hrb->phase	= MH$K_PH_SEND;
			a_hrb->t_step	= mh$now_ns();

			return	mh$blk_send(a_port, a_hrb);

		case	MH$K_PH_WRITE:
			l_unit->nwrite++;
			l_unit->wbytes += a_hrb->bcnt;
			a_hrb->done = a_hrb->bcnt;

			if ( a_hrb->mod & MSCP$M_MD_COMP )
				return	s_mh$hrb_io(a_port, a_hrb, MH$K_PH_CMPDISK);

			return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_SUCC);

		case	MH$K_PH_ZERO:
			l_unit->nwrite++;
			a_hrb->done = a_hrb->bcnt;

			return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_SUCC);

		case	MH$K_PH_CMPDISK:
			a_hrb->done = a_hrb->bcnt;

			return	s_mh$hrb_end(a_port, a_hrb, s_mh$hrb_compare(a_hrb));
		}

	return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_CNTLR);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A block transfer of a command is over (MHBLK): RET_CNF of the data we
**	sent, RET_DAT_LP of the data we asked for.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_hrb		The command
**	a_ok		0 - the client sent less than asked
**
**  RETURN VALUE:
**	The condition value of the next step.
**--
*/
int	mh$mscp_xferdone	(
		MH$PORT *	a_port,
		MH$HRB *	a_hrb,
		int		a_ok
			)
{
	s_mh$hrb_lap(a_hrb, &a_hrb->t_xfer);

	if ( !a_ok )
		return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_HSTBF);

	a_hrb->cmd_sts--;

	switch ( a_hrb->phase )
		{
		case	MH$K_PH_SEND:
			a_hrb->done = a_hrb->bcnt;

			/*
			**  READ with COMP: the data back from the client, compared
			*/
			if ( a_hrb->mod & MSCP$M_MD_COMP )
				{
				a_hrb->phase	= MH$K_PH_CMPHOST;
				a_hrb->t_step	= mh$now_ns();

				return	mh$blk_request(a_port, a_hrb);
				}

			return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_SUCC);

		case	MH$K_PH_FETCH:
			if ( a_hrb->aborted )
				return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_ABRTD);

			if ( a_hrb->op == MSCP$K_OP_COMP )
				return	s_mh$hrb_io(a_port, a_hrb, MH$K_PH_CMPDISK);

			return	s_mh$hrb_io(a_port, a_hrb, MH$K_PH_WRITE);

		case	MH$K_PH_CMPHOST:
			return	s_mh$hrb_end(a_port, a_hrb, s_mh$hrb_compare(a_hrb));
		}

	return	s_mh$hrb_end(a_port, a_hrb, MSCP$K_ST_CNTLR);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The command in progress ABORT or GET COMMAND STATUS is about: of the
**	same host, CMD_REF = OUT_REF, the same unit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_cdt		The connection
**	a_outref	OUT_REF
**	a_unitno	UNIT
**
**  RETURN VALUE:
**	The command; NULL - none.
**--
*/
static	MH$HRB	*s_mh$hrb_find	(
		MH$PORT *	a_port,
		MH$CDT *	a_cdt,
		uint32_t	a_outref,
		uint16_t	a_unitno
			)
{
MH$HRB	*l_hrb;

	for ( unsigned i = 0; i < MH$K_MAXHRB; i++ )
		{
		if ( (l_hrb = a_port->hrb[i]) && !l_hrb->orphan && (l_hrb->cdt == a_cdt) && (l_hrb->lconid == a_cdt->lconid)
			&& (l_hrb->cmd_ref == a_outref) && (nisca$get16(l_hrb->cmd + MSCP$K_UNIT) == a_unitno) )
			return	l_hrb;
		}

	return	NULL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take in an MSCP command of a client: check it, answer an immediate one,
**	give any other one its HRB and its turn on the unit.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_node		The node
**	a_cdt		The connection
**	a_cmd		The command
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MH$SCS_SEND.
**--
*/
int	mh$mscp_input	(
		MH$PORT *	a_port,
		MH$NODE *	a_node,
		MH$CDT *	a_cdt,
	const	uint8_t *	a_cmd,
		unsigned	a_len
			)
{
uint8_t		l_end [MH$K_SZ_APPL] = {0};
unsigned	l_op, l_endlen, l_status = MSCP$K_ST_SUCC, l_idx;
uint16_t	l_unitno, l_mod;
const MH$OPDEF	*l_def = NULL;
MH$UNIT		*l_unit = NULL;
MH$HRB		*l_hrb;

	a_cdt->ncmd++;

	if ( a_len < MSCP$K_MIN_SIZ )
		return	$LOG_FAO(STS$K_WARN, "MSCP packet of !UL octets from !AZ (!UL), dropped", a_len, a_node->name, a_node->sysid);

	memcpy(l_end, a_cmd, $MIN(a_len, sizeof(l_end)));

	l_op	= a_cmd[MSCP$K_OPCODE];
	l_unitno = nisca$get16(a_cmd + MSCP$K_UNIT);
	l_mod	= nisca$get16(a_cmd + MSCP$K_MODIFIER);

	if ( l_op < MSCP$K_OP_MAX )
		l_def = &s_opdef[l_op];

	if ( a_port->trace )
		$LOG_FAO(STS$K_INFO, "MSCP !AZ (!XB) from !AZ (!UL): CMD_REF !XL, unit !XW, modifier !XW, !UL octets",
			(l_def && l_def->name) ? l_def->name : "?", l_op, a_node->name, a_node->sysid, nisca$get32(a_cmd + MSCP$K_CMD_REF),
			l_unitno, l_mod, a_len);

	/*
	**  The check, in the order of the VMS server
	*/
	l_endlen = MSCP$K_MXCMDLEN;

	if ( !l_def || !l_def->cmdlen )
		l_status = $MSCP_ICMD(MSCP$K_OPCODE);
	else if ( a_len < l_def->cmdlen )
		l_status = $MSCP_ICMD(0);
	else if ( a_cmd[MSCP$K_FLAGS] )
		l_status = $MSCP_ICMD(MSCP$K_FLAGS);
	else if ( l_mod & ~l_def->mods )
		l_status = $MSCP_ICMD(MSCP$K_MODIFIER);
	else	l_endlen = l_def->endlen;

	if ( l_status != MSCP$K_ST_SUCC )
		goto	l_send;

	/*
	**  Not immediate: a command in progress, in the order of its unit
	*/
	if ( ((l_op >> 3) != 0) && ((l_op >> 3) != 6) )
		{
		if ( !(l_unit = s_mh$mscp_unit(a_port, l_unitno)) )
			{
			l_status = MSCP$K_ST_OFFLN;
			goto	l_send;
			}

		if ( !(l_hrb = s_mh$hrb_alloc(a_port)) )
			{
			l_status = MSCP$K_ST_CNTLR;
			goto	l_send;
			}

		l_hrb->node	= a_node;
		l_hrb->cdt	= a_cdt;
		l_hrb->lconid	= a_cdt->lconid;
		l_hrb->unit	= l_unit;
		l_hrb->op	= (uint8_t) l_op;
		l_hrb->mod	= l_mod;
		memcpy(l_hrb->cmd, a_cmd, $MIN(a_len, sizeof(l_hrb->cmd)));
		l_hrb->cmd_ref	= nisca$get32(a_cmd + MSCP$K_CMD_REF);

		if ( a_len >= MSCP$K_LBN + 4 )
			{
			l_hrb->bcnt	= nisca$get32(a_cmd + MSCP$K_BYTE_CNT);
			l_hrb->boff	= nisca$get32(a_cmd + MSCP$K_BUFFER + NISCA$K_BD_BOFF);
			l_hrb->bname	= nisca$get32(a_cmd + MSCP$K_BUFFER + NISCA$K_BD_BNAME);
			l_hrb->lbn	= nisca$get32(a_cmd + MSCP$K_LBN);
			}

		return	s_mh$hrb_submit(a_port, l_hrb);
		}

	switch ( l_op )
		{
		case	MSCP$K_OP_ABORT:
			/*
			**  Waiting for its turn or for memory: it ends at once;
			**  in progress: it ends ABRTD at its next step.  ABORT
			**  itself is always SUCC.
			*/
			if ( (l_hrb = s_mh$hrb_find(a_port, a_cdt, nisca$get32(a_cmd + MSCP$K_OUT_REF), l_unitno)) )
				{
				if ( l_hrb->state == MH$K_HRB_SEQ_WAIT )
					{
					s_mh$hrb_unq(&l_hrb->unit->qhead, &l_hrb->unit->qtail, l_hrb);
					s_mh$hrb_end(a_port, l_hrb, MSCP$K_ST_ABRTD);
					}
				else if ( l_hrb->state == MH$K_HRB_BUF_WAIT )
					{
					s_mh$hrb_unq(&a_port->bwhead, &a_port->bwtail, l_hrb);
					s_mh$hrb_end(a_port, l_hrb, MSCP$K_ST_ABRTD);
					}
				else	l_hrb->aborted = 1;
				}

			break;

		case	MSCP$K_OP_GTCMD:
			l_hrb = s_mh$hrb_find(a_port, a_cdt, nisca$get32(a_cmd + MSCP$K_OUT_REF), l_unitno);
			nisca$put32(l_end + MSCP$K_CMD_STS, l_hrb ? l_hrb->cmd_sts : 0);
			break;

		case	MSCP$K_OP_STCON:
			if ( nisca$get16(a_cmd + MSCP$K_VERSION) )
				{
				l_status = $MSCP_ICMD(MSCP$K_VERSION);
				l_endlen = MSCP$K_MXCMDLEN;
				break;
				}

			a_cdt->hst_flgs	= nisca$get16(a_cmd + MSCP$K_CNT_FLGS);
			a_cdt->hst_tmo	= nisca$get16(a_cmd + MSCP$K_HST_TMO);

			nisca$put16(l_end + MSCP$K_UNIT, a_port->alloclass);	/* CNT_ALCS in the low octet	*/
			nisca$put16(l_end + MSCP$K_VERSION, 0);
			nisca$put16(l_end + MSCP$K_CNT_FLGS, MSCP$M_CF_MLTHS | MSCP$M_CF_LOAD);
			nisca$put16(l_end + MSCP$K_CNT_TMO, MH$K_MSCP_CNT_TMO);
			l_end[MSCP$K_CNT_SVR] = 1;
			l_end[MSCP$K_CNT_HVR] = 5;			/* EMULA: the client wants 5 or more	*/
			nisca$put16(l_end + MSCP$K_CNT_ID, a_port->sysid);
			memset(l_end + MSCP$K_CNT_ID + 2, 0, 4);
			l_end[MSCP$K_CNT_ID + 6] = MSCP$K_CM_EMULA;
			l_end[MSCP$K_CNT_ID + 7] = MSCP$K_CL_CNTRL;
			nisca$put32(l_end + MSCP$K_MAXBCNT, MH$K_MSCP_MAXBCNT);

			if ( a_port->trace )
				$LOG_FAO(STS$K_INFO, "MSCP host !SL: host flags !XW, host timeout !UL s", a_cdt->host, a_cdt->hst_flgs, a_cdt->hst_tmo);

			break;

		case	MSCP$K_OP_GTUNT:
			nisca$put16(l_end + MSCP$K_LOAD_AVAIL, MH$K_MSCP_LOAD);

			if ( !l_unitno )
				{
				/*
				**  The sign of life of the client: a clean OFFLN
				*/
				l_status = MSCP$K_ST_OFFLN;
				break;
				}

			if ( l_unitno == MSCP$K_SLUN_RSVP )
				{
				/*
				**  Find the unit by UNIT_ID; not found: OFFLN, the unit
				**  number left as it came
				*/
				l_status = MSCP$K_ST_OFFLN;

				for ( l_idx = 0; l_idx < a_port->nunit; l_idx++ )
					{
					if ( !memcmp(a_cmd + MSCP$K_UNIT_ID, a_port->units[l_idx].unitid, 8) )
						{
						l_status = s_mh$mscp_gus(a_port, a_cdt, l_idx, l_end);
						break;
						}
					}

				break;
				}

			if ( l_mod & MSCP$M_MD_NXUNT )
				{
				/*
				**  The walk: from unit 1 or 0x4000 + n on; past the last -
				**  UNIT 0 and a clean OFFLN
				*/
				l_idx = (l_unitno & MSCP$M_UNIT_SLUN) ? (l_unitno & ~MSCP$M_UNIT_SLUN) : 0;

				if ( (l_unitno & 0x8000) || (!(l_unitno & MSCP$M_UNIT_SLUN) && (l_unitno != 1)) || (l_idx >= a_port->nunit) )
					{
					nisca$put16(l_end + MSCP$K_UNIT, 0);
					l_status = MSCP$K_ST_OFFLN;
					break;
					}

				l_status = s_mh$mscp_gus(a_port, a_cdt, l_idx, l_end);
				break;
				}

			if ( !(l_unit = s_mh$mscp_unit(a_port, l_unitno)) )
				{
				/*
				**  Not a SLUN of ours: never echo a number with no SLUN
				**  bit in an answer
				*/
				if ( !(l_unitno & MSCP$M_UNIT_SLUN) )
					nisca$put16(l_end + MSCP$K_UNIT, 0);

				l_status = MSCP$K_ST_OFFLN;
				break;
				}

			l_status = s_mh$mscp_gus(a_port, a_cdt, l_unitno & ~MSCP$M_UNIT_SLUN, l_end);
			break;

		case	MSCP$K_OP_GTUNM:
			if ( !(l_unit = s_mh$mscp_unit(a_port, l_unitno)) )
				{
				l_status = MSCP$K_ST_OFFLN;
				break;
				}

			nisca$put32(l_end + MSCP$K_DDR_NAMELEN, (uint32_t) strlen(l_unit->ddr));
			memset(l_end + MSCP$K_DDR_NAME, 0, 44 - MSCP$K_DDR_NAME);
			memcpy(l_end + MSCP$K_DDR_NAME, l_unit->ddr, strlen(l_unit->ddr));
			break;

		case	MSCP$K_OP_TERCO:
			if ( !s_mh$mscp_unit(a_port, l_unitno) )
				l_status = MSCP$K_ST_OFFLN;

			break;
		}

l_send:
	l_end[MSCP$K_OPCODE]	= (uint8_t) (l_op | MSCP$M_OP_END);
	nisca$put16(l_end + MSCP$K_STATUS, (uint16_t) l_status);

	return	s_mh$mscp_reply(a_port, a_node, a_cdt, l_end, l_endlen, (l_def && l_def->name) ? l_def->name : "?");
}
