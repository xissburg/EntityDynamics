#include "edyn/shapes/polyhedron_shape.hpp"
#include "edyn/util/shape_util.hpp"
#include "edyn/util/moment_of_inertia.hpp"

namespace edyn {

void polyhedron_shape::calculate_local_aabb() {
    local_aabb = point_cloud_aabb(mesh->vertices.begin(), mesh->vertices.end());
}

AABB polyhedron_shape::aabb(const vector3 &pos, const quaternion &orn) const {
    // For large meshes use the AABB of the transformed local AABB instead of
    // recalculating a tight fitting AABB every time.
    constexpr size_t convex_mesh_exact_aabb_vertex_count_limit = 24;

    if (mesh->vertices.size() < convex_mesh_exact_aabb_vertex_count_limit) {
        return point_cloud_aabb(mesh->vertices.begin(), mesh->vertices.end(), pos, orn);
    } else {
        return aabb_of_aabb(local_aabb, pos, orn);
    }
}

matrix3x3 polyhedron_shape::inertia(scalar mass) const {
    return moment_of_inertia_polyhedron(mass, mesh->vertices, mesh->triangles);
}

}