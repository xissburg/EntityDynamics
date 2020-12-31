#include "edyn/constraints/generic_constraint.hpp"
#include "edyn/comp/constraint.hpp"
#include "edyn/comp/constraint_row.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/math/constants.hpp"
#include "edyn/math/matrix3x3.hpp"
#include "edyn/util/constraint_util.hpp"
#include <entt/entt.hpp>

namespace edyn {

void generic_constraint::init(entt::entity entity, constraint &con, entt::registry &registry) {
    for (size_t i = 0; i < 6; ++i) {
        add_constraint_row(entity, con, registry, 100);
    }
}

void generic_constraint::prepare(entt::entity, constraint &con, entt::registry &registry, scalar dt) {
    auto &posA = registry.get<position>(con.body[0]);
    auto &posB = registry.get<position>(con.body[1]);

    auto &ornA = registry.get<orientation>(con.body[0]);
    auto rA = rotate(ornA, pivot[0]);

    auto &ornB = registry.get<orientation>(con.body[1]);
    auto rB = rotate(ornB, pivot[1]);

    auto rA_skew = skew(rA);
    auto rB_skew = skew(rB);
    const auto d = posA + rA - posB - rB;
    constexpr auto I = matrix3x3_identity;

    // Linear.
    for (size_t i = 0; i < 3; ++i) {
        auto &row = registry.get<constraint_row>(con.row[i]);
        auto p = rotate(ornA, I.row[i]);
        row.J = {p, rA_skew.row[i], -p, -rB_skew.row[i]};
        row.error = dot(p, d) / dt;
        row.lower_limit = -large_scalar;
        row.upper_limit = large_scalar;
    }

    // Angular.
    for (size_t i = 0; i < 3; ++i) {
        auto axis = rotate(ornA, I.row[i]);
        auto n = rotate(ornA, I.row[(i+1)%3]);
        auto m = rotate(ornB, I.row[(i+2)%3]);
        auto error = dot(n, m);

        auto &row = registry.get<constraint_row>(con.row[i + 3]);
        row.J = {vector3_zero, axis, vector3_zero, -axis};
        row.error = error / dt;
        row.lower_limit = -large_scalar;
        row.upper_limit = large_scalar;
    }
}

}