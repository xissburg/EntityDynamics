#ifndef EDYN_COLLISION_BROADPHASE_WORKER_HPP
#define EDYN_COLLISION_BROADPHASE_WORKER_HPP

#include <vector>
#include <entt/fwd.hpp>
#include "edyn/comp/aabb.hpp"
#include "edyn/collision/dynamic_tree.hpp"
#include "edyn/collision/contact_manifold_map.hpp"

namespace edyn {

class broadphase_worker {
    // Offset applied to AABBs when querying the trees.
    constexpr static auto m_aabb_offset = vector3_one * -contact_breaking_threshold;

    // Separation threshold for new manifolds.
    constexpr static auto m_separation_threshold = contact_breaking_threshold * scalar{4 * 1.3};

    void init_new_aabb_entities();
    bool should_collide(entt::entity, entt::entity) const;
    void collide_tree(const dynamic_tree &tree, entt::entity entity, const AABB &offset_aabb);

public:

    broadphase_worker(entt::registry &);
    void update();

    /**
     * @brief Returns a view of the procedural dynamic tree.
     * @return Tree view of the procedural dynamic tree.
     */
    tree_view view() const;

    void on_construct_aabb(entt::registry &, entt::entity);
    void on_destroy_node_id(entt::registry &, entt::entity);

private:
    entt::registry *m_registry;
    dynamic_tree m_tree; // Procedural dynamic tree.
    dynamic_tree m_np_tree; // Non-procedural dynamic tree.
    contact_manifold_map m_manifold_map;
    std::vector<entt::entity> m_new_aabb_entities;
};

}

#endif // EDYN_COLLISION_BROADPHASE_WORKER_HPP