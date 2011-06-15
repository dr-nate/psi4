#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include <utility>
#include <string>
#include <cstring>

#include <psifiles.h>
#include <physconst.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.hpp>
#include <libchkpt/chkpt.hpp>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>

#include <libmints/mints.h>
#include <libfunctional/superfunctional.h>
#include <libscf_solver/ks.h>
#include <libscf_solver/integralfunctors.h>
#include <libscf_solver/omegafunctors.h>

#include "omega.h"

#ifdef _OPENMP
#include <omp.h>
#endif

using namespace std;
using namespace psi;
using namespace psi::functional;
using namespace boost;

namespace psi{ namespace scf {

OmegaKS::OmegaKS(Options & options, boost::shared_ptr<PSIO> psio) :
    options_(options), psio_(psio)
{
    common_init();
}
OmegaKS::~OmegaKS()
{
}
void OmegaKS::common_init()
{
    memory_ = Process::environment.get_memory();
    nthread_ = 1;
    #ifdef _OPENMP
        nthread_ = omp_get_max_threads();
    #endif

    print_ = options_.get_int("PRINT");
    print_ = (print_ ? print_ : 1);

    debug_ = options_.get_int("DEBUG");

    // Read in energy convergence threshold
    int thresh = options_.get_int("E_CONVERGE");
    energy_threshold_ = pow(10.0, (double)-thresh);

    // Read in density convergence threshold
    thresh = options_.get_int("D_CONVERGE");
    density_threshold_ = pow(10.0, (double)-thresh);

    block_size_ = options_.get_int("DFT_BLOCK_SIZE");
    
    //Build the superfunctional
    functional_ = SuperFunctional::createSuperFunctional(options_.get_str("DFT_FUNCTIONAL"),block_size_,1);

    if (!functional_->isRangeCorrected())
        throw PSIEXCEPTION("No omega in this functional");
}
void OmegaKS::run_procedure()
{
    // What are we doing?
    print_header();

    // Form H
    form_H();

    // Form X according to the usual rules (always canonical herein)
    form_X();

    // Build the DF Integrals objects
    form_DF();

    // Build the KS Integrators
    form_KS();

    // Populate the necessary N, N+1, N-1, etc wavefunctions 
    // Provide hooks to DF and V objects, as well as X, H, C, D, etc
    populate();

    // Burn-in the wavefunctions
    fprintf(outfile, "  ==> Burn-In Procedure <==\n\n");
    for (std::map<std::string, boost::shared_ptr<OmegaWavefunction> >::iterator it = wfns_.begin(); it != wfns_.end(); it++) {

        std::string name = (*it).first;
        boost::shared_ptr<OmegaWavefunction> wfn = (*it).second;
      
        // Reset DIIS and whatever 
        wfn->reset();

        fprintf(outfile, "   => %s SCF <=\n\n", name.c_str());
        fprintf(outfile, "   %4s %20s %12s %12s %s\n", "Iter", "Energy", "Delta E", "Delta D", "Stat");
        fflush(outfile);
 
        bool converged = false;
        for (int iter = 0; iter < options_.get_int("MAXITER") || iter < 2; iter++) {

            // Take an SCF step (including any DIIS)
            std::string status = wfn->step();     

            double dE = wfn->deltaE();  
            double dD = wfn->deltaD();
            double E = wfn->E();

            fprintf(outfile, "   %4d %20.14f %12.5E %12.5E %s\n", iter + 1, E, dE, dD, status.c_str()); fflush(outfile);

            if (fabs(dE) < energy_threshold_ && fabs(dD) < density_threshold_) {
                converged = true;
                break;
            }
        }

        wfn->clear();
    
        if (!converged)
            throw PSIEXCEPTION("OmegaKS: Burn-In iterations did not converge");

        fprintf(outfile, "\n"); fflush(outfile);
    }  

    // Omega procedure 
    fprintf(outfile, "  ==> Omega Optimization Procedure <==\n\n"); 

    bool omega_converged = false;
    for (int omega_iter = 0; omega_iter < options_.get_int("OMEGA_MAXITER"); omega_iter++) {

        omega_step(omega_iter);

        for (std::map<std::string, boost::shared_ptr<OmegaWavefunction> >::iterator it = wfns_.begin(); it != wfns_.end(); it++) {

            std::string name = (*it).first;
            boost::shared_ptr<OmegaWavefunction> wfn = (*it).second;
          
            // Reset DIIS and whatever 
            wfn->reset();

            fprintf(outfile, "   => %s SCF <=\n\n", name.c_str());
            fprintf(outfile, "   %4s %20s %12s %12s %s\n", "Iter", "Energy", "Delta E", "Delta D", "Stat");
            fflush(outfile);
 
            bool converged = false;
            for (int iter = 0; iter < options_.get_int("MAXITER") || iter < 2; iter++) {

                // Take an SCF step (including any DIIS)
                std::string status = wfn->step();     

                double dE = wfn->deltaE();  
                double dD = wfn->deltaD();
                double E = wfn->E();

                fprintf(outfile, "   %4d %20.14f %12.5E %12.5E %s\n", iter + 1, E, dE, dD, status.c_str()); fflush(outfile);

                if (fabs(dE) < energy_threshold_ && fabs(dD) < density_threshold_) {
                    converged = true;
                    break;
                }
            }

            wfn->clear();
        
            if (!converged)
                throw PSIEXCEPTION("OmegaKS: Burn-In iterations did not converge");

            fprintf(outfile, "\n"); fflush(outfile);
        }  
         
        omega_converged = is_omega_converged();
        if (omega_converged) 
            break;
    }

    if (!omega_converged)
        throw PSIEXCEPTION("OmegaKS: Overall omega optimization did not converge");

    // Final  
    fprintf(outfile, "  ==> Final Convergence Procedure <==\n\n"); 

    for (std::map<std::string, boost::shared_ptr<OmegaWavefunction> >::iterator it = wfns_.begin(); it != wfns_.end(); it++) {

        std::string name = (*it).first;
        boost::shared_ptr<OmegaWavefunction> wfn = (*it).second;
      
        // Reset DIIS and whatever 
        wfn->reset();

        fprintf(outfile, "   => %s SCF <=\n\n", name.c_str());
        fprintf(outfile, "   %4s %20s %12s %12s %s\n", "Iter", "Energy", "Delta E", "Delta D", "Stat");
        fflush(outfile);
 
        bool converged = false;
        for (int iter = 0; iter < options_.get_int("MAXITER") || iter < 2; iter++) {

            // Take an SCF step (including any DIIS)
            std::string status = wfn->step();     

            double dE = wfn->deltaE();  
            double dD = wfn->deltaD();
            double E = wfn->E();

            fprintf(outfile, "   %4d %20.14f %12.5E %12.5E %s\n", iter + 1, E, dE, dD, status.c_str()); fflush(outfile);

            if (fabs(dE) < energy_threshold_ && fabs(dD) < density_threshold_) {
                converged = true;
                break;
            }
        }

        wfn->clear();
  
        fprintf(outfile, "\n   UKS %s Orbital Energies [a.u.]:\n\n", name.c_str()); 
        wfn->print_orbitals();

        fprintf(outfile, "  @UKS %s Final energy: %24.16f\n\n", name.c_str(), wfn->E());
        fflush(outfile);
 
        if (!converged)
            throw PSIEXCEPTION("OmegaKS: Final convergence iterations did not converge");

    } 

    // Printing and finalization
    finalize();
}
boost::shared_ptr<Matrix> OmegaKS::build_S(boost::shared_ptr<BasisSet> primary)
{
    int nso = primary->nbf();
    boost::shared_ptr<Matrix> S(new Matrix("S",nso,nso));

    boost::shared_ptr<IntegralFactory> factory(new IntegralFactory(primary,primary,primary,primary));
    boost::shared_ptr<OneBodyAOInt> Sint(factory->ao_overlap());

    Sint->compute(S);

    return S;
}
boost::shared_ptr<Matrix> OmegaKS::build_X(boost::shared_ptr<BasisSet> primary, double min_S)
{
    int nso = primary->nbf();
    boost::shared_ptr<Matrix> S(new Matrix("S",nso,nso));

    boost::shared_ptr<IntegralFactory> factory(new IntegralFactory(primary,primary,primary,primary));
    boost::shared_ptr<OneBodyAOInt> Sint(factory->ao_overlap());

    Sint->compute(S);

    boost::shared_ptr<Matrix> U(new Matrix("U",nso,nso));
    boost::shared_ptr<Vector> s(new Vector("s",nso));
   
    S->diagonalize(U,s);

    double* sp = s->pointer(); 
    int nmo = 0;
    for (int i = 0; i < nso; i++) {
        if (sp[i] > min_S)
            nmo++;
    }


    boost::shared_ptr<Matrix> X(new Matrix("X", nso,nmo));
    double** Xp = X->pointer();
    double** Up = U->pointer();

    int j = 0;
    for (int i = 0; i < nso; i++) {
        if (sp[i] > min_S) {
            C_DAXPY(nso,pow(sp[i], -1.0/2.0), &Up[0][i], nso, &Xp[0][j], nmo);
            j++;
        }
    }
    return X;
}
boost::shared_ptr<Matrix> OmegaKS::build_H(boost::shared_ptr<BasisSet> primary)
{
    int nso = primary->nbf();
    
    boost::shared_ptr<Matrix> H(new Matrix("H", nso, nso));
    boost::shared_ptr<Matrix> T(new Matrix("T", nso, nso));
    boost::shared_ptr<Matrix> V(new Matrix("V", nso, nso));

    // Integral factory
    boost::shared_ptr<IntegralFactory> integral(new IntegralFactory(primary, primary, primary, primary));
    boost::shared_ptr<OneBodyAOInt>    soT(integral->ao_kinetic());
    boost::shared_ptr<OneBodyAOInt>    soV(integral->ao_potential());

    soT->compute(T);
    soV->compute(V);

    H->copy(T);
    H->add(V);

    return H;
}
OmegaIPKS::OmegaIPKS(Options& options, boost::shared_ptr<PSIO> psio) :
    OmegaKS(options, psio)
{
    common_init();
}
OmegaIPKS::~OmegaIPKS()
{
}
void OmegaIPKS::common_init()
{
    reference_ = Process::environment.reference_wavefunction();
    factory_ = reference_->matrix_factory();

    if (factory_->nirrep() != 1)
        throw PSIEXCEPTION("You want symmetry? Find another coder.");

    molecule_ = reference_->molecule();
    basisset_ = reference_->basisset();

    // If the user doesn't spec a basis name, pick it yourself
    // TODO: Verify that the basis assign does not messs this up
    if (options_.get_str("RI_BASIS_SCF") == "") {
        basisset_->molecule()->set_basis_all_atoms(options_.get_str("BASIS") + "-JKFIT", "RI_BASIS_SCF");
        fprintf(outfile, "  No auxiliary basis selected, defaulting to %s-JKFIT\n\n", options_.get_str("BASIS").c_str()); 
    }

    boost::shared_ptr<BasisSetParser> parser(new Gaussian94BasisSetParser());
    auxiliary_ = BasisSet::construct(parser, basisset_->molecule(), "RI_BASIS_SCF");
    parser.reset();
}
void OmegaIPKS::print_header()
{
    fprintf(outfile, "\n");
    fprintf(outfile, "         ---------------------------------------------------------\n");
    fprintf(outfile, "                          Optimized-Omega RC-KS\n");
    fprintf(outfile, "                            IP-Only Algorithm                \n");
    fprintf(outfile, "                               Rob Parrish\n"); 
    fprintf(outfile, "                      %3d Threads, %6ld MiB Core\n", nthread_, memory_ / 1000000L);
    fprintf(outfile, "         ---------------------------------------------------------\n\n");
    
    fprintf(outfile, " ==> Geometry <==\n\n");
    molecule_->print();
    fprintf(outfile, "  Nuclear repulsion = %20.15f\n\n", basisset_->molecule()->nuclear_repulsion_energy());
    fprintf(outfile, "  Nalpha = %d\n", reference_->nalphapi()[0]);
    fprintf(outfile, "  Nbeta  = %d\n\n", reference_->nbetapi()[0]);
    fprintf(outfile, "  ==> Primary Basis <==\n\n");
    basisset_->print_by_level(outfile, print_);
    fprintf(outfile, "  ==> Primary Basis <==\n\n");
    auxiliary_->print_by_level(outfile, print_);
    fflush(outfile);

}
void OmegaIPKS::form_DF()
{
    df_ = boost::shared_ptr<OmegaDF>(new OmegaDF(psio_, basisset_, auxiliary_));
    df_->set_omega(functional_->getOmega());
}
void OmegaIPKS::form_KS()
{
    // Temporary print, to make sure we're in the right spot
    fprintf(outfile,"  Selected Functional is %s.\n\n",functional_->getName().c_str());
    boost::shared_ptr<Integrator> integrator = Integrator::build_integrator(molecule_, psio_, options_);

    //Grab the properties object for this basis
    boost::shared_ptr<Properties> properties = boost::shared_ptr<Properties> (new Properties(basisset_,block_size_));
    properties->setCutoffEpsilon(options_.get_double("DFT_BASIS_EPSILON"));
    //Always need rho
    properties->setToComputeDensity(true);
    if (functional_->isGGA()) {
        //Sometimes need gamma
        properties->setToComputeDensityGradient(true);
    }
    if (functional_->isMeta()) {
        //sometimes need tau
        properties->setToComputeKEDensity(true);
    }

    ks_ = boost::shared_ptr<OmegaV>(new OmegaV(psio_, basisset_,
        functional_, integrator, properties));
}
void OmegaIPKS::form_H()
{
    H_ = OmegaKS::build_H(basisset_);

    if (debug_ > 2 ) {
        H_->print(outfile);
    }
}
void OmegaIPKS::form_X()
{
    S_ = OmegaKS::build_S(basisset_);
    X_ = OmegaKS::build_X(basisset_, options_.get_double("S_MIN_EIGENVALUE"));

    if (debug_ > 2)
        X_->print();

    int nmo = X_->colspi()[0];
    int nso = X_->rowspi()[0];

    if (print_)
        fprintf(outfile, "  Canonical Orthogonalization: %d of %d functions retained, %d projected\n\n", nmo, nso, nso - nmo);
}
void OmegaIPKS::finalize() 
{
    fprintf(outfile, "  *** Omega Optimization Profile ***\n\n");
    fprintf(outfile, "  %4s %12s %12s %12s %12s %s\n", "Iter", "Omega", "kIP", "IP", "Delta IP", "Step Type");
    for (int i = 0; i < omega_trace_.size(); i++) {
        fprintf(outfile, "  %4d %12.5E %12.5E %12.5E %12.5E %s\n", i+1, get<0>(omega_trace_[i]),
            get<1>(omega_trace_[i]), get<2>(omega_trace_[i]), get<1>(omega_trace_[i]) -
            get<2>(omega_trace_[i]), get<3>(omega_trace_[i]).c_str());
    }
    fprintf(outfile, "\n");

    S_.reset();
    X_.reset();
    H_.reset();
}
void OmegaIPKS::populate()
{
    int na_N = reference_->nalphapi()[0];
    int nb_N = reference_->nbetapi()[0];

    int na_M;
    int nb_M;
    if (na_N == nb_N) {
        na_M = na_N;
        nb_M = nb_N - 1;
    } else {
        na_M = na_N - 1;
        nb_M = nb_N;
    }

    boost::shared_ptr<Matrix> Ca = reference_->Ca();
    boost::shared_ptr<Matrix> Cb = reference_->Cb();

    wfns_["N"] = boost::shared_ptr<OmegaWavefunction>(new OmegaWavefunction(
        options_, psio_, basisset_, Ca, na_N, Cb, nb_N, S_, X_, H_, df_, ks_)); 

    wfns_["N-1"] = boost::shared_ptr<OmegaWavefunction>(new OmegaWavefunction(
        options_, psio_, basisset_, Ca, na_M, Cb, nb_M, S_, X_, H_, df_, ks_)); 
}
void OmegaIPKS::omega_step(int iteration)
{
    if (iteration == 0) {
        double old_omega = functional_->getOmega();
        double kIP = wfns_["N"]->koopmansIP();
        double IP = wfns_["N-1"]->E() - wfns_["N"]->E();    
        omega_trace_.push_back(make_tuple(old_omega, kIP, IP, "Initial Guess"));        
        bracketed_ = false;
    }

    // Bracket first
    if (!bracketed_) {
        int n = omega_trace_.size();
        double old_omega = get<0>(omega_trace_[n-1]);
        double kIP =       get<1>(omega_trace_[n-1]);
        double IP =        get<2>(omega_trace_[n-1]);
        double delta = kIP - IP;
    
        double multiplier = options_.get_double("OMEGA_BRACKET_ALPHA");
        
        // too HF-like
        if (delta > 0) {
            multiplier = 1.0 / multiplier;
        // too KS-like
        } else { 
            multiplier = multiplier;
        }

        double new_omega = old_omega * multiplier;
    
        df_->set_omega(new_omega);
        ks_->set_omega(new_omega);
   
        fprintf(outfile, "  *** Bracketing Step ***\n\n");
        fprintf(outfile, "    Old Omega:    %15.8E\n", old_omega); 
        fprintf(outfile, "    Old kIP:      %15.8E\n", kIP); 
        fprintf(outfile, "    Old IP:       %15.8E\n", IP); 
        fprintf(outfile, "    Old Delta IP: %15.8E\n", kIP - IP); 
        fprintf(outfile, "    Multipler:    %15.8E\n", multiplier); 
        fprintf(outfile, "    New Omega:    %15.8E\n\n", new_omega); 
 
        return;
    }

    
    double new_omega = 0.0;
    
    if (options_.get_str("OMEGA_ROOT_ALGORITHM") == "REGULA_FALSI") {
        fprintf(outfile, "  *** Regula-Falsi Step ***\n\n");
        if (fabs(delta_l_ - delta_r_) > 1.0E-12) {
            new_omega = -delta_l_ * (omega_r_ - omega_l_) / (delta_r_ - delta_l_) + omega_l_;
        } else { 
            new_omega = omega_l_;
        }
    } else if (options_.get_str("OMEGA_ROOT_ALGORITHM") == "BISECTION") {
        fprintf(outfile, "  *** Bisection Step ***\n\n");
        new_omega = 0.5 * (omega_l_ + omega_r_); 
    }

    df_->set_omega(new_omega);
    ks_->set_omega(new_omega);

    fprintf(outfile, "    New Omega:    %15.8E\n\n", new_omega); 
}
bool OmegaIPKS::is_omega_converged()
{
    double new_omega = functional_->getOmega();
    double new_kIP = wfns_["N"]->koopmansIP();
    double new_IP = wfns_["N-1"]->E() - wfns_["N"]->E();    
    double new_delta = new_kIP - new_IP; 
   
    fprintf(outfile, "  *** Step Results ***\n\n");
    fprintf(outfile, "    Omega:    %15.8E\n", new_omega); 
    fprintf(outfile, "    kIP:      %15.8E\n", new_kIP); 
    fprintf(outfile, "    IP:       %15.8E\n", new_IP); 
    fprintf(outfile, "    Delta IP: %15.8E\n\n", new_kIP - new_IP); 
 
    // Has the root been bracketed?
    if (!bracketed_) {

        int n = omega_trace_.size();
        double old_omega = get<0>(omega_trace_[n-1]);
        double old_kIP =   get<1>(omega_trace_[n-1]);
        double old_IP =    get<2>(omega_trace_[n-1]);
        double old_delta = old_kIP - old_IP;

        omega_trace_.push_back(make_tuple(new_omega, new_kIP, new_IP, "Bracket Step"));        

        if (old_delta * new_delta < 0.0) {
            fprintf(outfile, "  *** Omega Bracketed ***\n\n");

            if (old_delta < 0.0) {
                omega_l_ = old_omega;
                delta_l_ = old_delta;
                omega_r_ = new_omega;
                delta_r_ = new_delta;
            } else {
                omega_r_ = old_omega;
                delta_r_ = old_delta;
                omega_l_ = new_omega;
                delta_l_ = new_delta;
            }

            bracketed_ = true;
        }
        return false;
    }

    if (new_delta < 0.0) {
        omega_l_ = new_omega;
        delta_l_ = new_delta;
    } else {
        omega_r_ = new_omega;
        delta_r_ = new_delta;
    }

    if (options_.get_str("OMEGA_ROOT_ALGORITHM") == "REGULA_FALSI") {
        omega_trace_.push_back(make_tuple(new_omega, new_kIP, new_IP, "Regula-Falsi Step"));        
    } else if (options_.get_str("OMEGA_ROOT_ALGORITHM") == "BISECTION") {
        omega_trace_.push_back(make_tuple(new_omega, new_kIP, new_IP, "Bisection Step"));        
    }

    double w_converge = pow(10.0, - options_.get_int("OMEGA_CONVERGE"));
    return (fabs(omega_l_ - omega_r_) < w_converge); 
}    

}} // End Namespaces
