#include"psi4-dec.h"
#include<psifiles.h>
#include <libplugin/plugin.h>
#include<boost/shared_ptr.hpp>
#include<lib3index/dftensor.h>
#include<liboptions/liboptions.h>
#include<libtrans/integraltransform.h>
#include<libtrans/mospace.h>
#include<libmints/matrix.h>
#include<libmints/wavefunction.h>
#include<libmints/vector.h>
#include<libchkpt/chkpt.h>
#include<libiwl/iwl.h>
#include <libpsio/psio.hpp>
#include<libciomr/libciomr.h>

#include<sys/times.h>

#include"density_fitting.h"
#include"globals.h"
#include"gpuhelper.h"
#include"blas.h"
#include"ccsd.h"
#include"sort.h"

#ifdef _OPENMP
    #include<omp.h>
#endif

using namespace psi;
using namespace boost;



// position in a symmetric packed matrix
long int Position(long int i,long int j){
  if (i<j){
    return ((j*(j+1))>>1)+i;
  }
  return ((i*(i+1))>>1)+j;
}

namespace psi{

  /*!
   ** PSIO_GET_ADDRESS(): Given a starting page/offset and a shift length
   ** (in bytes), return the page/offset of the next position in the file.
   ** \ingroup PSIO
   */

  psio_address psio_get_address(psio_address start, long int shift) {
    psio_address address;
    long int bytes_left;

    bytes_left = PSIO_PAGELEN - start.offset; /* Bytes remaining on fpage */

    if (shift >= bytes_left) { /* Shift to later page */
      address.page = start.page + (shift - bytes_left)/PSIO_PAGELEN+ 1;
      address.offset = shift - bytes_left -(address.page - start.page- 1)
          *PSIO_PAGELEN;
    } else { /* Block starts on current page */
      address.page = start.page;
      address.offset = start.offset + shift;
    }

    return address;
  }

CoupledCluster::CoupledCluster()
{}
CoupledCluster::~CoupledCluster()
{}

void CoupledCluster::WriteBanner(){
  fflush(outfile);
  fprintf(outfile,"\n\n");
  fprintf(outfile, "        *******************************************************\n");
  fprintf(outfile, "        *                                                     *\n");
  fprintf(outfile, "        *                        CCSD                         *\n");
  fprintf(outfile, "        *           Coupled-Cluster Singles Doubles           *\n");
  fprintf(outfile, "        *          (for heterogeneous architectures)          *\n");
  fprintf(outfile, "        *                                                     *\n");
  fprintf(outfile, "        *                   Eugene DePrince                   *\n");
  fprintf(outfile, "        *                                                     *\n");
  fprintf(outfile, "        *******************************************************\n");
  fprintf(outfile,"\n\n");
  fflush(outfile);


  /*boost::shared_ptr<psi::Wavefunction> ref = Process::environment.reference_wavefunction();
  int nocc = ref->doccpi()[0];
  int nvir = ref->nmopi()[0]-ref->doccpi()[0];
  int aocc = nocc-ref->frzcpi()[0];
  int avir = nvir-ref->frzvpi()[0];

  boost::shared_ptr<BasisSetParser> parser(new Gaussian94BasisSetParser());
  boost::shared_ptr<BasisSet> auxiliary = BasisSet::construct(parser, ref->molecule(), "RI_BASIS_CC");

  boost::shared_ptr<DFTensor> DF (new DFTensor(ref->basisset(),auxiliary,ref->Ca(),nocc,nvir,aocc,avir,Process::environment.options));
  SharedMatrix Qoo = DF->Qoo();
  SharedMatrix Qov = DF->Qov();
  SharedMatrix Qvv = DF->Qvv();*/
}

/*================================================================
  
  Initialize:
  set basic parameters (ndocc...). integral transformation.
  integral sort.
  
================================================================*/
void CoupledCluster::Initialize(Options &options){
 
  // grab the reference wave function and its parameters
  boost::shared_ptr<psi::Wavefunction> ref = Process::environment.reference_wavefunction();

  if (ref.get() !=NULL){
     escf    = Process::environment.globals["SCF TOTAL ENERGY"];
     nirreps = ref->nirrep();
     sorbs   = ref->nsopi();
     orbs    = ref->nmopi();
     docc    = ref->doccpi();
     fzc     = ref->frzcpi();
     fzv     = ref->frzvpi();
  }
  if (nirreps>1){
     throw PsiException("plugin_ccsd requires symmetry c1",__FILE__,__LINE__);
  }
  nso = nmo = ndocc = nvirt = nfzc = nfzv = 0;
  long int full=0;
  for (long int h=0; h<nirreps; h++){
      nfzc   += fzc[h];
      nfzv   += fzv[h];
      nso    += sorbs[h];
      full   += orbs[h];
      nmo    += orbs[h]-fzc[h]-fzv[h];
      ndocc  += docc[h];//-fzc[h];
  }
  ndoccact = ndocc - nfzc;
  nvirt  = nmo - ndoccact;

  // for triples, we use nvirt_no in case we've truncated the virtual space:
  nvirt_no = nvirt;
  scale_t = 1.0;

  // get paramters from input 
  conv    = options.get_double("CONVERGENCE");
  maxiter = options.get_int("MAXITER");
  maxdiis = options.get_int("MAX_DIIS_VECS");

  // memory is from process::environment, but can override that
  memory = Process::environment.get_memory();
  if (options["MEMORY"].has_changed()){
     memory  = options.get_int("MEMORY");
     memory *= (long int)1024*1024;
  }
  // minus some extra in case i've miscounted...
  //memory -= (long int)200*1024*1024;

  // initialize gpu helper class
  helper_ = boost::shared_ptr<GPUHelper>(new GPUHelper);

  // get device parameters, allocate gpu memory and pinned cpu memory
  helper_->ndoccact = ndoccact;
  helper_->nvirt    = nvirt;
  helper_->nmo      = nmo;

  helper_->CudaInit(options);

  // reduce available memory by the amount required by the helper class
  memory -= helper_->max_mapped_memory;

  // quit if max_mapped_memory exceeds available memory
  if ((double)memory<0){
     throw PsiException("max_mapped_memory must be less than available memory",__FILE__,__LINE__);
  }

  // quit if number of virtuals is less than number of doubly occupied
  if (nvirt<ndoccact){
     throw PsiException("ndocc must be larger than nvirt",__FILE__,__LINE__);
  }

  // so->mo tei transformation
  struct tms total_tmstime;
  const long clk_tck = sysconf(_SC_CLK_TCK);

  long int i;

  double time_start,user_start,sys_start,time_stop,user_stop,sys_stop;

  // sort integrals and write them to disk (or generate df integrals)
  if (!options.get_bool("DF_INTEGRALS")){
     times(&total_tmstime);
     time_start = time(NULL);
     user_start = ((double) total_tmstime.tms_utime)/clk_tck;
     sys_start  = ((double) total_tmstime.tms_stime)/clk_tck;
     OutOfCoreSort(nfzc,nfzv,nmotemp,ndoccact,nvirt);
     //RandomIntegralFiles();

     times(&total_tmstime);
     time_stop = time(NULL);
     user_stop = ((double) total_tmstime.tms_utime)/clk_tck;
     sys_stop  = ((double) total_tmstime.tms_stime)/clk_tck;
     fprintf(outfile,"  Time for integral sort:           %6.2lf s (user)\n",user_stop-user_start);
     fprintf(outfile,"                                    %6.2lf s (system)\n",sys_stop-sys_start);
     fprintf(outfile,"                                    %6d s (total)\n",(int)time_stop-(int)time_start);
  }
  else{
     times(&total_tmstime);
     time_start = time(NULL);
     user_start = ((double) total_tmstime.tms_utime)/clk_tck;
     sys_start  = ((double) total_tmstime.tms_stime)/clk_tck;

     DensityFittedIntegrals();

     times(&total_tmstime);
     time_stop = time(NULL);
     user_stop = ((double) total_tmstime.tms_utime)/clk_tck;
     sys_stop  = ((double) total_tmstime.tms_stime)/clk_tck;
     fprintf(outfile,"  Time for density fitting:         %6.2lf s (user)\n",user_stop-user_start);
     fprintf(outfile,"                                    %6.2lf s (system)\n",sys_stop-sys_start);
     fprintf(outfile,"                                    %6d s (total)\n",(int)time_stop-(int)time_start);
  }

  // orbital energies
  eps_test = ref->epsilon_a();
  double*tempeps = eps_test->pointer();
  eps = (double*)malloc(nmo*sizeof(double));
  F_DCOPY(nmo,tempeps+nfzc,1,eps,1);
  eps_test.reset();

  // by default, t2 will be held in core
  t2_in_core = true;

}
void CoupledCluster::RandomIntegralFiles(){
  long int o = ndoccact;
  long int v = nvirt;
  long int dim = 4*o*o*v*v;
  double*temp=(double*)malloc(dim*sizeof(double));
  srand(time(NULL));
  for (long int i=0; i<dim; i++) temp[i] = .00002*((double)rand()/RAND_MAX-1.0);

  boost::shared_ptr<PSIO> psio(new PSIO());

  psio_address addr = PSIO_ZERO;
  long int count=0;
  /*psio->open(PSIF_IJKL,PSIO_OPEN_NEW);
  for (long int i=0; i<o*o*o*o; i++){
      count++;
      //temp[count++] = .01*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_IJKL,"E2ijkl",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_IJKL,"E2ijkl",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_IJKL,1);*/
  psio->open(PSIF_IJAK,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*o*o*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_IJAK,"E2ijak",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_IJAK,"E2ijak",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_IJAK,1);
  /*psio->open(PSIF_IJAK2,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*o*o*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_IJAK2,"E2ijak2",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_IJAK2,"E2ijak2",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_IJAK2,1);*/
  psio->open(PSIF_KLCD,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*o*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_KLCD,"E2klcd",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_KLCD,"E2klcd",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_KLCD,1);
  /*psio->open(PSIF_AKJC2,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*o*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_AKJC2,"E2akjc2",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_AKJC2,"E2akjc2",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_AKJC2,1);*/
  psio->open(PSIF_ABCI,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*v*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCI,"E2abci",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCI,"E2abci",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCI,1);
return;



  psio->open(PSIF_ABCI2,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*v*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCI2,"E2abci2",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCI2,"E2abci2",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCI2,1);
  psio->open(PSIF_ABCI3,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*v*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCI3,"E2abci3",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCI3,"E2abci3",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCI3,1);
  psio->open(PSIF_ABCI4,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*v*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCI4,"E2abci4",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCI4,"E2abci4",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCI4,1);
  psio->open(PSIF_ABCI5,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<o*v*v*v; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCI5,"E2abci5",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCI5,"E2abci5",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCI5,1);
  psio->open(PSIF_ABCD1,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<v*(v+1)/2*v*(v+1)/2; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCD1,"E2abcd1",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCD1,"E2abcd1",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCD1,1);
  psio->open(PSIF_ABCD2,PSIO_OPEN_NEW);
  count=0;
  addr = PSIO_ZERO;
  for (long int i=0; i<v*(v+1)/2*v*(v+1)/2; i++){
      count++;
      //temp[count++] = .00002*((double)rand()/RAND_MAX-1.0);
      if (count==dim){
         psio->write(PSIF_ABCD2,"E2abcd2",(char*)&temp[0],count*sizeof(double),addr,&addr);
         count=0;
      }
  }
  if (count>0){
     psio->write(PSIF_ABCD2,"E2abcd2",(char*)&temp[0],count*sizeof(double),addr,&addr);
     count=0;
  }
  psio->close(PSIF_ABCD2,1);

  free(temp);
}

/*===================================================================

  solve ccsd equations

===================================================================*/
PsiReturnType CoupledCluster::CCSDIterations(){

  // timer stuff:
  struct tms total_tmstime;
  const long clk_tck = sysconf(_SC_CLK_TCK);
  time_t iter_start,iter_stop,time_start,time_stop;
  double user_start,user_stop,sys_start,sys_stop;

  int iter,sg,sg2,diis_iter,replace_diis_iter;
  long int o = ndoccact;
  long int v = nvirt;
  long int arraysize = o*o*v*v;
  long int ov2 = o*v*v;
  long int oo1o2 = o*(o+1)/2;
  long int vv1o2 = v*(v+1)/2;
  double nrm,Eold,s1,start,end,siter;

  iter=0;
  diis_iter=0;
  replace_diis_iter=1;
  nrm=1.0;
  Eold=1.0e9;
  eccsd=0.0;

  fprintf(outfile,"\n");
  fprintf(outfile,
    "  Begin singles and doubles coupled cluster iterations ... on GPUs!\n\n");
  fprintf(outfile,
    "   Iter  DIIS          Energy       d(Energy)          |d(T)|     time\n");
  fflush(outfile);

  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;

  // zero residual
  psio->open(PSIF_R2,PSIO_OPEN_NEW);
  memset((void*)tempt,'\0',o*o*v*v*sizeof(double));
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  // cc diagrams split up as tasks
  DefineTasks();

  // start timing the iterations
  times(&total_tmstime);
  time_start = time(NULL);
  user_start = ((double) total_tmstime.tms_utime)/clk_tck;
  sys_start  = ((double) total_tmstime.tms_stime)/clk_tck;
  while(iter<maxiter && nrm>conv){
      iter_start = time(NULL);

      // evaluate cc diagrams
      if (iter>0){
         memset((void*)w1,'\0',o*v*sizeof(double));
         for (int i=0; i<ncctasks; i++) {
             double start = 0.0;
             #ifdef _OPENMP
                 start = omp_get_wtime();
             #endif
             (*this.*CCTasklist[i].func)(CCParams[i]);
             double end = 0.0;
             #ifdef _OPENMP
                 end = omp_get_wtime();
             #endif
         }
      }

      // update the amplitudes and check the energy
      Eold = eccsd;
      UpdateT1(iter);
      eccsd = UpdateT2(iter);

      // add vector to list for diis
      DIISOldVector(iter,diis_iter,replace_diis_iter);

      // diis error vector and convergence check
      nrm = DIISErrorVector(diis_iter,replace_diis_iter,iter);

      // diis extrapolation
      if (diis_iter>2){
         if (diis_iter<maxdiis) DIIS(diisvec,diis_iter,arraysize+o*v);
         else                   DIIS(diisvec,maxdiis,arraysize+o*v);
         DIISNewAmplitudes(diis_iter);
      }

      if (diis_iter<=maxdiis) diis_iter++;
      else if (replace_diis_iter<maxdiis) replace_diis_iter++;
      else replace_diis_iter = 1;

      iter_stop = time(NULL);
      fprintf(outfile,"  %5i   %i %i %15.10f %15.10f %15.10f %8d\n",
            iter,diis_iter-1,replace_diis_iter,eccsd,eccsd-Eold,nrm,(int)iter_stop-(int)iter_start);
      fflush(outfile);
      iter++;
      if (iter==1) emp2 = eccsd;
  }
  times(&total_tmstime);
  time_stop = time(NULL);
  user_stop = ((double) total_tmstime.tms_utime)/clk_tck;
  sys_stop  = ((double) total_tmstime.tms_stime)/clk_tck;
  psio.reset();

  if (iter==maxiter){
     throw PsiException("  CCSD iterations did not converge.",__FILE__,__LINE__);
  }

  fprintf(outfile,"\n");
  fprintf(outfile,"  CCSD iterations converged!\n");
  fprintf(outfile,"\n");
  fprintf(outfile,"  MP2 Correlation Energy:  %20.12lf\n",emp2);
  fprintf(outfile,"  CCSD Correlation Energy: %20.12lf\n",eccsd);
  fprintf(outfile,"\n");
  fprintf(outfile,"  Total time for CCSD iterations: %10.2lf s (user)\n",user_stop-user_start);
  fprintf(outfile,"                                  %10.2lf s (system)\n",sys_stop-sys_start);
  fprintf(outfile,"                                  %10d s (total)\n",(int)time_stop-(int)time_start);
  fprintf(outfile,"\n");
  fprintf(outfile,"  Time per iteration:             %10.2lf s (user)\n",(user_stop-user_start)/(iter-1));
  fprintf(outfile,"                                  %10.2lf s (system)\n",(sys_stop-sys_start)/(iter-1));
  fprintf(outfile,"                                  %10.2lf s (total)\n",((double)time_stop-(double)time_start)/(iter-1));
  fflush(outfile);

  return Success;
}

/*===================================================================

  determine tiling for vabcd and vabci diagrams for the cpu
  this determines the size of blocks of integrals that 
  can be read into cpu memory.

===================================================================*/
void CoupledCluster::DefineTilingCPU(){
  long int i,v = nvirt;
  long int o = ndoccact;
  long int ov2 = o*v*v;
  long int ov = o*v;
  long int o2 = o*o;

  // number of doubles in total memory
  long int ndoubles = memory/8L;
  // minus storage for other necessary buffers 
  ndoubles -= 3L*o*o*v*v+5L*o*v+v*v+(o+v);

  fprintf(outfile,"\n");
  fprintf(outfile,"  Define tiling:\n");
  fprintf(outfile,"\n");

  if (ndoubles<0){
     throw PsiException("out of memory: no amount of tiling can fix this!",__FILE__,__LINE__);
  }

  ntiles = -999L;
  tilesize = v*(v+1L)/2L;
  ntiles = 1L;

  // check whether blocking of the vabcd diagram is necessary
  if (ndoubles>v*(v+1L)/2L*v*(v+1L)/2L){
     tilesize = v*(v+1L)/2L;
     ntiles = 1L;
  }
  else{
     for (i=2L; i<=v*(v+1L)/2L; i++){
         if (ndoubles>tilesize*v*(v+1L)/2L/i+1L){
            tilesize = v*(v+1L)/2L/i;
            if (i*tilesize < v*(v+1L)/2L) tilesize++;
            ntiles = i;
            break;
         }
     }
     if (ntiles==-999L){
        throw PsiException("out of memory: (ab,cd)",__FILE__,__LINE__);
     }
  }
  lasttile = v*(v+1L)/2L - (ntiles-1L)*tilesize;

  fprintf(outfile,"        v(ab,cd) diagrams will be evaluated in %3li blocks.\n",ntiles); 
  fflush(outfile);

  // ov^3 type 1:
  if (v>ndoubles){
     throw PsiException("out of memory: (ab,ci)",__FILE__,__LINE__);
  }
  nov2tiles=1L;
  ov2tilesize=ov2/1L;
  if (nov2tiles*ov2tilesize<ov2) ov2tilesize++;
  while(v*ov2tilesize>ndoubles){
     nov2tiles++;
     ov2tilesize = ov2/nov2tiles;
     if (nov2tiles*ov2tilesize<ov2) ov2tilesize++;
  }
  lastov2tile = ov2 - (nov2tiles-1L)*ov2tilesize;

  fprintf(outfile,"        v(ab,ci) diagrams will be evaluated in %3li blocks over ov2.\n",nov2tiles); 
  fflush(outfile);

  // ov^3 type 2:
  if (v*v>ndoubles){
     throw PsiException("out of memory: (ab,ci)",__FILE__,__LINE__);
  }
  novtiles=1L;
  ovtilesize=ov/1L;
  if (novtiles*ovtilesize<ov) ovtilesize++;
  while(v*v*ovtilesize>ndoubles){
     novtiles++;
     ovtilesize = ov/novtiles;
     if (novtiles*ovtilesize<ov) ovtilesize++;
  }
  lastovtile = ov - (novtiles-1L)*ovtilesize;
  fprintf(outfile,"        v(ab,ci) diagrams will be evaluated in %3li blocks over ov.\n",novtiles); 
  fflush(outfile);

}

/*===================================================================

  allocate cpu memory

===================================================================*/
void CoupledCluster::AllocateMemory(Options&options){

  long int i,o=ndoccact;
  long int v=nvirt;
  long int dim;

  // define tiling for v^4 and ov^3 diagrams according to how much memory is available
  DefineTilingCPU();

  dim = 0;
  if (tilesize*v*(v+1)/2 > dim) dim = tilesize*v*(v+1)/2;
  if (ovtilesize*v*v > dim)     dim = ovtilesize*v*v;
  if (ov2tilesize*v > dim)      dim = ov2tilesize*v;

  if (dim<o*o*v*v){
     throw PsiException("out of memory: general buffer cannot accomodate t2",__FILE__,__LINE__);
  }
  maxelem = dim;

  double total_memory = 1.*dim+2.*(o*o*v*v+o*v)+1.*o*o*v*v+2.*o*v+2.*v*v;
  total_memory *= 8./1024./1024.;

  fprintf(outfile,"\n");
  fprintf(outfile,"  Allocate cpu memory (%9.2lf mb).....",total_memory);
  integrals = (double*)malloc(dim*sizeof(double));
  tempt     = (double*)malloc((o*o*v*v+o*v)*sizeof(double));
  tempv     = (double*)malloc((o*o*v*v+o*v)*sizeof(double));
  tb        = (double*)malloc(o*o*v*v*sizeof(double));
  w1        = (double*)malloc(o*v*sizeof(double));
  t1        = (double*)malloc(o*v*sizeof(double));
  I1        = (double*)malloc(v*v*sizeof(double));
  I1p       = (double*)malloc(v*v*sizeof(double));
  fprintf(outfile,"done.\n");

  fprintf(outfile,"  Initialize cpu memory..................");
  memset((void*)integrals,'\0',dim*sizeof(double));
  memset((void*)tempv,'\0',(o*o*v*v+o*v)*sizeof(double));
  memset((void*)tempt,'\0',(o*o*v*v+o*v)*sizeof(double));
  memset((void*)tb,'\0',o*o*v*v*sizeof(double));
  memset((void*)w1,'\0',o*v*sizeof(double));
  memset((void*)t1,'\0',o*v*sizeof(double));
  memset((void*)I1,'\0',v*v*sizeof(double));
  memset((void*)I1p,'\0',v*v*sizeof(double));
  fprintf(outfile,"done.\n");

  // DIIS:
  diisvec    = (double*)malloc(sizeof(double)*(maxdiis+1));
  memset((void*)diisvec,'\0',(maxdiis+1)*sizeof(double));
}

void CoupledCluster::CPU_t1_vmeai(CCTaskParams params){
  long int o = ndoccact;
  long int v = nvirt;
  long int i,a,m,e,id,one=1;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_AKJC2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_AKJC2,"E2akjc2",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_AKJC2,1);

  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  F_DAXPY(o*o*v*v,-2.0,integrals,1,tempv,1);
  
  for (i=0; i<o; i++){
      F_DCOPY(v,t1+i,o,tempt+i*v,1);
  }
  F_DGEMV('n',o*v,o*v,-1.0,tempv,o*v,tempt,1,0.0,integrals,1);
  //helper_->GPUTiledDGEMM('t','t',one,o*v,o*v,-1.0,tempt,o*v,tempv,o*v,0.0,integrals,one);
  //F_DGEMM('t','t',one,o*v,o*v,-1.0,tempt,o*v,tempv,o*v,0.0,integrals,one);
  for (a=0; a<v; a++){
      F_DAXPY(o,1.0,integrals+a,v,w1+a*o,1);
  }

  psio.reset();
}

void CoupledCluster::CPU_t1_vmeni(CCTaskParams params){
  long int m,e,n,a,id;
  long int o=ndoccact;
  long int v=nvirt;
  for (a=0,id=0; a<v; a++){
  for (m=0; m<o; m++){
  for (n=0; n<o; n++){
  for (e=0; e<v; e++){
      tempt[id++] = 2.*tb[e*v*o*o+a*o*o+m*o+n]-tb[a*v*o*o+e*o*o+m*o+n];
  }}}}
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_IJAK,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK,"E2ijak",(char*)&tempv[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK,1);
  //helper_->GPUTiledDGEMM('t','n',o,v,o*o*v,-1.0,tempv,o*o*v,tempt,o*o*v,1.0,w1,o);
  helper_->GPUTiledDGEMM_NoThread('t','n',o,v,o*o*v,-1.0,tempv,o*o*v,tempt,o*o*v,1.0,w1,o,0);
  //F_DGEMM('t','n',o,v,o*o*v,-1.0,tempv,o*o*v,tempt,o*o*v,1.0,w1,o);
  psio.reset();
}

void CoupledCluster::CPU_t1_vmaef(CCTaskParams params){
  long int m,e,i,f,a,id;
  long int o=ndoccact;
  long int v=nvirt;
  for (f=0,id=0; f<v; f++){
  for (m=0; m<o; m++){
  for (e=0; e<v; e++){
  for (i=0; i<o; i++){
      tempt[id++] = 2.*tb[e*v*o*o+f*o*o+m*o+i]-tb[e*v*o*o+f*o*o+i*o+m];
  }}}}

  long int tilesize,lasttile,ntiles=1;
  long int ov2 = o*v*v;
  // tile v in chunks of o

  ntiles=1L;
  tilesize=v/1L;
  if (ntiles*tilesize<v) tilesize++;
  while(tilesize*ov2>maxelem){
     ntiles++;
     tilesize = v/ntiles;
     if (ntiles*tilesize<ov2) tilesize++;
  }
  lasttile = v - (ntiles-1L)*tilesize;

  /*tilesize=v;
  for (i=1; i<=v; i++){
      if (o>=(double)tilesize/i){
         tilesize = v/i;
         if (i*tilesize < v) tilesize++;
         ntiles = i;
         break;
      }
  }
  lasttile = v - (ntiles-1)*tilesize;*/

  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_ABCI3,PSIO_OPEN_OLD);
  psio_address addr;
  addr = PSIO_ZERO;

  for (i=0; i<ntiles-1; i++){
      psio->read(PSIF_ABCI3,"E2abci3",(char*)&integrals[0],tilesize*ov2*sizeof(double),addr,&addr);
      helper_->GPUTiledDGEMM_NoThread('n','n',o,tilesize,ov2,1.0,tempt,o,integrals,ov2,1.0,w1+i*tilesize*o,o,0);
  }
  i=ntiles-1;
  psio->read(PSIF_ABCI3,"E2abci3",(char*)&integrals[0],lasttile*ov2*sizeof(double),addr,&addr);
  helper_->GPUTiledDGEMM_NoThread('n','n',o,lasttile,ov2,1.0,tempt,o,integrals,ov2,1.0,w1+i*tilesize*o,o,0);
  psio->close(PSIF_ABCI3,1);
  psio.reset();

}

void CoupledCluster::CPU_I1ab(CCTaskParams params){
  long int o = ndoccact;
  long int v = nvirt;
  long int b,m,n,e,a,id=0;
  // build I1(a,b)
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  for (m=0; m<o; m++){
  for (e=0; e<v; e++){
  for (n=0; n<o; n++){
  for (b=0; b<v; b++){
      tempv[id] =(-2.*integrals[id]+integrals[m*o*v*v+b*o*v+n*v+e]);
      tempt[id++] = tb[e*v*o*o+b*o*o+m*o+n] + t1[e*o+m]*t1[b*o+n];
  }}}}
  helper_->GPUTiledDGEMM_NoThread('n','t',v,v,o*o*v,1.0,tempv,v,tempt,v,0.0,I1,v,0);

  // add the singles parts to I1(a,b). n^4
  double sum=0.;
  psio->open(PSIF_ABCI2,PSIO_OPEN_OLD);
  psio_address addr;
  addr = PSIO_ZERO;

  // test swapping indices on t1: this will make the out-of-core 
  // integral sort at the beginning MUCH easier
  long int i,j,l,k,c,d;
  for (i=0; i<o; i++){
      F_DCOPY(v,t1+i,o,tempt+i*v,1);
  }

  // try tiling dgemv bc ov^3 might be too large.
  long int v2tilesize,nv2tiles,lastv2tile;
  nv2tiles=1L;
  v2tilesize=v*v/1L;
  if (nv2tiles*v2tilesize<v*v) v2tilesize++;
  while(v2tilesize*o*v>maxelem){
     nv2tiles++;
     v2tilesize = v*v/nv2tiles;
     if (nv2tiles*v2tilesize<v*v) v2tilesize++;
  }
  lastv2tile = v*v - (nv2tiles-1L)*v2tilesize;

  for (i=0; i<nv2tiles-1; i++){
      psio->read(PSIF_ABCI2,"E2abci2",(char*)&integrals[0],v2tilesize*v*o*sizeof(double),addr,&addr);
      F_DGEMV('t',o*v,v2tilesize,-1.0,integrals,o*v,tempt,1,1.0,I1+i*v2tilesize,1);
  }
  i=nv2tiles-1;
  psio->read(PSIF_ABCI2,"E2abci2",(char*)&integrals[0],lastv2tile*v*o*sizeof(double),addr,&addr);
  F_DGEMV('t',o*v,lastv2tile,-1.0,integrals,o*v,tempt,1,1.0,I1+i*v2tilesize,1);


  psio->close(PSIF_ABCI2,1);

  id=0;
  for (l=0; l<o; l++){
  for (c=0; c<v; c++){
  for (k=0; k<o; k++){
  for (d=0; d<v; d++){
      tempt[id++] = tb[d*v*o*o+c*o*o+l*o+k];
  }}}}
  // use I1(a,b) for doubles residual:
  helper_->GPUTiledDGEMM_NoThread('t','n',v,o*o*v,v,1.0,I1,v,tempt,v,0.0,tempv,v,0);
  //F_DGEMM('t','n',v,o*o*v,v,1.0,I1,v,tempt,v,0.0,tempv,v);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++)
  for (b=0; b<v; b++){
  for (i=0; i<o; i++)
  for (j=0; j<o; j++)
      tempt[id++] += tempv[j*v*v*o+a*v*o+i*v+b]+tempv[i*v*v*o+b*v*o+j*v+a];
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  // use I1(a,b) for singles residual - 1st contribution to w1. (n^3)
  //helper_->GPUTiledDGEMM('n','n',o,v,v,1.0,t1,o,I1,v,1.0,w1,o);
  F_DGEMM('n','n',o,v,v,1.0,t1,o,I1,v,1.0,w1,o);

  psio.reset();
}


// CPU_I2p_abci required ov^3 storage.  by refactorizing, we reduce storage to o^3v, but increase cost by 2o^2v^3
void CoupledCluster::CPU_I2p_abci_refactored_term1(CCTaskParams params){
  long int o = ndoccact;
  long int v = nvirt;
  long int a,b,c,i,j,id=0;
  long int ov2 = o*v*v;
  long int o2v = o*o*v;

  // tilesize * v <= o^2v^2
  /*long int tilesize,lasttile,ntiles=1;
  tilesize=ov2;
  for (i=1; i<=ov2; i++){
      if (o*o*v*v>=(double)tilesize*v/i){
         tilesize = ov2/i;
         if (i*tilesize < ov2) tilesize++;
         ntiles = i;
         break;
      }
  }
  lasttile = ov2 - (ntiles-1)*tilesize;*/

  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_ABCI5,PSIO_OPEN_OLD);
  psio_address addr;
  addr = PSIO_ZERO;

  for (i=0; i<nov2tiles-1; i++){
      psio->read(PSIF_ABCI5,"E2abci5",(char*)&integrals[0],v*ov2tilesize*sizeof(double),addr,&addr);
      //helper_->GPUTiledDGEMM_NoThread('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempt+i*ov2tilesize*o,o,0);
      helper_->GPUTiledDGEMM('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempt+i*ov2tilesize*o,o);
  }
  i=nov2tiles-1;
  psio->read(PSIF_ABCI5,"E2abci5",(char*)&integrals[0],v*lastov2tile*sizeof(double),addr,&addr);
  //helper_->GPUTiledDGEMM_NoThread('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempt+i*ov2tilesize*o,o,0);
  helper_->GPUTiledDGEMM('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempt+i*ov2tilesize*o,o);
  psio->close(PSIF_ABCI5,1);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  for (a=0; a<v; a++){
  for (b=0; b<v; b++){
      F_DAXPY(o*o,1.0,tempt+b*v*o*o+a*o*o,1,tempv+a*v*o*o+b*o*o,1);
  }}
  for (a=0; a<v; a++){
  for (b=0; b<v; b++){
  for (i=0; i<o; i++){
      F_DAXPY(o,1.0,tempt+a*v*o*o+b*o*o+i,o,tempv+a*v*o*o+b*o*o+i*o,1);
  }}}
  psio->write_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();

}
void CoupledCluster::CPU_I2p_abci_refactored_term2(CCTaskParams params){
  long int o = ndoccact;
  long int v = nvirt;
  long int a,b,c,i,j,id=0;
  long int ov2 = o*v*v;
  long int o2v = o*o*v;

  boost::shared_ptr<PSIO> psio(new PSIO());

  // now build and use 2 new intermediates:
  psio->open(PSIF_AKJC2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_AKJC2,"E2akjc2",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_AKJC2,1);
  helper_->GPUTiledDGEMM_NoThread('n','n',o,o2v,v,-1.0,t1,o,tempv,v,0.0,tempt,o,0);
  helper_->GPUTiledDGEMM_NoThread('n','n',o2v,v,o,1.0,tempt,o2v,t1,o,0.0,tempv,o2v,0);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  F_DAXPY(o*o*v*v,1.0,tempv,1,tempt,1);
  for (a=0; a<v; a++){
  for (b=0; b<v; b++){
  for (i=0; i<o; i++){
      F_DAXPY(o,1.0,tempv+a*v*o*o+b*o*o+i*o,1,tempt+b*v*o*o+a*o*o+i,o);
  }}}
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  psio.reset();
}
void CoupledCluster::CPU_I2p_abci_refactored_term3(CCTaskParams params){
  long int o = ndoccact;
  long int v = nvirt;
  long int a,b,c,i,j,id=0;
  long int ov2 = o*v*v;
  long int o2v = o*o*v;

  boost::shared_ptr<PSIO> psio(new PSIO());

  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  helper_->GPUTiledDGEMM_NoThread('t','t',o2v,o,v,-1.0,tempv,v,t1,o,0.0,tempt,o2v,0);
  // TODO: this was one of the ones the gpu version was screwing up...
  // it seems that it only has  problems with cuda 3.2, not cuda 4.0
  helper_->GPUTiledDGEMM_NoThread('t','n',v,o2v,o,1.0,t1,o,tempt,o,0.0,tempv,v,0);
  //F_DGEMM('t','n',v,o2v,o,1.0,t1,o,tempt,o,0.0,tempv,v);
  // the other way...
  //F_DGEMM('t','n',o2v,v,o,1.0,tempt,o,t1,o,0.0,tempv,o2v);
  //helper_->GPUTiledDGEMM('t','n',o2v,v,o,1.0,tempt,o,t1,o,0.0,tempv,o2v);


  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++){
  for (b=0; b<v; b++){
  for (i=0; i<o; i++){
  for (j=0; j<o; j++){
      tempt[id++] += tempv[i*v*v*o+j*v*v+b*v+a] + tempv[j*v*v*o+i*v*v+a*v+b];
      //tempt[id++] += tempv[a*o*o*v+i*v*o+j*v+b] + tempv[b*o*o*v+j*v*o+i*v+a];
  }}}}
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();
}

void CoupledCluster::CPU_I1pij_I1ia_lessmem(CCTaskParams params){

  long int o = ndoccact;
  long int v = nvirt;
  long int m,j,e,f,i,a,b;//,one=1;
  long int ov2 = o*v*v;
  long int id=0;

  // build I1(i,a). n^4
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  F_DCOPY(o*o*v*v,integrals,1,tempv,1);
  for (i=0; i<o; i++){
  for (a=0; a<v; a++){
  for (m=0; m<o; m++){
  for (e=0; e<v; e++){
      tempv[i*v*v*o+a*v*o+m*v+e] -= 0.5*integrals[i*o*v*v+e*o*v+m*v+a];
  }}}}
  for (i=0; i<o; i++) F_DCOPY(v,t1+i,o,tempt+i*v,1);
  F_DGEMV('t',o*v,o*v,2.0,tempv,o*v,tempt,1,0.0,I1,1);
  //helper_->GPUTiledDGEMM('t','n',o*v,1,o*v,2.0,tempv,o*v,tempt,o*v,0.0,I1,o*v);
  //F_DGEMM('t','n',o*v,1,o*v,2.0,tempv,o*v,tempt,o*v,0.0,I1,o*v);

  // use I1(i,a) -> w1
  id=0;
  for (m=0; m<o; m++){
  for (e=0; e<v; e++){
  for (j=0; j<o; j++){
  for (f=0; f<v; f++){
      tempt[id++] = 2*tb[e*o*o*v+f*o*o+m*o+j]-tb[e*v*o*o+f*o*o+j*o+m];
  }}}}
  F_DGEMV('n',o*v,o*v,1.0,tempt,o*v,I1,1,0.0,tempv,1);
  //helper_->GPUTiledDGEMM('n','n',o*v,1,o*v,1.0,tempt,o*v,I1,o*v,0.0,tempv,o*v);
  //F_DGEMM('n','n',o*v,1,o*v,1.0,tempt,o*v,I1,o*v,0.0,tempv,o*v);
  for (i=0; i<o; i++){
      F_DAXPY(v,1.0,tempv+i*v,1,w1+i,o);
  }

  // build I1'(i,j)
  helper_->GPUTiledDGEMM_NoThread('t','n',o,o,ov2,1.0,tempt,ov2,integrals,ov2,0.0,I1p,o,0);
  //F_DGEMM('t','n',o,o,ov2,1.0,tempt,ov2,integrals,ov2,0.0,I1p,o);
  
  // only n^4
  psio->open(PSIF_IJAK,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK,"E2ijak",(char*)&tempt[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK,1);
  id=0;
  for (i=0; i<o; i++){
  for (j=0; j<o; j++){
  for (e=0; e<v; e++){
  for (m=0; m<o; m++){
      //tempv[id++] = 2.*E2ijak[i*o*o*v+m*o*v+j*v+e] - E2ijak[m*o*o*v+i*o*v+j*v+e];
      tempv[id++] = 2.*tempt[i*o*o*v+m*o*v+j*v+e] - tempt[m*o*o*v+i*o*v+j*v+e];
  }}}}
  F_DGEMV('t',o*v,o*o,1.0,tempv,o*v,t1,1,1.0,I1p,1);
  //helper_->GPUTiledDGEMM('t','n',o*o,1,o*v,1.0,tempv,o*v,t1,o*v,1.0,I1p,o*o);
  //F_DGEMM('t','n',o*o,1,o*v,1.0,tempv,o*v,t1,o*v,1.0,I1p,o*o);

  // use I1'(i,j) for singles residual. (n^3)
  //helper_->GPUTiledDGEMM('n','n',o,v,o,-1.0,I1p,o,t1,o,1.0,w1,o);
  F_DGEMM('n','n',o,v,o,-1.0,I1p,o,t1,o,1.0,w1,o);

  // build I1(i,j)
  //helper_->GPUTiledDGEMM('n','n',o,o,v,1.0,t1,o,I1,v,1.0,I1p,o);
  F_DGEMM('n','n',o,o,v,1.0,t1,o,I1,v,1.0,I1p,o);
  for (m=0,id=0; m<o; m++){
  for (e=0; e<v; e++){
  for (j=0; j<o; j++){
  for (f=0; f<v; f++){
      tempt[id++] = tb[e*o*o*v+f*o*o+m*o+j];
  }}}}
  //helper_->GPUTiledDGEMM_NoThread('n','t',o,ov2,o,-1.0,I1p,o,tempt,ov2,0.0,tempv,o,0);
  helper_->GPUTiledDGEMM('n','t',o,ov2,o,-1.0,I1p,o,tempt,ov2,0.0,tempv,o);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++)
  for (b=0; b<v; b++){
  for (i=0; i<o; i++)
  for (j=0; j<o; j++)
      tempt[id++] += tempv[a*o*o*v+j*v*o+b*o+i] + tempv[b*o*o*v+i*v*o+a*o+j];
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  psio.reset();
}

/*================================================================

   update amplitudes

================================================================*/
void CoupledCluster::UpdateT1(long int iter){

  long int v = nvirt;
  long int o = ndoccact;
  long int rs = nmo;
  long int i,j,a,b;
  long int id=0;
  double tnew,dia;
  if (iter<1){
     memset((void*)t1,'\0',o*v*sizeof(double));
     memset((void*)w1,'\0',o*v*sizeof(double));
  }
  else{
     for (a=o; a<rs; a++){
         for (i=0; i<o; i++){
             dia = -eps[i]+eps[a];
             tnew = - (w1[(a-o)*o+i])/dia;
             w1[(a-o)*o+i] = tnew;
         }
     }
  }
  // error vector for diis is in tempv:
  F_DCOPY(o*v,w1,1,tempv+o*o*v*v,1);
  F_DAXPY(o*v,-1.0,t1,1,tempv+o*o*v*v,1);
  F_DCOPY(o*v,w1,1,t1,1);
}
double CoupledCluster::UpdateT2(long int iter){

  long int v = nvirt;
  long int o = ndoccact;
  long int rs = nmo;
  long int i,j,a,b;
  double ta,tnew,dijab,da,dab,dabi;
  long int iajb,jaib,ijab=0;
  double energy = 0.0;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  // we still have the residual in memory in tempv
  //psio->open(PSIF_R2,PSIO_OPEN_OLD);
  //psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=o; a<rs; a++){
      da = eps[a];
      for (b=o; b<rs; b++){
          dab = da + eps[b];
          for (i=0; i<o; i++){
              dabi = dab - eps[i];
              for (j=0; j<o; j++){

                  iajb = i*v*v*o+(a-o)*v*o+j*v+(b-o);
                  jaib = iajb + (i-j)*v*(1-v*o);

                  dijab = dabi-eps[j];

                  tnew = - (integrals[iajb] + tempv[ijab])/dijab;
                  tempt[ijab] = tnew;
                  energy += (2.*integrals[iajb]-integrals[jaib])*(tnew+t1[(a-o)*o+i]*t1[(b-o)*o+j]);
                  ijab++;
              }
          }
      }
  }

  // error vectors for diis are in tempv:
  F_DCOPY(o*o*v*v,tempt,1,tempv,1);
  F_DAXPY(o*o*v*v,-1.0,tb,1,tempv,1);
  F_DCOPY(o*o*v*v,tempt,1,tb,1);

  psio.reset();

  return energy;
}

/*================================================================

   diis functions

================================================================*/
void CoupledCluster::DIIS(double*c,long int nvec,long int n){
  long int i,j,k;
  doublereal sum,dum;
  integer*ipiv,nvar;
  nvar = nvec+1;
  doublereal*A = (doublereal*)malloc(sizeof(doublereal)*nvar*nvar);
  doublereal*B = (doublereal*)malloc(sizeof(doublereal)*nvar);
  memset((void*)A,'\0',nvar*nvar*sizeof(double));
  memset((void*)B,'\0',nvar*sizeof(double));
  B[nvec] = -1.;
  ipiv = (integer*)malloc(nvar*sizeof(integer));

  char*evector=(char*)malloc(1000*sizeof(char));

  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_EVEC,PSIO_OPEN_OLD);

  for (i=0; i<nvec; i++){
      sprintf(evector,"evector%li",i+1);
      psio->read_entry(PSIF_EVEC,evector,(char*)&tempt[0],n*sizeof(double));
      for (j=i+1; j<nvec; j++){
          sprintf(evector,"evector%li",j+1);
          psio->read_entry(PSIF_EVEC,evector,(char*)&tempv[0],n*sizeof(double));
          sum = F_DDOT(n,tempt,1,tempv,1);
          A[j*nvar+i] = sum;
          A[i*nvar+j] = sum;
      }
      A[i*nvar+i] = F_DDOT(n,tempt,1,tempt,1);
  }
  j = nvec;
  for (i=0; i<nvar; i++){
      A[j*nvar+i] = -1.0;
      A[i*nvar+j] = -1.0;
  }
  A[nvar*nvar-1] = 0.;
  psio->close(PSIF_EVEC,1);
  free(evector);

  integer nrhs,lda,ldb,info;
  nrhs = 1;
  lda = ldb = nvar;
  info = 0;
  DGESV(nvar,nrhs,A,lda,ipiv,B,ldb,info);
  F_DCOPY(nvec,B,1,c,1);

  free(A);
  free(B);
  free(ipiv);
  psio.reset();
}
void CoupledCluster::DIISOldVector(long int iter,int diis_iter,int replace_diis_iter){
  long int j,o = ndoccact;
  long int arraysize,v = nvirt;
  arraysize=o*o*v*v;

  char*oldvector=(char*)malloc(1000*sizeof(char));

  if (diis_iter<=maxdiis && iter<=maxdiis){
     sprintf(oldvector,"oldvector%i",diis_iter);
  }
  else{
     sprintf(oldvector,"oldvector%i",replace_diis_iter);
  }

  boost::shared_ptr<PSIO> psio(new PSIO());
  if (diis_iter==0)
     psio->open(PSIF_OVEC,PSIO_OPEN_NEW);
  else
     psio->open(PSIF_OVEC,PSIO_OPEN_OLD);

  psio_address addr;
  addr = PSIO_ZERO;
  psio->write(PSIF_OVEC,oldvector,(char*)&tb[0],arraysize*sizeof(double),addr,&addr);
  psio->write(PSIF_OVEC,oldvector,(char*)&t1[0],o*v*sizeof(double),addr,&addr);
  psio->close(PSIF_OVEC,1);
  psio.reset();

  free(oldvector);
}
double CoupledCluster::DIISErrorVector(int diis_iter,int replace_diis_iter,int iter){
  double nrm;
  long int i,j,o = ndoccact;
  long int arraysize,v = nvirt;
  arraysize=o*o*v*v;

  char*evector   = (char*)malloc(1000*sizeof(char));
  if (diis_iter<=maxdiis && iter<=maxdiis){
     sprintf(evector,"evector%i",diis_iter);
  }
  else{
     sprintf(evector,"evector%i",replace_diis_iter);
  }

  boost::shared_ptr<PSIO> psio(new PSIO());
  if (diis_iter==0)
     psio->open(PSIF_EVEC,PSIO_OPEN_NEW);
  else
     psio->open(PSIF_EVEC,PSIO_OPEN_OLD);

  nrm = F_DNRM2(arraysize+o*v,tempv,1);
  psio->write_entry(PSIF_EVEC,evector,(char*)&tempv[0],(arraysize+o*v)*sizeof(double));

  psio->close(PSIF_EVEC,1);
  psio.reset();

  free(evector);

  // return convergence
  return nrm;
}
void CoupledCluster::DIISNewAmplitudes(int diis_iter){
  long int o = ndoccact;
  long int arraysize,v = nvirt;
  arraysize=o*o*v*v;

  char*oldvector;
  oldvector=(char*)malloc(1000*sizeof(char));

  boost::shared_ptr<PSIO> psio(new PSIO());
  psio->open(PSIF_OVEC,PSIO_OPEN_OLD);

  psio_address addr;
  memset((void*)tb,'\0',arraysize*sizeof(double));
  memset((void*)t1,'\0',o*v*sizeof(double));

  int max = diis_iter;
  if (max > maxdiis) max = maxdiis;

  for (long int j=1; j<=max; j++){
      addr = PSIO_ZERO;
      sprintf(oldvector,"oldvector%li",j);
      psio->read(PSIF_OVEC,oldvector,(char*)&tempt[0],arraysize*sizeof(double),addr,&addr);
      F_DAXPY(arraysize,diisvec[j-1],tempt,1,tb,1);
      psio->read(PSIF_OVEC,oldvector,(char*)&tempt[0],o*v*sizeof(double),addr,&addr);
      F_DAXPY(o*v,diisvec[j-1],tempt,1,t1,1);
  }
  psio->close(PSIF_OVEC,1);
  free(oldvector);
  psio.reset();
}

/**
 *  Build and use I2ijkl
 */
void CoupledCluster::I2ijkl(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());

  F_DCOPY(o*o*v*v,tb,1,tempt,1);
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempt[id++] += t1[a*o+i]*t1[b*o+j];
              }
          }
      }
  }
  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  for (j=0; j<o; j++){
      for (i=0; i<o; i++){
          for (b=0; b<v; b++){
              F_DCOPY(v,integrals+j*o*v*v+b*o*v+i*v,1,tempv+j*o*v*v+i*v*v+b*v,1);
          }
      }
  }
  psio->open(PSIF_IJKL,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJKL,"E2ijkl",(char*)&integrals[0],o*o*o*o*sizeof(double));

  psio->close(PSIF_IJKL,1);
  helper_->GPUTiledDGEMM('n','n',o*o,o*o,v*v,1.0,tempt,o*o,tempv,v*v,1.0,integrals,o*o);
  psio->open(PSIF_IJAK,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK,"E2ijak",(char*)&tempv[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK,1);
  helper_->GPUTiledDGEMM_NoThread('n','n',o,o*o*o,v,2.0,t1,o,tempv,v,1.0,integrals,o,0);
  helper_->GPUTiledDGEMM('n','n',o*o,v*v,o*o,0.5,integrals,o*o,tempt,o*o,0.0,tempv,o*o);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  F_DAXPY(o*o*v*v,1.0,tempv,1,tempt,1);
  for (a=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              F_DAXPY(o,1.0,tempv+b*v*o*o+a*o*o+i,o,tempt+a*v*o*o+b*o*o+i*o,1);
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();

}
/**
 *  Build and use I2'iajk
 */
void CoupledCluster::I2piajk(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;
  F_DCOPY(o*o*v*v,tb,1,tempt,1);
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempt[id++] += t1[a*o+i]*t1[b*o+j];
              }
          }
      }
  }
  psio->open(PSIF_IJAK2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK2,"E2ijak2",(char*)&tempv[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK2,1);
  //F_DCOPY(o*o*o*v,E2ijak2,1,tempv,1);

  addr = PSIO_ZERO;
  psio->open(PSIF_ABCI,PSIO_OPEN_OLD);
  for (j=0; j<novtiles-1; j++){
      psio->read(PSIF_ABCI,"E2abci",(char*)&integrals[0],ovtilesize*v*v*sizeof(double),addr,&addr);
      helper_->GPUTiledDGEMM('n','n',o*o,ovtilesize,v*v,1.0,tempt,o*o,integrals,v*v,1.0,tempv+j*o*o*ovtilesize,o*o);
  }
  j=novtiles-1;
  psio->read(PSIF_ABCI,"E2abci",(char*)&integrals[0],lastovtile*v*v*sizeof(double),addr,&addr);
  helper_->GPUTiledDGEMM('n','n',o*o,lastovtile,v*v,1.0,tempt,o*o,integrals,v*v,1.0,tempv+j*o*o*ovtilesize,o*o);
  psio->close(PSIF_ABCI,1);

  helper_->GPUTiledDGEMM_NoThread('n','n',o*o*v,v,o,-1.0,tempv,o*o*v,t1,o,0.0,tempt,o*o*v,0);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  F_DAXPY(o*o*v*v,1.0,tempt,1,tempv,1);
  for (a=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              F_DAXPY(o,1.0,tempt+b*v*o*o+a*o*o+i,o,tempv+a*v*o*o+b*o*o+i*o,1);
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();
}
/**
 *  Use Vabcd1
 */
void CoupledCluster::Vabcd1(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;
  F_DCOPY(o*o*v*v,tb,1,tempt,1);
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempt[id++] += t1[a*o+i]*t1[b*o+j];
              }
          }
      }
  }
  for (i=0; i<o; i++){
      for (j=i; j<o; j++){
          for (a=0; a<v; a++){
              for (b=a+1; b<v; b++){
                  tempv[Position(a,b)*o*(o+1)/2+Position(i,j)] =
                     tempt[a*o*o*v+b*o*o+i*o+j]+tempt[b*o*o*v+a*o*o+i*o+j];
              }
              tempv[Position(a,a)*o*(o+1)/2+Position(i,j)] =
                 tempt[a*o*o*v+a*o*o+i*o+j];
          }
      }
  }
  psio->open(PSIF_ABCD1,PSIO_OPEN_OLD);
  addr = PSIO_ZERO;
  for (j=0; j<ntiles-1; j++){
      psio->read(PSIF_ABCD1,"E2abcd1",(char*)&integrals[0],tilesize*v*(v+1)/2*sizeof(double),addr,&addr);
      helper_->GPUTiledDGEMM('n','n',o*(o+1)/2,tilesize,v*(v+1)/2,1.0,tempv,o*(o+1)/2,integrals,v*(v+1)/2,0.0,tempt+j*tilesize*o*(o+1)/2,o*(o+1)/2);
  }
  j=ntiles-1;
  psio->read(PSIF_ABCD1,"E2abcd1",(char*)&integrals[0],lasttile*v*(v+1)/2*sizeof(double),addr,&addr);
  helper_->GPUTiledDGEMM('n','n',o*(o+1)/2,lasttile,v*(v+1)/2,1.0,tempv,o*(o+1)/2,integrals,v*(v+1)/2,0.0,tempt+j*tilesize*o*(o+1)/2,o*(o+1)/2);
  psio->close(PSIF_ABCD1,1);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  for (a=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempv[a*o*o*v+b*o*o+i*o+j] += .5*tempt[Position(a,b)*o*(o+1)/2+Position(i,j)];
              }
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();

}

/**
 *  Use Vabcd2
 */
void CoupledCluster::Vabcd2(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  int sg,sg2;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;
  F_DCOPY(o*o*v*v,tb,1,tempt,1);
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempt[id++] += t1[a*o+i]*t1[b*o+j];
              }
          }
      }
  }
  for (i=0; i<o; i++){
      for (j=i; j<o; j++){
          for (a=0; a<v; a++){
              for (b=a; b<v; b++){
                  tempv[Position(a,b)*o*(o+1)/2+Position(i,j)] =
                    tempt[a*o*o*v+b*o*o+i*o+j]-tempt[b*o*o*v+a*o*o+i*o+j];
              }
          }
      }
  }
  psio->open(PSIF_ABCD2,PSIO_OPEN_OLD);
  addr = PSIO_ZERO;
  for (j=0; j<ntiles-1; j++){
      psio->read(PSIF_ABCD2,"E2abcd2",(char*)&integrals[0],tilesize*v*(v+1)/2*sizeof(double),addr,&addr);
      helper_->GPUTiledDGEMM('n','n',o*(o+1)/2,tilesize,v*(v+1)/2,1.0,tempv,o*(o+1)/2,integrals,v*(v+1)/2,0.0,tempt+j*tilesize*o*(o+1)/2,o*(o+1)/2);
  }
  j = ntiles-1;
  psio->read(PSIF_ABCD2,"E2abcd2",(char*)&integrals[0],lasttile*v*(v+1)/2*sizeof(double),addr,&addr);
  helper_->GPUTiledDGEMM('n','n',o*(o+1)/2,lasttile,v*(v+1)/2,1.0,tempv,o*(o+1)/2,integrals,v*(v+1)/2,0.0,tempt+j*tilesize*o*(o+1)/2,o*(o+1)/2);
  psio->close(PSIF_ABCD2,1);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  for (a=0; a<v; a++){
      for (b=0; b<v; b++){
          if (a>b) sg2 = -1;
          else     sg2 = 1;
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  if (i>j) sg = -1;
                  else     sg = 1;
                  tempv[a*o*o*v+b*o*o+i*o+j] += .5*sg2*sg*tempt[Position(a,b)*o*(o+1)/2+Position(i,j)];
              }
          }
      }
  }
  //psio->write_entry(PSIF_R2,"residual",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);
  psio.reset();
}
/**
 *  Build and use I2iabj
 */
void CoupledCluster::I2iabj(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;

  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  tempt[id++] = tb[b*v*o*o+a*o*o+j*o+i]+2.*t1[a*o+i]*t1[b*o+j];
              }
          }
      }
  }

  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  F_DCOPY(o*o*v*v,integrals,1,tempv,1);
  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,-0.5,tempt,o*v,integrals,o*v,1.0,tempv,o*v);


  // o^2v^3 contribution to intermediate
  psio->open(PSIF_IJAK,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK,"E2ijak",(char*)&integrals[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK,1);
  helper_->GPUTiledDGEMM_NoThread('n','n',o*o*v,v,o,-1.0,integrals,o*o*v,t1,o,0.0,tempt,o*o*v,0);
  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  tempv[id++] += tempt[a*o*o*v+i*o*v+j*v+b];
              }
          }
      }
  }
  // contribute to intermediate
  psio->open(PSIF_TEMP,PSIO_OPEN_NEW);
  psio->write_entry(PSIF_TEMP,"temporary",(char*)&tempv[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_TEMP,1);

  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);

  F_DCOPY(o*o*v*v,tempt,1,tempv,1);
  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  tempv[id++] -= 0.5*tempt[i*v*v*o+a*v*o+j*v+b];
              }
          }
      }
  }
  //memset((void*)tempt,'\0',o*o*v*v*sizeof(double));
  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  integrals[i*v*v*o+a*v*o+j*v+b] = tb[b*v*o*o+a*o*o+j*o+i];
              }
          }
      }
  }
  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,1.0,integrals,o*v,tempv,o*v,0.0,tempt,o*v);

  // o^2v^3 piece of intermediate
  addr = PSIO_ZERO;
  psio->open(PSIF_ABCI,PSIO_OPEN_OLD);

  for (j=0; j<nov2tiles-1; j++){
      psio->read(PSIF_ABCI,"E2abci",(char*)&integrals[0],ov2tilesize*v*sizeof(double),addr,&addr);
      //helper_->GPUTiledDGEMM_NoThread('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o,0);
      helper_->GPUTiledDGEMM('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o);
  }
  j=nov2tiles-1;
  psio->read(PSIF_ABCI,"E2abci",(char*)&integrals[0],lastov2tile*v*sizeof(double),addr,&addr);
  //helper_->GPUTiledDGEMM_NoThread('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o,0);
  helper_->GPUTiledDGEMM('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o);
  psio->close(PSIF_ABCI,1);
  for (i=0,id=0; i<o; i++){
      for (a=0; a<v; a++){
          for (b=0; b<v; b++){
              for (j=0; j<o; j++){
                  tempt[i*o*v*v+b*o*v+j*v+a] += tempv[id++];
              }
          }
      }
  }

  // contribute to intermediate
  psio->open(PSIF_TEMP,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_TEMP,"temporary",(char*)&tempv[0],o*o*v*v*sizeof(double));
  F_DAXPY(o*o*v*v,1.0,tempt,1,tempv,1);
  psio->close(PSIF_TEMP,0);

  // use I2iabj
  for (j=0,id=0; j<o; j++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (a=0; a<v; a++){
                  tempt[id++] = 2*tb[a*o*o*v+b*o*o+i*o+j]-tb[b*o*o*v+a*o*o+i*o+j];
              }
          }
      }
  }

  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,1.0,tempv,o*v,tempt,o*v,0.0,integrals,o*v);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  // if we KNOW this is the first diagram, we don't need to read in the old
  // residual.
  //psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  tempt[id++] = integrals[j*o*v*v+b*v*o+i*v+a] + integrals[i*o*v*v+a*v*o+j*v+b];
              }
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  psio.reset();

}
/**
 *  Build and use I2iajb
 */
void CoupledCluster::I2iajb(CCTaskParams params){
  long int id,i,j,a,b,o,v;
  o = ndoccact;
  v = nvirt;
  boost::shared_ptr<PSIO> psio(new PSIO());
  psio_address addr;

  psio->open(PSIF_KLCD,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_KLCD,"E2klcd",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_KLCD,1);
  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  integrals[id] = tb[b*o*o*v+a*o*o+j*o+i] + 2.*t1[a*o+i]*t1[b*o+j];
                  tempv[id++] = tempt[i*v*v*o+a*v*o+j*v+b];
              }
          }
      }
  }

  psio->open(PSIF_AKJC2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_AKJC2,"E2akjc2",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_AKJC2,1);
  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,-0.5,integrals,o*v,tempv,o*v,1.0,tempt,o*v);

  // o^2v^3 work
  addr = PSIO_ZERO;
  psio->open(PSIF_ABCI4,PSIO_OPEN_OLD);

  for (j=0; j<nov2tiles-1; j++){
      psio->read(PSIF_ABCI4,"E2abci4",(char*)&integrals[0],ov2tilesize*v*sizeof(double),addr,&addr);
      //helper_->GPUTiledDGEMM_NoThread('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o,0);
      helper_->GPUTiledDGEMM('n','n',o,ov2tilesize,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o);
  }
  j=nov2tiles-1;
  psio->read(PSIF_ABCI4,"E2abci4",(char*)&integrals[0],lastov2tile*v*sizeof(double),addr,&addr);
  //helper_->GPUTiledDGEMM_NoThread('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o,0);
  helper_->GPUTiledDGEMM('n','n',o,lastov2tile,v,1.0,t1,o,integrals,v,0.0,tempv+j*o*ov2tilesize,o);
  psio->close(PSIF_ABCI4,1);

  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  tempt[id++] += tempv[i*o*v*v+a*v*o+b*o+j];
              }
          }
      }
  }

  // stick o^3v^2 work on first tile
  psio->open(PSIF_IJAK2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_IJAK2,"E2ijak2",(char*)&integrals[0],o*o*o*v*sizeof(double));
  psio->close(PSIF_IJAK2,1);
  // TODO: this was a problem with cuda 3.2 vs 4.0
  helper_->GPUTiledDGEMM_NoThread('t','n',o*o*v,v,o,-1.0,integrals,o,t1,o,0.0,tempv,o*o*v,0);
  //F_DGEMM('t','n',o*o*v,v,o,-1.0,integrals,o,t1,o,0.0,tempv,o*o*v);
  for (i=0,id=0; i<o; i++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (a=0; a<v; a++){
                  //tempt[id++] += tempv[a*o*o*v+i*o*v+j*v+b];
                  tempt[id++] += tempv[a*o*o*v+i*o*v+b*o+j];
              }
          }
      }
  }

  // contribute to intermediate
  //psio->open(PSIF_TEMP,PSIO_OPEN_NEW);
  //psio->write_entry(PSIF_TEMP,"temporary",(char*)&tempt[0],o*o*v*v*sizeof(double));
  //psio->close(PSIF_TEMP,1);

  // use I2iajb
  for (j=0,id=0; j<o; j++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (a=0; a<v; a++){
                  integrals[id++] = tb[b*v*o*o+a*o*o+j*o+i];
              }
          }
      }
  }
  //psio->open(PSIF_TEMP,PSIO_OPEN_OLD);
  //psio->read_entry(PSIF_TEMP,"temporary",(char*)&tempt[0],o*o*v*v*sizeof(double));
  //psio->close(PSIF_TEMP,1);

  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,-1.0,tempt,o*v,integrals,o*v,0.0,tempv,o*v);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&integrals[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (i=0; i<o; i++){
              for (j=0; j<o; j++){
                  integrals[id++] += tempv[j*o*v*v+b*v*o+i*v+a] + tempv[i*o*v*v+a*v*o+j*v+b];
              }
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&integrals[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  // use I2iajb
  for (j=0,id=0; j<o; j++){
      for (a=0; a<v; a++){
          for (i=0; i<o; i++){
              for (b=0; b<v; b++){
                  tempv[id++] = tb[b*v*o*o+a*o*o+j*o+i];
              }
          }
      }
  }

  helper_->GPUTiledDGEMM('n','n',o*v,o*v,o*v,-1.0,tempt,o*v,tempv,o*v,0.0,integrals,o*v);

  // contribute to residual
  psio->open(PSIF_R2,PSIO_OPEN_OLD);
  psio->read_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  for (a=0,id=0; a<v; a++){
      for (b=0; b<v; b++){
          for (j=0; j<o; j++){
              for (i=0; i<o; i++){
                  tempt[id++] += integrals[j*o*v*v+b*v*o+i*v+a] + integrals[i*o*v*v+a*v*o+j*v+b];
              }
          }
      }
  }
  psio->write_entry(PSIF_R2,"residual",(char*)&tempt[0],o*o*v*v*sizeof(double));
  psio->close(PSIF_R2,1);

  psio.reset();
}

/**
 *  Tasks:
 */
void CoupledCluster::DefineTasks(){
  CCTasklist = new CCTask[1000];
  CCParams   = new CCTaskParams[1000];
  long int o = ndoccact;
  long int v = nvirt;

  // these will be used for the parallel version
  long int niabjranks,niajbranks;
  niabjranks=niajbranks=1;

  ncctasks=0;

  // used to be gpu functions
  //CCTasklist[ncctasks].func        = &psi::CoupledCluster::I2iabj;
  //CCTasklist[ncctasks++].flopcount = 2*(3*o*o*o*v*v*v+o*o*v*v*v+o*o*o*v*v);

  // I2iabj
  CCTasklist[ncctasks].func      = &psi::CoupledCluster::I2iabj;
  CCTasklist[ncctasks].flopcount = 2.*(3.*o*o*o*v*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  // I2iajb
  CCTasklist[ncctasks].func      = &psi::CoupledCluster::I2iajb;
  CCTasklist[ncctasks].flopcount = 2.*(3.*o*o*o*v*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  // I2ijkl ... n^6, so probably worth tiling at some point 
  CCTasklist[ncctasks].func        = &psi::CoupledCluster::I2ijkl;
  CCTasklist[ncctasks].flopcount = 2.*(2.*o*o*o*o*v*v+1.*o*o*o*o*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  // I2pijak:
  CCTasklist[ncctasks].func        = &psi::CoupledCluster::I2piajk;
  CCTasklist[ncctasks].flopcount = 2.*(1.*o*o*o*v*v*v+1.*o*o*o*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;
 
  // used to be cpu functions
  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_t1_vmeni;
  CCTasklist[ncctasks].flopcount = 2.*o*o*o*v*v;
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_t1_vmaef;
  CCTasklist[ncctasks].flopcount = 2.*o*o*v*v*v;
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_I2p_abci_refactored_term1;
  CCTasklist[ncctasks].flopcount = 2.*(o*o*v*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_I2p_abci_refactored_term2;
  CCTasklist[ncctasks].flopcount = 2.*(2.*o*o*o*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_I2p_abci_refactored_term3;
  CCTasklist[ncctasks].flopcount = 2.*(2.*o*o*o*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_I1ab;
  CCTasklist[ncctasks].flopcount = 2.*(2.*o*o*v*v*v+1.*o*v*v*v+1.*o*v*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_t1_vmeai;
  CCTasklist[ncctasks].flopcount = 2.*o*o*v*v;
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  CCTasklist[ncctasks].func        = &psi::CoupledCluster::CPU_I1pij_I1ia_lessmem;
  CCTasklist[ncctasks].flopcount = 2.*(2.*o*o*v*v+2.*o*o*o*v*v+1.*o*o*o*v+2.*o*o*v);
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  // tiles of Vabcd1:
  CCTasklist[ncctasks].func        = &psi::CoupledCluster::Vabcd1;
  CCTasklist[ncctasks].flopcount = 2.*o*(o+1)/2.*v*(v+1)/2*v*(v+1)/2.;
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

  // tiles of Vabcd2: 
  // this is the last diagram that contributes to doubles residual,
  // so we can keep it in memory rather than writing and rereading
  CCTasklist[ncctasks].func        = &psi::CoupledCluster::Vabcd2;
  CCTasklist[ncctasks].flopcount = 2.*o*(o+1)/2.*tilesize*v*(v+1)/2.;
  CCParams[ncctasks].mtile = -999;
  CCParams[ncctasks].ntile = -999;
  CCParams[ncctasks++].ktile = -999;

}


} // end of namespace psi
