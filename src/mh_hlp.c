#define	__MODULE__	"MHHLP"
#define	__IDENT__	"X01-11"
#define	__REV__		"1.11.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_hlp.c
**
**  ABSTRACT:	/HELP: the description of the server, answered out of its
**		help library by the HELP product.
**
**  DESCRIPTION: The library is looked for, in this order:
**
**		1. the environment variable MSCPHUB_HELPLIB, taken as it stands;
**		2. <image>/../share/help/mscphub.hlb - an installed tree;
**		3. <image>/mscphub.hlb - a build tree;
**		4. where the product was configured to install it.
**
**		Everything the library documents hangs below its one key of
**		level one, MSCPHUB, so the words of /HELP=... are looked up
**		from there: /HELP="Messages IDCLASH".
**
**		Built without the HELP product, the server answers /HELP by a
**		short summary of the parameters.
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

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>
#include	<sys/stat.h>

#include	"mh_def.h"

static	const char	s_summary [] =
	"MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA\n\n"
	"  mscphub /CONFIG=file | /INTERFACE=if /NODE=name /SYSTEMID=n /GROUP=n /AUTHORIZE=hex16\n"
	"          [/ALLOCLASS=n] [/UNITn=name,path[,RO][,MEDIA=m][,SECTOR=n]] ...\n"
	"          [/SYNC=DSYNC|GROUP|NONE] [/CREDITS=n] [/TIMEZONE=LOCAL|UTC|+hh:mm] [/ACK=HOLD|FLUSH]\n"
	"          [/LISTEN=s] [/PASSIVE] [/TRACE] [/UNSAFE] [/LOGFILE=file] [/HELP[=\"topic ...\"]]\n\n"
	"  The help library was not found; see the manual page: man 8 mscphub.\n";




/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Display the summary built into the image.
**
**  FORMAL PARAMETERS:
**	None.
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - WRITE(2) failed.
**--
*/
static	int	s_mh$summary	(void)
{
	return	(sizeof(s_summary) - 1 == (size_t) write(STDOUT_FILENO, s_summary, sizeof(s_summary) - 1)) ? STS$K_SUCCESS : STS$K_ERROR;
}

#ifdef	MSCPHUB_WITH_HLB

#include	"helpdef.h"

#define	MH$K_HLBNAME		"mscphub.hlb"
#define	MH$K_HLBTOP		"MSCPHUB"
#define	MH$K_SZ_PATH		1024
#define	MH$K_MAXWORD		8



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is the file a readable regular file?
**
**  FORMAL PARAMETERS:
**
**	a_fspec		The file specification
**
**  RETURN VALUE:
**	STS$K_SUCCESS - yes; STS$K_WARN - no.
**--
*/
static	int	s_mh$readable	(
	const	char *		a_fspec
			)
{
struct stat	l_st = {0};

	if ( !a_fspec || !*a_fspec )
		return	STS$K_WARN;

	if ( stat(a_fspec, &l_st) || !S_ISREG(l_st.st_mode) )
		return	STS$K_WARN;

	return	access(a_fspec, R_OK) ? STS$K_WARN : STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Find the help library, see the DESCRIPTION of the module.
**
**  FORMAL PARAMETERS:
**
**	a_bufsz		Size of the output area
**	a_buf		Output: the file specification, ASCIZ
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - there is no library.
**--
*/
static	int	s_mh$findlib	(
		unsigned	a_bufsz,
		char *		a_buf
			)
{
const char *	l_env;
char		l_image [MH$K_SZ_PATH] = {0};
char *		l_slash;
ssize_t		l_len;

	if ( (l_env = getenv("MSCPHUB_HELPLIB")) && *l_env )
		{
		snprintf(a_buf, a_bufsz, "%s", l_env);

		if ( 1 & s_mh$readable(a_buf) )
			return	STS$K_SUCCESS;
		}

	if ( 0 < (l_len = readlink("/proc/self/exe", l_image, sizeof(l_image) - 1)) )
		{
		l_image[l_len] = '\0';

		if ( (l_slash = strrchr(l_image, '/')) )
			{
			*l_slash = '\0';

			snprintf(a_buf, a_bufsz, "%s/../share/help/" MH$K_HLBNAME, l_image);

			if ( 1 & s_mh$readable(a_buf) )
				return	STS$K_SUCCESS;

			snprintf(a_buf, a_bufsz, "%s/" MH$K_HLBNAME, l_image);

			if ( 1 & s_mh$readable(a_buf) )
				return	STS$K_SUCCESS;
			}
		}

#ifdef	MSCPHUB_K_HLBSPEC
	snprintf(a_buf, a_bufsz, "%s", MSCPHUB_K_HLBSPEC);

	if ( 1 & s_mh$readable(a_buf) )
		return	STS$K_SUCCESS;
#endif

	a_buf[0] = '\0';

	return	STS$K_WARN;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Display a topic of the help library.
**
**  FORMAL PARAMETERS:
**
**	a_topic		The words of the topic, blank-separated, ASCIZ;
**			empty - the description of the server
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - no library, the summary has been shown;
**	MH$_NOTOPIC - the library has no such topic.
**--
*/
int	mh$help	(
	const	char *		a_topic
			)
{
HLB$LIB *	l_lib = NULL;
HELP$OUT	l_out;
const char *	l_txt = NULL;
char		l_fspec [MH$K_SZ_PATH] = {0}, l_path [MH$K_SZ_PATH] = {0}, l_words [ASC$K_SZ + 1];
char *		l_word [MH$K_MAXWORD], *l_save = NULL, *l_p;
unsigned	l_txtlen = 0, l_pathlen = 0;
int		l_parent, l_index, l_used, l_pos = 0, l_wordcnt = 0;

	if ( !(1 & s_mh$findlib((unsigned) sizeof(l_fspec), l_fspec)) || !(1 & hlb$open(l_fspec, &l_lib)) )
		{
		s_mh$summary();
		return	STS$K_WARN;
		}

	if ( !(1 & hlb$lookup(l_lib, -1, (unsigned) sizeof(MH$K_HLBTOP) - 1, MH$K_HLBTOP, &l_parent)) )
		{
		hlb$close(l_lib);
		s_mh$summary();
		return	STS$K_WARN;
		}

	snprintf(l_words, sizeof(l_words), "%s", a_topic ? a_topic : "");

	for ( l_p = strtok_r(l_words, " \t", &l_save); l_p && (l_wordcnt < MH$K_MAXWORD); l_p = strtok_r(NULL, " \t", &l_save) )
		l_word[l_wordcnt++] = l_p;

	while ( l_pos < l_wordcnt )
		{
		if ( !(1 & hlb$lookup_run(l_lib, l_parent, l_wordcnt - l_pos, l_word + l_pos, &l_used, &l_index)) )
			{
			hlb$close(l_lib);
			return	$MHMSG(MH$_NOTOPIC, l_word[l_pos]);
			}

		l_parent = l_index;
		l_pos += l_used;
		}

	if ( !(1 & hlb$gettext(l_lib, l_parent, &l_txtlen, &l_txt)) )
		{
		hlb$close(l_lib);
		return	STS$K_WARN;
		}

	help$out_init(&l_out, STDOUT_FILENO, STDIN_FILENO, isatty(STDOUT_FILENO), 0);

	if ( 1 & hlb$path(l_lib, l_parent, (unsigned) sizeof(l_path), l_path, &l_pathlen) )
		help$put(&l_out, "\n  !AD\n\n", 2, (fao_prm_t) l_pathlen, (fao_prm_t) l_path);

	help$put_text(&l_out, 4, l_txtlen, l_txt);
	help$put(&l_out, "\n", 0);
	help$flush(&l_out);

	hlb$close(l_lib);

	return	STS$K_SUCCESS;
}

#else	/* MSCPHUB_WITH_HLB */

int	mh$help	(
	const	char *		a_topic
			)
{
	s_mh$summary();

	return	STS$K_WARN;
}

#endif	/* MSCPHUB_WITH_HLB */
