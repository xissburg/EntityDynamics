#ifndef EDYN_NETWORKING_NETWORKING_EXTERNAL_HPP
#define EDYN_NETWORKING_NETWORKING_EXTERNAL_HPP

#include <tuple>
#include <type_traits>
#include <entt/core/fwd.hpp>
#include <entt/entity/registry.hpp>
#include "edyn/comp/action_list.hpp"
#include "edyn/networking/comp/network_input.hpp"
#include "edyn/networking/comp/networked_comp.hpp"
#include "edyn/networking/context/client_network_context.hpp"
#include "edyn/networking/context/server_network_context.hpp"
#include "edyn/networking/extrapolation_input.hpp"
#include "edyn/util/registry_operation.hpp"

namespace edyn {

namespace internal {
    template<typename... Components>
    auto make_is_networked_component_func([[maybe_unused]] std::tuple<Components...>) {
        return [](entt::id_type id) {
            return ((id == entt::type_index<Components>::value()) || ...);
        };
    }

    template<typename... Components>
    auto make_is_network_input_component_func([[maybe_unused]] std::tuple<Components...>) {
        return [](entt::id_type id) {
            return ((id == entt::type_index<Components>::value() &&
                     std::is_base_of_v<network_input, Components>) || ...);
        };
    }

    template<typename... Actions>
    auto make_is_action_list_component_func([[maybe_unused]] std::tuple<Actions...>) {
        return [](entt::id_type id) {
            return ((id == entt::type_index<action_list<Actions>>::value()) || ...);
        };
    }

    template<typename... Components>
    auto make_mark_replaced_network_dirty_func([[maybe_unused]] std::tuple<Components...>) {
        return [](entt::registry &registry,
                  const registry_operation_collection &ops,
                  const entity_map &emap, double timestamp) {
            ops.replace_for_each<Components...>([&](entt::entity remote_entity, const auto &c) {
                if (!emap.contains(remote_entity)) {
                    return;
                }

                auto local_entity = emap.at(remote_entity);

                if (registry.all_of<networked_tag>(local_entity)) {
                    auto &n_dirty = registry.get_or_emplace<network_dirty>(local_entity);
                    n_dirty.insert<std::decay_t<decltype(c)>>(timestamp);
                }
            });
        };
    }
}

extern bool(*g_is_networked_component)(entt::id_type);
extern bool(*g_is_networked_input_component)(entt::id_type);
extern bool(*g_is_action_list_component)(entt::id_type);
extern void(*g_mark_replaced_network_dirty)(entt::registry &, const registry_operation_collection &,
                                            const entity_map &, double timestamp);

/**
 * @brief Register external networked components.
 * @tparam Components All external networked components.
 * @tparam Actions All action types. Note that **actions are not components**.
 * Instead, they're stored in `edyn::action_list<Action>` which is an actual
 * component that is assigned to an entity and contains a list of actions.
 * @param registry Data source.
 * @param input Tuple of input components.
 */
template<typename... Components, typename... Actions>
void register_networked_components(entt::registry &registry, std::tuple<Actions...> actions = {}) {
    auto external = std::tuple<Components...>{};
    auto all = std::tuple_cat(networked_components, external);

    if (auto *ctx = registry.try_ctx<client_network_context>()) {
        ctx->snapshot_importer.reset(new client_snapshot_importer_impl(all));
        ctx->snapshot_exporter.reset(new client_snapshot_exporter_impl(all, actions));

        auto input = std::tuple_cat(std::conditional_t<std::is_base_of_v<network_input, Components>,
                                    std::tuple<Components>, std::tuple<>>{}...);
        auto action_lists = std::tuple<action_list<Actions>...>{};
        auto input_all = std::tuple_cat(input, action_lists);
        ctx->input_history = std::make_shared<decltype(input_state_history_impl(input_all))>();
    }

    if (auto *ctx = registry.try_ctx<server_network_context>()) {
        ctx->snapshot_importer.reset(new server_snapshot_importer_impl(all, actions));
        ctx->snapshot_exporter.reset(new server_snapshot_exporter_impl(all));
    }

    g_make_pool_snapshot_data = create_make_pool_snapshot_data_function(all);
    g_is_networked_component = internal::make_is_networked_component_func(all);
    g_is_networked_input_component = internal::make_is_network_input_component_func(all);
    g_is_action_list_component = internal::make_is_action_list_component_func(actions);
    g_mark_replaced_network_dirty = internal::make_mark_replaced_network_dirty_func(all);
}

/**
 * @brief Removes previously registered external networked components.
 * @param registry Data source.
 */
inline void unregister_networked_components(entt::registry &registry) {
    if (auto *ctx = registry.try_ctx<client_network_context>()) {
        ctx->snapshot_importer.reset(new client_snapshot_importer_impl(networked_components));
        ctx->snapshot_exporter.reset(new client_snapshot_exporter_impl(networked_components, {}));
        ctx->input_history = std::make_shared<input_state_history>();
    }

    if (auto *ctx = registry.try_ctx<server_network_context>()) {
        ctx->snapshot_importer.reset(new server_snapshot_importer_impl(networked_components, {}));
        ctx->snapshot_exporter.reset(new server_snapshot_exporter_impl(networked_components));
    }

    g_make_pool_snapshot_data = create_make_pool_snapshot_data_function(networked_components);
    g_is_networked_component = internal::make_is_networked_component_func(networked_components);
    g_is_networked_input_component = internal::make_is_network_input_component_func(networked_components);
    g_is_action_list_component = [](entt::id_type) { return false; }; // There are no native actions.
    g_mark_replaced_network_dirty = internal::make_mark_replaced_network_dirty_func(networked_components);
}

}

#endif // EDYN_NETWORKING_NETWORKING_EXTERNAL_HPP
