#ifndef EDYN_SHAPES_CONVEX_MESH_HPP
#define EDYN_SHAPES_CONVEX_MESH_HPP

#include <array>
#include <vector>
#include <cstdint>
#include "edyn/math/vector3.hpp"
#include "edyn/math/quaternion.hpp"
#include "edyn/config/config.h"

namespace edyn {

struct rotated_mesh;

/**
 * @brief Represents a convex polyhedron.
 */
struct convex_mesh {
    // Vertex positions.
    std::vector<vector3> vertices;

    // Vertex indices of all faces.
    std::vector<uint32_t> indices;

    // Each subsequent pair of integers represents the indices of the two
    // vertices of an edge in the `vertices` array.
    std::vector<uint32_t> edges;

    // Each subsequent pair of integers represents the index of the first
    // vertex of a face in the `indices` array and the number of vertices
    // in the face.
    std::vector<uint32_t> faces;

    // Face normals.
    std::vector<vector3> normals;

    // Data which is relevant in collision detection using SAT, i.e. unique
    // face normals and an index of a vertex on respective face and unique
    // edge directions.
    std::vector<uint32_t> relevant_indices;
    std::vector<vector3> relevant_normals;
    std::vector<vector3> relevant_edges;

    /**
     * @brief Initializes calculated properties. Call this after vertices,
     * indices and faces are assigned.
     * @note All vertices are shifted by the negated centroid location. This
     * makes all vertices to be positioned with respect to the centroid. This
     * is important because the moment of inertia of all shapes that can go
     * into a compound shape is calculated with respect to the center of mass
     * which is necessary for the parallel axis theorem to be applied when
     * calculating the moment of inertia of a compound shape.
     */
    void initialize();

    void update_calculated_properties();

    size_t num_edges() const {
        EDYN_ASSERT(edges.size() % 2 == 0);
        return edges.size() / 2;
    }

    size_t num_faces() const {
        EDYN_ASSERT(faces.size() % 2 == 0);
        return faces.size() / 2;
    }

    uint32_t face_vertex_index(size_t face_idx, size_t vertex_idx) const {
        auto face_index_idx = face_idx * 2;
        EDYN_ASSERT(face_index_idx < faces.size());
        auto index_idx = faces[face_index_idx];
        EDYN_ASSERT(index_idx < indices.size());
        EDYN_ASSERT(vertex_idx < faces[face_index_idx + 1]);
        return indices[index_idx + vertex_idx];
    }

    /**
     * @brief Returns the index of the first vertex of a face.
     * @param face_idx Face index.
     * @return Vertex index of the first vertex in the face.
     */
    uint32_t first_vertex_index(size_t face_idx) const {
        return face_vertex_index(face_idx, 0);
    }

    /**
     * @brief Returns the number of vertices on a face.
     * @param face_idx Face index.
     * @return Number of vertices on the face.
     */
    uint32_t vertex_count(size_t face_idx) const {
        auto face_count_idx = face_idx * 2 + 1;
        EDYN_ASSERT(face_count_idx < faces.size());
        return faces[face_count_idx];
    }

    /**
     * @brief Returns the two vertices of an edge.
     * @param idx Edge index.
     * @return The coordinates of the two vertices.
     */
    std::array<vector3, 2> get_edge(size_t idx) const;

    /**
     * @brief Returns the two vertices of a rotated edge from the rotated
     * mesh that corresponds to this shape.
     * @param rmesh The rotated mesh associated with this convex shape.
     * @param idx Edge index.
     * @return The coordinates of the two rotated vertices.
     */
    std::array<vector3, 2> get_rotated_edge(const rotated_mesh &, size_t idx) const;

    void shift_to_centroid();
    void calculate_normals();
    void calculate_edges();
    void calculate_relevant_normals();
    void calculate_relevant_edges();

#ifdef EDYN_DEBUG
    void validate() const;
#endif
};

/**
 * @brief Accompanying component for `convex_mesh`es containg their
 * rotated vertices, normals and edges to prevent repeated recalculation of
 * these values.
 */
struct rotated_mesh {
    std::vector<vector3> vertices;
    std::vector<vector3> relevant_normals;
    std::vector<vector3> relevant_edges;
};

/**
 * @brief Creates a `rotated_mesh` from a `convex_mesh` with the given orientation.
 * @param mesh The source convex mesh.
 * @param orn Orientation to apply to all vertices and normals.
 * @return A `rotated_mesh` with the rotated vertices and normals of `mesh`.
 */
rotated_mesh make_rotated_mesh(const convex_mesh &mesh, const quaternion &orn = quaternion_identity);

}

#endif // EDYN_SHAPES_CONVEX_MESH_HPP
