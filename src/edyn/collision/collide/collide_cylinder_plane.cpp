#include "edyn/collision/collide.hpp"
#include "edyn/math/math.hpp"

namespace edyn {

void collide(const cylinder_shape &shA, const plane_shape &shB,
             const collision_context &ctx, collision_result &result) {
    const auto &posA = ctx.posA;
    const auto &ornA = ctx.ornA;

    auto normal = shB.normal;
    auto center = normal * shB.constant;
    auto projA = -shA.support_projection(posA, ornA, -normal);
    auto projB = shB.constant;
    auto distance = projA - projB;

    if (distance > ctx.threshold) {
        return;
    }

    cylinder_feature featureA;
    size_t feature_indexA;
    shA.support_feature(posA, ornA, -normal, featureA, feature_indexA,
                        support_feature_tolerance);

    switch (featureA) {
    case cylinder_feature::face:{
        auto multipliers = std::array<scalar, 4>{0, 1, 0, -1};
        auto pivotA_x = shA.half_length * to_sign(feature_indexA == 0);

        for(int i = 0; i < 4; ++i) {
            auto pivotA = vector3{pivotA_x,
                                  shA.radius * multipliers[i],
                                  shA.radius * multipliers[(i + 1) % 4]};
            auto pivotA_world = to_world_space(pivotA, posA, ornA);
            auto pivotB = project_plane(pivotA_world, center, normal);
            auto local_distance = dot(pivotA_world - pivotB, normal);
            result.maybe_add_point({pivotA, pivotB, normal, local_distance});
        }
        break;
    }
    case cylinder_feature::side_edge:
    case cylinder_feature::cap_edge: {
        auto cyl_axis = quaternion_x(ornA);
        auto cyl_vertices = std::array<vector3, 2>{};
        auto num_vertices = 0;

        if (featureA == cylinder_feature::cap_edge) {
            cyl_vertices[0] = posA + cyl_axis * shA.half_length * to_sign(feature_indexA == 0);
            num_vertices = 1;
        } else {
            cyl_vertices[0] = posA - cyl_axis * shA.half_length;
            cyl_vertices[1] = posA + cyl_axis * shA.half_length;
            num_vertices = 2;
        }

        // Negated normal projected onto plane of cylinder caps. Multiply it by
        // radius and add the cap center to get the exact pivot on A.
        auto dirA = normalize(project_direction(-normal, cyl_axis));

        for (auto i = 0; i < num_vertices; ++i) {
            auto &vertex = cyl_vertices[i];
            auto pivotA_world = vertex + dirA * shA.radius;
            auto pivotA = to_object_space(pivotA_world, posA, ornA);
            auto pivotB = project_plane(pivotA_world, center, normal);
            auto local_distance = dot(pivotA_world - pivotB, normal);
            result.maybe_add_point({pivotA, pivotB, normal, local_distance});
        }
        break;
    }
    }
}

void collide(const plane_shape &shA, const cylinder_shape &shB,
             const collision_context &ctx, collision_result &result) {
    swap_collide(shA, shB, ctx, result);
}

}
