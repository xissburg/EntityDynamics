#include <entt/entity/registry.hpp>
#include "edyn/math/matrix3x3.hpp"
#include "edyn/util/rigidbody.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/aabb.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/spin.hpp"
#include "edyn/comp/gravity.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/tire_material.hpp"
#include "edyn/comp/present_position.hpp"
#include "edyn/comp/present_orientation.hpp"
#include "edyn/comp/collision_filter.hpp"
#include "edyn/comp/tire_state.hpp"
#include "edyn/comp/continuous.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/util/moment_of_inertia.hpp"
#include "edyn/util/aabb_util.hpp"
#include "edyn/util/tuple_util.hpp"
#include "edyn/parallel/island_coordinator.hpp"
#include "edyn/edyn.hpp"

namespace edyn {

void rigidbody_def::update_inertia() {
    inertia = moment_of_inertia(*shape, mass);
}

void make_rigidbody(entt::entity entity, entt::registry &registry, const rigidbody_def &def) {
    registry.emplace<position>(entity, def.position);
    registry.emplace<orientation>(entity, def.orientation);

    if (def.kind == rigidbody_kind::rb_dynamic) {
        EDYN_ASSERT(def.mass > EDYN_EPSILON && def.mass < large_scalar);
        registry.emplace<mass>(entity, def.mass);
        registry.emplace<mass_inv>(entity, scalar(1) / def.mass);
        registry.emplace<inertia>(entity, def.inertia);

        auto I_inv = inverse_matrix_symmetric(def.inertia);
        registry.emplace<inertia_inv>(entity, I_inv);
        registry.emplace<inertia_world_inv>(entity, I_inv);
    } else {
        registry.emplace<mass>(entity, EDYN_SCALAR_MAX);
        registry.emplace<mass_inv>(entity, scalar(0));
        registry.emplace<inertia>(entity, matrix3x3_zero);
        registry.emplace<inertia_inv>(entity, matrix3x3_zero);
        registry.emplace<inertia_world_inv>(entity, matrix3x3_zero);
    }

    if (def.kind == rigidbody_kind::rb_static) {
        registry.emplace<linvel>(entity, vector3_zero);
        registry.emplace<angvel>(entity, vector3_zero);
    } else {
        registry.emplace<linvel>(entity, def.linvel);
        registry.emplace<angvel>(entity, def.angvel);
    }

    auto gravity = def.gravity ? *def.gravity : get_gravity(registry);

    if (gravity != vector3_zero && def.kind == rigidbody_kind::rb_dynamic) {
        registry.emplace<edyn::gravity>(entity, gravity);
    }

    if (!def.sensor) {
        registry.emplace<material>(entity, def.restitution, def.friction,
                                  def.stiffness, def.damping);

        if (def.is_tire) {
            registry.emplace<tire_material>(entity, def.lon_tread_stiffness, def.lat_tread_stiffness,
                                            def.speed_sensitivity, def.load_sensitivity);
            registry.emplace<tire_state>(entity);
        }
    }

    if (def.presentation && def.kind == rigidbody_kind::rb_dynamic) {
        registry.emplace<present_position>(entity, def.position);
        registry.emplace<present_orientation>(entity, def.orientation);

        if (def.spin_enabled) {
            registry.emplace<present_spin_angle>(entity, def.spin_angle);
        }
    }

    if (auto opt = def.shape) {
        std::visit([&] (auto &&shape) {
            using ShapeType = std::decay_t<decltype(shape)>;

            // Ensure shape is valid for this type of rigid body.
            if (def.kind != rigidbody_kind::rb_static) {
                EDYN_ASSERT((!has_type<ShapeType, std::decay_t<decltype(static_shapes_tuple)>>::value));
            }

            registry.emplace<ShapeType>(entity, shape);
            registry.emplace<shape_index>(entity, get_shape_index<ShapeType>());
            auto aabb = shape_aabb(shape, def.position, def.orientation);
            registry.emplace<AABB>(entity, aabb);
        }, *def.shape);

        auto &filter = registry.emplace<collision_filter>(entity);
        filter.group = def.collision_group;
        filter.mask = def.collision_mask;
    }

    if (def.continuous_contacts) {
        registry.emplace<continuous_contacts_tag>(entity);
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
        // TODO: synchronized merges would eliminate the need to share these
        // components continuously.
        registry.emplace<continuous>(entity).insert<position, orientation, linvel, angvel>();
    }

    if (def.spin_enabled && def.kind == rigidbody_kind::rb_dynamic) {
        registry.emplace<spin_angle>(entity, def.spin_angle);
        registry.emplace<spin>(entity, def.spin);
        registry.get_or_emplace<continuous>(entity).insert<spin_angle, spin>();
    }

    auto non_connecting = def.kind != rigidbody_kind::rb_dynamic;
    auto node_index = registry.ctx<entity_graph>().insert_node(entity, non_connecting);
    registry.emplace<graph_node>(entity, node_index);

    // Always do this last to signal the completion of the construction of this
    // rigid body.
    registry.emplace<rigidbody_tag>(entity);
}

entt::entity make_rigidbody(entt::registry &registry, const rigidbody_def &def) {
    auto ent = registry.create();
    make_rigidbody(ent, registry, def);
    return ent;
}

std::vector<entt::entity> batch_rigidbodies(entt::registry &registry, const std::vector<rigidbody_def> &defs) {
    std::vector<entt::entity> entities;
    entities.reserve(defs.size());

    for (auto &def : defs) {
        entities.push_back(make_rigidbody(registry, def));
    }

    auto &coordinator = registry.ctx<island_coordinator>();
    coordinator.create_island(entities);
    return entities;
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
    view.each([] (linvel &v, angvel &w) {
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

void set_rigidbody_mass(entt::registry &registry, entt::entity entity, scalar mass) {
    EDYN_ASSERT(mass > EDYN_EPSILON && mass < large_scalar);
    EDYN_ASSERT(registry.has<dynamic_tag>(entity));
    EDYN_ASSERT(registry.has<rigidbody_tag>(entity));
    registry.replace<edyn::mass>(entity, mass);
    registry.replace<edyn::mass_inv>(entity, scalar(1.0) / mass);
    refresh<edyn::mass, edyn::mass_inv>(registry, entity);
}

void set_rigidbody_inertia(entt::registry &registry, entt::entity entity, const matrix3x3 &inertia) {
    EDYN_ASSERT(registry.has<dynamic_tag>(entity));
    EDYN_ASSERT(registry.has<rigidbody_tag>(entity));
    auto I_inv = inverse_matrix_symmetric(inertia);
    registry.replace<edyn::inertia>(entity, inertia);
    registry.replace<edyn::inertia_inv>(entity, I_inv);
    refresh<edyn::inertia, edyn::inertia_inv>(registry, entity);
}

void set_rigidbody_friction(entt::registry &registry, entt::entity entity, scalar friction) {
    EDYN_ASSERT(registry.has<rigidbody_tag>(entity));

    auto material_view = registry.view<material>();
    auto manifold_view = registry.view<contact_manifold>();
    auto cp_view = registry.view<contact_point>();

    material_view.get(entity).friction = friction;
    refresh<material>(registry, entity);

    auto &graph = registry.ctx<entity_graph>();
    auto &node = registry.get<graph_node>(entity);

    graph.visit_edges(node.node_index, [&] (auto edge_entity) {
        if (!manifold_view.contains(edge_entity)) {
            return;
        }

        auto &manifold = manifold_view.get(edge_entity);

        auto other_entity = manifold.body[0] == entity ? manifold.body[1] : manifold.body[0];
        auto &other_material = material_view.get(other_entity);
        auto num_points = manifold.num_points();

        for (size_t i = 0; i < num_points; ++i) {
            auto &cp = cp_view.get(manifold.point[i]);
            cp.friction = friction * other_material.friction;
            refresh<contact_point>(registry, manifold.point[i]);
        }
    });
}

}
