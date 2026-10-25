#define	__MODULE__	"T_ODS"
#define	__IDENT__	"X01-22"
#define	__REV__		"1.22.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_ods.c
**
**  ABSTRACT:	Unit tests of the reader of a Files-11 volume: a small ODS-2
**		volume is made here - the home block, INDEXF.SYS, the MFD, a
**		root SYS10 with its SYSCOMMON an alias of VMS$COMMON, two
**		SYSEXE directories and their files - and the files are found
**		and read through the reader, the way the MOP server takes the
**		bootstrap and the parameter file of a satellite.
**
**		With three arguments it reads a file of a real volume instead:
**
**		    t_ods <image or device> <[DIR...]NAME.TYPE> <output file>
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  25-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		25-OCT-2026	RRL	X01-22 : Initial revision.
**
**--
*/

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<fcntl.h>
#include	<unistd.h>
#include	<sys/stat.h>

#include	"mh_def.h"
#include	"t_vol.h"

static	int		s_fails, s_checks;

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
	fprintf(stderr, "T_ODS:%d: FAILED: %s\n", a_line, a_what);

	return	STS$K_ERROR;
}






/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read a file of a real volume into a file of the host.
**
**  FORMAL PARAMETERS:
**
**	a_vol		The image or the device
**	a_path		[DIR...]NAME.TYPE
**	a_out		The file to write
**
**  RETURN VALUE:
**	0 - read; 1 - not.
**--
*/
static	int	s_t$extract	(
	const	char *		a_vol,
	const	char *		a_path,
	const	char *		a_out
			)
{
MH$ODS		l_ods;
static	MH$ODSFILE	s_file;
struct stat	l_st;
uint8_t		*l_data;
FILE		*l_fp;
int		l_fd;

	if ( (0 > (l_fd = open(a_vol, O_RDONLY))) || fstat(l_fd, &l_st) )
		{
		perror(a_vol);
		return	1;
		}

	if ( !(1 & mh$ods_open(&l_ods, l_fd, (uint64_t) l_st.st_size)) || !(1 & mh$ods_lookup(&l_ods, a_path, &s_file)) )
		{
		fprintf(stderr, "T_ODS: %s\n", l_ods.why);
		return	1;
		}

	printf("T_ODS: volume %s, ODS-%u; %s is (%u,%u), %u block(s), %u extent(s), %llu octets\n", l_ods.volname, l_ods.level,
		a_path, s_file.num, s_file.seq, s_file.blocks, s_file.nmap, (unsigned long long) s_file.size);

	for ( unsigned i = 0; i < s_file.nmap; i++ )
		printf("T_ODS:   VBN %u: %u block(s) at LBN %llu\n", s_file.map[i].vbn, s_file.map[i].count,
			(unsigned long long) s_file.map[i].lbn);

	if ( !(l_data = malloc((size_t) s_file.blocks * 512 + 1)) || !(1 & mh$ods_read(&l_ods, &s_file, 1, s_file.blocks, l_data)) )
		{
		fprintf(stderr, "T_ODS: %s\n", l_ods.why);
		return	1;
		}

	if ( !(l_fp = fopen(a_out, "wb")) || (fwrite(l_data, 1, (size_t) s_file.size, l_fp) != s_file.size) || fclose(l_fp) )
		{
		perror(a_out);
		return	1;
		}

	mh$ods_close(&l_ods);
	close(l_fd);

	return	0;
}



int	main	(
		int		a_argc,
		char **		a_argv
		)
{
MH$ODS		l_ods;
static	MH$ODSFILE	s_file;
uint8_t		l_data [8 * 512];
char		l_img [] = "t_ods_XXXXXX";
int		l_fd, l_ok;

	mh$inimsg();

	if ( a_argc == 4 )
		return	s_t$extract(a_argv[1], a_argv[2], a_argv[3]);

	s_t$mkvol();

	l_fd = mkstemp(l_img);
	$CHECK((l_fd >= 0) && (write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol)), "the volume is written");

	$CHECK(1 & mh$ods_open(&l_ods, l_fd, sizeof(s_vol)), "the home block and INDEXF.SYS are taken");
	$CHECK((l_ods.level == 2) && !strcmp(l_ods.volname, "SATSYS"), "ODS-2, the volume SATSYS");

	/*
	**  The bootstrap of a satellite: through the alias SYSCOMMON, the
	**  highest version, two extents, the second in an extension header
	*/
	$CHECK(1 & mh$ods_lookup(&l_ods, "[SYS10.SYSCOMMON.SYSEXE]APB.EXE", &s_file), "[SYS10.SYSCOMMON.SYSEXE]APB.EXE is found");
	$CHECK((s_file.num == 25) && (s_file.seq == 3), "... the highest version, (25,3)");
	$CHECK((s_file.nmap == 2) && (s_file.blocks == 5) && (s_file.size == 5 * 512), "... 2 extents, 5 blocks, 2560 octets");
	$CHECK((s_file.map[1].vbn == 4) && (s_file.map[1].lbn == 1000) && (s_file.map[1].count == 2), "... VBN 4 at LBN 1000 by format 3");

	memset(l_data, 0, sizeof(l_data));
	$CHECK(1 & mh$ods_read(&l_ods, &s_file, 1, 5, l_data), "... its 5 blocks are read");

	l_ok = 1;

	for ( unsigned i = 0; i < 5; i++ )
		for ( unsigned j = 0; j < 512; j++ )
			l_ok &= (l_data[i * 512 + j] == 0xA0 + i);

	$CHECK(l_ok, "... every block is the one of its VBN");

	$CHECK(1 & mh$ods_read(&l_ods, &s_file, 3, 2, l_data), "... VBN 3 - 4, across the extents");
	$CHECK((l_data[0] == 0xA2) && (l_data[512] == 0xA3), "... the right blocks");
	$CHECK(STS$K_ERROR == mh$ods_read(&l_ods, &s_file, 6, 1, l_data), "... VBN 6 is not mapped");

	/*
	**  The parameter file: the directory has it in its second block; the
	**  end of file is in the second block, at 188
	*/
	$CHECK(1 & mh$ods_lookup(&l_ods, "<SYS10.SYSEXE>ALPHAVMSSYS.PAR", &s_file), "<SYS10.SYSEXE>ALPHAVMSSYS.PAR is found");
	$CHECK((s_file.num == 24) && (s_file.size == 512 + 188), "... (24,1), 700 octets");
	$CHECK(1 & mh$ods_lookup(&l_ods, "[sys10.sysexe]alphavmssys.par", &s_file), "... and in lower case");

	/*
	**  What is not there, or not right
	*/
	$CHECK(STS$K_WARN == mh$ods_lookup(&l_ods, "[SYS10.SYSEXE]APB.EXE", &s_file), "no APB.EXE in [SYS10.SYSEXE]: WARN");
	$CHECK(STS$K_WARN == mh$ods_lookup(&l_ods, "[SYS11.SYSEXE]APB.EXE", &s_file), "no [SYS11]: WARN");
	$CHECK(STS$K_ERROR == mh$ods_lookup(&l_ods, "[SYS10.SYSCOMMON.SYSEXE]BAD.EXE", &s_file), "a FID of a wrong sequence: ERROR");
	$CHECK(STS$K_ERROR == mh$ods_lookup(&l_ods, "[SYS10.SYSCOMMON.SYSEXE]LOST.EXE", &s_file), "a file with no header: ERROR");
	$CHECK(STS$K_ERROR == mh$ods_lookup(&l_ods, "[SYS10.SYSEXE]", &s_file), "no file name: ERROR");
	$CHECK(STS$K_ERROR == mh$ods_lookup(&l_ods, "SYS10.SYSEXE]APB.EXE", &s_file), "no bracket: ERROR");

	/*
	**  A header with a wrong checksum is not taken
	*/
	$BLK(T$K_HDRLBN + 24 - 1)[0x100] ^= 1;
	lseek(l_fd, 0, SEEK_SET);
	$CHECK(write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol), "a header spoilt");
	$CHECK(STS$K_ERROR == mh$ods_lookup(&l_ods, "[SYS10.SYSEXE]ALPHAVMSSYS.PAR", &s_file), "... is not taken: ERROR");
	mh$ods_close(&l_ods);

	/*
	**  No home block at LBN 1: the next one that is one; none at all
	*/
	memcpy($BLK(7), $BLK(1), 512);
	nisca$put32($BLK(7), 7);
	s_t$cksum($BLK(7), 0x03A);
	s_t$cksum($BLK(7), 0x1FE);
	memset($BLK(1), 0, 512);
	lseek(l_fd, 0, SEEK_SET);
	$CHECK(write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol), "the home block moved to LBN 7");
	$CHECK(1 & mh$ods_open(&l_ods, l_fd, sizeof(s_vol)), "... it is found there");
	mh$ods_close(&l_ods);

	memset($BLK(7), 0, 512);
	lseek(l_fd, 0, SEEK_SET);
	$CHECK(write(l_fd, s_vol, sizeof(s_vol)) == (ssize_t) sizeof(s_vol), "no home block at all");
	$CHECK(STS$K_ERROR == mh$ods_open(&l_ods, l_fd, sizeof(s_vol)), "... not a Files-11 volume: ERROR");
	mh$ods_close(&l_ods);

	close(l_fd);
	unlink(l_img);

	printf("T_ODS: %d check(s), %d failed\n", s_checks, s_fails);

	return	s_fails ? 1 : 0;
}
