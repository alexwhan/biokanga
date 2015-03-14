#pragma once
#include "./commdefs.h"
#include "./bgzf.h"

const char cszProgVer[] = "1.0.0";		 // default versioning if not supplied by application in initial Create()

const int cMaxBAMSeqLen = cMaxReadLen+1;	// max length sequence which can be processed
const int cMaxBAMAuxValLen = 100;			// max length BAM aux value length
const int cMaxBAMCigarOps = 20;				// max number of BAM MAGIC ops handled
const int cMaxBAMAuxTags = 20;				// max number of BAM aux tags handled
const int cMaxBAMLineLen = (cMaxDescrIDLen + cMaxGeneNameLen + 2000 + (cMaxBAMSeqLen * 2));	// max SAM line length expected with full length query and quality sequences plus a few tags		

const UINT32 cMaxSAIRefSeqLen = 0x20000000; // SAI indexes have an inherient limit of 512Mbp for chunk to bin associations - UGH!!!!
                                         // so if alignment end loci are >= 512Mbp need to alert user and quit processing  

const int cMaxRptSAMSeqsThres = 10000;	// default number of chroms to report if SAM output
const int cDfltComprLev = 6;			// default compression level if BAM output

const size_t cAllocBAMSize = (size_t)cMaxRptSAMSeqsThres * (3 * cMaxDatasetSpeciesChrom);	// initial allocation for  to hold BAM header which includes the sequence names + sequence lengths
const size_t cAllocSAMSize = (size_t)cMaxRptSAMSeqsThres * (10 * cMaxDatasetSpeciesChrom);	// initial allocation for holding SAM header 

const size_t cAllocBAISize = (size_t)cMaxRptSAMSeqsThres * cMaxGeneNameLen * 10;	// initial allocation for  to hold SAI, will be realloc'd if required
const size_t cAllocRefSeqSize = (size_t)cMaxRptSAMSeqsThres * cMaxGeneNameLen * 5;	// initial allocation for  to hold reference sequences, will be realloc'd if required
const int cAllocBAIChunks = 10000;		// initial allocation for this many BAI chunks, will be realloc'd if more chunks required
const int cNumSAIBins = 37450;          // total number of SAI bins (bins are referenced as bins 0 to 37449)
										// Bin 0 spans a 512Mbp region,
										// bins 1-8 span 64Mbp each
										// bins 9-72 span 8Mbp each
										// bins 73-584 span 1Mbp each
										// bins 585-4680 span 128Kbp each
										// bins span 4681-37449 span 16Kbp each

const int cMaxLocateRefSeqHist = 10;		// search history for reference sequence identifiers is maintained to this depth

typedef enum TAG_eSAMFileType {
	eSFTSAM = 0,		// SAM raw text file
	eSFTSAMgz,			// SAM which was compressed with gzip
	eSFTBAM,			// BAM bgzf compressed file
	eSFTBAM_BAI			// BAM bgzf plus associated BAI file
} eSAMFileType;


#pragma pack(1)

typedef struct TAG_sBAMauxData {
		UINT8 tag[2];			// Two-character tag
		UINT8 val_type;			// Value type: if SAM then one of AifZHB, if BAM then can be one of AcCsSiIfZHB
		int NumVals;			// number of values in value[]
		UINT8 array_type;       // type of values in value[] - with SAM or BAM then one of cCsSiIf
 		UINT8 value[cMaxBAMAuxValLen*sizeof(int32)];	// allow tag values to be at most this long 
} tsBAMauxData;

typedef struct TAG_tBAMalign {
		UINT32 block_size;		// length of this alignment record incl any auxiliary data	
		INT32  refID;			// Reference sequence ID,  -1 <= refID < n_ref; -1 for a read without a mapping position
		char szRefSeqName[cMaxDescrIDLen+1]; // reference sequence name; truncated if longer than cMaxDescrIDLen
		INT32  pos;				// 0-based leftmost coordinate (= POS - 1) 
		INT32  end;				// 0-based rightmost coordinate
		UINT32 bin_mq_nl;		// bin<<16|MAPQ<<8|l_read_name ; bin is computed by the reg2bin(); l_read_name is the length of read name below (= length(QNAME) + 1).
		UINT32 flag_nc;			// FLAG<<16|n_cigar_op; n_cigar_op is the number of operations in CIGAR
		INT32 l_seq;			// Length of SEQ
		INT32 next_refID;		// Ref-ID of the next segment (-1 <= mate_refID < n_ref)
		char szMateRefSeqName[cMaxDescrIDLen+1]; // next segment sequence name; truncated if longer than cMaxDescrIDLen
		INT32 next_pos;		    // 0-based leftmost pos of the next segment (= PNEXT - 1)
		INT32 tlen;				// Template length (= TLEN)
		INT32 NumReadNameBytes;  // number of bytes required for read_name (includes terminating '\0';
		char read_name[cMaxDescrIDLen+1];	// char[l_read name] | NULL terminated (QNAME plus a tailing `\0')
		INT32 NumCigarBytes;  // number of bytes required for cigar
		UINT32 cigar[cMaxBAMCigarOps];      // uint32[n_cigar_op] | CIGAR: op_len<<4|op. `MIDNSHP=X' --> `012345678' 
		INT32 NumSeqBytes;  // number of bytes required for seq
		UINT8 seq[(cMaxBAMSeqLen+1)/2];   // UINT8 t[(l_seq+1)/2] | 4-bit encoded read: `=ACMGRSVTWYHKDBN'! [0; 15]; other characters mapped to `N'; high nybble first (1st base in the highest 4-bit of the 1st byte)
		UINT8 qual[cMaxBAMSeqLen];		  // char[l_seq]  | Phred base quality (a sequence of 0xFF if absent)
		int NumAux;				// actual number of auxiliary data items (auxData[]) in this alignment record
		tsBAMauxData auxData[cMaxBAMAuxTags]; // to hold any auxiliary data
} tsBAMalign;

// reference sequence name dictionary
typedef struct {
	int SeqID;		// unique identifier for this reference sequence (1..n)
	int SeqLen;		// reference sequence length
	int SeqNameLen;	// sequence name length (excludes terminating '\0')
	char szSeqName[1]; // to hold '\0' terminated sequence name
} tsRefSeq;

typedef struct TAG_sBAIChunk {
	UINT32 Bin;				// chunk is associated to this bin
	UINT32 NextChunk;		// next chunk for same bin
	UINT32 Start;			// chunk starts at this loci
	UINT64 StartVA;			// start alignment BAM record is at this virtual address
	UINT32 End;				// chunk ends at this loci
	UINT64 EndVA;			// end alignment BAM record is at this virtual address
	} tsBAIChunk;

typedef struct TAG_sBAIbin {
	UINT32 NumChunks;		// number of chunks in this bin 
	UINT32 FirstChunk;		// first chunk in this bin (1..n)
	UINT32 LastChunk;		// last chunk in this bin (1..n)
	} tsBAIbin;

#pragma pack()


class CSAMfile
{
	eSAMFileType m_SAMFileType;				// SAM/BAM/BAI file to be processed

	BGZF* m_pBGZF;							// BAM is BGZF compressed 

	size_t m_AllocRefSeqsSize;				// currently allocated m_pRefSeqs memory size in bytes
	UINT32 m_NumRefSeqNames;				// number of reference sequence names
	UINT32 m_CurRefSeqNameID;				// identifies current reference sequence 
	size_t m_CurRefSeqsLen;					// currently used m_pRefSeqs in bytes
	tsRefSeq *m_pRefSeqs;					// allocated to hold reference sequence names, and their sequence lengths
	tsRefSeq *m_pCurRefSeq;					// current reference sequence

	int m_LocateRefSeqHistDepth;				// current ref seq name search history depth
	tsRefSeq *m_pLocateRefSeqHist[cMaxLocateRefSeqHist]; // ptrs to last cMaxLocateRefSeqHist successful searches
	char m_szLastNotLocatedRefSeqName[cMaxDescrIDLen+1]; // last reference sequence name which could not be located

	// note that m_pBAM references are also utilised as buffer space when processing for BAM/SAM input reads
	size_t m_AllocBAMSize;					// currently allocated m_pBAM memory size in bytes
	size_t m_CurBAMLen;						// currently used m_pBAM in bytes
	UINT32 m_NumBAMSeqNames;				// number of BAM sequence names
	UINT8 *m_pBAM;							// allocated to hold BAM header

	bool m_bInEOF;							// set true when all input has been read from file
	size_t m_CurInBAMIdx;					// when input from file processing then next byte to process is at this m_pBAM[m_CurInBAMIdx]
	size_t m_TotInBAMProc;					// total number of input bytes thus far processed
	int m_InBAMHdrLen;						// expected header length

	size_t m_AllocBAISize;					// currently allocated m_pBAI memory size in bytes
	size_t m_CurBAILen;						// currently used m_pBAI in bytes
	UINT32 m_NumBAISeqNames;				// number of BAI sequence names
	UINT8 *m_pBAI;							// allocated to hold BAI
	UINT32 m_AllocBAIChunks;				// currently this many chunks have been allocated
	UINT32 m_NumChunks;						// current number of chunks
	tsBAIChunk *m_pBAIChunks;				// to hold BAI chunks
	UINT32 m_NumBinsWithChunks;				// number of bins with at least one chunk
	tsBAIbin *m_pChunkBins;					// for each bin has number of chunks and identifies first and last chunk for that bin
	UINT32 m_NumOf16Kbps;					// number of virtual addresses in m_p16KOfsVirtAddrs 
	UINT64 *m_p16KOfsVirtAddrs;				// allocated to hold SAI 16Kbp linear virtual addresses

	gzFile m_gzInSAMfile;					// input when reading SAM as gzip
	int m_hInSAMfile;						// file handle used when reading SAM file
	BGZF* m_pInBGZF;						// BAM is BGZF compressed 

	gzFile m_gzOutSAMfile;					// output when compressing SAM as gzip
	int m_hOutSAMfile;						// file handle used when writing SAM file
	int m_hOutBAIfile;						// file handle used when writing BAI file

	bool m_bBAMfile;						// false if processing SAM, true if processing BAM/SAI file

	char m_szSAMfileName[_MAX_PATH];		// name of SAM or BAM file currently being processed
	char m_szBAIfileName[_MAX_PATH];		// name of SAI file currently being procssed

	char m_szVer[20];						// version text to use in generated SAM/BAM headers

	int m_ParseSeqState;					// 0 if next descr + sequence to be parsed, 1 if descr + sequence have been parsed, set back to 0 when sequence returned to user
	char m_szParsedDescriptor[cMaxDescrIDLen*2]; // to hold last parsed alignment descriptor by ReadSequence()
	int m_ParsedDescrLen;						// strlen(m_szDescriptor)
	char m_szParsedSeqBases[cMaxReadLen*3];		// to hold last parsed out sequence by ReadSequence()
	int m_ParsedSeqLen;							// strlen(m_szParsedSeqBases)
	int m_ParsedFlags;						// parsed out flags by ReadSequence()
	char m_ParsedszChrom[cMaxDescrIDLen*2];   // parsed out chrom by ReadSequence()
	int m_ParsedStartLoci;                  // parsed out start loci by ReadSequence()

		// calculate bin given an alignment covering [beg,end) (zero-based, half-close-half-open)
	int BAMreg2bin(int beg, int end);

	// calculate the list of bins that may overlap with region [beg,end) (zero-based)
	int BAMreg2bins(int beg, int end, UINT16 *plist);

	int
		AddChunk(UINT64 StartVA,			// chunk start alignment BAM record is at this virtual address
				UINT32 Start,				// chunk starts at this loci
				UINT64 EndVA,				// chunk alignment BAM record ends at this virtual address
				UINT32 End);				// chunk ends at this loci
	
	int UpdateSAIIndex(bool bFinal = false);	// alignments to current sequence completed, update SAI file with bins/chunks for this sequence

	static char *TrimWhitespace(char *pTxt);	// trim whitespace

public:
	CSAMfile(void);
	~CSAMfile(void);

	void Reset(bool bSync = false);			// if bSync true then fsync before closing output file handles

	static bool										// open and check if a SAM or BAM format file, returns true if SAM or BAM
		IsSAM(char *pszSAMFile);			// expected to be a SAM(gz) or if extension '.BAM' then a BAM file

	UINT32									// returns estimated number of sequences in SAM or BAM file
		EstSizes(char *pszFile,				// SAM or BAM file path+name to estimate sizes
			  INT64 *pFileSize,				// file is this size on disk
			  INT32 *pEstMaxDescrLen,		// with estimated maximum descriptor length
			  INT32 *pEstMeanDescrLen,		// estimated mean descriptor length
			  INT32 *pEstMaxSeqLen,			// and estimated maximum sequence length
			  INT32 *pEstMeanSeqLen,		// estimated mean sequence length
			  INT32 *pEstScoreSchema);		// currently will always return 0: no scoring 


	int										// open and initiate processing for SAM/BAM reads processing
		Open(char *pszSAMFile);				// expected to be a SAM(gz) or if extension '.BAM' then a BAM file

	int				// locates reference sequence name and returns it's SeqID, returns 0 if unable to locate a match
		LocateRefSeqID(char *pszRefSeqName); // reference sequence name to locate

	int											// negative if errors parsing otherwise 0 for success
		ParseSAM2BAMalign(char *pszSAMline,		// parsing this SAM format line
					tsBAMalign *pBAMalign,     // into this tsBAMalign structure
			        CBEDfile *pBEDremapper = NULL);		  // with optional remapping of alignment loci from features (contigs) in this BED file

	int				// alignment length as calculated from SAM/BAM CIGAR string, only 'M','X','=' lengths contribute
		CigarAlignLen(char *pszCigar);	// alignment length as calculated from SAM/BAM CIGAR

	int										// number of chars returned in pszNxtLine
		GetNxtSAMline(char *pszNxtLine);	// copy next line read from input source to this line buffer; caller must ensure that at least cMaxBAMLineLen has been allocated for the line buffer

	int ReadDescriptor(char *pszDescriptor,int MaxLen); // copies last descriptor processed into pszDescriptor and returns copied length

	int						// returns actual number bases in sequence (eBSFSuccess == EOF,eBSFFastaDescr == End of current sequence, descriptor line now available)
		ReadSequence(void *pRetSeq,		// where to return sequence, can be NULL if only interested in the sequence length
					 int Max2Ret,		// max to return, ignored if pRetSeq is NULL
					 bool bSeqBase,		// if false then return ascii, if true then return as etSeqBase
					 bool RptMskUpperCase);	// default is false, UCSC softmasked use lowercase when repeat masked

	int										// creat and initiate processing for SAM or BAM - with optional BAI index - file generation
		Create(eSAMFileType SAMType,		// file type, expected to be either eSFTSAM or eSFTBAM_BAI
				char *pszSAMFile,			// SAM(gz) or BAM file name
				int ComprLev = cDfltComprLev,	// if BAM then BGZF compress at this requested level (0..9)
			    char *pszBAIFile = NULL,	// BAI file name - if NULL then defaults to samfile name plus '.bai' appended
				char *pszVer = NULL);		// version text to use in generated SAM/BAM headers - if NULL then defaults to cszProgVer

		// reference sequence names are expected to be presorted in seqname ascending alpha order and then AddRefSeq'd in that ascending order
	int AddRefSeq(char *pszSpecies,			// sequence from this species
				  char *pszSeqName,			// sequence name
				  UINT32 SeqLen);			// sequence is of this length

	int StartAlignments(void);				// completed added reference sequences, about to add alignments

	int					// add alignment to be reported
		AddAlignment(tsBAMalign *pBAMalign,  // alignment to report
						bool bLastAligned = false);  // true if this is the last read which was aligned, may be more reads but these are non-aligned reads

	int Close(void);

};
