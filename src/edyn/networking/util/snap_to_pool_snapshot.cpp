#include "edyn/networking/util/snap_to_pool_snapshot.hpp"
#include "edyn/networking/util/pool_snapshot.hpp"
#include "edyn/networking/sys/assign_previous_transforms.hpp"
#include "edyn/networking/sys/accumulate_discontinuities.hpp"
#include "edyn/replication/entity_map.hpp"

namespace edyn {

void snap_to_pool_snapshot(entt::registry &registry, const entity_map &emap,
                           const std::vector<entt::entity> &entities,
                           const std::vector<pool_snapshot> &pools) {
    assign_previous_transforms(registry);

    for (auto &pool : pools) {
        pool.ptr->replace_into_registry(registry, entities, emap);
    }

    accumulate_discontinuities(registry);
}

void snap_to_pool_snapshot(entt::registry &registry,
                           const std::vector<entt::entity> &entities,
                           const std::vector<pool_snapshot> &pools) {
    assign_previous_transforms(registry);

    for (auto &pool : pools) {
        pool.ptr->replace_into_registry(registry, entities);
    }

    accumulate_discontinuities(registry);
}

}
