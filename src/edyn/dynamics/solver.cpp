#include "edyn/dynamics/solver.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/sys/apply_gravity.hpp"
#include "edyn/sys/integrate_linvel.hpp"
#include "edyn/sys/integrate_angvel.hpp"
#include "edyn/sys/integrate_spin.hpp"
#include "edyn/sys/update_tire_state.hpp"
#include "edyn/sys/update_aabbs.hpp"
#include "edyn/sys/update_rotated_meshes.hpp"
#include "edyn/sys/update_inertias.hpp"
#include "edyn/sys/update_origins.hpp"
#include "edyn/constraints/constraint_row.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/spin.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/constraints/constraint.hpp"
#include "edyn/constraints/constraint_impulse.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/dynamics/restitution_solver.hpp"
#include "edyn/context/settings.hpp"
#include <entt/entity/registry.hpp>

namespace edyn {

scalar solve(constraint_row &row) {
    auto dsA = vector3_zero;
    auto dsB = vector3_zero;

    if (row.use_spin[0] && row.dsA != nullptr) {
        dsA = row.spin_axis[0] * *row.dsA;
    }

    if (row.use_spin[1] && row.dsB != nullptr) {
        dsB = row.spin_axis[1] * *row.dsB;
    }

    auto delta_relvel = dot(row.J[0], *row.dvA) +
                        dot(row.J[1], *row.dwA + dsA) +
                        dot(row.J[2], *row.dvB) +
                        dot(row.J[3], *row.dwB + dsB);
    auto delta_impulse = (row.rhs - delta_relvel) * row.eff_mass;
    auto impulse = row.impulse + delta_impulse;

    if (impulse < row.lower_limit) {
        delta_impulse = row.lower_limit - row.impulse;
        row.impulse = row.lower_limit;
    } else if (impulse > row.upper_limit) {
        delta_impulse = row.upper_limit - row.impulse;
        row.impulse = row.upper_limit;
    } else {
        row.impulse = impulse;
    }

    return delta_impulse;
}

static
scalar solve3(constraint_row &row) {
    auto dsA = vector3_zero;
    auto dsB = vector3_zero;
    auto dsC = vector3_zero;

    if (row.use_spin[0] && row.dsA != nullptr) {
        dsA = row.spin_axis[0] * *row.dsA;
    }

    if (row.use_spin[1] && row.dsB != nullptr) {
        dsB = row.spin_axis[1] * *row.dsB;
    }

    if (row.use_spin[2] && row.dsC != nullptr) {
        dsC = row.spin_axis[2] * *row.dsC;
    }

    auto delta_relvel = dot(row.J[0], *row.dvA) +
                        dot(row.J[1], *row.dwA + dsA) +
                        dot(row.J[2], *row.dvB) +
                        dot(row.J[3], *row.dwB + dsB) +
                        dot(row.J[4], *row.dvC) +
                        dot(row.J[5], *row.dwC + dsC);
    auto delta_impulse = (row.rhs - delta_relvel) * row.eff_mass;
    auto impulse = row.impulse + delta_impulse;

    if (impulse < row.lower_limit) {
        delta_impulse = row.lower_limit - row.impulse;
        row.impulse = row.lower_limit;
    } else if (impulse > row.upper_limit) {
        delta_impulse = row.upper_limit - row.impulse;
        row.impulse = row.upper_limit;
    } else {
        row.impulse = impulse;
    }

    return delta_impulse;
}

template<typename C>
void update_impulse(entt::registry &registry, row_cache &cache, size_t &con_idx, size_t &row_idx) {
    auto con_view = registry.view<C>();
    auto imp_view = registry.view<constraint_impulse>();

    for (auto entity : con_view) {
        auto &imp = imp_view.get<constraint_impulse>(entity);
        auto num_rows = cache.con_num_rows[con_idx];
        for (size_t i = 0; i < num_rows; ++i) {
            imp.values[i] = cache.rows[row_idx + i].impulse;
        }

        row_idx += num_rows;
        ++con_idx;
    }
}

// Specialization to assign the impulses of friction constraints which are not
// stored in traditional constraint rows.
template<>
void update_impulse<contact_constraint>(entt::registry &registry, row_cache &cache, size_t &con_idx, size_t &row_idx) {
    auto con_view = registry.view<contact_constraint>();
    auto cp_view = registry.view<contact_point>();
    auto imp_view = registry.view<constraint_impulse>();
    auto &ctx = registry.ctx<internal::contact_constraint_context>();
    auto local_idx = size_t(0);
    auto roll_idx = size_t(0);

    for (auto entity : con_view) {
        auto &imp = imp_view.get<constraint_impulse>(entity);
        auto num_rows = cache.con_num_rows[con_idx];
        // Normal impulse.
        imp.values[0] = cache.rows[row_idx].impulse;

        // Friction impulse.
        auto &friction_rows = ctx.friction_rows[local_idx];

        for (auto i = 0; i < 2; ++i) {
            imp.values[1 + i] = friction_rows.row[i].impulse;
        }

        // Rolling friction impulse.
        if (cp_view.get<contact_point>(entity).roll_friction > 0) {
            auto &roll_rows = ctx.roll_friction_rows[roll_idx];
            for (auto i = 0; i < 2; ++i) {
                imp.values[4 + i] = roll_rows.row[i].impulse;
            }
            ++roll_idx;
        }

        // Spinning friction impulse.
        if (num_rows > 1) {
            imp.values[3] = cache.rows[row_idx + 1].impulse;
        }

        row_idx += num_rows;
        ++con_idx;
        ++local_idx;
    }
}

void update_impulses(entt::registry &registry, row_cache &cache) {
    // Assign impulses from constraint rows back into the `constraint_impulse`
    // components. The rows are inserted into the cache for each constraint type
    // in the order they're found in `constraints_tuple` and in the same order
    // they're in their EnTT pools, which means the rows in the cache can be
    // matched by visiting each constraint type in the order they appear in the
    // tuple.
    size_t con_idx = 0;
    size_t row_idx = 0;

    std::apply([&] (auto ... c) {
        (update_impulse<decltype(c)>(registry, cache, con_idx, row_idx), ...);
    }, constraints_tuple);
}

solver::solver(entt::registry &registry)
    : m_registry(&registry)
{
    registry.on_construct<linvel>().connect<&entt::registry::emplace<delta_linvel>>();
    registry.on_construct<angvel>().connect<&entt::registry::emplace<delta_angvel>>();
    registry.on_construct<spin>().connect<&entt::registry::emplace<delta_spin>>();

    registry.set<internal::contact_constraint_context>();
}

solver::~solver() = default;

void solver::update(scalar dt) {
    auto &registry = *m_registry;
    auto &settings = registry.ctx<edyn::settings>();

    m_row_cache.clear();

    // Apply restitution impulses before gravity to prevent resting objects to
    // start bouncing due to the initial gravity acceleration.
    solve_restitution(registry, dt);

    apply_gravity(registry, dt);

    // Setup constraints.
    prepare_constraints(registry, m_row_cache, dt);

    // Solve constraints.
    for (unsigned i = 0; i < settings.num_solver_velocity_iterations; ++i) {
        // Prepare constraints for iteration.
        iterate_constraints(registry, m_row_cache, dt);

        // Solve rows.
        for (auto &row : m_row_cache.rows) {
            if (row.num_entities == 3) {
                auto delta_impulse = solve3(row);
                apply_impulse3(delta_impulse, row);
            } else {
                auto delta_impulse = solve(row);
                apply_impulse(delta_impulse, row);
            }
        }
    }

    // Apply constraint velocity correction.
    auto vel_view = registry.view<linvel, angvel, delta_linvel, delta_angvel, dynamic_tag>();
    vel_view.each([] (linvel &v, angvel &w, delta_linvel &dv, delta_angvel &dw) {
        v += dv;
        w += dw;
        dv = vector3_zero;
        dw = vector3_zero;
    });

    // Assign applied impulses.
    update_impulses(registry, m_row_cache);

    auto spin_view = m_registry->view<spin, delta_spin, dynamic_tag>();
    spin_view.each([] (spin &s, delta_spin &delta) {
        s += delta;
        delta.s = 0;
    });

    // Integrate velocities to obtain new transforms.
    integrate_linvel(registry, dt);
    integrate_angvel(registry, dt);
    integrate_spin(registry, dt);

    update_tire_state(registry, dt);

    // Now that rigid bodies have moved, perform positional correction.
    for (unsigned i = 0; i < settings.num_solver_position_iterations; ++i) {
        if (solve_position_constraints(registry, dt)) {
            break;
        }
    }


    update_origins(registry);

    // Update rotated vertices of convex meshes after rotations change. It is
    // important to do this before `update_aabbs` because the rotated meshes
    // will be used to calculate AABBs of polyhedrons.
    update_rotated_meshes(registry);

    // Update AABBs after transforms change.
    update_aabbs(registry);

    // Update world-space moment of inertia.
    update_inertias(registry);
}

}
