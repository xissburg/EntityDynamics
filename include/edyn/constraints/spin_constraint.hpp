#ifndef EDYN_CONSTRAINTS_SPIN_CONSTRAINT_HPP
#define EDYN_CONSTRAINTS_SPIN_CONSTRAINT_HPP

#include "constraint_base.hpp"

namespace edyn {

struct spin_constraint : public constraint_base<spin_constraint> {
    scalar m_ratio {1};
    scalar m_stiffness {1e5};
    scalar m_damping {1e2};
    scalar m_offset;

    void init(entt::entity, constraint &, const relation &, entt::registry &);
    void prepare(entt::entity, constraint &, const relation &, entt::registry &, scalar dt);

    void set_ratio(scalar, const relation &, entt::registry &);
    scalar get_error(const relation &, entt::registry &) const;
};

}

#endif // EDYN_CONSTRAINTS_SPIN_CONSTRAINT_HPP