#ifndef EDYN_UTIL_CONSTRAINT_UTIL_HPP
#define EDYN_UTIL_CONSTRAINT_UTIL_HPP

#include <entt/entity/registry.hpp>
#include "edyn/comp/graph_node.hpp"
#include "edyn/core/entity_pair.hpp"
#include "edyn/math/vector3.hpp"

namespace edyn {

struct contact_manifold;
struct constraint_row;
struct constraint_row_with_spin;
struct constraint_row_triple;
struct constraint_row_options;
struct matrix3x3;

namespace internal {
    bool pre_make_constraint(entt::registry &registry, entt::entity entity,
                             entt::entity body0, entt::entity body1);
}

/**
 * @brief Assigns a constraint component of type `T` to the given entity and does
 * all the other necessary steps to tie things together correctly.
 * @tparam T Constraint type.
 * @tparam SetupFunc Type of function to configure the constraint.
 * @param registry The `entt::registry`.
 * @param entity The constraint entity.
 * @param body0 First rigid body entity.
 * @param body1 Second rigid body entity.
 * @param setup Optional function to configure the constraint. Ensures the
 * assigned properties are propagated to the simulation worker when running
 * in asynchronous execution mode.
 */
template<typename T, typename... SetupFunc>
void make_constraint(entt::registry &registry, entt::entity entity,
                     entt::entity body0, entt::entity body1, SetupFunc... setup) {
    internal::pre_make_constraint(registry, entity, body0, body1);
    registry.emplace<T>(entity, body0, body1);
    (registry.patch<T>(entity, setup), ...);
}

/*! @copydoc make_constraint */
template<typename T, typename... SetupFunc>
auto make_constraint(entt::registry &registry,
                     entt::entity body0, entt::entity body1,
                     SetupFunc... setup) {
    auto entity = registry.create();
    make_constraint<T>(registry, entity, body0, body1, setup...);
    return entity;
}

/**
 * @brief Visit all edges of a node in the entity graph. This can be used to
 * iterate over all constraints assigned to a rigid body, including contacts.
 * @tparam Func Visitor function type.
 * @param entity Node entity.
 * @param func Visitor function with signature `void(entt::entity)` or
 * `bool(entt::entity)`. The latter can return false to abort the visit.
 */
template<typename Func>
void visit_edges(entt::registry &registry, entt::entity entity, Func func) {
    auto &node = registry.get<graph_node>(entity);
    auto &graph = registry.ctx().at<entity_graph>();
    graph.visit_edges(node.node_index, [&](auto edge_index) {
        if constexpr(std::is_invocable_r_v<bool, Func, entt::entity>) {
            return func(graph.edge_entity(edge_index));
        } else {
            func(graph.edge_entity(edge_index));
        }
    });
}

/**
 * @brief Visit all neighboring nodes of a node in the entity graph. This can
 * be used to iterate over all rigid bodies that are connected one body via
 * constraints.
 * @tparam Func Visitor function type.
 * @param entity Node entity.
 * @param func Visitor function with signature `void(entt::entity)` or
 * `bool(entt::entity)`. The latter can return false to abort the visit.
 */
template<typename Func>
void visit_neighbors(entt::registry &registry, entt::entity entity, Func func) {
    auto &node = registry.get<graph_node>(entity);
    auto &graph = registry.ctx().at<entity_graph>();
    graph.visit_neighbors(node.node_index, func);
}

entt::entity make_contact_manifold(entt::registry &registry,
                                   entt::entity body0, entt::entity body1,
                                   scalar separation_threshold);

void make_contact_manifold(entt::entity contact_entity, entt::registry &,
                           entt::entity body0, entt::entity body1,
                           scalar separation_threshold);

void swap_manifold(contact_manifold &manifold);

scalar get_effective_mass(const constraint_row &);

scalar get_effective_mass(const std::array<vector3, 4> &J,
                          scalar inv_mA, const matrix3x3 &inv_IA,
                          scalar inv_mB, const matrix3x3 &inv_IB);

scalar get_effective_mass(const std::array<vector3, 6> &J,
                          scalar inv_mA, const matrix3x3 &inv_IA,
                          scalar inv_mB, const matrix3x3 &inv_IB,
                          scalar inv_mC, const matrix3x3 &inv_IC);

scalar get_relative_speed(const std::array<vector3, 4> &J,
                          const vector3 &linvelA,
                          const vector3 &angvelA,
                          const vector3 &linvelB,
                          const vector3 &angvelB);

}

#endif // EDYN_UTIL_CONSTRAINT_UTIL_HPP
