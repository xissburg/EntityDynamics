#include "edyn/constraints/contact_patch_constraint.hpp"
#include "edyn/comp/origin.hpp"
#include "edyn/config/constants.hpp"
#include "edyn/constraints/constraint_row.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/comp/center_of_mass.hpp"
#include "edyn/comp/spin.hpp"
#include "edyn/math/quaternion.hpp"
#include "edyn/math/scalar.hpp"
#include "edyn/math/transform.hpp"
#include "edyn/math/vector3.hpp"
#include "edyn/util/tire_util.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/math/matrix3x3.hpp"
#include "edyn/math/math.hpp"
#include "edyn/shapes/cylinder_shape.hpp"
#include <entt/entt.hpp>

namespace edyn {

std::pair<vector3, vector3> get_tire_directions(vector3 axis, vector3 normal, quaternion orn) {
    auto lat_dir = project_direction(axis, normal);
    auto lon_dir = vector3_zero;

    if (try_normalize(lat_dir)) {
        lon_dir = cross(lat_dir, normal);
    } else {
        auto tire_z = quaternion_z(orn);
        lon_dir = tire_z - normal * dot(tire_z, normal);
        lon_dir = normalize(lon_dir);
        lat_dir = cross(normal, lon_dir);
    }

    return {lon_dir, lat_dir};
}

bool intervals_intersect(scalar min_a, scalar max_a, scalar min_b, scalar max_b) {
    EDYN_ASSERT(min_a <= max_a);
    EDYN_ASSERT(min_b <= max_b);
    return min_a <= max_b && max_a >= min_b;
}

bool intervals_intersect_wrap_around(scalar min_a, scalar max_a, scalar min_b, scalar max_b, scalar range_min, scalar range_max) {
    EDYN_ASSERT(min_a >= range_min && min_a <= range_max);
    EDYN_ASSERT(min_b >= range_min && min_b <= range_max);
    EDYN_ASSERT(max_a >= range_min && max_a <= range_max);
    EDYN_ASSERT(max_b >= range_min && max_b <= range_max);

    if (min_a <= max_a && min_b <= max_b) {
        return intervals_intersect(min_a, max_a, min_b, max_b);
    }

    if (min_a > max_a) {
        if (min_b > max_b) {
            // If both wrap around then they intersect as a consequence.
            return true;
        } else {
            return intervals_intersect(range_min, max_a, min_b, max_b) ||
                   intervals_intersect(min_a, range_max, min_b, max_b);
        }
    }

    EDYN_ASSERT(min_b > max_b);
    return intervals_intersect(min_a, max_a, range_min, max_b) ||
           intervals_intersect(min_a, max_a, min_b, range_max);
}

void contact_patch_constraint::prepare(const entt::registry &registry, entt::entity entity, const contact_manifold &manifold,
                                       constraint_row_prep_cache &cache, scalar dt,
                                       const constraint_body &bodyA, const constraint_body &bodyB) {
    if (manifold.num_points == 0) {
        return;
    }

    // Wheel spin axis in world space.
    const auto axis = quaternion_x(bodyA.orn);
    auto spin_axisA = axis;
    auto spinvelA = spin_axisA * bodyA.spin;
    auto spin_axisB = quaternion_x(bodyB.orn);
    auto spinvelB = spin_axisB * bodyB.spin;

    auto spin_ornA = bodyA.orn * quaternion_axis_angle(vector3_x, bodyA.spin_angle);
    auto spin_angvelA = bodyA.angvel + spinvelA;

    // Store initial size of the constraint row cache so the number of rows
    // for this contact constraint can be calculated at the end.
    unsigned imp_idx = 0;

    // Create non-penetration constraint rows for each contact point.
    // Ignore spin for normal constraint since it only affects tangential
    // directions and for cylinders the normal always points towards the
    // spin axis.
    for (unsigned pt_idx = 0; pt_idx < manifold.num_points; ++pt_idx) {
        auto &cp = manifold.get_point(pt_idx);

        EDYN_ASSERT(length_sqr(cp.normal) > EDYN_EPSILON);
        auto normal = cp.normal;
        auto pivotA = to_world_space(cp.pivotA, bodyA.origin, bodyA.orn);
        auto pivotB = to_world_space(cp.pivotB, bodyB.origin, bodyB.orn);
        auto rA = pivotA - bodyA.pos;
        auto rB = pivotB - bodyB.pos;

        auto &row = cache.add_row_with_spin();
        row.J = {normal, cross(rA, normal), -normal, -cross(rB, normal)};
        row.impulse = impulse[imp_idx++];
        row.use_spin[0] = true;
        row.use_spin[1] = true;
        row.spin_axis[0] = spin_axisA;
        row.spin_axis[1] = spin_axisB;
        row.lower_limit = 0;

        auto vA = bodyA.linvel + cross(bodyA.angvel, rA);
        auto vB = bodyB.linvel + cross(bodyB.angvel, rB);
        auto relvel = vA - vB;
        auto normal_relspd = dot(relvel, normal);
        auto stiffness = velocity_dependent_vertical_stiffness(m_normal_stiffness,
                                                               std::max(normal_relspd, scalar(0)));

        // Divide stiffness by number of points in the same contact plane
        // for correct force distribution.
        unsigned num_points_in_same_plane = 0;

        for (unsigned i = 0; i < manifold.num_points; ++i) {
            auto &other_cp = manifold.get_point(i);
            auto other_pivotB = to_world_space(other_cp.pivotB, bodyB.origin, bodyB.orn);

            if (std::abs(dot(pivotB - other_pivotB, normal)) < collision_threshold) {
                ++num_points_in_same_plane;
            }
        }

        auto deflection = std::max(-cp.distance, scalar(0));
        auto spring_force = deflection * stiffness / num_points_in_same_plane;
        auto damper_force = m_normal_damping * -normal_relspd / num_points_in_same_plane;

        row.upper_limit = std::abs(spring_force + damper_force) * dt;

        auto &options = cache.get_options();
        options.error = -large_scalar;
    }

    auto &cyl = registry.get<cylinder_shape>(body[0]);

    struct point_info {
        scalar angle;
        scalar half_length;
        scalar deflection;
        vector3 normal;
        vector3 pivot;
        scalar impulse;
        scalar friction;
        uint32_t lifetime;
    };

    std::array<point_info, max_contacts> infos;

    auto max_row_half_length = cyl.radius * scalar(0.9);
    auto r0_inv = scalar(1) / cyl.radius;
    auto num_points = size_t{};

    for (unsigned pt_idx = 0; pt_idx < manifold.num_points; ++pt_idx) {
        auto &cp = manifold.get_point(pt_idx);
        auto defl = std::max(-cp.distance, scalar(0));

        if (!(defl > EDYN_EPSILON)) {
            continue;
        }

        scalar angle = std::atan2(cp.pivotA.y, cp.pivotA.z);

        // Transform angle from [-π, π] to [0, 2π] because in spin space
        // angles are represented by a value in that range plus a number of
        // complete turns.
        if (angle < 0) {
            angle += 2 * pi;
        }

        // Add spin angle to bring the contact angle into spin space.
        angle += bodyA.spin_angle;

        auto &info = infos[num_points++];
        info.angle = angle;

        info.deflection = defl;
        info.half_length = std::min(scalar(0.4) * cyl.radius *
                                    (defl * r0_inv + scalar(2.25) *
                                    std::sqrt(defl * r0_inv)),
                                    max_row_half_length);
        info.normal = cp.normal;
        // A point on the contact plane.
        info.pivot = to_world_space(cp.pivotB, bodyB.origin, bodyB.orn);
        info.friction = cp.friction;
        info.lifetime = cp.lifetime;
        // Need to verify whether this really contains the applied normal impulse.
        // Doesn't seem so since there isn't a correspondence between contact
        // points and contact patches. There can be one patch for more than one point
        // if the lie about the same line longitudinally.
        info.impulse = impulse[pt_idx * 4];
    }

    if (num_points == 0) {
        return;
    }

    // Merge points together into a single patch based on their distance
    // along the circumference of the tire.
    for (unsigned i = 0; i < num_points; ++i) {
        auto &info_i = infos[i];
        auto min_i = normalize_angle(info_i.angle - info_i.half_length / cyl.radius);
        auto max_i = normalize_angle(info_i.angle + info_i.half_length / cyl.radius);
        unsigned patch_points = 1;

        auto weighted_angle = info_i.angle * info_i.deflection;
        auto weighted_normal = info_i.normal * info_i.deflection;
        auto weighted_pivot = info_i.pivot * info_i.deflection;
        auto defl_accum = info_i.deflection;

        // Look for nearby points ignoring the ones that were already
        // processed previously.
        for (unsigned k = i + 1; k < num_points;) {
            auto &info_k = infos[k];
            auto min_k = normalize_angle(info_k.angle - info_k.half_length / cyl.radius);
            auto max_k = normalize_angle(info_k.angle + info_k.half_length / cyl.radius);

            // Check if intervals intersect, considering they're in the [-π, π]
            // range and they wrap around.
            if (intervals_intersect_wrap_around(min_i, max_i, min_k, max_k, -pi, pi)) {
                weighted_angle += info_k.angle * info_k.deflection;
                weighted_normal += info_k.normal * info_k.deflection;
                weighted_pivot += info_k.pivot * info_k.deflection;
                defl_accum += info_k.deflection;
                info_i.impulse += info_k.impulse;
                info_i.friction += info_k.friction;
                info_i.lifetime = std::max(info_i.lifetime, info_k.lifetime);
                ++patch_points;

                // Remove k-th element by replacing with last and
                // decrementing size.
                info_k = infos[--num_points];
            } else {
                ++k;
            }
        }

        info_i.angle = weighted_angle / defl_accum;
        info_i.normal = normalize(weighted_normal / defl_accum);
        info_i.pivot = weighted_pivot / defl_accum;
        info_i.deflection = defl_accum / patch_points;
        info_i.friction /= patch_points;
    }

    // Look for an existing contact patch that is at about the same angle
    // as the newly calculated locations. Remove patches that are at locations
    // that do not have a close match among the new locations.

    // Keep track of which of the existing points have been merged into a patch
    // so they can be skipped later when creating new patches.
    auto merged_infos = make_array<max_contacts>(false);

    // Store the angle of each patch before assigning their new angle so the
    // delta can be calculated.
    std::array<scalar, max_contacts> prev_patch_angles;

    auto init_patch_with_info = [] (contact_patch_constraint::contact_patch &patch, const point_info &info) {
        patch.angle          = info.angle;
        patch.deflection     = info.deflection;
        patch.normal         = info.normal;
        patch.pivot          = info.pivot;
        patch.normal_impulse = info.impulse;
        patch.friction       = info.friction;
        patch.lifetime       = info.lifetime;
        patch.length         = info.half_length * 2;
    };

    for (unsigned i = 0; i < num_patches;) {
        auto &patch = patches[i];
        // Predict what is most likely to be the current angle of a patch
        // by subtracting the angle change over one step from the previous
        // angle.
        auto predicted_angle = normalize_angle(patch.angle - bodyA.spin * dt);

        bool found = false;

        for (unsigned j = 0; j < num_points; ++j) {
            auto &info = infos[j];

            // Consider wrap around.
            auto new_angle = normalize_angle(info.angle);
            auto a = std::min(predicted_angle, new_angle);
            auto b = std::max(predicted_angle, new_angle);
            auto dist = std::min(b - a, a + pi2 - b);

            if (dist < to_radians(5)) {
                prev_patch_angles[i] = patch.angle;
                init_patch_with_info(patch, info);
                merged_infos[j] = true;
                found = true;
                break;
            }
        }

        if (found) {
            ++i;
        } else {
            // Remove patch by assigning last and decrementing count.
            patch = patches[--num_patches];
        }
    }

    // Insert new contact patches.
    for (unsigned i = 0; i < num_points; ++i) {
        if (merged_infos[i]) continue;

        auto &info = infos[i];
        auto k = num_patches++;
        auto &patch = patches[k];
        prev_patch_angles[k] = patch.angle;
        init_patch_with_info(patch, info);
    }

    EDYN_ASSERT(num_patches <= max_contacts);

    // Create constraint rows for each contact patch.
    for (unsigned i = 0; i < num_patches; ++i) {
        auto &patch = patches[i];

        const auto normal = patch.normal;
        auto sin_camber = std::clamp(dot(axis, normal), scalar(-1), scalar(1));
        auto camber_angle = std::asin(sin_camber);
        auto [lon_dir, lat_dir] = get_tire_directions(axis, normal, bodyA.orn);

        // Calculate starting point of contact patch on the contact plane.
        auto point_on_circle = project_plane(patch.pivot, bodyA.pos, axis);
        auto point_on_cylinder = normalize(point_on_circle - bodyA.pos) * cyl.radius + bodyA.pos;
        auto point_on_edge = point_on_cylinder + axis * cyl.half_length * (sin_camber > 0 ? -1 : 1);
        auto circle_center = bodyA.pos + axis * cyl.half_length * (sin_camber > 0 ? -1 : 1);
        auto radial_dir = point_on_edge - circle_center;
        auto radial_dir_norm = normalize(radial_dir);
        bool is_parallel = std::abs(dot(radial_dir_norm, normal)) < 0.001;
        vector3 patch_lat_pos0;

        if (is_parallel) {
            // The contact patch starting point is at the top of the
            // sidewall in this case.
            patch_lat_pos0 = circle_center + radial_dir - radial_dir_norm * (cyl.radius - m_sidewall_height);
        } else {
            // The starting point is at the intersection between the line
            // connecting the center of the cylinder cap face closest to the
            // contact plane and the support point along -normal with the
            // contact plane up to the height of the sidewall.
            auto fraction = dot(patch.pivot - circle_center, normal) / dot(radial_dir, normal);

            if (fraction * cyl.radius > cyl.radius - m_sidewall_height) {
                patch_lat_pos0 = circle_center + radial_dir * fraction;
            } else {
                patch_lat_pos0 = circle_center + radial_dir_norm * (cyl.radius - m_sidewall_height);
            }
        }

        // Push the pivot downwards to match the height of the patch
        // position in case they differ in level.
        patch.pivot += normal * dot(normal, patch_lat_pos0 - patch.pivot);

        // Recalculate deflection in case it's been clamped since deformation
        // is limited by the sidewall height.
        patch.deflection = dot(normal, patch_lat_pos0 - point_on_edge);

        auto normal_force = patch.normal_impulse / dt;

        // Calculate contact patch width.
        auto normalized_contact_width = std::max(scalar(0.08),
            scalar(1) - scalar(1) /
            (normal_force * scalar(0.001) * (half_pi - std::abs(camber_angle)) / (std::abs(camber_angle) + scalar(0.001)) + 1));
        patch.width = cyl.half_length * 2 * normalized_contact_width;

        // Calculate the other end of the contact patch along the width of
        // the tire.
        auto patch_lat_pos1 = patch_lat_pos0 + lat_dir * patch.width * (sin_camber > 0 ? 1 : -1) * (dot(lat_dir, axis) > 0 ? 1 : -1);

        // Calculate center of pressure.
        auto normalized_center_offset = -std::sin(std::atan(camber_angle));

        // Where the tread row starts in the x-axis in object space.
        auto row_start = sin_camber > 0 ? -cyl.half_length : cyl.half_length - patch.width;

        auto center_lerp_param = (normalized_center_offset + scalar(1)) * scalar(0.5);
        auto contact_center = lerp(patch_lat_pos0, patch_lat_pos1, center_lerp_param);
        auto geometric_center = lerp(patch_lat_pos0, patch_lat_pos1, scalar(0.5));

        // Calculate deflection on each side of the contact patch which will be
        // interpolated to find the deflection at each tread row.
        // Ensure deflection0 is the deflection at the row start, i.e.
        // at negative x in tire's object space.
        scalar deflection0, deflection1;

        if (sin_camber > 0) {
            deflection0 = patch.deflection;
            deflection1 = deflection0 - sin_camber * patch.width;
        } else {
            deflection1 = patch.deflection;
            deflection0 = deflection1 + sin_camber * patch.width;
        }

        // Make the smaller deflection at least 10% of the bigger deflection.
        auto min_defl = std::max(deflection0, deflection1) * scalar(0.1);
        deflection0 = std::max(deflection0, min_defl);
        deflection1 = std::max(deflection1, min_defl);

        auto sin_contact_angle = std::sin(patch.angle);
        auto cos_contact_angle = std::cos(patch.angle);

        // Accumulate forces and errors along all bristles.
        auto lon_force = scalar(0);
        auto lat_force = scalar(0);
        auto aligning_torque = scalar(0);
        auto tread_width = patch.width / num_tread_rows;
        auto normal_pressure = normal_force / (patch.width * patch.length);

        // Number of full turns since last update.
        auto spin_count_delta = bodyA.spin_count - patch.spin_count;
        // Calculate previous contact angle including the full turns so all
        // ranges and angles are all laid out in a segment without wrapping around.
        auto prev_contact_angle = prev_patch_angles[i] - spin_count_delta * pi2;
        auto num_sliding_bristles = 0;

        for (size_t row_idx = 0; row_idx < num_tread_rows; ++row_idx) {
            auto &tread_row = patch.tread_rows[row_idx];

            // The patch is divided in tread rows of equal width. Each row has a
            // different length, which is proportional to the deflection. Sample
            // the points along the middle of the row.
            auto row_x = row_start + tread_width * scalar(row_idx + 0.5);

            // Normal deflection and length for this row of bristles.
            auto row_fraction = (row_x - row_start) / patch.width;
            auto defl = lerp(deflection0, deflection1, row_fraction);

            if (defl < EDYN_EPSILON) {
                // Reset tread and bristles.
                tread_row.half_length = 0;
                tread_row.half_angle = 0;

                // Set bristle properties according to the contact angle. This is
                // an empty range of the patch thus put all bristles at the same
                // location.
                auto row_start_pos_local = vector3{
                    row_x,
                    sin_contact_angle * cyl.radius,
                    cos_contact_angle * cyl.radius
                };
                auto row_start_pos = project_plane(to_world_space(row_start_pos_local, bodyA.pos, spin_ornA), contact_center, normal);
                auto row_start_posB = to_object_space(row_start_pos, bodyB.pos, bodyB.orn);
                tread_row.start_posB = tread_row.end_posB = row_start_posB;

                for (auto &bristle : tread_row.bristles) {
                    bristle.tip = bristle.root = row_start_pos;
                    bristle.pivotA = row_start_pos_local;
                    bristle.pivotB = row_start_posB;
                }

                continue;
            }

            auto max_row_half_length = cyl.radius * scalar(0.9);
            auto row_half_length = std::min(scalar(0.4) * cyl.radius *
                                            (defl * r0_inv + scalar(2.25) *
                                            std::sqrt(defl * r0_inv)),
                                            max_row_half_length);
            auto row_length = scalar(2) * row_half_length;
            auto row_half_angle = std::asin(row_half_length / cyl.radius);
            auto row_angle = scalar(2) * row_half_angle;

            auto bristle_angle_delta = row_angle / scalar(bristles_per_row);
            auto prev_bristle_angle_delta = (tread_row.half_angle * scalar(2)) / scalar(bristles_per_row);

            auto bristle_length_delta = row_length / scalar(bristles_per_row);

            // Contact patch extents in radians for this row.
            auto patch_start_angle = patch.angle - row_half_angle;
            auto patch_end_angle   = patch.angle + row_half_angle;

            auto prev_patch_start_angle = prev_contact_angle - tread_row.half_angle;
            auto prev_patch_end_angle   = prev_contact_angle + tread_row.half_angle;

            // Calculate intersection between previous and current contact patch
            // range. Bristles that lie in the intersection will be matched to a
            // brush tip calculated as an interpolation of the previous values.
            // For the bristles that lie outside of the intersection, assign a
            // brush tip from the line connecting the start of the previous and
            // current contact patches for this row, which is where new bristles
            // are laid down as the tire rolls over the surface.
            auto intersection_start_angle = std::max(patch_start_angle, prev_patch_start_angle);
            auto intersection_end_angle = std::min(patch_end_angle, prev_patch_end_angle);
            auto intersects = intersection_start_angle < intersection_end_angle;

            // Midpoint of the tread row in spin space.
            auto row_mid_pos_local = vector3{row_x, sin_contact_angle * (cyl.radius - defl),
                                                cos_contact_angle * (cyl.radius - defl)};
            // Direction vector of tread row in spin space. It is the radial direction
            // rotated 90 degrees clockwise.
            auto row_dir_local = vector3{0, cos_contact_angle, -sin_contact_angle};

            auto row_start_pos_local = row_mid_pos_local - row_dir_local * row_half_length;
            auto row_end_pos_local = row_mid_pos_local + row_dir_local * row_half_length;
            auto row_start_pos = project_plane(to_world_space(row_start_pos_local, bodyA.pos, spin_ornA), contact_center, normal);
            auto row_end_pos   = project_plane(to_world_space(row_end_pos_local, bodyA.pos, spin_ornA), contact_center, normal);

            auto prev_row_start_pos = to_world_space(tread_row.start_posB, bodyB.pos, bodyB.orn);
            auto prev_row_end_pos   = to_world_space(tread_row.end_posB, bodyB.pos, bodyB.orn);

            auto prev_bristle_defl = vector3_zero;
            auto prev_bristle_pivotB = std::array<vector3, contact_patch_constraint::bristles_per_row>{};
            auto prev_bristle_tips = std::array<vector3, contact_patch_constraint::bristles_per_row>{};

            // Values of `bristle.pivotB` (i.e. the object space position of the bristle tip)
            // and `bristle.tip` will change during bristle updates. Store their
            // previous values here so they can be referenced when interpolating
            // between the previous bristle tips in the region where the previous
            // and current contact patches intersect.
            for (size_t i = 0; i < bristles_per_row; ++i) {
                prev_bristle_pivotB[i] = tread_row.bristles[i].pivotB;
                prev_bristle_tips[i] = tread_row.bristles[i].tip;
            }

            for (size_t bristle_idx = 0; bristle_idx < bristles_per_row; ++bristle_idx) {
                auto &bristle = tread_row.bristles[bristle_idx];

                // Calculate bristle root position in tire's space. It lies along the
                // tread row length if there's no camber. With non-zero camber, an offset
                // is added to account for the curvature of the tread row with respect
                // to the contact surface, which is modeled as a section of an ellipse
                // which is a semi-circle scaled down along the x axis by `sin_camber`,
                // i.e. `f(x) = sqrt(r^2 - x^2) * sin(camber)`. This ellipse can be
                // observed by looking into the tire tread circles from above along the
                // normal vector.
                auto bristle_root_fraction = scalar(bristle_idx + 0.5) / scalar(bristles_per_row);
                // Z position from center. Use it in the circle function.
                auto bristle_z_local = (bristle_root_fraction - scalar(0.5)) * row_length;
                // Also subtract the tread row effective radius times `sin_camber` to
                // remove the offset from the tread row segment.
                auto camber_offset = std::sqrt(cyl.radius * cyl.radius - bristle_z_local * bristle_z_local) * sin_camber - (cyl.radius - defl) * sin_camber;
                auto bristle_root = lerp(row_start_pos, row_end_pos, bristle_root_fraction) + lat_dir * camber_offset;
                bristle.pivotA = to_object_space(bristle_root, bodyA.pos, spin_ornA);

                // Calculate bristle tip position and relative velocity between
                // root and tip.
                // Note the 0.5 term which places the sampling point in the middle
                // of the rectangular tread area.
                auto bristle_angle = patch_start_angle + bristle_angle_delta * scalar(bristle_idx + 0.5);
                auto bristle_tip = vector3_zero;

                if (intersects && bristle_angle >= intersection_start_angle && bristle_angle <= intersection_end_angle) {
                    // Bristle lies in the intersection.
                    // Find index of bristles in the previous patch which surround this bristle.
                    auto fraction = (bristle_angle - prev_patch_start_angle) /
                                    (prev_patch_end_angle - prev_patch_start_angle);
                    auto after_idx = static_cast<size_t>(std::round(fraction * bristles_per_row));
                    EDYN_ASSERT(after_idx <= bristles_per_row);
                    scalar before_angle, after_angle;
                    vector3 before_pivotB, after_pivotB;

                    if (after_idx == 0) {
                        // This bristle is located before the first bristle in the previous
                        // contact patch. Use the start of the previous patch as the location
                        // before it and the first bristle as the location after it.
                        before_angle = prev_patch_start_angle;
                        after_angle = before_angle + prev_bristle_angle_delta * scalar(0.5);
                        before_pivotB = tread_row.start_posB; // This still holds the previous value.
                        after_pivotB = prev_bristle_pivotB[after_idx];
                    } else if (after_idx == bristles_per_row) {
                        // This bristle is located after the last bristle in the previous
                        // contact patch. Use the last bristle as the location before it and
                        // the end of the previous contact patch as the location after it.
                        auto before_idx = after_idx - 1;
                        after_angle = prev_patch_end_angle;
                        before_angle = after_angle - prev_bristle_angle_delta * scalar(0.5);
                        before_pivotB = prev_bristle_pivotB[before_idx];
                        after_pivotB = tread_row.end_posB; // This still holds the previous value.
                    } else {
                        // This bristle lies between two of the bristles in the previous
                        // contact patch.
                        auto before_idx = after_idx - 1;
                        before_angle = prev_patch_start_angle + prev_bristle_angle_delta * scalar(before_idx + 0.5);
                        after_angle = prev_patch_start_angle + prev_bristle_angle_delta * scalar(after_idx + 0.5);
                        before_pivotB = prev_bristle_pivotB[before_idx];
                        after_pivotB = prev_bristle_pivotB[after_idx];
                    }

                    // Linearly interpolate the bristle tips.
                    auto inbetween_fraction = (bristle_angle - before_angle) / (after_angle - before_angle);
                    bristle.pivotB = lerp(before_pivotB, after_pivotB, inbetween_fraction);
                    bristle_tip = to_world_space(bristle.pivotB, bodyB.pos, bodyB.orn);
                } else if (bristle_angle >= prev_patch_end_angle) {
                    // Bristle is located after the end of the previous contact patch.
                    // Place it along the line connecting the end position of the previous
                    // to the start position of the current contact patch.
                    auto fraction = (bristle_angle - prev_patch_end_angle) /
                                    (patch_end_angle - prev_patch_end_angle);
                    bristle_tip = lerp(prev_row_end_pos, row_end_pos, fraction);
                    bristle.pivotB = to_object_space(bristle_tip, bodyB.pos, bodyB.orn);
                } else if (bristle_angle <= prev_patch_start_angle) {
                    // Bristle is located before the start of the previous contact patch.
                    // Place it along the line connecting the start position of the previous
                    // to the start position of the current contact patch.
                    auto fraction = (bristle_angle - prev_patch_start_angle) /
                                    (patch_start_angle - prev_patch_start_angle);
                    bristle_tip = lerp(prev_row_start_pos, row_start_pos, fraction);
                    bristle.pivotB = to_object_space(bristle_tip, bodyB.pos, bodyB.orn);
                } else {
                    EDYN_ASSERT(false);
                }

                auto mu0 = patch.friction * std::exp(scalar(-0.001) * m_load_sensitivity * normal_force);
                bristle.friction = mu0 / (1 + m_speed_sensitivity * bristle.sliding_spd);

                // The length for the first bristle is halved since it is located in
                // the middle of the rectangular tread.
                auto tread_length = (bristle_idx == 0 ? scalar(0.5) : scalar(1)) * bristle_length_delta;
                auto tread_area = tread_width * tread_length;

                // TODO: handle anysotropic stiffness.
                // The force is calculated as an integral from the previous deflection until the
                // current deflection along the row.
                auto bristle_defl = bristle_tip - bristle_root;
                auto bristle_defl_len = length(bristle_defl);

                auto bristle_pressure = m_lon_tread_stiffness * bristle_defl_len;
                auto max_friction_pressure = bristle.friction * normal_pressure;
                auto spring_force = vector3_zero;

                if (!(bristle_pressure > max_friction_pressure)) {
                    spring_force = m_lon_tread_stiffness * tread_area * (prev_bristle_defl + bristle_defl) * scalar(0.5);
                    bristle.sliding_spd = 0;
                } else {
                    // Bristle deflection force is greater than maximum friction force
                    // for the current normal load, which means the bristle must slide.
                    // Thus, move the bristle tip closer to its root so that the
                    // tangential deflection force is equals to the maximum friction force.
                    // The tread will begin to slide between the two discrete bristles,
                    // thus the integral has to be split in two piecewise integrals.
                    // It starts to slide when `f(s) = |v0 * (1 - s) + v1 * s|` is
                    // equals to `max_defl`, which becomes a quadratic equation,
                    // where `v0` and `v1` are the previous and current bristle deflections,
                    // respectively. If `dot(v1, v0)` is zero, then it is a linear equation.
                    auto max_defl = max_friction_pressure / m_lon_tread_stiffness;
                    auto v0 = prev_bristle_defl;
                    auto v1 = bristle_defl;
                    auto s = scalar(0);
                    auto a = scalar(-2) * dot(v0, v1);
                    auto b = dot(v1, v1) + scalar(2) * dot(v0, v1) - dot(v0, v0);
                    auto c = dot(v0, v0) - max_defl * max_defl;

                    if (std::abs(a) > EDYN_EPSILON) {
                        // Solve quadratic `as^2 + bs + c = 0`. Only the bigger solution
                        // should be considered.
                        auto d = b * b - scalar(4) * a * c;

                        if (!(d < 0)) {
                            s = (-b + std::sqrt(d)) / (scalar(2) * a);
                        }
                    } else if (std::abs(b) > EDYN_EPSILON) {
                        // It is a linear equation `bs + c = 0`.
                        s = -c / b;
                    }

                    //EDYN_ASSERT(s > -0.1 && s < 1.1);

                    // Deflection vector at the point where it starts to slide.
                    auto midpoint_defl = lerp(prev_bristle_defl, bristle_defl, s);
                    auto bristle_dir = bristle_defl / bristle_defl_len;
                    bristle_defl = bristle_dir * max_defl;

                    auto area0 = tread_area * s;
                    auto area1 = tread_area * (1 - s);
                    auto f0 = m_lon_tread_stiffness * area0 * (prev_bristle_defl + midpoint_defl) * scalar(0.5);
                    auto f1 = m_lon_tread_stiffness * area1 * (midpoint_defl + bristle_defl) * scalar(0.5);
                    spring_force = f0 + f1;

                    bristle_tip = bristle_root + bristle_defl;

                    auto vel_tipA = project_direction(bodyA.linvel + cross(spin_angvelA, bristle_tip - bodyA.pos), normal);
                    auto vel_tipB = project_direction(bodyB.linvel + cross(bodyB.angvel + spinvelB, bristle_tip - bodyB.pos), normal);
                    bristle.sliding_spd = length(vel_tipA - vel_tipB);

                    // Move pivot in B to match new tip location.
                    bristle.pivotB = to_object_space(bristle_tip, bodyB.pos, bodyB.orn);

                    ++num_sliding_bristles;
                }

                // Point of force application.
                auto prev_bristle_root = bristle_idx > 0 ?
                    tread_row.bristles[bristle_idx - 1].root :
                    row_start_pos;
                auto midpoint = (bristle_root + prev_bristle_root) * scalar(0.5);

                lon_force += dot(lon_dir, spring_force);
                lat_force += dot(lat_dir, spring_force);
                aligning_torque += dot(cross(midpoint - contact_center, spring_force), normal);

                // Assign new root and tip.
                bristle.tip = bristle_tip;
                bristle.root = bristle_root;

                prev_bristle_defl = bristle_defl;
            }

            // Add force from last bristle until end of patch.
            {
                auto tread_length = bristle_length_delta / scalar(2);
                auto tread_area = tread_width * tread_length;
                auto bristle_root = tread_row.bristles.back().root;
                auto midpoint = (row_end_pos + bristle_root) * scalar(0.5);

                auto spring_force = m_lon_tread_stiffness * tread_area * prev_bristle_defl * scalar(0.5);
                lon_force += dot(lon_dir, spring_force);
                lat_force += dot(lat_dir, spring_force);
                aligning_torque += dot(cross(midpoint - contact_center, spring_force), normal);
            }

            tread_row.half_angle = row_half_angle;
            tread_row.half_length = row_half_length;
            tread_row.start_posB = to_object_space(row_start_pos, bodyB.pos, bodyB.orn);
            tread_row.end_posB = to_object_space(row_end_pos, bodyB.pos, bodyB.orn);
            tread_row.start_pos = row_start_pos;
            tread_row.end_pos = row_end_pos;
        }

        // Calculate average bristle sliding speed.
        patch.sliding_spd_avg = scalar(0);

        for (auto &row : patch.tread_rows) {
            for (auto &bristle : row.bristles) {
                patch.sliding_spd_avg += bristle.sliding_spd;
            }
        }

        patch.sliding_spd_avg /= num_tread_rows * bristles_per_row;
        patch.sliding_ratio = scalar(num_sliding_bristles) / scalar(num_tread_rows * bristles_per_row);
        auto rA = contact_center - bodyA.pos;
        auto rB = contact_center - bodyB.pos;

        // Longitudinal stiffness.
        {
            auto p = cross(rA, lon_dir);
            auto q = cross(rB, lon_dir);
            auto spring_impulse = lon_force * dt;

            auto &row = cache.add_row_with_spin();
            row.J = {lon_dir, p, -lon_dir, -q};
            row.lower_limit = std::min(spring_impulse, scalar(0));
            row.upper_limit = std::max(scalar(0), spring_impulse);
            row.impulse = impulse[imp_idx++];
            row.use_spin[0] = true;
            row.use_spin[1] = true;
            row.spin_axis[0] = spin_axisA;
            row.spin_axis[1] = spin_axisB;

            auto &options = cache.get_options();
            options.error = spring_impulse > 0 ? -large_scalar : large_scalar;
        }

        // Lateral stiffness.
        {
            auto p = cross(rA, lat_dir);
            auto q = cross(rB, lat_dir);
            auto spring_impulse = lat_force * dt;

            auto &row = cache.add_row();
            row.J = {lat_dir, p, -lat_dir, -q};
            row.lower_limit = std::min(spring_impulse, scalar(0));
            row.upper_limit = std::max(scalar(0), spring_impulse);
            row.impulse = impulse[imp_idx++];

            auto &options = cache.get_options();
            options.error = spring_impulse > 0 ? -large_scalar : large_scalar;
        }

        // Aligning moment.
        {
            // TODO: Also apply force on center of mass due to torque not necessarily
            // being aligned with the center of mass (e.g. when there's non-zero camber).
            auto spring_impulse = aligning_torque * dt;

            auto &row = cache.add_row();
            row.J = {vector3_zero, normal, vector3_zero, -normal};
            row.lower_limit = std::min(spring_impulse, scalar(0));
            row.upper_limit = std::max(scalar(0), spring_impulse);
            row.impulse = impulse[imp_idx++];

            auto &options = cache.get_options();
            options.error = spring_impulse > 0 ? -large_scalar : large_scalar;
        }

        patch.sin_camber = sin_camber;
        patch.center = geometric_center;
        patch.pivot = contact_center;
        patch.lat_dir = lat_dir;
        patch.lon_dir = lon_dir;
        patch.spin_count = bodyA.spin_count;
    }
}

}
