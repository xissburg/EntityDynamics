#include "edyn/constraints/contact_constraint.hpp"
#include "edyn/constraints/constraint_row.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/comp/spin.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/origin.hpp"
#include "edyn/comp/roll_direction.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/math/constants.hpp"
#include "edyn/math/geom.hpp"
#include "edyn/math/math.hpp"
#include "edyn/math/transform.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/context/settings.hpp"
#include <entt/entity/registry.hpp>

namespace edyn {

namespace internal {
    void solve_friction_row_pair(contact_friction_row_pair &friction_row_pair, constraint_row &normal_row) {
        vector2 delta_impulse;
        vector2 impulse;
        auto &friction_rows = friction_row_pair.row;

        auto dsA = vector3_zero;
        auto dsB = vector3_zero;

        if (normal_row.dsA != nullptr) {
            dsA = normal_row.spin_axis[0] * *normal_row.dsA;
        }

        if (normal_row.dsB != nullptr) {
            dsB = normal_row.spin_axis[1] * *normal_row.dsB;
        }

        for (auto i = 0; i < 2; ++i) {
            auto &friction_row = friction_rows[i];
            auto delta_relspd = get_relative_speed(friction_row.J,
                                                   *normal_row.dvA, *normal_row.dwA + dsA,
                                                   *normal_row.dvB, *normal_row.dwB + dsB);
            delta_impulse[i] = (friction_row.rhs - delta_relspd) * friction_row.eff_mass;
            impulse[i] = friction_row.impulse + delta_impulse[i];
        }

        auto impulse_len_sqr = length_sqr(impulse);
        auto max_impulse_len = friction_row_pair.friction_coefficient * normal_row.impulse;

        // Limit impulse by normal load.
        if (impulse_len_sqr > square(max_impulse_len)) {
            auto impulse_len = std::sqrt(impulse_len_sqr);

            if (impulse_len > EDYN_EPSILON) {
                impulse = impulse / impulse_len * max_impulse_len;
            } else {
                impulse = {0, 0};
            }

            for (auto i = 0; i < 2; ++i) {
                delta_impulse[i] = impulse[i] - friction_rows[i].impulse;
            }
        }

        // Apply delta impulse.
        for (auto i = 0; i < 2; ++i) {
            auto &friction_row = friction_rows[i];
            friction_row.impulse = impulse[i];

            *normal_row.dvA += normal_row.inv_mA * friction_row.J[0] * delta_impulse[i];
            *normal_row.dvB += normal_row.inv_mB * friction_row.J[2] * delta_impulse[i];

            auto deltaA = normal_row.inv_IA * friction_row.J[1] * delta_impulse[i];
            auto deltaB = normal_row.inv_IB * friction_row.J[3] * delta_impulse[i];

            // Split spin impulse as in `apply_angular_impulse`.
            if (normal_row.dsA) {
                auto spin = dot(normal_row.spin_axis[0], deltaA);
                *normal_row.dsA += spin;
                *normal_row.dwA += deltaA - normal_row.spin_axis[0] * spin;
            } else {
                *normal_row.dwA += deltaA;
            }

            if (normal_row.dsB) {
                auto spin = dot(normal_row.spin_axis[1], deltaB);
                *normal_row.dsB += spin;
                *normal_row.dwB += deltaB - normal_row.spin_axis[1] * spin;
            } else {
                *normal_row.dwB += deltaB;
            }
        }
    }

    void apply_friction_angular_impulse(scalar impulse,
                                        contact_friction_row &row,
                                        constraint_row &normal_row,
                                        size_t ent_idx) {
        auto idx_J = ent_idx * 2 + 1;
        matrix3x3 inv_I;
        delta_angvel *dw;
        delta_spin *ds;

        if (ent_idx == 0) {
            inv_I = normal_row.inv_IA;
            dw = normal_row.dwA;
            ds = normal_row.dsA;
        } else {
            inv_I = normal_row.inv_IB;
            dw = normal_row.dwB;
            ds = normal_row.dsB;
        }

        auto delta = inv_I * row.J[idx_J] * impulse;

        if (ds) {
            // Split delta in a spin component and an angular component.
            auto spin = dot(normal_row.spin_axis[ent_idx], delta);
            *ds += spin;
            // Subtract spin to obtain only angular component.
            *dw += delta - normal_row.spin_axis[ent_idx] * spin;
        } else {
            *dw += delta;
        }
    }

    void apply_friction_impulse(scalar impulse, contact_friction_row &row, constraint_row &normal_row) {
        // Apply linear impulse.
        *normal_row.dvA += normal_row.inv_mA * row.J[0] * impulse;
        *normal_row.dvB += normal_row.inv_mB * row.J[2] * impulse;

        // Apply angular impulse.
        apply_friction_angular_impulse(impulse, row, normal_row, 0);
        apply_friction_angular_impulse(impulse, row, normal_row, 1);
    }

    void warm_start_friction(contact_friction_row &row, constraint_row &normal_row) {
        apply_friction_impulse(row.impulse, row, normal_row);
    }
}

template<>
void prepare_constraints<contact_constraint>(entt::registry &registry, row_cache &cache, scalar dt) {
    auto body_view = registry.view<position, orientation, linvel, angvel,
                                   mass_inv, inertia_world_inv,
                                   delta_linvel, delta_angvel>();
    auto con_view = registry.view<contact_constraint, contact_manifold>();
    auto origin_view = registry.view<origin>();
    auto spin_view = registry.view<spin, delta_spin>();
    auto roll_dir_view = registry.view<roll_direction>();
    auto &settings = registry.ctx().at<edyn::settings>();

    cache.rows.reserve(cache.rows.size() + con_view.size_hint());

    auto &ctx = registry.ctx().at<internal::contact_constraint_context>();
    ctx.row_start_index = cache.rows.size();
    ctx.row_count_start_index = cache.con_num_rows.size();
    ctx.friction_rows.clear();
    ctx.friction_rows.reserve(con_view.size_hint());
    ctx.roll_friction_rows.clear();

    for (auto [entity, con, manifold] : con_view.each()) {
        auto body = manifold.body;

        auto [posA, ornA, linvelA, angvelA, inv_mA, inv_IA, dvA, dwA] = body_view.get(body[0]);
        auto [posB, ornB, linvelB, angvelB, inv_mB, inv_IB, dvB, dwB] = body_view.get(body[1]);

        auto spinvelA = vector3_zero;
        auto spinvelB = vector3_zero;
        auto spin_axisA = vector3_zero;
        auto spin_axisB = vector3_zero;
        delta_spin *delta_spinA = nullptr, *delta_spinB = nullptr;

        if (spin_view.contains(con.body[0])) {
            auto &s = spin_view.get<spin>(con.body[0]);
            spin_axisA = quaternion_x(ornA);
            spinvelA = spin_axisA * scalar(s);
            delta_spinA = &spin_view.get<delta_spin>(con.body[0]);
        }

        if (spin_view.contains(con.body[1])) {
            auto &s = spin_view.get<spin>(con.body[1]);
            spin_axisB = quaternion_x(ornB);
            spinvelB = spin_axisB * scalar(s);
            delta_spinB = &spin_view.get<delta_spin>(con.body[1]);
        }

        auto originA = origin_view.contains(body[0]) ? origin_view.get<origin>(body[0]) : static_cast<vector3>(posA);
        auto originB = origin_view.contains(body[1]) ? origin_view.get<origin>(body[1]) : static_cast<vector3>(posB);

        // Store initial size of the constraint row cache so the number of rows
        // for this contact constraint can be calculated at the end.
        const auto row_start_index = cache.rows.size();

        // Create constraint rows for each contact point.
        for (unsigned pt_idx = 0; pt_idx < manifold.num_points; ++pt_idx) {
            auto &cp = manifold.get_point(pt_idx);

            EDYN_ASSERT(length_sqr(cp.normal) > EDYN_EPSILON);
            auto normal = cp.normal;
            auto pivotA = to_world_space(cp.pivotA, originA, ornA);
            auto pivotB = to_world_space(cp.pivotB, originB, ornB);
            auto rA = pivotA - posA;
            auto rB = pivotB - posB;

            // Create normal row, i.e. non-penetration constraint.
            auto &normal_row = cache.rows.emplace_back();
            normal_row.J = {normal, cross(rA, normal), -normal, -cross(rB, normal)};
            normal_row.inv_mA = inv_mA; normal_row.inv_IA = inv_IA;
            normal_row.inv_mB = inv_mB; normal_row.inv_IB = inv_IB;
            normal_row.dvA = &dvA; normal_row.dwA = &dwA; normal_row.dsA = delta_spinA;
            normal_row.dvB = &dvB; normal_row.dwB = &dwB; normal_row.dsB = delta_spinB;
            normal_row.use_spin[0] = true;
            normal_row.use_spin[1] = true;
            normal_row.spin_axis[0] = spin_axisA;
            normal_row.spin_axis[1] = spin_axisB;
            normal_row.impulse = cp.normal_impulse;
            normal_row.lower_limit = 0;

            auto normal_options = constraint_row_options{};

            // Do not use the traditional restitution path if the restitution solver
            // is being used.
            if (settings.num_restitution_iterations == 0) {
                normal_options.restitution = cp.restitution;
            }

            if (cp.distance < 0) {
                if (cp.stiffness < large_scalar) {
                    auto vA = linvelA + cross(angvelA, rA);
                    auto vB = linvelB + cross(angvelB, rB);
                    auto relvel = vA - vB;
                    auto normal_relvel = dot(relvel, normal);
                    // Divide stiffness by number of points for correct force
                    // distribution. All points have the same stiffness.
                    auto spring_force = cp.distance * cp.stiffness / manifold.num_points;
                    auto damper_force = normal_relvel * cp.damping / manifold.num_points;
                    normal_row.upper_limit = std::abs(spring_force + damper_force) * dt;
                    normal_options.error = -large_scalar;
                } else {
                    normal_row.upper_limit = large_scalar;
                }
            } else if (cp.stiffness >= large_scalar) {
                // It is not penetrating thus apply an impulse that will prevent
                // penetration after the following physics update.
                normal_options.error = cp.distance / dt;
                normal_row.upper_limit = large_scalar;
            }

            prepare_row(normal_row, normal_options, linvelA, angvelA + spinvelA, linvelB, angvelB + spinvelB);
            warm_start(normal_row);

            // Create special friction rows, always one pair per contact point.
            auto &friction_rows = ctx.friction_rows.emplace_back();
            friction_rows.friction_coefficient = cp.friction;

            vector3 tangents[2];
            plane_space(normal, tangents[0], tangents[1]);

            for (auto i = 0; i < 2; ++i) {
                auto &friction_row = friction_rows.row[i];
                friction_row.J = {tangents[i], cross(rA, tangents[i]), -tangents[i], -cross(rB, tangents[i])};
                friction_row.impulse = cp.friction_impulse[i];
                friction_row.eff_mass = get_effective_mass(friction_row.J, inv_mA, inv_IA, inv_mB, inv_IB);
                friction_row.rhs = -get_relative_speed(friction_row.J, linvelA, angvelA + spinvelA, linvelB, angvelB + spinvelB);
                warm_start_friction(friction_row, normal_row);
            }

            if (cp.roll_friction > 0) {
                auto &roll_rows = ctx.roll_friction_rows.emplace_back();
                roll_rows.friction_coefficient = cp.roll_friction;

                for (auto i = 0; i < 2; ++i) {
                    auto axis = tangents[i];

                    // If any of the bodies has a rolling direction, scale down the
                    // axis by the projection of the roll direction onto the axis,
                    // thus preventing impulses in the undesired directions.
                    for (auto j = 0; j < 2; ++j) {
                        if (roll_dir_view.contains(body[j])) {
                            auto roll_dir = rotate(ornA, roll_dir_view.get<roll_direction>(body[j]));
                            axis *= dot(roll_dir, axis);
                        }
                    }

                    auto &roll_row = roll_rows.row[i];
                    roll_row.J = {vector3_zero, axis, vector3_zero, -axis};
                    roll_row.impulse = cp.rolling_friction_impulse[i];
                    auto J_invM_JT = dot(inv_IA * roll_row.J[1], roll_row.J[1]) +
                                     dot(inv_IB * roll_row.J[3], roll_row.J[3]);

                    if (J_invM_JT > EDYN_EPSILON) {
                        roll_row.eff_mass = scalar(1) / J_invM_JT;
                    } else {
                        roll_row.eff_mass = 0;
                    }

                    roll_row.rhs = -get_relative_speed(roll_row.J, linvelA, angvelA + spinvelA, linvelB, angvelB + spinvelB);
                    warm_start_friction(roll_row, normal_row);
                }
            }

            if (cp.spin_friction > 0) {
                auto &spin_row = cache.rows.emplace_back();
                spin_row.J = {vector3_zero, normal, vector3_zero, -normal};
                spin_row.inv_mA = inv_mA; spin_row.inv_IA = inv_IA;
                spin_row.inv_mB = inv_mB; spin_row.inv_IB = inv_IB;
                spin_row.dvA = &dvA; spin_row.dwA = &dwA; spin_row.dsA = delta_spinA;
                spin_row.dvB = &dvB; spin_row.dwB = &dwB; spin_row.dsB = delta_spinB;
                spin_row.use_spin[0] = true;
                spin_row.use_spin[1] = true;
                spin_row.spin_axis[0] = spin_axisA;
                spin_row.spin_axis[1] = spin_axisB;
                spin_row.impulse = cp.spin_friction_impulse;

                prepare_row(spin_row, {}, linvelA, angvelA + spinvelA, linvelB, angvelB + spinvelB);
                warm_start(spin_row);
            }
        }

        auto num_rows = cache.rows.size() - row_start_index;
        cache.con_num_rows.push_back(num_rows);
    }
}

template<>
void iterate_constraints<contact_constraint>(entt::registry &registry, row_cache &cache, scalar dt) {
    auto &ctx = registry.ctx().at<internal::contact_constraint_context>();
    auto row_idx = ctx.row_start_index;
    auto roll_idx = size_t(0);
    auto cp_idx = size_t(0); // Global contact point index.

    // Remember that not all manifolds have a contact constraint, which happens
    // when one of the rigid bodies is a sensor, i.e. it doesn't have material.
    auto con_view = registry.view<contact_constraint, contact_manifold>();

    // Solve friction rows locally using a non-standard method where the impulse
    // is limited by the length of a 2D vector to assure a friction circle.
    // These are the same fundamental operations found in `edyn::solver` adapted
    // to couple the two friction constraints together.
    // It is interesting to note that all friction constraints will be solved
    // before the non-penetration constraints, i.e. they're not interleaved.
    // Solving the non-penetration constraints last helps minimize penetration
    // errors because there won't be additional errors introduced by other
    // constraints.
    for (auto entity : con_view) {
        auto &manifold = con_view.get<contact_manifold>(entity);

        manifold.each_point([&](contact_point &cp) {
            auto &normal_row = cache.rows[row_idx++];
            auto &friction_row_pair = ctx.friction_rows[cp_idx];
            internal::solve_friction_row_pair(friction_row_pair, normal_row);

            if (cp.roll_friction > 0) {
                auto &roll_row_pair = ctx.roll_friction_rows[roll_idx++];
                internal::solve_friction_row_pair(roll_row_pair, normal_row);
            }

            if (cp.spin_friction > 0) {
                auto &spin_row = cache.rows[row_idx++];
                auto max_impulse_len = cp.spin_friction * normal_row.impulse;
                spin_row.lower_limit = -max_impulse_len;
                spin_row.upper_limit = max_impulse_len;
            }

            ++cp_idx;
        });
    }
}

template<>
bool solve_position_constraints<contact_constraint>(entt::registry &registry, scalar dt) {
    // Solve position constraints by applying linear and angular corrections
    // iteratively. Based on Box2D's solver:
    // https://github.com/erincatto/box2d/blob/cd2c28dba83e4f359d08aeb7b70afd9e35e39eda/src/dynamics/b2_contact_solver.cpp#L676

    // Remember that not all manifolds have a contact constraint.
    auto con_view = registry.view<contact_constraint, contact_manifold>();
    auto body_view = registry.view<position, orientation, mass_inv, inertia_world_inv>();
    auto origin_view = registry.view<origin>();
    auto min_dist = scalar(0);

    for (auto entity : con_view) {
        auto &manifold = con_view.get<contact_manifold>(entity);

        if (manifold.num_points == 0) {
            continue;
        }

        // Ignore soft contacts.
        if (manifold.get_point(0).stiffness < large_scalar) {
            continue;
        }

        auto [posA, ornA, inv_mA, inv_IA] = body_view.get(manifold.body[0]);
        auto [posB, ornB, inv_mB, inv_IB] = body_view.get(manifold.body[1]);

        auto originA = origin_view.contains(manifold.body[0]) ? origin_view.get<origin>(manifold.body[0]) : static_cast<vector3>(posA);
        auto originB = origin_view.contains(manifold.body[1]) ? origin_view.get<origin>(manifold.body[1]) : static_cast<vector3>(posB);

        for (unsigned pt_idx = 0; pt_idx < manifold.num_points; ++pt_idx) {
            auto &cp = manifold.get_point(pt_idx);
            auto pivotA = to_world_space(cp.pivotA, originA, ornA);
            auto pivotB = to_world_space(cp.pivotB, originB, ornB);

            switch (cp.normal_attachment) {
            case contact_normal_attachment::normal_on_A:
                cp.normal = rotate(ornA, cp.local_normal);
                break;
            case contact_normal_attachment::normal_on_B:
                cp.normal = rotate(ornB, cp.local_normal);
                break;
            case contact_normal_attachment::none:
                break;
            }

            auto normal = cp.normal;
            cp.distance = dot(pivotA - pivotB, normal);
            min_dist = std::min(cp.distance, min_dist);

            auto rA = pivotA - posA;
            auto rB = pivotB - posB;
            auto J = std::array<vector3, 4>{normal, cross(rA, normal), -normal, -cross(rB, normal)};
            auto eff_mass = get_effective_mass(J, inv_mA, inv_IA, inv_mB, inv_IB);
            auto error = std::min(cp.distance, scalar(0));
            auto correction = -error * contact_position_correction_rate * eff_mass;

            posA += inv_mA * J[0] * correction;
            posB += inv_mB * J[2] * correction;

            // Use quaternion derivative to apply angular correction which should
            // be good enough for small angles.
            auto angular_correctionA = inv_IA * J[1] * correction;
            ornA += quaternion_derivative(ornA, angular_correctionA);
            ornA = normalize(ornA);

            auto angular_correctionB = inv_IB * J[3] * correction;
            ornB += quaternion_derivative(ornB, angular_correctionB);
            ornB = normalize(ornB);
        }

        auto basisA = to_matrix3x3(ornA);
        inv_IA = basisA * inv_IA * transpose(basisA);

        auto basisB = to_matrix3x3(ornB);
        inv_IB = basisB * inv_IB * transpose(basisB);
    }

    return min_dist > contact_position_solver_min_error;
}

}
