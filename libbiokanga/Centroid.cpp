// Copyright 2013 CSIRO  ( http://www.csiro.au/ ) 
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License
//   Please contact stuart.stephen@csiro.au for support or 
//   to submit modifications to this source

#include "stdafx.h"
#ifdef _WIN32
#include "./commhdrs.h"
#else
#include "./commhdrs.h"
#endif

CCentroid::CCentroid(void)
{
m_pCentroidParams = NULL;
m_pTransMatrices = NULL;
m_NumCentroids = 0;
m_NumProbMatrices = 0;
m_CentroidNMer = 0;
m_TransMatricesNMer = 0;
m_szCentroidParamFile[0] = '\0';
m_szTransMatricesFile[0] = '\0';
}

CCentroid::~CCentroid(void)
{
if(m_pCentroidParams != NULL)
	delete m_pCentroidParams;
if(m_pTransMatrices != NULL)
	delete m_pTransMatrices;
}

// StructParamsLoaded
// Returns true if structural parameters loaded
bool
CCentroid::CentroidParamsLoaded(void)
{
return(m_pCentroidParams == NULL ? false : true);
}

// LoadCentroidParams
// Load centroid parameters from file
// This file is a CSV file as generated by proccentroids
// File layout is that of one comma separated set of parameters per line
//
// "Chrom",SeqID,"Centroid","Centroid3","Seq",IGPFixed,IGPTrans,IGPTransv,US5PFixed,US5PTrans,US5PTransv,
// -- continued -- UTR5PFixed,UTR5PTrans,UTR5PTransv,CDSPFixed,CDSPTrans,CDSPTransv,IntronPFixed,IntronPTrans,IntronPTransv,
// -- continued -- UTR3PFixed,UTR3PTrans,UTR3PTransv,DS3PFixed,DS3PTrans,DS3PTransv
// Currently we are only interested in processing SeqID,IGPFixed,US5PFixed,UTR5PFixed,CDSFixed,IntronPFixed,UTR3PFixed and DS3PFixed
//
// Note:It is assumed that the file contains all n-Mer centroids where n could be 1,3,5 or 7
// The total number of entries = 4^n, thus if the file contains 7-Mer centroids then it is expected that there will
// be 4^7 (16384) entries.
// n			Expected Entries
// 1-mer		4
// 3-mer		64
// 5-mer		1024
// 7-mer		16384
teBSFrsltCodes 
CCentroid::LoadCentroidParams(char *pszCentroidParamsFile) // load  parameters from file
{
FILE *pParamsStream;
int LineNum;
int NumParams;
char szLineBuff[4096];
int Cnt;
int CentroidIdx;
double IGFixProb,USFixProb,UTR5FixProb,CDSFixProb,IntronFixProb,UTR3FixProb,DSFixProb;
tsCentroidParam *pCentroid1;
char chr, *pDst, *pSrc;

m_CentroidNMer = 0;
m_NumCentroids = 0;
if(m_pCentroidParams == NULL)
	{
	// allocate for 7-mer even if 1-mer processed
	m_pCentroidParams = (tsCentroidParam *) new unsigned char[cCentroidParamAllocSize];
	if(m_pCentroidParams == NULL)
		{
		AddErrMsg("CCentroid::LoadCentroidParams","Unable to allocate memory to hold centroid parameters");
		return(eBSFerrMem);
		}
	}
memset(m_pCentroidParams,0,cCentroidParamAllocSize);
m_szCentroidParamFile[0] = '\0';

if((pParamsStream = fopen(pszCentroidParamsFile,"r"))==NULL)
	{
	AddErrMsg("CCentroid::LoadCentroidParams","Unable to open parameters file %s error: %s",pszCentroidParamsFile,strerror(errno));
	delete m_pCentroidParams;
	m_pCentroidParams = NULL;
	return(eBSFerrOpnFile);
	}
LineNum = 0;
NumParams = 0;
while(fgets(szLineBuff,sizeof(szLineBuff),pParamsStream)!= NULL)
	{
	LineNum++;
	if(strlen(szLineBuff) < 5)	// simply slough lines which are too short to contain anything worth parsing
		continue;

	// strip any whitespace and quotes
	pDst = pSrc = szLineBuff;
	while(chr = *pSrc++)
		if(!isspace(chr) && chr != '\'' && chr != '"')
			*pDst++ = chr;
	*pDst = '\0';
	if(szLineBuff[0] == '\0')
		continue;

	 Cnt = sscanf(szLineBuff," %*[^,], %d , %*[^,], %*[^,], %*[^,], %lf , %*f , %*f , %lf , %*f , %*f , %lf , %*f , %*f , %lf , %*f , %*f , %lf , %*f , %*f , %lf , %*f , %*f , %lf",
			&CentroidIdx,&IGFixProb,&USFixProb,&UTR5FixProb,&CDSFixProb,&IntronFixProb,&UTR3FixProb,&DSFixProb);
	 if(Cnt != 8 && LineNum==1)	// if not expected format then assume its the header line only on the 1st line
		continue;

	 if(Cnt != 8)
		{
		AddErrMsg("CCentroid::LoadCentroidParams","Error parsing centroids parameters file %s at line %d, expected 8 but only parsed %d parameters\n%s\n",pszCentroidParamsFile,LineNum,Cnt,szLineBuff);
		fclose(pParamsStream);
		delete m_pCentroidParams;
		m_pCentroidParams = NULL;
		return(eBSFerrCentroidParam); 
		}
	 if(CentroidIdx < 0 || CentroidIdx > 16383)
		{
		AddErrMsg("CCentroid::LoadCentroidParams","CentroidsIdx outside of expected range in parameters file %s at line %d, expected between 1 and 16384 but parsed %d\n%s\n",pszCentroidParamsFile,LineNum,CentroidIdx,szLineBuff);
		fclose(pParamsStream);
		delete m_pCentroidParams;
		m_pCentroidParams = NULL;
		return(eBSFerrCentroidParam); 
		}
	pCentroid1 = &m_pCentroidParams[CentroidIdx];
	pCentroid1->Param.IGFixProb = (int)(IGFixProb * 10000.0);
	pCentroid1->Param.USFixProb = (int)(USFixProb * 10000.0);
	pCentroid1->Param.UTR5FixProb = (int)(UTR5FixProb * 10000.0);
	pCentroid1->Param.CDSFixProb = (int)(CDSFixProb * 10000.0);
	pCentroid1->Param.IntronFixProb = (int)(IntronFixProb * 10000.0);
	pCentroid1->Param.UTR3FixProb = (int)(UTR3FixProb * 10000.0);
	pCentroid1->Param.DSFixProb = (int)(DSFixProb * 10000.0);
	NumParams++;
	}
fclose(pParamsStream);
switch(NumParams) {
	case 4:
		m_CentroidNMer = 1;
		break;
	case 64: 
		m_CentroidNMer = 3;
		break;
	case 1024: 
		m_CentroidNMer = 5;
		break;
	case 16384:
		m_CentroidNMer = 7;
		break;
	default:
		AddErrMsg("CCentroid::LoadCentroidParams","Error, missing structural properties for some centroids in '%s' - %d had properties\n",pszCentroidParamsFile,NumParams);
		delete m_pCentroidParams;
		m_pCentroidParams = NULL;
		return(eBSFerrCentroidParam); 
	}

m_NumCentroids = NumParams;
strncpy(m_szCentroidParamFile,pszCentroidParamsFile,_MAX_PATH-1);
m_szCentroidParamFile[_MAX_PATH-1] = '\0';
return(eBSFSuccess);
}

// load transitional probabilities matrices from file
teBSFrsltCodes 
CCentroid::LoadTransMatrices(char *pszTransMatricesFile) 
{
FILE *pParamsStream;
int LineNum;
int ChrsParsed;
int LineOfs;
int NumParams;
int RowIdx;
char szLineBuff[4096];
int Cnt;
int MatrixIdx;
int Region;
tsTransProbMatrix ProbMatrix;
tsTransProbMatrix *pMatrix;
double *pProb;
char chr, *pDst, *pSrc;

m_TransMatricesNMer = 0;
m_NumCentroids = 0;
if(m_pTransMatrices == NULL)
	{
	// allocate for 7-mer even if 1-mer processed
	m_pTransMatrices = (tsTransProbMatrix *) new unsigned char[cTransMatixAllocSize];
	if(m_pTransMatrices == NULL)
		{
		AddErrMsg("CCentroid::LoadTransMatrices","Unable to allocate memory to hold transistion probabilities matrices");
		return(eBSFerrMem);
		}
	}
memset(m_pTransMatrices,0,cTransMatixAllocSize);
m_szTransMatricesFile[0] = '\0';

if((pParamsStream = fopen(pszTransMatricesFile,"r"))==NULL)
	{
	AddErrMsg("CCentroid::LoadTransMatrices","Unable to open matrices file %s error: %s",pszTransMatricesFile,strerror(errno));
	delete m_pTransMatrices;
	m_pTransMatrices = NULL;
	return(eBSFerrOpnFile);
	}
LineNum = 0;
NumParams = 0;
while(fgets(szLineBuff,sizeof(szLineBuff),pParamsStream)!= NULL)
	{
	LineNum++;
	if(strlen(szLineBuff) < 5)	// simply slough lines which are too short to contain anything worth parsing
		continue;

	// strip any whitespace and quotes
	pDst = pSrc = szLineBuff;
	while(chr = *pSrc++)
		if(!isspace(chr) && chr != '\'' && chr != '"')
			*pDst++ = chr;
	*pDst = '\0';
	if(szLineBuff[0] == '\0')
		continue;

	 Cnt = sscanf(szLineBuff," %*[^,], %d , %*[^,], %*[^,], %*[^,]%n",
			&MatrixIdx,&LineOfs);
	 if(Cnt != 1 && LineNum==1)	// if not expected format then assume its the header line only on the 1st line
		continue;

	 if(Cnt != 1)
		{
		AddErrMsg("CCentroid::LoadTransMatrices","Error parsing matrices file %s at line %d, invalid format\n%s\n",pszTransMatricesFile,LineNum,Cnt,szLineBuff);
		fclose(pParamsStream);
		delete m_pTransMatrices;
		m_pTransMatrices = NULL;
		return(eBSFerrProbMatrices); 
		}

	 if(MatrixIdx < 0 || MatrixIdx > 16383)
		{
		AddErrMsg("CCentroid::LoadTransMatrices","MatrixIdx outside of expected range in parameters file %s at line %d, expected between 0 and 16383 but parsed %d\n%s\n",pszTransMatricesFile,LineNum,MatrixIdx,szLineBuff);
		fclose(pParamsStream);
		delete m_pTransMatrices;
		m_pTransMatrices = NULL;
		return(eBSFerrProbMatrices); 
		}

	for(Region = 0; Region < 7; Region++)
		{
		pProb = &ProbMatrix.Els[Region][0][0];
		for(RowIdx = 0; RowIdx < 4; RowIdx++,pProb += 4)
			{
			Cnt = sscanf(&szLineBuff[LineOfs]," , %lf , %lf , %lf , %lf %n",
					pProb,pProb+1,pProb+2,pProb+3,&ChrsParsed);
			if(Cnt != 4)
				break;
			LineOfs += ChrsParsed;
			}
		if(RowIdx != 4)
			break;
		}
	if(RowIdx != 4 && Region != 7)
		{
		AddErrMsg("CCentroid::LoadTransMatrices","Missing or invalid probability value in file %s at line %d\n%s\n",
				pszTransMatricesFile,LineNum,szLineBuff);
		fclose(pParamsStream);
		delete m_pTransMatrices;
		m_pTransMatrices = NULL;
		return(eBSFerrProbMatrices); 

		}

	pMatrix = &m_pTransMatrices[MatrixIdx];
	*pMatrix = ProbMatrix;
	NumParams++;
	}
fclose(pParamsStream);
switch(NumParams) {
	case 4: 
		m_TransMatricesNMer = 1;
		break;

	case 64: 
		m_TransMatricesNMer = 3;
		break;

	case 1024:
		m_TransMatricesNMer = 5;
		break;

	case 16384:
		m_TransMatricesNMer = 7;
		break;

	default:
		AddErrMsg("CCentroid::LoadTransMatrices","Error, missing probabilities matrices in '%s' - %d were defined\n",pszTransMatricesFile,NumParams);
		delete m_pTransMatrices;
		m_pTransMatrices = NULL;
		return(eBSFerrProbMatrices); 
	}

m_NumProbMatrices = NumParams;
strncpy(m_szTransMatricesFile,pszTransMatricesFile,_MAX_PATH-1);
m_szTransMatricesFile[_MAX_PATH-1] = '\0';
return(eBSFSuccess);

}

// OligoIdx
// maps oligo onto index (0..n)
// eBSFErrBase returned if any base in oligo is indeterminate - 'N' - or unrecognised
// eBSFerrParams returned if neither conformation nor transition matrices have been loaded 
int
CCentroid::OligoIdx(char *pszOligo)		// sequence
{
char Chr;
etSeqBase Seq[7];
etSeqBase *pBase = Seq;
int Len = m_CentroidNMer >= m_TransMatricesNMer ? m_CentroidNMer : m_TransMatricesNMer;
while((Chr = *pszOligo++) != '0' && Len--)
	{
	switch(Chr) {
		case 'a': case 'A':
			*pBase++ = eBaseA;
			break;
		case 'c': case 'C':
			*pBase++ = eBaseC;
			break;
		case 'g': case 'G':
			*pBase++ = eBaseG;
			break;
		case 't': case 'T':
			*pBase++ = eBaseT;
			break;
		default:
			return(eBSFErrBase);
		}
	}
return(OligoIdx(Seq));
}

// OligoIdx
// maps oligo onto index (0..n)
// eBSFErrBase returned if any base in oligo is indeterminate - 'N' or unrecognised
// eBSFerrParams returned if neither conformation nor transition matrices have been loaded 
int
CCentroid::OligoIdx(etSeqBase *pOligo)		// sequence
{
etSeqBase Base;
int Idx = 0;
int Len ;
if(pOligo == NULL || (m_TransMatricesNMer == 0 && m_CentroidNMer == 0))
	return((int)eBSFerrParams);
Len = m_CentroidNMer >= m_TransMatricesNMer ? m_CentroidNMer : m_TransMatricesNMer;
while(Len--)
	{
	Idx <<= 2;
	Base = *pOligo++  & ~cRptMskFlg;
	if(Base > eBaseT)
		return((int)eBSFErrBase);								// unrecognised base
	Idx |= Base;
	}
return(Idx);
}


teBSFrsltCodes
CCentroid::GetSequenceCentroids(teFuncRegion Param,		// which centroid parameter value to return
				 unsigned int iStartOfs, // initial starting offset (0..n) in pSeq
				  unsigned int iNumSteps,		  // number of steps (0 for all) to process starting at pSeq[iStartPsn]|pSeq[iStartPsn+1]
  				  unsigned int SeqLen,			  // total length of sequence
				  etSeqBase *pSeq,				  // sequence to be processed
				  int *pRetConfValue,			  // where to return centroid value
				  int UndefBaseValue)			  // value to return for undefined or indeterminate ('N') bases 
{
unsigned int Step;
unsigned int LastStep;
int IdxRetVal;

if(!m_CentroidNMer || (int)SeqLen < m_CentroidNMer || iStartOfs >= SeqLen - 1 || 
   iStartOfs + iNumSteps > SeqLen ||
   pSeq == NULL || m_pCentroidParams == NULL)
	return(eBSFerrParams);

if(iNumSteps == 0)
	iNumSteps = SeqLen - iStartOfs - 1;
LastStep = iStartOfs + iNumSteps;
for(IdxRetVal =0,Step = iStartOfs; Step < LastStep; IdxRetVal++,Step++)
	pRetConfValue[IdxRetVal] = CentroidValue(Param,Step,SeqLen,pSeq,UndefBaseValue);
return(eBSFSuccess);
}

int
CCentroid::CentroidValue(teFuncRegion Param,		// which centroid parameter value to return
			unsigned int Step,			// which step (0..n) in sequence to return centroid value for
			unsigned int SeqLen,		// total length of sequence
			etSeqBase *pSeq,			// sequence to be processed
			int UndefBaseValue)			// value to return for undefined or indeterminate ('N') bases 
{
tsCentroidParam *pCentroid;
etSeqBase *pOligo = NULL;
int Idx;
int ParamOfs;

switch(Param) {
	case eFRIntergenic:				
		ParamOfs = offsetof(tsCentroidParam,Param.IGFixProb);
		break;

	case eFRUpstream:	
		ParamOfs = offsetof(tsCentroidParam,Param.USFixProb);
		break;

	case eFR5UTR:	
		ParamOfs = offsetof(tsCentroidParam,Param.UTR5FixProb);
		break;

	case eFRCDS:
		ParamOfs = offsetof(tsCentroidParam,Param.CDSFixProb);
		break;

	case eFRIntronic:		
		ParamOfs = offsetof(tsCentroidParam,Param.IntronFixProb);
		break;

	case eFR3UTR:
		ParamOfs = offsetof(tsCentroidParam,Param.UTR3FixProb);
		break;

	case eFRDnstream:	
		ParamOfs = offsetof(tsCentroidParam,Param.DSFixProb);
		break;

	default:
		return(eBSFerrParams);
	};

switch(m_CentroidNMer) {
	case 1:					// single base
		if(Step >= 0 && Step <= SeqLen - 1)
			pOligo = pSeq + Step;
		break;
	case 3:				// 3
		if(Step >= 1 && Step <= SeqLen - 2)
			pOligo = pSeq + Step - 1;
		break;
	case 5:				// 5
		if(Step >= 2 && Step <= SeqLen - 3)
			pOligo = pSeq + Step - 2;
		break;
	case 7:				// 7
		if(Step >= 3 && Step <= SeqLen - 4)
			pOligo = pSeq + Step - 3;
		break;
	}
if(pOligo != NULL)
	{
	if((Idx = OligoIdx(pOligo)) < 0)
		return(UndefBaseValue);
	else
		{
		pCentroid = &m_pCentroidParams[Idx];
		return(*(int *)(((unsigned char *)pCentroid)+ParamOfs));
		}
	}
return(UndefBaseValue);
}

// EvolveSeq
// Evolve sequence for one generation
int				// either number of bases mutated or if negative then error code - teBSFrsltCodes
CCentroid::EvolveSeq(teFuncRegion Region, // region to evolve
				   etSeqBase *pSeq,	// sequence to be evolved (input and output)
				   int SeqLen,		// sequence length
				   int RandLociSeed, // -1 == use rand() as seed, >= 0 then use this as random loci seed
				   int RandBaseSeed) // -1 == use rand() as seed, >= 0 then use this as random base seed
{


int MutateLoci;
etSeqBase Centroid;
int Idx;
int SeqIdx;
int OligoId;
int FlankLen;
tsTransProbMatrix *pMatrix;
double *pTransProb;
double Prob[4];
double SelProbs;
double LimLo;
double LimHi;
int ExLoci;
int *pRandIdxs;		// will be initialised to a list of indexes into pSeq
int NumMutated;		// total count of all bases which were mutated
INT64 Now;		// used if random generator seeds not specified

#ifndef _WIN32
struct timeval TimeNow;
#endif

if(pSeq == NULL || Region < eFRIntergenic || Region >= eFRDnstream ||
	 SeqLen < m_TransMatricesNMer) // currently seq length must be at least NMer so can process at least one centroid
	return(eBSFerrParams);

pRandIdxs = (int *)new int [SeqLen];

for(SeqIdx = 0; SeqIdx < SeqLen; SeqIdx++)
	pRandIdxs[SeqIdx] = SeqIdx;

#ifndef _WIN32
gettimeofday(&TimeNow,NULL);
#endif

if(RandLociSeed < 0)
	{
#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER *)&Now);
	RandLociSeed = (int)(Now & 0x07f3f5ff6);
#else
	RandLociSeed = (int)(TimeNow.tv_usec & 0x07f3f5ff6);
#endif
	}

if(RandBaseSeed < 0)
	{
#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER *)&Now);
	RandBaseSeed = (int)(Now & 0x07fffffff);
#else
	RandBaseSeed = (int)(TimeNow.tv_usec & 0x07fffffff);
#endif
	}

TRandomMersenne RandLoci(RandLociSeed);		// use different generators to ensure sequences will differ for same seed
TRanrotBGenerator RandBase(RandBaseSeed);

// randomise loci index selection order
for(SeqIdx = 0; SeqIdx < (SeqLen-1); SeqIdx++)
	{
	MutateLoci = RandLoci.IRandom(SeqIdx+1, SeqLen - 1);
	ExLoci = pRandIdxs[SeqIdx];
	pRandIdxs[SeqIdx] = pRandIdxs[MutateLoci];
	pRandIdxs[MutateLoci] = ExLoci;
	}

// determine number of flanking bases around centroid
switch(m_TransMatricesNMer) {
	case 1:
		FlankLen = 0;
		break;
	case 3:
		FlankLen = 1;
		break;
	case 5:
		FlankLen = 2;
		break;
	case 7:
		FlankLen = 3;
		break;
	}

// iterate over all sequence bases in pRandIdxs order
for(NumMutated = SeqIdx = 0; SeqIdx < SeqLen; SeqIdx++)
	{
	// select random loci on sequence at which to apply mutational event
	MutateLoci = pRandIdxs[SeqIdx];	// order was previously randomised

	if(MutateLoci < FlankLen || MutateLoci > (SeqLen - FlankLen - 1))
		{
		// can't calculate the initial (and last) FlankLen bases in the sequence
		// so the fudge is to give these bases equal probabilities of 0.25
		if(SelProbs < 0.25)
			pSeq[MutateLoci] = eBaseA;
		else
			{
			if(SelProbs < 0.5)
				pSeq[MutateLoci] = eBaseC;
			else
				{
				if(SelProbs < 0.75)
					pSeq[MutateLoci] = eBaseG;
				else
					pSeq[MutateLoci] = eBaseT;
				}
			}
		continue;
		}

	// determine centroid and oligo identifier for NMer around centroid
	Centroid = pSeq[MutateLoci] & ~cRptMskFlg;
	OligoId = OligoIdx(&pSeq[MutateLoci-FlankLen]);
	if(OligoId < 0)		// if oligo contains any other base than a,c,g or t then can't evolve this loci
		{
		continue;
		}

	// can now determine transistional probabilities for the current centroid
	pMatrix = &m_pTransMatrices[OligoId];
	pTransProb = &pMatrix->Els[Region][0][0];
	switch(Centroid) {
		case eBaseA:
			Prob[0] = pTransProb[0];				// a->a
			Prob[1] = pTransProb[1];				// a->c
			Prob[2] = pTransProb[2];				// a->g
			Prob[3] = pTransProb[3];				// a->t
			break;

		case eBaseC:
			Prob[0] = pTransProb[4];				// c->a
			Prob[1] = pTransProb[5];				// c->c
			Prob[2] = pTransProb[6];				// c->g
			Prob[3] = pTransProb[7];				// c->t
			break;

		case eBaseG:
			Prob[0] = pTransProb[8];				// g->a
			Prob[1] = pTransProb[9];				// g->c
			Prob[2] = pTransProb[10];				// g->g
			Prob[3] = pTransProb[11];				// g->t
			break;

		case eBaseT:
			Prob[0] = pTransProb[12];				// t->a
			Prob[1] = pTransProb[13];				// t->c
			Prob[2] = pTransProb[14];				// t->g
			Prob[3] = pTransProb[15];				// t->t
			break;
		}

	// transitional probabilities known
	// determine which base to substitute current centroid with
	SelProbs = RandBase.Random();
	if(SelProbs < 0.02 || SelProbs > 0.98)
		LimHi = 1.0;
	for(LimHi = LimLo = 0.0, Idx = 0; Idx < 3; Idx++)
		{
		LimHi += Prob[Idx];
		if(SelProbs >= LimLo && SelProbs < LimHi) 
			break;
		LimLo = LimHi;
		}
	if((pSeq[MutateLoci] & ~cRptMskFlg) != (etSeqBase)Idx)
		NumMutated++;
	pSeq[MutateLoci] = (etSeqBase)Idx;
	}
delete pRandIdxs;
return(NumMutated);
}


teBSFrsltCodes 
CCentroid::StationarySeqProbs(teFuncRegion Region, // region to calc stationary probs for
				   etSeqBase *pSeq,	// sequence to calc stationary probs over
				   int SeqLen,		// sequence length
				   int Period,		// which period is of interest (1..n)
				   double *pToRetA,	// where to return probabilities of A at Period N
				   double *pToRetC, // where to return probabilities of C at Period N
				   double *pToRetG, // where to return probabilities of G at Period N
				   double *pToRetT) // where to return probabilities of T at Period N
{
teBSFrsltCodes Rslt;
int Idx;
int OligoIx;
int FlankLen;
if(pSeq == NULL || Region < eFRIntergenic || Region >= eFRDnstream ||
	Period < 1 || Period > cMaxStatTransPeriods || 
	SeqLen < m_TransMatricesNMer || // currently seq length must be at least NMer so can process at least one centroid
   pToRetA == NULL || pToRetC == NULL || pToRetG == NULL || pToRetT == NULL)
	return(eBSFerrParams);

double *pProbs = new double [Period * 4];
double *pProbA = pProbs;
double *pProbC = &pProbs[Period];
double *pProbG = &pProbs[Period * 2];
double *pProbT = &pProbs[Period * 3];

// determine number of flanking bases around centroid
switch(m_TransMatricesNMer) {
	case 1:
		FlankLen = 0;
		break;
	case 3:
		FlankLen = 1;
		break;
	case 5:
		FlankLen = 2;
		break;
	case 7:
		FlankLen = 3;
		break;
	}

// can't calculate the initial (and last) FlankLen bases in the sequence
// so the fudge is to give these bases equal probabilities of 0.25
for(Idx = 0; Idx < FlankLen; Idx++)
	{
	*pToRetA++ = 0.25;	
	*pToRetC++ = 0.25;
	*pToRetG++ = 0.25;
	*pToRetT++ = 0.25;
	}

for(Idx = 0; Idx < (SeqLen-(m_TransMatricesNMer-1)); Idx++)
	{
	OligoIx = OligoIdx(&pSeq[Idx]);
	if(OligoIx < 0)
		{
		delete pProbs;
		return((teBSFrsltCodes)OligoIx);
		}
	if((Rslt = StationaryCentroidProbs(Region,OligoIx,Period,pProbA,pProbC,pProbG,pProbT))!=eBSFSuccess)
		{
		delete pProbs;
		return(Rslt);
		}
	*pToRetA++ = pProbA[Period-1];
	*pToRetC++ = pProbC[Period-1];
	*pToRetG++ = pProbG[Period-1];
	*pToRetT++ = pProbT[Period-1];
	}

delete pProbs;
// can't calculate the (initial and) last FlankLen bases in the sequence
// so the fudge is to give these bases equal probabilities of 0.25
for(Idx = 0; Idx < FlankLen; Idx++)
	{
	*pToRetA++ = 0.25;	
	*pToRetC++ = 0.25;
	*pToRetG++ = 0.25;
	*pToRetT++ = 0.25;
	}
return(eBSFSuccess);
}

teBSFrsltCodes 
CCentroid::StationarySeqProbs(teFuncRegion Region, // region to calc stationary probs for
				   char *pszSeq,	// sequence to calc stationary probs over
				   int SeqLen,		// sequence length
				   int Period,		// which period is of interest (1..n)
				   double *pToRetA,	// where to return probabilities of A at Period N
				   double *pToRetC, // where to return probabilities of C at Period N
				   double *pToRetG, // where to return probabilities of G at Period N
				   double *pToRetT) // where to return probabilities of T at Period N
{
teBSFrsltCodes Rslt;
etSeqBase *pSeq;
if(pszSeq == NULL || Region < eFRIntergenic || Region >= eFRDnstream ||
	Period < 1 || Period > cMaxStatTransPeriods || 
	SeqLen < m_TransMatricesNMer || // currently seq length must be at least NMer so can process at least one centroid
   pToRetA == NULL || pToRetC == NULL || pToRetG == NULL || pToRetT == NULL)
	return(eBSFerrParams);
 
pSeq = new unsigned char[SeqLen];
CSeqTrans::MapAscii2Sense(pszSeq,SeqLen,pSeq);
Rslt = StationarySeqProbs(Region,pSeq,SeqLen,Period,pToRetA,pToRetC,pToRetG,pToRetT);
delete pSeq;
return(Rslt);
}

teBSFrsltCodes 
CCentroid::StationaryCentroidProbs(teFuncRegion Region, // region to calc stationary probs for
			    int OligoIdx,		// uniquely identifies oligo (0..n) see CentroidParamIdx()
			    int NumPeriods,		// number of time periods (1..n)
				double *pProbA,		// where to return probabilities in each time period for A
				double *pProbC,		// where to return probabilities in each time period for C
				double *pProbG,		// where to return probabilities in each time period for G
				double *pProbT)		// where to return probabilities in each time period for T
{
int Period;
double SumProbs;
tsTransProbMatrix *pMatrix;
double *pTransProb;
etSeqBase InitialBase;

if(Region < eFRIntergenic || Region > eFRDnstream ||
	OligoIdx < 0 || OligoIdx >= m_NumProbMatrices ||
   NumPeriods < 1 || NumPeriods > cMaxStatTransPeriods ||
   pProbA == NULL || pProbC == NULL || pProbG == NULL || pProbT == NULL)
	return(eBSFerrParams);

if(m_pTransMatrices == NULL)
	return(eBSFerrFileClosed);
pMatrix = &m_pTransMatrices[OligoIdx];
pTransProb = &pMatrix->Els[Region][0][0];

// determine initial centroid base
switch(m_TransMatricesNMer) {
	case 1:
		InitialBase = (etSeqBase)(OligoIdx & 0x03);
		break;
	case 3:
		InitialBase = (etSeqBase)((OligoIdx >> 2) & 0x03);
		break;
	case 5:
		InitialBase = (etSeqBase)((OligoIdx >> 4) & 0x03);
		break;
	case 7:
		InitialBase = (etSeqBase)((OligoIdx >> 6) & 0x03);
		break;
	}

// starting probability of centroid must be 1.0
*pProbA++ = InitialBase == eBaseA ? 1.0 : 0.0;
*pProbC++ = InitialBase == eBaseC ? 1.0 : 0.0;
*pProbG++ = InitialBase == eBaseG ? 1.0 : 0.0;
*pProbT++ = InitialBase == eBaseT ? 1.0 : 0.0;

// iterate over each period starting at T, using the probabilities observed at
// T and apply the transisative static probabilities to determine T+1
for(Period = 1; Period < NumPeriods; Period++,pProbA++,pProbC++,pProbG++,pProbT++)
	{
	*pProbA = pProbA[-1] * pTransProb[0] +			// a->a
			  pProbC[-1] * pTransProb[4] +			// c->a
			  pProbG[-1] * pTransProb[8] +			// g->a
			  pProbT[-1] * pTransProb[12];			// t->a
	
	*pProbC = pProbA[-1] * pTransProb[1] +			// a->c
			  pProbC[-1] * pTransProb[5] +			// c->c 
			  pProbG[-1] * pTransProb[9] +			// g->c
			  pProbT[-1] * pTransProb[13];			// t->c

	*pProbG = pProbA[-1] * pTransProb[2] +			// a->g
			  pProbC[-1] * pTransProb[6] +			// c->g
			  pProbG[-1] * pTransProb[10] +			// g->g	
			  pProbT[-1] * pTransProb[14];			// t->g

	*pProbT = pProbA[-1] * pTransProb[3] +			// a->t
			  pProbC[-1] * pTransProb[7] +			// c->t
			  pProbG[-1] * pTransProb[11] +			// g->t
			  pProbT[-1] * pTransProb[15];			// t->t

	// probabilities over all 4 bases should sum to 1.0 but there could be small FP errors
	// adjust the base with maximal probability so all do sum to 1.0
	SumProbs = *pProbA + *pProbC + *pProbG + *pProbT;
	if(SumProbs != 1.0)
		{
		if(*pProbA >= *pProbC && *pProbA >= *pProbG && *pProbA >= *pProbT)
			{
			*pProbA = 1.0 - *pProbC - *pProbG - *pProbT;
			}
		else
			if(*pProbC >= *pProbG && *pProbC >= *pProbT)
				{
				*pProbC = 1.0 - *pProbA - *pProbG - *pProbT;
				}
			else
				if(*pProbG >= *pProbT)
					*pProbG = 1.0 - *pProbA - *pProbC - *pProbT;
				else
					*pProbT = 1.0 - *pProbA - *pProbC - *pProbG;
		}
	}

return(eBSFSuccess);
}

