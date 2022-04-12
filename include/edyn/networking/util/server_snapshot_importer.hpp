#ifndef EDYN_NETWORKING_UTIL_SERVER_SNAPSHOT_IMPORTER_HPP
#define EDYN_NETWORKING_UTIL_SERVER_SNAPSHOT_IMPORTER_HPP

#include <entt/entity/registry.hpp>
#include <type_traits>
#include "edyn/networking/comp/network_dirty.hpp"
#include "edyn/networking/comp/network_input.hpp"
#include "edyn/networking/util/pool_snapshot.hpp"
#include "edyn/networking/comp/remote_client.hpp"
#include "edyn/networking/comp/entity_owner.hpp"
#include "edyn/networking/packet/registry_snapshot.hpp"
#include "edyn/parallel/map_child_entity.hpp"
#include "edyn/edyn.hpp"

namespace edyn {

extern bool(*g_is_networked_input_component)(entt::id_type);
bool is_fully_owned_by_client(const entt::registry &registry, entt::entity client_entity, entt::entity entity);

class server_snapshot_importer {
public:
    virtual ~server_snapshot_importer() = default;

    // Import components in pool into registry. If the island where the entity
    // resides is not fully owned by the given client, the update won't be applied.
    // Input components of entities owned by the client are always applied.
    virtual void import(entt::registry &registry, entt::entity client_entity,
                        const packet::registry_snapshot &snap, bool check_ownership) = 0;

    // Import input components of a pool containing local entities.
    virtual void import_input_local(entt::registry &registry, entt::entity client_entity,
                                    const packet::registry_snapshot &snap, double time) = 0;

    // Transform contained entities from remote to local using the remote client's entity map.
    virtual void transform_to_local(const entt::registry &registry, entt::entity client_entity,
                                    packet::registry_snapshot &snap, bool check_ownership) = 0;
};

template<typename... Components>
class server_snapshot_importer_impl : public server_snapshot_importer {

    template<typename Component>
    bool is_owned_by_client(const entt::registry &registry, entt::entity client_entity, entt::entity local_entity) {
        // If the entity is not fully owned by the client, the update must
        // not be applied, because in this case the server is in control of
        // the procedural state. Input components are one exception because
        // they must always be applied.
        if constexpr(std::is_base_of_v<network_input, Component>) {
            if (auto *owner = registry.try_get<entity_owner>(local_entity);
                owner && owner->client_entity == client_entity)
            {
                return true;
            }
        } else if (is_fully_owned_by_client(registry, client_entity, local_entity)) {
            return true;
        }

        return false;
    }

    template<typename Component>
    void import_components(entt::registry &registry, entt::entity client_entity,
                           const std::vector<entt::entity> &pool_entities,
                           const pool_snapshot_data_impl<Component> &pool,
                           bool check_ownership) {
        auto &client = registry.get<remote_client>(client_entity);

        for (size_t i = 0; i < pool.entity_indices.size(); ++i) {
            auto entity_index = pool.entity_indices[i];
            auto remote_entity = pool_entities[entity_index];

            if (!client.entity_map.contains(remote_entity)) {
                continue;
            }

            auto local_entity = client.entity_map.at(remote_entity);

            if (!registry.valid(local_entity)) {
                client.entity_map.erase(remote_entity);
                continue;
            }

            if (check_ownership && !is_owned_by_client<Component>(registry, client_entity, local_entity)) {
                continue;
            }

            if constexpr(std::is_empty_v<Component>) {
                if (!registry.any_of<Component>(local_entity)) {
                    registry.emplace<Component>(local_entity);
                }
            } else {
                auto comp = pool.components[i];
                internal::map_child_entity(registry, client.entity_map, comp);

                if (registry.any_of<Component>(local_entity)) {
                    registry.replace<Component>(local_entity, comp);
                } else {
                    registry.emplace<Component>(local_entity, comp);
                }
            }
        }
    }

    template<typename Component>
    void import_input_components_local(entt::registry &registry, entt::entity client_entity,
                                       const std::vector<entt::entity> &pool_entities,
                                       const pool_snapshot_data_impl<Component> &pool,
                                       double time) {
        auto owner_view = registry.view<entity_owner>();

        for (size_t i = 0; i < pool.entity_indices.size(); ++i) {
            auto entity_index = pool.entity_indices[i];
            auto local_entity = pool_entities[entity_index];

            if (!registry.valid(local_entity)) {
                continue;
            }

            // Entity must be owned by client.
            if (!owner_view.contains(local_entity) ||
                std::get<0>(owner_view.get(local_entity)).client_entity != client_entity)
            {
                continue;
            }

            auto &n_dirty = registry.get_or_emplace<network_dirty>(local_entity);
            n_dirty.insert(entt::type_index<Component>::value(), time);

            if constexpr(std::is_empty_v<Component>) {
                if (!registry.any_of<Component>(local_entity)) {
                    registry.emplace<Component>(local_entity);
                }
            } else {
                auto &comp = pool.components[i];

                if (registry.any_of<Component>(local_entity)) {
                    registry.replace<Component>(local_entity, comp);
                } else {
                    registry.emplace<Component>(local_entity, comp);
                }
            }
        }
    }

    template<typename Component>
    void transform_components_to_local(const entt::registry &registry, entt::entity client_entity,
                                       const std::vector<entt::entity> &pool_entities,
                                       pool_snapshot_data_impl<Component> &pool,
                                       bool check_ownership) {
        auto &client = registry.get<remote_client>(client_entity);

        auto remove = [&] (size_t i) {
            pool.entity_indices[i] = pool.entity_indices.back(), pool.entity_indices.pop_back();
            pool.components[i] = pool.components.back(), pool.components.pop_back();
        };

        // Do not increment `i` here because items will be removed by swapping
        // with last and popping.
        for (size_t i = 0; i < pool.entity_indices.size();) {
            auto entity_index = pool.entity_indices[i];
            auto entity = pool_entities[entity_index];

            if (!registry.valid(entity)) {
                remove(i);
                continue;
            }

            if (check_ownership && !is_owned_by_client<Component>(registry, client_entity, entity)) {
                remove(i);
                continue;
            }

            auto &comp = pool.components[i];
            internal::map_child_entity(registry, client.entity_map, comp);

            ++i;
        }
    }

public:
    server_snapshot_importer_impl([[maybe_unused]] std::tuple<Components...>) {}

    void import(entt::registry &registry, entt::entity client_entity,
                const packet::registry_snapshot &snap, bool check_ownership) override {
        const std::tuple<Components...> all_components;

        for (auto &pool : snap.pools) {
            visit_tuple(all_components, pool.component_index, [&] (auto &&c) {
                using CompType = std::decay_t<decltype(c)>;
                auto *typed_pool = static_cast<pool_snapshot_data_impl<CompType> *>(pool.ptr.get());
                import_components(registry, client_entity, snap.entities, *typed_pool, check_ownership);
            });
        }
    }

    void import_input_local(entt::registry &registry, entt::entity client_entity,
                            const packet::registry_snapshot &snap, double time) override {
        const std::tuple<Components...> all_components;

        for (auto &pool : snap.pools) {
            visit_tuple(all_components, pool.component_index, [&] (auto &&c) {
                using CompType = std::decay_t<decltype(c)>;

                if ((*g_is_networked_input_component)(entt::type_index<CompType>::value())) {
                    auto *typed_pool = static_cast<pool_snapshot_data_impl<CompType> *>(pool.ptr.get());
                    import_input_components_local(registry, client_entity, snap.entities, *typed_pool, time);
                }
            });
        }
    }

    void transform_to_local(const entt::registry &registry, entt::entity client_entity,
                            packet::registry_snapshot &snap, bool check_ownership) override {
        const std::tuple<Components...> all_components;

        for (auto &pool : snap.pools) {
            visit_tuple(all_components, pool.component_index, [&] (auto &&c) {
                using CompType = std::decay_t<decltype(c)>;

                if constexpr(!std::is_empty_v<CompType>) {
                    auto *typed_pool = static_cast<pool_snapshot_data_impl<CompType> *>(pool.ptr.get());
                    transform_components_to_local(registry, client_entity, snap.entities, *typed_pool, check_ownership);
                }
            });
        }
    }
};

}

#endif // EDYN_NETWORKING_UTIL_SERVER_SNAPSHOT_IMPORTER_HPP
