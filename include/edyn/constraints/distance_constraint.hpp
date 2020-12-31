#ifndef EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT
#define EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT

#include <array>
#include "constraint_base.hpp"
#include "edyn/math/vector3.hpp"

namespace edyn {

struct distance_constraint : public constraint_base<distance_constraint> {
    std::array<vector3, 2> pivot;
    scalar distance {0};

    void init(entt::entity, constraint &, entt::registry &);
    void prepare(entt::entity, constraint &, entt::registry &, scalar dt);
};

}

#endif // EDYN_CONSTRAINTS_DISTANCE_CONSTRAINT