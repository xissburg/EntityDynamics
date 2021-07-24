#ifndef EDYN_UTIL_COLLISION_UTIL_HPP
#define EDYN_UTIL_COLLISION_UTIL_HPP

#include <algorithm>
#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include "edyn/comp/aabb.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/shapes/shapes.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/collision_result.hpp"
#include "edyn/constraints/constraint_impulse.hpp"

namespace edyn {

/**
 * Update distance of persisted contact points.
 */
void update_contact_distances(entt::registry &registry);

/**
 * Merges a `collision_point` onto a `contact_point`.
 */
void merge_point(const collision_result::collision_point &rp, contact_point &cp);

/**
 * Creates a contact constraint for a contact point.
 */
void create_contact_constraint(entt::registry &registry,
                               entt::entity contact_entity,
                               contact_point &cp);

/**
 * Finds the index of a point in the result that's closest to the contact point.
 * This is used to find a similar point in the result which could be replaced
 * by the new contact point.
 */
size_t find_nearest_contact(const contact_point &cp,
                            const collision_result &result);

size_t find_nearest_contact_tire(const contact_point &cp,
                                 const collision_result &result);

/**
 * Creates a contact point from a result point and inserts it into a
 * manifold.
 */
entt::entity create_contact_point(entt::registry& registry,
                                  entt::entity manifold_entity,
                                  contact_manifold& manifold,
                                  const collision_result::collision_point& rp);

/**
 * Removes a contact point from a manifold if it's separating.
 */
bool maybe_remove_point(contact_manifold &manifold, const contact_point &cp, size_t pt_idx,
                        const vector3 &posA, const quaternion &ornA,
                        const vector3 &posB, const quaternion &ornB);

/**
 * Destroys a contact point that has been removed from the manifold
 * using `maybe_remove_point`.
 */
void destroy_contact_point(entt::registry &registry, entt::entity manifold_entity,
                           entt::entity contact_entity);

using detect_collision_body_view_t = entt::basic_view<entt::entity, entt::exclude_t<>,
                                     AABB, shape_index, position, orientation>;

using com_view_t = entt::basic_view<entt::entity, entt::exclude_t<>, center_of_mass>;

/**
 * Detects collision between two bodies and adds closest points to the given
 * collision result
 */
void detect_collision(std::array<entt::entity, 2> body, collision_result &,
                      const detect_collision_body_view_t &, const com_view_t &,
                      const tuple_of_shape_views_t &);

/**
 * Processes a collision result and inserts/replaces points into the manifold.
 * It also removes points in the manifold that are separating. `new_point_func`
 * is called for each point that is created and `destroy_point_func` is called
 * for every point that is removed (remember to call `destroy_contact_point`
 * when appropriate for each point that is removed).
 */
template<typename ContactPointView, typename ImpulseView, typename TransformView,
         typename TireView, typename NewPointFunc, typename DestroyPointFunc>
void process_collision(entt::entity manifold_entity, contact_manifold &manifold,
                       const collision_result &result,
                       ContactPointView &cp_view,
                       ImpulseView &imp_view,
                       TransformView &tr_view,
                       const com_view_t &com_view,
                       TireView &tire_view,
                       NewPointFunc new_point_func,
                       DestroyPointFunc destroy_point_func) {
    auto [posA, ornA] = tr_view.template get<position, orientation>(manifold.body[0]);
    auto [posB, ornB] = tr_view.template get<position, orientation>(manifold.body[1]);

    tire_material *tire = nullptr;
    auto is_tireA = false;
    auto is_tireB = false;

    if (tire_view.contains(manifold.body[0])) {
        tire = &tire_view.get(manifold.body[0]);
        is_tireA = true;
    }

    if (tire_view.contains(manifold.body[1])) {
        tire = &tire_view.get(manifold.body[1]);
        is_tireB = true;
    }

    auto originA = static_cast<vector3>(posA);
    auto originB = static_cast<vector3>(posB);

    if (com_view.contains(manifold.body[0])) {
        auto &com = com_view.get(manifold.body[0]);
        originA = to_world_space(-com, posA, ornA);
    }

    if (com_view.contains(manifold.body[1])) {
        auto &com = com_view.get(manifold.body[1]);
        originB = to_world_space(-com, posB, ornB);
    }

    // Merge new with existing contact points.
    auto merged_indices = std::array<bool, max_contacts>{};
    std::fill(merged_indices.begin(), merged_indices.end(), false);

    for (auto i = manifold.num_points(); i > 0; --i) {
        auto pt_idx = i - 1;
        // Find a point in the result that's closest to the current point and
        // replace it. If there isn't any, check if the point is separating and
        // remove it if so. Increment lifetime if the point survives or gets
        // replaced by a matching result point.
        auto point_entity = manifold.point[pt_idx];
        auto &cp = cp_view.template get<contact_point>(point_entity);
        ++cp.lifetime;

        auto nearest_idx = SIZE_MAX;

        if (tire) {
            nearest_idx = find_nearest_contact_tire(cp, result);
        } else {
            nearest_idx = find_nearest_contact(cp, result);
        }

        if (nearest_idx < result.num_points && !merged_indices[nearest_idx]) {
            merge_point(result.point[nearest_idx], cp);
            merged_indices[nearest_idx] = true;
        } else if (maybe_remove_point(manifold, cp, pt_idx, originA, ornA, originB, ornB)) {
            destroy_point_func(point_entity);
        }
    }

    // Do not continue if all result points were merged.
    auto merged_indices_end = merged_indices.begin() + result.num_points;
    if (std::find(merged_indices.begin(), merged_indices_end, false) == merged_indices_end) {
        return;
    }

    // Use a local array of points to store the final combination of existing
    // with new contact points. Points can be inserted and replaced thus doing
    // this directly into the manifold would create some confusion.
    struct local_contact_point {
        collision_result::collision_point point;
        entt::entity entity {entt::null};
        point_insertion_type type {point_insertion_type::none};
    };

    // Start with current manifold points.
    auto local_points = std::array<local_contact_point, max_contacts>{};
    auto num_points = manifold.num_points();

    if (num_points > 0) {
        for (size_t i = 0; i < num_points; ++i) {
            auto &cp = cp_view.template get<contact_point>(manifold.point[i]);
            local_points[i].point = {cp.pivotA, cp.pivotB, cp.normal, cp.distance};
            local_points[i].entity = manifold.point[i];
        }
    } else {
        ++num_points;
        local_points[0].point = result.point[0];
        local_points[0].type = point_insertion_type::append;
        merged_indices[0] = true;
    }

    // Insert the remaining result points seeking to maximize the contact area.
    for (size_t pt_idx = 0; pt_idx < result.num_points; ++pt_idx) {
        if (merged_indices[pt_idx]) {
            continue;
        }

        auto &rp = result.point[pt_idx];

        // Look for a point to be replaced. Try pivotA first.
        // Note that `insertion_point_index` will increment `num_points`
        // for the caller if it determines this is a new point that must be
        // appended, i.e. `res.type` will be `point_insertion_type::append`.
        std::array<vector3, max_contacts> pivots;
        std::array<scalar, max_contacts> distances;

        for (size_t i = 0; i < num_points; ++i) {
            pivots[i] = local_points[i].point.pivotA;
            distances[i] = local_points[i].point.distance;
        }

        auto res = insertion_point_index(pivots, distances, num_points, rp.pivotA, rp.distance, is_tireA);

        // No closest point found for pivotA, try pivotB.
        if (res.type == point_insertion_type::none) {
            for (size_t i = 0; i < num_points; ++i) {
                pivots[i] = local_points[i].point.pivotB;
            }

            res = insertion_point_index(pivots, distances, num_points, rp.pivotB, rp.distance, is_tireB);
        }

        if (res.type != point_insertion_type::none) {
            local_points[res.index].point = rp;
            local_points[res.index].type = res.type;
        }
    }

    // Assign some points to manifold and replace others.
    for (size_t pt_idx = 0; pt_idx < num_points; ++pt_idx) {
        auto &local_pt = local_points[pt_idx];
        switch (local_pt.type) {
        case point_insertion_type::append:
            // Notify creation of a new point if it was inserted. It could have
            // been replaced after being inserted in the steps above, but in
            // either case, it's still a new point.
            new_point_func(local_pt.point);
            break;
        case point_insertion_type::similar:
            // It was replaced by a similar point, thus merge.
            // There is the possibility that this point actually replaced a similar
            // point which is new and is not in the manifold yet, i.e. the collision
            // result has two or more points that are very close to one another.
            // Create a new point in that case.
            if (local_pt.entity == entt::null) {
                new_point_func(local_pt.point);
            } else {
                merge_point(local_pt.point, cp_view.get(local_pt.entity));
            }
            break;
        case point_insertion_type::replace:
            // If it was replaced, the old point must be destroyed and a new
            // point must be created.
            // There is a chance the replaced point is actually one of the new
            // and thus there's no real point to be destroyed.
            if (local_pt.entity != entt::null) {
                auto num_manifold_points = manifold.num_points();

                for (size_t i = 0; i < num_manifold_points; ++i) {
                    if (manifold.point[i] == local_pt.entity) {
                        // Assign last to i-th and set last to null.
                        size_t last_idx = num_manifold_points - 1;
                        manifold.point[i] = manifold.point[last_idx];
                        manifold.point[last_idx] = entt::null;
                        break;
                    }
                }

                destroy_point_func(local_pt.entity);
            }
            new_point_func(local_pt.point);
            break;
        default:
            break;
        }
    }
}

}

#endif // EDYN_UTIL_COLLISION_UTIL_HPP
