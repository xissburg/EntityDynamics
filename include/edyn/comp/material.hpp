#ifndef EDYN_COMP_MATERIAL_HPP
#define EDYN_COMP_MATERIAL_HPP

#include <limits>
#include "edyn/math/scalar.hpp"
#include "edyn/math/constants.hpp"

namespace edyn {

/**
 * @brief Base material info which can be set into the material mixing table
 * for a pair of material ids.
 */
struct material_base {
    scalar restitution {0};
    scalar friction {0.5};
    scalar spin_friction {0};
    scalar roll_friction {0};
    scalar stiffness {large_scalar};
    scalar damping {large_scalar};
};

/**
 * @brief Rigid body material component with optional identifier for material mixing.
 */
struct material : public material_base {
    using id_type = unsigned;
    id_type id {std::numeric_limits<id_type>::max()};
};

}

#endif // EDYN_COMP_MATERIAL_HPP
