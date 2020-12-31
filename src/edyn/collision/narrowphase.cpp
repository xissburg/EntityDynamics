#include "edyn/collision/narrowphase.hpp"
#include "edyn/comp/constraint.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/tire_material.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/shape.hpp"
#include "edyn/comp/constraint_row.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/aabb.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/collision/collide.hpp"
#include "edyn/math/geom.hpp"
#include "edyn/util/tire_util.hpp"
#include "edyn/util/constraint_util.hpp"
#include <entt/entt.hpp>
#include <utility>

namespace edyn {

// Update distance of persisted contact points.
static
void update_contact_distances(entt::registry &registry) {
    auto cp_view = registry.view<contact_point>();
    auto tr_view = registry.view<position, orientation>();

    cp_view.each([&] (auto, contact_point &cp) {
        auto [posA, ornA] = tr_view.get<position, orientation>(cp.body[0]);
        auto [posB, ornB] = tr_view.get<position, orientation>(cp.body[1]);
        auto pivotA_world = posA + rotate(ornA, cp.pivotA);
        auto pivotB_world = posB + rotate(ornB, cp.pivotB);
        auto normal_world = rotate(ornB, cp.normalB);
        cp.distance = dot(normal_world, pivotA_world - pivotB_world);
    });
}

// Merges a `collision_point` onto a `contact_point`.
static
void merge_point(const collision_result::collision_point &rp, contact_point &cp) {
    cp.pivotA = rp.pivotA;
    cp.pivotB = rp.pivotB;
    cp.normalB = rp.normalB;
    cp.distance = rp.distance;
}

static
void create_contact_constraint(entt::registry &registry, entt::entity manifold_entity,
                               entt::entity contact_entity, contact_point &cp,
                               tire_material *tire) {

    auto &materialA = registry.get<material>(cp.body[0]);
    auto &materialB = registry.get<material>(cp.body[1]);

    cp.restitution = materialA.restitution * materialB.restitution;
    cp.friction = materialA.friction * materialB.friction;

    auto stiffness = large_scalar;
    auto damping = large_scalar;

    if (materialA.stiffness < large_scalar || materialB.stiffness < large_scalar) {
        stiffness = 1 / (1 / materialA.stiffness + 1 / materialB.stiffness);
        damping = 1 / (1 / materialA.damping + 1 / materialB.damping);
    }

    if (tire) {
        // Contact patch is always a soft contact since it needs deflection
        // to generate friction forces.
        EDYN_ASSERT(stiffness < large_scalar);

        auto contact = contact_patch_constraint();
        contact.m_normal_stiffness = stiffness;
        contact.m_normal_damping = damping;
        contact.m_speed_sensitivity = tire->speed_sensitivity;
        contact.m_load_sensitivity = tire->load_sensitivity;
        contact.m_lat_tread_stiffness = tire->lat_tread_stiffness;
        contact.m_lon_tread_stiffness = tire->lon_tread_stiffness;
        
        // Swap entities to ensure the tire/cylinder is in the first entity.
        auto &shapeA = registry.get<shape>(cp.body[0]);
        if (std::holds_alternative<cylinder_shape>(shapeA.var)) {
            make_constraint(contact_entity, registry, std::move(contact), cp.body[0], cp.body[1], &manifold_entity);
        } else {
            make_constraint(contact_entity, registry, std::move(contact), cp.body[1], cp.body[0], &manifold_entity);
        }
    } else {
        auto contact = contact_constraint();
        contact.stiffness = stiffness;
        contact.damping = damping;
        make_constraint(contact_entity, registry, std::move(contact), cp.body[0], cp.body[1], &manifold_entity);
    }
}

static
void contact_point_changed(entt::entity entity, entt::registry &registry, 
                           contact_point &cp) {
    auto &con = registry.get<constraint>(entity);
    // One of the existing contacts has been replaced. Update its rows.
    // Zero out warm-starting impulses.
    for (size_t i = 0; i < con.num_rows(); ++i) {
        auto &row = registry.get<constraint_row>(con.row[i]);
        row.impulse = 0;
    }
}

template<typename ContactPointViewType>
static 
size_t find_nearest_contact(const contact_manifold &manifold, 
                            const collision_result::collision_point &coll_pt,
                            const ContactPointViewType &cp_view) {
    auto shortest_dist = contact_caching_threshold * contact_caching_threshold;
    auto nearest_idx = manifold.num_points();

    for (size_t i = 0; i < manifold.num_points(); ++i) {
        auto &cp = cp_view.get(manifold.point[i]);
        auto dA = length_sqr(coll_pt.pivotA - cp.pivotA);
        auto dB = length_sqr(coll_pt.pivotB - cp.pivotB);

        if (dA < shortest_dist) {
            shortest_dist = dA;
            nearest_idx = i;
        }

        if (dB < shortest_dist) {
            shortest_dist = dB;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

template<typename ContactPointViewType>
static
size_t find_nearest_contact_tire(const contact_manifold &manifold, 
                                 const collision_result::collision_point &coll_pt,
                                 const ContactPointViewType &cp_view, 
                                 scalar tire_radius) {
    // Find point closest to the same angle along the plane orthogonal to
    // the cylinder axis.
    const auto coll_pt_angle = std::atan2(coll_pt.pivotA.y, coll_pt.pivotA.z);

    auto shortest_dist = contact_caching_threshold;
    auto nearest_idx = manifold.num_points();

    for (size_t i = 0; i < manifold.num_points(); ++i) {
        auto &cp = cp_view.get(manifold.point[i]);
        auto cp_angle = std::atan2(cp.pivotA.y, cp.pivotA.z);
        auto dist = std::abs(cp_angle - coll_pt_angle) * tire_radius;

        if (dist < shortest_dist) {
            shortest_dist = dist;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

static
void process_collision(entt::registry &registry, entt::entity manifold_entity, 
                       contact_manifold &manifold, const collision_result &result) {

    auto *tire0 = registry.try_get<tire_material>(manifold.body[0]);
    auto *tire1 = registry.try_get<tire_material>(manifold.body[1]);
    auto *tire = tire0 ? tire0 : tire1;
    scalar tire_radius;

    if (tire) {
        auto &tire_shape = registry.get<shape>(manifold.body[0]);
        auto &tire_cyl = std::get<cylinder_shape>(tire_shape.var);
        tire_radius = tire_cyl.radius;
    }

    auto cp_view = registry.view<contact_point>();

    // Merge new with existing contact points.
    for (size_t i = 0; i < result.num_points; ++i) {
        auto &rp = result.point[i];

        // Find closest existing point.
        size_t nearest_idx;
        
        if (tire) {
            nearest_idx = find_nearest_contact_tire(manifold, rp, cp_view, tire_radius);
        } else {
            nearest_idx = find_nearest_contact(manifold, rp, cp_view);
        }

        if (nearest_idx < manifold.num_points()) {
            auto &cp = cp_view.get(manifold.point[nearest_idx]);
            ++cp.lifetime;
            merge_point(rp, cp);
        } else {
            // Assign it to array of points and set it up.
            // Find best insertion index. Try pivotA first.
            std::array<vector3, max_contacts> pivots;
            std::array<scalar, max_contacts> distances;
            for (size_t i = 0; i < manifold.num_points(); ++i) {
                auto &cp = cp_view.get(manifold.point[i]);
                pivots[i] = cp.pivotB;
                distances[i] = cp.distance;
            }

            auto idx = insert_index(pivots, distances, manifold.num_points(), rp.pivotB, rp.distance);

            // No closest point found for pivotA, try pivotB.
            if (idx >= manifold.num_points()) {
                for (size_t i = 0; i < manifold.num_points(); ++i) {
                    auto &cp = cp_view.get(manifold.point[i]);
                    pivots[i] = cp.pivotB;
                }

                idx = insert_index(pivots, distances, manifold.num_points(), rp.pivotB, rp.distance);
            }

            if (idx < max_contacts) {
                auto is_new_contact = idx == manifold.num_points();

                if (is_new_contact) {
                    auto contact_entity = registry.create();
                    manifold.point[idx] = contact_entity;

                    auto &cp = registry.emplace<contact_point>(
                        contact_entity, 
                        manifold.body,
                        rp.pivotA, // pivotA
                        rp.pivotB, // pivotB
                        rp.normalB, // normalB
                        0, // friction
                        0, // restitution
                        0, // lifetime
                        rp.distance // distance
                    );

                    if (registry.has<material>(manifold.body[0]) && registry.has<material>(manifold.body[1])) {
                        create_contact_constraint(registry, manifold_entity, contact_entity, cp, tire);
                    }

                    registry.get_or_emplace<dirty>(contact_entity)
                        .set_new()
                        .created<contact_point>();

                    registry.get_or_emplace<dirty>(manifold_entity).updated<contact_manifold>();
                } else {
                    // Replace existing contact point.
                    auto contact_entity = manifold.point[idx];
                    auto &cp = cp_view.get(contact_entity);
                    cp.lifetime = 0;
                    merge_point(rp, cp);
                    contact_point_changed(contact_entity, registry, cp);
                }
            }
        }
    }
}

static
void prune(entt::registry &registry, entt::entity entity, 
           contact_manifold &manifold,
           const vector3 &posA, const quaternion &ornA, 
           const vector3 &posB, const quaternion &ornB) {
    constexpr auto threshold_sqr = contact_breaking_threshold * contact_breaking_threshold;
    auto cp_view = registry.view<contact_point>();

    // Remove separating contact points.
    for (size_t i = manifold.num_points(); i > 0; --i) {
        size_t k = i - 1;
        auto point_entity = manifold.point[k];
        auto &cp = cp_view.get(point_entity);
        auto pA = posA + rotate(ornA, cp.pivotA);
        auto pB = posB + rotate(ornB, cp.pivotB);
        auto n = rotate(ornB, cp.normalB);
        auto d = pA - pB;
        auto dn = dot(d, n); // separation along normal
        auto dp = d - dn * n; // tangential separation on contact plane

        if (dn > contact_breaking_threshold || length_sqr(dp) > threshold_sqr) {
            registry.destroy(point_entity);

            // Swap with last element.
            size_t last_idx = manifold.num_points() - 1;
            
            if (last_idx != k) {
                manifold.point[k] = manifold.point[last_idx];
            }

            manifold.point[last_idx] = entt::null;

            auto &node_parent = registry.get<island_node_parent>(entity);
            node_parent.children.erase(point_entity);

            registry.get_or_emplace<dirty>(entity)
                .updated<contact_manifold, island_node_parent>();
        }
    }
}

narrowphase::narrowphase(entt::registry &reg)
    : m_registry(&reg)
{}

void narrowphase::update() {
    update_contact_distances(*m_registry);

    auto manifold_view = m_registry->view<contact_manifold>();
    update_contact_manifolds(manifold_view.begin(), manifold_view.end(), manifold_view);
}

void narrowphase::update_contact_manifold(entt::entity entity, contact_manifold &manifold, narrowphase::body_view_t &body_view) {
    auto [aabbA, posA, ornA] = body_view.get<AABB, position, orientation>(manifold.body[0]);
    auto [aabbB, posB, ornB] = body_view.get<AABB, position, orientation>(manifold.body[1]);
    const auto offset = vector3_one * -contact_breaking_threshold;

    // Only proceed to closest points calculation if AABBs intersect, since
    // a manifold is allowed to exist whilst the AABB separation is smaller
    // than `manifold.separation_threshold` which is greater than the
    // contact breaking threshold.
    if (intersect(aabbA.inset(offset), aabbB)) {
        auto &shapeA = body_view.get<shape>(manifold.body[0]);
        auto &shapeB = body_view.get<shape>(manifold.body[1]);

        auto result = collision_result{};
        // Structured binding is not captured by lambda, thus use an explicit
        // capture list (https://stackoverflow.com/a/48103632/749818).
        std::visit([&result, &shapeB, pA = posA, oA = ornA, pB = posB, oB = ornB] (auto &&sA) {
            std::visit([&] (auto &&sB) {
                result = collide(sA, pA, oA, sB, pB, oB, 
                                    contact_breaking_threshold);
            }, shapeB.var);
        }, shapeA.var);

        process_collision(*m_registry, entity, manifold, result);
    }

    prune(*m_registry, entity, manifold, posA, ornA, posB, ornB);
}

}