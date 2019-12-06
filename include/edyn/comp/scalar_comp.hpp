#ifndef EDYN_COMP_SCALAR_COMP_HPP
#define EDYN_COMP_SCALAR_COMP_HPP

#include "edyn/math/scalar.hpp"

namespace edyn {

/**
 * Base sutrct for scalar components (e.g. mass). Provides functions for 
 * implict conversion to scalar.
 */
struct scalar_comp {
    scalar s;

    operator scalar &() {
        return s;
    }

    operator scalar() const {
        return s;
    }
};

}

#endif // EDYN_COMP_SCALAR_COMP_HPP