#ifndef	__MSCP$H__
#define	__MSCP$H__	1

/*
**++
**
**  FACILITY:	MSCPHUB - MSCP disk server for OpenVMS nodes, over NISCA
**
**  MODULE:	mscp.h
**
**  ABSTRACT:	The packets of MSCP, the disk protocol a VMS client
**		(DUDRIVER) speaks to the server over its SCS connection: the
**		command and end message headers, the fields of the commands of
**		v1, the opcodes, the status codes.
**
**  DESCRIPTION: Offsets are from the start of the MSCP packet - the data of
**		the SCS APPL_MSG; all little-endian.
**		An end message is built in place of the command it ends: the
**		opcode gets bit 7, STATUS is written, the rest stays as it came.
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

#ifndef	__MODULE__
#define	__MODULE__	"MSCP"
#endif

#ifndef	__IDENT__
#define	__IDENT__	"X01-10"
#endif

#ifndef	__REV__
#define	__REV__		"1.10.0"
#endif

/*
**  The header, command and end message
*/
#define	MSCP$K_CMD_REF		0x00		/* L							*/
#define	MSCP$K_UNIT		0x04		/* W; STCON end: B CNT_ALCS				*/
#define	MSCP$K_OPCODE		0x08		/* B; end: ENDCODE = OPCODE | 0x80			*/
#define	MSCP$K_FLAGS		0x09		/* B; must be 0 in a command				*/
#define	MSCP$K_MODIFIER		0x0A		/* W; end: STATUS					*/
#define	MSCP$K_STATUS		0x0A
#define	MSCP$K_MIN_SIZ		12

#define	MSCP$M_OP_END		0x80
#define	MSCP$M_UNIT_SLUN	0x4000		/* A unit number the server gave		*/
#define	MSCP$K_SLUN_RSVP	0x7FFF		/* GUS: find the unit by UNIT_ID		*/

/*
**  STCON
*/
#define	MSCP$K_VERSION		0x0C		/* W							*/
#define	MSCP$K_CNT_FLGS		0x0E		/* W							*/
#define	MSCP$K_HST_TMO		0x10		/* W; end: CNT_TMO					*/
#define	MSCP$K_CNT_TMO		0x10
#define	MSCP$K_CNT_SVR		0x12		/* B							*/
#define	MSCP$K_CNT_HVR		0x13		/* B							*/
#define	MSCP$K_CNT_ID		0x14		/* Q: SYSTEMID 6, model, class				*/
#define	MSCP$K_MAXBCNT		0x1C		/* L							*/

#define	MSCP$M_CF_MLTHS		0x0004		/* Multi-host: take the allocation class from us	*/
#define	MSCP$M_CF_LOAD		0x2000		/* LOAD_AVAIL in GUS				*/
#define	MSCP$K_CM_EMULA		4		/* Controller model: an emulator (the VMS server)	*/
#define	MSCP$K_CL_CNTRL		1		/* Class: an MSCP controller			*/

/*
**  GUS, ONLINE, STUNT
*/
#define	MSCP$K_MULT_UNT		0x0C		/* W							*/
#define	MSCP$K_UNT_FLGS		0x0E		/* W							*/
#define	MSCP$K_UNIT_ID		0x14		/* Q; GUS 0x7FFF: the key				*/
#define	MSCP$K_MEDIA_ID		0x1C		/* L							*/
#define	MSCP$K_DMAXBCNT		0x20		/* L, GUS						*/
#define	MSCP$K_SHDW_UNT		0x20		/* W, ONLINE						*/
#define	MSCP$K_SHDW_STS		0x22		/* W, ONLINE						*/
#define	MSCP$K_TRACK		0x24		/* W, GUS						*/
#define	MSCP$K_UNT_SIZE		0x24		/* L, ONLINE						*/
#define	MSCP$K_GROUP		0x26		/* W, GUS						*/
#define	MSCP$K_CYLINDER		0x28		/* W, GUS						*/
#define	MSCP$K_VOL_SER		0x28		/* L, ONLINE						*/
#define	MSCP$K_UNIT_SVR		0x2A		/* B							*/
#define	MSCP$K_UNIT_HVR		0x2B		/* B							*/
#define	MSCP$K_RCT_SIZE		0x2C		/* W							*/
#define	MSCP$K_RBNS		0x2E		/* B							*/
#define	MSCP$K_RCT_CPYS		0x2F		/* B							*/
#define	MSCP$K_LOAD_AVAIL	0x30		/* W							*/

#define	MSCP$M_UF_WRTPH		0x2000
#define	MSCP$M_UF_SSMEM		0x4000
#define	MSCP$M_UF_SSMST		0x0200
#define	MSCP$M_UF_HOST		0x1847		/* What a host may ask for: CMPRD CMPWR 576 SCCHH WBKNV WRTPS	*/

#define	MSCP$M_MD_NXUNT		0x0001		/* GUS: the next unit				*/

/*
**  GTCMD, GTUNM
*/
#define	MSCP$K_OUT_REF		0x0C		/* L							*/
#define	MSCP$K_CMD_STS		0x10		/* L							*/
#define	MSCP$K_DDR_NAMELEN	0x0C		/* L							*/
#define	MSCP$K_DDR_NAME		0x10

/*
**  READ, WRITE, ERASE, COMPARE HOST DATA: 32 octets; the
**  end of WRITE and ERASE is 36, HRN at +20 as the command had it
*/
#define	MSCP$K_BYTE_CNT		0x0C		/* L; end: what has been moved			*/
#define	MSCP$K_BUFFER		0x10		/* 12: the buffer descriptor of the client	*/
#define	MSCP$K_LBN		0x1C		/* L; end: FRST_BAD, left as LBN		*/
#define	MSCP$K_HRN		0x20		/* L							*/

#define	MSCP$M_MD_COMP		0x4000		/* READ, WRITE: compare after the operation	*/
#define	MSCP$M_MD_ERROR		0x1000		/* WRITE, ERASE: forced error - v2		*/

#define	MSCP$K_CMD_STS_INIT	0xFFFFFFFEU	/* CMD_STS of a command taken: down by 1 a step	*/

/*
**  Opcodes of a disk
*/
enum	{
	MSCP$K_OP_ABORT		= 0x01,
	MSCP$K_OP_GTCMD		= 0x02,
	MSCP$K_OP_GTUNT		= 0x03,
	MSCP$K_OP_STCON		= 0x04,
	MSCP$K_OP_GTUNM		= 0x07,
	MSCP$K_OP_AVAIL		= 0x08,
	MSCP$K_OP_ONLIN		= 0x09,
	MSCP$K_OP_STUNT		= 0x0A,
	MSCP$K_OP_DTACP		= 0x0B,
	MSCP$K_OP_ACCES		= 0x10,
	MSCP$K_OP_CMPCD		= 0x11,
	MSCP$K_OP_ERASE		= 0x12,
	MSCP$K_OP_FLUSH		= 0x13,
	MSCP$K_OP_REPLC		= 0x14,
	MSCP$K_OP_WRHIM		= 0x19,
	MSCP$K_OP_COMP		= 0x20,
	MSCP$K_OP_READ		= 0x21,
	MSCP$K_OP_WRITE		= 0x22,
	MSCP$K_OP_TERCO		= 0x30,

	MSCP$K_OP_MAX				/* End-Of-List				*/
	};

/*
**  Status: code in bits 0 - 4, subcode above (STATUS = code + subcode * 32)
*/
enum	{
	MSCP$K_ST_SUCC		= 0x00,
	MSCP$K_ST_ICMD		= 0x01,
	MSCP$K_ST_ABRTD		= 0x02,
	MSCP$K_ST_OFFLN		= 0x03,
	MSCP$K_ST_AVLBL		= 0x04,
	MSCP$K_ST_WRTPR		= 0x06,
	MSCP$K_ST_COMP		= 0x07,
	MSCP$K_ST_DATA		= 0x08,
	MSCP$K_ST_HSTBF		= 0x09,
	MSCP$K_ST_CNTLR		= 0x0A,
	MSCP$K_ST_DRIVE		= 0x0B
	};

#define	MSCP$K_ST_MASK		0x001F
#define	MSCP$K_ST_ALONL		0x0100		/* SUCC: already ONLINE to this host		*/
#define	$MSCP_ICMD(a_off)	(MSCP$K_ST_ICMD | ((a_off) << 8))	/* ICMD: the offset of the bad field	*/
#define	MSCP$K_MXCMDLEN		36		/* The length of an ICMD end message		*/

#endif	/* __MSCP$H__ */
