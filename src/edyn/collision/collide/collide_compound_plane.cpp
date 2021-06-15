#include "edyn/collision/collide.hpp"
#include "edyn/util/aabb_util.hpp"

namespace edyn {

void collide(const compound_shape &shA, const plane_shape &shB,
             const collision_context &ctx, collision_result &result) {
    for (auto &node : shA.nodes) {
        const auto inset = vector3_one * -contact_breaking_threshold;
        auto aabbA = aabb_to_world_space(node.aabb, ctx.posA, ctx.ornA).inset(inset);

        if (!intersect(aabbA, ctx.aabbB)) {
            continue;
        }

        auto child_ctx = ctx;
        child_ctx.posA = to_world_space(node.position, ctx.posA, ctx.ornA);
        child_ctx.ornA *= node.orientation;
        collision_result child_result;

        std::visit([&] (auto &&sh) {
            collide(sh, shB, child_ctx, child_result);
        }, node.shape_var);

        for (size_t i = 0; i < child_result.num_points; ++i) {
            auto &child_point = child_result.point[i];
            child_point.pivotA = to_world_space(child_point.pivotA, node.position, node.orientation);
            result.maybe_add_point(child_point);
        }
    }
}

// Plane-Compound
void collide(const plane_shape &shA, const compound_shape &shB,
             const collision_context &ctx, collision_result &result) {
    swap_collide(shA, shB, ctx, result);
}

}
