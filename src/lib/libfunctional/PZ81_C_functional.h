#ifndef PZ81_C_functional_h
#define PZ81_C_functional_h
/**********************************************************
* PZ81_C_functional.h: declarations for PZ81_C_functional for KS-DFT
* Robert Parrish, robparrish@gmail.com
* Autogenerated by MATLAB Script on 25-May-2011
*
***********************************************************/
#include "functional.h"

namespace psi { namespace functional {

class PZ81_C_Functional : public Functional {
public:
    PZ81_C_Functional(int npoints, int deriv);
    virtual ~PZ81_C_Functional();
    virtual void computeRKSFunctional(boost::shared_ptr<Properties> prop);
    virtual void computeUKSFunctional(boost::shared_ptr<Properties> prop);
};
}}
#endif

