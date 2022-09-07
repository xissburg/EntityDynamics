#ifndef EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT
#define EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT

#include <array>
#include <entt/entity/fwd.hpp>
#include "edyn/math/vector3.hpp"
#include "edyn/constraints/constraint_base.hpp"

namespace edyn {

struct matrix3x3;
struct quaternion;
struct constraint_row_prep_cache;

struct distance_constraint : public constraint_base {
    std::array<vector3, 2> pivot;
    scalar distance {0};
    scalar impulse {0};

    void prepare(
        const entt::registry &, entt::entity,
        constraint_row_prep_cache &cache, scalar dt,
        const vector3 &originA, const vector3 &posA, const quaternion &ornA,
        const vector3 &linvelA, const vector3 &angvelA,
        scalar inv_mA, const matrix3x3 &inv_IA,
        const vector3 &originB, const vector3 &posB, const quaternion &ornB,
        const vector3 &linvelB, const vector3 &angvelB,
        scalar inv_mB, const matrix3x3 &inv_IB);
};

template<typename Archive>
void serialize(Archive &archive, distance_constraint &c) {
    archive(c.body);
    archive(c.pivot);
    archive(c.distance);
    archive(c.impulse);
}

}

#endif // EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT
