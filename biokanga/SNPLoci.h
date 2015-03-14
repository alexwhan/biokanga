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

#pragma once

const int cAlignNumSNPfields = 23;			// if generated by 'biokanga align' then there will be this many CSV fields
const int cSNPMarkerNumFields = 4 + 9;		// if generated by 'biokanga snpmarkers' then there will be a minimum of this number of CSVfields

typedef struct TAG_sCultivar {
	char szName[cMaxDatasetSpeciesChrom+1];		// name of this cultivar
} tsCultivar;

class CSNPLoci
{
	char m_TargAssemblyName[cMaxDatasetSpeciesChrom+1]; // alignments were against this targeted assembly
	int m_NumCultivars;							// number of cultivars being processed
	tsCultivar m_Cultivars[cMaxCultivars];		// cultivar specific metadata
	CCSVFile *m_pCSV;							// used to load SNP calls
	int LoadSNPs(char *pszSNPFile);				// load SNP calls from this CSV file
	int LoadSeqs(char *pszSeqFile);				// load sequences from this multifasta file, SNPs were called relatative to these sequences
	int Filter(int MinSep);						// filter out SNPs which are not separated by at least this many bases from any other SNP loci 
	int Dedupe(bool bSenseOnly=false);			// remove any SNP sequences which are duplicates of other SNP sequences
	int Report(char *pszOutFile);				// report SNP sequences to this file

public:
	CSNPLoci(void);
	~CSNPLoci(void);

	void Reset(void);							// re-initialise  
	int Process(char *pszSNPFile,				// load SNP calls from this CSV file
				char *pszSeqFile,				// load sequences from this multifasta file, SNPs were called relatative to these sequences
				char *pszOutFile,				// report SNP sequences to this file
				int  Extd5,						// extend SNP 5' this many bases
				int  Extd3,						// extend SNP 3' this many bases
				int MinSep,						// filter out SNPs which are not separated by at least this many bases from any other SNP loci
				bool bSenseOnly=false);			// remove any SNP sequences which are duplicates of other SNP sequences
};
