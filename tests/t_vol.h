#ifndef	__T_VOL$H__
#define	__T_VOL$H__	1

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	t_vol.h
**
**  ABSTRACT:	A small ODS-2 volume for the unit tests, made in memory: the
**		home block, INDEXF.SYS, the MFD, a root SYS10 with its SYSCOMMON
**		an alias of VMS$COMMON, two SYSEXE directories, APB.EXE of two
**		extents (the second in an extension header) and
**		ALPHAVMSSYS.PAR.  T_ODS reads it, T_MOP loads a satellite from
**		it.
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

#include	<stdint.h>
#include	<string.h>

#include	"nisca.h"

#define	T$K_BLOCKS	4096			/* The volume: 2 MB				*/
#define	T$K_IBMAPVBN	3
#define	T$K_IBMAPLBN	10
#define	T$K_HDRLBN	11			/* The header of file 1				*/
#define	T$K_MAXFILES	64

static	uint8_t		s_vol [T$K_BLOCKS * 512];

#define	$BLK(lbn)		(s_vol + (size_t) (lbn) * 512)



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put the checksum of a block: the sum of the words before it.
**
**  FORMAL PARAMETERS:
**
**	a_blk		The block
**	a_off		Offset of the checksum word
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$cksum	(
		uint8_t *	a_blk,
		unsigned	a_off
			)
{
uint16_t	l_sum = 0;

	for ( unsigned i = 0; i < a_off; i += 2 )
		l_sum = (uint16_t) (l_sum + nisca$get16(a_blk + i));

	nisca$put16(a_blk + a_off, l_sum);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make a file header in the index file: the FID, the extension FID, the
**	end of file, the map of retrieval pointers given as words.
**
**  FORMAL PARAMETERS:
**
**	a_num, a_seq	The FID
**	a_xnum, a_xseq	The extension header, 0 - none
**	a_efblk		End of file: its block ...
**	a_ffbyte	... its first free octet
**	a_map		Retrieval pointers, words
**	a_nmap		How many words
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$hdr	(
		uint32_t	a_num,
		uint16_t	a_seq,
		uint32_t	a_xnum,
		uint16_t	a_xseq,
		uint32_t	a_efblk,
		uint16_t	a_ffbyte,
	const	uint16_t *	a_map,
		unsigned	a_nmap
			)
{
uint8_t	*l_h = $BLK(T$K_HDRLBN + a_num - 1);

	memset(l_h, 0, 512);
	l_h[0x00] = 0x28;				/* IDOFFSET: words				*/
	l_h[0x01] = 0x64;				/* MPOFFSET					*/
	l_h[0x02] = 0xFF;
	l_h[0x03] = 0xFF;
	nisca$put16(l_h + 0x06, 0x0201);		/* Structure level 2, version 1			*/
	nisca$put16(l_h + 0x08, (uint16_t) a_num);
	nisca$put16(l_h + 0x0A, a_seq);
	l_h[0x0D] = (uint8_t) (a_num >> 16);
	nisca$put16(l_h + 0x0E, (uint16_t) a_xnum);
	nisca$put16(l_h + 0x10, a_xseq);
	nisca$put16(l_h + 0x14 + 0x08, (uint16_t) (a_efblk >> 16));	/* EFBLK, high word first	*/
	nisca$put16(l_h + 0x14 + 0x0A, (uint16_t) a_efblk);
	nisca$put16(l_h + 0x14 + 0x0C, a_ffbyte);
	l_h[0x3A] = (uint8_t) a_nmap;

	for ( unsigned i = 0; i < a_nmap; i++ )
		nisca$put16(l_h + 0xC8 + 2 * i, a_map[i]);

	s_t$cksum(l_h, 0x1FE);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Put a directory record: a name and one or two versions.
**
**  FORMAL PARAMETERS:
**
**	a_blk		The block
**	a_off		Offset; output: the offset after the record
**	a_flags		DIR$B_FLAGS
**	a_name		NAME.TYPE
**	a_num, a_seq	The FID of the highest version
**	a_num2, a_seq2	... of the version below it, 0 - none
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$dirent	(
		uint8_t *	a_blk,
		unsigned *	a_off,
		uint8_t		a_flags,
	const	char *		a_name,
		uint32_t	a_num,
		uint16_t	a_seq,
		uint32_t	a_num2,
		uint16_t	a_seq2
			)
{
uint8_t		*l_r = a_blk + *a_off;
unsigned	l_nlen = (unsigned) strlen(a_name), l_val, l_size;

	l_val = 6 + ((l_nlen + 1) & ~1u);
	l_size = l_val + (a_num2 ? 16 : 8);

	nisca$put16(l_r + 0, (uint16_t) (l_size - 2));
	nisca$put16(l_r + 2, 0);
	l_r[4] = a_flags;
	l_r[5] = (uint8_t) l_nlen;
	memcpy(l_r + 6, a_name, l_nlen);

	nisca$put16(l_r + l_val, a_num2 ? 2 : 1);
	nisca$put16(l_r + l_val + 2, (uint16_t) a_num);
	nisca$put16(l_r + l_val + 4, a_seq);
	l_r[l_val + 7] = (uint8_t) (a_num >> 16);

	if ( a_num2 )
		{
		nisca$put16(l_r + l_val + 8, 1);
		nisca$put16(l_r + l_val + 10, (uint16_t) a_num2);
		nisca$put16(l_r + l_val + 12, a_seq2);
		}

	*a_off += l_size;
	nisca$put16(a_blk + *a_off, 0xFFFF);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	A retrieval pointer of format 1: up to 256 blocks below LBN 2^22.
**
**  FORMAL PARAMETERS:
**
**	a_map		Output: two words
**	a_lbn		The first block
**	a_count		Blocks
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$fm1	(
		uint16_t *	a_map,
		uint32_t	a_lbn,
		uint32_t	a_count
			)
{
	a_map[0] = (uint16_t) (0x4000 | (((a_lbn >> 16) & 0x3F) << 8) | (a_count - 1));
	a_map[1] = (uint16_t) a_lbn;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Make the volume: the files and their blocks, every block of a data
**	file filled with a value of its own.
**
**  FORMAL PARAMETERS:
**	None.
**
**  IMPLICIT OUTPUTS:
**	S_VOL.
**
**  RETURN VALUE:
**	None.
**--
*/
static	void	s_t$mkvol	(void)
{
uint8_t		*l_hm = $BLK(1), *l_b;
uint16_t	l_map [8];
unsigned	l_off;

	memset(s_vol, 0, sizeof(s_vol));

	/*
	**  The home block at LBN 1
	*/
	nisca$put32(l_hm + 0x000, 1);
	nisca$put16(l_hm + 0x00C, 0x0201);
	nisca$put16(l_hm + 0x00E, 1);
	nisca$put16(l_hm + 0x016, T$K_IBMAPVBN);
	nisca$put32(l_hm + 0x018, T$K_IBMAPLBN);
	nisca$put32(l_hm + 0x01C, T$K_MAXFILES);
	nisca$put16(l_hm + 0x020, 1);
	s_t$cksum(l_hm, 0x03A);
	memcpy(l_hm + 0x1D8, "SATSYS      ", 12);
	memcpy(l_hm + 0x1F0, "DECFILE11B  ", 12);
	s_t$cksum(l_hm, 0x1FE);

	/*
	**  INDEXF.SYS (1,1): VBN 1 - 2 the boot and the home block, VBN 3 the
	**  bitmap at LBN 10, then the headers from LBN 11
	*/
	s_t$fm1(l_map, T$K_IBMAPLBN - T$K_IBMAPVBN + 1, T$K_IBMAPVBN + T$K_MAXFILES);
	s_t$hdr(1, 1, 0, 0, T$K_IBMAPVBN + T$K_MAXFILES + 1, 0, l_map, 2);

	/*
	**  The MFD (4,4) at LBN 200: SYS10 (20,1), VMS$COMMON (21,1)
	*/
	s_t$fm1(l_map, 200, 1);
	s_t$hdr(4, 4, 0, 0, 2, 0, l_map, 2);
	l_b = $BLK(200);
	l_off = 0;
	s_t$dirent(l_b, &l_off, 0, "000000.DIR", 4, 4, 0, 0);
	s_t$dirent(l_b, &l_off, 0, "SYS10.DIR", 20, 1, 0, 0);
	s_t$dirent(l_b, &l_off, 0, "VMS$COMMON.DIR", 21, 1, 0, 0);

	/*
	**  [SYS10] (20,1) at LBN 201: SYSCOMMON is an alias of VMS$COMMON,
	**  SYSEXE (22,1)
	*/
	s_t$fm1(l_map, 201, 1);
	s_t$hdr(20, 1, 0, 0, 2, 0, l_map, 2);
	l_b = $BLK(201);
	l_off = 0;
	s_t$dirent(l_b, &l_off, 0, "SYSCOMMON.DIR", 21, 1, 0, 0);
	s_t$dirent(l_b, &l_off, 0, "SYSEXE.DIR", 22, 1, 0, 0);

	/*
	**  [VMS$COMMON] (21,1) at LBN 202: SYSEXE (23,1)
	*/
	s_t$fm1(l_map, 202, 1);
	s_t$hdr(21, 1, 0, 0, 2, 0, l_map, 2);
	l_b = $BLK(202);
	l_off = 0;
	s_t$dirent(l_b, &l_off, 0, "SYSEXE.DIR", 23, 1, 0, 0);

	/*
	**  [SYS10.SYSEXE] (22,1) at LBN 203: ALPHAVMSSYS.PAR (24,1), in the
	**  second block of the directory - the first ends at once
	*/
	s_t$fm1(l_map, 203, 2);
	s_t$hdr(22, 1, 0, 0, 3, 0, l_map, 2);
	nisca$put16($BLK(203), 0xFFFF);
	l_b = $BLK(204);
	l_off = 0;
	s_t$dirent(l_b, &l_off, 0, "ALPHAVMSSYS.PAR", 24, 1, 0, 0);

	/*
	**  [VMS$COMMON.SYSEXE] (23,1) at LBN 205: a name of UCS-2 that would
	**  match, a FID of a wrong sequence, APB.EXE (25,3) over (26,1)
	*/
	s_t$fm1(l_map, 205, 1);
	s_t$hdr(23, 1, 0, 0, 2, 0, l_map, 2);
	l_b = $BLK(205);
	l_off = 0;
	s_t$dirent(l_b, &l_off, 0x18, "APB.EXE", 30, 1, 0, 0);
	s_t$dirent(l_b, &l_off, 0, "APB.EXE", 25, 3, 26, 1);
	s_t$dirent(l_b, &l_off, 0, "BAD.EXE", 25, 9, 0, 0);
	s_t$dirent(l_b, &l_off, 0, "LOST.EXE", 60, 1, 0, 0);

	/*
	**  ALPHAVMSSYS.PAR (24,1): 2 blocks at LBN 300, 700 octets
	*/
	s_t$fm1(l_map, 300, 2);
	s_t$hdr(24, 1, 0, 0, 2, 188, l_map, 2);
	memset($BLK(300), 0x50, 1024);

	/*
	**  APB.EXE (25,3): 3 blocks at LBN 400 by format 1, and 2 at LBN 1000
	**  by format 3 in the extension header (27,1); 5 blocks, whole
	*/
	s_t$fm1(l_map, 400, 3);
	s_t$hdr(25, 3, 27, 1, 6, 0, l_map, 2);
	l_map[0] = 0xC000;				/* Format 3: count - 1 = 1, LBN 1000		*/
	l_map[1] = 1;
	l_map[2] = 1000;
	l_map[3] = 0;
	s_t$hdr(27, 1, 0, 0, 0, 0, l_map, 4);

	for ( unsigned i = 0; i < 3; i++ )
		memset($BLK(400 + i), 0xA0 + i, 512);

	for ( unsigned i = 0; i < 2; i++ )
		memset($BLK(1000 + i), 0xA3 + i, 512);

	/*
	**  The old version (26,1): 1 block of 0x11 at LBN 500
	*/
	s_t$fm1(l_map, 500, 1);
	s_t$hdr(26, 1, 0, 0, 2, 0, l_map, 2);
	memset($BLK(500), 0x11, 512);
}

#endif	/* __T_VOL$H__ */
