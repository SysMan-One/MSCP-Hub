#define	__MODULE__	"MHODS"
#define	__IDENT__	"X01-22"
#define	__REV__		"1.22.0"

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mh_ods.c
**
**  ABSTRACT:	A reader of the Files-11 volume of a unit, ODS-2 and ODS-5, read
**		only: a file named by its directory path is found and its
**		blocks are read.  The MOP server takes the primary bootstrap
**		and the parameter file of a satellite with it from the very
**		system disk the satellite boots from.
**
**  DESCRIPTION: The volume is read through the descriptor of the unit, the one
**		the backend does its I/O with: a block device or a file may be
**		O_DIRECT, so every read is of whole pages, from a page
**		boundary, into a buffer of the reader, and what was asked for
**		is copied out of it.
**
**		The way to a file:
**
**		    home block   - LBN 1, or the first block of the volume that
**		                   names itself and is DECFILE11B;
**		    INDEXF.SYS   - its header is the first one after the index
**		                   file bitmap; its map gives the LBN of the
**		                   header of file N: VBN IBMAPVBN + IBMAPSIZE + N - 1;
**		    000000.DIR   - the MFD, file (4,4,0);
**		    a directory  - records of a name and its versions, the
**		                   highest version first; a block ends at a
**		                   record size of FFFF.
**
**		A file header is taken only if its checksum is right, it is of
**		structure level 2 or 5 and it is the file asked for, sequence
**		number and all.  The map is the one of the header and of its
**		extension headers.  Names are compared without regard to case;
**		a name of 16-bit characters (ODS-5 UCS-2) matches nothing.
**
**		The volume is not locked against the writes of the nodes: a
**		file read while a node changes it may come out torn.  The files
**		the MOP server reads are the ones a node does not write while
**		a satellite boots.
**
**  AUTHOR:	StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
**
**  CREATION DATE:  25-OCT-2026
**
**  MODIFICATION HISTORY:
**
**		25-OCT-2026	RRL	X01-22 : Initial version: the files of a satellite read
**						from its system disk.
**
**--
*/

#include	<errno.h>
#include	<stdio.h>
#include	<string.h>
#include	<strings.h>
#include	<stdlib.h>
#include	<unistd.h>

#include	"mh_def.h"

/*
**  The home block, HM2
*/
#define	HM2$L_HOMELBN		0x000
#define	HM2$W_STRUCLEV		0x00C
#define	HM2$W_CLUSTER		0x00E
#define	HM2$W_IBMAPVBN		0x016
#define	HM2$L_IBMAPLBN		0x018
#define	HM2$L_MAXFILES		0x01C
#define	HM2$W_IBMAPSIZE		0x020
#define	HM2$W_CHECKSUM1		0x03A
#define	HM2$T_VOLNAME		0x1D8
#define	HM2$T_FORMAT		0x1F0
#define	HM2$W_CHECKSUM2		0x1FE

/*
**  The file header, FH2, and its record attributes, FAT
*/
#define	FH2$B_IDOFFSET		0x000
#define	FH2$B_MPOFFSET		0x001
#define	FH2$B_ACOFFSET		0x002
#define	FH2$W_STRUCLEV		0x006
#define	FH2$W_FID_NUM		0x008
#define	FH2$W_FID_SEQ		0x00A
#define	FH2$B_FID_NMX		0x00D
#define	FH2$W_EX_FIDNUM		0x00E
#define	FH2$W_EX_FIDSEQ		0x010
#define	FH2$B_EX_FIDNMX		0x013
#define	FH2$W_RECATTR		0x014
#define	FH2$B_MAP_INUSE		0x03A
#define	FH2$W_CHECKSUM		0x1FE

#define	FAT$W_HIBLKH		(FH2$W_RECATTR + 0x04)	/* The VBNs are two words, the high one first	*/
#define	FAT$W_EFBLKH		(FH2$W_RECATTR + 0x08)
#define	FAT$W_FFBYTE		(FH2$W_RECATTR + 0x0C)

/*
**  The retrieval pointers: the format in the two high bits of the first word
*/
enum	{
	FM2$C_PLACEMENT	= 0,
	FM2$C_FORMAT1,
	FM2$C_FORMAT2,
	FM2$C_FORMAT3
	};

/*
**  A directory record, DIR, and a version of it, DIR1
*/
#define	DIR$W_SIZE		0x000
#define	DIR$B_FLAGS		0x004
#define	DIR$B_NAMECOUNT		0x005
#define	DIR$T_NAME		0x006
#define	DIR$K_VERSION		8		/* A version: the number, the FID		*/
#define	DIR$M_TYPE		0x07		/* The type of the entry: 0 - a FID		*/
#define	DIR$V_NAMETYPE		3		/* ... of its name: 3 - UCS-2			*/
#define	DIR$C_UCS2		3

#define	ODS$K_BLK		512
#define	ODS$K_PAGE		4096		/* A read starts and ends on it: O_DIRECT	*/
#define	ODS$K_HOMESCAN		2048		/* Blocks searched for a home block		*/
#define	ODS$K_MAXEXT		16		/* Extension headers of a file, at most		*/
#define	ODS$K_FID_INDEXF	1
#define	ODS$K_FID_MFD		4

static	int	s_mh$ods_hdr	(MH$ODS *a_ods, uint32_t a_num, uint16_t a_seq, uint8_t *a_hdr);
static	int	s_mh$ods_map	(MH$ODS *a_ods, uint32_t a_num, uint16_t a_seq, MH$ODSFILE *a_file);



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read blocks of the volume: the pages around them into the buffer of
**	the reader, a piece at a time, and the blocks out of it.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_lbn		The first block
**	a_count		Blocks
**	a_buf		Output area, A_COUNT * 512 octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - beyond the end of the volume, or PREAD(2)
**	failed (ERRNO in the text of the reader).
**--
*/
static	int	s_mh$ods_rdblk	(
		MH$ODS *	a_ods,
		uint64_t	a_lbn,
		uint32_t	a_count,
		uint8_t *	a_buf
			)
{
uint64_t	l_off, l_end, l_pg, l_pgend, l_upto;
size_t		l_len;
ssize_t		l_got;

	l_off = a_lbn * ODS$K_BLK;
	l_end = l_off + (uint64_t) a_count * ODS$K_BLK;

	if ( l_end > a_ods->size )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "LBN %llu + %u is beyond the end of the volume",
			(unsigned long long) a_lbn, a_count);
		return	STS$K_ERROR;
		}

	while ( l_off < l_end )
		{
		l_pg	= l_off & ~((uint64_t) ODS$K_PAGE - 1);
		l_pgend	= (l_end + ODS$K_PAGE - 1) & ~((uint64_t) ODS$K_PAGE - 1);

		if ( l_pgend - l_pg > MH$K_ODS_BUFSZ )
			l_pgend = l_pg + MH$K_ODS_BUFSZ;

		/*
		**  Up to where this piece ends: the blocks asked for, or the
		**  buffer.  The offsets are 64-bit: $MIN of STARLET takes int
		*/
		l_upto = (l_pgend < l_end) ? l_pgend : l_end;

		if ( 0 > (l_got = pread(a_ods->fd, a_ods->buf, (size_t) (l_pgend - l_pg), (off_t) l_pg)) )
			{
			snprintf(a_ods->why, sizeof(a_ods->why), "pread() at %llu, errno=%d (%s)",
				(unsigned long long) l_pg, errno, strerror(errno));
			return	STS$K_ERROR;
			}

		/*
		**  The last page of a file not of whole pages comes short
		*/
		if ( (uint64_t) l_got < l_upto - l_pg )
			{
			snprintf(a_ods->why, sizeof(a_ods->why), "pread() at %llu got %zd octets", (unsigned long long) l_pg, l_got);
			return	STS$K_ERROR;
			}

		l_len = (size_t) (l_upto - l_off);
		memcpy(a_buf, a_ods->buf + (l_off - l_pg), l_len);
		a_buf += l_len;
		l_off += l_len;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The checksum of a block of Files-11: the sum of its words before the
**	one that holds it.
**
**  FORMAL PARAMETERS:
**
**	a_blk		The block
**	a_off		Offset of the checksum word
**
**  RETURN VALUE:
**	1 - the checksum is right; 0 - not.
**--
*/
static	int	s_mh$ods_cksum	(
	const	uint8_t *	a_blk,
		unsigned	a_off
			)
{
uint16_t	l_sum = 0;

	for ( unsigned i = 0; i < a_off; i += 2 )
		l_sum = (uint16_t) (l_sum + nisca$get16(a_blk + i));

	return	l_sum == nisca$get16(a_blk + a_off);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Is this block a home block of ODS-2 or ODS-5: it names its own LBN,
**	both checksums are right, the format is DECFILE11B.
**
**  FORMAL PARAMETERS:
**
**	a_blk		The block
**	a_lbn		Where it was read
**
**  RETURN VALUE:
**	1 - it is; 0 - not.
**--
*/
static	int	s_mh$ods_ishome	(
	const	uint8_t *	a_blk,
		uint64_t	a_lbn
			)
{
uint8_t	l_lev = a_blk[HM2$W_STRUCLEV + 1];

	return	(nisca$get32(a_blk + HM2$L_HOMELBN) == a_lbn)
		&& ((l_lev == 2) || (l_lev == 5))
		&& !memcmp(a_blk + HM2$T_FORMAT, "DECFILE11B  ", 12)
		&& s_mh$ods_cksum(a_blk, HM2$W_CHECKSUM1)
		&& s_mh$ods_cksum(a_blk, HM2$W_CHECKSUM2);
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Open the volume of a unit: the home block, the map of the index file.
**
**  FORMAL PARAMETERS:
**
**	a_ods		Output: the volume
**	a_fd		The descriptor of the unit; it is not closed by the reader
**	a_size		The size of the unit, octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - no Files-11 volume, the reason in WHY.
**--
*/
int	mh$ods_open	(
		MH$ODS *	a_ods,
		int		a_fd,
		uint64_t	a_size
			)
{
uint8_t		l_blk [ODS$K_BLK];
uint64_t	l_lbn;
unsigned	l_scan;
int		l_status;

	memset(a_ods, 0, sizeof(*a_ods));
	a_ods->fd	= a_fd;
	a_ods->size	= a_size;

	if ( !(a_ods->buf = aligned_alloc(ODS$K_PAGE, MH$K_ODS_BUFSZ)) )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "no memory for the buffer");
		return	STS$K_ERROR;
		}

	/*
	**  LBN 1 almost always; else the first block that is one
	*/
	l_scan = (a_size / ODS$K_BLK < ODS$K_HOMESCAN) ? (unsigned) (a_size / ODS$K_BLK) : ODS$K_HOMESCAN;

	for ( l_lbn = 1; l_lbn < l_scan; l_lbn++ )
		{
		if ( !(1 & (l_status = s_mh$ods_rdblk(a_ods, l_lbn, 1, l_blk))) )
			return	l_status;

		if ( s_mh$ods_ishome(l_blk, l_lbn) )
			break;
		}

	if ( l_lbn >= l_scan )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "no home block of ODS-2 or ODS-5 in the first %u blocks", l_scan);
		return	STS$K_ERROR;
		}

	a_ods->level	= l_blk[HM2$W_STRUCLEV + 1];
	a_ods->ibmapvbn	= nisca$get16(l_blk + HM2$W_IBMAPVBN);
	a_ods->ibmaplbn	= nisca$get32(l_blk + HM2$L_IBMAPLBN);
	a_ods->ibmapsize = nisca$get16(l_blk + HM2$W_IBMAPSIZE);
	a_ods->maxfiles	= nisca$get32(l_blk + HM2$L_MAXFILES);
	memcpy(a_ods->volname, l_blk + HM2$T_VOLNAME, 12);

	for ( int i = 11; (i >= 0) && (a_ods->volname[i] == ' '); i-- )
		a_ods->volname[i] = '\0';

	/*
	**  The header of INDEXF.SYS is the first one after the bitmap; its
	**  map is what every other header is found with
	*/
	a_ods->indexf.nmap	= 1;
	a_ods->indexf.map[0].lbn = a_ods->ibmaplbn + a_ods->ibmapsize;
	a_ods->indexf.map[0].count = 1;
	a_ods->indexf.map[0].vbn = a_ods->ibmapvbn + a_ods->ibmapsize;

	/*
	**  So that VBN IBMAPVBN + IBMAPSIZE maps to that LBN: a map of one
	**  extent that starts at the VBN of the first header
	*/
	if ( !(1 & (l_status = s_mh$ods_map(a_ods, ODS$K_FID_INDEXF, ODS$K_FID_INDEXF, &a_ods->indexf))) )
		return	l_status;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Close the volume: the buffer is freed, the descriptor of the unit is
**	left alone.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**
**  RETURN VALUE:
**	STS$K_SUCCESS.
**--
*/
int	mh$ods_close	(
		MH$ODS *	a_ods
			)
{
	free(a_ods->buf);
	a_ods->buf = NULL;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	The LBN of a VBN of a file, and how many blocks follow it in the same
**	extent.
**
**  FORMAL PARAMETERS:
**
**	a_file		The file: its map
**	a_vbn		The VBN, from 1
**	a_lbn		Output: its LBN
**	a_run		Output: blocks from it to the end of the extent
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the VBN is not mapped.
**--
*/
static	int	s_mh$ods_vbn	(
	const	MH$ODSFILE *	a_file,
		uint32_t	a_vbn,
		uint64_t *	a_lbn,
		uint32_t *	a_run
			)
{
	for ( unsigned i = 0; i < a_file->nmap; i++ )
		{
		const	MH$ODSEXT *l_ext = &a_file->map[i];

		if ( (a_vbn >= l_ext->vbn) && (a_vbn - l_ext->vbn < l_ext->count) )
			{
			*a_lbn = l_ext->lbn + (a_vbn - l_ext->vbn);
			*a_run = l_ext->count - (a_vbn - l_ext->vbn);

			return	STS$K_SUCCESS;
			}
		}

	return	STS$K_ERROR;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read the header of a file: the block of the index file it is in, and
**	the checks - checksum, structure level, the FID.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_num		File number
**	a_seq		Its sequence number
**	a_hdr		Output: the header, 512 octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the reason in WHY.
**--
*/
static	int	s_mh$ods_hdr	(
		MH$ODS *	a_ods,
		uint32_t	a_num,
		uint16_t	a_seq,
		uint8_t *	a_hdr
			)
{
uint64_t	l_lbn;
uint32_t	l_run, l_num;
uint8_t		l_lev;
int		l_status;

	if ( !a_num || (a_ods->maxfiles && (a_num > a_ods->maxfiles)) )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "file number %u is out of the index file", a_num);
		return	STS$K_ERROR;
		}

	if ( !(1 & s_mh$ods_vbn(&a_ods->indexf, a_ods->ibmapvbn + a_ods->ibmapsize + a_num - 1, &l_lbn, &l_run)) )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "the header of file %u is not mapped in INDEXF.SYS", a_num);
		return	STS$K_ERROR;
		}

	if ( !(1 & (l_status = s_mh$ods_rdblk(a_ods, l_lbn, 1, a_hdr))) )
		return	l_status;

	l_num	= nisca$get16(a_hdr + FH2$W_FID_NUM) | ((uint32_t) a_hdr[FH2$B_FID_NMX] << 16);
	l_lev	= a_hdr[FH2$W_STRUCLEV + 1];

	if ( !s_mh$ods_cksum(a_hdr, FH2$W_CHECKSUM) || ((l_lev != 2) && (l_lev != 5)) || (l_num != a_num)
		|| (nisca$get16(a_hdr + FH2$W_FID_SEQ) != a_seq) )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "the header at LBN %llu is not one of file (%u,%u)",
			(unsigned long long) l_lbn, a_num, a_seq);
		return	STS$K_ERROR;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Build the map of a file: the retrieval pointers of its header and of
**	its extension headers, the VBN of every extent; the end of file from
**	the record attributes.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_num		File number
**	a_seq		Its sequence number
**	a_file		Output: the map, EFBLK, FFBYTE, the size in octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - the reason in WHY.
**--
*/
static	int	s_mh$ods_map	(
		MH$ODS *	a_ods,
		uint32_t	a_num,
		uint16_t	a_seq,
		MH$ODSFILE *	a_file
			)
{
uint8_t		l_hdr [ODS$K_BLK];
MH$ODSFILE	l_map = {0};
uint32_t	l_vbn = 1, l_count, l_num = a_num;
uint16_t	l_seq = a_seq;
uint64_t	l_lbn;
unsigned	l_mp, l_end, l_w0;
int		l_status;

	for ( unsigned l_seg = 0; l_num; l_seg++ )
		{
		if ( l_seg > ODS$K_MAXEXT )
			{
			snprintf(a_ods->why, sizeof(a_ods->why), "file (%u,%u) has more than %u extension headers", a_num, a_seq, ODS$K_MAXEXT);
			return	STS$K_ERROR;
			}

		if ( !(1 & (l_status = s_mh$ods_hdr(a_ods, l_num, l_seq, l_hdr))) )
			return	l_status;

		/*
		**  The end of file is in the first header
		*/
		if ( !l_seg )
			{
			l_map.efblk	= ((uint32_t) nisca$get16(l_hdr + FAT$W_EFBLKH) << 16) | nisca$get16(l_hdr + FAT$W_EFBLKH + 2);
			l_map.ffbyte	= nisca$get16(l_hdr + FAT$W_FFBYTE);
			l_map.hiblk	= ((uint32_t) nisca$get16(l_hdr + FAT$W_HIBLKH) << 16) | nisca$get16(l_hdr + FAT$W_HIBLKH + 2);
			}

		l_mp	= 2u * l_hdr[FH2$B_MPOFFSET];
		l_end	= l_mp + 2u * l_hdr[FH2$B_MAP_INUSE];

		if ( (l_mp < 0x3C) || (l_end > FH2$W_CHECKSUM) )
			{
			snprintf(a_ods->why, sizeof(a_ods->why), "file (%u,%u): the map area is out of the header", a_num, a_seq);
			return	STS$K_ERROR;
			}

		while ( l_mp + 2 <= l_end )
			{
			l_w0 = nisca$get16(l_hdr + l_mp);

			switch ( l_w0 >> 14 )
				{
				case	FM2$C_PLACEMENT:
					l_mp += 2;
					continue;

				case	FM2$C_FORMAT1:
					l_count	= (l_w0 & 0xFF) + 1;
					l_lbn	= ((uint64_t) ((l_w0 >> 8) & 0x3F) << 16) | nisca$get16(l_hdr + l_mp + 2);
					l_mp	+= 4;
					break;

				case	FM2$C_FORMAT2:
					l_count	= (l_w0 & 0x3FFF) + 1;
					l_lbn	= nisca$get32(l_hdr + l_mp + 2);
					l_mp	+= 6;
					break;

				default:
					l_count	= ((((uint32_t) l_w0 & 0x3FFF) << 16) | nisca$get16(l_hdr + l_mp + 2)) + 1;
					l_lbn	= nisca$get32(l_hdr + l_mp + 4);
					l_mp	+= 8;
					break;
				}

			if ( l_mp > l_end )
				{
				snprintf(a_ods->why, sizeof(a_ods->why), "file (%u,%u): a retrieval pointer runs out of the map", a_num, a_seq);
				return	STS$K_ERROR;
				}

			if ( l_map.nmap >= MH$K_ODS_MAXMAP )
				{
				snprintf(a_ods->why, sizeof(a_ods->why), "file (%u,%u) has more than %u extents", a_num, a_seq, MH$K_ODS_MAXMAP);
				return	STS$K_ERROR;
				}

			l_map.map[l_map.nmap].vbn	= l_vbn;
			l_map.map[l_map.nmap].lbn	= l_lbn;
			l_map.map[l_map.nmap].count	= l_count;
			l_map.nmap++;
			l_vbn += l_count;
			}

		l_num	= nisca$get16(l_hdr + FH2$W_EX_FIDNUM) | ((uint32_t) l_hdr[FH2$B_EX_FIDNMX] << 16);
		l_seq	= nisca$get16(l_hdr + FH2$W_EX_FIDSEQ);
		}

	l_map.num	= a_num;
	l_map.seq	= a_seq;
	l_map.blocks	= l_vbn - 1;

	/*
	**  EFBLK is the block the end of file is in, FFBYTE the first free
	**  octet of it: a file of whole blocks has EFBLK one past its last
	*/
	l_map.size	= l_map.efblk ? (uint64_t) (l_map.efblk - 1) * ODS$K_BLK + l_map.ffbyte : 0;

	if ( l_map.size > (uint64_t) l_map.blocks * ODS$K_BLK )
		l_map.size = (uint64_t) l_map.blocks * ODS$K_BLK;

	*a_file = l_map;

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Read blocks of a file.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_file		The file
**	a_vbn		The first block, from 1
**	a_count		Blocks
**	a_buf		Output area, A_COUNT * 512 octets
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_ERROR - a block is not mapped, or the read
**	failed; the reason in WHY.
**--
*/
int	mh$ods_read	(
		MH$ODS *	a_ods,
	const	MH$ODSFILE *	a_file,
		uint32_t	a_vbn,
		uint32_t	a_count,
		uint8_t *	a_buf
			)
{
uint64_t	l_lbn;
uint32_t	l_run;
int		l_status;

	while ( a_count )
		{
		if ( !(1 & s_mh$ods_vbn(a_file, a_vbn, &l_lbn, &l_run)) )
			{
			snprintf(a_ods->why, sizeof(a_ods->why), "VBN %u of file (%u,%u) is not mapped", a_vbn, a_file->num, a_file->seq);
			return	STS$K_ERROR;
			}

		l_run = (l_run < a_count) ? l_run : a_count;

		if ( !(1 & (l_status = s_mh$ods_rdblk(a_ods, l_lbn, l_run, a_buf))) )
			return	l_status;

		a_buf	+= (size_t) l_run * ODS$K_BLK;
		a_vbn	+= l_run;
		a_count	-= l_run;
		}

	return	STS$K_SUCCESS;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Find a name in a directory: the FID of its highest version.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_dir		The directory file
**	a_name		NAME.TYPE, no version
**	a_len		Its length
**	a_num		Output: the file number
**	a_seq		Output: its sequence number
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - no such name; STS$K_ERROR - the
**	directory cannot be read; the reason in WHY.
**--
*/
static	int	s_mh$ods_find	(
		MH$ODS *	a_ods,
	const	MH$ODSFILE *	a_dir,
	const	char *		a_name,
		unsigned	a_len,
		uint32_t *	a_num,
		uint16_t *	a_seq
			)
{
uint8_t		l_blk [ODS$K_BLK];
unsigned	l_off, l_size, l_nlen, l_val;
uint32_t	l_blocks;
int		l_status;

	/*
	**  The blocks in use: up to the end of file
	*/
	l_blocks = a_dir->efblk ? a_dir->efblk - (a_dir->ffbyte ? 0 : 1) : a_dir->blocks;
	l_blocks = (l_blocks < a_dir->blocks) ? l_blocks : a_dir->blocks;

	for ( uint32_t l_vbn = 1; l_vbn <= l_blocks; l_vbn++ )
		{
		if ( !(1 & (l_status = mh$ods_read(a_ods, a_dir, l_vbn, 1, l_blk))) )
			return	l_status;

		for ( l_off = 0; l_off + DIR$T_NAME <= ODS$K_BLK; l_off += l_size + 2 )
			{
			if ( 0xFFFF == (l_size = nisca$get16(l_blk + l_off + DIR$W_SIZE)) )
				break;

			l_nlen	= l_blk[l_off + DIR$B_NAMECOUNT];
			l_val	= l_off + DIR$T_NAME + ((l_nlen + 1) & ~1u);

			if ( (l_size & 1) || (l_off + 2 + l_size > ODS$K_BLK) || (l_val + DIR$K_VERSION > l_off + 2 + l_size) )
				{
				snprintf(a_ods->why, sizeof(a_ods->why), "a bad record at %u of VBN %u of directory (%u,%u)",
					l_off, l_vbn, a_dir->num, a_dir->seq);
				return	STS$K_ERROR;
				}

			if ( (l_blk[l_off + DIR$B_FLAGS] & DIR$M_TYPE)
				|| (((l_blk[l_off + DIR$B_FLAGS] >> DIR$V_NAMETYPE) & 7) == DIR$C_UCS2)
				|| (l_nlen != a_len) || strncasecmp((const char *) l_blk + l_off + DIR$T_NAME, a_name, a_len) )
				continue;

			/*
			**  The versions follow the name, the highest first
			*/
			*a_num = nisca$get16(l_blk + l_val + 2) | ((uint32_t) l_blk[l_val + 7] << 16);
			*a_seq = nisca$get16(l_blk + l_val + 4);

			return	STS$K_SUCCESS;
			}
		}

	return	STS$K_WARN;
}



/*
**++
**  FUNCTIONAL DESCRIPTION:
**
**	Find a file by its path from the MFD: [DIR.DIR...]NAME.TYPE, the
**	brackets [] or <>, no device, no version.  A directory of the path
**	may be an alias, SYSCOMMON of a root: it is followed as any other.
**
**  FORMAL PARAMETERS:
**
**	a_ods		The volume
**	a_path		The path, ASCIZ
**	a_file		Output: the file - its map and size
**
**  RETURN VALUE:
**	STS$K_SUCCESS; STS$K_WARN - no such file; STS$K_ERROR - the volume
**	cannot be read; the reason in WHY.
**--
*/
int	mh$ods_lookup	(
		MH$ODS *	a_ods,
	const	char *		a_path,
		MH$ODSFILE *	a_file
			)
{
MH$ODSFILE	l_dir;
const	char	*l_p = a_path, *l_end;
char		l_name [MH$K_ODS_NAMESZ];
uint32_t	l_num = ODS$K_FID_MFD;
uint16_t	l_seq = ODS$K_FID_MFD;
unsigned	l_len;
int		l_status, l_indir = 1, l_isfile = 0;

	if ( (*l_p != '[') && (*l_p != '<') )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "%s: no directory", a_path);
		return	STS$K_ERROR;
		}

	l_p++;

	while ( *l_p )
		{
		/*
		**  Inside the brackets a name ends at . or at the bracket and is
		**  a directory: .DIR is put to it; after them the rest is the file
		*/
		if ( l_indir )
			{
			for ( l_end = l_p; *l_end && (*l_end != '.') && (*l_end != ']') && (*l_end != '>'); l_end++ )
				;

			l_len = (unsigned) (l_end - l_p);

			if ( !*l_end || !l_len || (l_len + 4 >= sizeof(l_name)) )
				{
				snprintf(a_ods->why, sizeof(a_ods->why), "%s: a bad directory name", a_path);
				return	STS$K_ERROR;
				}

			memcpy(l_name, l_p, l_len);
			memcpy(l_name + l_len, ".DIR", 4);
			l_len	+= 4;
			l_indir	= (*l_end == '.');
			l_p	= l_end + 1;
			}
		else	{
			l_len = (unsigned) strlen(l_p);

			if ( !strchr(l_p, '.') || (l_len >= sizeof(l_name)) )
				{
				snprintf(a_ods->why, sizeof(a_ods->why), "%s: a bad file name", a_path);
				return	STS$K_ERROR;
				}

			memcpy(l_name, l_p, l_len);
			l_p += l_len;
			l_isfile = 1;
			}

		if ( !(1 & (l_status = s_mh$ods_map(a_ods, l_num, l_seq, &l_dir))) )
			return	l_status;

		if ( !(1 & (l_status = s_mh$ods_find(a_ods, &l_dir, l_name, l_len, &l_num, &l_seq))) )
			{
			if ( l_status == STS$K_WARN )
				snprintf(a_ods->why, sizeof(a_ods->why), "%s: no %.*s", a_path, (int) l_len, l_name);

			return	l_status;
			}
		}

	if ( !l_isfile )
		{
		snprintf(a_ods->why, sizeof(a_ods->why), "%s: no file name", a_path);
		return	STS$K_ERROR;
		}

	return	s_mh$ods_map(a_ods, l_num, l_seq, a_file);
}
