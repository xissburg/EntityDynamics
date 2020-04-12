#ifndef EDYN_SHAPES_MESH_SHAPE_HPP
#define EDYN_SHAPES_MESH_SHAPE_HPP

#include <memory>

#include "edyn/comp/aabb.hpp"
#include "edyn/math/quaternion.hpp"
#include "triangle_mesh.hpp"

namespace edyn {

struct mesh_shape {
    std::shared_ptr<triangle_mesh> trimesh;

    AABB aabb(const vector3 &pos, const quaternion &orn) const {
        return {trimesh->aabb.min + pos, trimesh->aabb.max + pos};
    }

    vector3 inertia(scalar mass) const {
        return vector3_max;
    }
};

}

#endif // EDYN_SHAPES_MESH_SHAPE_HPP