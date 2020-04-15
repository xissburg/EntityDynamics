#include "edyn/collision/collide.hpp"
#include "edyn/math/math.hpp"
#include <algorithm>

namespace edyn {

enum cylinder_feature {
    // Either of the two cylinder caps.
    CYLINDER_FEATURE_FACE,
    // An edge on the side wall of the cylinder.
    CYLINDER_FEATURE_SIDE_EDGE,
    // The edge/border of a cylinder cap.
    CYLINDER_FEATURE_FACE_EDGE,
};

enum triangle_feature {
    TRIANGLE_FEATURE_VERTEX,
    TRIANGLE_FEATURE_EDGE,
    TRIANGLE_FEATURE_FACE
};

struct separating_axis {
    vector3 pos;
    vector3 dir;
    scalar distance;
    cylinder_feature cyl_feature;
    triangle_feature tri_feature;
    uint16_t cyl_feature_index; // 0: positive face, 1: negative face.
    uint16_t tri_feature_index; // Vertex index or edge index.
    vector3 pivotA;
    vector3 pivotB;
};

/**
 * Gets the minimum and maximum projections of the triangle onto the given axis
 * along with the features present at both extremes.
 */
static
void get_triangle_min_max_features(const triangle_vertices &vertices, 
                                   const vector3 &axis_pos, const vector3 &axis_dir,
                                   scalar &proj_min, scalar &proj_max,
                                   triangle_feature &min_tri_feature, triangle_feature &max_tri_feature,
                                   uint8_t &min_tri_feature_index, uint8_t &max_tri_feature_index) {
    proj_min = large_scalar;
    proj_max = -large_scalar;

    for (uint16_t i = 0; i < 3; ++i) {
        auto &v = vertices[i];
        auto proj = dot(v - axis_pos, axis_dir);

        // If the projection is near the current maximum, it means 
        // there's another vertex already at that spot, thus the 
        // feature could be either an edge or the face.
        if (std::abs(proj - proj_max) < EDYN_EPSILON) {
            // If the maximum feature is a vertex, then the current vertex
            // is included to form an edge.
            if (max_tri_feature == TRIANGLE_FEATURE_VERTEX) {
                max_tri_feature = TRIANGLE_FEATURE_EDGE;
                if (i == 2) {
                    // If this is the third vertex (index 2), the previous in this 
                    // for loop could have been either vertex 1 or 0. If 0, then the 
                    // edge is the last one, i.e. edge 2. If 1, then the edge is #1.
                    if (max_tri_feature_index == 0) {
                        max_tri_feature_index = 2;
                    } else {
                        max_tri_feature_index = 1;
                    }
                } else {
                    // If this is the second vertex (index 1), the previous could
                    // only have been vertex 0, thus this must be edge 0.
                    max_tri_feature_index = 0;
                }
            } else if (max_tri_feature == TRIANGLE_FEATURE_EDGE) {
                // If the maximum feature was already an edge, adding this
                // vertex to it makes it a face.
                max_tri_feature = TRIANGLE_FEATURE_FACE;
            }
        } else if (proj > proj_max) {
            proj_max = proj;
            max_tri_feature = TRIANGLE_FEATURE_VERTEX;
            max_tri_feature_index = i;
        }
        
        // Same as above though for the negative/minimum direction.
        if (std::abs(proj - proj_min) < EDYN_EPSILON) {
            if (min_tri_feature == TRIANGLE_FEATURE_VERTEX) {
                min_tri_feature = TRIANGLE_FEATURE_EDGE;
                if (i == 2) {
                    if (min_tri_feature_index == 0) {
                        min_tri_feature_index = 2;
                    } else {
                        min_tri_feature_index = 1;
                    }
                } else {
                    min_tri_feature_index = 0;
                }
            } else if (max_tri_feature == TRIANGLE_FEATURE_EDGE) {
                min_tri_feature = TRIANGLE_FEATURE_FACE;
            }
        } else if (proj < proj_min) {
            proj_min = proj;
            min_tri_feature = TRIANGLE_FEATURE_VERTEX;
            min_tri_feature_index = i;
        }
    }
}

collision_result collide(const cylinder_shape &shA, const vector3 &posA, const quaternion &ornA,
                         const mesh_shape &shB, const vector3 &posB, const quaternion &ornB,
                         scalar threshold) {
    auto result = collision_result{};

    // Cylinder position and orientation in mesh's space.
    auto posA_in_B = rotate(conjugate(ornB), posA - posB);
    auto ornA_in_B = conjugate(ornB) * ornA;

    const auto cyl_axis = quaternion_x(ornA_in_B);
    const auto disc_center_pos = posA_in_B + cyl_axis * shA.half_length;
    const auto disc_center_neg = posA_in_B - cyl_axis * shA.half_length;

    auto aabb = shA.aabb(posA_in_B, ornA_in_B);
    shB.trimesh->visit(aabb, [&] (size_t tri_idx, const triangle_vertices &vertices) {
        if (result.num_points == max_contacts) {
            return;
        }

        // Separating-axis test. Find axis with greatest distance between intervals.
        std::vector<separating_axis> sep_axes;

        const auto edges = get_triangle_edges(vertices);
        const auto tri_normal = normalize(cross(edges[0], edges[1]));

        // Cylinder cap normal. Test both directions of the cylinder axis to
        // cover both caps.
        {
            auto axis = separating_axis{};
            axis.dir = cyl_axis;
            axis.pos = posA_in_B;
            axis.cyl_feature = CYLINDER_FEATURE_FACE;
            
            // Projection along separating axis.
            auto minA = -shA.half_length;
            auto maxA = shA.half_length;
            // For the triangle they'll be taken as the min/max projection of 
            // all vertices.
            scalar minB, maxB;
            // The triangle feature at each end of the projection.
            triangle_feature min_tri_feature, max_tri_feature;
            uint8_t min_tri_feature_index, max_tri_feature_index;

            get_triangle_min_max_features(vertices,
                                          axis.pos, axis.dir, 
                                          minB, maxB, 
                                          min_tri_feature, max_tri_feature, 
                                          min_tri_feature_index, max_tri_feature_index);

            if (minB > maxA) {
                // B is after A (i.e. wrt `axis.dir`).
                axis.distance = minB - maxA;
                // Positive cylinder face.
                axis.cyl_feature_index = 0;
                // Triangle features at the opposite direction of the separating axis.
                axis.tri_feature = min_tri_feature;
                axis.tri_feature_index = min_tri_feature_index;
                // Make direction point from B to A so it can be correctly used as contact normal.
                axis.dir *= -1;
            } else if (maxB < minA) {
                // B is before A.
                axis.distance = minA - maxB;
                axis.cyl_feature_index = 1;
                axis.tri_feature = max_tri_feature;
                axis.tri_feature_index = max_tri_feature_index;
            } else {
                // Ranges intersect.
                if (minB < minA) {
                    // Left side of B is before left side of A.
                    if (maxB > maxA) {
                        // A's projection is contained in B's.
                        if (maxA - minB < maxB - minA) {
                            axis.cyl_feature_index = 0;
                            axis.tri_feature = min_tri_feature;
                            axis.tri_feature_index = min_tri_feature_index;
                            axis.distance = minB - maxA;
                            axis.dir *= -1;
                        } else {
                            axis.cyl_feature_index = 1;
                            axis.tri_feature = max_tri_feature;
                            axis.tri_feature_index = max_tri_feature_index;
                            axis.distance = minA - maxB;
                        }
                    } else {
                        // Right side of B is before right side of A.
                        axis.distance = minA - maxB;
                        axis.cyl_feature_index = 1;
                        axis.tri_feature = max_tri_feature;
                        axis.tri_feature_index = max_tri_feature_index;
                    }
                } else {
                    if (maxB < maxA) {
                        // B's projection is contained in A's.
                        if (maxA - minB < maxB - minA) {
                            axis.cyl_feature_index = 0;
                            axis.tri_feature = min_tri_feature;
                            axis.tri_feature_index = min_tri_feature_index;
                            axis.distance = minB - maxA;
                            axis.dir *= -1;
                        } else {
                            axis.cyl_feature_index = 1;
                            axis.tri_feature = max_tri_feature;
                            axis.tri_feature_index = max_tri_feature_index;
                            axis.distance = minA - maxB;
                        }
                    } else {
                        axis.distance = minB - maxA;
                        axis.cyl_feature_index = 0;
                        axis.tri_feature = min_tri_feature;
                        axis.tri_feature_index = min_tri_feature_index;
                    }
                }
            }

            sep_axes.push_back(axis);
        }

        // Triangle face normal.
        {
            auto axis = separating_axis{};
            axis.tri_feature = TRIANGLE_FEATURE_FACE;
            axis.dir = tri_normal;
            axis.pos = vertices[0];

            auto axis_dot = dot(tri_normal, cyl_axis);

            if (std::abs(axis_dot) + EDYN_EPSILON >= 1) {
                // Cylinder is orthogonal to triangle face.
                axis.cyl_feature = CYLINDER_FEATURE_FACE;
                axis.cyl_feature_index = axis_dot > 0 ? 1 : 0;
                auto disc_center = axis_dot > 0 ? disc_center_neg : disc_center_pos;
                axis.distance = dot(tri_normal, disc_center - vertices[0]);
            } else if (std::abs(axis_dot) <= EDYN_EPSILON) {
                // Cylinder side is parallel to triangle face.
                axis.cyl_feature = CYLINDER_FEATURE_SIDE_EDGE;
                axis.distance = dot(tri_normal, disc_center_pos - vertices[0]) - shA.radius;
            } else {
                // Get support points for both discs in the opposite direction
                // of triangle normal.
                auto sup_pos = support_point_circle(shA.radius, disc_center_pos, ornA_in_B, -tri_normal);
                auto sup_neg = support_point_circle(shA.radius, disc_center_neg, ornA_in_B, -tri_normal);
                auto proj_pos = dot(sup_pos - axis.pos, axis.dir);
                auto proj_neg = dot(sup_neg - axis.pos, axis.dir);
                axis.cyl_feature = CYLINDER_FEATURE_FACE_EDGE;

                // Select deepest disc.
                if (proj_pos < proj_neg) {
                    axis.cyl_feature_index = 0;
                    axis.distance = proj_pos;
                    axis.pivotA = sup_pos;
                } else {
                    axis.cyl_feature_index = 1;
                    axis.distance = proj_neg;
                    axis.pivotA = sup_neg;
                }
            }

            sep_axes.push_back(axis);
        }

        // Cylinder side wall edges.
        {
            // Cylinder side wall against triangle vertices.
            for (uint8_t i = 0; i < 3; ++i) {
                auto axis = separating_axis{};
                axis.cyl_feature = CYLINDER_FEATURE_SIDE_EDGE;
                axis.tri_feature = TRIANGLE_FEATURE_VERTEX;
                axis.tri_feature_index = i;
                axis.pos = vertices[i];

                // Find closest point in cylinder segment to this vertex and use the
                // axis connecting them as the separating axis.
                scalar t;
                vector3 closest;
                auto dist_sqr = closest_point_segment(disc_center_pos, disc_center_neg, 
                                                      vertices[i], t, closest);

                // Ignore points at the extremes.
                if (t > 0 && t < 1) {
                    auto dist = std::sqrt(dist_sqr);
                    axis.dir = (closest - vertices[i]) / dist;

                    // It has to be the vertex closest to the cylinder along this axis.
                    // Note that `dist = dot(-axis.dir, vertices[i] - disc_center_pos)`.
                    auto proj0 = dot(-axis.dir, vertices[(i + 1) % 3] - disc_center_pos);
                    auto proj1 = dot(-axis.dir, vertices[(i + 2) % 3] - disc_center_pos);

                    if (dist < proj0 && dist < proj1) {
                        axis.distance = dist - shA.radius;
                        axis.pivotA = closest - axis.dir * shA.radius;
                        sep_axes.push_back(axis);
                    }
                }
            }                        

            // Cylinder side wall against triangle edges.
            for (uint8_t i = 0; i < 3; ++i) {
                auto axis = separating_axis{};
                axis.cyl_feature = CYLINDER_FEATURE_SIDE_EDGE;
                axis.tri_feature = TRIANGLE_FEATURE_EDGE;
                axis.tri_feature_index = i;
                axis.pos = vertices[i];

                axis.dir = cross(edges[i], cyl_axis);

                if (length2(axis.dir) <= EDYN_EPSILON) {
                    // Parallel. Find a vector that's orthogonal to both
                    // which lies in the same plane.
                    auto plane_normal = cross(edges[i], disc_center_pos - vertices[i]);
                    axis.dir = cross(plane_normal, edges[i]);
                }

                if (dot(posA_in_B - axis.pos, axis.dir) < 0) {
                    axis.dir *= -1;
                }

                axis.dir = normalize(axis.dir);

                // The vertex with greatest projection along axis has to be one
                // of the vertices in the current edge, because otherwise there
                // is anpther axis with greater projection distance.
                auto max_proj = -EDYN_SCALAR_MAX;
                uint8_t max_vertex_idx;

                for (uint8_t j = 0; j < 3; ++j) {
                    auto proj = dot(axis.dir, vertices[j] - axis.pos);

                    if (proj > max_proj) {
                        max_proj = proj;
                        max_vertex_idx = j;
                    }
                }

                if (max_vertex_idx == i || max_vertex_idx == (i + 1) % 3) {
                    auto sup = shA.support_point(posA_in_B, ornA_in_B, -axis.dir);
                    axis.distance = -(dot(-axis.dir, sup - axis.pos) + max_proj);
                    sep_axes.push_back(axis);
                }
            }
        }

        // Cylinder face edges.
        {
            for (uint8_t i = 0; i < 2; ++i) {
                auto disc_center = i == 0 ? disc_center_pos : disc_center_neg;

                for (uint8_t j = 0; j < 3; ++j) {
                    auto &v0 = vertices[j];
                    auto &v1 = vertices[(j + 1) % 3];
                    size_t num_points;
                    scalar s0, s1;
                    vector3 cc0, cl0, cc1, cl1;
                    vector3 normal;
                    closest_point_disc_line(disc_center, ornA_in_B, shA.radius, v0, v1, 
                                            num_points, s0, cc0, cl0, s1, cc1, cl1, normal, threshold);
                    
                    if (num_points > 0) {
                        auto axis = separating_axis{};
                        axis.cyl_feature = CYLINDER_FEATURE_FACE_EDGE;
                        axis.cyl_feature_index = i;
                        axis.tri_feature = TRIANGLE_FEATURE_EDGE;
                        axis.tri_feature_index = j;
                        axis.dir = normalize(cc0 - cl0);
                        axis.pos = cl0;
                        axis.pivotA = cc0;
                        axis.pivotB = cl0;

                        auto sup_pos = support_point_circle(shA.radius, disc_center_pos, ornA_in_B, axis.dir);
                        auto sup_neg = support_point_circle(shA.radius, disc_center_neg, ornA_in_B, axis.dir);
                        auto proj_pos = dot(sup_pos - axis.pos, axis.dir);
                        auto proj_neg = dot(sup_neg - axis.pos, axis.dir);

                        if (std::abs(proj_pos) > std::abs(proj_neg)) {
                            axis.distance = proj_neg;

                            if (dot(cyl_axis, axis.dir) < 0) {
                                axis.dir *= -1;
                                axis.distance *= -1;
                            }
                        } else {
                            axis.distance = proj_pos;

                            if (dot(cyl_axis, axis.dir) > 0) {
                                axis.dir *= -1;
                                axis.distance *= -1;
                            }
                        }

                        sep_axes.push_back(axis);
                    }
                }
            }
        }

        // Get axis with greatest penetration.
        auto penetration = -EDYN_SCALAR_MAX;
        uint16_t sep_axis_idx = UINT16_MAX;

        for (size_t i = 0; i < sep_axes.size(); ++i) {
            auto &axis = sep_axes[i];

            if (axis.distance > penetration) {
                sep_axis_idx = i;
                penetration = axis.distance;
            }
        }

        if (penetration < threshold && sep_axis_idx != UINT16_MAX) {
            auto &axis = sep_axes[sep_axis_idx];

            switch (axis.cyl_feature) {
            case CYLINDER_FEATURE_FACE:
                switch (axis.tri_feature) {
                case TRIANGLE_FEATURE_VERTEX: {
                    auto vertex = vertices[axis.tri_feature_index];
                    auto vertex_world = posB + rotate(ornB, vertex);
                    auto vertex_in_A = rotate(conjugate(ornA), vertex_world - posA);
                    auto vertex_proj_in_A = vertex_in_A;
                    vertex_proj_in_A.x = shA.half_length * (axis.tri_feature_index == 0 ? 1 : -1);

                    auto idx = result.num_points++;
                    result.point[idx].pivotA = vertex_proj_in_A;
                    result.point[idx].pivotB = vertex;
                    result.point[idx].normalB = axis.dir;
                    result.point[idx].distance = penetration;
                    break;
                }

                case TRIANGLE_FEATURE_EDGE: {
                    auto v0 = vertices[axis.tri_feature_index];
                    auto v0_world = posB + rotate(ornB, v0);
                    auto v0_A = rotate(conjugate(ornA), v0_world - posA);

                    auto v1 = vertices[(axis.tri_feature_index + 1) % 3];
                    auto v1_world = posB + rotate(ornB, v1);
                    auto v1_A = rotate(conjugate(ornA), v1_world - posA);

                    scalar s0, s1;
                    auto num_points = intersect_line_circle(v0_A.z, v0_A.y, 
                                                            v1_A.z, v1_A.y, 
                                                            shA.radius, s0, s1);

                    EDYN_ASSERT(num_points > 0);

                    s0 = clamp_unit(s0);
                    auto p0 = lerp(v0_A, v1_A, s0);
                    auto pivotA_x = shA.half_length * (axis.cyl_feature_index == 0 ? 1 : -1);
                    auto p0_world = posA + rotate(ornA, p0);
                    auto p0_B = rotate(conjugate(ornB), p0_world - posB);

                    auto idx = result.num_points++;
                    result.point[idx].pivotA = {pivotA_x, p0.y, p0.z};
                    result.point[idx].pivotB = p0_B;
                    result.point[idx].normalB = axis.dir;
                    result.point[idx].distance = penetration;

                    if (num_points == 2) {
                        s1 = clamp_unit(s1);
                        auto p1 = lerp(v0_A, v1_A, s1);
                        auto p1_world = posA + rotate(ornA, p1);
                        auto p1_B = rotate(conjugate(ornB), p1_world - posB);

                        auto idx = result.num_points++;
                        result.point[idx].pivotA = {pivotA_x, p1.y, p1.z};
                        result.point[idx].pivotB = p1_B;
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = penetration;
                    }
                    break;
                }

                case TRIANGLE_FEATURE_FACE: {
                    uint8_t num_edge_intersections = 0;
                    uint8_t last_edge_index;

                    for(uint8_t i = 0; i < 3; ++i) {
                        auto v0 = vertices[i];
                        auto v0_world = posB + rotate(ornB, v0);
                        auto v0_A = rotate(conjugate(ornA), v0_world - posA);

                        auto v1 = vertices[(i + 1) % 3];
                        auto v1_world = posB + rotate(ornB, v1);
                        auto v1_A = rotate(conjugate(ornA), v1_world - posA);

                        scalar s0, s1;
                        auto num_points = intersect_line_circle(v0_A.z, v0_A.y, 
                                                                v1_A.z, v1_A.y, 
                                                                shA.radius, s0, s1);

                        if (num_points == 0) {
                            continue;
                        }

                        ++num_edge_intersections;
                        last_edge_index = i;

                        s0 = clamp_unit(s0);
                        auto p0 = lerp(v0_A, v1_A, s0);
                        auto pivotA_x = shA.half_length * (axis.cyl_feature_index == 0 ? 1 : -1);
                        auto p0_world = posA + rotate(ornA, p0);
                        auto p0_B = rotate(conjugate(ornB), p0_world - posB);

                        auto idx = result.num_points++;
                        result.point[idx].pivotA = {pivotA_x, p0.y, p0.z};
                        result.point[idx].pivotB = p0_B;
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = penetration;

                        if (num_points == 2) {
                            s1 = clamp_unit(s1);
                            auto p1 = lerp(v0_A, v1_A, s1);
                            auto p1_world = posA + rotate(ornA, p1);
                            auto p1_B = rotate(conjugate(ornB), p1_world - posB);

                            auto idx = result.num_points++;
                            result.point[idx].pivotA = {pivotA_x, p1.y, p1.z};
                            result.point[idx].pivotB = p1_B;
                            result.point[idx].normalB = axis.dir;
                            result.point[idx].distance = penetration;
                        }
                    }

                    if (num_edge_intersections == 0) {
                        // Either circle is contained in triangle or triangle is contained in circle.
                        // If the circle doesn't intersect the edges of the triangle and the distance
                        // between one vertex of the triangle and the center of the circle is smaller
                        // than the radius, it means the triangle is contained within the circle.
                        scalar t;
                        vector3 r;

                        if (closest_point_line(posA_in_B, axis.dir, vertices[0], t, r) < shA.radius * shA.radius) {
                            result.num_points = 3;
                            auto pivotA_x = shA.half_length * (axis.cyl_feature_index == 0 ? 1 : -1);

                            for(uint8_t i = 0; i < 3; ++i) {
                                auto vertex_B = vertices[i];
                                auto vertex_world = posB + rotate(ornB, vertex_B);
                                auto vertex_A = rotate(conjugate(ornA), vertex_world - posA);
                                vertex_A.x = pivotA_x;

                                result.point[i].pivotA = vertex_A;
                                result.point[i].pivotB = vertex_B;
                                result.point[i].normalB = axis.dir;
                                result.point[i].distance = penetration;
                            }
                        } else if (point_in_triangle(vertices, tri_normal, posA_in_B)) {
                            // If the circle doesn't intersect the edges of the triangle and its center
                            // is contained within the triangle, it means the entire circle is contained
                            // within the triangle.
                            result.num_points = 4;
                            auto multipliers = std::array<scalar, 4>{0, 1, 0, -1};
                            for(uint8_t i = 0; i < 4; ++i) {
                                auto pivotA_x = shA.half_length * (axis.cyl_feature_index == 0 ? 1 : -1);
                                auto pivotA = vector3{pivotA_x, 
                                                      shA.radius * multipliers[i], 
                                                      shA.radius * multipliers[(i + 1) % 4]};
                                auto pivotA_world = posA + rotate(ornA, pivotA);
                                auto pivotB = rotate(conjugate(ornB), pivotA_world - posB);
                                result.point[i].pivotA = pivotA;
                                result.point[i].pivotB = pivotB;
                                result.point[i].normalB = axis.dir;
                                result.point[i].distance = penetration;
                            }
                        }
                    } else if (num_edge_intersections == 1) {
                        // Add extra points to create a stable base.
                        auto tangent = cross(tri_normal, edges[last_edge_index]);
                        auto other_vertex_idx = (last_edge_index + 2) % 3;
                        if (dot(tangent, vertices[other_vertex_idx] - vertices[last_edge_index]) < 0) {
                            tangent *= -1;
                        }

                        tangent = normalize(tangent);
                        auto disc_center = axis.cyl_feature_index == 0 ? disc_center_pos : disc_center_neg;
                        auto pivotA_in_B = disc_center + tangent * shA.radius;
                        auto pivotA_world = posB + rotate(ornB, pivotA_in_B);
                        auto pivotA = rotate(conjugate(ornA), pivotA_world - posA);
                        auto pivotB = project_plane(pivotA_in_B, vertices[0], tri_normal);
                        auto idx = result.num_points++;
                        result.point[idx].pivotA = pivotA;
                        result.point[idx].pivotB = pivotB;
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = penetration;
                    }
                    break;
                }
                }
            break;

            case CYLINDER_FEATURE_SIDE_EDGE:
                switch (axis.tri_feature) {
                case TRIANGLE_FEATURE_FACE: {
                    // Cylinder is on its side laying on the triangle face.
                    // Segment-triangle intersection/containment test.
                    auto c0_in_tri = point_in_triangle(vertices, tri_normal, disc_center_pos);
                    auto c1_in_tri = point_in_triangle(vertices, tri_normal, disc_center_neg);

                    if (c0_in_tri) {
                        auto idx = result.num_points++;
                        auto p0 = disc_center_pos - tri_normal * shA.radius;
                        auto p0_world = posB + rotate(ornB, p0);
                        auto p0_A = rotate(conjugate(ornA), p0_world - posA);
                        result.point[idx].pivotA = p0_A;
                        result.point[idx].pivotB = project_plane(disc_center_pos, vertices[0], tri_normal);
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = penetration;
                    }

                    if (c1_in_tri) {
                        auto idx = result.num_points++;
                        auto p1 = disc_center_neg - tri_normal * shA.radius;
                        auto p1_world = posB + rotate(ornB, p1);
                        auto p1_A = rotate(conjugate(ornA), p1_world - posA);
                        result.point[idx].pivotA = p1_A;
                        result.point[idx].pivotB = project_plane(disc_center_neg, vertices[0], tri_normal);
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = penetration;
                    }

                    if (!c0_in_tri || !c1_in_tri) {
                        // One of them is outside. Perform segment intersection test.
                        for (uint8_t i = 0; i < 3; ++i) {
                            scalar s[2], t[2];
                            vector3 p0[2], p1[2];
                            size_t num_points = 0;
                            closest_point_segment_segment(disc_center_pos, disc_center_neg, 
                                                          vertices[i], vertices[(i + 1) % 3],
                                                          s[0], t[0], p0[0], p1[0], &num_points, 
                                                          &s[1], &t[1], &p0[1], &p1[1]);

                            for (uint8_t i = 0; i < num_points; ++i) {
                                if (s[i] > 0 && s[i] < 1 && t[i] > 0 && t[i] < 1) {
                                    auto idx = result.num_points++;
                                    auto pA_in_B = p0[i] - tri_normal * shA.radius;
                                    auto pA_world = posB + rotate(ornB, pA_in_B);
                                    auto pA = rotate(conjugate(ornA), pA_world - posA);
                                    result.point[idx].pivotA = pA;
                                    result.point[idx].pivotB = p1[i];
                                    result.point[idx].normalB = axis.dir;
                                    result.point[idx].distance = penetration;
                                }
                            }
                        }
                    }
                    
                    break;
                }
                case TRIANGLE_FEATURE_EDGE: {
                    auto v0 = vertices[axis.tri_feature_index];
                    auto v1 = vertices[(axis.tri_feature_index + 1) % 3];
                    scalar s[2], t[2];
                    vector3 p0[2], p1[2];
                    size_t num_points = 0;
                    closest_point_segment_segment(disc_center_pos, disc_center_neg, v0, v1,
                                                  s[0], t[0], p0[0], p1[0], &num_points, 
                                                  &s[1], &t[1], &p0[1], &p1[1]);

                    for (uint8_t i = 0; i < num_points; ++i) {
                        if (s[i] > 0 && s[i] < 1 && t[i] > 0 && t[i] < 1) {
                            auto idx = result.num_points++;
                            auto pA_in_B = p0[i] - axis.dir * shA.radius;
                            auto pA_world = posB + rotate(ornB, pA_in_B);
                            auto pA = rotate(conjugate(ornA), pA_world - posA);
                            result.point[idx].pivotA = pA;
                            result.point[idx].pivotB = p1[i];
                            result.point[idx].normalB = axis.dir;
                            result.point[idx].distance = penetration;
                        }
                    }
                    break;
                }
                case TRIANGLE_FEATURE_VERTEX: {
                    auto idx = result.num_points++;
                    auto pA_in_B = axis.pivotA;
                    auto pA_world = posB + rotate(ornB, pA_in_B);
                    auto pA = rotate(conjugate(ornA), pA_world - posA);
                    result.point[idx].pivotA = pA;
                    result.point[idx].pivotB = vertices[axis.tri_feature_index];
                    result.point[idx].normalB = axis.dir;
                    result.point[idx].distance = penetration;
                    break;
                }
                }
            break;

            case CYLINDER_FEATURE_FACE_EDGE: {
                switch (axis.tri_feature) {
                case TRIANGLE_FEATURE_FACE: {
                    if (point_in_triangle(vertices, tri_normal, axis.pivotA)) {
                        auto pivotA_world = posB + rotate(ornB, axis.pivotA);
                        auto pivotA = rotate(conjugate(ornA), pivotA_world - posA);
                        auto pivotB = project_plane(axis.pivotA, vertices[0], tri_normal);
                        auto idx = result.num_points++;
                        result.point[idx].pivotA = pivotA;
                        result.point[idx].pivotB = pivotB;
                        result.point[idx].normalB = axis.dir;
                        result.point[idx].distance = axis.distance;
                    }
                    break;
                }
                case TRIANGLE_FEATURE_EDGE: {
                    auto pivotA_world = posB + rotate(ornB, axis.pivotA);
                    auto pivotA = rotate(conjugate(ornA), pivotA_world - posA);
                    auto idx = result.num_points++;
                    result.point[idx].pivotA = pivotA;
                    result.point[idx].pivotB = axis.pivotB;
                    result.point[idx].normalB = axis.dir;
                    result.point[idx].distance = axis.distance;
                    break;
                }
                case TRIANGLE_FEATURE_VERTEX: {
                    break;
                }
                }
                break;
            }
            }
        }
    });

    return result;
}

}