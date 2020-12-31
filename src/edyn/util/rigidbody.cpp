#include <entt/entt.hpp>
#include "edyn/util/rigidbody.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/aabb.hpp"
#include "edyn/comp/shape.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/spin.hpp"
#include "edyn/comp/linacc.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/tire_material.hpp"
#include "edyn/comp/present_position.hpp"
#include "edyn/comp/present_orientation.hpp"
#include "edyn/comp/collision_filter.hpp"
#include "edyn/comp/tire_state.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/comp/continuous.hpp"

namespace edyn {

void rigidbody_def::update_inertia() {
    std::visit([&] (auto &&s) {
        inertia = s.inertia(mass);
    }, *shape_opt);
}

void make_rigidbody(entt::entity entity, entt::registry &registry, const rigidbody_def &def) {    
    registry.emplace<position>(entity, def.position);
    registry.emplace<orientation>(entity, def.orientation);

    if (def.kind == rigidbody_kind::rb_dynamic) {
        EDYN_ASSERT(def.mass > 0);
        registry.emplace<mass>(entity, def.mass);
        registry.emplace<mass_inv>(entity, def.mass < EDYN_SCALAR_MAX ? 1 / def.mass : 0);
        registry.emplace<inertia>(entity, def.inertia);
        auto &invI = registry.emplace<inertia_inv>(entity, 
            vector3 {
                def.inertia.x < EDYN_SCALAR_MAX ? 1 / def.inertia.x : 0,
                def.inertia.y < EDYN_SCALAR_MAX ? 1 / def.inertia.y : 0,
                def.inertia.z < EDYN_SCALAR_MAX ? 1 / def.inertia.z : 0
            });
        registry.emplace<inertia_world_inv>(entity, diagonal(invI));

        if (def.spin_enabled) {
            registry.emplace<spin_angle>(entity, def.spin_angle);
            registry.emplace<spin>(entity, def.spin);
        }
    } else {
        registry.emplace<mass>(entity, EDYN_SCALAR_MAX);
        registry.emplace<mass_inv>(entity, 0);
        registry.emplace<inertia>(entity, vector3_max);
        registry.emplace<inertia_inv>(entity, vector3_zero);
        registry.emplace<inertia_world_inv>(entity, matrix3x3_zero);
    }

    if (def.kind == rigidbody_kind::rb_static) {
        registry.emplace<linvel>(entity, vector3_zero);
        registry.emplace<angvel>(entity, vector3_zero);
    } else {
        registry.emplace<linvel>(entity, def.linvel);
        registry.emplace<angvel>(entity, def.angvel);
    }

    if (def.kind == rigidbody_kind::rb_dynamic && def.gravity != vector3_zero) {
        registry.emplace<linacc>(entity, def.gravity);
    }

    if (!def.sensor) {
        registry.emplace<material>(entity, def.restitution, def.friction,
                                  def.stiffness, def.damping);

        if (def.is_tire) {
            registry.emplace<tire_material>(entity, def.lon_tread_stiffness,
                                            def.lat_tread_stiffness, 
                                            def.speed_sensitivity, def.load_sensitivity);
            registry.emplace<tire_state>(entity);
        }
    }

    if (def.presentation) {
        registry.emplace<present_position>(entity, def.position);
        registry.emplace<present_orientation>(entity, def.orientation);
    }

    if (auto opt = def.shape_opt) {
        auto &sh = registry.emplace<shape>(entity, *opt);

        std::visit([&] (auto &&s) {
            registry.emplace<AABB>(entity, s.aabb(def.position, def.orientation));
        }, sh.var);

        auto &filter = registry.emplace<collision_filter>(entity);
        filter.group = def.collision_group;
        filter.mask = def.collision_mask;
    }

    switch (def.kind) {
    case rigidbody_kind::rb_dynamic:
        registry.emplace<dynamic_tag>(entity);
        registry.emplace<procedural_tag>(entity);
        break;
    case rigidbody_kind::rb_kinematic:
        registry.emplace<kinematic_tag>(entity);
        break;
    case rigidbody_kind::rb_static:
        registry.emplace<static_tag>(entity);
        break;
    }

    if (def.kind == rigidbody_kind::rb_dynamic) {
        // Instruct island worker to continuously send position, orientation and
        // velocity updates back to coordinator. The velocity is needed for calculation
        // of the present position and orientation in `update_presentation`.
        registry.emplace<continuous>(entity).insert<position, orientation, linvel, angvel>();
    }

    registry.emplace<island_node>(entity);
    registry.emplace<island_container>(entity);
}

entt::entity make_rigidbody(entt::registry &registry, const rigidbody_def &def) {
    auto ent = registry.create();
    make_rigidbody(ent, registry, def);
    return ent;
}

void rigidbody_set_mass(entt::registry &registry, entt::entity entity, scalar mass) {
    registry.replace<edyn::mass>(entity, mass);
    rigidbody_update_inertia(registry, entity);
}

void rigidbody_update_inertia(entt::registry &registry, entt::entity entity) {
    auto &mass = registry.get<edyn::mass>(entity);

    edyn::vector3 inertia;
    auto& shape = registry.get<edyn::shape>(entity);
    std::visit([&] (auto&& s) {
        inertia = s.inertia(mass);
    }, shape.var);
    registry.replace<edyn::inertia>(entity, inertia);
}

void rigidbody_apply_impulse(entt::registry &registry, entt::entity entity, 
                             const vector3 &impulse, const vector3 &rel_location) {
    auto &m_inv = registry.get<mass_inv>(entity);
    auto &i_inv = registry.get<inertia_world_inv>(entity);
    registry.get<linvel>(entity) += impulse * m_inv;
    registry.get<angvel>(entity) += i_inv * cross(rel_location, impulse);
}

void update_kinematic_position(entt::registry &registry, entt::entity entity, const vector3 &pos, scalar dt) {
    EDYN_ASSERT(registry.has<kinematic_tag>(entity));
    auto &curpos = registry.get<position>(entity);
    auto &vel = registry.get<linvel>(entity);
    vel = (pos - curpos) / dt;
    curpos = pos;
}

void update_kinematic_orientation(entt::registry &registry, entt::entity entity, const quaternion &orn, scalar dt) {
    EDYN_ASSERT(registry.has<kinematic_tag>(entity));
    auto &curorn = registry.get<orientation>(entity);
    auto q = normalize(conjugate(curorn) * orn);
    auto &vel = registry.get<angvel>(entity);
    vel = (quaternion_axis(q) * quaternion_angle(q)) / dt;
    curorn = orn;
}

void clear_kinematic_velocities(entt::registry &registry) {
    auto view = registry.view<kinematic_tag, linvel, angvel>();
    view.each([] ([[maybe_unused]] auto, linvel &v, angvel &w) {
        v = vector3_zero;
        w = vector3_zero;
    });

    registry.view<kinematic_tag, spin>().each([] ([[maybe_unused]] auto, spin &s) {
        s.s = 0;
    });
}

bool validate_rigidbody(entt::entity entity, entt::registry &registry) {
    return registry.has<position, orientation, linvel, angvel>(entity);
}

}
