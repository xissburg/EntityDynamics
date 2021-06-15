#ifndef EDYN_UTIL_MOMENT_OF_INERTIA_HPP
#define EDYN_UTIL_MOMENT_OF_INERTIA_HPP

#include <vector>
#include <cstdint>
#include "edyn/math/vector3.hpp"
#include "edyn/math/matrix3x3.hpp"
#include "edyn/shapes/shapes.hpp"

namespace edyn {

vector3 moment_of_inertia_solid_box(scalar mass, const vector3 &extents);
vector3 moment_of_inertia_solid_capsule(scalar mass, scalar len, scalar radius);
scalar moment_of_inertia_solid_sphere(scalar mass, scalar radius);
scalar moment_of_inertia_hollow_sphere(scalar mass, scalar radius);

/**
 * @brief Calculates the diagonal of the inertia tensor for a solid cylinder
 * aligned with the x axis.
 * @param mass Mass of cylinder in kilograms.
 * @param len Length of cylinder along its axis.
 * @param radius Radius of cylinder.
 * @return Diagonal of inertia tensor.
 */
vector3 moment_of_inertia_solid_cylinder(scalar mass, scalar len, scalar radius);

/**
 * @brief Calculates the diagonal of the inertia tensor for a hollow cylinder
 * aligned with the x axis.
 * @param mass Mass of cylinder in kilograms.
 * @param len Length of cylinder along its axis.
 * @param inner_radius Inner radius of cylinder where there's no material.
 * @param outer_radius Material exists inbetween the inner and outer radii of
 * the cylinder.
 * @return Diagonal of inertia tensor.
 */
vector3 moment_of_inertia_hollow_cylinder(scalar mass, scalar len, 
                                          scalar inner_radius, scalar outer_radius);

matrix3x3 moment_of_inertia_polyhedron(scalar mass, 
                                       const std::vector<vector3> &vertices, 
                                       const std::vector<uint16_t> &indices,
                                       const std::vector<uint16_t> &faces);

// Default moment of inertia for shapes.
matrix3x3 moment_of_inertia(const plane_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const sphere_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const cylinder_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const capsule_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const mesh_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const box_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const polyhedron_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const compound_shape &sh, scalar mass);
matrix3x3 moment_of_inertia(const paged_mesh_shape &sh, scalar mass);

/**
 * @brief Visits the shape variant and calculates the moment of inertia of the
 * shape it holds.
 * @param var The shape variant.
 * @param mass Shape's mass.
 * @return Inertia tensor.
 */
matrix3x3 moment_of_inertia(const shapes_variant_t &var, scalar mass);

}

#endif // EDYN_UTIL_MOMENT_OF_INERTIA_HPP
