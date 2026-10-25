#define	__MODULE__	"MHMOP"
#define	__IDENT__	"X01-22"
#define	__REV__		"1.22.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_mop.c
**
**  ABSTRACT:	The MOP server of the diskless Alpha satellites: the console
**		of a satellite (>>> BOOT EWA0) is given the primary bootstrap,
**		APB.EXE, from the system disk of the satellite that the server
**		serves, and the parameters of the satellite after it - so that
**		a satellite needs no OpenVMS node to boot, only to join a
**		cluster.
**
**  DESCRIPTION: The exchange, the console asking:
**
**		    REQUEST PROGRAM, multicast AB-00-00-01-00-00
**		                          -> ASSISTANCE VOLUNTEER, if the image and
**		                             the parameters are ready
**		    REQUEST PROGRAM, to us -> MEMORY LOAD 0, at once
**		    REQUEST MEMORY LOAD n  -> n is the one sent: it again; the
**		                             next one: the next MEMORY LOAD;
**		                             another: the load is ended
**		    ...                       the image: its first section, from 0
**		                          -> MEMORY LOAD of the parameters (LAPL),
**		                             at the address the image ends at
**		                          -> PARAMETER LOAD WITH TRANSFER ADDRESS
**		    REQUEST MEMORY LOAD    -> the satellite is loaded
**
**		What is not answered in a second is sent again; a console
**		silent for 60 seconds has its load ended.  The answer goes in
**		the format the console asked in: V3 - Ethernet, 60-01 with the
**		length word - or V4 - 802.3 with the SNAP 08-00-2B-60-01.  Only
**		the adapters of /SATELLITEn are answered, and only what they send
**		to the multicast or to us: a console another MOP server loads is
**		left to it.
**
**		The image is the one of the disk itself, read by MHODS when a
**		console asks: [root.SYSCOMMON.SYSEXE]APB.EXE - its header of
**		HDRBLKCNT blocks is skipped, the first image section is sent.
**		The parameters of the satellite come of its parameter file,
**		[root.SYSEXE]ALPHAVMSSYS.PAR: SCSNODE, SCSSYSTEMID,
**		NISCS_PORT_SERV, NISCS_CONV_BOOT, in the order they are in the
**		file; VAXCLUSTER must not be 0.  The rest is the server's own:
**		the name of the service of the unit (_$42$DKA100:) and its
**		UNIT_ID, the group and the password of the cluster, the SCSNODE
**		and the system ID of the server.  The bootstrap then asks for
**		the very unit by SOLICIT_SRV - MHCC answers it.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  25-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		25-OCT-2026	RRL	X01-22 : Initial version.
**
**--
*/

#include	<errno.h>
#include	<stdio.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>
#include	<time.h>

#include	"mh_def.h"

/*
**  The codes of the MOP messages of a load
*/
enum	{
	MOP$K_MEMLOAD_XFER	= 0,		/* MEMORY LOAD WITH TRANSFER ADDRESS		*/
	MOP$K_MEMLOAD		= 2,		/* MEMORY LOAD					*/
	MOP$K_ASSVOL		= 3,		/* ASSISTANCE VOLUNTEER				*/
	MOP$K_REQPGM		= 8,		/* REQUEST PROGRAM				*/
	MOP$K_REQMEMLOAD	= 10,		/* REQUEST MEMORY LOAD				*/
	MOP$K_PARAMLOAD_XFER	= 20		/* PARAMETER LOAD WITH TRANSFER ADDRESS		*/
	};

#define	MOP$K_PGM_SYSTEM	2		/* REQUEST PROGRAM: an operating system		*/

/*
**  The items of the PARAMETER LOAD: of Phase IV, and the time of V3
*/
enum	{
	MOP$K_PAR_CLIENT_NAME	= 1,
	MOP$K_PAR_CLIENT_ADDR,
	MOP$K_PAR_HOST_NAME,
	MOP$K_PAR_HOST_ADDR,
	MOP$K_PAR_TIME
	};

/*
**  The items of the parameters of a satellite, the LAPL: W code, W length
**  with these 4 octets, the data; code 0 ends them
*/
enum	{
	LAPL$K_END		= 0,
	LAPL$K_MSCP_UNIT,			/* UNIT_ID of the system disk			*/
	LAPL$K_SYSROOT,				/* <SYS10.>					*/
	LAPL$K_CLUS_GROUP,			/* The group of the cluster, L			*/
	LAPL$K_BOOT_SCSNODENAME,		/* SCSNODE of the satellite, 8			*/
	LAPL$K_BOOT_SCSNODEID,			/* ... the low word of its SCSSYSTEMID, L	*/
	LAPL$K_PE_SERVICES,			/* ... NISCS_PORT_SERV, L			*/
	LAPL$K_FLAGS,				/* ... NISCS_CONV_BOOT, B			*/
	LAPL$K_SERV_SCSNODENAME,		/* SCSNODE of the MOP server			*/
	LAPL$K_SERV_SCSNODEID,			/* ... its system ID, 6				*/
	LAPL$K_AUTHORIZE,			/* The password hash of the cluster, Q		*/
	LAPL$K_SYSD_UNIQUE_DNAME		/* The service: ASCIC, padded to 16		*/
	};

#define	LAPL$K_DNAMESZ		16

/*
**  The image of an Alpha: what of its header and of its first section
**  descriptor is looked at
*/
#define	EIHD$L_MAJORID		0x00
#define	EIHD$L_ISDOFF		0x0C
#define	EIHD$L_HDRBLKCNT	0x4C
#define	EIHD$K_MAJORID		3
#define	EISD$L_SECSIZE		0x0C

#define	MH$K_MOP_MAXPAR		(256 * 1024)	/* A parameter file at most			*/

static	const uint8_t	s_snap [8] = { 0xAA, 0xAA, 0x03, 0x08, 0x00, 0x2B, 0x60, 0x01 };
static	const uint8_t	s_mopmc [NISCA$K_SZ_MAC] = { 0xAB, 0x00, 0x00, 0x01, 0x00, 0x00 };
static	const char	*s_fmtname [MH$K_MOP_NFMT] = { "V3", "V4" };

static	char		s_hubname [NISCA$K_SZ_NODENAME + 1];	/* SCSNODE of the server		*/
static	uint32_t	s_hubsysid;
static	uint16_t	s_hubgroup;

/*
**  The parameters of the parameter file a satellite gets, as they are
**  named there: a counted name in a field of 16, the value after it
*/
static	const struct {
	const char *	name;
	uint8_t		code;
	uint8_t		len;
	} s_parms [] = {
	{ "SCSNODE",		LAPL$K_BOOT_SCSNODENAME,	8 },
	{ "SCSSYSTEMID",	LAPL$K_BOOT_SCSNODEID,		2 },
	{ "NISCS_PORT_SERV",	LAPL$K_PE_SERVICES,		4 },
	{ "NISCS_CONV_BOOT",	LAPL$K_FLAGS,			1 },
	{ "VAXCLUSTER",		LAPL$K_END,			4 }
	};

#define	MH$K_PAR_FIELD		16



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a MAC address: six pairs of hexadecimal digits, by - or :.
**
**  FORMAL PARAMETERS:
**
**	a_str		The text
**	a_len		Its length
**	a_mac		Output: the address
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - not a MAC address.
**--
*/
static	int	s_mh$mop_mac	(
	const	char *		a_str,
		unsigned	a_len,
		uint8_t *	a_mac
			)
{
char	l_hex [2 * NISCA$K_SZ_MAC];

	if ( a_len != 3 * NISCA$K_SZ_MAC - 1 )
		return	STS$K_ERROR;

	for ( unsigned i = 0; i < NISCA$K_SZ_MAC; i++ )
		{
		if ( (i < NISCA$K_SZ_MAC - 1) && (a_str[3 * i + 2] != '-') && (a_str[3 * i + 2] != ':') )
			return	STS$K_ERROR;

		l_hex[2 * i]	 = a_str[3 * i];
		l_hex[2 * i + 1] = a_str[3 * i + 1];
		}

	return	mh$hex2bin(l_hex, sizeof(l_hex), a_mac, NISCA$K_SZ_MAC);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take the satellites of /SATELLITE0 ... /SATELLITE7: the adapter of a
**	satellite, the unit of its system disk, its root, and the size of
**	the MEMORY LOADs, e.g.
**
**	    /SATELLITE0=08-00-2B-E5-43-01,DKA100,SYS10
**	    /SATELLITE1=08-00-2B-E5-43-02,DKA100,SYS11,SIZE=1024
**
**	The unit must be one of the server, writable (a system disk); the
**	root may be written <SYS10.> or [SYS10.] as for LANCP.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context: the units taken; output: the satellites
**	a_cfg		The configuration
**
**  RETURN VALUE:
**	STS$K_SUCCESS; MH$_IVPARAM has been signalled otherwise.
**--
*/
int	mh$mop_init	(
		MH$PORT *	a_port,
	const	MH$CFG *	a_cfg
			)
{
char		l_spec [ASC$K_SZ + 1], l_what [16], l_mac [3 * NISCA$K_SZ_MAC], *l_f [4], *l_p;
unsigned	l_nf, l_len;
MH$MOPSAT	*l_sat;
long		l_size;

	memcpy(s_hubname, $ASCPTR(&a_cfg->node), $ASCLEN(&a_cfg->node));
	s_hubname[$ASCLEN(&a_cfg->node)] = '\0';
	s_hubsysid	= (uint32_t) a_cfg->sysid;
	s_hubgroup	= (uint16_t) a_cfg->group;

	a_port->nsat = 0;

	for ( unsigned i = 0; i < MH$K_MOP_NFMT; i++ )
		a_port->mopsd[i] = -1;

	for ( unsigned i = 0; i < MH$K_MAXSAT; i++ )
		{
		if ( !$ASCLEN(&a_cfg->sat[i]) )
			continue;

		snprintf(l_what, sizeof(l_what), "SATELLITE%u", i);
		memcpy(l_spec, $ASCPTR(&a_cfg->sat[i]), $ASCLEN(&a_cfg->sat[i]));
		l_spec[$ASCLEN(&a_cfg->sat[i])] = '\0';

		/*
		**  mac,unit,root[,SIZE=n]
		*/
		l_nf = 0;

		for ( l_p = l_spec; l_p && (l_nf < $ARRSZ(l_f)); l_nf++ )
			{
			l_f[l_nf] = l_p;

			if ( (l_p = strchr(l_p, ',')) )
				*l_p++ = '\0';
			}

		if ( (l_nf < 3) || l_p )
			return	$MHMSG(MH$_IVPARAM, $ASCPTR(&a_cfg->sat[i]), l_what, "mac,unit,root[,SIZE=n] is expected");

		l_sat = &a_port->sats[a_port->nsat];
		memset(l_sat, 0, sizeof(*l_sat));

		if ( !(1 & s_mh$mop_mac(l_f[0], (unsigned) strlen(l_f[0]), l_sat->mac)) || (l_sat->mac[0] & 1) )
			return	$MHMSG(MH$_IVPARAM, l_f[0], l_what, "the MAC address of an adapter, 08-00-2B-E5-43-01, is expected");

		for ( unsigned j = 0; j < a_port->nsat; j++ )
			if ( !memcmp(a_port->sats[j].mac, l_sat->mac, NISCA$K_SZ_MAC) )
				return	$MHMSG(MH$_IVPARAM, l_f[0], l_what, "the adapter of another satellite");

		for ( l_sat->unit = 0; l_sat->unit < a_port->nunit; l_sat->unit++ )
			if ( !strcasecmp(a_port->units[l_sat->unit].name, l_f[1]) )
				break;

		if ( l_sat->unit >= a_port->nunit )
			return	$MHMSG(MH$_IVPARAM, l_f[1], l_what, "not a unit of the server");

		if ( a_port->units[l_sat->unit].ro )
			return	$MHMSG(MH$_IVPARAM, l_f[1], l_what, "the unit is read only: a system disk must be writable");

		/*
		**  The root: SYS10, <SYS10.>, [SYS10.]
		*/
		l_p = l_f[2];
		l_p += ((*l_p == '<') || (*l_p == '['));
		l_len = (unsigned) strlen(l_p);
		l_len -= (l_len && ((l_p[l_len - 1] == '>') || (l_p[l_len - 1] == ']')));
		l_len -= (l_len && (l_p[l_len - 1] == '.'));

		if ( !l_len || (l_len >= sizeof(l_sat->root)) )
			return	$MHMSG(MH$_IVPARAM, l_f[2], l_what, "the root of the satellite, SYS10, is expected");

		for ( unsigned j = 0; j < l_len; j++ )
			{
			char	l_c = l_p[j];

			if ( (l_c >= 'a') && (l_c <= 'z') )
				l_c = (char) (l_c - 'a' + 'A');

			if ( !(((l_c >= 'A') && (l_c <= 'Z')) || ((l_c >= '0') && (l_c <= '9')) || (l_c == '$') || (l_c == '_') || (l_c == '-')) )
				return	$MHMSG(MH$_IVPARAM, l_f[2], l_what, "the root of the satellite, SYS10, is expected");

			l_sat->root[j] = l_c;
			}

		l_sat->size = MH$K_MOP_DEFSIZE;

		if ( l_nf > 3 )
			{
			if ( strncasecmp(l_f[3], "SIZE=", 5) || ((l_size = strtol(l_f[3] + 5, &l_p, 10)), *l_p)
				|| (l_size < MH$K_MOP_MINSIZE) || (l_size > MH$K_MOP_DEFSIZE) )
				return	$MHMSG(MH$_IVPARAM, l_f[3], l_what, "SIZE=246 - 1482 is expected");

			l_sat->size = (unsigned) l_size;
			}

		mh$mac2str(l_sat->mac, l_mac, sizeof(l_mac));
		snprintf(l_sat->name, sizeof(l_sat->name), "?");
		a_port->nsat++;

		$MHMSG(MH$_SATCFG, l_mac, a_port->units[l_sat->unit].dvname, l_sat->root, l_sat->size);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A load is ended, or has failed: the image is let go.  A failure is
**	counted and told, the same one not more often than once a minute.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_why		What has failed, NULL - nothing
**
**  RETURN VALUE:
**	STS$K_SUCCESS - ended; MH$_MOPFAIL - failed, told or not.
**--
*/
static	int	s_mh$mop_end	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	char *		a_why
			)
{
char	l_mac [3 * NISCA$K_SZ_MAC];

	free(a_sat->img);
	a_sat->img	= NULL;
	a_sat->imglen	= 0;
	a_sat->ready	= 0;
	a_sat->state	= MH$K_MOP_IDLE;
	a_sat->due	= 0;

	if ( !a_why )
		return	STS$K_SUCCESS;

	a_sat->fails++;

	if ( !a_sat->faillog || (a_port->tick - a_sat->faillog >= MH$K_MOP_FAILLOG) )
		{
		mh$mac2str(a_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_MOPFAIL, l_mac, a_sat->name, a_why);
		a_sat->faillog = a_port->tick;
		}

	return	MH$_MOPFAIL;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put an item to the parameters of a satellite.
**
**  FORMAL PARAMETERS:
**
**	a_sat		The satellite: LAPL, LAPLLEN
**	a_code		LAPL$K_*
**	a_data		Its data
**	a_len		... length
**
**  RETURN VALUE:
**	None: the items are sized so that they fit.
**--
*/
static	void	s_mh$mop_item	(
		MH$MOPSAT *	a_sat,
		uint16_t	a_code,
	const	void *		a_data,
		unsigned	a_len
			)
{
	nisca$put16(a_sat->lapl + a_sat->lapllen, a_code);
	nisca$put16(a_sat->lapl + a_sat->lapllen + 2, (uint16_t) (a_len + 4));
	memcpy(a_sat->lapl + a_sat->lapllen + 4, a_data, a_len);
	a_sat->lapllen += a_len + 4;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make the parameters of a satellite of its parameter file: the items of
**	the server first, then those of the file in the order they are in it,
**	then the group and the password.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite; output: LAPL, SCSNODE, SCSID, NAME
**	a_par		The parameter file
**	a_len		... its length
**	a_why		Output: what is wrong, a_whysz octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - a parameter is not there, or the
**	satellite is no member of a cluster.
**--
*/
static	int	s_mh$mop_lapl	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	uint8_t *	a_par,
		uint32_t	a_len,
		char *		a_why,
		unsigned	a_whysz
			)
{
const	MH$UNIT	*l_unit = &a_port->units[a_sat->unit];
uint8_t		l_key [MH$K_PAR_FIELD], l_b [LAPL$K_DNAMESZ], l_val [8];
uint32_t	l_at [$ARRSZ(s_parms)], l_vaxc;
unsigned	l_klen, l_next, l_order [$ARRSZ(s_parms)], l_n;
char		l_root [48];

	/*
	**  Where each parameter is in the file: the first counted name
	*/
	for ( unsigned i = 0; i < $ARRSZ(s_parms); i++ )
		{
		l_klen = (unsigned) strlen(s_parms[i].name);
		l_key[0] = (uint8_t) l_klen;
		memcpy(l_key + 1, s_parms[i].name, l_klen);
		l_at[i] = UINT32_MAX;

		for ( uint32_t j = 0; j + MH$K_PAR_FIELD + 8 <= a_len; j++ )
			if ( !memcmp(a_par + j, l_key, l_klen + 1) )
				{
				l_at[i] = j;
				break;
				}

		if ( l_at[i] == UINT32_MAX )
			{
			snprintf(a_why, a_whysz, "no %s in [%s.SYSEXE]ALPHAVMSSYS.PAR", s_parms[i].name, a_sat->root);
			return	STS$K_ERROR;
			}
		}

	l_vaxc = nisca$get32(a_par + l_at[$ARRSZ(s_parms) - 1] + MH$K_PAR_FIELD);

	if ( !l_vaxc )
		{
		snprintf(a_why, a_whysz, "VAXCLUSTER is 0 in [%s.SYSEXE]ALPHAVMSSYS.PAR: no member of a cluster", a_sat->root);
		return	STS$K_ERROR;
		}

	memcpy(a_sat->scsnode, a_par + l_at[0] + MH$K_PAR_FIELD, sizeof(a_sat->scsnode));
	memcpy(a_sat->scsid, a_par + l_at[1] + MH$K_PAR_FIELD, sizeof(a_sat->scsid));

	for ( l_n = 0; (l_n < sizeof(a_sat->scsnode)) && (a_sat->scsnode[l_n] > ' '); l_n++ )
		a_sat->name[l_n] = (char) a_sat->scsnode[l_n];

	a_sat->name[l_n] = '\0';

	/*
	**  The items of the server: the root, its name, the service and the
	**  UNIT_ID of the unit, its system ID
	*/
	a_sat->lapllen = 0;

	l_n = (unsigned) snprintf(l_root, sizeof(l_root), "<%s.>", a_sat->root);
	s_mh$mop_item(a_sat, LAPL$K_SYSROOT, l_root, l_n);
	s_mh$mop_item(a_sat, LAPL$K_SERV_SCSNODENAME, s_hubname, (unsigned) strlen(s_hubname));

	memset(l_b, 0, sizeof(l_b));
	l_b[0] = (uint8_t) snprintf((char *) l_b + 1, sizeof(l_b) - 1, "_%s:", l_unit->dvname);
	s_mh$mop_item(a_sat, LAPL$K_SYSD_UNIQUE_DNAME, l_b, sizeof(l_b));

	s_mh$mop_item(a_sat, LAPL$K_MSCP_UNIT, l_unit->unitid, sizeof(l_unit->unitid));

	nisca$put32(l_val, s_hubsysid);
	nisca$put16(l_val + 4, 0);
	s_mh$mop_item(a_sat, LAPL$K_SERV_SCSNODEID, l_val, 6);

	/*
	**  The items of the file, in its order; the low word of SCSSYSTEMID
	**  goes as a longword
	*/
	for ( unsigned i = 0; i < $ARRSZ(s_parms); i++ )
		l_order[i] = i;

	for ( unsigned i = 1; i < $ARRSZ(s_parms); i++ )
		for ( unsigned j = i; j && (l_at[l_order[j - 1]] > l_at[l_order[j]]); j-- )
			{
			l_next		= l_order[j];
			l_order[j]	= l_order[j - 1];
			l_order[j - 1]	= l_next;
			}

	for ( unsigned i = 0; i < $ARRSZ(s_parms); i++ )
		{
		unsigned l_p = l_order[i];

		if ( !s_parms[l_p].code )
			continue;

		memset(l_val, 0, sizeof(l_val));
		memcpy(l_val, a_par + l_at[l_p] + MH$K_PAR_FIELD, s_parms[l_p].len);
		s_mh$mop_item(a_sat, s_parms[l_p].code, l_val, (s_parms[l_p].code == LAPL$K_BOOT_SCSNODEID) ? 4 : s_parms[l_p].len);
		}

	/*
	**  The cluster: the group, the password; the end
	*/
	nisca$put32(l_val, s_hubgroup);
	s_mh$mop_item(a_sat, LAPL$K_CLUS_GROUP, l_val, 4);
	s_mh$mop_item(a_sat, LAPL$K_AUTHORIZE, a_port->auth, MH$K_SZ_AUTH);

	memset(a_sat->lapl + a_sat->lapllen, 0, 4);
	a_sat->lapllen += 4;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make the load of a satellite ready: the first section of APB.EXE of
**	its root and its parameters, read from its system disk now.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite; output: IMG, IMGLEN, LAPL, READY
**
**  RETURN VALUE:
**	STS$K_SUCCESS; the condition value of MOPFAIL otherwise - the load is
**	ended, nothing is answered.
**--
*/
static	int	s_mh$mop_ready	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat
			)
{
const	MH$UNIT	*l_unit = &a_port->units[a_sat->unit];
static	MH$ODSFILE	s_file;
MH$ODS		l_ods;
uint8_t		l_hdr [512], *l_par = NULL;
uint32_t	l_hdrblk, l_isd, l_sec, l_nblk;
char		l_path [96], l_why [sizeof(l_ods.why) + 128];
int		l_ok = 0;

	free(a_sat->img);
	a_sat->img = NULL;
	l_why[0] = '\0';

	if ( !(1 & mh$ods_open(&l_ods, l_unit->dev.fd, (uint64_t) l_unit->size * 512)) )
		{
		snprintf(l_why, sizeof(l_why), "unit %s: %s", l_unit->dvname, l_ods.why);
		mh$ods_close(&l_ods);

		return	s_mh$mop_end(a_port, a_sat, l_why);
		}

	do	{
		/*
		**  The bootstrap: its header, the size of its first section
		*/
		snprintf(l_path, sizeof(l_path), "[%s.SYSCOMMON.SYSEXE]APB.EXE", a_sat->root);

		if ( !(1 & mh$ods_lookup(&l_ods, l_path, &s_file)) || !(1 & mh$ods_read(&l_ods, &s_file, 1, 1, l_hdr)) )
			{
			snprintf(l_why, sizeof(l_why), "%s", l_ods.why);
			break;
			}

		l_hdrblk	= nisca$get32(l_hdr + EIHD$L_HDRBLKCNT);
		l_isd		= nisca$get32(l_hdr + EIHD$L_ISDOFF);
		l_sec		= (l_isd && (l_isd <= sizeof(l_hdr) - 0x10)) ? nisca$get32(l_hdr + l_isd + EISD$L_SECSIZE) : 0;
		l_nblk		= (l_sec + 511) / 512;

		if ( (nisca$get32(l_hdr + EIHD$L_MAJORID) != EIHD$K_MAJORID) || !l_hdrblk || !l_sec || (l_sec > MH$K_MOP_MAXIMG)
			|| ((uint64_t) l_hdrblk + l_nblk > s_file.blocks) )
			{
			snprintf(l_why, sizeof(l_why), "%s is not an image of an Alpha: header of %u block(s), section of %u octets",
				l_path, l_hdrblk, l_sec);
			break;
			}

		if ( !(a_sat->img = malloc((size_t) l_nblk * 512)) || !(1 & mh$ods_read(&l_ods, &s_file, l_hdrblk + 1, l_nblk, a_sat->img)) )
			{
			snprintf(l_why, sizeof(l_why), "%s", a_sat->img ? l_ods.why : "no memory for the image");
			break;
			}

		a_sat->imglen = l_sec;

		/*
		**  The parameter file of the root
		*/
		snprintf(l_path, sizeof(l_path), "[%s.SYSEXE]ALPHAVMSSYS.PAR", a_sat->root);

		if ( !(1 & mh$ods_lookup(&l_ods, l_path, &s_file)) )
			{
			snprintf(l_why, sizeof(l_why), "%s", l_ods.why);
			break;
			}

		if ( !s_file.size || (s_file.size > MH$K_MOP_MAXPAR) || !(l_par = malloc((size_t) s_file.blocks * 512))
			|| !(1 & mh$ods_read(&l_ods, &s_file, 1, s_file.blocks, l_par)) )
			{
			snprintf(l_why, sizeof(l_why), "%s: %s", l_path, l_par ? l_ods.why : "empty, too long or no memory");
			break;
			}

		l_ok = 1 & s_mh$mop_lapl(a_port, a_sat, l_par, (uint32_t) s_file.size, l_why, sizeof(l_why));
		}
	while ( 0 );

	free(l_par);
	mh$ods_close(&l_ods);

	if ( !l_ok )
		return	s_mh$mop_end(a_port, a_sat, l_why);

	a_sat->ready = a_port->tick ? a_port->tick : 1;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send a MOP message to a satellite in the format of its console, and
**	keep it to be sent again.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite: FMT, MAC; output: FRAME, DUE
**	a_msg		The message
**	a_len		Its length
**	a_again		1 - send it again if it is not answered
**
**  RETURN VALUE:
**	The condition value of MH$LAN_MOP_SEND.
**--
*/
static	int	s_mh$mop_send	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	uint8_t *	a_msg,
		unsigned	a_len,
		int		a_again
			)
{
uint8_t		*l_f = a_sat->frame;
unsigned	l_hdr;

	memcpy(l_f, a_sat->mac, NISCA$K_SZ_MAC);
	memcpy(l_f + NISCA$K_SZ_MAC, a_port->mac, NISCA$K_SZ_MAC);

	if ( a_sat->fmt == MH$K_MOP_V3 )
		{
		l_f[12] = (uint8_t) (MH$K_MOP_ETHERTYPE >> 8);
		l_f[13] = (uint8_t) MH$K_MOP_ETHERTYPE;
		nisca$put16(l_f + 14, (uint16_t) a_len);
		l_hdr = 16;
		}
	else	{
		l_f[12] = (uint8_t) ((sizeof(s_snap) + a_len) >> 8);
		l_f[13] = (uint8_t) (sizeof(s_snap) + a_len);
		memcpy(l_f + 14, s_snap, sizeof(s_snap));
		l_hdr = 14 + sizeof(s_snap);
		}

	memcpy(l_f + l_hdr, a_msg, a_len);
	a_sat->framelen = l_hdr + a_len;

	if ( a_sat->framelen < NISCA$K_MINFRAME )
		{
		memset(l_f + a_sat->framelen, 0, NISCA$K_MINFRAME - a_sat->framelen);
		a_sat->framelen = NISCA$K_MINFRAME;
		}

	a_sat->due = a_again ? a_port->tick + MH$K_MOP_RETRY : 0;
	a_sat->tx++;
	a_sat->nmsg++;

	return	mh$lan_mop_send(a_port, a_sat->fmt, l_f, a_sat->framelen);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Send the next message of a load: a MEMORY LOAD of the image from
**	OFFSET, the one of the parameters after the image, or the PARAMETER
**	LOAD WITH TRANSFER ADDRESS.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite: STATE, LOADNUM, OFFSET; output: CURLEN
**
**  RETURN VALUE:
**	The condition value of S_MH$MOP_SEND.
**--
*/
static	int	s_mh$mop_next	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat
			)
{
uint8_t		l_msg [MH$K_MOP_MSGSZ];
unsigned	l_len, l_n;
struct tm	l_tm;
time_t		l_now;

	l_msg[1] = a_sat->loadnum;
	a_sat->curlen = 0;

	switch ( a_sat->state )
		{
		case	MH$K_MOP_IMAGE:
			a_sat->curlen = a_sat->imglen - a_sat->offset;
			a_sat->curlen = (a_sat->curlen < a_sat->size) ? a_sat->curlen : a_sat->size;

			l_msg[0] = MOP$K_MEMLOAD;
			nisca$put32(l_msg + 2, a_sat->offset);
			memcpy(l_msg + 6, a_sat->img + a_sat->offset, a_sat->curlen);
			l_len = 6 + a_sat->curlen;
			break;

		case	MH$K_MOP_PARAM:
			/*
			**  The parameters lie where the image ends: the bootstrap
			**  looks for them right after itself
			*/
			l_msg[0] = MOP$K_MEMLOAD;
			nisca$put32(l_msg + 2, a_sat->imglen);
			memcpy(l_msg + 6, a_sat->lapl, a_sat->lapllen);
			l_len = 6 + a_sat->lapllen;
			break;

		default:
			/*
			**  The satellite and the server as Phase IV names them,
			**  the time, then the transfer address 0
			*/
			l_msg[0] = MOP$K_PARAMLOAD_XFER;
			l_len = 2;

			for ( l_n = 0; (l_n < sizeof(a_sat->scsnode)) && (a_sat->scsnode[l_n] > ' '); l_n++ )
				;

			l_msg[l_len++] = MOP$K_PAR_CLIENT_NAME;
			l_msg[l_len++] = (uint8_t) l_n;
			memcpy(l_msg + l_len, a_sat->scsnode, l_n);
			l_len += l_n;

			l_msg[l_len++] = MOP$K_PAR_CLIENT_ADDR;
			l_msg[l_len++] = 2;
			memcpy(l_msg + l_len, a_sat->scsid, 2);
			l_len += 2;

			l_n = (unsigned) strlen(s_hubname);
			l_msg[l_len++] = MOP$K_PAR_HOST_NAME;
			l_msg[l_len++] = (uint8_t) l_n;
			memcpy(l_msg + l_len, s_hubname, l_n);
			l_len += l_n;

			l_msg[l_len++] = MOP$K_PAR_HOST_ADDR;
			l_msg[l_len++] = 2;
			nisca$put16(l_msg + l_len, (uint16_t) s_hubsysid);
			l_len += 2;

			l_now = time(NULL);
			localtime_r(&l_now, &l_tm);
			l_msg[l_len++] = MOP$K_PAR_TIME;
			l_msg[l_len++] = 10;
			l_msg[l_len++] = (uint8_t) ((l_tm.tm_year + 1900) / 100);
			l_msg[l_len++] = (uint8_t) ((l_tm.tm_year + 1900) % 100);
			l_msg[l_len++] = (uint8_t) (l_tm.tm_mon + 1);
			l_msg[l_len++] = (uint8_t) l_tm.tm_mday;
			l_msg[l_len++] = (uint8_t) l_tm.tm_hour;
			l_msg[l_len++] = (uint8_t) l_tm.tm_min;
			l_msg[l_len++] = (uint8_t) l_tm.tm_sec;
			l_msg[l_len++] = 0;
			l_msg[l_len++] = 0;
			l_msg[l_len++] = 0;

			l_msg[l_len++] = 0;
			nisca$put32(l_msg + l_len, 0);
			l_len += 4;
			break;
		}

	return	s_mh$mop_send(a_port, a_sat, l_msg, l_len, 1);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A REQUEST PROGRAM: by multicast, the satellite asks who will load it
**	- we volunteer if its load can be made ready; to us, it asks us to
**	load it - the first MEMORY LOAD goes at once.  A console that asks
**	for a file by its name (BOOT -FILE), or for anything but an operating
**	system, is not ours to load.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_mcast		1 - the request is a multicast
**	a_msg		The message
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of a message.
**--
*/
static	int	s_mh$mop_reqpgm	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
		int		a_mcast,
	const	uint8_t *	a_msg,
		unsigned	a_len
			)
{
static	const uint8_t	s_assvol [1] = { MOP$K_ASSVOL };
char		l_mac [3 * NISCA$K_SZ_MAC];
int		l_status;

	if ( (a_len < 5) || (a_msg[3] != MOP$K_PGM_SYSTEM) || ((int8_t) a_msg[4] > 0) )
		{
		if ( a_port->trace )
			$LOG_FAO(STS$K_INFO, "REQUEST PROGRAM of type !UL, software ID !SL, of !AZ: not ours", (a_len > 3) ? a_msg[3] : 0,
				(a_len > 4) ? (int) (int8_t) a_msg[4] : 0, a_sat->name);

		return	STS$K_SUCCESS;
		}

	if ( a_mcast )
		{
		if ( !(1 & (l_status = s_mh$mop_ready(a_port, a_sat))) )
			return	l_status;

		a_sat->state = MH$K_MOP_VOLUNTEER;
		mh$mac2str(a_sat->mac, l_mac, sizeof(l_mac));
		$MHMSG(MH$_MOPVOL, l_mac, a_sat->name, s_fmtname[a_sat->fmt], a_port->units[a_sat->unit].dvname, a_sat->root, a_sat->imglen);

		return	s_mh$mop_send(a_port, a_sat, s_assvol, sizeof(s_assvol), 0);
		}

	/*
	**  The load: what we have volunteered with, if it is fresh; anew
	**  otherwise - and anew when a console starts over
	*/
	if ( !a_sat->img || !a_sat->ready || (a_port->tick - a_sat->ready >= MH$K_MOP_FRESH) || (a_sat->state != MH$K_MOP_VOLUNTEER) )
		if ( !(1 & (l_status = s_mh$mop_ready(a_port, a_sat))) )
			return	l_status;

	a_sat->state	= MH$K_MOP_IMAGE;
	a_sat->loadnum	= 0;
	a_sat->offset	= 0;
	a_sat->begun	= a_port->tick;
	a_sat->nmsg	= 0;
	a_sat->nrexmt	= 0;

	return	s_mh$mop_next(a_port, a_sat);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A REQUEST MEMORY LOAD: the number the console wants next.  The one
**	sent - it is lost, again; the one after it - what was sent is taken,
**	the next goes, or the load is over; another - the load is ended.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_sat		The satellite
**	a_msg		The message
**	a_len		Its length
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of a message.
**--
*/
static	int	s_mh$mop_reqmem	(
		MH$PORT *	a_port,
		MH$MOPSAT *	a_sat,
	const	uint8_t *	a_msg,
		unsigned	a_len
			)
{
char		l_mac [3 * NISCA$K_SZ_MAC], l_why [64];
uint64_t	l_ms;

	if ( (a_len < 2) || (a_sat->state < MH$K_MOP_IMAGE) )
		return	STS$K_SUCCESS;

	if ( a_msg[1] == a_sat->loadnum )
		{
		a_sat->rexmt++;
		a_sat->nrexmt++;

		return	mh$lan_mop_send(a_port, a_sat->fmt, a_sat->frame, a_sat->framelen);
		}

	if ( a_msg[1] != (uint8_t) (a_sat->loadnum + 1) )
		{
		snprintf(l_why, sizeof(l_why), "the console asks for MEMORY LOAD %u, %u was sent", a_msg[1], a_sat->loadnum);

		return	s_mh$mop_end(a_port, a_sat, l_why);
		}

	a_sat->loadnum++;

	switch ( a_sat->state )
		{
		case	MH$K_MOP_IMAGE:
			a_sat->offset += a_sat->curlen;

			if ( a_sat->offset >= a_sat->imglen )
				a_sat->state = MH$K_MOP_PARAM;

			break;

		case	MH$K_MOP_PARAM:
			a_sat->state = MH$K_MOP_XFER;
			break;

		default:
			/*
			**  The transfer address is taken: the bootstrap runs
			*/
			a_sat->loads++;
			l_ms = (a_port->tick - a_sat->begun) * MH$K_TICK_MS;
			mh$mac2str(a_sat->mac, l_mac, sizeof(l_mac));
			$MHMSG(MH$_MOPLOAD, l_mac, a_sat->name, a_sat->imglen, a_sat->nmsg, a_sat->nrexmt, (unsigned) l_ms);

			return	s_mh$mop_end(a_port, a_sat, NULL);
		}

	return	s_mh$mop_next(a_port, a_sat);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Take a frame of MOP: the message out of its frame of V3 or V4, the
**	satellite by its adapter, the message to its handler.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**	a_fmt		The socket it came by: MH$K_MOP_V3, MH$K_MOP_V4
**	a_frame		The frame, the Ethernet header included
**	a_len		Its length
**
**  IMPLICIT INPUTS/OUTPUTS:
**	MOP_RX, MOP_OTHER of the port.
**
**  RETURN VALUE:
**	STS$K_SUCCESS, or the condition value of a message.
**--
*/
int	mh$mop_input	(
		MH$PORT *	a_port,
		unsigned	a_fmt,
	const	uint8_t *	a_frame,
		unsigned	a_len
			)
{
const	uint8_t	*l_msg;
unsigned	l_mlen;
int		l_mcast;
MH$MOPSAT	*l_sat = NULL;

	if ( a_fmt == MH$K_MOP_V3 )
		{
		if ( (a_len < 17) || (((a_frame[12] << 8) | a_frame[13]) != MH$K_MOP_ETHERTYPE) )
			return	STS$K_SUCCESS;

		l_mlen	= nisca$get16(a_frame + 14);
		l_msg	= a_frame + 16;

		if ( 16 + l_mlen > a_len )
			return	STS$K_SUCCESS;
		}
	else	{
		l_mlen = (unsigned) ((a_frame[12] << 8) | a_frame[13]);

		if ( (a_len < 23) || memcmp(a_frame + 14, s_snap, sizeof(s_snap)) || (l_mlen <= sizeof(s_snap)) || (14 + l_mlen > a_len) )
			return	STS$K_SUCCESS;

		l_mlen	-= sizeof(s_snap);
		l_msg	= a_frame + 14 + sizeof(s_snap);
		}

	/*
	**  Ours are the multicast of MOP and what is sent to us; a frame to
	**  another MOP server - a bridge, or a promiscuous adapter, shows it -
	**  is not: answering it would load a console that another server loads
	*/
	l_mcast = !memcmp(a_frame, s_mopmc, NISCA$K_SZ_MAC);

	if ( !l_mcast && memcmp(a_frame, a_port->mac, NISCA$K_SZ_MAC) )
		return	STS$K_SUCCESS;

	a_port->mop_rx++;

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		if ( !memcmp(a_port->sats[i].mac, a_frame + NISCA$K_SZ_MAC, NISCA$K_SZ_MAC) )
			{
			l_sat = &a_port->sats[i];
			break;
			}

	if ( !l_sat )
		{
		a_port->mop_other++;
		return	STS$K_SUCCESS;
		}

	l_sat->heard = a_port->tick;

	switch ( l_msg[0] )
		{
		case	MOP$K_REQPGM:
			l_sat->fmt = a_fmt;

			return	s_mh$mop_reqpgm(a_port, l_sat, l_mcast, l_msg, l_mlen);

		case	MOP$K_REQMEMLOAD:
			if ( a_fmt != l_sat->fmt )
				return	STS$K_SUCCESS;

			return	s_mh$mop_reqmem(a_port, l_sat, l_msg, l_mlen);
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The tick of the MOP server: a message unanswered for a second goes
**	again, a load whose console is silent for a minute is ended, an image
**	we volunteered with and nobody came for is let go.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$mop_tick	(
		MH$PORT *	a_port
			)
{
	for ( unsigned i = 0; i < a_port->nsat; i++ )
		{
		MH$MOPSAT *l_sat = &a_port->sats[i];

		switch ( l_sat->state )
			{
			case	MH$K_MOP_IDLE:
				break;

			case	MH$K_MOP_VOLUNTEER:
				if ( a_port->tick - l_sat->ready >= MH$K_MOP_FRESH )
					s_mh$mop_end(a_port, l_sat, NULL);

				break;

			default:
				if ( a_port->tick - l_sat->heard >= MH$K_MOP_TMO )
					{
					s_mh$mop_end(a_port, l_sat, "the console has not answered for 60 seconds, the load is ended");
					break;
					}

				if ( l_sat->due && (a_port->tick >= l_sat->due) )
					{
					l_sat->rexmt++;
					l_sat->nrexmt++;
					l_sat->due = a_port->tick + MH$K_MOP_RETRY;
					mh$lan_mop_send(a_port, l_sat->fmt, l_sat->frame, l_sat->framelen);
					}

				break;
			}
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The counters of every satellite, on SIGUSR1.
**
**  FORMAL PARAMETERS:
**
**	a_port		The port context
**
**  RETURN VALUE:
**	The condition value of the last message, STS$K_SUCCESS if none.
**--
*/
int	mh$mop_stats	(
		MH$PORT *	a_port
			)
{
static	const char	*s_state [MH$K_MOP_NSTATE] = { "idle", "volunteered", "loading the image", "loading the parameters",
				"transfer address sent" };
char	l_mac [3 * NISCA$K_SZ_MAC];
int	l_status = STS$K_SUCCESS;

	for ( unsigned i = 0; i < a_port->nsat; i++ )
		{
		const	MH$MOPSAT *l_sat = &a_port->sats[i];

		mh$mac2str(l_sat->mac, l_mac, sizeof(l_mac));
		l_status = $MHMSG(MH$_MOPSTATS, l_mac, l_sat->name, s_state[l_sat->state], l_sat->loads, l_sat->fails, l_sat->tx, l_sat->rexmt);
		}

	return	l_status;
}
