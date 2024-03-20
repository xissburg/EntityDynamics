#ifndef EDYN_UTIL_PAGED_MESH_LOAD_REPORTING_HPP
#define EDYN_UTIL_PAGED_MESH_LOAD_REPORTING_HPP

#include <entt/entity/fwd.hpp>
#include <entt/signal/sigh.hpp>

namespace edyn {

/**
 * @brief Triggers when a new page is loaded on a paged mesh shape.
 * @param registry Data source.
 * @return Sink which allows observing page load events. It provides the shape
 * entity and the index of page that was loaded.
 */
entt::sink<entt::sigh<void(entt::entity, size_t)>> on_paged_mesh_page_loaded(entt::registry &);

}

namespace edyn::internal {

void init_paged_mesh_load_reporting(entt::registry &registry);
void update_paged_mesh_load_reporting(entt::registry &registry);
void deinit_paged_mesh_load_reporting(entt::registry &registry);

}

#endif // EDYN_UTIL_PAGED_MESH_LOAD_REPORTING_HPP
