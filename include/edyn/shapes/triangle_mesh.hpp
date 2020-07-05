#ifndef EDYN_SHAPES_TRIANGLE_MESH_HPP
#define EDYN_SHAPES_TRIANGLE_MESH_HPP

#include <cstdint>
#include <vector>
#include "edyn/math/vector3.hpp"
#include "edyn/math/geom.hpp"
#include "edyn/comp/aabb.hpp"
#include "edyn/shapes/triangle_shape.hpp"
#include "edyn/collision/static_tree.hpp"

namespace edyn {

struct triangle_mesh {
    std::vector<vector3> vertices;
    std::vector<uint16_t> indices;
    std::vector<scalar> cos_angles;
    std::vector<bool> is_concave_edge;
    static_tree tree;

    size_t num_triangles() const {
        EDYN_ASSERT(indices.size() % 3 == 0);
        return indices.size() / 3;
    }

    const AABB &get_aabb() const {
        EDYN_ASSERT(!tree.m_nodes.empty());
        return tree.m_nodes.front().aabb;
    }

    template<typename Func>
    void visit(const AABB &aabb, Func func) const {
        constexpr auto inset = vector3 {
            -contact_breaking_threshold, 
            -contact_breaking_threshold, 
            -contact_breaking_threshold
        };
        
        tree.visit(aabb.inset(inset), [&] (auto tri_idx) {
            auto verts = triangle_vertices{
                vertices[indices[tri_idx * 3 + 0]],
                vertices[indices[tri_idx * 3 + 1]],
                vertices[indices[tri_idx * 3 + 2]]
            };

            func(tri_idx, verts);
        });
    }

    template<typename Func>
    void visit_all(Func func) const {
        for (size_t i = 0; i < num_triangles(); ++i) {
            auto verts = triangle_vertices{
                vertices[indices[i * 3 + 0]],
                vertices[indices[i * 3 + 1]],
                vertices[indices[i * 3 + 2]]
            };

            func(i, verts);
        }
    }

    void initialize();
    void initialize_edge_angles();
    void calculate_edge_angles();
    void build_tree();
};

}

#endif // EDYN_SHAPES_TRIANGLE_MESH_HPP