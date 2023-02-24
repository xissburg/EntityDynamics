#ifndef EDYN_NETWORKING_SYS_ACCUMULATE_DISCONTINUITIES_HPP
#define EDYN_NETWORKING_SYS_ACCUMULATE_DISCONTINUITIES_HPP

#include <entt/entity/registry.hpp>
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/networking/comp/discontinuity.hpp"

namespace edyn {

inline void accumulate_discontinuities(entt::registry &registry) {
    auto discontinuity_view = registry.view<previous_position, position, previous_orientation, orientation, discontinuity>();

    for (auto [entity, p_pos, pos, p_orn, orn, discontinuity] : discontinuity_view.each()) {
        // TODO: if error is too large, past a threshold, do not accumulate.
        // Zero it out instead. That is, just snap into the new transform.
        discontinuity.position_offset += p_pos - pos;
        discontinuity.orientation_offset *= p_orn * conjugate(orn);
    }

    auto discontinuity_spin_view = registry.view<previous_spin_angle, spin_angle, discontinuity_spin>();

    for (auto [e, p_spin, spin, dis] : discontinuity_spin_view.each()) {
        dis.offset += (p_spin.count - spin.count) * pi2 + p_spin.s - spin.s;
    }
}

}

#endif // EDYN_NETWORKING_SYS_ACCUMULATE_DISCONTINUITIES_HPP
