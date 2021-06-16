#ifndef EDYN_SYS_INTEGRATE_LINACC_HPP
#define EDYN_SYS_INTEGRATE_LINACC_HPP

#include <entt/entity/registry.hpp>
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/linacc.hpp"
#include "edyn/comp/tag.hpp"

namespace edyn {

inline void integrate_linacc(entt::registry &registry, scalar dt) {
    auto view = registry.view<linvel, linacc, dynamic_tag>();
    view.each([&] (linvel &vel, linacc &acc) {
        vel += acc * dt;
    });
}

}

#endif // EDYN_SYS_INTEGRATE_LINACC_HPP
