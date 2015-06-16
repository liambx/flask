/* corrlnfields: Written by Henrique S. Xavier on Nov-2014
   e-mail: hsxavier@if.usp.br
 */

#include <iostream>
#include <gsl/gsl_linalg.h>     // Cholesky descomposition.
#include <gsl/gsl_randist.h>    // Random numbers.
#include <iomanip>              // For 'setprecision'.
#include <alm.h>
#include <healpix_map.h>
#include <alm_healpix_tools.h>
#include <omp.h>                // For OpenMP functions, not pragmas.
#include <limits.h>             // For finding out max. value of INT variables.
#include "definitions.hpp"      // Global variables and #defines.
#include "corrlnfields_aux.hpp" // Auxiliary functions made for this program.
#include "GeneralOutput.hpp"    // Various file output functions.
#include "ParameterList.hpp"    // Configuration and input system.
#include "Utilities.hpp"        // Error handling, tensor allocations.
#include "gsl_aux.hpp"          // Using and reading GSL matrices.
#include "SelectionFunc.hpp"
#include "ClProcessing.hpp"
#include "fitsfunctions.hpp"    // For WriteCatalog2Fits function.
#include <unistd.h> // debugging

#define RAND_OFFSET 10000000 // For generating random numbers in parallel, add multiples of this to seed.


/********************/
/*** Main Program ***/
/********************/
int main (int argc, char *argv[]) {
  using std::cout; using std::endl;                     // Basic stuff.
  using namespace definitions;                          // Global definitions.
  using namespace ParDef; ParameterList config;         // Easy configuration file use.
  Cosmology cosmo;                                      // Cosmological parameters.
  char message[100];                                    // Handling warnings and errors.
  std::string filename, ExitAt;
  std::ofstream outfile;                                // File for output.
  simtype dist;                                         // For specifying simulation type.
  gsl_matrix **CovByl; 
  int status, i, j, k, l, m, N1, N2, Nfields, *ftype, Nls, MaxThreads;
  double *means, *shifts, **zrange; 
  long long1, long2;
  gsl_set_error_handler_off();                                              // !!! All GSL return messages MUST be checked !!!
  


  /**********************************************/
  /*** PART 0: Test code and load config file ***/
  /**********************************************/

  // Testing the code:
  Announce("Testing the code... "); 
  // Verify consistency between f&z <-> i conversions:
  test_fzij(3,11); test_fzij(13,4);
  // Verify that max. value for INT is not smaller than expected:
  sprintf(message, "%d", INT_MAX); filename.assign(message);
  if (filename.size() < 10) 
    warning("corrlnfields: INT_MAX is smaller than expected, may mess parallel random number generator.");
  Announce();
  
  MaxThreads = omp_get_max_threads();
  cout << "Max. # of threads:  "<<MaxThreads<<endl;
  if (MaxThreads>210) warning("corrlnfields: # of threads too big, may mess parallel random number generator.");

  // Loading config file:
  if (argc<=1) { cout << "You must supply a config file." << endl; return 0;}
  config.load(argv[1]);
  cout << endl;
  cout << "-- Configuration setup:\n";
  cout << "   File: "<<argv[1]<<endl;
  config.lineload(argc, argv);
  config.show();
  cout << endl;
  cosmo.load(&config);
  ExitAt = config.reads("EXIT_AT");
  if (ExitAt!="0") config.findpar(ExitAt+":"); // Produce warning if last output is unknown.
  // - Lognormal or Gausian realizations:
  if (config.reads("DIST")=="LOGNORMAL")        dist=lognormal;
  else if (config.reads("DIST")=="GAUSSIAN")    dist=gaussian;
  else if (config.reads("DIST")=="HOMOGENEOUS") dist=homogeneous;
  else error("corrlnfields: unknown DIST: "+config.reads("DIST"));
  

  /***********************************/
  /*** PART 1: Loads fields info   ***/
  /***********************************/
  bool *fnzSet;
  double **aux;

  // Load means, shifts, type and z range data file:
  Announce("Loading means and shifts from file "+config.reads("FIELDS_INFO")+"... ");
  aux     = LoadTable<double>(config.reads("FIELDS_INFO"), &long1, &long2);
  Nfields = (int)long1;
  fnzSet  = vector<bool>     (0, Nfields-1); for (i=0; i<Nfields; i++) fnzSet[i]=0;
  means   = vector<double>   (0, Nfields-1);
  ftype   = vector<int>      (0, Nfields-1);
  zrange  = matrix<double>   (0, Nfields-1, 0,1);
  if (dist==lognormal) shifts = vector<double>(0, Nfields-1);
  // Check if some input properties are as expected:
  if  ((int)Minimum(aux, 0, Nfields)!=1) error("corrlnfields: first field index in FIELDS_INFO should be 1.");
  if  ((int)Minimum(aux, 1, Nfields)!=1) error("corrlnfields: first redshift index FIELDS_INFO should be 1.");
  N1 = (int)Maximum(aux, 0, Nfields);
  N2 = (int)Maximum(aux, 1, Nfields);
  if  (N1*N2 != Nfields)            error("corrlnfields: mismatch between number and indexes of fields.");
  // Should introduce other checks here, e.g. if there are no gaps or other issues in field IDs.
  
  // Parse information to separate arrays:
  for (j=0; j<Nfields; j++) {
    fz2n((int)aux[j][0], (int)aux[j][1], &i, N1, N2); // Find conventional position of field in arrays.
    if (fnzSet[i]==1) error ("corrlnfields: found more than one mean & shift entry for the same f-z.");
    fnzSet[i] = 1; 
    means[i]  = aux[j][2];  
    if (dist==lognormal) shifts[i] = aux[j][3];
    ftype[i] = (int)aux[j][4];
    zrange[i][0] = aux[j][5]; zrange[i][1] = aux[j][6]; 
  }
  // A few checks on the input:
  for (i=0; i<Nfields; i++) if (fnzSet[i]!=1) error("corrlnfields: the properties of a field were not set.");
  for (i=0; i<Nfields; i++) if (zrange[i][0]>zrange[i][1]) error("corrlnfields: zmin > zmax for a field.");
  if (dist==lognormal) for (i=0; i<Nfields; i++) if(means[i]+shifts[i]<=0) {
	printf(message, "corrlnfields: mean+shift at position %d must be greater than zero.", i); error(message);
      }
  free_vector(fnzSet, 0, Nfields-1);
  free_matrix(aux, 0, Nfields-1, 0, long2-1);
  Announce();
  
  cout << "Infered from FIELDS_INFO file:  Nf = " << N1 << "   Nz = " << N2 << endl;


  /**************************************************************/
  /*** PART 2: Loads mixing matrices or compute them from Cls ***/
  /**************************************************************/  
  std::string CholeskyInPrefix;
  int lmax, lmin;
  
  CholeskyInPrefix = config.reads("CHOL_IN_PREFIX");
  lmax             = config.readi("LMAX");
  lmin             = config.readi("LMIN");

  // Skip mixing matrices if generating homogeneous uncorrelated fields: matrices would be zero:
  if (dist!=homogeneous) {
  
    // If input triangular mixing matrices unspecified, compute them from input Cls:
    if (CholeskyInPrefix=="0") {
      // Load C(l)s and compute auxiliary Gaussian cov. matrices:
      status = ClProcess(&CovByl, means, shifts, N1, N2, &Nls, config);
      if (status==1) { // Exit if fast output was inside ClProcess.
	cout << "\nTotal number of warnings: " << warning("count") << endl;
	cout<<endl;
	return 0; 
      }
      cout << "Maximum l in input C(l)s: "<<Nls-1<<endl;
      cout << "Will use "<<lmin<<" <= l <= "<<lmax<<endl;
    
      // Cholesky decomposition:
      Announce("Performing Cholesky decompositions of cov. matrices... ");
      j=0; // Will count number of Cholesky failures.
      for (l=lmin; l<=lmax; l++) {
	//cout << "** Working with cov. matrix for l="<<l<<":\n";
	status = gsl_linalg_cholesky_decomp(CovByl[l]);
	if (status==GSL_EDOM) { 
	  sprintf(message,"Cholesky decomposition failed: cov. matrix for l=%d is not positive-definite.", l); 
	  warning(message); j++; 
	}
      }
      Announce();
    
      // Exit if any Cholesky failed:
      if (j>0) {sprintf(message,"Cholesky decomposition failed %d times.",j); error(message);}
      // Output mixing matrices if requested:
      GeneralOutput(CovByl, config, "CHOLESKY_PREFIX", 0);
      if (config.reads("CHOLESKY_PREFIX")!="0") 
	cout << ">> Mixing matrices written to prefix "+config.reads("CHOLESKY_PREFIX")<<endl;
    }

    // If input triangular matrices are specified, allocate memory for them:
    else {
      Announce("Allocating memory for mixing matrices (CHOL_IN_PREFIX)... ");
      CovByl = GSLMatrixArray(lmax+1, Nfields, Nfields); // Allocation should have offset to avoid unnecessary low ells.
      Announce();                                        // If we are loading the matrices ell by ell, an array is not necessary! 
      Announce("Loading mixing matrices... ");
      for (l=lmin; l<=lmax; l++) {
	filename = CholeskyInPrefix+"l"+ZeroPad(l,lmax)+".dat";
	LoadGSLMatrix(filename, CovByl[l]);
      }
      status=0;
      Announce();    
    }
  } // End of IF not homogeneous.
  else cout << "HOMOGENEOUS realizations: skipped mixing matrix preparation.\n";
 
  // Exit if dealing with mixing matrices was the last task:
  if (ExitAt=="CHOLESKY_PREFIX") {
    cout << "\nTotal number of warnings: " << warning("count") << endl;
    cout<<endl;
    return 0;
  }
  
  
  /*************************************************/
  /*** PART 4: Auxiliary Gaussian alm generation ***/
  /*************************************************/
  const double OneOverSqr2=0.7071067811865475;
  bool almout;
  double ***gaus0, ***gaus1;
  gsl_rng **rnd;
  Alm<xcomplex <ALM_PRECISION> > *aflm, *bflm;
  int jmax, jmin, rndseed0;
    
  // Set random number generators for each thread, plus one for serial stuff [0]:
  // Method is meant to:
  //                    (1) generate aux. alm's fast (in parallel);
  //                    (2) give independent samples for different RNDSEED (parallel seeds may never overlap);
  //                    (3) maintain reproducibility (seeds used in each part of computations must be the same for fixed # of threads).
  Announce("Initializing random number generators... ");
  rndseed0 = config.readi("RNDSEED");
  rnd      = vector<gsl_rng*>(0,MaxThreads+1);
  if (rndseed0 > RAND_OFFSET-1) warning("corrlnfields: RNDSEED exceeds RAND_OFFSET-1 in code.");
  for (i=0; i<=MaxThreads; i++) {
    rnd[i] = gsl_rng_alloc(gsl_rng_mt19937);
    if (rnd==NULL) error("corrlnfields: gsl_rng_alloc failed!");
    gsl_rng_set(rnd[i], i*RAND_OFFSET+rndseed0);    // set random seed
  }
  Announce();
  cout << "First random numbers: "<<gsl_rng_uniform(rnd[0])<<" "<<gsl_rng_uniform(rnd[0])<<" "<<gsl_rng_uniform(rnd[0])<<endl;

  // Skip alm generation if creating homogeneous uncorrelated fields: all would be zero:
  if (dist!=homogeneous) {
  
    // Allocate memory for gaussian alm's:
    Announce("Allocating memory for auxiliary gaussian alm's... ");
    gaus0 = tensor3<double>(1,MaxThreads, 0,Nfields-1, 0,1); // Complex random variables, [0] is real, [1] is imaginary part.
    gaus1 = tensor3<double>(1,MaxThreads, 0,Nfields-1, 0,1);  
    aflm  = vector<Alm<xcomplex <ALM_PRECISION> > >(0,Nfields-1);   // Allocate Healpix Alm objects and set their size and initial value.
    for (i=0; i<Nfields; i++) {
      aflm[i].Set(lmax,lmax);
      for(l=0; l<=lmax; l++) for (m=0; m<=l; m++) aflm[i](l,m).Set(0,0);
    }
    Announce();

    // LOOP over l's and m's together:
    Announce("Generating auxiliary gaussian alm's... ");
    jmin = (lmin*(lmin+1))/2;
    jmax = (lmax*(lmax+3))/2;
#pragma omp parallel for schedule(static) private(l, m, i, k)
    for(j=jmin; j<=jmax; j++) {
    
      // Find out which random generator to use:
      k = omp_get_thread_num()+1;
      // Find out which multipole to compute:    
      l = (int)((sqrt(8.0*j+1.0)-1.0)/2.0);
      m = j-(l*(l+1))/2;
    
      // Generate independent 1sigma complex random variables:
      if (m==0) for (i=0; i<Nfields; i++) {
	  gaus0[k][i][0] = gsl_ran_gaussian(rnd[k], 1.0);
	  gaus0[k][i][1] = 0.0;
	}                                                      // m=0 are real, so real part gets all the variance.
      else      for (i=0; i<Nfields; i++) {
	  gaus0[k][i][0] = gsl_ran_gaussian(rnd[k], OneOverSqr2);
	  gaus0[k][i][1] = gsl_ran_gaussian(rnd[k], OneOverSqr2);
	}
    
      // Generate correlated complex gaussian variables according to CovMatrix:
      CorrGauss(gaus1[k], CovByl[l], gaus0[k]);
  
      // Save alm to tensor:
      for (i=0; i<Nfields; i++) aflm[i](l,m).Set(gaus1[k][i][0], gaus1[k][i][1]);   
       
    } // End of LOOP over l's and m's.
    Announce();
    free_GSLMatrixArray(CovByl, Nls);
    free_tensor3(gaus0, 1,MaxThreads, 0,Nfields-1, 0,1);
    free_tensor3(gaus1, 1,MaxThreads, 0,Nfields-1, 0,1);
    // If requested, write alm's to file:
    GeneralOutput(aflm, config, "AUXALM_OUT", N1, N2);
  } // End of IF not homogeneous.

  else cout << "HOMOGENEOUS realizations: skipped alm generation.\n";

  // Exit if this is the last output requested:
  if (ExitAt=="AUXALM_OUT") {
    cout << "\nTotal number of warnings: " << warning("count") << endl;
    cout<<endl;
    return 0;
  }
  

  /******************************/
  /*** Part 5: Map generation ***/
  /******************************/
  int nside, npixels;
  Healpix_Map<MAP_PRECISION> *mapf;
  double expmu, gmean, gvar;
  pointing coord;
  char *arg[5];
  char opt1[]="-bar", val1[]="1";

  // Allocate memory for pixel maps:
  Announce("Allocating memory for pixel maps... "); 
  nside   = config.readi("NSIDE");
  if (nside>sqrt(INT_MAX/12)) warning("corrlnfields: NSIDE too large, number of pixels will overflow INT variables");
  npixels = 12*nside*nside;
  mapf=vector<Healpix_Map<MAP_PRECISION> >(0,Nfields-1);
  for(i=0; i<Nfields; i++) mapf[i].SetNside(nside, RING); 		
  Announce();

  // Generate maps from alm's for each field if not creating homogeneous uncorrelated fields:
  if (dist!=homogeneous) {
    Announce("Generating maps from alm's... ");
    for(i=0; i<Nfields; i++) alm2map(aflm[i],mapf[i]);
    Announce();
  }
  // Generate mean maps if creating homogeneous fields:
  else {
    Announce("HOMOGENEOUS realizations: filing maps with mean values... ");
    for(i=0; i<Nfields; i++) mapf[i].fill(means[i]);
    Announce();
  }
  
  // If generating lognormal, alm's are not needed anymore (for gaussian the klm's are used to generate shear):
  if (dist==lognormal) free_vector(aflm, 0, Nfields-1);
  // Write auxiliary map to file as a table if requested:
  GeneralOutput(mapf, config, "AUXMAP_OUT", N1, N2);
  
  // Exit if this is the last output requested:
  if (ExitAt=="AUXMAP_OUT") {
      cout << "\nTotal number of warnings: " << warning("count") << endl;
      cout<<endl;
      return 0;
  }

  // If LOGNORMAL, exponentiate pixels:
  if (dist==lognormal) {
    Announce("LOGNORMAL realizations: exponentiating pixels... ");
    for (i=0; i<Nfields; i++) {
      gmean = 0; gvar = 0;
#pragma omp parallel for reduction(+:gmean)
      for (j=0; j<npixels; j++) gmean += mapf[i][j];
      gmean = gmean/npixels;
#pragma omp parallel for reduction(+:gvar)
      for (j=0; j<npixels; j++) gvar += pow(mapf[i][j]-gmean, 2);
      gvar = gvar/(npixels-1);
      expmu=(means[i]+shifts[i])/exp(gvar/2);
#pragma omp parallel for
      for(j=0; j<npixels; j++) mapf[i][j] = expmu*exp(mapf[i][j])-shifts[i];
    }
    Announce();
  }
  // If GAUSSIAN, only add mean:
  else if (dist==gaussian) {
    Announce("GAUSSIAN realizations: adding mean values to pixels... ");
    for (i=0; i<Nfields; i++) {
#pragma omp parallel for 
      for(j=0; j<npixels; j++) mapf[i][j] = mapf[i][j] + means[i];
    }
    Announce();
  }
  // Free memory for means and shifts:
  if (dist==lognormal) free_vector(shifts, 0, Nfields-1);
  free_vector(means, 0, Nfields-1);
  
  // Write final map to file as a table if requested:
  GeneralOutput(mapf, config, "MAP_OUT", N1, N2);
  // Map output to fits and/or tga files:
  GeneralOutput(mapf, config, "MAPFITS_PREFIX", N1, N2, 1);
  
  // Exit if this is the last output requested:
  if (ExitAt=="MAP_OUT" ||
      ExitAt=="MAPFITS_PREFIX") {
      cout << "\nTotal number of warnings: " << warning("count") << endl;
      cout<<endl;
      return 0;
  }


  // If requested, compute and write recovered alm's and/or Cl's to files:
  if (config.reads("RECOVALM_OUT")!="0" || config.reads("RECOVCLS_OUT")!="0") {
    // Allocate and clear variables to receive new alm's:
    bflm  = vector<Alm<xcomplex <ALM_PRECISION> > >(0,Nfields-1);
    for (i=0; i<Nfields; i++) {
      bflm[i].Set(lmax,lmax);
      for(l=0; l<=lmax; l++) for (m=0; m<=l; m++) bflm[i](l,m).Set(0,0);
    }    
    // Compute alm's from map:
    arr<double> weight(2*mapf[0].Nside());
    weight.fill(1);
    Announce("Recovering alm's from map... ");
    for(i=0; i<Nfields; i++) map2alm(mapf[i],bflm[i],weight);
    Announce();
    weight.dealloc();
    // Output to file:
    GeneralOutput(bflm, config, "RECOVALM_OUT", N1, N2);

    // Compute Cl's if requested:
    if (config.reads("RECOVCLS_OUT")!="0") {
      double **recovCl;
      int lminout, lmaxout, mmax;
      Announce("Recovering Cl's from maps... ");
      lminout = config.readi("LRANGE_OUT", 0);
      lmaxout = config.readi("LRANGE_OUT", 1);
      mmax    = config.readi("MMAX_OUT");
      if (mmax>lminout) error ("corrlnfields: current code only allows MMAX_OUT <= LMIN_OUT.");
      status  = Nfields*(Nfields+1)/2;
      recovCl = matrix<double>(0, status-1, lminout, lmaxout);
      for (k=0; k<status; k++) {
	l = (int)((sqrt(8.0*(status-1-k)+1.0)-1.0)/2.0);
	m = status-1-k-(l*(l+1))/2;
	i = Nfields-1-l;
	j = Nfields-1-m;
	for(l=lminout; l<=lmaxout; l++) {
	  recovCl[k][l] = 0;
	  if (mmax<0) for (m=0; m<=l; m++)    recovCl[k][l] += (bflm[i](l,m)*(bflm[j](l,m).conj())).real();
	  else        for (m=0; m<=mmax; m++) recovCl[k][l] += (bflm[i](l,m)*(bflm[j](l,m).conj())).real();
	  recovCl[k][l] = recovCl[k][l]/((double)(l+1));
	}
      } // End of Cl computing.
      Announce();
      GeneralOutput(recovCl, N1, N2, config, "RECOVCLS_OUT");
      free_matrix(recovCl, 0, status-1, lminout, lmaxout); 
    }
    
    // Free memory:
    free_vector(bflm, 0, Nfields-1);
  }


  // Exit if this is the last output requested:
  if (ExitAt=="RECOVALM_OUT" || 
      ExitAt=="RECOVCLS_OUT") {
      cout << "\nTotal number of warnings: " << warning("count") << endl;
      cout<<endl;
      return 0;
  }
    

  /************************************/
  /*** Part 6: Maps to Observables  ***/
  /************************************/


  /*** Galaxy fields ***/
  
  //double PixelSolidAngle=12.56637061435917/npixels; // 4pi/npixels.
  double PixelSolidAngle = 1.4851066049791e8/npixels; // in arcmin^2.
  double dwdz;
  SelectionFunction selection;
  int f, z, *counter;
  
  // Read in selection functions from FITS files and possibly text files (for radial part):
  Announce("Reading selection functions from files... ");
  selection.load(config, ftype, zrange, N1, N2); 
  if (selection.Nside()!=-2 && selection.Nside()!=mapf[0].Nside())
    error("corrlnfields: Selection function and maps have different number of pixels.");
  if (selection.Scheme()!=-2 && selection.Scheme()!=mapf[0].Scheme()) 
    error("corrlnfields: Selection function and maps have different pixel ordering schemes.");
  Announce();

  // Poisson Sampling the galaxy fields:
  if (config.readi("POISSON")==1) {
    counter = vector<int>(1, MaxThreads);
    // LOOP over fields:
    for (i=0; i<Nfields; i++) if (ftype[i]==fgalaxies) {
	n2fz(i, &f, &z, N1, N2);
	sprintf(message, "Poisson sampling f%dz%d... ", f, z); filename.assign(message); 
	Announce(filename);
	for(k=1; k<=MaxThreads; k++) counter[k]=0;
	dwdz    = PixelSolidAngle*(zrange[i][1]-zrange[i][0]);
	// LOOP over pixels of field 'i':
#pragma omp parallel for schedule(static) private(k)
	for(j=0; j<npixels; j++) {
	  k = omp_get_thread_num()+1;
	  if (mapf[i][j] < -1.0) { counter[k]++; mapf[i][j]=0.0; } // If density is negative, set it to zero.	  
	  mapf[i][j] = gsl_ran_poisson(rnd[k], selection(i,j)*(1.0+mapf[i][j])*dwdz);	  
	}
	Announce();
	j=0; for (k=1; k<=MaxThreads; k++) j+=counter[k];
	cout << "Negative density fraction (that was set to 0): "<<std::setprecision(2)<< ((double)j)/npixels*100 <<"%\n";
      }
    free_vector(counter, 1, MaxThreads);
  }
  
  // Just generate the expected number density, if requested:
  else if (config.readi("POISSON")==0) {
    for (i=0; i<Nfields; i++) if (ftype[i]==fgalaxies) {
	n2fz(i, &f, &z, N1, N2);
	sprintf(message,"Using expected number density for f%dz%d...", f, z); filename.assign(message);
	Announce(message);
	dwdz = PixelSolidAngle*(zrange[i][1]-zrange[i][0]);
#pragma omp parallel for
	for(j=0; j<npixels; j++) mapf[i][j] = selection(i,j)*(1.0+mapf[i][j])*dwdz;
	Announce();
      }
  }
  else error ("corrlnfields: unknown POISSON option.");
  
  
  /*** Lensing fields ***/

  double coeff;
  Healpix_Map<MAP_PRECISION> *gamma1f, *gamma2f;
  gamma1f = vector<Healpix_Map <MAP_PRECISION> >(0,Nfields-1);
  gamma2f = vector<Healpix_Map <MAP_PRECISION> >(0,Nfields-1);
  Alm<xcomplex <ALM_PRECISION> > Eflm(lmax,lmax), Bflm(lmax,lmax); // Temp memory
  arr<double> weight(2*mapf[0].Nside()); weight.fill(1);           // Temp memory
  for(l=0; l<=lmax; l++) for (m=0; m<=l; m++) Bflm(l,m).Set(0,0);  // B-modes are zero for weak lensing.
  
  // LOOP over convergence fields:
  for (i=0; i<Nfields; i++) if (ftype[i]==fshear) {
      n2fz(i, &f, &z, N1, N2);
      cout << "** Will compute shear for f"<<f<<"z"<<z<<":\n";
      
      // Preparing memory:
      Announce("   Allocating and cleaning memory... ");
      gamma1f[i].SetNside(nside, RING); gamma1f[i].fill(0);
      gamma2f[i].SetNside(nside, RING); gamma2f[i].fill(0);
      Announce();
      
      // LOGNORMAL REALIZATIONS: get convergence alm's from lognormal convergence map:
      if (dist==lognormal) {
	Announce("   Transforming convergence map to harmonic space... ");
	if (lmax>nside) warning("LMAX > NSIDE introduces noise in the transformation.");
	for(l=0; l<=lmax; l++) for (m=0; m<=l; m++) Eflm(l,m).Set(0,0);
	map2alm(mapf[i], Eflm, weight); // Get klm.
	Announce();
      }
 
      if (dist!=homogeneous) {
	// Calculate shear E-mode alm's from convergence alm's:
	Announce("   Computing shear harmonic coefficients from klm... ");
	if (dist==lognormal)     Kappa2ShearEmode(Eflm, Eflm);
	else if (dist==gaussian) Kappa2ShearEmode(Eflm, aflm[i]);
	Announce();
      }
      else {
	Announce("HOMOGENEOUS realizations: setting shear E-mode to zero... ");
	for(l=0; l<=lmax; l++) for (m=0; m<=l; m++) Eflm(l,m).Set(0,0);
	Announce();
      }
      n2fz(i, &f, &z, N1, N2);
      GeneralOutput(Eflm, config, "SHEAR_ALM_PREFIX", f, z);

      // Go from shear E-mode alm's to gamma1 and gamma2 maps:
      Announce("   Transforming harmonic coefficients into shear map... ");
      alm2map_spin(Eflm, Bflm, gamma1f[i], gamma2f[i], 2);
      Announce();
      // Write kappa, gamma1 and gamma2 to FITS file:
      n2fz(i, &f, &z, N1, N2);
      GeneralOutput(mapf[i], gamma1f[i], gamma2f[i], config, "SHEAR_FITS_PREFIX", f, z);

    } // End of LOOP over convergence fields.

  // Memory deallocation:
  if (dist==gaussian) free_vector(aflm, 0, Nfields-1);
  weight.dealloc();
  Eflm.Set(0,0); 
  Bflm.Set(0,0);

  // Exit if this is the last output requested:
  if (ExitAt=="SHEAR_ALM_PREFIX"  ||
      ExitAt=="SHEAR_FITS_PREFIX") {
      cout << "\nTotal number of warnings: " << warning("count") << endl;
      cout<<endl;
      return 0;
  }

  // Write final map to file as a table if requested:
  GeneralOutput(mapf, config, "MAPWER_OUT", N1, N2);
  // Map output to fits and/or tga files:
  GeneralOutput(mapf, config, "MAPWERFITS_PREFIX", N1, N2, 1);  
  // Exit if this is the last output requested:
  if (ExitAt=="MAPWER_OUT" ||
      ExitAt=="MAPWERFITS_PREFIX") {
    cout << "\nTotal number of warnings: " << warning("count") << endl;
    cout<<endl;
    return 0;
  }

  // Output shear maps to TEXT tables:
  GeneralOutput(gamma1f, gamma2f, config, "SHEAR_MAP_OUT", N1, N2);
  // Exit if this is the last output requested:
  if (ExitAt=="SHEAR_MAP_OUT") {
      cout << "\nTotal number of warnings: " << warning("count") << endl;
      cout<<endl;
      return 0;
  }

  /**********************************/
  /*** Part 6: Generate catalog   ***/
  /**********************************/

  CAT_PRECISION **catalog;
  char **catSet;
  double esig, ellip1, ellip2, randz;
  int gali, cellNgal, ncols;
  long *ThreadNgals, Ngalaxies, kl, Ncells, PartialNgal;  
  pointing ang;
  int ziter, fiter;
  std::string CatalogHeader;
  int theta_pos, phi_pos, z_pos, galtype_pos, kappa_pos, gamma1_pos, gamma2_pos, 
    ellip1_pos, ellip2_pos, pixel_pos, maskbit_pos;

  esig = config.readd("ELLIP_SIGMA");
  
  Announce("Counting galaxies... ");
  // Partial count of galaxies for each thread (assuming the threads get the data in order):
  ThreadNgals = vector<long>(0, MaxThreads);       
  for(l=0; l<=MaxThreads; l++) ThreadNgals[l] = 0;
  // Loop over 3D cells:
  Ncells  = ((long)npixels)*((long)N2); // Number of 3D cells.
#pragma omp parallel for schedule(static) private(l, j, ziter, fiter, i)
  for (kl=0; kl<Ncells; kl++) {
    l     = omp_get_thread_num();
    j     = kl/N2;     // angular pixel.
    ziter = kl%N2 + 1; // z slice.
    for (fiter=1; fiter<=N1; fiter++) {
      fz2n(fiter, ziter, &i, N1, N2);
      if (ftype[i]==fgalaxies) ThreadNgals[l+1] += mapf[i][j];
    }
  }
  for (l=2; l<=MaxThreads; l++) ThreadNgals[l] += ThreadNgals[l-1];
  Ngalaxies = ThreadNgals[MaxThreads];
  Announce();     
  cout << "# of galaxies: "<<Ngalaxies<<endl;
  if (Ngalaxies>INT_MAX) warning("corrlnfields: catalogue generation not tested for this amount of galaxies");
  
  // Using transposed catalog (catalog[col][row]), better for FITS outputting:
  CatalogHeader = config.reads("CATALOG_COLS");
  ncols         = CountWords(CatalogHeader);
  catalog       = matrix<CAT_PRECISION>(0,ncols-1, 0,Ngalaxies-1); 
  catSet        = matrix<char>         (0,ncols-1, 0,Ngalaxies-1);
  for (kl=0; kl<Ngalaxies; kl++) for (j=0; j<ncols; j++) catSet[j][kl]=0;
  
  // Find position of entries according to catalog header:
  theta_pos   = GetSubstrPos("theta"  , CatalogHeader); 
  phi_pos     = GetSubstrPos("phi"    , CatalogHeader);  
  z_pos       = GetSubstrPos("z"      , CatalogHeader);  
  galtype_pos = GetSubstrPos("galtype", CatalogHeader);  
  kappa_pos   = GetSubstrPos("kappa"  , CatalogHeader);  
  gamma1_pos  = GetSubstrPos("gamma1" , CatalogHeader);  
  gamma2_pos  = GetSubstrPos("gamma2" , CatalogHeader);  
  ellip1_pos  = GetSubstrPos("ellip1" , CatalogHeader);  
  ellip2_pos  = GetSubstrPos("ellip2" , CatalogHeader);  
  pixel_pos   = GetSubstrPos("pixel"  , CatalogHeader); 
  maskbit_pos = GetSubstrPos("maskbit", CatalogHeader);
  // Reaname 'theta' and 'phi' to 'dec' and 'ra' if change of coords. was requested:
  if (config.readi("ANGULAR_COORD")==2) {
    StrReplace(CatalogHeader, "theta", "dec");
    StrReplace(CatalogHeader, "phi", "ra");
  }

  // LOOP over 3D cells (pixels and redshifts):
  Announce("Generating catalog... ");
  PartialNgal=0;                       // Counter of galaxies inside thread.
#pragma omp parallel for schedule(static) private(l, j, ziter, gali, fiter, i, m, ang, ellip1, ellip2, randz, cellNgal) firstprivate(PartialNgal)
  // Since this FOR has the same parameters as the one above for counting, thread assignment should be the same. 
  for (kl=0; kl<Ncells; kl++) {
    l        = omp_get_thread_num();   // Processor number.
    j        = kl/N2;                   // pixel.
    ziter    = kl%N2 + 1;               // z slice.
    gali     = 0;                      // Galaxy number inside cell.
    cellNgal = 0;                      // Total galaxy number inside cell.

    // Count total number of galaxies of all types inside cell:
    for (fiter=1; fiter<=N1; fiter++) {
      fz2n(fiter, ziter, &i, N1, N2);
      if (ftype[i]==fgalaxies) for(m=0; m<(int)mapf[i][j]; m++) cellNgal++;
    }
    
    // LOOP over field IDs:
    for (fiter=1; fiter<=N1; fiter++) {
      fz2n(fiter, ziter, &i, N1, N2);
      
      // Add entry of type GALAXY:      
      if (ftype[i]==fgalaxies) for(m=0; m<(int)mapf[i][j]; m++) {
	  if (theta_pos!=-1 || phi_pos!=-1) ang   = RandAngInPix(rnd[l+1], mapf[i], j);
	  if (z_pos!=-1)                    randz = selection.RandRedshift(rnd[l+1],i,j);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, theta_pos  , ang.theta           , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, phi_pos    , ang.phi             , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, z_pos      , randz               , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, galtype_pos, fiter               , catSet);	    
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, pixel_pos  , j                   , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+gali, maskbit_pos, selection.MaskBit(j), catSet);
	  gali++;
	}
      
      // Add entry of type SHEAR:
      else if (ftype[i]==fshear) for (m=0; m<cellNgal; m++) {
	  if (ellip1_pos!=-1 || ellip2_pos!=-1) GenEllip(rnd[l+1], esig, mapf[i][j], gamma1f[i][j], gamma2f[i][j], &ellip1, &ellip2);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+m, kappa_pos , mapf[i][j]   , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+m, gamma1_pos, gamma1f[i][j], catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+m, gamma2_pos, gamma2f[i][j], catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+m, ellip1_pos, ellip1       , catSet);
	  CatalogFill(catalog, ThreadNgals[l]+PartialNgal+m, ellip2_pos, ellip2       , catSet);
	}
    } // End of LOOP over field IDs.
    PartialNgal += (long)cellNgal;
  } // End of LOOP over cells.  
  free_vector(ThreadNgals, 0, MaxThreads);

  // Check if every entry was set once and only once:
  for (kl=0; kl<Ngalaxies; kl++) for (j=0; j<ncols; j++) {
      if (catSet[j][kl]<1)     error("corrlnfields: Catalog has missing information.");
      if (catSet[j][kl]>1)     {cout<<"j: "<<j<<endl; error("corrlnfields: Catalog entry being set more than once.");}
    }
  free_matrix(catSet, 0,ncols-1, 0,Ngalaxies-1);
  Announce();

  // Change angular coordinates if requested:
  l = config.readi("ANGULAR_COORD");
  if ((theta_pos!=-1 || phi_pos!=-1) && l!=0) {
    Announce("Changing angular coordinates... ");
    // Only change units:
    if (l==1) {
      if (theta_pos!=-1) // Theta:
#pragma omp parallel for
	for (kl=0; kl<Ngalaxies; kl++) catalog[theta_pos][kl] = rad2deg(catalog[theta_pos][kl]);
      if (phi_pos  !=-1) // Phi:
#pragma omp parallel for
	for (kl=0; kl<Ngalaxies; kl++) catalog[  phi_pos][kl] = rad2deg(catalog[  phi_pos][kl]);    
    }
    // Change to RADEC:
    else if (l==2) {
      if (theta_pos!=-1) // Theta:
#pragma omp parallel for
	for (kl=0; kl<Ngalaxies; kl++) catalog[theta_pos][kl] = theta2dec(catalog[theta_pos][kl]);
      if (phi_pos  !=-1) // Phi:
#pragma omp parallel for
	for (kl=0; kl<Ngalaxies; kl++) catalog[  phi_pos][kl] =    phi2ra(catalog[  phi_pos][kl]);    
    }
    else if (l!=0) warning("corrlnfields: unknown ANGULAR_COORD option, will keep Theta & Phi in radians.");
    Announce();
  }

  // Write catalog to file if requested:
  if (config.reads("CATALOG_OUT")!="0") {
    k        = config.readi("CATALOG_FORMAT");
    filename = config.reads("CATALOG_OUT");
    switch (k) {
    case 0: // TEXT file:
      outfile.open(filename.c_str());
      if (!outfile.is_open()) warning("corrlnfields: cannot open file "+filename);
      else {
	outfile << "# "<< CatalogHeader <<endl;
	//PrintTable(catalog, Ngalaxies, ncols, &outfile); 
	PrintVecs(catalog, Ngalaxies, ncols, &outfile); 
	outfile.close();
      }
      break;
    case 1: // FITS file:
      WriteCatalog2Fits(filename, catalog, Ngalaxies, config);
      break;
    default: // Unknown:
      warning("corrlnfields: unknown CATALOG_FORMAT, will use FITS.");
      WriteCatalog2Fits(filename, catalog, Ngalaxies, config);
      break;
    }
    cout << ">> Catalog written to " << filename << endl;
  }  
  free_matrix(catalog, 0,ncols-1, 0,Ngalaxies-1);
  

  // End of the program
  free_vector(ftype,   0, Nfields-1 );
  free_matrix(zrange,  0, Nfields-1, 0,1);
  free_vector(mapf,    0, Nfields-1 );
  free_vector(gamma1f, 0, Nfields-1 );
  free_vector(gamma2f, 0, Nfields-1 );
  for (i=0; i<=MaxThreads; i++) gsl_rng_free(rnd[i]);
  free_vector(rnd, 0,MaxThreads+1);
  cout << "\nTotal number of warnings: " << warning("count") << endl;
  cout<<endl;
  return 0;
}
