#include "edyn/collision/collide.hpp"

namespace edyn {

collision_result collide(const sphere_shape &shA, const vector3 &posA, const quaternion &ornA,
                         const paged_mesh_shape &shB, const vector3 &posB, const quaternion &ornB,
                         scalar threshold) {
    auto result = collision_result{};

    // Sphere position in mesh's space.
    auto posA_in_B = rotate(conjugate(ornB), posA - posB);
    auto ornA_in_B = conjugate(ornB) * ornA;

    auto aabb = shA.aabb(posA_in_B, ornA); // Invariant to orientation.
    shB.trimesh->visit(aabb, [&] (size_t mesh_idx, size_t tri_idx, const triangle_vertices &vertices) {
        if (result.num_points == max_contacts) {
            return;
        }

        std::array<bool, 3> is_concave_edge;
        std::array<scalar, 3> cos_angles;
        auto *trimesh = shB.trimesh->get_submesh(mesh_idx);

        for (int i = 0; i < 3; ++i) {
            is_concave_edge[i] = trimesh->is_concave_edge[tri_idx * 3 + i];
            cos_angles[i] = trimesh->cos_angles[tri_idx * 3 + i];
        }

        collide_sphere_triangle(shA, posA_in_B, ornA_in_B, vertices, 
                                is_concave_edge, cos_angles, threshold, result);
    });

    return result;
}

}