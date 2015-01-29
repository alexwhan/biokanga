// kangade.cpp : Defines the entry point for the console application.
// Purpose -
// Identifies the differentially expressed transcripts
//
// 1.0.0	first reasonably stable release
// 1.0.6	added outlier aligned reads reduction - if read count at a loci is significantly above the flanking loci counts then reduce the count differential
//          slide a window of 25bp over transcript and at each loci check for read counts more than 3x the observed variance within the window; reduce the count by sqrt of the difference
// 1.0.7	Report the unique read start loci
// 1.0.8	User can specify a minimum number of unique start loci per transcript
// 1.0.11	Calc PValues using Chisquare
// 1.0.12   Allow for feature classes; user optionally specifies a feature classification file
// 1.0.13   Changed the fold change calculation back to the more universally accepted standard of a pure ratio
// 1.0.14   Allowed for user to specify exclusion zones (could be zones including loci of rRNA transcriptions as an example) which are to be excluded from any processing including library size normalisation
// 1.0.15   Fix for inport control file name being incorectly reported when it was the input experiment file load which failed
// 1.0.16   Implemented SAM input file processing
// 1.0.17   Improved error reporting if unable to load input files
// 1.0.18   Optimisations to improve throughput
// 1.0.19   Poisson the total library counts when generating ChiSquared P-values
// 1.12.0   Public release
// 1.12.1   Fix for issue of when counts above threshold with fold median == 0.0 then that feature is being assigned DECntsScore of 4 instead of 1
// 1.12.5   Allow SAM input format processing
// 1.12.6   changed _MAX_PATH to 260 to maintain compatibility with windows
// 2.0.0    removed limits on assembly sizes
// 2.0.1    cosmetic change only - version reported had 2 comma prefixes
// 2.0.2    fix for issue where if very few transcipts then occasionally a transcript would not be attributed with counts
// 2.0.3    changed bin distributions to be read depth coverage instead of previous read starts
// 2.0.4    increased max parameters accepted from 20 to 50

#include "stdafx.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if _WIN32
#include <process.h>
#include "../libbiokanga/commhdrs.h"
#else
#include <sys/mman.h>
#include <pthread.h>
#include "../libbiokanga/commhdrs.h"
#endif

const char *cpszProgVer = "2.0.4";			// increment with each release

const int cMaxWorkerThreads = 64;			// allow up to this many worker threads

const double cNormCntsScale = 0.0;			// default normalise experiment to control counts is autoscaling

const double cClampFoldChange = 25.0;		// clamp fold changes to be at most cClampFoldChange

const int cMaxInFileSpecs = 10;				// allow at most a total of this many wildcarded control or experiment input read alignment loci files

const int cMinNumBins= 5;					// min number of bins
const int cDfltNumBins = 10;				// default number of bins
const int cMaxNumBins = 100;				// max number of bins

const int cMinStartLociThres = 1;			// user specified min number of unique start loci
const int cDfltStartLociThres = 5;			// default min number of unique start loci
const int cMaxStartLociThres = 200;			// user specified max number of unique start loci

const int cMinFeatCntThres = 1;				// user specified min feature read counts
const int cDfltFeatCntThres = 10;			// default min feature read counts
const int cMaxFeatCntThres = 200;			// user specified max min feature read counts

const int cMaxCoalesceWinLen = 20;			// maximum counts coalescing window length
const int cDfltCoalesceWinLen = 1;			// default counts coalescing window length

const int cMaxFeats2ProcAlloc = 200;		// allocate at most this many FeatureIDs to a thread for processing at any time

const int cWrtBinBuffSize = 0x03fffff;		// use a 4 MB write buffer size
const int cWrtStatBuffSize = 0x0fffff;		// use a 1 MB write buffer size

const int cAlignReadsLociInitalAlloc  = 30000000;	// initial allocation to hold this many read alignment loci
const int cAlignReadsLociRealloc	  = 15000000;	// realloc read alignments allocation in this sized increments

// there are some assumptions here in terms of the max number of aligned read loci within the max length transcripts
// one day these assumptions may be shown to be invalid but let's see....
const int cMaxAssumTransLen = 2000000;					// assume no transcribed region will be longer than this
const int cMaxAssumTransLoci = cMaxAssumTransLen/10;	// assume very long transcribed regions will be low abundance reads and number of unique read aligned loci will be at most 10% of cMaxAssumTransLen

const int cMaxConfidenceIterations = 10000;	// max number of iterations when calculating confidence intervals and PValues

const int cMaxExclZones = 1000;			// max allowed number of exclusion zones within which reads are to be excluded

// following thresholds are used for characterisation of differential transcription state
// characterisation is into high, moderate, low and none with none including both negatively corelated and corelections less than cLoPeason
const double cHiPearsonThres = 0.8;					// >= this Pearson if control and experiment highly corelated in their reads distributions
const double cModPearsonThes = 0.5;					// >= this Pearson if control and experiment moderately corelated
const double cLoPearsonThres = 0.3;					// >= this Pearson if control and experiment have low corelation
const double cNoPearsonThres = cLoPearsonThres;		// < this Pearson if control and experiment have either negative or no corelation

const double cNoFoldChange = 1.25;				// if less than this fold change then characterise as none
const double cLoFoldChange = 1.50;				// if less than this fold change then characterise as low
const double cModFoldChange = 1.75;				// if less than this fold change then characterise as moderate
const double cHiFoldChange = cModFoldChange;	// if equal or more than this fold change then characterise as hi

// There are multiple processing phases
typedef enum TAG_eProcPhase {
	ePPInit = 0,								// initial initialisation
	ePPLoadFeatures,							// loading features of interest from BED file
	ePPLoadFeatClass,							// loading gene feature classifications
	ePPLoadExclZones,							// loading loci of reads which are to be excluded from any processing
	ePPLoadReads,								// loading read alignments from disk
	ePPReducePCRartifacts,						// reducing library PCR excessive read count artifacts
	ePPCoalesceReads,							// coalescing reads
	ePPNormLibCnts,								// normalise library counts to be very nearly the same
	ePPAllocDEmem,								// allocating memory for usage by threads and to hold feature DE fold changes etc
	ePPDDd,										// attempting to determine transcripts DE status for both counts and Pearson with PValue on transcript counts
	ePPReport,									// report to file transcript status, fold changes and Pearson
	ePPCompleted								// processing completed
	} etProcPhase;

typedef enum TAG_ePearsonScore {
	ePSIndeterminate = 0,						// insufficent counts, or other filtering criteria, by which Pearson can be calculated
	ePSNone,									// control and experiment have either negative or corelation below that of cLoPearsonThres
	ePSLow,										// lowly corelated Pearsons (cLoPearsonThres)
	ePSMod,										// moderately corelated Pearsons (cModPearsonThres)
	ePSHi										// highly corelated Pearsons (cHiPearsonThres)
} etPearsonScore;

typedef enum TAG_eCntsScore {
	eDEIndeterminate = 0,						// insufficent counts, or other filtering criteria, by which foldchange can be calculated
	eDEHi = 1,									// high changes in DE counts (cHiFoldChange)
	eDESMod,									// moderate changes in DE counts (cModFoldChange)
	eDSElow,									// low change in DE lowly corelated Pearsons (cLoFoldChange)
	eDESNone,									// no or very little change in DE counts (cNoFoldChange)
} etCntsScore;

// processing modes
typedef enum TAG_ePMode {
	ePMdefault = 0,				// Standard sensitivity (2500 iterations)
	ePMMoreSens,				// More sensitive (slower 5000 iterations)
	ePMUltraSens,				// Ultra sensitive (very slow 10000 iterations)
	ePMLessSens,				// Less sensitive (quicker 1000 iterations)
	ePMplaceholder				// used to set the enumeration range
	} etPMode;


typedef enum eBEDRegion {
	eMEGRAny = 0,				// process any region
	eMEGRExons,					// only process exons
	eMEGRIntrons,				// only process introns
	eMEGRCDS,					// only process CDSs
	eMEGUTR,					// only process UTRs
	eMEG5UTR,					// only process 5'UTRs
	eMEG3UTR					// only process 3'UTRs
} etBEDRegion;

// strand processing modes
typedef enum TAG_eStrandProc {
		eStrandDflt,			// default is to ignore the strand
		eStrandWatson,			// process for Watson or sense
		eStrandCrick,			// process for Crick or antisense
		eStrandPlaceholder
} etStrandProc;

#pragma pack(1)

typedef struct TAG_sExclZone {
	int RegionID;			// identifies this loci exclusion
	int ChromID;			// loci on this chrom, as generated by IDtoChrom()
	char Strand;		    // loci on this strand
	int StartLoci;			// loci starts inclusive
	int EndLoci;			// loci ends inclusive
} tsExclZone;

typedef struct TAG_sRefIDChrom {
	UINT32 ChromID;			// uniquely identifies chromosome
	UINT32 Hash;				// hash on chromosome name - CUtility::GenHash16()
	char szChromName[cMaxDatasetSpeciesChrom];	// chromosome name
} tsRefIDChrom;

typedef struct TAG_sAlignReadLoci {
	UINT8 ExprFlag:1;			// 0 if aligned read from control, 1 if from experiment
	UINT8 Sense:1;				// 0 if '-' or antisense crick, 1 if '+' or sense watson
	UINT8 FileID:5;				// from which reads alignment file was this alignment loaded
	UINT32 NormCnts;			// coalesced counts with control vs experiment library size normalisation
	UINT32 ArtCnts;				// used as temp count storage during artefact reduction processing
	UINT32 AlignHitIdx;			// current read hit index + 1 for this read
	UINT32 ChromID;				// identifies hit chromosome - ChromToID(pszChrom)
	UINT32 Loci;				// 5' start loci on chromosome of this alignment
	UINT32 ReadLen;				// read length
} tsAlignReadLoci;

typedef struct TAG_sAlignLociInstStarts {
	UINT32 Bin;					// instance counts are for this bin
	UINT32 RelLoci;				// instance counts are at this loci
	UINT32 NumCtrlStarts;		// number of control start loci instances at this loci instance
	UINT32 NumExprStarts;		// number of experiment start loci instances at this loci instance
} tsAlignLociInstStarts ;

typedef struct TAG_sAlignBin {
	UINT32 Bin;					// poisson counts are from this bin (1..n)
	UINT32 BinRelStartLoci;		// bin starts at this relative loci
	UINT32 BinRelEndLoci;		// bin starts at this relative loci
	UINT32 NumCtrlInstStarts;	// number of control start loci instances in this bin
	UINT32 NumExprInstStarts;	// number of experiment start loci instances in this bin
	UINT32 ControlCnts;			// original total control counts in this bin over control start loci instances
	UINT32 ExperimentCnts;		// original total experiment counts in this bin over experiment start loci instances
	UINT32 ControlCoverage;     // coverage apportioned to this bin (control reads likely to overlap multiple bins)
	UINT32 ExperimentCoverage;  // coverage apportioned to this bin (experiment reads likely to overlap multiple bins)
	UINT32 ControlPoissonCnts;	 // after poisson control counts
	UINT32 ExperimentPoissonCnts;// after poisson experiment counts
} tsAlignBin;

typedef struct TAG_sFeatDE {
		char szFeatName[128];	// feature name
		int FeatLen;			// feature transcribed length
		int NumExons;			// number of exons
		int UserClass;			// user classification for this feature
		int DEscore;			// transformed DE score (range 0..9)
		int CntsScore;			// score for DE cnts (range 0..4)
		int PearsonScore;		// score for Pearson (range 0..4)
		int CtrlCnts;			// control read counts
		int ExprCnts;			// expression read counts
		int SumCtrlExprCnts;	// sum control + expression counts
		double PValueMedian;	// median P-value (0..1.0)
		double PValueLow95;		// low 95 P-value (0..1.0)
		double PValueHi95;		// high 95 P-value (0..1.0)
		double ObsFoldChange;	// observed fold change (0..n) if less than 1.0 then negative fold change
		double FoldMedian;		// fold change median (0..50.0) if less than 1.0 then negative fold change
		double FoldLow95;		// fold change low 95 percentile (0..50.0) if less than 1.0 then negative fold change
		double FoldHi95;		// fold change high 95 percentile (0..50.0) if less than 1.0 then negative fold change
		double PearsonObs;		// Pearson observed	(-1.0 to 1.0)
		double PearsonMedian;	// Pearson median (-1.0 to 1.0)
		double PearsonLow95;	// Pearson low 95 percentile (-1.0 to 1.0)
		double PearsonHi95;		// Pearson high 95 percentile (-1.0 to 1.0)
		int TotCtrlStartLoci;	// total number of unique control start loci in this transcript
		int TotExprStartLoci;	// total number of unique experiment start loci in this transcript
		int BinsShared;			// number of bins with both control and experiment counts
		int BinsExclCtrl;		// number of bins holding counts exclusive to control
		int BinsExclExpr;		// number of bins holding counts exclusive to experiment
		UINT32 BinsCtrlDepth[cMaxNumBins];	// to hold read coverage depth in each control bin
		UINT32 BinsExprDepth[cMaxNumBins];	// to hold read coverage depth in each experiment bin
} tsFeatDE;


// each thread has it's own instance of the following
typedef struct TAG_ThreadInstData {
	UINT32 ThreadInst;				// uniquely identifies this thread instance
#ifdef _WIN32
	HANDLE threadHandle;			// handle as returned by _beginthreadex()
	unsigned int threadID;			// identifier as set by _beginthreadex()
#else
	int threadRslt;					// result as returned by pthread_create ()
	pthread_t threadID;				// identifier as set by pthread_create ()
#endif
	CStats *pStats;						// used for ChiSquare processing
	teBSFrsltCodes Rslt;				// thread processing completed result - eBSFSuccess if no errors
	int FeatureID;						// currently processing this feature

	UINT32 CurFeatLen;					// current feature length over which counts are being processed
	int CurRegionLen;					// region length for curent gene or feature being processed

	double *pPValues;					// to hold PValues for transcript
	double *pFeatFoldChanges;			// to hold feature fold changes (Ctrl + 0.001)/(Expr + 0.001)) whilst determining feature confidence interval
	double *pPearsons;					 // prealocated to hold Pearsons whilst determining confidence interval
	UINT32 NumBinsWithLoci;				// pAlignBins currently contains this number of bin instances with at least one aligned loci instance
	tsAlignBin *pAlignBins;				// preallocated to hold alignment bin cnts reserved for this thread

	tsAlignBin *pPoissonAlignBins;		// to hold alignment bins with poisson applied
	UINT32 NumBinInstStarts;				// m_pBinInstsCnts currently contains this many experiment and control start instances
	tsAlignLociInstStarts *pBinLociInstStarts;	// preallocated pts to list of control and experiment start instance counts for all bins
	int MaxFeats2Proc;					// max number of FeatureIDs which can be allocated for processing by this thread by GetFeats2Proc() into Feats2Proc[]
	int NumFeats2Proc;					// process this number of features in Feats2Proc[]
	int NumFeatsProcessed;				// number features processed currently from Feats2Proc
	int Feats2Proc[cMaxFeats2ProcAlloc];	// these are the feature identifiers for the features to be processed
	CSimpleRNG *pSimpleRNG;				// random generator exclusively for use by this thread
} tsThreadInstData;

#pragma pack()

#ifdef _WIN32
HANDLE m_hDEMtxIterReads;					// used to serialise writes to results files
SRWLOCK m_hDERwLock;
HANDLE m_hDEThreadLoadReads;
unsigned __stdcall ThreadedDEproc(void * pThreadPars);
#else
pthread_mutex_t m_hDEMtxIterReads;
pthread_rwlock_t m_hDERwLock;
void *ThreadedDEproc(void * pThreadPars);
#endif

char *Region2Txt(etBEDRegion Region);
char ReportStrand(etStrandProc StrandProc);
void DEReset(void);
void DEInit(void);

teBSFrsltCodes Process(etPMode PMode,					// processing mode
					int NumThreads,						// number of threads (0 defaults to number of CPUs)
					int  CoWinLen,						// counts coalescing window length
					int ArtifactCntsThres,				// if counts at any loci are >= this threshold then process for PCR artifact reduction
				    UINT32 LimitAligned,				// for test/evaluation can limit number of reads parsed to be no more than this number (0 for no limit)
					bool bFiltNonaligned,				// true if only features having at least one read aligned are to be be reported
					char AlignStrand,					// process for reads aligning to this strand only
					char FeatStrand,					// process for genes or features on this strand only
					etBEDRegion Region,					// process for this genomic region only
					int	NumBins,						// number of non-overlapping count bins
					int MinFeatCntThres,				// minimum feature count threshold, control or experiment, required (1 to 200, defaults to 20)
					int MinStartLociThres,				// minimum feature unique start loci threshold, control or experiment, required (1 to 200, defaults to 10)
					double NormCntsScale,				// counts normalisation scale factor
					int FType,							// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
					int NumInputControlSpecs,			// number of input file specs
					char **pszInControlFiles,			// input control aligned reads files
					int NumInputExperimentSpecs,		// number of input file specs
					char **pszInExperimentFiles,		// input experiment aligned reads files
					char *pszInFeatFile,				// input gene or feature BED file
					char *pszFeatClassFile,				// classify genes or features from this file
					char *pszExclZonesFile,				// exclude reads overlaying zone loci specified from this file
					char *pszOutfile,					// output into this file
					char *pszBinCountsFile);			// output bin counts to this file

double ClampFoldChange(double Scale);

teBSFrsltCodes
LoadGeneFeatures(char Strand,			// features on this strand
				 char *pszInFeatFile);	// from this BED file

int
LoadAlignedReadFiles(char Strand,		// process for this strand '+' or '-' or for either '*'
			int FType,					// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
			int NumInputControlSpecs,	// number of input file specs
			char **pszInControlFiles,	// input control aligned reads files
			int NumInputExperimentSpecs,// number of input file specs
			char **pszInExperimentFiles);	// input experiment aligned reads files

teBSFrsltCodes
LoadAlignedReadsBED(bool bIsExperiment,		// false if control file, true if experiment
			int FileID,						// uniquely identifies this file
			char *pszInFile,
			char FiltStrand);

teBSFrsltCodes
LoadAlignedReadsCSV(bool bIsExperiment,		// false if control file, true if experiment
			int FileID,						// uniquely identifies this file
			char *pszInFile,
			char FiltStrand);



int
CoalesceReadAlignments(int WinLen,				// coalescing window length 1..20
					   bool bSamesense,			// if true then only coalesce reads with same sense
					   bool bExperiment);		// if true then coalesce experiment read loci otherwise control read loci

teBSFrsltCodes
ReducePCRartifacts(int FlankLen,				// 5' and 3' flank length over which the background mean rate is to be measured
			int ArtifactCntsThres);				// if counts at any loci are >= this threshold then process for PCR artifact reduction

teBSFrsltCodes NormaliseLibraryCounts(void);	// normalise library sizes

teBSFrsltCodes GenPoissonLibraryCounts(void);   // generate poisson noise over all library counts

int
AddDEPearsons(tsThreadInstData *pThreadInst,
			char *pszFeatName,        	// counts DE and Pearson is for this feature
			int NumExons,				// feature has this number of exons
			int UserClass);				// user classification for this feature

double								    // returned median PValue
PearsonsPValue(tsThreadInstData *pThreadInst,	// thread instance
		double Pearson,					// observed pearson
		int MaxPerms,					// maximum number of permutions
		tsAlignBin *pAlignBins,			// bins containing alignment counts
		double *pPValueLow95,			// returned low 95 percentile
		double *pPValueHi95,			// returned upper 95 percentile
		double *pLow95,					// returned low 95 percentile
		double *pHi95,					// returned upper 95 percentile
		double *pMedian,				// returned median
		double *pFeatLow95,				// returned low 95 percentile for feature
		double *pFeatHi95,				// returned upper 95 percentile for feature
		double *pFeatMedian);			// returned median for feature

teBSFrsltCodes
GenBinAlignStarts(tsThreadInstData *pThreadInst,	// thread instance
			      UINT32 RegionOfs,			// StartLoci is at this region offset
				  char *pszChrom,			// alignments are on this chrom
				  UINT32 StartLoci,			// must start on or after this loci
				  UINT32 EndLoci);			// must start on or before this loci


teBSFrsltCodes
AddAlignBinCnts(tsThreadInstData *pThreadInst,	// thread instance data
			int RelLoci,			// relative offset within  pThreadInst->CurRegionLen from which these cnts were derived
			int MeanControlReadLen,	// control reads were of this mean length
			bool bSense,				// 1 if aligned sense, 0 if antisense
			int ControlCnts,		// attribute this many control counts (alignments) to relevant bin(s)
			int MeanExperimentReadLen,	// experiment reads were of this mean length
			int ExperimentCnts);		// attribute this many experiment counts (alignments) to relevant bin(s)

int ReportDEandPearsons(void);
int ReportDEandPearsonBinCounts(void);

char *IDtoChrom(UINT32 ChromID);			// returns ptr to chrom for ChromID
UINT32 ChromToID(char *pszChrom);			// get unique chromosome identifier

double	poz (double	z);						/* returns cumulative probability from -oo to z */

static int SortAlignments(const void *arg1, const void *arg2);
static int SortDoubles(const void *arg1, const void *arg2);
static int SortDEScore(const void *arg1, const void *arg2);
static int SortFoldMedian(const void *arg1, const void *arg2);

CStopWatch gStopWatch;
CDiagnostics gDiagnostics;				// for writing diagnostics messages to log file
char gszProcName[_MAX_FNAME];			// process name

#ifdef _WIN32
// required by str library
#if !defined(__AFX_H__)  ||  defined(STR_NO_WINSTUFF)
HANDLE STR_get_stringres()
{
	return NULL;	//Works for EXEs; in a DLL, return the instance handle
}
#endif

const STRCHAR* STR_get_debugname()
{
	return _T("kangade");
}
// end of str library required code
#endif


#ifdef _WIN32
int _tmain(int argc, char* argv[])
{
// determine my process name
_splitpath(argv[0],NULL,NULL,gszProcName,NULL);
#else
int
main(int argc, const char** argv)
{
// determine my process name
CUtility::splitpath((char *)argv[0],NULL,gszProcName);
#endif
int iScreenLogLevel;		// level of file diagnostics
int iFileLogLevel;			// level of file diagnostics
char szLogFile[_MAX_PATH];	// write diagnostics to this file
int Rslt = 0;   			// function result code >= 0 represents success, < 0 on failure

int Idx;
etPMode PMode;				// processing mode
int NumberOfProcessors;		// number of installed CPUs
int NumThreads;				// number of threads (0 defaults to number of CPUs)

int FType;					// expected input element file type - auto, CSV, BED or SAM

bool bFiltNonaligned;		// true if only features having at least one read aligned are to be be reported
etBEDRegion Region;			// process for this functional region only
double NormCntsScale;		// counts normalisation scale factor
int LimitAligned;			// for test/evaluation can limit number of reads parsed to be no more than this number (0 for no limit)
int NumBins;				// this number of non-overlapping bins
int ArtifactCntsThres;		// if counts at any loci are >= this threshold then process for PCR artifact reduction
int CoWinLen;				// counts coalescing window length
int MinFeatCntThres;		// minimum count threshold (control + expression) per feature before processing for differential expression
int MinStartLociThres;		// minimum feature unique start loci threshold, control or experiment, required (1 to 200, defaults to 10)


etStrandProc AlignStrand;	// process for reads on this strand only
etStrandProc FeatStrand;	// process for genes or features on this strand only


int NumInputControlSpecs;			// number of input file specs
char *pszInControlFiles[cMaxInFileSpecs];			// input control aligned reads files
int NumInputExperimentSpecs;		// number of input file specs
char *pszInExperimentFiles[cMaxInFileSpecs];		// input experiment aligned reads files

char szInFeatFile[_MAX_PATH];        // input genes or features BED file
char szFeatClassFile[_MAX_PATH];	 // classify genes or features from this file
char szExclZonesFile[_MAX_PATH];     // exclude reads overlaying zone loci specified in this file from processing
char szOutfile[_MAX_PATH];			 // output file
char szBinCountsFile[_MAX_PATH];	 // output file for bin counts


// command line args
struct arg_lit  *help    = arg_lit0("hH","help",                "print this help and exit");
struct arg_lit  *version = arg_lit0("v","version,ver",			"print version information and exit");
struct arg_int *FileLogLevel=arg_int0("f", "FileLogLevel",		"<int>","Level of diagnostics written to screen and logfile 0=fatal,1=errors,2=info,3=diagnostics,4=debug");
struct arg_file *LogFile = arg_file0("F","log","<file>",		"diagnostics log file");

struct arg_int *pmode = arg_int0("m","mode","<int>",		    "processing sensitivity: 0 - standard sensitivity, 1 - more sensitive (slower), 2 - ultra sensitive (slowest), 3 - less sensitive (quicker) (default is 0)");
struct arg_int *region = arg_int0("r","region","<int>",		    "process region: 0 - complete transcript, 1: Exons, 2: Introns, 3: CDSs, 4: UTRs, 5: 5'UTRs, 6: 3'UTRs (default 1 Exons)");
struct arg_lit  *filtnonaligned = arg_lit0("A","nonalign",		"do not report on features which have no aligned reads");

struct arg_dbl *normcntsscale = arg_dbl0("n","mode","<dbl>",	"control counts normalisation scale factor 0.1 to 10.0 to scale expr counts, -0.1 to -10.0 to scale control (default is 0 for auto-library size normalisation)");

struct arg_int  *alignstrand = arg_int0("s","alignstrand","<int>","read alignment strand processing: 0 - independent, 1 - sense, 2 - antisense (default is independent)");
struct arg_int  *featstrand = arg_int0("S","featstrand","<int>","gene or feature strand processing: 0 - independent, 1 - sense, 2 - antisense (default is independent)");
struct arg_int  *cowinlen = arg_int0("c","cowinlen","<int>",	"counts coalescing window length (1 to 20, defaults to 1 or no coalescence)");
struct arg_int  *artifactcntthres = arg_int0("a","artifactthres","<int>",	"artifact loci cnt reduction threshold, 0 to disable (1 to 500, defaults to 20)");

struct arg_int  *minfeatcntthres = arg_int0("C","minfeatcnts","<int>","minimum feature count threshold, control or experiment, required (1 to 200, defaults to 10)");
struct arg_int  *minstartlocithres = arg_int0("z","minfeatloci","<int>","minimum feature unique start loci, control or experiment, required (1 to 200, defaults to 5)");

struct arg_int  *numbins = arg_int0("b","numbins","<int>",		"bin counts for each gene/feature into this many non-overlapping bins (5 to 100, defaults to 10)");
struct arg_int  *limitaligned = arg_int0("L","limitaligned","<int>","for test/evaluation can limit number of reads parsed to be no more than this number (default 0 for no limit)");

struct arg_int *ftype = arg_int0("t","filetype","<int>",		"input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM (default = 0)");

struct arg_file *incontrolfiles = arg_filen("i","control","<file>",1,cMaxInFileSpecs, "input control read alignments files (wildcards allowed)");
struct arg_file *inexperfiles = arg_filen("I","experiment","<file>",1,cMaxInFileSpecs,"input experiment read alignments file (wildcards allowed)");
struct arg_file *infeatfile = arg_file1("g","ingene","<file>",	   "input gene or feature biobed BED file");

struct arg_file *featclass = arg_file0("G","featclass","<file>", "input gene or feature classification CSV file");
struct arg_file *excludezones = arg_file0("x","excludezones","<file>", "exclude reads overlaying zone loci specified in this CSV file from any processing");

struct arg_file *outfile = arg_file1("o","out","<file>",		"output transcript differentials to this file as CSV");
struct arg_file *bincountsfile = arg_file0("O","bincounts","<file>","output transcript bin counts to this file as CSV");
struct arg_int *threads = arg_int0("T","threads","<int>",		"number of processing threads 0..n (defaults to 0 which sets threads to number of CPU cores, max 64)");

struct arg_end *end = arg_end(50);

void *argtable[] = {help,version,FileLogLevel,LogFile,
					pmode,ftype,filtnonaligned,limitaligned,artifactcntthres,alignstrand,featstrand,region,normcntsscale,minfeatcntthres,minstartlocithres,cowinlen,
					numbins,incontrolfiles,inexperfiles,outfile,bincountsfile,infeatfile,featclass,excludezones,threads,
					end};

char **pAllArgs;
int argerrors;
argerrors = CUtility::arg_parsefromfile(argc,(char **)argv,&pAllArgs);
if(argerrors >= 0)
	argerrors = arg_parse(argerrors,pAllArgs,argtable);

/* special case: '--help' takes precedence over error reporting */
if (help->count > 0)
        {
		printf("\n%s Kanga differential expression analyser, Version %s\nOptions ---\n", gszProcName,cpszProgVer);
        arg_print_syntax(stdout,argtable,"\n");
        arg_print_glossary(stdout,argtable,"  %-25s %s\n");
		printf("\nNote: Parameters can be entered into a parameter file, one parameter per line.");
		printf("\n      To invoke this parameter file then precede it's name with '@'");
		printf("\n      e.g. %s @myparams.txt\n",gszProcName);
		printf("\nPlease report any issues regarding usage of %s to stuart.stephen@csiro.au\n\n",gszProcName);
		exit(1);
        }

    /* special case: '--version' takes precedence error reporting */
if (version->count > 0)
        {
		printf("\n%s Version %s\n",gszProcName,cpszProgVer);
		exit(1);
        }

if (!argerrors)
	{
	if(FileLogLevel->count && !LogFile->count)
		{
		printf("\nError: FileLogLevel '-f%d' specified but no logfile '-F<logfile>\n'",FileLogLevel->ival[0]);
		exit(1);
		}

	iScreenLogLevel = iFileLogLevel = FileLogLevel->count ? FileLogLevel->ival[0] : eDLInfo;
	if(iFileLogLevel < eDLNone || iFileLogLevel > eDLDebug)
		{
		printf("\nError: FileLogLevel '-l%d' specified outside of range %d..%d\n",iFileLogLevel,eDLNone,eDLDebug);
		exit(1);
		}

	if(LogFile->count)
		{
		strncpy(szLogFile,LogFile->filename[0],_MAX_PATH);
		szLogFile[_MAX_PATH-1] = '\0';
		}
	else
		{
		iFileLogLevel = eDLNone;
		szLogFile[0] = '\0';
		}

	// now that log parameters have been parsed then initialise diagnostics log system
	if(!gDiagnostics.Open(szLogFile,(etDiagLevel)iScreenLogLevel,(etDiagLevel)iFileLogLevel,true))
		{
		printf("\nError: Unable to start diagnostics subsystem\n");
		if(szLogFile[0] != '\0')
			printf(" Most likely cause is that logfile '%s' can't be opened/created\n",szLogFile);
		exit(1);
		}

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Version: %s",cpszProgVer);

	PMode = (etPMode)(pmode->count ? pmode->ival[0] : ePMdefault);
	if(PMode < ePMdefault || PMode >= ePMplaceholder)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Processing sensitivity '-m%d' specified outside of range %d..%d",PMode,0,(int)ePMplaceholder-1);
		exit(1);
		}

#ifdef _WIN32
	SYSTEM_INFO SystemInfo;
	GetSystemInfo(&SystemInfo);
	NumberOfProcessors = SystemInfo.dwNumberOfProcessors;
#else
	NumberOfProcessors = sysconf(_SC_NPROCESSORS_CONF);
#endif
	int MaxAllowedThreads = min(cMaxWorkerThreads,NumberOfProcessors);	// limit to be at most cMaxWorkerThreads
	if((NumThreads = threads->count ? threads->ival[0] : MaxAllowedThreads)==0)
		NumThreads = MaxAllowedThreads;
	if(NumThreads < 0 || NumThreads > MaxAllowedThreads)
		{
		gDiagnostics.DiagOut(eDLWarn,gszProcName,"Warning: Number of threads '-T%d' specified was outside of range %d..%d",NumThreads,1,MaxAllowedThreads);
		gDiagnostics.DiagOut(eDLWarn,gszProcName,"Warning: Defaulting number of threads to %d",MaxAllowedThreads);
		NumThreads = MaxAllowedThreads;
		}


	FType = ftype->count ? ftype->ival[0] : 0;
	if(FType < 0 || FType >= 4)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Expected input element file format '-t%d' specified outside of range %d..%d",FType,0,3);
		exit(1);
		}

	bFiltNonaligned = filtnonaligned->count ? true : false;

	Region = (etBEDRegion)(region->count ? region->ival[0] : eMEGRExons);	// default as being exons
	if(Region < eMEGRAny || Region > eMEG3UTR)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Specified region '-g%d' outside of range 0..%d",Region,eMEG3UTR);
		exit(1);
		}

	AlignStrand = (etStrandProc)(alignstrand->count ? alignstrand->ival[0] : eStrandDflt);
	if(AlignStrand < eStrandDflt || AlignStrand >= eStrandPlaceholder)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Alignment strand '-s%d' must be in range %d..%d",AlignStrand,eStrandDflt,eStrandCrick);
		exit(1);
		}

	FeatStrand = (etStrandProc)(alignstrand->count ? alignstrand->ival[0] : eStrandDflt);
	if(FeatStrand < eStrandDflt || FeatStrand >= eStrandPlaceholder)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Feature strand '-S%d' must be in range %d..%d",FeatStrand,eStrandDflt,eStrandCrick);
		exit(1);
		}

	MinStartLociThres = minstartlocithres->count ? minstartlocithres->ival[0] : cDfltStartLociThres;
	if(MinStartLociThres < cMinStartLociThres || MinStartLociThres > cMaxStartLociThres)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Minimum unique feature start loci threshold '-C%d' must be in range 1..%d",MinStartLociThres,cMinStartLociThres,cMaxStartLociThres);
		exit(1);
		}

	MinFeatCntThres = minfeatcntthres->count ? minfeatcntthres->ival[0] : cDfltFeatCntThres;
	if(MinFeatCntThres < cMinFeatCntThres || MinFeatCntThres > cMaxFeatCntThres)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Minimum feature count threshold '-C%d' must be in range 1..%d",MinFeatCntThres,cMinFeatCntThres,cMaxFeatCntThres);
		exit(1);
		}

	ArtifactCntsThres = artifactcntthres->count ? artifactcntthres->ival[0] : 10;
	if(ArtifactCntsThres < 0 || ArtifactCntsThres > 500)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Artifact loci read counts threshold '-a%d' must be in range 0..500",ArtifactCntsThres);
		exit(1);
		}

	LimitAligned = limitaligned->count ? limitaligned->ival[0] : 0;
	if(LimitAligned < 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Limit on aligned reads processed '-L%d' must be >= 0",LimitAligned);
		exit(1);
		}

	CoWinLen = cowinlen->count ? cowinlen->ival[0] : cDfltCoalesceWinLen;
	if(CoWinLen < 1 || CoWinLen > cMaxCoalesceWinLen)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: counts coalescing window length '-c%d' must be in range 1..%d",CoWinLen,cMaxCoalesceWinLen);
		exit(1);
		}

	NumBins = numbins->count ? numbins->ival[0] : cDfltNumBins;
	if(NumBins == 0)
		NumBins = cDfltNumBins;
	if(NumBins < cMinNumBins || NumBins > cMaxNumBins)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Number of bins '-b%d' must be in range of %d to %d",NumBins,cMinNumBins,cMaxNumBins);
			exit(1);
			}

	NormCntsScale = (double)(normcntsscale->count ? normcntsscale->dval[0] : 0.0);
	double AbsNormCntsScale = NormCntsScale;
	if(AbsNormCntsScale < 0.0)
		AbsNormCntsScale *= -1.0;
	if(NormCntsScale != 0.0 && (AbsNormCntsScale < 0.1 || AbsNormCntsScale >= 10.0))
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: Manual override counts normalisation scale factor '-m%f' specified outside of range +/- %f..%f",NormCntsScale,0.1,10.0);
		exit(1);
		}

	for(NumInputControlSpecs=Idx=0;NumInputControlSpecs < cMaxInFileSpecs && Idx < incontrolfiles->count; Idx++)
		{
		pszInControlFiles[Idx] = NULL;
		if(pszInControlFiles[NumInputControlSpecs] == NULL)
			pszInControlFiles[NumInputControlSpecs] = new char [_MAX_PATH];
		strncpy(pszInControlFiles[NumInputControlSpecs],incontrolfiles->filename[Idx],_MAX_PATH);
		pszInControlFiles[NumInputControlSpecs][_MAX_PATH-1] = '\0';
		CUtility::TrimQuotedWhitespcExtd(pszInControlFiles[NumInputControlSpecs]);
		if(pszInControlFiles[NumInputControlSpecs][0] != '\0')
			NumInputControlSpecs++;
		}

	if(!NumInputControlSpecs)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace, no input file(s) specified with '-i<filespec>' option)\n");
		exit(1);
		}

	for(NumInputExperimentSpecs=Idx=0;NumInputExperimentSpecs < cMaxInFileSpecs && Idx < inexperfiles->count; Idx++)
		{
		pszInExperimentFiles[Idx] = NULL;
		if(pszInExperimentFiles[NumInputExperimentSpecs] == NULL)
			pszInExperimentFiles[NumInputExperimentSpecs] = new char [_MAX_PATH];
		strncpy(pszInExperimentFiles[NumInputExperimentSpecs],inexperfiles->filename[Idx],_MAX_PATH);
		pszInExperimentFiles[NumInputExperimentSpecs][_MAX_PATH-1] = '\0';
		CUtility::TrimQuotedWhitespcExtd(pszInExperimentFiles[NumInputExperimentSpecs]);
		if(pszInExperimentFiles[NumInputExperimentSpecs][0] != '\0')
			NumInputExperimentSpecs++;
		}

	if(!NumInputExperimentSpecs)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Error: After removal of whitespace, no input file(s) specified with '-I<filespec>' option)\n");
		exit(1);
		}


	strcpy(szInFeatFile,infeatfile->filename[0]);			// input genes or features BED file
	strcpy(szOutfile,outfile->filename[0]);					// output file

	if(bincountsfile->count > 0)
		strcpy(szBinCountsFile,bincountsfile->filename[0]);
	else
		szBinCountsFile[0] = '\0';


	if(featclass->count > 0)
		strcpy(szFeatClassFile,featclass->filename[0]);
	else
		szFeatClassFile[0] = '\0';

	if(excludezones->count > 0)
		strcpy(szExclZonesFile,excludezones->filename[0]);
	else
		szExclZonesFile[0] = '\0';

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Processing parameters:");

	const char *pszProcMode;
	switch(PMode) {
		case ePMdefault:
			pszProcMode = "Standard sensitivity";
			break;
		case ePMMoreSens:
			pszProcMode = "More sensitive (slower)";
			break;
		case ePMUltraSens:
			pszProcMode = "Ultra sensitive (very slow)";
			break;
		case ePMLessSens:
		default:
			pszProcMode = "Less sensitive (quicker)";
			break;
		}
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Processing mode: '%s'",pszProcMode);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Report to include features to which no reads align: '%s'",bFiltNonaligned ? "No" : "Yes");

	if(LimitAligned > 0)
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"Process at most this number of aligned reads: %d",LimitAligned);
	else
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"Process at most this number of aligned reads: No Limit");

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Process aligned reads strand: '%c'",ReportStrand(AlignStrand));

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Process gene or feature strand: '%c'",ReportStrand(FeatStrand));

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Process cnts in region: %s",Region2Txt((etBEDRegion)Region));

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"artifact loci read count reduction threshold: %d",ArtifactCntsThres);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Counts coalescing window length: %d",CoWinLen);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum feature count threshold: %d",MinFeatCntThres);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Minimum feature unique start loci threshold: %d",MinStartLociThres);

	gDiagnostics.DiagOutMsgOnly(eDLInfo,"Bin cnts into this many non-overlapping bins: %d",NumBins);

	if(NormCntsScale == 0.0)
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"Control counts normalisation scale factor: Auto");
	else
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"Control counts normalisation scale factor: %0.3f",NormCntsScale);

		switch(FType) {
		case 0:
			gDiagnostics.DiagOutMsgOnly(eDLInfo,"Auto-classify input element file as either CSV, BED or SAM");
			break;

		case 1:
			gDiagnostics.DiagOutMsgOnly(eDLInfo,"Expecting input element file to be CSV format");
			break;

		case 2:
			gDiagnostics.DiagOutMsgOnly(eDLInfo,"Expecting input element file to be BED format");
			break;

		case 3:
			gDiagnostics.DiagOutMsgOnly(eDLInfo,"Expecting input element file to be SAM format");
			break;
		}

	for(Idx=0; Idx < NumInputControlSpecs; Idx++)
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"input control aligned reads file (%d): '%s'",Idx+1,pszInControlFiles[Idx]);
	for(Idx=0; Idx < NumInputExperimentSpecs; Idx++)
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"input experiment aligned reads file (%d): '%s'",Idx+1,pszInExperimentFiles[Idx]);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"input gene or feature BED file: '%s'", szInFeatFile);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"output file to create: '%s'", szOutfile);
	if(szBinCountsFile[0] != '\0')
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"write transcript bin counts to file: '%s'", szBinCountsFile);
	if(szFeatClassFile[0] != '\0')
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"classify genes or features in this file: '%s'", szFeatClassFile);
	if(szExclZonesFile[0] != '\0')
		gDiagnostics.DiagOutMsgOnly(eDLInfo,"Do not process reads overlaying zone loci defined in this CSV file: '%s'", szExclZonesFile);
	gDiagnostics.DiagOutMsgOnly(eDLInfo,"number of threads : %d",NumThreads);
#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
	gStopWatch.Start();
	Rslt = Process(PMode,						// processing mode
					NumThreads,					// number of threads (0 defaults to number of CPUs)
					CoWinLen,					// counts coalescing window length
					ArtifactCntsThres,			// if counts at any loci are >= this then process for PCR artifact reduction
					LimitAligned,				// for test/evaluation can limit number of reads parsed to be no more than this number (0 for no limit)
					bFiltNonaligned,			// true if only features having at least one read aligned are to be be reported
					ReportStrand(AlignStrand),	// process for reads on this strand only
					ReportStrand(FeatStrand),	// process for genes or features on this strand only
					Region,						// which genomic region is to be processed
					NumBins,					// number of non-overlapping count bins
					MinFeatCntThres,			// minimum feature count threshold, control + experiment, required (1 to 200, defaults to 20)
					MinStartLociThres,				// minimum feature unique start loci threshold, control or experiment, required (1 to 200, defaults to 10)
					NormCntsScale,				// counts normalisation scale factor
					FType,						// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
					NumInputControlSpecs,		// number of input file specs
					pszInControlFiles,			// input control aligned reads files
					NumInputExperimentSpecs,	// number of input file specs
					pszInExperimentFiles,		// input experiment aligned reads files
					szInFeatFile,				// input gene or feature BED file
					szFeatClassFile,			// classify genes or features from this file
					szExclZonesFile,			// exclude from processing reads overlaying zone loci defined in this file
					szOutfile,					// output into this file
					szBinCountsFile);			// output bin counts to this file
	gStopWatch.Stop();
	Rslt = Rslt >=0 ? 0 : 1;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Exit code: %d Total processing time: %s",Rslt,gStopWatch.Read());
	exit(Rslt);
	}
else
	{
	printf("\n%s Kanga differential expression analyser, Version %s\n",gszProcName,cpszProgVer);
	arg_print_errors(stdout,end,gszProcName);
	arg_print_syntax(stdout,argtable,"\nUse '-h' to view option and parameter usage\n");
	exit(1);
	}
return 0;
}

char *
Region2Txt(etBEDRegion Region)
{
switch(Region) {
	case eMEGRAny:		// process any region
		return((char *)"All except Intergenic");

	case eMEGRExons:	// only process exons
		return((char *)"EXONS");

	case eMEGRIntrons:	// only process introns
		return((char *)"INTRONS");

	case eMEGRCDS:		// only process CDSs
		return((char *)"CDS");

	case eMEGUTR:		// only process UTRs
		return((char *)"UTR");

	case eMEG5UTR:		// only process 5'UTRs
		return((char *)"5'UTR");

	case eMEG3UTR:		// only process 3'UTRs
		return((char *)"3'UTR");

	default:
		break;
	}
return((char *)"Unsupported");
}


char
ReportStrand(etStrandProc StrandProc)
{
static char Strand;

switch(StrandProc) {
	case eStrandDflt:
		Strand = '*';
		break;
	case eStrandWatson:
		Strand = '+';
		break;
	case eStrandCrick:
		Strand = '-';
		break;
	}
return(Strand);
}

const int cDataBuffAlloc = 0x0fffffff;		// allocation size to hold gene features
bool m_bDEMutexesCreated = false;				// true if mutexes for serialisation have been created
int m_NumDEThreads;							// number of processing threads
int m_MaxConfidenceIterations;				// max number of iterations over start loci when inducing random poisson noise
tsThreadInstData *m_pThreadsInstData;		// all allocated thread instance data


etPMode m_DEPMode;					// processing mode
etProcPhase m_ProcessingPhase;		// current processing phase
bool m_bFiltNonaligned;				// true if only features having at least one read aligned are to be be reported

int m_NumExclZones;					// total number of read exclusion zones loaded
int m_NumExclReads;					// total number of reads which were excluded because they overlaid an exclusion zone
tsExclZone *m_pExclZones;			// pts to exclusion zones to which overlaying reads are to be excluded from processing

UINT32 m_LimitAligned;				// for test/evaluation can limit number of reads parsed to be no more than this number (0 for no limit)
int m_CoWinLen;						// counts coalescing window length

tsAlignReadLoci *m_pCtrlAlignReadLoci = NULL; // memory allocated to hold control read alignment loci, reads are written contiguously into this memory
UINT32 m_AllocdCtrlAlignReadsLoci;			  // how instances of control tsAlignReadLoci have been allocated
UINT32 m_CurNumCtrlAlignReadsLoci;			  // m_pAlignReadLoci currently contains a total of this many control read alignment loci

tsAlignReadLoci *m_pExprAlignReadLoci = NULL;	// memory allocated to hold experiment read alignment loci, reads are written contiguously into this memory
UINT32 m_AllocdExprAlignReadsLoci;	// how instances of experiment tsAlignReadLoci have been allocated
UINT32 m_CurNumExprAlignReadsLoci;	// m_pAlignReadLoci currently contains a total of this many experiment read alignment loci

UINT32 m_NumLoadedCtrlReads;			// total number of control reads actually loaded prior to any  coalescing and library size normalisation
UINT32 m_NumLoadedExprReads;			// total number of expression reads loaded loaded prior to any coalescing and library size normalisation

UINT32 m_NumNormCtrlReads;			// total number of control reads after library size normalisation
UINT32 m_NumNormExprReads;			// total number of expression after library size normalisation

UINT32 m_NumPoissonNormCtrlReads;		// total number of control reads after poisson noise
UINT32 m_NumPoissonNormExprReads;		// total number of expression after after poisson noise

UINT32 m_NumFeaturesLoaded;				// total number of features loaded for processing
UINT32 m_NumFeaturesProcessed;			// current number of features processed

tsAlignBin *m_pAlignBins = NULL;		// memory allocated to hold alignment bin cnts
UINT32 m_AllocdAlignBins = 0;			// how instances of tsAlignBin have been allocated
tsAlignLociInstStarts *m_pBinInstsStarts;	// pts to list of control and experiment start instance counts for all bins
UINT32 m_AllocBinInstStarts = 0;			// m_pBinInstsCnts is currently allocated to hold at most this number of start loci instance counts

int m_FeatsPerThread;					// number of features to be processed as a block by each thread

double *m_pPearsons;					// to hold Pearsons whilst determining confidence interval
size_t m_AllocNumPearsons;				// number allocated


tsThreadInstData *m_pThreadInst;		// pts to current thread instance

char m_DEAlignStrand;				// process for reads on this strand only
char m_FeatStrand;					// process for genes or features on this strand only
etBEDRegion m_Region;				// process for this genomic region only
int m_NumBins;						// Bin regions into this many non-overlapping bins

double m_LibSizeNormExpToCtrl;		// factor by which experiment counts can be normalised to that of control counts (accounts for library size ratio)

CBEDfile *m_pBEDFeatFile;

size_t m_AllocBinWrtBuff = 0;		// memory allocated for output bin write buffers
size_t m_AllocStatsWrtBuff = 0;		// memory allocated for output stats write buffers
size_t m_WrtBinBuffOfs = 0;			// offset at which to next write
size_t m_WrtStatsBuffOfs = 0;		// offset at which to next write
UINT8 *m_pWrtBinBuff;				// used to buffer output writes to bin counts file
UINT8 *m_pWrtStatsBuff;				// used to buffer output writes to stats file
int m_hOutStatsFile;				// output stats results file
bool m_bWrtStatHdr;					// true if stats file requires header
int m_hOutBinFile;					// output bin counts file
bool m_bWrtBinHdr;					// true if bin counts file requires header row

double *m_pFeatFoldChanges;			// to hold feature fold changes (Ctrl + 0.001)/(Expr + 0.001)) whilst determining feature confidence interval
size_t m_AllocNumFeatFoldChanges;	// number allocated

double *m_pPValues;					// to hold feature PValues
size_t m_AllocNumPValues;			// number allocated

tsAlignBin *m_pPoissonAlignBins;	 // to hold bin poisson counts whilst determining pearson confidence interval
size_t m_AllocNumPoissonCnts;		 // number allocated

CSimpleRNG m_SimpleRNG;				// used to generate Poisson distributed random cnts

int m_NumFeatsDEd;					// number of features processed into tsFeatDE's
int m_AllocdFeatsDEd;				// number alloc'd
tsFeatDE *m_pFeatDEs;				// allocated to hold features processed for DE

int m_NumNoneDEd;					// number of features which are candidates for determining as being none DE'd in phase ePPAllocDEmem

int m_MinFeatCntThres;				// minimum feature count threshold, control or experiment, required (1 to 200, defaults to 20)
int m_MinStartLociThres;			// minimum feature unique start loci threshold, control or experiment, required (1 to 200, defaults to 10)


tsRefIDChrom *m_pChroms = NULL;
int m_NumChromsAllocd = 0;
int m_CurNumChroms = 0;
int m_MRAChromID = 0;

int m_LastFeatureAllocProc = 0;					// set to the last FeatureID allocated to a thread for processing, 0 if starting feature allocation, -1 if all features have been allocated


CMTqsort m_mtqsort;					// multithreaded quicksort


const int cPossion1SeqLen = 10000;
const int cPossion2SeqLen = 20000;
const int cPossion3SeqLen = 40000;
const int cPossion4SeqLen = 80000;
const int cPossion5SeqLen = 100000;
const int cPossion6SeqLen = 200000;
const int cPossion7SeqLen = 400000;
const int cPossion8SeqLen = 800000;
const int cPossion9SeqLen = 1000000;
const int cPossion10SeqLen = 2000000;

int m_Possion1[cPossion1SeqLen];
int m_Possion2[cPossion2SeqLen];
int m_Possion3[cPossion3SeqLen];
int m_Possion4[cPossion4SeqLen];
int m_Possion5[cPossion5SeqLen];
int m_Possion6[cPossion6SeqLen];
int m_Possion7[cPossion7SeqLen];
int m_Possion8[cPossion8SeqLen];
int m_Possion9[cPossion9SeqLen];
int m_Possion10[cPossion10SeqLen];

// GenPoissonSeqs
void
InitPoissonSeqs(tsThreadInstData *pThreadInst)
{
int Idx;
int Len;
int Lambda;
int *pPoisson;
for(Lambda = 1; Lambda <= 10; Lambda++)
	{
	switch(Lambda) {
		case 1:
			Len = cPossion1SeqLen;
			pPoisson = m_Possion1;
			break;
		case 2:
			Len = cPossion2SeqLen;
			pPoisson = m_Possion2;
			break;
		case 3:
			Len = cPossion3SeqLen;
			pPoisson = m_Possion3;
			break;
		case 4:
			Len = cPossion4SeqLen;
			pPoisson = m_Possion4;
			break;
		case 5:
			Len = cPossion5SeqLen;
			pPoisson = m_Possion5;
			break;
		case 6:
			Len = cPossion6SeqLen;
			pPoisson = m_Possion6;
			break;
		case 7:
			Len = cPossion7SeqLen;
			pPoisson = m_Possion7;
			break;
		case 8:
			Len = cPossion8SeqLen;
			pPoisson = m_Possion8;
			break;
		case 9:
			Len = cPossion9SeqLen;
			pPoisson = m_Possion9;
			break;
		case 10:
			Len = cPossion10SeqLen;
			pPoisson = m_Possion10;
			break;
		}
	for(Idx = 0; Idx < Len; Idx++)
		{
		if(pThreadInst == NULL)
			*pPoisson++ = m_SimpleRNG.GetPoisson(Lambda);
		else
			*pPoisson++ =  pThreadInst->pSimpleRNG->GetPoisson(Lambda);
		}
	}
}

int
RandPoisson(tsThreadInstData *pThreadInst,int Lambda)
{
UINT32 IdxPoisson;
int *pPoissons;
int Range;

if(Lambda == 0)
	return(0);

if(Lambda > 10)
	return(pThreadInst == NULL ? m_SimpleRNG.GetPoisson(Lambda) : pThreadInst->pSimpleRNG->GetPoisson(Lambda));

switch(Lambda) {
	case 1:
		pPoissons = m_Possion1;
		Range = cPossion1SeqLen;
		break;
	case 2:
		pPoissons = m_Possion2;
		Range = cPossion2SeqLen;
		break;
	case 3:
		pPoissons = m_Possion3;
		Range = cPossion3SeqLen;
		break;
	case 4:
		pPoissons = m_Possion4;
		Range = cPossion4SeqLen;
		break;
	case 5:
		pPoissons = m_Possion5;
		Range = cPossion5SeqLen;
		break;
	case 6:
		pPoissons = m_Possion6;
		Range = cPossion6SeqLen;
		break;
	case 7:
		pPoissons = m_Possion7;
		Range = cPossion7SeqLen;
		break;
	case 8:
		pPoissons = m_Possion8;
		Range = cPossion8SeqLen;
		break;
	case 9:
		pPoissons = m_Possion9;
		Range = cPossion9SeqLen;
		break;
	case 10:
		pPoissons = m_Possion10;
		Range = cPossion10SeqLen;
		break;
	}
IdxPoisson = pThreadInst == NULL ? m_SimpleRNG.GetUint() : pThreadInst->pSimpleRNG->GetUint();
return(pPoissons[IdxPoisson  % Range]);
}


int
DECreateMutexes(void)
{
if(m_bDEMutexesCreated)
	return(eBSFSuccess);

#ifdef _WIN32
if((m_hDEMtxIterReads = CreateMutex(NULL,false,NULL))==NULL)
	{
#else
if(pthread_mutex_init (&m_hDEMtxIterReads,NULL)!=0)
	{
#endif
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Fatal: unable to create mutex");
	return(eBSFerrInternal);
	}
m_bDEMutexesCreated = true;
return(eBSFSuccess);
}

void
DEDeleteMutexes(void)
{
if(!m_bDEMutexesCreated)
	return;
#ifdef _WIN32
CloseHandle(m_hDEMtxIterReads);
#else
pthread_mutex_destroy(&m_hDEMtxIterReads);
#endif
m_bDEMutexesCreated = false;
}

void
DEAcquireSerialise(void)
{
#ifdef _WIN32
WaitForSingleObject(m_hDEMtxIterReads,INFINITE);
#else
pthread_mutex_lock(&m_hDEMtxIterReads);
#endif
}

void
DEReleaseSerialise(void)
{
#ifdef _WIN32
ReleaseMutex(m_hDEMtxIterReads);
#else
pthread_mutex_unlock(&m_hDEMtxIterReads);
#endif
}
void
DEInit(void)
{
m_pThreadsInstData = NULL;
m_pCtrlAlignReadLoci = NULL;
m_pExprAlignReadLoci = NULL;
m_pAlignBins = NULL;
m_pBinInstsStarts = NULL;
m_pWrtBinBuff = NULL;
m_pWrtStatsBuff = NULL;
m_pChroms = NULL;
m_pBEDFeatFile = NULL;
m_pPoissonAlignBins = NULL;
m_pFeatFoldChanges = NULL;
m_pExclZones = NULL;
m_pPValues = NULL;
m_pPearsons = NULL;
m_pFeatDEs = NULL;
m_hOutStatsFile = -1;
m_hOutBinFile = -1;
m_bDEMutexesCreated = false;
DEReset();
InitPoissonSeqs(NULL);
}

void
DEReset(void)
{
if(m_hOutStatsFile != -1)
	{
#ifdef _WIN32
	_commit(m_hOutStatsFile);
#else
	fsync(m_hOutStatsFile);
#endif
	close(m_hOutStatsFile);
	m_hOutStatsFile = -1;
	}

if(m_hOutBinFile != -1)
	{
#ifdef _WIN32
	_commit(m_hOutBinFile);
#else
	fsync(m_hOutBinFile);
#endif
	close(m_hOutBinFile);
	m_hOutBinFile = -1;
	}

if(m_pFeatDEs != NULL)
	{
#ifdef _WIN32
	free(m_pFeatDEs);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pFeatDEs != MAP_FAILED)
		munmap(m_pFeatDEs,m_AllocdFeatsDEd * sizeof(tsFeatDE));
#endif
	m_pFeatDEs = NULL;
	}

if(m_pThreadsInstData != NULL)
	{
	delete m_pThreadsInstData;
	m_pThreadsInstData = NULL;
	}

if(m_pChroms != NULL)
	{
	delete m_pChroms;
	m_pChroms = NULL;
	}

if(m_pCtrlAlignReadLoci != NULL)
	{
#ifdef _WIN32
	free(m_pCtrlAlignReadLoci);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pCtrlAlignReadLoci != MAP_FAILED)
		munmap(m_pCtrlAlignReadLoci,m_AllocdCtrlAlignReadsLoci * sizeof(tsAlignReadLoci));
#endif
	m_pCtrlAlignReadLoci = NULL;
	}

if(m_pExprAlignReadLoci != NULL)
	{
#ifdef _WIN32
	free(m_pExprAlignReadLoci);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pExprAlignReadLoci != MAP_FAILED)
		munmap(m_pExprAlignReadLoci,m_AllocdExprAlignReadsLoci * sizeof(tsAlignReadLoci));
#endif
	m_pExprAlignReadLoci = NULL;
	}

if(m_pAlignBins != NULL)
	{
#ifdef _WIN32
	free(m_pAlignBins);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pAlignBins != MAP_FAILED)
		munmap(m_pAlignBins,m_AllocdAlignBins * sizeof(tsAlignBin));
#endif
	m_pAlignBins = NULL;
	}

if(m_pBinInstsStarts != NULL)
	{
#ifdef _WIN32
	free(m_pBinInstsStarts);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pBinInstsStarts != MAP_FAILED)
		munmap(m_pBinInstsStarts,m_AllocBinInstStarts * sizeof(tsAlignLociInstStarts));
#endif
	m_pBinInstsStarts = NULL;
	}

if(m_pPoissonAlignBins != NULL)
	{
#ifdef _WIN32
	free(m_pPoissonAlignBins);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pPoissonAlignBins != MAP_FAILED)
		munmap(m_pPoissonAlignBins,m_AllocNumPoissonCnts * sizeof(tsAlignBin));
#endif
	m_pPoissonAlignBins = NULL;
	}

if(m_pFeatFoldChanges != NULL)
	{
#ifdef _WIN32
	free(m_pFeatFoldChanges);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pFeatFoldChanges != MAP_FAILED)
		munmap(m_pFeatFoldChanges,m_AllocNumFeatFoldChanges * sizeof(double));
#endif
	m_pFeatFoldChanges = NULL;
	}

if(m_pPValues != NULL)
	{
#ifdef _WIN32
	free(m_pPValues);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pPValues != MAP_FAILED)
		munmap(m_pPValues,m_AllocNumPValues * sizeof(double));
#endif
	m_pPValues = NULL;
	}

if(m_pPearsons != NULL)
	{
#ifdef _WIN32
	free(m_pPearsons);				// was allocated with malloc/realloc, or mmap/mremap, not c++'s new....
#else
	if(m_pPearsons != MAP_FAILED)
		munmap(m_pPearsons,m_AllocNumPearsons * sizeof(double));
#endif
	m_pPearsons = NULL;
	}

if(m_pExclZones != NULL)
	{
	delete m_pExclZones;
	m_pExclZones = NULL;
	}

if(m_pBEDFeatFile != NULL)
	{
	delete m_pBEDFeatFile;
	m_pBEDFeatFile = NULL;
	}

if(m_pWrtBinBuff != NULL)
	{
	delete m_pWrtBinBuff;
	m_pWrtBinBuff = NULL;
	}

if(m_pWrtStatsBuff != NULL)
	{
	delete m_pWrtStatsBuff;
	m_pWrtStatsBuff = NULL;
	}

if(m_bDEMutexesCreated)
	DEDeleteMutexes();

m_NumExclZones = 0;
m_NumExclReads = 0;
m_bFiltNonaligned = false;
m_NumChromsAllocd = 0;
m_CurNumChroms = 0;
m_MRAChromID = 0;

m_AllocdCtrlAlignReadsLoci = 0;
m_CurNumCtrlAlignReadsLoci = 0;
m_AllocdExprAlignReadsLoci = 0;
m_CurNumExprAlignReadsLoci = 0;

m_AllocdAlignBins = 0;
m_AllocBinInstStarts = 0;

m_LimitAligned = 0;
m_LibSizeNormExpToCtrl = cNormCntsScale;
m_NumBins = 0;

m_AllocNumPoissonCnts = 0;
m_AllocNumPearsons = 0;
m_AllocNumPValues = 0;
m_AllocNumFeatFoldChanges = 0;

m_MinFeatCntThres = cDfltFeatCntThres;
m_MinStartLociThres = cDfltStartLociThres;
m_bWrtBinHdr = false;
m_bWrtStatHdr = false;
m_AllocBinWrtBuff = 0;
m_AllocStatsWrtBuff = 0;
m_WrtBinBuffOfs = 0;
m_WrtStatsBuffOfs = 0;
m_NumFeaturesProcessed = 0;
m_NumFeaturesLoaded = 0;
m_LastFeatureAllocProc = 0;
m_FeatsPerThread = 0;
m_NumFeatsDEd = 0;
m_AllocdFeatsDEd = 0;
m_MaxConfidenceIterations = cMaxConfidenceIterations;
}

// thread entry
int
ProcessFeature(tsThreadInstData *pThreadInst)
{
teBSFrsltCodes Rslt;
int RegionOfs;
int StartLoci;
int EndLoci;
char szChrom[128];
char szFeatName[128];
char Strand;
int IntergenicStart;
int	NumExons;
int	NumIntrons;
int	CDSstart;
int	CDSend;
int UserClass;
int Idx;

IntergenicStart = 0;
if((Rslt = m_pBEDFeatFile->GetFeature(pThreadInst->FeatureID,// feature instance identifier
			szFeatName,				// where to return feature name
			szChrom,				// where to return chromosome name
			&StartLoci,				// where to return feature start on chromosome (0..n)
			&EndLoci,				// where to return feature end on chromosome
			NULL,					// where to return score
			&Strand)) < eBSFSuccess)				// where to return strand
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Unexpected error returned from GetFeature: %d",Rslt);
	return(Rslt);
	}
UserClass = m_pBEDFeatFile->GetUserClass(pThreadInst->FeatureID);

if(m_FeatStrand != '*' && Strand != m_FeatStrand)
	return(eBSFSuccess);


pThreadInst->NumBinsWithLoci = 0;
pThreadInst->NumBinInstStarts = 0;
pThreadInst->CurFeatLen = 0;
NumExons = m_pBEDFeatFile->GetNumExons(pThreadInst->FeatureID);					// returns number of exons - includes UTRs + CDS
if(m_Region != eMEGRAny)
	{
	NumIntrons = m_pBEDFeatFile->GetNumIntrons(pThreadInst->FeatureID);
	CDSstart = StartLoci + m_pBEDFeatFile->GetCDSStart(pThreadInst->FeatureID);		// returns relative start offset of CDS - NOTE add to '+' strand gene start, subtract on '-' strand gene start
	CDSend = StartLoci + m_pBEDFeatFile->GetCDSEnd(pThreadInst->FeatureID);			// returns relative end offset of CDS - NOTE add to '+' strand gene start, subtract on '-' strand gene start
	}
Rslt = eBSFSuccess;

switch(m_Region) {
	case eMEGRAny:			// retain any region
		pThreadInst->CurRegionLen = m_pBEDFeatFile->GetFeatLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			Rslt=GenBinAlignStarts(pThreadInst,0,szChrom,StartLoci,EndLoci);
		break;

	case eMEGRExons:		// only retain exons
		pThreadInst->CurRegionLen = m_pBEDFeatFile->GetTranscribedLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			{
			RegionOfs = 0;
			for(Idx = 1; Idx <= NumExons; Idx++)
				{
				StartLoci = m_pBEDFeatFile->GetExonStart(pThreadInst->FeatureID,Idx);
				EndLoci   = m_pBEDFeatFile->GetExonEnd(pThreadInst->FeatureID,Idx);
				if(StartLoci <= EndLoci)
					{
					Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
					RegionOfs += (1 + EndLoci - StartLoci);
					}
				}
			}
		break;

	case eMEGRIntrons:		// only retain introns
		if(NumIntrons)
			{
			pThreadInst->CurRegionLen =  m_pBEDFeatFile->GetFeatLen(pThreadInst->FeatureID);
			pThreadInst->CurRegionLen -= m_pBEDFeatFile->GetTranscribedLen(pThreadInst->FeatureID);
			if(pThreadInst->CurRegionLen > 0)
				{
				RegionOfs = 0;
				for(Idx = 1; Idx <= NumIntrons; Idx++)
					{
					StartLoci = m_pBEDFeatFile->GetIntronStart(pThreadInst->FeatureID,Idx);
					EndLoci = m_pBEDFeatFile->GetIntronEnd(pThreadInst->FeatureID,Idx);
					if(StartLoci <= EndLoci)
						{
						Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
						RegionOfs += (1 + EndLoci - StartLoci);
						}
					}
				}
			}
		break;

	case eMEGRCDS:			// only retain CDSs
		pThreadInst->CurRegionLen = m_pBEDFeatFile->GetCDSLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			{
			RegionOfs = 0;
			for(Idx = 1; Idx <= NumExons; Idx++)
				{
				StartLoci = m_pBEDFeatFile->GetExonStart(pThreadInst->FeatureID,Idx);
				EndLoci   = m_pBEDFeatFile->GetExonEnd(pThreadInst->FeatureID,Idx);
				if(EndLoci < CDSstart || StartLoci > CDSend)
					continue;
				if(StartLoci < CDSstart)
					StartLoci = CDSstart;
				if(EndLoci > CDSend)
					EndLoci = CDSend;
				if(StartLoci <= EndLoci)
					{
					GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
					RegionOfs += (1 + EndLoci - StartLoci);
					}
				}
			}
		break;

	case eMEGUTR:			// only process UTRs - single exon may have both 5' and 3' UTRs
		pThreadInst->CurRegionLen = m_pBEDFeatFile->Get5UTRLen(pThreadInst->FeatureID);
		pThreadInst->CurRegionLen += m_pBEDFeatFile->Get3UTRLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			{
			RegionOfs = 0;
			for(Idx = 1; Idx <= NumExons; Idx++)
				{
				StartLoci = m_pBEDFeatFile->GetExonStart(pThreadInst->FeatureID,Idx);
				EndLoci   = m_pBEDFeatFile->GetExonEnd(pThreadInst->FeatureID,Idx);
				if(EndLoci <= CDSend && StartLoci >= CDSstart) // is exon CDS only?
					continue;

				// check if 5' UTR
				if(StartLoci < CDSstart)
					{
					if(EndLoci >= CDSstart)
						Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,CDSstart-1);
					else
						Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
					RegionOfs += (1 + EndLoci - StartLoci);
					}

				// check if 3'UTR
				if(EndLoci > CDSend)
					{
					if(StartLoci <= CDSend)
						StartLoci = CDSend+1;
					Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
					RegionOfs += (1 + EndLoci - StartLoci);
					}
				}
			}
		break;

	case eMEG5UTR:			// only process 5'UTRs - strand sensitive
		pThreadInst->CurRegionLen = m_pBEDFeatFile->Get5UTRLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			{
			RegionOfs = 0;
			for(Idx = 1; Idx <= NumExons; Idx++)
				{
				StartLoci = m_pBEDFeatFile->GetExonStart(pThreadInst->FeatureID,Idx);
				EndLoci   = m_pBEDFeatFile->GetExonEnd(pThreadInst->FeatureID,Idx);
				if(EndLoci <= CDSend && StartLoci >= CDSstart) // is exon CDS only?
					continue;

				if(Strand != '-')
					{
					// check if 5' UTR on '+' strand
					if(StartLoci < CDSstart)
						{
						if(EndLoci >= CDSstart)
							EndLoci = CDSstart - 1;
						}
					}
				else
					{
					// check if 5'UTR on '-' strand
					if(EndLoci > CDSend)
						{
						if(StartLoci <= CDSend)
							StartLoci = CDSend+1;
						}
					}
				Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
				RegionOfs += (1 + EndLoci - StartLoci);
				}
			}
		break;

	case eMEG3UTR:			// only process 3'UTRs  - strand sensitive
		pThreadInst->CurRegionLen = m_pBEDFeatFile->Get3UTRLen(pThreadInst->FeatureID);
		if(pThreadInst->CurRegionLen > 0)
			{
			RegionOfs = 0;
			for(Idx = 1; Idx <= NumExons; Idx++)
				{
				StartLoci = m_pBEDFeatFile->GetExonStart(pThreadInst->FeatureID,Idx);
				EndLoci   = m_pBEDFeatFile->GetExonEnd(pThreadInst->FeatureID,Idx);
				if(EndLoci <= CDSend && StartLoci >= CDSstart) // is exon CDS only?
					continue;

				if(Strand == '-')
					{
					// check if 3' UTR on '-' strand
					if(StartLoci < CDSstart)
						{
						if(EndLoci >= CDSstart)
							EndLoci = CDSstart - 1;
						}
					}
				else
					{
					// check if 3'UTR on '+' strand
					if(EndLoci > CDSend)
						{
						if(StartLoci <= CDSend)
							StartLoci = CDSend+1;
						}
					}
				Rslt=GenBinAlignStarts(pThreadInst,RegionOfs,szChrom,StartLoci,EndLoci);
				RegionOfs += (1 + EndLoci - StartLoci);
				}
			}
		break;

	}

// have finished counts for complete gene region
if(!m_bFiltNonaligned || pThreadInst->NumBinsWithLoci)
	{
	AddDEPearsons(pThreadInst,szFeatName,NumExons,UserClass);
	pThreadInst->CurFeatLen = 0;
	}
return(eBSFSuccess);
}

teBSFrsltCodes
ProcessReads4DE(void)
{
teBSFrsltCodes Rslt;
tsThreadInstData *pThreadInst;
double *pPearsons;
double *pPValues;
double *pFeatFoldChanges;
tsAlignBin *pAlignBins;
tsAlignBin *pPoissonAlignBins;
tsAlignLociInstStarts *pAlignLociInstStarts;
UINT32 ThreadInst;

m_NumFeatsDEd = 0;
m_LastFeatureAllocProc = 0;
m_NumFeaturesProcessed = 0;

pPearsons = m_pPearsons;
pPValues = m_pPValues;
pFeatFoldChanges = m_pFeatFoldChanges;
pAlignBins = m_pAlignBins;
pPoissonAlignBins = m_pPoissonAlignBins;
pAlignLociInstStarts = m_pBinInstsStarts;
pThreadInst = m_pThreadsInstData;
memset(m_pThreadsInstData,0,sizeof(tsThreadInstData) * m_NumDEThreads);
for(ThreadInst = 1; ThreadInst <= (UINT32)m_NumDEThreads; ThreadInst++,pThreadInst++,
										pPearsons += cMaxConfidenceIterations,
										pPValues += cMaxConfidenceIterations,
										pAlignBins += m_NumBins,
										pPoissonAlignBins += m_NumBins,
										pAlignLociInstStarts += cMaxAssumTransLoci,
										pFeatFoldChanges += cMaxConfidenceIterations)
	{
	pThreadInst->ThreadInst = ThreadInst;
	pThreadInst->pSimpleRNG = new CSimpleRNG();
	pThreadInst->pSimpleRNG->Reset();
	pThreadInst->pStats = new CStats();
	pThreadInst->pStats->Init();
    pThreadInst->MaxFeats2Proc = m_FeatsPerThread;
	pThreadInst->pPearsons = pPearsons;
	pThreadInst->pAlignBins = pAlignBins;
	pThreadInst->pPoissonAlignBins = pPoissonAlignBins;
	pThreadInst->pBinLociInstStarts = pAlignLociInstStarts;
	pThreadInst->pFeatFoldChanges = pFeatFoldChanges;
	pThreadInst->pPValues = pPValues;
#ifdef _WIN32
	pThreadInst->threadHandle = (HANDLE)_beginthreadex(NULL,0x0fffff,ThreadedDEproc,pThreadInst,0,&pThreadInst->threadID);
#else
	pThreadInst->threadRslt =	pthread_create (&pThreadInst->threadID, NULL , ThreadedDEproc , pThreadInst );
#endif
	}

Rslt = eBSFSuccess;
// iterate features
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Iterating features and processing alignments for differential expression...");

// allow threads a few seconds to startup
#ifdef _WIN32
	Sleep(5000);
#else
	sleep(5);
#endif


UINT32 NumFeaturesProcessed = 0;
// wait for all threads to have completed
pThreadInst = m_pThreadsInstData;
for(ThreadInst = 1;ThreadInst <= (UINT32)m_NumDEThreads; ThreadInst++,pThreadInst++)
	{
#ifdef _WIN32
	while(WAIT_TIMEOUT == WaitForSingleObject( pThreadInst->threadHandle, 60000))
		{
		// report on progress
		DEAcquireSerialise();
		NumFeaturesProcessed = m_NumFeaturesProcessed;
		DEReleaseSerialise();
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Progress: %u (%1.2f%%) features processed from %d loaded",NumFeaturesProcessed,(NumFeaturesProcessed * 100.0)/m_NumFeaturesLoaded,m_NumFeaturesLoaded);
		}
	CloseHandle( pThreadInst->threadHandle);
#else
	struct timespec ts;
	int JoinRlt;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 60;
	while((JoinRlt = pthread_timedjoin_np(pThreadInst->threadID, NULL, &ts)) != 0)
		{
		// report on progress
		DEAcquireSerialise();
		NumFeaturesProcessed = m_NumFeaturesProcessed;
		DEReleaseSerialise();
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Progress: %u (%1.2f%%) features processed from %d loaded",NumFeaturesProcessed,(NumFeaturesProcessed * 100.0)/m_NumFeaturesLoaded,m_NumFeaturesLoaded);
		ts.tv_sec += 60;
		}

#endif
	delete pThreadInst->pSimpleRNG;
	 pThreadInst->pSimpleRNG = NULL;
	 delete pThreadInst->pStats;
	 pThreadInst->pStats = NULL;

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Thread %d finished...", ThreadInst);
	if(pThreadInst->Rslt != eBSFSuccess)
		Rslt = pThreadInst->Rslt;
	}

return(Rslt);
}

// LoadGeneFeatClasses
// Loads gene or feature classification from a CSV or tab delimited file
// Expectation is that the file contains at least 2 columns
// Col1:  name of feature
// Col2:  numerical classification to associate with feature
//        0 -> unclassified
//        1 -> spikein
int
LoadGeneFeatClasses(char *pszFeatClassFile)
{
int Rslt;
int NumFields;
int NumElsRead;
char *pszFeatName;
int FeatClass;
int UnableToAssocCnt;

CCSVFile *pCSV = new CCSVFile;
if(pCSV == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CCSVfile");
	return(eBSFerrObj);
	}

if((Rslt=pCSV->Open(pszFeatClassFile))!=eBSFSuccess)
	{
	while(pCSV->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,pCSV->GetErrMsg());
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open file: %s",pszFeatClassFile);
	delete pCSV;
	return(Rslt);
	}
NumElsRead = 0;
UnableToAssocCnt = 0;
while((Rslt=pCSV->NextLine()) > 0)	// onto next line containing fields
	{
	NumFields = pCSV->GetCurFields();
	if(NumFields < 2)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Expected at least 2 fields in '%s', GetCurFields() returned '%d'",pszFeatClassFile,NumFields);
		delete pCSV;
		return(eBSFerrFieldCnt);
		}

	if(!NumElsRead && pCSV->IsLikelyHeaderLine())
		continue;
	NumElsRead += 1;
	pCSV->GetText(1,&pszFeatName);
	pCSV->GetInt(2,&FeatClass);
	FeatClass &= 0x0ffffff;
	if((Rslt = m_pBEDFeatFile->SetUserClass(pszFeatName,FeatClass))!=eBSFSuccess)
		{
		if(UnableToAssocCnt++ < 10)
			gDiagnostics.DiagOut(eDLWarn,gszProcName,"Unable to locate feature '%s' in bed file'",pszFeatName);
		continue;
		}
	}
delete pCSV;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Loaded %d feature classifications, accepted %d but unable to associate %d",NumElsRead, NumElsRead-UnableToAssocCnt,UnableToAssocCnt);
return(eBSFSuccess);
}


// LoadExclZones
// Loads read start/end loci for which any covering reads are to be excluded
// These regions are expected to be defined as chrom, start, end, with an optional strand
int
LoadExclZones(char *pszExclZonesFile)
{
int Rslt;
int NumFields;
int NumElsRead;
char *pszFeatName;
int ChromID;
int StartLoci;
int EndLoci;
char Strand;
char *pszStrand;
int UnableToAssocCnt;
tsExclZone *pExclZone;

CCSVFile *pCSV = new CCSVFile;
if(pCSV == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CCSVfile");
	return(eBSFerrObj);
	}

if((Rslt=pCSV->Open(pszExclZonesFile))!=eBSFSuccess)
	{
	while(pCSV->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,pCSV->GetErrMsg());
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open file: %s",pszExclZonesFile);
	delete pCSV;
	return(Rslt);
	}

if(m_pExclZones == NULL)
	m_pExclZones = new tsExclZone [cMaxExclZones];
m_NumExclZones = 0;
m_NumExclReads = 0;
pExclZone = m_pExclZones;			// pts to features which are to be excluded

NumElsRead = 0;
UnableToAssocCnt = 0;
while((Rslt=pCSV->NextLine()) > 0)	// onto next line containing fields
	{
	NumFields = pCSV->GetCurFields();
	if(NumFields < 3)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Expected at least 3 fields (chrom,start,end and optional strand) in '%s', GetCurFields() returned '%d'",pszExclZonesFile,NumFields);
		delete pCSV;
		return(eBSFerrFieldCnt);
		}

	if(!NumElsRead && pCSV->IsLikelyHeaderLine())
		continue;
	NumElsRead += 1;
	if(m_NumExclZones >= cMaxExclZones)
		{
		gDiagnostics.DiagOut(eDLWarn,gszProcName,"Reached limit of %d zone exclusions, sloughing any additional exclusions",cMaxExclZones);
		break;
		}

	pCSV->GetText(1,&pszFeatName);
	pCSV->GetInt(2,&StartLoci);
	pCSV->GetInt(3,&EndLoci);
	if(NumFields > 3)
		{
		pCSV->GetText(4,&pszStrand);
		Strand = *pszStrand;
		}
	else
		Strand = '*';

	ChromID = ChromToID(pszFeatName);
	pExclZone = &m_pExclZones[m_NumExclZones++];			// add this exclusion zone
	pExclZone->ChromID = ChromID;
	pExclZone->StartLoci = StartLoci;
	pExclZone->EndLoci = EndLoci;
	pExclZone->Strand = Strand;
	}
delete pCSV;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Loaded %d exclusion zones",m_NumExclZones);
return(eBSFSuccess);
}


teBSFrsltCodes
Process(etPMode PMode,									// processing mode
					int NumThreads,						// number of threads (0 defaults to number of CPUs)
					int CoWinLen,						// counts coalescing window length
					int ArtifactCntsThres,				// if counts at any loci are >= this threshold then process for PCR artifact reduction
					UINT32 LimitAligned,				// for test/evaluation can limit number of reads parsed to be no more than this number (0 for no limit)
					bool bFiltNonaligned,				// true if only features having at least one read aligned are to be be reported
					char AlignStrand,					// process for reads on this strand only
					char FeatStrand,					// process for genes or features on this strand only
					etBEDRegion Region,					// process for this genomic region only
					int	NumBins,						// number of non-overlapping count bins
					int MinFeatCntThres,				// minimum feature count threshold, control or experiment, required (1 to 200, defaults to 20)
					int MinStartLociThres,				// minimum feature unique start loci threshold, control or experiment, required (1 to 200, defaults to 10)
					double NormCntsScale,				// manual override counts normalisation scale factor
					int FType,							// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
					int NumInputControlSpecs,			// number of input file specs
					char **pszInControlFiles,			// input control aligned reads files
					int NumInputExperimentSpecs,		// number of input file specs
					char **pszInExperimentFiles,		// input experiment aligned reads files
					char *pszInFeatFile,				// input gene or feature BED file
					char *pszFeatClassFile,				// classify genes or features from this file
					char *pszExclZonesFile,			// exclude from processing genes and features from this file
					char *pszOutfile,					// output into this file
					char *pszBinCountsFile)				// output bin counts to this file
{
teBSFrsltCodes Rslt;
int ArtifactFlankLen;

DEInit();
m_DEPMode = PMode;
m_NumDEThreads = NumThreads;
m_LibSizeNormExpToCtrl = NormCntsScale;
m_NumBins = NumBins;
m_LimitAligned = LimitAligned;
m_bFiltNonaligned = bFiltNonaligned;
m_CoWinLen = CoWinLen;
m_MinFeatCntThres = MinFeatCntThres;
m_MinStartLociThres = MinStartLociThres;

m_FeatStrand = FeatStrand;
m_DEAlignStrand = AlignStrand;
m_Region = Region;
m_LastFeatureAllocProc = 0;
m_ProcessingPhase = ePPInit;

switch(PMode) {
	case ePMdefault:				// Standard sensitivity (2500 iterations)
		m_MaxConfidenceIterations = cMaxConfidenceIterations/4;
		break;
	case ePMMoreSens:				// More sensitive (slower 5000 iterations)
		m_MaxConfidenceIterations = cMaxConfidenceIterations/2;
		break;
	case ePMUltraSens:				// Ultra sensitive (very slow 10000 iterations)
		m_MaxConfidenceIterations = cMaxConfidenceIterations;
		break;
	case ePMLessSens:				// Less sensitive (quicker 1000 iterations)
		m_MaxConfidenceIterations = cMaxConfidenceIterations/10;
		break;
	}


#ifdef _WIN32
if((m_hOutStatsFile = open(pszOutfile, _O_RDWR | _O_BINARY | _O_SEQUENTIAL | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE ))==-1)
#else
if((m_hOutStatsFile = open(pszOutfile, O_RDWR | O_CREAT |O_TRUNC, S_IREAD | S_IWRITE))==-1)
#endif
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to create or truncate %s - %s",pszOutfile,strerror(errno));
	DEReset();
	return(eBSFerrCreateFile);
	}
m_bWrtStatHdr = true;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Output results file created/truncated: '%s'",pszOutfile);
if((m_pWrtStatsBuff = new UINT8 [cWrtStatBuffSize])==NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate %d bytes for output write buffering",cWrtStatBuffSize);
	DEReset();
	return(eBSFerrMem);
	}
m_AllocStatsWrtBuff = cWrtStatBuffSize;
m_WrtStatsBuffOfs = 0;

if(pszBinCountsFile != NULL && pszBinCountsFile[0] != '\0')
	{
#ifdef _WIN32
	if((m_hOutBinFile = open(pszBinCountsFile, _O_RDWR | _O_BINARY | _O_SEQUENTIAL | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE ))==-1)
#else
	if((m_hOutBinFile = open(pszBinCountsFile, O_RDWR | O_CREAT |O_TRUNC, S_IREAD | S_IWRITE))==-1)
#endif
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to create or truncate %s - %s",pszBinCountsFile,strerror(errno));
		DEReset();
		return(eBSFerrCreateFile);
		}

	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Output bin counts file created/truncated: '%s'",pszBinCountsFile);
	m_bWrtBinHdr = true;
	if((m_pWrtBinBuff = new UINT8 [cWrtBinBuffSize])==NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to allocate %d bytes for output write buffering",cWrtBinBuffSize);
		DEReset();
		return(eBSFerrMem);
		}
	m_AllocBinWrtBuff = cWrtBinBuffSize;
	m_WrtBinBuffOfs = 0;
	}
else
	{
	m_hOutBinFile = -1;
	m_pWrtBinBuff = NULL;
	m_AllocBinWrtBuff = 0;
	}


// load the gene or feature BED file
m_ProcessingPhase = ePPLoadFeatures;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Loading gene features from '%s'...",pszInFeatFile);
if((Rslt = (teBSFrsltCodes)LoadGeneFeatures(FeatStrand,pszInFeatFile))!=eBSFSuccess)
	{
	DEReset();
	return(Rslt);
	}
m_NumFeaturesLoaded = m_pBEDFeatFile->GetNumFeatures();
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed loading %d features",m_NumFeaturesLoaded);
m_pBEDFeatFile->InitUserClass(0);		// reset any previously existing user classifications back to the indeterminate state

if(pszFeatClassFile != NULL && pszFeatClassFile[0] != '\0')
	{
	m_ProcessingPhase = ePPLoadFeatClass;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Loading gene feature classifications from '%s'...",pszFeatClassFile);
	if((Rslt = (teBSFrsltCodes)LoadGeneFeatClasses(pszFeatClassFile))!=eBSFSuccess)
		{
		DEReset();
		return(Rslt);
		}
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed loading gene feature classifications");
	}

if(pszExclZonesFile != NULL && pszExclZonesFile[0] != '\0')
	{
	m_ProcessingPhase = ePPLoadExclZones;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Loading read exclusion zone loci from '%s'...",pszExclZonesFile);
	if((Rslt = (teBSFrsltCodes)LoadExclZones(pszExclZonesFile))!=eBSFSuccess)
		{
		DEReset();
		return(Rslt);
		}
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed loading read exclusion zone loci");
	}

m_ProcessingPhase = ePPLoadReads;

// load the aligned reads for both control and experiment
if((Rslt = (teBSFrsltCodes)LoadAlignedReadFiles(AlignStrand,FType,NumInputControlSpecs,pszInControlFiles,NumInputExperimentSpecs,pszInExperimentFiles)) < eBSFSuccess)
	{
	DEReset();
	return(Rslt);
	}

// alignment loci have been sorted, iterate each loci and if identical to next then can be collapsed into a single loci having multiple reads aligning to that loci
// when collapsed then can normalise the loci counts
m_ProcessingPhase = ePPCoalesceReads;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Starting to coalese read alignments with window size %d, there are %d control reads and %u experiment reads",CoWinLen,m_CurNumCtrlAlignReadsLoci,m_CurNumExprAlignReadsLoci);
if((Rslt = (teBSFrsltCodes)CoalesceReadAlignments(CoWinLen,false,false))!=eBSFSuccess)
	{
	DEReset();
	return(Rslt);
	}
if((Rslt = (teBSFrsltCodes)CoalesceReadAlignments(CoWinLen,false,true))!=eBSFSuccess)
	{
	DEReset();
	return(Rslt);
	}
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed coalesence, there are %d unique control loci sites and %u unique experiment loci sites",m_CurNumCtrlAlignReadsLoci,m_CurNumExprAlignReadsLoci);

if(ArtifactCntsThres > 0)
	{
	ArtifactFlankLen = 50;
	m_ProcessingPhase = ePPReducePCRartifacts;
	Rslt = ReducePCRartifacts(ArtifactFlankLen,ArtifactCntsThres);
	}

m_ProcessingPhase = ePPNormLibCnts;
Rslt = NormaliseLibraryCounts();
if(Rslt < eBSFSuccess)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Normalisation of library counts failed");
	DEReset();
	return(Rslt);
	}

m_ProcessingPhase = ePPAllocDEmem;
// each thread has it's own data instance containing the variables it is currently processing/generating
if((m_pThreadsInstData = new tsThreadInstData [NumThreads])==NULL)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"Failed allocating memory for thread instance data");
	DEReset();
	return(eBSFerrMem);
	}


// calc number of bins etc required to be allocated per thread
size_t memreq;
memreq = m_NumBins * sizeof(tsAlignBin) * NumThreads;
#ifdef _WIN32
m_pAlignBins = (tsAlignBin *) malloc((size_t)memreq);
if(m_pAlignBins == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bins failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pAlignBins = (tsAlignBin *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pAlignBins == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bins through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pAlignBins = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocdAlignBins = m_NumBins * NumThreads;


memreq = m_NumBins * sizeof(tsAlignBin) * NumThreads;
#ifdef _WIN32
m_pPoissonAlignBins = (tsAlignBin *) malloc((size_t)memreq);
if(m_pPoissonAlignBins == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bins failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pPoissonAlignBins = (tsAlignBin *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pPoissonAlignBins == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bins through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pPoissonAlignBins = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocNumPoissonCnts = m_NumBins * NumThreads;


memreq = cMaxAssumTransLoci * NumThreads * sizeof(tsAlignLociInstStarts);
#ifdef _WIN32
m_pBinInstsStarts = (tsAlignLociInstStarts *) malloc((size_t)memreq);
if(m_pBinInstsStarts == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bin loci instances failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pBinInstsStarts = (tsAlignLociInstStarts *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pBinInstsStarts == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for bin loci instances through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pBinInstsStarts = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocBinInstStarts = cMaxAssumTransLoci * NumThreads;


memreq = cMaxConfidenceIterations * NumThreads * sizeof(double);
#ifdef _WIN32
m_pFeatFoldChanges = (double *) malloc((size_t)memreq);
if(m_pFeatFoldChanges == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for fold change instances failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pFeatFoldChanges = (double *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pFeatFoldChanges == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for fold change instances through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pFeatFoldChanges = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocNumFeatFoldChanges = cMaxConfidenceIterations * NumThreads;

memreq = cMaxConfidenceIterations * NumThreads * sizeof(double);
#ifdef _WIN32
m_pPValues = (double *) malloc((size_t)memreq);
if(m_pPValues == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for PValue instances failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pPValues = (double *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pPValues == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for PValue instances through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pPValues = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocNumPValues = cMaxConfidenceIterations * NumThreads;

memreq = cMaxConfidenceIterations * NumThreads * sizeof(double);
#ifdef _WIN32
m_pPearsons = (double *) malloc((size_t)memreq);
if(m_pPearsons == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for fold change pearsons instances failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pPearsons = (double *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pFeatFoldChanges == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for fold change instances through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pFeatFoldChanges = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocNumPearsons = cMaxConfidenceIterations * NumThreads;

memreq = (m_NumFeaturesLoaded + 1) * sizeof(tsFeatDE);
#ifdef _WIN32
m_pFeatDEs = (tsFeatDE *) malloc((size_t)memreq);
if(m_pFeatDEs == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for processed feature instances failed",(INT64)memreq);
	DEReset();
	return(eBSFerrMem);
	}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
m_pFeatDEs = (tsFeatDE *)mmap(NULL,memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
if(m_pFeatDEs == MAP_FAILED)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Process: Memory allocation of %lld bytes for processed feature instances through mmap()  failed",(INT64)memreq,strerror(errno));
	m_pFeatDEs = NULL;
	DEReset();
	return(eBSFerrMem);
	}
#endif
m_AllocdFeatsDEd = m_NumFeaturesLoaded + 1;
m_NumFeatsDEd = 0;

DECreateMutexes();

#ifdef _DEBUG
#ifdef _WIN32
_ASSERTE( _CrtCheckMemory());
#endif
#endif

// now initialise thread instance data

if(m_NumFeaturesLoaded < (UINT32)NumThreads)
	m_NumDEThreads = m_NumFeaturesLoaded;
m_FeatsPerThread = min(cMaxFeats2ProcAlloc,m_NumFeaturesLoaded/m_NumDEThreads);


m_ProcessingPhase = ePPDDd;
ProcessReads4DE();

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Feature processing completed");

// report to file
m_ProcessingPhase = ePPReport;
ReportDEandPearsons();
ReportDEandPearsonBinCounts();


if(m_hOutStatsFile != -1)
	{
	if(m_WrtStatsBuffOfs > 0)
		{
		CUtility::SafeWrite(m_hOutStatsFile,m_pWrtStatsBuff,m_WrtStatsBuffOfs);
		m_WrtStatsBuffOfs = 0;
		}
#ifdef _WIN32
	_commit(m_hOutStatsFile);
#else
	fsync(m_hOutStatsFile);
#endif
	close(m_hOutStatsFile);
	m_hOutStatsFile = -1;
	}

if(m_hOutBinFile != -1)
	{
	if(m_WrtBinBuffOfs > 0)
		{
		CUtility::SafeWrite(m_hOutBinFile,m_pWrtBinBuff,m_WrtBinBuffOfs);
		m_WrtBinBuffOfs = 0;
		}
#ifdef _WIN32
	_commit(m_hOutBinFile);
#else
	fsync(m_hOutBinFile);
#endif
	close(m_hOutBinFile);
	m_hOutBinFile = -1;
	}

m_ProcessingPhase = ePPCompleted;

#ifdef _DEBUG
#ifdef _WIN32
_ASSERTE( _CrtCheckMemory());
#endif
#endif

DEReset();

return(eBSFSuccess);
}


bool													// true if at least one FeatureID to be processed, false if all FeatureIDs have been already allocated for processing
GetFeats2Proc(tsThreadInstData *pThreadInst)
{
int CurFeatureID;
pThreadInst->NumFeats2Proc = 0;
pThreadInst->Feats2Proc[0] = 0;
if(pThreadInst->MaxFeats2Proc > cMaxFeats2ProcAlloc)	// sanity check!
	pThreadInst->MaxFeats2Proc = cMaxFeats2ProcAlloc;
DEAcquireSerialise();
if((CurFeatureID = m_LastFeatureAllocProc) < 0)			// all features already allocated for processing?
	{
	DEReleaseSerialise();
	return(false);
	}

while((CurFeatureID = m_pBEDFeatFile->GetNextFeatureID(CurFeatureID)) > 0)
	{
	pThreadInst->Feats2Proc[pThreadInst->NumFeats2Proc++] = CurFeatureID;
	if(pThreadInst->NumFeats2Proc >= pThreadInst->MaxFeats2Proc)
		break;
	}
m_LastFeatureAllocProc = CurFeatureID > 0 ? CurFeatureID : -1;
DEReleaseSerialise();
return(pThreadInst->NumFeats2Proc > 0 ? true : false);
}


#ifdef _WIN32
unsigned __stdcall ThreadedDEproc(void * pThreadPars)
#else
void *ThreadedDEproc(void * pThreadPars)
#endif
{
tsThreadInstData *pThreadInst = (tsThreadInstData *)pThreadPars; // makes it easier not having to deal with casts!
int Rslt = eBSFSuccess;
int NumProcessed = 0;
int DeltaNumProcessed;
int PrevNumProcessed = 0;

pThreadInst->Rslt = (teBSFrsltCodes)-1;
while(GetFeats2Proc(pThreadInst))
	{
	pThreadInst->NumFeatsProcessed = 0;
	do
		{
		pThreadInst->FeatureID = pThreadInst->Feats2Proc[pThreadInst->NumFeatsProcessed++];
		ProcessFeature(pThreadInst);
		NumProcessed += 1;
		DeltaNumProcessed = NumProcessed - PrevNumProcessed;
		if(DeltaNumProcessed > 20)
			{
			DEAcquireSerialise();
			m_NumFeaturesProcessed += DeltaNumProcessed;
			DEReleaseSerialise();
			PrevNumProcessed = NumProcessed;
			}
		}
	while(pThreadInst->NumFeatsProcessed < pThreadInst->NumFeats2Proc);
	}

DeltaNumProcessed = NumProcessed - PrevNumProcessed;
if(DeltaNumProcessed > 0)
	{
	DEAcquireSerialise();
	m_NumFeaturesProcessed += DeltaNumProcessed;
	DEReleaseSerialise();
	}
pThreadInst->Rslt = eBSFSuccess;
#ifdef _WIN32
_endthreadex(0);
return(eBSFSuccess);
#else
pthread_exit(NULL);
#endif
}


teBSFrsltCodes
LoadGeneFeatures(char Strand,			// features on this strand
				 char *pszInFeatFile)	// from this BED file
{
teBSFrsltCodes Rslt;


if((m_pBEDFeatFile = new CBEDfile)==NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CBEDfile");
	DEReset();
	return(eBSFerrObj);
	}

if((Rslt=m_pBEDFeatFile->Open(pszInFeatFile,eBTAnyBed)) !=eBSFSuccess)
	{
	while(m_pBEDFeatFile->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,m_pBEDFeatFile->GetErrMsg());
	DEReset();
	return(eBSFerrOpnFile);
	}

return(eBSFSuccess);
}

tsAlignLociInstStarts *
UpdateBinLociInstStarts(tsThreadInstData *pThreadInst,			// thread instance
					UINT32 Bin,								// starts are in this bin
					int RelLoci,							// starts at this relative (to start of binned transcript) loci
					UINT32 CtrlStarts,						// this number of control reads start at RelLoci
					UINT32 ExprStarts)						// this number of experiment reads start at RelLoci
{
tsAlignLociInstStarts *pBinLociInstStarts;
UINT32 Idx;
pBinLociInstStarts = pThreadInst->pBinLociInstStarts;
if(pThreadInst->NumBinInstStarts)
	{
	for(Idx = 0; Idx < pThreadInst->NumBinInstStarts; Idx++,pBinLociInstStarts++)
		if(pBinLociInstStarts->RelLoci == RelLoci && pBinLociInstStarts->Bin == Bin)
			{
			pBinLociInstStarts->NumCtrlStarts += CtrlStarts;
			pBinLociInstStarts->NumExprStarts += ExprStarts;
			return(pBinLociInstStarts);
			}
	}

pBinLociInstStarts = &pThreadInst->pBinLociInstStarts[pThreadInst->NumBinInstStarts++];
memset(pBinLociInstStarts,0,sizeof(tsAlignLociInstStarts));
pBinLociInstStarts->RelLoci = RelLoci;
pBinLociInstStarts->Bin = Bin;
pBinLociInstStarts->NumCtrlStarts = CtrlStarts;
pBinLociInstStarts->NumExprStarts = ExprStarts;
return(pBinLociInstStarts);
}


teBSFrsltCodes
AddAlignBinCnts(tsThreadInstData *pThreadInst,	// thread instance data
			UINT32 RelLoci,			// relative offset within  pThreadInst->CurRegionLen from which these cnts were derived
			UINT32 MeanControlReadLen,	// control reads were of this mean length
			bool bSense,				// 1 if aligned sense, 0 if antisense
			UINT32 ControlCnts,		// attribute this many control counts (alignments) to relevant bin(s)
			UINT32 MeanExperimentReadLen,	// experiment reads were of this mean length
			UINT32 ExperimentCnts)		// attribute this many experiment counts (alignments) to relevant bin(s)
{
tsAlignBin *pAlignBin;
tsAlignBin *pCovAlignBin;
INT32 CurLoci;
UINT32 BasesCovered;
int CurBinIdx;
UINT32 CurBinLen;
UINT32 CurStartLoci;

if(RelLoci >= (UINT32)pThreadInst->CurRegionLen) // should always start within region but best to check it does!
	return(eBSFerrInternal);

if(pThreadInst->NumBinsWithLoci == 0)					// first bin for current feature, if so then initialise all bins for this feature
	{
	memset(pThreadInst->pAlignBins,0,sizeof(tsAlignBin) * m_NumBins); 
	pAlignBin = pThreadInst->pAlignBins;
	CurStartLoci = 0;
	for(CurBinIdx = 0; CurBinIdx < m_NumBins; CurBinIdx++,pAlignBin++)
		{
		CurBinLen = (pThreadInst->CurRegionLen - CurStartLoci) / (m_NumBins - CurBinIdx);
		pAlignBin->BinRelStartLoci = CurStartLoci;
		pAlignBin->BinRelEndLoci = CurStartLoci + CurBinLen - 1;
		CurStartLoci = pAlignBin->BinRelEndLoci + 1;
		}
	}

pAlignBin = pThreadInst->pAlignBins; 
for(CurBinIdx = 0; CurBinIdx < m_NumBins; CurBinIdx++,pAlignBin++)
	if(RelLoci <= pAlignBin->BinRelEndLoci)
		break;

if(pAlignBin->Bin == 0)
	{
	pThreadInst->NumBinsWithLoci += 1;
	pAlignBin->Bin = CurBinIdx+1;
	}

if(ControlCnts > 0)
	{
	pAlignBin->ControlCnts += ControlCnts;
	pAlignBin->NumCtrlInstStarts += 1;
	}

if(ExperimentCnts > 0)
	{
	pAlignBin->ExperimentCnts += ExperimentCnts;
	pAlignBin->NumExprInstStarts += 1;
	}

// use the read length and apportion reads over 1 or more bins so as to derive coverage
pCovAlignBin = pAlignBin;
CurLoci = RelLoci;
if(!bSense)				// if was aligned antisense
	{
	if(MeanControlReadLen <= (UINT32)CurLoci + 1)
		CurLoci -= MeanControlReadLen - 1;
	else
		{
		MeanControlReadLen -= CurLoci + 1;
		CurLoci = 0;
		}
	pCovAlignBin = pThreadInst->pAlignBins; 
	for(CurBinIdx = 0; CurBinIdx < m_NumBins; CurBinIdx++,pCovAlignBin++)
		if(CurLoci <= (INT32)pCovAlignBin->BinRelEndLoci)
			break;
	}

do {
	if(ControlCnts)
		{
		BasesCovered = pCovAlignBin->BinRelEndLoci + 1 - CurLoci;
		if(BasesCovered > MeanControlReadLen)
			BasesCovered = MeanControlReadLen;

		pCovAlignBin->ControlCoverage += BasesCovered * ControlCnts;
		MeanControlReadLen -= BasesCovered;
		}
	if(ExperimentCnts)
		{
		BasesCovered = pCovAlignBin->BinRelEndLoci + 1 - CurLoci;
		if(BasesCovered > MeanExperimentReadLen)
			BasesCovered = MeanExperimentReadLen;

		pCovAlignBin->ExperimentCoverage += BasesCovered * ExperimentCnts;
		MeanExperimentReadLen -= BasesCovered;
		}
	CurLoci = pCovAlignBin->BinRelEndLoci + 1;
	pCovAlignBin += 1;
	CurBinIdx += 1;
	}
while(CurBinIdx < m_NumBins && (MeanControlReadLen > 0 || MeanExperimentReadLen > 0));

UpdateBinLociInstStarts(pThreadInst,pAlignBin->Bin,RelLoci,ControlCnts,ExperimentCnts);
return(eBSFSuccess);
}


teBSFrsltCodes						// if > eBSFSuccess then read was silently sloughed because it was in an exclusion loci
AddReadHit(int FileID,				// parsed from this file
			bool bIsExperiment,		// true if this is an experimental read
			char *pszChrom,			// hit to this chrom
			char Strand,			// on this strand
			int StartLoci,			// starts at this loci
			int ReadLen)			// and is of this length
{
size_t memreq;
UINT8 *pTmpAlloc;
UINT32 ChromID;
tsAlignReadLoci *pAlignReadLoci;

if(bIsExperiment)
	{
	// need to allocate more memory?
	if(m_CurNumExprAlignReadsLoci >= m_AllocdExprAlignReadsLoci)
		{
		memreq = (m_AllocdExprAlignReadsLoci + cAlignReadsLociRealloc) * sizeof(tsAlignReadLoci);
	#ifdef _WIN32
		pTmpAlloc = (UINT8 *) realloc(m_pExprAlignReadLoci,memreq);
	#else
		pTmpAlloc = (UINT8 *)mremap(m_pExprAlignReadLoci,m_AllocdExprAlignReadsLoci *  sizeof(tsAlignReadLoci),memreq,MREMAP_MAYMOVE);
		if(pTmpAlloc == MAP_FAILED)
			pTmpAlloc = NULL;
	#endif
		if(pTmpAlloc == NULL)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddReadLoci: Memory reallocation to %lld bytes failed - %s",memreq,strerror(errno));
			DEReset();
			return(eBSFerrMem);
			}
		m_pExprAlignReadLoci = (tsAlignReadLoci *)pTmpAlloc;
		m_AllocdExprAlignReadsLoci += cAlignReadsLociRealloc;
		}

	}
else
	{
	// need to allocate more memory?
	if(m_CurNumCtrlAlignReadsLoci >= m_AllocdCtrlAlignReadsLoci)
		{
		memreq = (m_AllocdCtrlAlignReadsLoci + cAlignReadsLociRealloc) * sizeof(tsAlignReadLoci);
	#ifdef _WIN32
		pTmpAlloc = (UINT8 *) realloc(m_pCtrlAlignReadLoci,memreq);
	#else
		pTmpAlloc = (UINT8 *)mremap(m_pCtrlAlignReadLoci,m_AllocdCtrlAlignReadsLoci *  sizeof(tsAlignReadLoci),memreq,MREMAP_MAYMOVE);
		if(pTmpAlloc == MAP_FAILED)
			pTmpAlloc = NULL;
	#endif
		if(pTmpAlloc == NULL)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddReadLoci: Memory reallocation to %lld bytes failed - %s",memreq,strerror(errno));
			DEReset();
			return(eBSFerrMem);
			}
		m_pCtrlAlignReadLoci = (tsAlignReadLoci *)pTmpAlloc;
		m_AllocdCtrlAlignReadsLoci += cAlignReadsLociRealloc;
		}
	}

// if read start covers a loci to be excluded then simply slough this read
ChromID = ChromToID(pszChrom);
if(Strand != '-')		// assume that if strand is not to crick '-' then must be to watson '+' or sense strand
	Strand = '+';

if(m_NumExclZones > 0)			// if any exclusion zones loaded then need to check for reads to be excluded
	{
	int ExclIdx;
	int EndLoci = StartLoci + ReadLen - 1;
	tsExclZone *pExclLoci = m_pExclZones;
	for(ExclIdx = 0; ExclIdx < m_NumExclZones; ExclIdx++, pExclLoci++)
		{
		if(ChromID == pExclLoci->ChromID)
			{
			if(pExclLoci->Strand != '*' && pExclLoci->Strand != Strand)	// must match on strand?
				continue;												// read strand not matching try next exclusion loci
			if(StartLoci > pExclLoci->EndLoci)							// read starts after exclusion end loci so try next
				continue;
			if(EndLoci < pExclLoci->StartLoci)							// read ends before exclusion start loci so try next
				continue;
			m_NumExclReads += 1;										// this read has been excluded
			return((teBSFrsltCodes)1);									// report read as being excluded
			}
		}
	}

// need to ensure read starts are normalised to the strand from which that read aligned
if(bIsExperiment)
	pAlignReadLoci = &m_pExprAlignReadLoci[m_CurNumExprAlignReadsLoci++];
else
	pAlignReadLoci = &m_pCtrlAlignReadLoci[m_CurNumCtrlAlignReadsLoci++];
memset(pAlignReadLoci,0,sizeof(tsAlignReadLoci));
pAlignReadLoci->ExprFlag = bIsExperiment ? 1 : 0;
pAlignReadLoci->ChromID = ChromID;
pAlignReadLoci->Sense = Strand == '-' ? 0 : 1;				// assume that if strand is not to crick '-' then must be to watson '+' or sense strand
if(pAlignReadLoci->Sense)									// onto sense, 5' start is the loci
	pAlignReadLoci->Loci = StartLoci;
else
	pAlignReadLoci->Loci = StartLoci + ReadLen - 1;
pAlignReadLoci->FileID = FileID;
pAlignReadLoci->NormCnts = 1;
pAlignReadLoci->ReadLen = ReadLen;
pAlignReadLoci->ArtCnts = 1;
return(eBSFSuccess);
}

// CoalesceReadAlignments
// Coalesce the read alignments by coalescing those alignments starting at, or very near, the same loci
// A user specified sliding window of WinLen is used and reads within the window are coalesced
int
CoalesceReadAlignments(int WinLen,		// coalescing window length 1..20
                       bool bSamesense,	 // if true then only coalesce reads with same sense
					   bool bExperiment) // if true then coalesce experiment read loci otherwise control read loci
{
size_t memreq;
UINT8 *pTmpAlloc;
UINT32 Idx;
UINT32 NumAlignReadsLoci;

UINT32 CurNumAlignReadsLoci;
tsAlignReadLoci *pAlignReadLoci;

tsAlignReadLoci *pCurLoci;
tsAlignReadLoci *pSrcLoci;
tsAlignReadLoci *pWinLoci;

if(bExperiment)
	{
	if(m_CurNumExprAlignReadsLoci < 2)
		{
		if(m_CurNumExprAlignReadsLoci != 0)
			m_pExprAlignReadLoci->ArtCnts = m_pExprAlignReadLoci->NormCnts;
		return(eBSFSuccess);
		}
	pAlignReadLoci = m_pExprAlignReadLoci;
	CurNumAlignReadsLoci = m_CurNumExprAlignReadsLoci;
	}
else
	{
	if(m_CurNumCtrlAlignReadsLoci < 2)
		{
		if(m_CurNumCtrlAlignReadsLoci != 0)
			m_pCtrlAlignReadLoci->ArtCnts = m_pCtrlAlignReadLoci->NormCnts;
		return(eBSFSuccess);
		}
	pAlignReadLoci = m_pCtrlAlignReadLoci;
	CurNumAlignReadsLoci = m_CurNumCtrlAlignReadsLoci;
	}

if(WinLen < 1)
	WinLen = 1;
else
	if(WinLen > cMaxCoalesceWinLen)
		WinLen = cMaxCoalesceWinLen;

pCurLoci = pAlignReadLoci;
pCurLoci->ArtCnts = pCurLoci->NormCnts;
pSrcLoci = pCurLoci + 1;
pSrcLoci->ArtCnts = pSrcLoci->NormCnts;
NumAlignReadsLoci = 1;
for(Idx = 1; Idx < CurNumAlignReadsLoci; Idx++,pSrcLoci++)
	{
	if(pSrcLoci->NormCnts == 0)				// already coalesced this loci's read cnt?
		continue;

	pWinLoci = pSrcLoci;
	while(pWinLoci->ChromID == pCurLoci->ChromID)
		{
		if(pWinLoci->Loci >= (pCurLoci->Loci + WinLen))
			break;
		if(!bSamesense || (pCurLoci->Sense == pWinLoci->Sense))
			{
			pCurLoci->NormCnts += pWinLoci->NormCnts;
			pCurLoci->ArtCnts = pCurLoci->NormCnts;
			pWinLoci->NormCnts = 0;
			pWinLoci->ArtCnts = 0;
			}
		pWinLoci += 1;
		}
	if(pSrcLoci->NormCnts == 0)
		continue;
	pCurLoci += 1;
	if(pCurLoci != pSrcLoci)
		*pCurLoci = *pSrcLoci;
	pCurLoci->ArtCnts = pCurLoci->NormCnts;
	NumAlignReadsLoci += 1;
	}


if(bExperiment)
	{
	// only realloc if worth the effort
	if((m_AllocdExprAlignReadsLoci - NumAlignReadsLoci)  > 1000000)
		{
		memreq = NumAlignReadsLoci * sizeof(tsAlignReadLoci);
	#ifdef _WIN32
		pTmpAlloc = (UINT8 *) realloc(m_pExprAlignReadLoci,memreq);
	#else
		pTmpAlloc = (UINT8 *)mremap(m_pExprAlignReadLoci,m_AllocdExprAlignReadsLoci *  sizeof(tsAlignReadLoci),memreq,MREMAP_MAYMOVE);
		if(pTmpAlloc == MAP_FAILED)
			pTmpAlloc = NULL;
	#endif
		if(pTmpAlloc == NULL)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"CoalesceReadAlignments: Memory reallocation to %lld bytes failed - %s",memreq,strerror(errno));
			DEReset();
			return(eBSFerrMem);
			}
		m_pExprAlignReadLoci = (tsAlignReadLoci *)pTmpAlloc;
		m_AllocdExprAlignReadsLoci = NumAlignReadsLoci;
		}
	m_CurNumExprAlignReadsLoci = NumAlignReadsLoci;
	}
else
	{
	// only realloc if worth the effort
	if((m_AllocdCtrlAlignReadsLoci - NumAlignReadsLoci)  > 1000000)
		{
		memreq = NumAlignReadsLoci * sizeof(tsAlignReadLoci);
	#ifdef _WIN32
		pTmpAlloc = (UINT8 *) realloc(m_pCtrlAlignReadLoci,memreq);
	#else
		pTmpAlloc = (UINT8 *)mremap(m_pCtrlAlignReadLoci,m_AllocdCtrlAlignReadsLoci *  sizeof(tsAlignReadLoci),memreq,MREMAP_MAYMOVE);
		if(pTmpAlloc == MAP_FAILED)
			pTmpAlloc = NULL;
	#endif
		if(pTmpAlloc == NULL)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"CoalesceReadAlignments: Memory reallocation to %lld bytes failed - %s",memreq,strerror(errno));
			DEReset();
			return(eBSFerrMem);
			}
		m_pCtrlAlignReadLoci = (tsAlignReadLoci *)pTmpAlloc;
		m_AllocdCtrlAlignReadsLoci = NumAlignReadsLoci;
		}

	m_CurNumCtrlAlignReadsLoci = NumAlignReadsLoci;
	}

return(eBSFSuccess);
}

// Scale the counts such that the control and library total number of reads are very nearly equal, e.g. +/- 1 count is near enough :-)
// Scaling is on the coalesced reads and will scale the library with lowest number of reads up to the library with highest number of reads
teBSFrsltCodes
NormaliseLibraryCounts(void)
{
bool bScaleExpr;
UINT32 TargTotal;
double NormCnts;
double Diff;
UINT32 TotNormCnts;
UINT32 Idx;

UINT32 CurNumAlignReadsLoci;
tsAlignReadLoci *pAlignReadLoci;
tsAlignReadLoci *pCurLoci;

if(m_LibSizeNormExpToCtrl == -1.0)
	m_LibSizeNormExpToCtrl = 1.0;

if(m_LibSizeNormExpToCtrl == 1.0 || (m_LibSizeNormExpToCtrl == 0.0 && (m_NumLoadedCtrlReads == m_NumLoadedExprReads)))
	{
	m_NumNormCtrlReads = m_NumLoadedCtrlReads;
	m_NumNormExprReads = m_NumLoadedExprReads;
	m_LibSizeNormExpToCtrl = 1.0;
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"No library size normalisation required");
	return(eBSFSuccess);
	}

if(m_LibSizeNormExpToCtrl == 0.0)  // if autoscaling
	{
	// autoscale from library with lowest number of counts to library with highest
	if(m_NumLoadedCtrlReads > m_NumLoadedExprReads)
		{
		m_LibSizeNormExpToCtrl = (double)m_NumLoadedCtrlReads/m_NumLoadedExprReads;
		bScaleExpr = true;
		}
	else
		{
		m_LibSizeNormExpToCtrl = (double)m_NumLoadedExprReads/m_NumLoadedCtrlReads;
		bScaleExpr = false;
		}
	}
else								// not autoscaling
	{
	if(m_LibSizeNormExpToCtrl > 0.0)
		bScaleExpr = true;
	else
		{
		bScaleExpr = false;
		m_LibSizeNormExpToCtrl = -1.0 * m_LibSizeNormExpToCtrl;
		}
	}

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Starting library size normalisation on %s with scale factor: %f",bScaleExpr ? "expression" : "control", m_LibSizeNormExpToCtrl);

if(bScaleExpr)
	{
	m_NumNormCtrlReads = m_NumLoadedCtrlReads;
	pAlignReadLoci = m_pExprAlignReadLoci;
	CurNumAlignReadsLoci = m_CurNumExprAlignReadsLoci;
	TargTotal = m_NumLoadedCtrlReads;
	}
else
	{
	m_NumNormExprReads = m_NumLoadedExprReads;
	pAlignReadLoci = m_pCtrlAlignReadLoci;
	CurNumAlignReadsLoci = m_CurNumCtrlAlignReadsLoci;
	TargTotal = m_NumLoadedExprReads;
	}

Diff = 0.0;
TotNormCnts = 0;
pCurLoci = pAlignReadLoci;
for(Idx = 0; Idx < CurNumAlignReadsLoci; Idx++,pCurLoci++)
	{
	if(!pCurLoci->NormCnts)
		continue;
	NormCnts = pCurLoci->NormCnts * m_LibSizeNormExpToCtrl;
	pCurLoci->NormCnts = (UINT32)NormCnts;
	Diff += NormCnts - (double)pCurLoci->NormCnts;
	if(Diff >= 0.5)
		{
		pCurLoci->NormCnts += 1;
	    Diff -= 1.0;
		}
	pCurLoci->ArtCnts = pCurLoci->NormCnts;
	TotNormCnts += pCurLoci->NormCnts;
	}

if(bScaleExpr)
	m_NumNormExprReads = TotNormCnts;
else
	m_NumNormCtrlReads = TotNormCnts;

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Completed library size normalisation on %s, control library size: %u expression library size: %u",bScaleExpr ? "experiment" : "control",m_NumNormCtrlReads,m_NumNormExprReads);
return(eBSFSuccess);
}

UINT32										// reduced total counts by this
ReducePCR(int FlankLen,						// 5' and 3' flank length over which the background mean rate is to be measured
		int ArtifactCntsThres,				// if counts at any loci are >= this threshold then process for PCR artifact reduction
		UINT32 NumAlignReadsLoci,tsAlignReadLoci *pAlignReadLoci)
{
UINT32 Idx;
UINT32 WinIdx;
UINT32 ChromRelIdx;
UINT32 WinCnts;
UINT32 NumCntLoci;
UINT32 ReducedBy;
UINT32 NormCnts;
double MeanX3;
tsAlignReadLoci *pTargLoci;
tsAlignReadLoci *pWinLoci;

UINT32 CurChromID;
CurChromID = 0;
ReducedBy = 0;
pTargLoci = pAlignReadLoci;
for(Idx = 0; Idx < NumAlignReadsLoci; Idx++,pTargLoci++)
	{
	pTargLoci->ArtCnts  = pTargLoci->NormCnts;
	if(pTargLoci->ChromID != CurChromID)
		{
		ChromRelIdx = Idx;
		CurChromID = pTargLoci->ChromID;
		}
	if(pTargLoci->NormCnts <= (UINT32)ArtifactCntsThres)
		continue;

	WinCnts = 0;
	NumCntLoci = 0;
	if((Idx - ChromRelIdx) >= (UINT32)FlankLen)
		WinIdx = Idx - FlankLen;
	else
		WinIdx = ChromRelIdx;
	while(WinIdx < Idx)
		{
		pWinLoci = &pAlignReadLoci[WinIdx++];
		if((pWinLoci->Loci + FlankLen) < pTargLoci->Loci)
			continue;
		if(pWinLoci->NormCnts > 0)
			{
			WinCnts += pWinLoci->NormCnts;
			NumCntLoci += 1;
			}
		}

	if(Idx != NumAlignReadsLoci - 1)
		{
		WinIdx = Idx;
		while(WinIdx++ < NumAlignReadsLoci)
			{
			pWinLoci = &pAlignReadLoci[WinIdx];
			if(pWinLoci->ChromID != CurChromID || pWinLoci->Loci > (pTargLoci->Loci + FlankLen))
				break;
			if(pWinLoci->NormCnts > 0)
				{
				WinCnts += pWinLoci->NormCnts;
				NumCntLoci += 1;
				}
			}
		}

	// sum of counts in 5' and 3' flanks, together with the number of loci, is now known
	if(NumCntLoci >= 1)
		{
		MeanX3 = (double)(3 * WinCnts) / NumCntLoci;
		if(pTargLoci->NormCnts > (UINT32)MeanX3)
			NormCnts = (UINT32)(MeanX3 + sqrt((double)pTargLoci->NormCnts - MeanX3));
		else
			continue;
		}
	else
		NormCnts = (UINT32)(ArtifactCntsThres + sqrt((double)pTargLoci->NormCnts - ArtifactCntsThres));
	ReducedBy += pTargLoci->NormCnts - NormCnts;
	pTargLoci->ArtCnts = NormCnts;
	}

pTargLoci = pAlignReadLoci;
for(Idx = 0; Idx < NumAlignReadsLoci; Idx++,pTargLoci++)
	pTargLoci->NormCnts = pTargLoci->ArtCnts;

return(ReducedBy);
}


// ReducePCRartifacts
// Attempt to reduce read counts resulting from significant PCR amplification artifacts
// These artifacts show as aligned loci to which reads were aligned at a rate significantly above the background rate within a window centered at the read loci being processed
teBSFrsltCodes
ReducePCRartifacts(int FlankLen,		// 5' and 3' flank length over which the background mean rate is to be measured
			int ArtifactCntsThres)		// if counts at any loci are >= this threshold then process for PCR artifact reduction
{
UINT32 ReducedBy;

gDiagnostics.DiagOut(eDLInfo,gszProcName,"Starting PCR artifact count processing");

ReducedBy = ReducePCR(FlankLen,ArtifactCntsThres,m_CurNumCtrlAlignReadsLoci,m_pCtrlAlignReadLoci);
m_NumLoadedCtrlReads -= ReducedBy;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Control PCR artifact read counts reduced by %d, accepted control reads now %d",ReducedBy,m_NumLoadedCtrlReads);

ReducedBy = ReducePCR(FlankLen,ArtifactCntsThres,m_CurNumExprAlignReadsLoci,m_pExprAlignReadLoci);
m_NumLoadedExprReads -= ReducedBy;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"Experiment PCR artifact read counts reduced by %d, accepted experiment reads now %d",ReducedBy,m_NumLoadedExprReads);

return(eBSFSuccess);
}




char *
TrimWhitespace(char *pTxt)
{
char *pStart;
char Chr;
	// strip leading whitespace
while(Chr = *pTxt++)
	if(!isspace(Chr))
			break;
if(Chr == '\0')					// empty line?
	return(pTxt-1);
pStart = pTxt-1;
while(Chr = *pTxt)			// fast forward to line terminator
	pTxt++;
pTxt-=1;
while(Chr = *pTxt--)
	if(!isspace(Chr))
		break;
pTxt[2] = '\0';
return(pStart);
}

teBSFrsltCodes
LoadAlignedReadsSAM(bool bIsExperiment,		// false if control file, true if experiment
			int FileID,						// uniquely identifies this file
			char *pszInFile,
			char FiltStrand)
{
teBSFrsltCodes Rslt;
int NumProcessed;
int NumRdsExcluded;
FILE *pSAMStream;
char szLine[16000];				// buffer input lines
char szChrom[128];				// parsed out chrom
char szDescriptor[128];			// parsed out descriptor
int Flags;						// parsed out flags
int StartLoci;					// start loci
int TLen;						// length
char szReadSeq[16000];			// to hold read sequence
char szCigar[128];
char szRNext[128];
char *pTxt;
int MAPQ;
int PNext;

// open SAM for reading
if(pszInFile == NULL || *pszInFile == '\0')
	return(eBSFerrParams);
if((pSAMStream = fopen(pszInFile,"r"))==NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadsSAM: Unable to fopen SAM format file %s error: %s",pszInFile,strerror(errno));
	return(eBSFerrOpnFile);
	}

NumRdsExcluded = 0;
NumProcessed = 0;

unsigned long Now;
unsigned long PrevNow = gStopWatch.ReadUSecs();
while(fgets(szLine,sizeof(szLine)-1,pSAMStream)!= NULL)
	{
	if(m_LimitAligned > 0 && (UINT32)NumProcessed > m_LimitAligned)
		break;

	if(!(NumProcessed % 10000))
		{
		Now = gStopWatch.ReadUSecs();
		if((Now - PrevNow) > 30)
			gDiagnostics.DiagOut(eDLInfo,gszProcName," Loading aligned read %d",NumProcessed);
		PrevNow = Now;
		}
	NumProcessed += 1;

	szLine[sizeof(szLine)-1] = '\0';
	pTxt = TrimWhitespace(szLine);
	if(*pTxt=='\0' || *pTxt=='@')	// simply slough lines which were just whitespace or start with '@'
		continue;

	// expecting to parse as "%s\t%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t", szDescriptor, Flags, m_szSAMTargChromName, StartLoci+1,MAPQ,szCigar,pszRNext,PNext,TLen);
	// interest is in the chromname, startloci, and length
	sscanf(szLine,"%s\t%d\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%s\t",szDescriptor, &Flags, szChrom, &StartLoci,&MAPQ,szCigar,szRNext,&PNext,&TLen,szReadSeq);
	if(StartLoci > 0)					// SAM loci are 1..n whereas BED and CSV are 0..n
		StartLoci -= 1;
	if(TLen == 0)						// if length on target not known then use the read sequence length
		TLen = (int)strlen(szReadSeq);

	if((Rslt = AddReadHit(FileID,		// parsed from this file
		    bIsExperiment,			// true if this is an experimental read
			szChrom,				// hit to this chrom
			Flags & 0x010 ? '-' : '+',					// on this strand
			StartLoci,				// starts at this loci
			TLen))	// and is of this length
			!= eBSFSuccess)
		{
		if(Rslt < eBSFSuccess)
			{
			fclose(pSAMStream);
			return(Rslt);
			}
		NumRdsExcluded += 1;
		Rslt = eBSFSuccess;
		}
	}

if(!m_NumExclZones)
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d",NumProcessed);
else
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d, %d acepted, %d were excluded because overlaying exclusion zone",NumProcessed,NumProcessed-NumRdsExcluded,NumRdsExcluded);
fclose(pSAMStream);
return(eBSFSuccess);
}

teBSFrsltCodes
LoadAlignedReadsBED(bool bIsExperiment,		// false if control file, true if experiment
			int FileID,						// uniquely identifies this file
			char *pszInFile,
			char FiltStrand)
{
teBSFrsltCodes Rslt;
int NumProcessed;
int NumRdsExcluded;
CBEDfile *pBEDFile;
int CurFeatureID;
int StartLoci;
int EndLoci;
int Score;
char szChrom[128];
char szFeatName[128];
char Strand;

if((pBEDFile = new CBEDfile)==NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CBEDfile");
	return(eBSFerrObj);
	}

if((Rslt=pBEDFile->Open(pszInFile,eBTAnyBed,false,m_LimitAligned)) !=eBSFSuccess)
	{
	while(pBEDFile->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,pBEDFile->GetErrMsg());
	delete pBEDFile;
	return(eBSFerrOpnFile);
	}

CurFeatureID = 0;
NumProcessed = 0;
NumRdsExcluded = 0;
unsigned long Now;
unsigned long PrevNow = gStopWatch.ReadUSecs();
while(Rslt == eBSFSuccess && (CurFeatureID = pBEDFile->GetNextFeatureID(CurFeatureID)) > 0)
	{
	if(m_LimitAligned > 0 && (UINT32)NumProcessed > m_LimitAligned)
		break;

	if(!(NumProcessed % 10000))
		{
		Now = gStopWatch.ReadUSecs();
		if((Now - PrevNow) > 30)
			gDiagnostics.DiagOut(eDLInfo,gszProcName," Loading aligned read %d",NumProcessed);
		PrevNow = Now;
		}
	NumProcessed += 1;

	pBEDFile->GetFeature(CurFeatureID,// feature instance identifier
				szFeatName,				// where to return feature name
				szChrom,				// where to return chromosome name
				&StartLoci,				// where to return feature start on chromosome (0..n)
				&EndLoci,				// where to return feature end on chromosome
 				&Score,					// where to return score
 				&Strand);				// where to return strand


	if(FiltStrand != '*' && Strand != FiltStrand)
		continue;

	if((Rslt = AddReadHit(FileID,		// parsed from this file
		    bIsExperiment,			// true if this is an experimental read
			szChrom,				// hit to this chrom
			Strand,					// on this strand
			StartLoci,				// starts at this loci
			1 + EndLoci - StartLoci))	// and is of this length
			!= eBSFSuccess)
		{
		if(Rslt < eBSFSuccess)
			{
			delete pBEDFile;
			return(Rslt);
			}
		NumRdsExcluded += 1;
		Rslt = eBSFSuccess;
		}
	}
if(!m_NumExclZones)
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d",NumProcessed);
else
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d, %d acepted, %d were excluded because overlaying exclusion zone",NumProcessed,NumProcessed-NumRdsExcluded,NumRdsExcluded);
delete pBEDFile;
return(eBSFSuccess);
}


teBSFrsltCodes
LoadAlignedReadsCSV(bool bIsExperiment,		// false if control file, true if experiment
			int FileID,						// uniquely identifies this file
			char *pszInFile,				// load reads from this file
			char FiltStrand)					// process for this strand '+' or '-' or for both '*'
{
teBSFrsltCodes Rslt;
int NumProcessed;
int ReadID;
int MatchLen;
char *pszTargSpecies;
char *pszChromName;
int Loci;
char *pszStrand;
int NumRdsExcluded;

int NumFields;
char szRawLine[200];


CCSVFile *pCSVAligns = NULL;


// load into memory
if((pCSVAligns = new CCSVFile) == NULL)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to instantiate CCSVfile");
	return(eBSFerrObj);
	}
pCSVAligns->SetMaxFields(14);	// expecting at least 8, upto 14, fields in reads alignment CSV file
if((pCSVAligns->Open(pszInFile))<0)
	{
	while(pCSVAligns->NumErrMsgs())
		gDiagnostics.DiagOut(eDLFatal,gszProcName,pCSVAligns->GetErrMsg());
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open file: %s",pszInFile);
	delete pCSVAligns;
	return(eBSFerrOpnFile);
	}

NumRdsExcluded = 0;
NumProcessed = 0;
int NumLineErrs = 0;
unsigned long Now;
unsigned long PrevNow = gStopWatch.ReadUSecs();
while((Rslt=(teBSFrsltCodes)pCSVAligns->NextLine()) > 0)	// onto next line containing fields
	{
	if(m_LimitAligned > 0 && (UINT32)NumProcessed > m_LimitAligned)
		{
		Rslt = eBSFSuccess;
		break;
		}
	if(!(NumProcessed % 1000))
		{
		Now = gStopWatch.ReadUSecs();
		if((Now - PrevNow) > 30)
			gDiagnostics.DiagOut(eDLInfo,gszProcName," Loading aligned read %d",NumProcessed);
		PrevNow = Now;
		}

	NumFields = pCSVAligns->GetCurFields();
	if(NumFields < 8)
		{
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"file: %s near line %d contains %d fields, expected at least 8\nRaw line was:",pszInFile,pCSVAligns->GetLineNumber(),NumFields);
		int RawLineLen = pCSVAligns->GetLine(sizeof(szRawLine)-1,szRawLine);	// returns a copy of raw line as processed by NextLine 
		szRawLine[sizeof(szRawLine)-1] = '\0';
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"%s",szRawLine);
		if(++NumLineErrs < 5)
			continue;
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Too many field parse errors");
		delete pCSVAligns;
		return(eBSFerrParams);
		}

	if(!NumProcessed && pCSVAligns->IsLikelyHeaderLine())
		continue;
	NumProcessed += 1;
	pCSVAligns->GetInt(1,&ReadID);
	pCSVAligns->GetText(3,&pszTargSpecies);
	pCSVAligns->GetText(4,&pszChromName);
	pCSVAligns->GetInt(5,&Loci);
	pCSVAligns->GetInt(7,&MatchLen);
	pCSVAligns->GetText(8,&pszStrand);
	if(FiltStrand != '*' && FiltStrand != *pszStrand)
		continue;

	if((Rslt = AddReadHit(FileID,		// parsed from this file
		    bIsExperiment,				// true if this is an experimental read
			pszChromName,				// hit to this chrom
			*pszStrand,					// on this strand
			Loci,						// starts at this loci
			MatchLen))					// and is of this length
			!= eBSFSuccess)
		{
		if(Rslt < eBSFSuccess)
			{
			delete pCSVAligns;
			return(Rslt);
			}
		NumRdsExcluded += 1;
		Rslt = eBSFSuccess;
		}
	}
if(!m_NumExclZones)
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d",NumProcessed);
else
	gDiagnostics.DiagOut(eDLInfo,gszProcName," Completed loading aligned reads %d, %d acepted, %d were excluded because overlaying exclusion zone",NumProcessed,NumProcessed-NumRdsExcluded,NumRdsExcluded);

delete pCSVAligns;
return(Rslt);
}

char *
IDtoChrom(UINT32 ChromID)	// returns ptr to chrom for ChromID
{
char *pszChrom;
if(m_pChroms == NULL || ChromID == 0 || ChromID > (UINT32)m_CurNumChroms)
	{
	DEReleaseSerialise();
	return(NULL);
	}
pszChrom = m_pChroms[ChromID-1].szChromName;
return(pszChrom);
}

UINT32
ChromToID(char *pszChrom)	// get unique chromosome identifier
{
tsRefIDChrom *pNewChrom;
UINT16 aHash = CUtility::GenHash16(pszChrom);

if(m_pChroms == NULL)		// NULL if first time...
	{
	m_pChroms = new tsRefIDChrom [cChromsInitalAllocNum*10];
	if(m_pChroms == NULL)
		{
		DEReleaseSerialise();
		return(0);
		}
	m_NumChromsAllocd = cChromsInitalAllocNum * 10;
	m_CurNumChroms = 0;
	m_MRAChromID = 0;
	}


if(m_CurNumChroms != 0)
	{
	// high probability that chromosome will be one which was last accessed
	if(m_MRAChromID > 0)
		{
		pNewChrom = &m_pChroms[m_MRAChromID-1];
		if(aHash == pNewChrom->Hash && !stricmp(pszChrom,pNewChrom->szChromName))
			return(pNewChrom->ChromID);
		}
	// not most recently accessed, need to do a linear search
	pNewChrom = m_pChroms;
	for(m_MRAChromID = 1; m_MRAChromID <= m_CurNumChroms; m_MRAChromID++,pNewChrom++)
		if(aHash == pNewChrom->Hash && !stricmp(pszChrom,pNewChrom->szChromName))
			return(pNewChrom->ChromID);

	}

if(m_CurNumChroms == m_NumChromsAllocd)
	{
	pNewChrom = new tsRefIDChrom [m_NumChromsAllocd + (cChromsGrowAllocNum * 10)];
	if(pNewChrom == NULL)
		return(0);
	memcpy(pNewChrom,m_pChroms,sizeof(tsRefIDChrom) * m_CurNumChroms);
	delete m_pChroms;
	m_pChroms = pNewChrom;
	m_NumChromsAllocd += cChromsGrowAllocNum;
	}

// new chromosome entry
pNewChrom = &m_pChroms[m_CurNumChroms++];
pNewChrom->Hash = aHash;
strcpy(pNewChrom->szChromName,pszChrom);
pNewChrom->ChromID = m_CurNumChroms;
m_MRAChromID = m_CurNumChroms;
return((UINT32)m_CurNumChroms);
}

int
LoadAlignedReads(bool bExpr,	// false if loading control, true if loading experiment
			char Strand,		// process for this strand '+' or '-' or for both '*'
			int FType,				// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
			int FileID,			// uniquely identifies this input file
			char *pszInAlignFile)	// load aligned reads from this file)
{
int Rslt;
etClassifyFileType FileType;
if(FType == 0)
	FileType = CUtility::ClassifyFileType(pszInAlignFile);
else
	FileType = (etClassifyFileType)(FType - 1);

switch(FileType) {
	case eCFTopenerr:		// unable to open file for reading
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to open file: '%s'",pszInAlignFile);
		return(eBSFerrOpnFile);

	case eCFTlenerr:		// file length is insufficent to classify type
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Unable to classify file type (insuffient data points): '%s'",pszInAlignFile);
		return(eBSFerrFileAccess);

	case eCFTunknown:		// unable to reliably classify
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"Unable to reliably classify file type: '%s'",pszInAlignFile);
		return(eBSFerrFileType);

	case eCFTCSV:			// file has been classified as being CSV
		Rslt = LoadAlignedReadsCSV(bExpr,FileID,pszInAlignFile,Strand);
		break;

	case eCFTBED:			// file has been classified as being BED
		Rslt = LoadAlignedReadsBED(bExpr,FileID,pszInAlignFile,Strand);
		break;

	case eCFTSAM:			// file has been classified as being SAM
		Rslt = LoadAlignedReadsSAM(bExpr,FileID,pszInAlignFile,Strand);
		break;
	}
return(Rslt);
}

int
LoadAlignedReadFiles(char Strand,		// process for this strand '+' or '-' or for both '*'
			int FType,					// input element file format: 0 - auto, 1 - CSV, 2 - BED, 3 - SAM
			int NumInputControlSpecs,	// number of input file specs
			char **pszInControlFiles,	// input control aligned reads files
			int NumInputExperimentSpecs,// number of input file specs
			char **pszInExperimentFiles)	// input experiment aligned reads files
{
int Rslt;
int Idx;
char *pszInfile;
int NumInputFilesProcessed;
size_t memreq;

if(m_pCtrlAlignReadLoci == NULL)
	{
	memreq = cAlignReadsLociInitalAlloc * sizeof(tsAlignReadLoci);
#ifdef _WIN32
	m_pCtrlAlignReadLoci = (tsAlignReadLoci *) malloc((size_t)memreq);
	if(m_pCtrlAlignReadLoci == NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddEntry: Memory allocation of %lld bytes failed",(INT64)memreq);
		DEReset();
		return(eBSFerrMem);
		}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
	m_pCtrlAlignReadLoci = (tsAlignReadLoci *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
	if(m_pCtrlAlignReadLoci == MAP_FAILED)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Memory allocation of %lld bytes through mmap()  failed",(INT64)memreq,strerror(errno));
		m_pCtrlAlignReadLoci = NULL;
		DEReset();
		return(eBSFerrMem);
		}
#endif
	m_AllocdExprAlignReadsLoci = cAlignReadsLociInitalAlloc;
	m_CurNumCtrlAlignReadsLoci = 0;
	memset(m_pCtrlAlignReadLoci,0,sizeof(tsAlignReadLoci));
	}

if(m_pExprAlignReadLoci == NULL)
	{
	memreq = cAlignReadsLociInitalAlloc * sizeof(tsAlignReadLoci);
#ifdef _WIN32
	m_pExprAlignReadLoci = (tsAlignReadLoci *) malloc((size_t)memreq);
	if(m_pExprAlignReadLoci == NULL)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"AddEntry: Memory allocation of %lld bytes failed",(INT64)memreq);
		DEReset();
		return(eBSFerrMem);
		}
#else
	// gnu malloc is still in the 32bit world and can't handle more than 2GB allocations
	m_pExprAlignReadLoci = (tsAlignReadLoci *)mmap(NULL,(size_t)memreq, PROT_READ |  PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS, -1,0);
	if(m_pExprAlignReadLoci == MAP_FAILED)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Memory allocation of %lld bytes through mmap()  failed",(INT64)memreq,strerror(errno));
		m_pExprAlignReadLoci = NULL;
		DEReset();
		return(eBSFerrMem);
		}
#endif
	m_AllocdExprAlignReadsLoci = cAlignReadsLociInitalAlloc;
	m_CurNumExprAlignReadsLoci = 0;
	memset(m_pExprAlignReadLoci,0,sizeof(tsAlignReadLoci));
	}


CSimpleGlob glob(SG_GLOB_FULLSORT);
NumInputFilesProcessed = 0;
for(Idx = 0; Idx < NumInputControlSpecs; Idx++)
	{
	glob.Init();
	if(glob.Add(pszInControlFiles[Idx]) < SG_SUCCESS)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to glob '%s",pszInControlFiles[Idx]);
		DEReset();
		return(eBSFerrOpnFile);	// treat as though unable to open file
		}
	if(glob.FileCount() <= 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to locate any input control file matching '%s",pszInControlFiles[Idx]);
		continue;
		}

	Rslt = eBSFSuccess;
	for (int FileID = 0; Rslt >= eBSFSuccess &&  FileID < glob.FileCount(); ++FileID)
		{
		pszInfile = glob.File(FileID);
		NumInputFilesProcessed += 1;
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: Loading control read alignments from file: %s",pszInfile);
		Rslt = LoadAlignedReads(false,Strand,FType,FileID+1,pszInfile);
		if(Rslt < 0)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Failed loading control read alignments from file: %s",pszInfile);
			DEReset();
			return(Rslt);
			}
		}
	}

if(NumInputFilesProcessed == 0 || m_CurNumCtrlAlignReadsLoci == 0)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Failed to load any control read alignments from any file");
	DEReset();
	return(eBSFerrOpnFile);
	}
m_NumLoadedCtrlReads = m_CurNumCtrlAlignReadsLoci;
gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: Accepted %d control aligned reads on strand '%c'",m_NumLoadedCtrlReads,Strand);

NumInputFilesProcessed = 0;
for(Idx = 0; Idx < NumInputExperimentSpecs; Idx++)
	{
	glob.Init();
	if(glob.Add(pszInExperimentFiles[Idx]) < SG_SUCCESS)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to glob '%s",pszInExperimentFiles[Idx]);
		DEReset();
		return(eBSFerrOpnFile);	// treat as though unable to open file
		}
	if(glob.FileCount() <= 0)
		{
		gDiagnostics.DiagOut(eDLFatal,gszProcName,"Unable to locate any input experiment file matching '%s",pszInExperimentFiles[Idx]);
		continue;
		}

	Rslt = eBSFSuccess;
	for (int FileID = 0; Rslt >= eBSFSuccess &&  FileID < glob.FileCount(); ++FileID)
		{
		pszInfile = glob.File(FileID);
		NumInputFilesProcessed += 1;
		gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: Loading experiment read alignments from file: %s",pszInfile);
		Rslt = LoadAlignedReads(true,Strand,FType,FileID+1,pszInfile);
		if(Rslt < 0)
			{
			gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Failed loading experiment read alignments from file: %s",pszInfile);
			DEReset();
			return(Rslt);
			}
		}
	}

m_NumLoadedExprReads = m_CurNumExprAlignReadsLoci;
if(NumInputFilesProcessed == 0 || m_NumLoadedExprReads == 0)
	{
	gDiagnostics.DiagOut(eDLFatal,gszProcName,"LoadAlignedReadFiles: Failed to load any experiment read alignments from any file");
	DEReset();
	return(eBSFerrOpnFile);
	}
gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: accepted %d experiment aligned reads on strand '%c'",m_NumLoadedExprReads,Strand);
gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: accepted total of %d control and experiment aligned reads on strand '%c'",m_NumLoadedCtrlReads + m_NumLoadedExprReads,Strand);

	// finally, create sorted index by chrom, loci, strand, control over the loaded aligned reads
if(m_CurNumCtrlAlignReadsLoci > 1)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: sorting %d control aligned reads...",m_NumLoadedCtrlReads);
	m_mtqsort.qsort(m_pCtrlAlignReadLoci,m_NumLoadedCtrlReads,sizeof(tsAlignReadLoci),SortAlignments);
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: sorting %d control aligned reads completed",m_NumLoadedCtrlReads);
	}

if(m_CurNumExprAlignReadsLoci > 1)
	{
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: sorting %d experiment aligned reads...",m_NumLoadedExprReads);
	m_mtqsort.qsort(m_pExprAlignReadLoci,m_NumLoadedExprReads,sizeof(tsAlignReadLoci),SortAlignments);
	gDiagnostics.DiagOut(eDLInfo,gszProcName,"LoadAlignedReadFiles: sorting %d experiment aligned reads completed",m_NumLoadedExprReads);
	}

return(Rslt);
}


int
LocateStartAlignment(char Strand,UINT32 ChromID,UINT32 StartLoci,UINT32 EndLoci, UINT32 NumAlignReadsLoci,tsAlignReadLoci *pAlignReadLoci)
{
if(pAlignReadLoci == NULL|| NumAlignReadsLoci == 0)
	return(0);

tsAlignReadLoci *pEl2;

int CmpRslt;

UINT32 TargPsn;
UINT32 Lo,Hi;

Lo = 0;
Hi = NumAlignReadsLoci - 1;
do {
	TargPsn = (Lo + Hi) / 2;
	pEl2 = &pAlignReadLoci[TargPsn];
	CmpRslt = 0;
	if(ChromID < pEl2->ChromID)
		CmpRslt = -1;
	else
		if(ChromID > pEl2->ChromID)
			CmpRslt = 1;

	if(!CmpRslt && EndLoci < pEl2->Loci)
		CmpRslt = -1;
	else
		if(!CmpRslt && StartLoci > pEl2->Loci)
			CmpRslt = 1;

	if(!CmpRslt && Strand != '*')
		{
		if(Strand != (pEl2->Sense ? '+' : '-'))
			CmpRslt = Strand == '+' ? -1 : 1;
		}

	// if CmpRslt == 0 then have a match which is on requested chromosome and with it's loci in range StartLoci to EndLoci
	// BUT may not be the lowest loci in the range so need to do a little more work...
	if(!CmpRslt)	
		{
		// now locate the lowest loci in the range StartLoci to EndLoci
		if(TargPsn == 0 || Lo == TargPsn) // check if already lowest
			{
			if(pEl2->Loci > EndLoci)   // should never occur but best to check!
				return(0);
			return(TargPsn + 1);
			}
		// iterate down until lowest loci located which is still within range
		while(TargPsn > Lo)
			{
			pEl2 -= 1;
			if(pEl2->Loci < StartLoci || pEl2->ChromID != ChromID)
				break;
			// still in range
			TargPsn -= 1;
			}
		return(TargPsn + 1);
		}

	// still trying to locate chrom with a loci which is within requested range StartLoci to EndLoci
	if(CmpRslt < 0)
		{
		if(TargPsn == 0)
			break;
		Hi = TargPsn - 1;
		}
	else
		Lo = TargPsn+1;
	}
while(Hi >= Lo);

return(0);	// unable to locate any alignment instances
}


teBSFrsltCodes
GenBinStarts(tsThreadInstData *pThreadInst,	// thread instance
			      UINT32 RegionOfs,			// StartLoci is at this rlative offset within the current region
				  UINT32 ChromID,			// alignments are on this chrom
				  UINT32 StartLoci,			// must start on or after this loci
				  UINT32 EndLoci,			// must start on or before this loci
				  UINT32 NumAlignReadsLoci,
				  tsAlignReadLoci *pAlignReadsLoci)
{
UINT32 AlignIdx;
int CurAlignDiffIdx;
UINT8 CurSense;
UINT32 ControlCnts;
UINT32 ExperimentCnts;
UINT32 SumControlReadLen;
UINT32 SumExperimentReadLen;
UINT32 MeanControlReadLen;
UINT32 MeanExperimentReadLen;
int CurLoci;
int CurBin;
int PrvBin;
tsAlignReadLoci *pAlignReadLoci;

if((AlignIdx = (UINT32)LocateStartAlignment(m_DEAlignStrand,ChromID,StartLoci,EndLoci,NumAlignReadsLoci,pAlignReadsLoci))==0)
	return(eBSFSuccess);

pAlignReadLoci = &pAlignReadsLoci[AlignIdx -1];
ControlCnts = 0;
ExperimentCnts = 0;
SumControlReadLen = 0;
SumExperimentReadLen = 0;
MeanControlReadLen = 0;
MeanExperimentReadLen = 0;
CurSense = 0;
CurLoci = -1;
CurBin = 0;
PrvBin = 0;
CurAlignDiffIdx = 0;
for(; AlignIdx < NumAlignReadsLoci; AlignIdx++,pAlignReadLoci++)
	{
	if(pAlignReadLoci->ChromID != ChromID ||						// only interested in alignment if still on ChromID and starting <= EndLoci
		pAlignReadLoci->Loci > EndLoci)
		break;

	if(m_DEAlignStrand != '*' && m_DEAlignStrand != (pAlignReadLoci->Sense ? '+' : '-'))		// only interested in alignment if on requested strand
		continue;

	if(CurLoci != -1 && (pAlignReadLoci->Loci != CurLoci || pAlignReadLoci->Sense != CurSense)) // is this aligned read starting at a different loci or different strand?
		{
		// onto a different loci
		if(ControlCnts)
			MeanControlReadLen = SumControlReadLen / ControlCnts;
		else
			MeanControlReadLen = 0;
		if(ExperimentCnts)
			MeanExperimentReadLen = SumExperimentReadLen / ExperimentCnts;
		else
			MeanExperimentReadLen = 0;
		AddAlignBinCnts(pThreadInst,RegionOfs + (CurLoci - StartLoci),MeanControlReadLen,CurSense ? true : false,ControlCnts,MeanExperimentReadLen,ExperimentCnts);
		ControlCnts = 0;
		ExperimentCnts = 0;
		SumControlReadLen = 0;
		SumExperimentReadLen = 0;
		MeanControlReadLen = 0;
		MeanExperimentReadLen = 0;
		}

	// accumulate counts for control and experiment
	if(pAlignReadLoci->ExprFlag == 0)
		{
		ControlCnts += pAlignReadLoci->NormCnts;
		SumControlReadLen += pAlignReadLoci->ReadLen * pAlignReadLoci->NormCnts;
		}
	else
		{
		ExperimentCnts += pAlignReadLoci->NormCnts;
		SumExperimentReadLen += pAlignReadLoci->ReadLen * pAlignReadLoci->NormCnts;
		}

	CurLoci = pAlignReadLoci->Loci;
	CurSense = pAlignReadLoci->Sense;
	}

if(ControlCnts > 0 || ExperimentCnts > 0)	// is there at least one control or experiment alignment?
	{
	if(ControlCnts)
		MeanControlReadLen = SumControlReadLen / ControlCnts;
	else
		MeanControlReadLen = 0;
	if(ExperimentCnts)
		MeanExperimentReadLen = SumExperimentReadLen / ExperimentCnts;
	else
		MeanExperimentReadLen = 0;
	AddAlignBinCnts(pThreadInst,RegionOfs + (CurLoci - StartLoci),MeanControlReadLen,CurSense ? true : false,ControlCnts,MeanExperimentReadLen,ExperimentCnts);
	}
return(eBSFSuccess);
}


teBSFrsltCodes
GenBinAlignStarts(tsThreadInstData *pThreadInst,	// thread instance
				  UINT32 RegionOfs,			// StartLoci is at this relative offset within the current region
				  char *pszChrom,			// alignments are on this chrom
				  UINT32 StartLoci,			// must start on or after this loci
				  UINT32 EndLoci)			// must start on or before this loci
{
teBSFrsltCodes Rslt;
UINT32 ChromID;

pThreadInst->CurFeatLen += 1 + EndLoci - StartLoci;

// position to alignment at lowest loci >= StartLoci
if((ChromID = ChromToID(pszChrom)) == 0)
	return(eBSFSuccess);

// first get and bin the control read alignments within the specified loci region
if((Rslt = GenBinStarts(pThreadInst,RegionOfs,ChromID,StartLoci,EndLoci,m_CurNumCtrlAlignReadsLoci,m_pCtrlAlignReadLoci)) >= eBSFSuccess)
	// next get and bin the experiment read alignments within the specified loci region
	Rslt = GenBinStarts(pThreadInst,RegionOfs,ChromID,StartLoci,EndLoci,m_CurNumExprAlignReadsLoci,m_pExprAlignReadLoci);

return(Rslt);
}


const double cConfInterval95 = 1.959963984540;
const double cConfInterval99 = 2.575829303549;

// Convert Fisher z' to Pearson r
double
z2r(double z)
{
if(z >= 17.6163615864504)
	return(1.0);
if(z <= 17.6163615864504)
	return(-1.0);
double e = exp(2.0 * z);
return((e-1)*(e+1));
}

// Convert Pearson r to Fisher z'
// Note that Pearson is checked for the possiblity of an underflow
double
r2z(double r)
{
if(r > 0.999999999999999)
	return(17.6163615864504);
if(r < -0.999999999999999)
	return(-17.6163615864504);
return(log((1.0+r)/(1.0-r))/2.0);
}

double								// returns *pUpper - *pLower
ConfInterval95(int N,				// number of bins containing at least one count
			   double Pearson,		// Pearsons r
			   double *pUpper,		// returned upper for a confidence interval of 95
			   double *pLower)		// returned lower for a confidence interval of 95
{
double z;
double uz;
double lz;
double StdErr;

if(N < 4)
	N = 4;

StdErr = 1.0/sqrt((double(N-3)));

z = r2z(Pearson);
uz = z + (cConfInterval95 * StdErr);
lz = z - (cConfInterval95 * StdErr);

uz = exp(2*uz);
if(uz == -1.0)
	uz += 0.000000001;

lz = exp(2*lz);
if(lz == -1.0)
	lz += 0.000000001;

*pUpper = (uz-1.0)/(uz+1.0);
*pLower = (lz-1.0)/(lz+1.0);

return(*pUpper - *pLower);
}

double								// returns *pUpper - *pLower
ConfInterval99(int N,				// number of bins containing at least one count
			   double Pearson,		// Pearsons r
			   double *pUpper,		// returned upper for a confidence interval of 95
			   double *pLower)		// returned lower for a confidence interval of 95
{
double z;
double uz;
double lz;
double StdErr;

if(N < 4)
	N = 4;

StdErr = 1.0/sqrt((double(N-3)));

z = r2z(Pearson);
uz = z + (cConfInterval99 * StdErr);
lz = z - (cConfInterval99 * StdErr);

uz = exp(2*uz);
if(uz == -1.0)
	uz += 0.000000001;

lz = exp(2*lz);
if(lz == -1.0)
	lz += 0.000000001;

*pUpper = (uz-1.0)/(uz+1.0);
*pLower = (lz-1.0)/(lz+1.0);

return(*pUpper - *pLower);
}



// Pearson sample correlation coefficient
// Defined as the covariance of the two variables divided by the product of their standard deviations
// A count of 1 is added to both control and experiment so as to provide for Laplaces smoothing and prevent divide by zero errors
double									// returned Pearson
Pearsons(tsAlignBin *pAlignBins)		// bins containing alignment counts
{
tsAlignBin *pACnts;
int Idx;
int NumLociCnts;
double MeanC;
double MeanE;
double TmpC;
double TmpE;
double SumNum;
double SumDenC;
double SumDenE;
double Correl;

MeanC = 0.0;
MeanE = 0.0;
NumLociCnts = 0;
pACnts = pAlignBins;

// calc the means
for(Idx = 0; Idx < m_NumBins; Idx++, pACnts++)
	{
	if(pACnts->Bin == 0)
		continue;
	if(pACnts->ControlCoverage < 1 && pACnts->ExperimentCoverage < 1) // shouldn't occur but skip bins for which there are no control or experimental counts
		continue;
	NumLociCnts += 1;
	MeanC += pACnts->ControlCoverage+1;								// adding a psuedocount of 1 is Laplace's smoothing
	MeanE += pACnts->ExperimentCoverage+1;							// which also conveniently ensures can never have divide by zero errors!
	}
if(NumLociCnts == 0)
	return(0.0);

MeanC /= NumLociCnts;												// can now determine the means
MeanE /= NumLociCnts;

if(MeanC < 0.9 || MeanE < 0.9)										// should never have means of less than 1.0 because of Laplace's add 1
	return(0.0);

SumNum = 0.0;
SumDenC = 0.0;
SumDenE = 0.0;
pACnts = pAlignBins;
for(Idx = 0; Idx < m_NumBins; Idx++, pACnts++)
	{
	if(pACnts->Bin == 0)
		continue;
	if(pACnts->ControlCoverage < 1 && pACnts->ExperimentCoverage < 1) // shouldn't occur but skip bins for which there are no control or experimental counts
		continue;
	TmpC = pACnts->ControlCoverage - MeanC;
	TmpE = pACnts->ExperimentCoverage - MeanE;
	SumNum += (TmpC * TmpE);
	SumDenC += (TmpC * TmpC);
	SumDenE += (TmpE * TmpE);
	}

if(SumDenC < 0.00001)			// set a floor so as to prevent the chance of a subsequent divide by zero error
	SumDenC = 0.00001;
if(SumDenE < 0.00001)
	SumDenE = 0.00001;
Correl = SumNum/sqrt(SumDenC*SumDenE);
return(Correl);
}

double										// returned Pearson
PoissonPearsons(tsAlignBin *pAlignBins)		// bins containing alignment counts
{
tsAlignBin *pACnts;
int Idx;
int NumLociCnts;
double MeanC;
double MeanE;
double TmpC;
double TmpE;
double SumNum;
double SumDenC;
double SumDenE;
double Correl;

MeanC = 0.0;
MeanE = 0.0;
NumLociCnts = 0;
pACnts = pAlignBins;

// calc the means
for(Idx = 0; Idx < m_NumBins; Idx++, pACnts++)
	{
	if(pACnts->Bin == 0)
		continue;
	if(pACnts->ControlPoissonCnts < 1 && pACnts->ExperimentPoissonCnts < 1) // shouldn't occur but skip bins for which there are no control or experimental counts
		continue;
	NumLociCnts += 1;
	MeanC += pACnts->ControlPoissonCnts+1;								// adding a psuedocount of 1 is Laplace's smoothing
	MeanE += pACnts->ExperimentPoissonCnts+1;							// which also conveniently ensures can never have divide by zero errors!
	}
if(NumLociCnts == 0)
	return(0.0);

MeanC /= NumLociCnts;												// can now determine the means
MeanE /= NumLociCnts;

if(MeanC < 0.9 || MeanE < 0.9)										// should never have means of less than 1.0 because of Laplace's add 1
	return(0.0);

SumNum = 0.0;
SumDenC = 0.0;
SumDenE = 0.0;
pACnts = pAlignBins;
for(Idx = 0; Idx < m_NumBins; Idx++, pACnts++)
	{
	if(pACnts->Bin == 0)
		continue;
	if(pACnts->ControlPoissonCnts < 1 && pACnts->ExperimentPoissonCnts < 1) // shouldn't occur but skip bins for which there are no control or experimental counts
		continue;
	TmpC = pACnts->ControlPoissonCnts - MeanC;
	TmpE = pACnts->ExperimentPoissonCnts - MeanE;
	SumNum += (TmpC * TmpE);
	SumDenC += (TmpC * TmpC);
	SumDenE += (TmpE * TmpE);
	}

if(SumDenC < 0.00001)			// set a floor so as to prevent the chance of a subsequent divide by zero error
	SumDenC = 0.00001;
if(SumDenE < 0.00001)
	SumDenE = 0.00001;
Correl = SumNum/sqrt(SumDenC*SumDenE);
return(Correl);
}

// Clamps fold changes to be no more than cClampFoldChange fold
double
ClampFoldChange(double Scale)
{
if(Scale < (1.0/(2.0*cClampFoldChange)))
	return(0.0);
if(Scale <= (1.0/cClampFoldChange))
	return(1.0/cClampFoldChange);
if(Scale >= cClampFoldChange)
	return(cClampFoldChange);
return(Scale);
}


// Calculate a PValue for fold change through a counts permutation test
// Independently poisson permutes counts for both control and experiment
double								    // returned median PValue
PearsonsPValue(tsThreadInstData *pThreadInst,	// thread instance
		double Pearson,					// observed pearson
		int MaxPerms,					// maximum number of permutions
		tsAlignBin *pAlignBins,			// bins containing alignment counts
		double *pPValueLow95,			// returned low 95 percentile
		double *pPValueHi95,			// returned upper 95 percentile
		double *pLow95,					// returned low 95 percentile
		double *pHi95,					// returned upper 95 percentile
		double *pMedian,				// returned median
		double *pFeatLow95,				// returned low 95 percentile for feature
		double *pFeatHi95,				// returned upper 95 percentile for feature
		double *pFeatMedian)			// returned median for feature
{
int PermIter;
int SrcIdx;
int MaxNumPerms;
int Supportive;
double *pPearson;
double *pPValues;
double *pFeatFoldChanges;
tsAlignBin *pAlignBin;
tsAlignBin *pSrcCnts;

double ChiSqr;
double PValue;
int Cells[2][2];


UINT32 SumFeatCtrlPoissonCnts;
UINT32 SumFeatExprPoissonCnts;

double CurPearson;

*pLow95 = 0.0;
*pHi95 = 0.0;
*pMedian = 0.0;

*pFeatLow95 = 0.0;
*pFeatHi95 = 0.0;
*pFeatMedian = 0.0;

if(pThreadInst->NumBinsWithLoci < 1)	// if all bins are empty then can't really determine a PValue..
	return(0.0);

memcpy(pThreadInst->pPoissonAlignBins,pAlignBins,sizeof(tsAlignBin) * m_NumBins);
MaxNumPerms = m_NumBins * 2000;
if(MaxNumPerms > MaxPerms)
	MaxNumPerms = MaxPerms;

Supportive = 0;
pPearson = pThreadInst->pPearsons;
pFeatFoldChanges = pThreadInst->pFeatFoldChanges;
pPValues = pThreadInst->pPValues;
for(PermIter = 0; PermIter < MaxNumPerms; PermIter++,pPearson++,pFeatFoldChanges++,pPValues++)
	{
	SumFeatCtrlPoissonCnts = 0;
	SumFeatExprPoissonCnts = 0;
	pAlignBin = pAlignBins;
	pSrcCnts = pThreadInst->pPoissonAlignBins;
	// for each bin
	for(SrcIdx = 0; SrcIdx < m_NumBins; SrcIdx++,pSrcCnts++,pAlignBin++)
		{
		if(pAlignBin->Bin == 0)
			continue;

		pSrcCnts->ControlPoissonCnts = RandPoisson(pThreadInst,pSrcCnts->ControlCoverage);
		pSrcCnts->ExperimentPoissonCnts = RandPoisson(pThreadInst,pSrcCnts->ExperimentCoverage);
		SumFeatCtrlPoissonCnts += pSrcCnts->ControlPoissonCnts;
		SumFeatExprPoissonCnts += pSrcCnts->ExperimentPoissonCnts;
		}

	CurPearson = PoissonPearsons(pThreadInst->pPoissonAlignBins);
	*pPearson = CurPearson;

	if(SumFeatCtrlPoissonCnts >= 1)
		*pFeatFoldChanges = (double)SumFeatExprPoissonCnts /  (double)SumFeatCtrlPoissonCnts;
	else
		*pFeatFoldChanges =  (double)SumFeatExprPoissonCnts / 0.75;

	if(CurPearson > Pearson)
		Supportive += 1;

	// Poissson the total library counts
	UINT32 PoissonCtrlLibCnts;
	UINT32 PoissonExprLibCnts;
	PoissonExprLibCnts = (UINT32)RandPoisson(pThreadInst,m_NumNormExprReads  - SumFeatExprPoissonCnts);
	PoissonCtrlLibCnts = (UINT32)RandPoisson(pThreadInst,m_NumNormCtrlReads  - SumFeatCtrlPoissonCnts);
	// put a floor on the Poisson'd library counts
	if(PoissonCtrlLibCnts < SumFeatCtrlPoissonCnts)
		PoissonCtrlLibCnts = SumFeatCtrlPoissonCnts;
	if(PoissonExprLibCnts < SumFeatExprPoissonCnts)
		PoissonExprLibCnts = SumFeatExprPoissonCnts;

	// try a chisquare on this...
	Cells[0][0] = PoissonCtrlLibCnts;				// total control library reads less the control transcript counts
	Cells[0][1] = SumFeatCtrlPoissonCnts;			// control transcript counts
	Cells[1][0] = PoissonExprLibCnts;				// total experiment library coounts
	Cells[1][1] = SumFeatExprPoissonCnts;			// experiment transcript counts
	ChiSqr = pThreadInst->pStats->CalcChiSqr(2,2,(int *)&Cells);
	*pPValues = pThreadInst->pStats->ChiSqr2PVal(1,ChiSqr);
	if(*pPValues < 0.0)
		*pPValues = 0.0;
	}

qsort(pThreadInst->pPearsons,MaxNumPerms,sizeof(double),SortDoubles);
qsort(pThreadInst->pFeatFoldChanges,MaxNumPerms,sizeof(double),SortDoubles);
qsort(pThreadInst->pPValues,MaxNumPerms,sizeof(double),SortDoubles);

int LowerIdx;
int UpperIdx;

LowerIdx = (MaxNumPerms-1)/2;
if(MaxNumPerms & 0x01)
	*pMedian = pThreadInst->pPearsons[LowerIdx];
else
	*pMedian = (pThreadInst->pPearsons[LowerIdx] + pThreadInst->pPearsons[LowerIdx+1])/2.0;

if(MaxNumPerms & 0x01)
	PValue = pThreadInst->pPValues[LowerIdx];
else
	PValue = (pThreadInst->pPValues[LowerIdx] + pThreadInst->pPValues[LowerIdx+1])/2.0;

if(MaxNumPerms & 0x01)
	*pFeatMedian = m_pFeatFoldChanges[LowerIdx];
else
	*pFeatMedian = (pThreadInst->pFeatFoldChanges[LowerIdx] + pThreadInst->pFeatFoldChanges[LowerIdx+1])/2.0;

LowerIdx = (MaxNumPerms * 5) / 200;
UpperIdx = MaxNumPerms - LowerIdx;
*pLow95 = pThreadInst->pPearsons[LowerIdx];
*pHi95 = pThreadInst->pPearsons[UpperIdx];
*pPValueLow95 = pThreadInst->pPValues[LowerIdx];
*pPValueHi95 = pThreadInst->pPValues[UpperIdx];

*pFeatLow95 = pThreadInst->pFeatFoldChanges[LowerIdx];
*pFeatHi95 = pThreadInst->pFeatFoldChanges[UpperIdx];

return(PValue);
}


int
ReportDEandPearsonBinCounts(void)
{
int Idx;
tsFeatDE *pFeatDE;
UINT32 *pBinCnts;
int CurBin;

if(m_hOutBinFile == -1 || m_pWrtBinBuff == NULL)
	return(eBSFSuccess);

if(m_bWrtBinHdr)
	{
	m_WrtBinBuffOfs = sprintf((char *)m_pWrtBinBuff,"\"Classification\",\"Feat\",\"FeatLen\",\"Exons\",\"Score\",\"DECntsScore\",\"PearsonScore\",\"CtrlUniqueLoci\",\"ExprUniqueLoci\",\"CtrlExprLociRatio\",\"PValueMedian\",\"PValueLow95\",\"PValueHi95\",\"TotCtrlCnts\",\"TotExprCnts\",\"TotCtrlExprCnts\",\"ObsFoldChange\",\"FoldMedian\",\"FoldLow95\",\"FoldHi95\",\"Which\",\"ObsPearson\",\"PearsonMedian\",\"PearsonLow95\",\"PearsonHi95\",\"TotBins\",\"CtrlAndExprBins\",\"CtrlOnlyBins\",\"ExprOnlyBins\"");
	for(CurBin = 1; CurBin <= m_NumBins; CurBin++)
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],",\"Bin%d\"",CurBin);
	m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"\n");
	m_bWrtBinHdr = false;
	}
else
	m_WrtBinBuffOfs = 0;

pFeatDE = m_pFeatDEs;
for(Idx = 0; Idx < m_NumFeatsDEd; Idx++,pFeatDE++)
	{
	if(m_bFiltNonaligned && pFeatDE->SumCtrlExprCnts < 1)
		continue;
	if((m_WrtBinBuffOfs + 10000) > m_AllocBinWrtBuff)
		{
		CUtility::SafeWrite(m_hOutBinFile,m_pWrtBinBuff,m_WrtBinBuffOfs);
		m_WrtBinBuffOfs = 0;
		}
	if(pFeatDE->SumCtrlExprCnts < 1)
		{
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"%d,\"%s\",%d,%d,0,0,0,0,0,0.0,0.0,0.0,0.0,0,0,0,0.0,0.0,0.0,0.0,\"Control\",0.0,0.0,0.0,0.0,%d,0,0,0",pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons,m_NumBins);
		CurBin = 1;
		while(CurBin <= m_NumBins)
			{
			m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],",0");
			CurBin += 1;
			}
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"\n%d,\"%s\",%d,%d,0,0,0,0,0,0.0,0.0,0.0,0.0,0,0,0,0.0,0.0,0.0,0.0,\"Experiment\",0.0,0.0,0.0,0.0,%d,0,0,0",pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons,m_NumBins);
		CurBin = 1;
		while(CurBin <= m_NumBins)
			{
			m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],",0");
			CurBin += 1;
			}
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"\n");
		continue;
		}

	double LociRatio;

	if(pFeatDE->TotExprStartLoci > 0)
		LociRatio = (double)pFeatDE->TotCtrlStartLoci/(double)pFeatDE->TotExprStartLoci;
	else
		LociRatio = (double)pFeatDE->TotCtrlStartLoci + 0.01;

	m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"%d,\"%s\",%d,%d,%d,%d,%d,%d,%d,%f,%f,%f,%f,%d,%d,%d,%f,%f,%f,%f,\"Control\",%f,%f,%f,%f,%d,%d,%d,%d",
							pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons,pFeatDE->DEscore,pFeatDE->CntsScore,pFeatDE->PearsonScore,
							pFeatDE->TotCtrlStartLoci,pFeatDE->TotExprStartLoci,LociRatio,pFeatDE->PValueMedian,pFeatDE->PValueLow95,pFeatDE->PValueHi95,
							pFeatDE->CtrlCnts,pFeatDE->ExprCnts,pFeatDE->SumCtrlExprCnts,
							ClampFoldChange(pFeatDE->ObsFoldChange),ClampFoldChange(pFeatDE->FoldMedian),ClampFoldChange(pFeatDE->FoldLow95),ClampFoldChange(pFeatDE->FoldHi95),pFeatDE->PearsonObs,
							pFeatDE->PearsonMedian,pFeatDE->PearsonLow95,pFeatDE->PearsonHi95,m_NumBins,pFeatDE->BinsShared,pFeatDE->BinsExclCtrl,pFeatDE->BinsExclExpr);
	pBinCnts = pFeatDE->BinsCtrlDepth;
	for(CurBin = 0; CurBin < m_NumBins; CurBin++, pBinCnts++)
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],",%d",*pBinCnts);


	m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"\n%d,\"%s\",%d,%d,%d,%d,%d,%d,%d,%f,%f,%f,%f,%d,%d,%d,%f,%f,%f,%f,\"Experiment\",%f,%f,%f,%f,%d,%d,%d,%d",
							pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons,pFeatDE->DEscore,pFeatDE->CntsScore,pFeatDE->PearsonScore,
							pFeatDE->TotCtrlStartLoci,pFeatDE->TotExprStartLoci,LociRatio,pFeatDE->PValueMedian,pFeatDE->PValueLow95,pFeatDE->PValueHi95,
							pFeatDE->CtrlCnts,pFeatDE->ExprCnts,pFeatDE->SumCtrlExprCnts,
							ClampFoldChange(pFeatDE->ObsFoldChange),ClampFoldChange(pFeatDE->FoldMedian),ClampFoldChange(pFeatDE->FoldLow95),ClampFoldChange(pFeatDE->FoldHi95),pFeatDE->PearsonObs,
							pFeatDE->PearsonMedian,pFeatDE->PearsonLow95,pFeatDE->PearsonHi95,m_NumBins,pFeatDE->BinsShared,pFeatDE->BinsExclCtrl,pFeatDE->BinsExclExpr);

	pBinCnts = pFeatDE->BinsExprDepth;
	for(CurBin = 0; CurBin < m_NumBins; CurBin++, pBinCnts++)
		m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],",%d",*pBinCnts);
	m_WrtBinBuffOfs += sprintf((char *)&m_pWrtBinBuff[m_WrtBinBuffOfs],"\n");
	}

if(m_WrtBinBuffOfs > 0)
	{
	CUtility::SafeWrite(m_hOutBinFile,m_pWrtBinBuff,m_WrtBinBuffOfs);
	m_WrtBinBuffOfs = 0;
	}

return(eBSFSuccess);
}

int
ReportDEandPearsons(void)
{
tsFeatDE *pFeatDE;
int Idx;

if(m_hOutStatsFile == -1 || m_pWrtStatsBuff == NULL)
	return(eBSFSuccess);

if(m_bWrtStatHdr)
	{
	m_bWrtStatHdr = false;
	m_WrtStatsBuffOfs = sprintf((char *)m_pWrtStatsBuff,"\"Classification\",\"Feat\",\"FeatLen\",\"Exons\",\"Score\",\"DECntsScore\",\"PearsonScore\",\"CtrlUniqueLoci\",\"ExprUniqueLoci\",\"CtrlExprLociRatio\",\"PValueMedian\",\"PValueLow95\",\"PValueHi95\",\"TotCtrlCnts\",\"TotExprCnts\",\"TotCtrlExprCnts\",\"ObsFoldChange\",\"FoldMedian\",\"FoldLow95\",\"FoldHi95\",\"ObsPearson\",\"PearsonMedian\",\"PearsonLow95\",\"PearsonHi95\"\n");
	}
else
	m_WrtStatsBuffOfs = 0;

if(m_NumFeatsDEd > 0)
	{
	pFeatDE = m_pFeatDEs;
	for(Idx = 0; Idx < m_NumFeatsDEd; Idx++,pFeatDE++)
		{
		if(m_bFiltNonaligned && pFeatDE->SumCtrlExprCnts < 1)
			continue;
		if((m_WrtStatsBuffOfs + 10000) > m_AllocStatsWrtBuff)
			{
			CUtility::SafeWrite(m_hOutStatsFile,m_pWrtStatsBuff,m_WrtStatsBuffOfs);
			m_WrtStatsBuffOfs = 0;
			}

		if(pFeatDE->SumCtrlExprCnts < 1)
			{
			m_WrtStatsBuffOfs += sprintf((char *)&m_pWrtStatsBuff[m_WrtStatsBuffOfs],"%d,\"%s\",%d,%d,0,0,0,0,0,0.0,0.0,0.0,0.0,0,0,0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0\n",pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons);
			continue;
			}

		if((m_WrtStatsBuffOfs + 1000) > m_AllocStatsWrtBuff)
			{
			CUtility::SafeWrite(m_hOutStatsFile,m_pWrtStatsBuff,m_WrtStatsBuffOfs);
			m_WrtStatsBuffOfs = 0;
			}

	double LociRatio;
	if(pFeatDE->TotExprStartLoci > 0)
		LociRatio = (double)pFeatDE->TotCtrlStartLoci/(double)pFeatDE->TotExprStartLoci;
	else
		LociRatio = (double)pFeatDE->TotCtrlStartLoci + 0.01;

		m_WrtStatsBuffOfs += sprintf((char *)&m_pWrtStatsBuff[m_WrtStatsBuffOfs],"%d,\"%s\",%d,%d,%d,%d,%d,%d,%d,%f,%f,%f,%f,%d,%d,%d,%f,%f,%f,%f,%f,%f,%f,%f\n",
			pFeatDE->UserClass,pFeatDE->szFeatName,pFeatDE->FeatLen,pFeatDE->NumExons,pFeatDE->DEscore,pFeatDE->CntsScore,pFeatDE->PearsonScore,
			pFeatDE->TotCtrlStartLoci,pFeatDE->TotExprStartLoci,LociRatio,pFeatDE->PValueMedian,pFeatDE->PValueLow95,pFeatDE->PValueHi95,
			pFeatDE->CtrlCnts,pFeatDE->ExprCnts,pFeatDE->SumCtrlExprCnts,
			ClampFoldChange(pFeatDE->ObsFoldChange),ClampFoldChange(pFeatDE->FoldMedian),
			ClampFoldChange(pFeatDE->FoldLow95),ClampFoldChange(pFeatDE->FoldHi95),pFeatDE->PearsonObs,pFeatDE->PearsonMedian,pFeatDE->PearsonLow95,pFeatDE->PearsonHi95);
		}
	}

if(m_WrtStatsBuffOfs)
	{
	CUtility::SafeWrite(m_hOutStatsFile,m_pWrtStatsBuff,m_WrtStatsBuffOfs);
	m_WrtStatsBuffOfs = 0;
	}

return(eBSFSuccess);
}


int
AddDEPearsons(tsThreadInstData *pThreadInst,
			char *pszFeatName,	// Pearson is for this feature
			int NumExons,      // number of exons
			int UserClass)		// user classification for this feature
{
tsFeatDE *pFeatDE;
int CurFeatID;

tsAlignBin *pAlignBin;
UINT32 *pCtrlBin;
UINT32 *pExprBin;
UINT32 BinLen;
int Idx;

DEAcquireSerialise();
pFeatDE = &m_pFeatDEs[m_NumFeatsDEd++];
CurFeatID = m_NumFeatsDEd;
DEReleaseSerialise();

memset(pFeatDE,0,sizeof(tsFeatDE));
strcpy(pFeatDE->szFeatName,pszFeatName);
pFeatDE->FeatLen = pThreadInst->CurFeatLen;
pFeatDE->NumExons = NumExons;
pFeatDE->UserClass = UserClass;
// defaults in case filtering results in scores not being generated
pFeatDE->CntsScore = eDEIndeterminate;
pFeatDE->PearsonScore = ePSIndeterminate;
pFeatDE->ObsFoldChange = 0.0;

if(pThreadInst->NumBinsWithLoci < 1)
	return(CurFeatID);


pAlignBin = pThreadInst->pAlignBins;
pCtrlBin = &pFeatDE->BinsCtrlDepth[0];
pExprBin = &pFeatDE->BinsExprDepth[0];
for(Idx = 0; Idx < m_NumBins; Idx++, pAlignBin++,pCtrlBin++,pExprBin++)
	{
	BinLen = (1 + pAlignBin->BinRelEndLoci - pAlignBin->BinRelStartLoci);

	if(pAlignBin->ControlCoverage > 0)
		{
		pFeatDE->TotCtrlStartLoci += pAlignBin->NumCtrlInstStarts;
		pAlignBin->ControlCoverage = (pAlignBin->ControlCoverage + (BinLen/2))  / BinLen; // normalise for length of bin to get depth, rounding up ...
		if(pAlignBin->ControlCoverage == 0)
			pAlignBin->ControlCoverage = 1;
		*pCtrlBin = pAlignBin->ControlCoverage;
		pFeatDE->CtrlCnts += pAlignBin->ControlCnts;
		if(pAlignBin->ExperimentCoverage == 0)
			pFeatDE->BinsExclCtrl += 1;
		}

	if(pAlignBin->ExperimentCoverage > 0)
		{
		pFeatDE->TotExprStartLoci += pAlignBin->NumExprInstStarts;
		pAlignBin->ExperimentCoverage = (pAlignBin->ExperimentCoverage + BinLen - 1)  / BinLen; // normalise for length of bin to get depth, rounding up ...
		if(pAlignBin->ExperimentCoverage == 0)
			pAlignBin->ExperimentCoverage = 1;
		*pExprBin = pAlignBin->ExperimentCoverage;
		pFeatDE->ExprCnts += pAlignBin->ExperimentCnts;
		if(pAlignBin->ControlCoverage == 0)
			pFeatDE->BinsExclExpr += 1;
		}

	if(pAlignBin->ControlCoverage > 0 && pAlignBin->ExperimentCoverage > 0)
		pFeatDE->BinsShared += 1;
	}
pFeatDE->SumCtrlExprCnts = pFeatDE->CtrlCnts+pFeatDE->ExprCnts;

if((pFeatDE->CtrlCnts >= m_MinFeatCntThres || pFeatDE->ExprCnts >= m_MinFeatCntThres) &&
	(pFeatDE->TotCtrlStartLoci >= m_MinStartLociThres || pFeatDE->TotExprStartLoci >= m_MinStartLociThres))
	{
	pFeatDE->PearsonObs = Pearsons(pThreadInst->pAlignBins);
	pFeatDE->PValueMedian = PearsonsPValue(pThreadInst,pFeatDE->PearsonObs,m_MaxConfidenceIterations,pThreadInst->pAlignBins,
									&pFeatDE->PValueLow95,&pFeatDE->PValueHi95,
									&pFeatDE->PearsonLow95,&pFeatDE->PearsonHi95,&pFeatDE->PearsonMedian,
									&pFeatDE->FoldLow95,&pFeatDE->FoldHi95,&pFeatDE->FoldMedian);
	if(pFeatDE->CtrlCnts >= 1)
		pFeatDE->ObsFoldChange = (double)pFeatDE->ExprCnts/(double)pFeatDE->CtrlCnts;
	else
		pFeatDE->ObsFoldChange = (double)pFeatDE->ExprCnts * 1.0001;

	// characterise the Pearson (-1.0 to 1.0) into 1 of 4 classes
	if(pFeatDE->PearsonMedian >= cHiPearsonThres)
		pFeatDE->PearsonScore = ePSHi;
	else
		{
		if(pFeatDE->PearsonMedian >= cModPearsonThes)
			pFeatDE->PearsonScore = ePSMod;
		else
			{
			if(pFeatDE->PearsonMedian >= cLoPearsonThres)
				pFeatDE->PearsonScore = ePSLow;
			else
				pFeatDE->PearsonScore = ePSNone;
			}
		}

    // characterise the FoldMedian (0.0 to n.0) into 1 of 4 classes
	double AbsFoldMedian;
	AbsFoldMedian = ClampFoldChange(pFeatDE->FoldMedian);
	if(pFeatDE->FoldMedian >= 0.1)
		{
		if(AbsFoldMedian < 1.0)
			AbsFoldMedian = 1.0 / AbsFoldMedian;
		if(AbsFoldMedian <= cNoFoldChange)
			pFeatDE->CntsScore = eDESNone;
		else
			{
			if(AbsFoldMedian <= cLoFoldChange)
				pFeatDE->CntsScore = eDSElow;
			else
				{
				if(AbsFoldMedian <= cModFoldChange)
					pFeatDE->CntsScore = eDESMod;
				else
					pFeatDE->CntsScore = eDEHi;
				}
			}
		}
	else
		pFeatDE->CntsScore = eDEHi;
	pFeatDE->DEscore = pFeatDE->CntsScore * pFeatDE->PearsonScore;
	if(pFeatDE->DEscore > 4)			// 0,1,2,3,4,6,8,9,12,16
		{
		pFeatDE->DEscore -= 1;
		if(pFeatDE->DEscore > 5)		// 0,1,2,3,4,5,7,8,11,15
			{
			pFeatDE->DEscore -= 1;
			if(pFeatDE->DEscore > 7)	// 0,1,2,3,4,5,6,7,10,14
				{
				pFeatDE->DEscore -= 2;
				if(pFeatDE->DEscore > 8)	// 0,1,2,3,4,5,6,7,8,12
					pFeatDE->DEscore -= 3;	// 0,1,2,3,4,5,6,7,8,9
				}
			}
		}
	}

return(CurFeatID);
}


// SortAlignments
// Sort by ascending chrom identifier, loci, strand, control, experiment
static int
SortAlignments(const void *arg1, const void *arg2)
{
tsAlignReadLoci *pEl1 = (tsAlignReadLoci *)arg1;
tsAlignReadLoci *pEl2 = (tsAlignReadLoci *)arg2;

if(pEl1->ChromID < pEl2->ChromID)
		return(-1);
if(pEl1->ChromID > pEl2->ChromID)
	return(1);

if(pEl1->Loci < pEl2->Loci )
		return(-1);
if(pEl1->Loci > pEl2->Loci )
	return(1);
if(pEl1->Sense > pEl2->Sense)
		return(-1);
if(pEl1->Sense < pEl2->Sense )
	return(1);

if(pEl1->ExprFlag < pEl2->ExprFlag )
		return(-1);
if(pEl1->ExprFlag > pEl2->ExprFlag )
	return(1);

return(0);
}

// CmpLoose
// Returns 0 if Ctrl within Delta of Expr, -1 if Ctrl more than Delta below Expr, 1 if Ctrl more than Delta above Expr
int
CmpLoose(double Delta,double Ctrl, double Expr)
{
if(Ctrl < (Expr - Delta))
	return(1);
if(Ctrl > (Expr + Delta))
	return(-1);
return(0);
}


// SortDEScore
// Sort by DEscore decending then FoldMedian ascending values
static int
SortDEScore(const void *arg1, const void *arg2)
{
tsFeatDE *pEl1 = (tsFeatDE *)arg1;
tsFeatDE *pEl2 = (tsFeatDE *)arg2;

if(pEl1->DEscore < pEl2->DEscore)
	return(1);
if(pEl1->DEscore > pEl2->DEscore)
	return(-1);

if(pEl1->FoldMedian < pEl2->FoldMedian)
	return(-1);
if(pEl1->FoldMedian > pEl2->FoldMedian)
	return(1);
return(0);
}

// SortFoldMedian
static int
SortFoldMedian(const void *arg1, const void *arg2)
{
tsFeatDE *pEl1 = *(tsFeatDE **)arg1;
tsFeatDE *pEl2 = *(tsFeatDE **)arg2;

double Med1 = pEl1->FoldMedian;
double Med2 = pEl2->FoldMedian;
if(Med1 < 1.0)
	Med1 = 1.0/Med1;
if(Med2 < 1.0)
	Med2 = 1.0/Med1;

if(Med1 < Med2)
	return(-1);
if(Med1 > Med1)
	return(1);
return(0);
}


// SortDoubles
// Sort by ascending values (lower values at start of sorted array)
static int
SortDoubles(const void *arg1, const void *arg2)
{
double *pEl1 = (double *)arg1;
double *pEl2 = (double *)arg2;

if(*pEl1 < *pEl2)
	return(-1);
if(*pEl1 > *pEl2)
	return(1);
return(0);
}

/*HEADER
	Module:       z.c
	Purpose:      compute approximations to normal z distribution probabilities
	Programmer:   Gary Perlman
	Organization: Wang Institute, Tyngsboro, MA 01879
	Tester:       compile with -DZTEST to include main program
	Copyright:    none
	Tabstops:     4
*/

/*LINTLIBRARY*/
static char sccsfid[] = "@(#) z.c 5.1 (|stat) 12/26/85";
#include	<math.h>

#define	Z_EPSILON      0.000001       /* accuracy of critz approximation */
#define	Z_MAX          6.0            /* maximum meaningful z value */


double	critz (double	p);


/*FUNCTION poz: probability of normal z value */
/*ALGORITHM
	Adapted from a polynomial approximation in:
		Ibbetson D, Algorithm 209
		Collected Algorithms of the CACM 1963 p. 616
	Note:
		This routine has six digit accuracy, so it is only useful for absolute
		z values < 6.  For z values >= to 6.0, poz() returns 0.0.
*/
double            /*VAR returns cumulative probability from -oo to z */
poz (double	z)        /*VAR normal z value */
	{
	double	y, x, w;

	if (z == 0.0)
		x = 0.0;
	else
		{
		y = 0.5 * fabs (z);
		if (y >= (Z_MAX * 0.5))
			x = 1.0;
		else if (y < 1.0)
			{
			w = y*y;
			x = ((((((((0.000124818987 * w
				-0.001075204047) * w +0.005198775019) * w
				-0.019198292004) * w +0.059054035642) * w
				-0.151968751364) * w +0.319152932694) * w
				-0.531923007300) * w +0.797884560593) * y * 2.0;
			}
		else
			{
			y -= 2.0;
			x = (((((((((((((-0.000045255659 * y
				+0.000152529290) * y -0.000019538132) * y
				-0.000676904986) * y +0.001390604284) * y
				-0.000794620820) * y -0.002034254874) * y
				+0.006549791214) * y -0.010557625006) * y
				+0.011630447319) * y -0.009279453341) * y
				+0.005353579108) * y -0.002141268741) * y
				+0.000535310849) * y +0.999936657524;
			}
		}
	return (z > 0.0 ? ((x + 1.0) * 0.5) : ((1.0 - x) * 0.5));
	}

/*FUNCTION critz: compute critical z value to produce given probability */
/*ALGORITHM
	Begin with upper and lower limits for z values (maxz and minz)
	set to extremes.  Choose a z value (zval) between the extremes.
	Compute the probability of the z value.  Set minz or maxz, based
	on whether the probability is less than or greater than the
	desired p.  Continue adjusting the extremes until they are
	within Z_EPSILON of each other.
*/
double        /*VAR returns z such that fabs (poz(p) - z) <= .000001 */
critz (double	p)    /*VAR critical probability level */
	{
	double	minz = -Z_MAX;    /* minimum of range of z */
	double	maxz = Z_MAX;     /* maximum of range of z */
	double	zval = 0.0;       /* computed/returned z value */
	double	pval;			  /* prob (z) function, pval := poz (zval) */

	if (p <= 0.0 || p >= 1.0)
		return (0.0);

	while (maxz - minz > Z_EPSILON)
		{
		pval = poz (zval);
		if (pval > p)
			maxz = zval;
		else
			minz = zval;
		zval = (maxz + minz) * 0.5;
		}
	return (zval);
	}
