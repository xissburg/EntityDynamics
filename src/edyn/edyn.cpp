#include "edyn/edyn.hpp"
#include "edyn/collision/broadphase.hpp"
#include "edyn/collision/contact_event_emitter.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_manifold_map.hpp"
#include "edyn/collision/narrowphase.hpp"
#include "edyn/comp/child_list.hpp"
#include "edyn/comp/collision_exclusion.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/tire_stats.hpp"
#include "edyn/comp/tree_resident.hpp"
#include "edyn/config/config.h"
#include "edyn/config/execution_mode.hpp"
#include "edyn/constraints/constraint.hpp"
#include "edyn/constraints/null_constraint.hpp"
#include "edyn/context/registry_operation_context.hpp"
#include "edyn/context/settings.hpp"
#include "edyn/networking/comp/entity_owner.hpp"
#include "edyn/networking/context/client_network_context.hpp"
#include "edyn/parallel/message_dispatcher.hpp"
#include "edyn/shapes/shapes.hpp"
#include "edyn/simulation/stepper_async.hpp"
#include "edyn/simulation/stepper_sequential.hpp"
#include "edyn/dynamics/material_mixing.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/util/paged_mesh_load_reporting.hpp"
#include "edyn/util/rigidbody.hpp"
#include <entt/meta/factory.hpp>
#include <entt/core/hashed_string.hpp>

namespace edyn {

static void init_meta() {
    using namespace entt::literals;

    entt::meta<contact_manifold>().type()
        .data<&contact_manifold::body, entt::as_ref_t>("body"_hs);

    entt::meta<collision_exclusion>().type()
        .data<&collision_exclusion::entity, entt::as_ref_t>("entity"_hs);

    std::apply([&](auto ... c) {
        (entt::meta<decltype(c)>().type().template data<&decltype(c)::body, entt::as_ref_t>("body"_hs), ...);
    }, constraints_tuple);

    entt::meta<null_constraint>().type().data<&null_constraint::body, entt::as_ref_t>("body"_hs);

    entt::meta<entity_owner>().type()
        .data<&entity_owner::client_entity, entt::as_ref_t>("client_entity"_hs);

    entt::meta<antiroll_constraint>().type()
        .data<&antiroll_constraint::m_third_entity, entt::as_ref_t>("m_third_entity"_hs);

    entt::meta<island_resident>().type()
        .data<&island_resident::island_entity, entt::as_ref_t>("island_entity"_hs);

    entt::meta<parent_comp>().type()
        .data<&parent_comp::child, entt::as_ref_t>("child"_hs);

    entt::meta<child_list>().type()
        .data<&child_list::parent, entt::as_ref_t>("parent"_hs)
        .data<&child_list::next, entt::as_ref_t>("next"_hs);

    entt::meta<tire_stats>().type()
        .data<&tire_stats::other_entity, entt::as_ref_t>("other_entity"_hs);
    entt::meta<tire_stats>().type()
        .data<&tire_stats::patch_entity, entt::as_ref_t>("patch_entity"_hs);
}

void attach(entt::registry &registry, const init_config &config) {
    init_meta();

    auto &dispatcher = job_dispatcher::global();

    if (!dispatcher.running()) {
        auto num_workers = size_t{};

        switch (config.execution_mode) {
        case execution_mode::sequential:
            num_workers = 1; // One worker is needed for background tasks.
            break;
        case execution_mode::sequential_multithreaded:
            num_workers = config.num_worker_threads > 0 ?
                          config.num_worker_threads :
                          std::max(std::thread::hardware_concurrency(), 2u) - 1;
                          // Subtract one for the main thread.
            break;
        case execution_mode::asynchronous:
            num_workers = config.num_worker_threads > 0 ?
                          config.num_worker_threads :
                          std::max(std::thread::hardware_concurrency(), 3u) - 2;
                          // Subtract one for the main thread and another for the
                          // dedicated simulation worker thread.
            break;
        }

        dispatcher.start(num_workers);
    }

    auto &settings = registry.ctx().emplace<edyn::settings>();
    settings.execution_mode = config.execution_mode;
    settings.fixed_dt = config.fixed_dt;

    registry.ctx().emplace<entity_graph>();
    registry.ctx().emplace<material_mix_table>();
    registry.ctx().emplace<contact_manifold_map>(registry);
    registry.ctx().emplace<contact_event_emitter>(registry);
    registry.ctx().emplace<registry_operation_context>();
    auto timestamp = config.timestamp ? *config.timestamp : (*settings.time_func)();

    switch (config.execution_mode) {
    case execution_mode::sequential:
    case execution_mode::sequential_multithreaded:
        registry.ctx().emplace<broadphase>(registry);
        registry.ctx().emplace<narrowphase>(registry);
        registry.ctx().emplace<stepper_sequential>(registry, timestamp,
                                                   config.execution_mode == execution_mode::sequential_multithreaded);
        break;
    case execution_mode::asynchronous:
        registry.ctx().emplace<stepper_async>(registry, timestamp);
        break;
    }

    internal::init_paged_mesh_load_reporting(registry);
}

template<typename... Ts>
void registry_clear(entt::registry &registry, [[maybe_unused]] const std::tuple<Ts...> &) {
    registry.clear<Ts...>();
}

void detach(entt::registry &registry) {
    internal::deinit_paged_mesh_load_reporting(registry);

    registry.ctx().erase<settings>();
    registry.ctx().erase<entity_graph>();
    registry.ctx().erase<material_mix_table>();
    registry.ctx().erase<contact_manifold_map>();
    registry.ctx().erase<contact_event_emitter>();
    registry.ctx().erase<registry_operation_context>();
    registry.ctx().erase<broadphase>();
    registry.ctx().erase<narrowphase>();
    registry.ctx().erase<stepper_async>();
    registry.ctx().erase<stepper_sequential>();

    registry.clear<rigidbody_tag, constraint_tag, dynamic_tag, kinematic_tag, static_tag,
                   procedural_tag, networked_tag, external_tag, network_exclude_tag,
                   sleeping_disabled_tag, sleeping_tag, disabled_tag>();
    registry.clear<collision_filter>();

    registry.clear<shape_index>();
    registry.clear<AABB>();
    registry.clear<rolling_tag>();
    registry.clear<roll_direction>();

    registry_clear(registry, shapes_tuple);
    registry_clear(registry, constraints_tuple);

    registry.clear<material, gravity, center_of_mass>();

    registry.clear<linvel, angvel>();
    registry.clear<mass, mass_inv>();
    registry.clear<inertia, inertia_inv, inertia_world_inv>();

    registry.clear<present_position, present_orientation>();
    registry.clear<position, orientation>();

    registry.clear<graph_node>();
    registry.clear<graph_edge>();
    registry.clear<island_resident, multi_island_resident>();
    registry.clear<tree_resident>();
    registry.clear<null_constraint>();

    // All manifolds are created by the engine thus destroy all entities.
    auto manifold_view = registry.view<contact_manifold>();
    registry.destroy(manifold_view.begin(), manifold_view.end());

    job_dispatcher::global().stop();
    message_dispatcher::global().clear_queues();
}

scalar get_fixed_dt(const entt::registry &registry) {
    return registry.ctx().at<settings>().fixed_dt;
}

void set_fixed_dt(entt::registry &registry, scalar dt) {
    auto &settings = registry.ctx().at<edyn::settings>();
    settings.fixed_dt = dt;

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->settings_changed();
    }

    if (auto *ctx = registry.ctx().find<client_network_context>()) {
        ctx->extrapolator->set_settings(settings);
    }
}

void set_max_steps_per_update(entt::registry &registry, unsigned max_steps) {
    auto &settings = registry.ctx().at<edyn::settings>();
    settings.max_steps_per_update = max_steps;

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->settings_changed();
    }

    if (auto *ctx = registry.ctx().find<client_network_context>()) {
        ctx->extrapolator->set_settings(settings);
    }
}

bool is_paused(const entt::registry &registry) {
    return registry.ctx().at<settings>().paused;
}

void set_paused(entt::registry &registry, bool paused) {
    registry.ctx().at<settings>().paused = paused;

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->set_paused(paused);
    } else {
        registry.ctx().at<stepper_sequential>().set_paused(paused);
    }
}

void update(entt::registry &registry) {
    auto &settings = registry.ctx().at<edyn::settings>();
    auto time = (*settings.time_func)();
    update(registry, time);
}

void update(entt::registry &registry, double time) {
    if (registry.ctx().contains<stepper_async>()) {
        registry.ctx().at<stepper_async>().update(time);
    } else if (registry.ctx().contains<stepper_sequential>()) {
        registry.ctx().at<stepper_sequential>().update(time);
    }

    internal::update_paged_mesh_load_reporting(registry);
}

void step_simulation(entt::registry &registry) {
    EDYN_ASSERT(is_paused(registry));

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->step_simulation();
    } else {
        auto &settings = registry.ctx().at<edyn::settings>();
        auto time = (*settings.time_func)();
        registry.ctx().at<stepper_sequential>().step_simulation(time);
    }

    internal::update_paged_mesh_load_reporting(registry);
}

void step_simulation(entt::registry &registry, double time) {
    EDYN_ASSERT(is_paused(registry));

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->step_simulation();
    } else {
        registry.ctx().at<stepper_sequential>().step_simulation(time);
    }

    internal::update_paged_mesh_load_reporting(registry);
}

execution_mode get_execution_mode(const entt::registry &registry) {
    auto &settings = registry.ctx().at<edyn::settings>();
    return settings.execution_mode;
}

void set_time_source(entt::registry &registry, double(*time_func)(void)) {
    EDYN_ASSERT(time_func != nullptr);

    auto &settings = registry.ctx().at<edyn::settings>();
    settings.time_func = time_func;

    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        stepper->settings_changed();
    }

    if (auto *ctx = registry.ctx().find<client_network_context>()) {
        ctx->extrapolator->set_settings(settings);
    }
}

double get_time(entt::registry &registry) {
    auto &settings = registry.ctx().at<edyn::settings>();
    auto time = (*settings.time_func)();
    return time;
}

double get_simulation_timestamp(entt::registry &registry) {
    if (auto *stepper = registry.ctx().find<stepper_async>()) {
        return stepper->get_simulation_timestamp();
    }

    if (auto *stepper = registry.ctx().find<stepper_sequential>()) {
        return stepper->get_simulation_timestamp();
    }

    return 0;
}

}
