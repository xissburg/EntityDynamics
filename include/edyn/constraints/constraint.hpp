#ifndef EDYN_CONSTRAINTS_CONSTRAINT_HPP
#define EDYN_CONSTRAINTS_CONSTRAINT_HPP

#include <tuple>
#include <entt/entity/fwd.hpp>
#include "edyn/constraints/distance_constraint.hpp"
#include "edyn/constraints/soft_distance_constraint.hpp"
#include "edyn/constraints/point_constraint.hpp"
#include "edyn/constraints/contact_constraint.hpp"
#include "edyn/constraints/hinge_constraint.hpp"
#include "edyn/constraints/generic_constraint.hpp"
#include "edyn/constraints/cvjoint_constraint.hpp"
#include "edyn/constraints/cone_constraint.hpp"
#include "edyn/constraints/gravity_constraint.hpp"
#include "edyn/constraints/tirecarcass_constraint.hpp"
#include "edyn/constraints/springdamper_constraint.hpp"
#include "edyn/constraints/spin_angle_constraint.hpp"
#include "edyn/constraints/spin_constraint.hpp"
#include "edyn/constraints/antiroll_constraint.hpp"
#include "edyn/constraints/doublewishbone_constraint.hpp"
#include "edyn/constraints/differential_constraint.hpp"
#include "edyn/constraints/triple_spin_constraint.hpp"
#include "edyn/constraints/tierod_constraint.hpp"
#include "edyn/constraints/contact_patch_constraint.hpp"
#include "edyn/dynamics/row_cache.hpp"

namespace edyn {

/**
 * @brief Tuple of all available constraints. They are solved in this order so
 * the more important constraints should be the last in the list.
 */
using constraints_tuple_t = std::tuple<
    gravity_constraint,
    distance_constraint,
    soft_distance_constraint,
    hinge_constraint,
    generic_constraint,
    cvjoint_constraint,
    cone_constraint,
    point_constraint,
    tirecarcass_constraint,
    springdamper_constraint,
    spin_angle_constraint,
    spin_constraint,
    antiroll_constraint,
    doublewishbone_constraint,
    triple_spin_constraint,
    differential_constraint,
    tierod_constraint,
    contact_patch_constraint,
    contact_constraint
>;

static const constraints_tuple_t constraints_tuple = constraints_tuple_t{};

}

#endif // EDYN_CONSTRAINTS_CONSTRAINT_HPP
