#ifndef M05_2X_functional_h
#define M05_2X_functional_h
/**********************************************************
* M05_2X_functional.h: declarations for M05_2X_functional for KS-DFT
* Robert Parrish, robparrish@gmail.com
* Autogenerated by MATLAB Script on 25-May-2011
*
***********************************************************/
#include "functional.h"

namespace psi { namespace functional {

class M05_2X_Functional : public Functional {
public:
    M05_2X_Functional(int npoints, int deriv);
    virtual ~M05_2X_Functional();
    virtual void computeRKSFunctional(boost::shared_ptr<Properties> prop);
    virtual void computeUKSFunctional(boost::shared_ptr<Properties> prop);
};
}}
#endif

