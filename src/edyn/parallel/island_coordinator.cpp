#include "edyn/parallel/island_coordinator.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/comp/present_orientation.hpp"
#include "edyn/comp/present_position.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/shape_index.hpp"
#include "edyn/parallel/message.hpp"
#include "edyn/shapes/shapes.hpp"
#include "edyn/config/config.h"
#include "edyn/constraints/constraint.hpp"
#include "edyn/constraints/contact_constraint.hpp"
#include "edyn/constraints/constraint_impulse.hpp"
#include "edyn/parallel/island_delta.hpp"
#include "edyn/parallel/island_worker.hpp"
#include "edyn/comp/dirty.hpp"
#include "edyn/time/time.hpp"
#include "edyn/parallel/entity_graph.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/comp/graph_edge.hpp"
#include "edyn/util/vector.hpp"
#include "edyn/context/settings.hpp"
#include <entt/entity/registry.hpp>
#include <set>

namespace edyn {

island_coordinator::island_coordinator(entt::registry &registry)
    : m_registry(&registry)
{
    registry.on_construct<graph_node>().connect<&island_coordinator::on_construct_graph_node>(*this);
    registry.on_destroy<graph_node>().connect<&island_coordinator::on_destroy_graph_node>(*this);
    registry.on_construct<graph_edge>().connect<&island_coordinator::on_construct_graph_edge>(*this);
    registry.on_destroy<graph_edge>().connect<&island_coordinator::on_destroy_graph_edge>(*this);

    registry.on_destroy<island_resident>().connect<&island_coordinator::on_destroy_island_resident>(*this);
    registry.on_destroy<multi_island_resident>().connect<&island_coordinator::on_destroy_multi_island_resident>(*this);

    registry.on_destroy<contact_manifold>().connect<&island_coordinator::on_destroy_contact_manifold>(*this);
}

island_coordinator::~island_coordinator() {
    for (auto &pair : m_island_ctx_map) {
        auto &ctx = pair.second;
        ctx->terminate();
    }
}

void island_coordinator::on_construct_graph_node(entt::registry &registry, entt::entity entity) {
    if (m_importing_delta) return;

    m_new_graph_nodes.push_back(entity);

    if (registry.has<procedural_tag>(entity)) {
        registry.emplace<island_resident>(entity);
    } else {
        registry.emplace<multi_island_resident>(entity);
    }
}

void island_coordinator::on_construct_graph_edge(entt::registry &registry, entt::entity entity) {
    if (m_importing_delta) return;

    m_new_graph_edges.push_back(entity);
    // Assuming this graph edge is a constraint or contact manifold, which
    // are always procedural, thus can only reside in one island.
    registry.emplace<island_resident>(entity);
}

void island_coordinator::on_destroy_graph_node(entt::registry &registry, entt::entity entity) {
    auto &node = registry.get<graph_node>(entity);
    registry.ctx<entity_graph>().remove_node(node.node_index);
}

void island_coordinator::on_destroy_graph_edge(entt::registry &registry, entt::entity entity) {
    auto &edge = registry.get<graph_edge>(entity);
    registry.ctx<entity_graph>().remove_edge(edge.edge_index);
}

void island_coordinator::on_destroy_island_resident(entt::registry &registry, entt::entity entity) {
    auto &resident = registry.get<island_resident>(entity);

    // Remove from island.
    auto &ctx = m_island_ctx_map.at(resident.island_entity);
    ctx->m_nodes.erase(entity);
    ctx->m_edges.erase(entity);

    if (m_importing_delta) return;

    // When importing delta, the entity is removed from the entity map as part
    // of the import process. Otherwise, the removal has to be done here.
    if (ctx->m_entity_map.has_loc(entity)) {
        ctx->m_entity_map.erase_loc(entity);
    }

    // Notify the worker of the destruction which happened in the main registry
    // first.
    ctx->m_delta_builder->destroyed(entity);

    // Manually call these on_destroy functions since they could be triggered
    // by the EnTT delegate after the island resident is destroyed and the island
    // resident component is needed in these on_destroy functions.
    if (registry.has<contact_manifold>(entity)) {
        on_destroy_contact_manifold(registry, entity);
    }
}

void island_coordinator::on_destroy_multi_island_resident(entt::registry &registry, entt::entity entity) {
    auto &resident = registry.get<multi_island_resident>(entity);

    // Remove from islands.
    for (auto island_entity : resident.island_entities) {
        auto &ctx = m_island_ctx_map.at(island_entity);
        ctx->m_nodes.erase(entity);

        if (!m_importing_delta)  {
            ctx->m_delta_builder->destroyed(entity);
        }
    }
}

void island_coordinator::on_destroy_contact_manifold(entt::registry &registry, entt::entity entity) {
    if (m_importing_delta) return;
    if (!registry.has<island_resident>(entity)) return;

    auto &resident = registry.get<island_resident>(entity);
    auto &ctx = m_island_ctx_map.at(resident.island_entity);
    auto &manifold = registry.get<contact_manifold>(entity);
    auto num_points = manifold.num_points();

    for (size_t i = 0; i < num_points; ++i) {
        auto contact_entity = manifold.point[i];
        registry.destroy(contact_entity);
        ctx->m_delta_builder->destroyed(contact_entity);
    }
}

void island_coordinator::init_new_nodes_and_edges() {
    if (m_new_graph_nodes.empty() && m_new_graph_edges.empty()) return;

    auto &graph = m_registry->ctx<entity_graph>();
    auto node_view = m_registry->view<graph_node>();
    auto edge_view = m_registry->view<graph_edge>();
    std::set<entity_graph::index_type> procedural_node_indices;

    for (auto entity : m_new_graph_nodes) {
        if (m_registry->has<procedural_tag>(entity)) {
            auto &node = node_view.get(entity);
            procedural_node_indices.insert(node.node_index);
        } else {
            init_new_non_procedural_node(entity);
        }
    }

    for (auto edge_entity : m_new_graph_edges) {
        auto &edge = edge_view.get(edge_entity);
        auto node_entities = graph.edge_node_entities(edge.edge_index);

        if (m_registry->has<procedural_tag>(node_entities.first)) {
            auto &node = node_view.get(node_entities.first);
            procedural_node_indices.insert(node.node_index);
        }

        if (m_registry->has<procedural_tag>(node_entities.second)) {
            auto &node = node_view.get(node_entities.second);
            procedural_node_indices.insert(node.node_index);
        }
    }

    m_new_graph_nodes.clear();
    m_new_graph_edges.clear();

    if (procedural_node_indices.empty()) return;

    std::vector<entt::entity> connected_nodes;
    std::vector<entt::entity> connected_edges;
    std::vector<entt::entity> island_entities;
    auto resident_view = m_registry->view<island_resident>();
    auto procedural_view = m_registry->view<procedural_tag>();

    graph.reach(
        procedural_node_indices.begin(), procedural_node_indices.end(),
        [&] (entt::entity entity) { // visitNodeFunc
            // Always add non-procedurals to the connected component.
            // Only add procedural if it's not in an island yet.
            auto is_procedural = procedural_view.contains(entity);

            if (!is_procedural ||
                (is_procedural && resident_view.get(entity).island_entity == entt::null)) {
                connected_nodes.push_back(entity);
            }
        },
        [&] (entt::entity entity) { // visitEdgeFunc
            auto &edge_resident = resident_view.get(entity);

            if (edge_resident.island_entity == entt::null) {
                connected_edges.push_back(entity);
            } else {
                auto contains_island = vector_contains(island_entities, edge_resident.island_entity);

                if (!contains_island) {
                    island_entities.push_back(edge_resident.island_entity);
                }
            }
        },
        [&] (entity_graph::index_type node_index) { // shouldVisitFunc
            auto other_entity = graph.node_entity(node_index);

            // Collect islands involved in this connected component.
            // Always visit the non-procedural nodes. Their edges won't be
            // visited later because in the graph they're non-connecting nodes.
            if (!procedural_view.contains(other_entity)) {
                return true;
            }

            // Visit neighbor node if it's not in an island yet.
            auto &other_resident = resident_view.get(other_entity);

            if (other_resident.island_entity == entt::null) {
                return true;
            }

            auto contains_island = vector_contains(island_entities, other_resident.island_entity);

            if (!contains_island) {
                island_entities.push_back(other_resident.island_entity);
            }

            bool continue_visiting = false;

            // Visit neighbor if it contains an edge that is not in an island yet.
            graph.visit_edges(node_index, [&] (entt::entity edge_entity) {
                if (resident_view.get(edge_entity).island_entity == entt::null) {
                    continue_visiting = true;
                }
            });

            return continue_visiting;
        },
        [&] () { // connectedComponentFunc
            if (island_entities.empty()) {
                auto island_entity = create_island(m_timestamp, false);
                insert_to_island(island_entity, connected_nodes, connected_edges);
            } else if (island_entities.size() == 1) {
                auto island_entity = *island_entities.begin();
                insert_to_island(island_entity, connected_nodes, connected_edges);
            } else {
                merge_islands(island_entities, connected_nodes, connected_edges);
            }

            connected_nodes.clear();
            connected_edges.clear();
            island_entities.clear();
        });
}

void island_coordinator::init_new_non_procedural_node(entt::entity node_entity) {
    EDYN_ASSERT(!(m_registry->has<procedural_tag>(node_entity)));

    auto procedural_view = m_registry->view<procedural_tag>();
    auto resident_view = m_registry->view<island_resident>();
    auto &node = m_registry->get<graph_node>(node_entity);
    auto &resident = m_registry->get<multi_island_resident>(node_entity);

    // Add new non-procedural entity to islands of neighboring procedural entities.
    m_registry->ctx<entity_graph>().visit_neighbors(node.node_index, [&] (entt::entity other) {
        if (!procedural_view.contains(other)) return;

        auto &other_resident = resident_view.get(other);
        if (other_resident.island_entity == entt::null) return;

        auto &ctx = m_island_ctx_map.at(other_resident.island_entity);
        ctx->m_nodes.insert(node_entity);

        if (!resident.island_entities.count(other_resident.island_entity)) {
            resident.island_entities.insert(other_resident.island_entity);
            ctx->m_delta_builder->created(node_entity);
            ctx->m_delta_builder->created_all(node_entity, *m_registry);
        }
    });
}

entt::entity island_coordinator::create_island(double timestamp, bool sleeping) {
    auto island_entity = m_registry->create();
    m_registry->emplace<island>(island_entity);
    auto &isle_time = m_registry->emplace<island_timestamp>(island_entity);
    isle_time.value = timestamp;

    auto [main_queue_input, main_queue_output] = make_message_queue_input_output();
    auto [isle_queue_input, isle_queue_output] = make_message_queue_input_output();

    // The `island_worker` is dynamically allocated and kept alive while
    // the associated island lives. The job that's created for it calls its
    // `update` function which reschedules itself to be run over and over again.
    // After the `finish` function is called on it (when the island is destroyed),
    // it will be deallocated on the next run.
    auto &settings = m_registry->ctx<edyn::settings>();
    auto *worker = new island_worker(island_entity, settings,
                                     message_queue_in_out(main_queue_input, isle_queue_output));
    auto ctx = std::make_unique<island_worker_context>(island_entity, worker,
                                                       (*settings.make_island_delta_builder)(),
                                                       message_queue_in_out(isle_queue_input, main_queue_output));

    // Insert the first entity mapping between the remote island entity and
    // the local island entity.
    ctx->m_entity_map.insert(worker->island_entity(), island_entity);

    // Register to receive delta.
    ctx->island_delta_sink().connect<&island_coordinator::on_island_delta>(*this);
    ctx->split_island_sink().connect<&island_coordinator::on_split_island>(*this);

    // Send over a delta containing this island entity to the island worker
    // before it even starts.
    auto builder = (*settings.make_island_delta_builder)();
    builder->created(island_entity, isle_time);

    if (sleeping) {
        m_registry->emplace<sleeping_tag>(island_entity);
        builder->created(island_entity, sleeping_tag{});
    }

    ctx->send<island_delta>(builder->finish());

    m_island_ctx_map.emplace(island_entity, std::move(ctx));

    return island_entity;
}

void island_coordinator::insert_to_island(entt::entity island_entity,
                                          const std::vector<entt::entity> &nodes,
                                          const std::vector<entt::entity> &edges) {

    auto &ctx = m_island_ctx_map.at(island_entity);
    ctx->m_nodes.insert(nodes.begin(), nodes.end());
    ctx->m_edges.insert(edges.begin(), edges.end());

    auto resident_view = m_registry->view<island_resident>();
    auto multi_resident_view = m_registry->view<multi_island_resident>();
    auto manifold_view = m_registry->view<contact_manifold>();
    auto cp_view = m_registry->view<contact_point>();
    auto contact_view = m_registry->view<contact_constraint>();
    auto impulse_view = m_registry->view<constraint_impulse>();

    // Calculate total number of certain kinds of entities to later reserve
    // the expected number of components for better performance.
    size_t total_num_points = 0;
    size_t total_num_constraints = 0;

    for (auto entity : edges) {
        if (manifold_view.contains(entity)) {
            auto &manifold = manifold_view.get(entity);
            total_num_points += manifold.num_points();
            total_num_constraints += manifold.num_points();
        } else {
            total_num_constraints += 1;
        }
    }

    ctx->m_delta_builder->reserve_created(nodes.size() + edges.size() + total_num_points);
    ctx->m_delta_builder->reserve_created<contact_constraint>(total_num_points);
    ctx->m_delta_builder->reserve_created<contact_point>(total_num_points);
    ctx->m_delta_builder->reserve_created<constraint_impulse>(total_num_constraints);
    ctx->m_delta_builder->reserve_created<position, orientation, linvel, angvel, continuous>(nodes.size());
    ctx->m_delta_builder->reserve_created<mass, mass_inv, inertia, inertia_inv, inertia_world_inv>(nodes.size());

    // Insert created/updated components one-by-one explicitly. Could use
    // created_all/updated_all but this is more efficient.
    auto tr_view = m_registry->view<position, orientation>();
    auto vel_view = m_registry->view<linvel, angvel>();
    auto mass_view = m_registry->view<mass, mass_inv, inertia, inertia_inv, inertia_world_inv>();
    auto acc_view = m_registry->view<linacc>();
    auto material_view = m_registry->view<material>();
    auto continuous_view = m_registry->view<continuous>();
    auto procedural_view = m_registry->view<procedural_tag>();
    auto static_view = m_registry->view<static_tag>();
    auto continuous_contacts_view = m_registry->view<continuous_contacts_tag>();
    auto collision_view = m_registry->view<shape_index, AABB, collision_filter>();
    auto shape_views_tuple = get_tuple_of_shape_views(*m_registry);

    for (auto entity : nodes) {
        if (procedural_view.contains(entity)) {
            auto &resident = resident_view.get(entity);
            resident.island_entity = island_entity;

            ctx->m_delta_builder->created(entity);

            ctx->m_delta_builder->created(entity, tr_view.get<position>(entity));
            ctx->m_delta_builder->created(entity, tr_view.get<orientation>(entity));
            ctx->m_delta_builder->created(entity, vel_view.get<linvel>(entity));
            ctx->m_delta_builder->created(entity, vel_view.get<angvel>(entity));

            ctx->m_delta_builder->created(entity, mass_view.get<mass>(entity));
            ctx->m_delta_builder->created(entity, mass_view.get<mass_inv>(entity));
            ctx->m_delta_builder->created(entity, mass_view.get<inertia>(entity));
            ctx->m_delta_builder->created(entity, mass_view.get<inertia_inv>(entity));
            ctx->m_delta_builder->created(entity, mass_view.get<inertia_world_inv>(entity));

            if (acc_view.contains(entity)) {
                ctx->m_delta_builder->created(entity, acc_view.get(entity));
            }

            if (material_view.contains(entity)) {
                ctx->m_delta_builder->created(entity, material_view.get(entity));
            }

            if (collision_view.contains(entity)) {
                auto sh_idx = collision_view.get<shape_index>(entity);
                ctx->m_delta_builder->created(entity, sh_idx);

                visit_shape(sh_idx, entity, shape_views_tuple, [&] (auto &&shape) {
                    ctx->m_delta_builder->created(entity, shape);
                });

                ctx->m_delta_builder->created(entity, collision_view.get<AABB>(entity));
                ctx->m_delta_builder->created(entity, collision_view.get<collision_filter>(entity));
            }

            ctx->m_delta_builder->created(entity, dynamic_tag{});
            ctx->m_delta_builder->created(entity, procedural_tag{});
            ctx->m_delta_builder->created(entity, continuous_view.get(entity));

            ctx->m_delta_builder->created_external(entity, *m_registry);
        } else {
            auto &resident = multi_resident_view.get(entity);

            if (resident.island_entities.count(island_entity) == 0) {
                // Non-procedural entity is not yet in this island, thus create it.
                resident.island_entities.insert(island_entity);

                ctx->m_delta_builder->created(entity);

                ctx->m_delta_builder->created(entity, tr_view.get<position>(entity));
                ctx->m_delta_builder->created(entity, tr_view.get<orientation>(entity));
                ctx->m_delta_builder->created(entity, vel_view.get<linvel>(entity));
                ctx->m_delta_builder->created(entity, vel_view.get<angvel>(entity));

                ctx->m_delta_builder->created(entity, mass_view.get<mass>(entity));
                ctx->m_delta_builder->created(entity, mass_view.get<mass_inv>(entity));
                ctx->m_delta_builder->created(entity, mass_view.get<inertia>(entity));
                ctx->m_delta_builder->created(entity, mass_view.get<inertia_inv>(entity));
                ctx->m_delta_builder->created(entity, mass_view.get<inertia_world_inv>(entity));

                if (material_view.contains(entity)) {
                    ctx->m_delta_builder->created(entity, material_view.get(entity));
                }

                if (collision_view.contains(entity)) {
                    auto sh_idx = collision_view.get<shape_index>(entity);
                    ctx->m_delta_builder->created(entity, sh_idx);

                    visit_shape(sh_idx, entity, shape_views_tuple, [&] (auto &&shape) {
                        ctx->m_delta_builder->created(entity, shape);
                    });

                    ctx->m_delta_builder->created(entity, collision_view.get<AABB>(entity));
                    ctx->m_delta_builder->created(entity, collision_view.get<collision_filter>(entity));
                }

                if (static_view.contains(entity)) {
                    ctx->m_delta_builder->created(entity, static_tag{});
                } else {
                    ctx->m_delta_builder->created(entity, kinematic_tag{});
                }

                ctx->m_delta_builder->created_external(entity, *m_registry);
            } else if (!static_view.contains(entity)) {
                // Non-procedural entity is already in this island.
                // If kinematic, update transform and velocity.
                ctx->m_delta_builder->updated(entity, tr_view.get<position>(entity));
                ctx->m_delta_builder->updated(entity, tr_view.get<orientation>(entity));
                ctx->m_delta_builder->updated(entity, vel_view.get<linvel>(entity));
                ctx->m_delta_builder->updated(entity, vel_view.get<angvel>(entity));

                // Also update external components.
                ctx->m_delta_builder->updated_external(entity, *m_registry);
            }
        }

        if (continuous_contacts_view.contains(entity)) {
            ctx->m_delta_builder->created(entity, continuous_contacts_tag{});
        }
    }

    for (auto entity : edges) {
        // Assign island to residents. All edges are procedural, thus having an
        // `island_resident`, which refers to a single island.
        auto &resident = resident_view.get(entity);
        resident.island_entity = island_entity;
        // Add new entities to the delta builder.
        ctx->m_delta_builder->created(entity);

        // Add child entities.
        if (manifold_view.contains(entity)) {
            auto &manifold = manifold_view.get(entity);
            ctx->m_delta_builder->created(entity, manifold);
            ctx->m_delta_builder->created<procedural_tag>(entity, *m_registry);

            auto num_points = manifold.num_points();

            for (size_t i = 0; i < num_points; ++i) {
                auto point_entity = manifold.point[i];
                auto &point_resident = resident_view.get(point_entity);
                point_resident.island_entity = island_entity;

                auto point = cp_view.get(point_entity);
                ctx->m_delta_builder->created(point_entity);
                ctx->m_delta_builder->created(point_entity, point);

                if (contact_view.contains(point_entity)) {
                    ctx->m_delta_builder->created(point_entity, contact_view.get(point_entity));
                    ctx->m_delta_builder->created(point_entity, impulse_view.get(point_entity));
                }

                if (continuous_view.contains(point_entity)) {
                    ctx->m_delta_builder->created(point_entity, continuous_view.get(point_entity));
                }
            }
        } else {
            std::apply([&] (auto ... c) {
                ((m_registry->has<decltype(c)>(entity) ?
                    ctx->m_delta_builder->created(entity, m_registry->get<decltype(c)>(entity)) :
                    void(0)), ...);
            }, constraints_tuple);

            ctx->m_delta_builder->created<procedural_tag>(entity, *m_registry);
            ctx->m_delta_builder->created(entity, impulse_view.get(entity));
        }

        ctx->m_delta_builder->created_external(entity, *m_registry);
    }
}

entt::entity island_coordinator::merge_islands(const std::vector<entt::entity> &island_entities,
                                               const std::vector<entt::entity> &new_nodes,
                                               const std::vector<entt::entity> &new_edges) {
    EDYN_ASSERT(island_entities.size() > 1);

    // Pick biggest island and move the other entities into it.
    entt::entity island_entity;
    size_t biggest_size = 0;

    for (auto entity : island_entities) {
        auto &ctx = m_island_ctx_map.at(entity);
        auto size = ctx->m_nodes.size() + ctx->m_edges.size();

        if (size > biggest_size) {
            biggest_size = size;
            island_entity = entity;
        }
    }

    auto other_island_entities = island_entities;
    vector_erase(other_island_entities, island_entity);

    auto all_nodes = new_nodes;
    auto all_edges = new_edges;

    for (auto other_island_entity : other_island_entities) {
        auto &ctx = m_island_ctx_map.at(other_island_entity);
        all_nodes.insert(all_nodes.end(), ctx->m_nodes.begin(), ctx->m_nodes.end());
        all_edges.insert(all_edges.end(), ctx->m_edges.begin(), ctx->m_edges.end());
    }

    auto multi_resident_view = m_registry->view<multi_island_resident>();

    for (auto entity : all_nodes) {
        // Entity might be coming from a sleeping island. Remove `sleeping_tag`s
        // since the island is supposed to be awake after a merge.
        m_registry->remove_if_exists<sleeping_tag>(entity);

        // Remove islands to be destroyed from multi island residents.
        if (multi_resident_view.contains(entity)) {
            auto &resident = multi_resident_view.get(entity);

            for (auto other_island_entity : other_island_entities) {
                resident.island_entities.erase(other_island_entity);
            }
        }
    }

    for (auto entity : all_edges) {
        m_registry->remove_if_exists<sleeping_tag>(entity);

        if (auto *manifold = m_registry->try_get<contact_manifold>(entity)) {
            auto num_points = manifold->num_points();

            for (size_t i = 0; i < num_points; ++i) {
                auto contact_entity = manifold->point[i];
                m_registry->remove_if_exists<sleeping_tag>(contact_entity);
            }
        }
    }

    insert_to_island(island_entity, all_nodes, all_edges);

    // Destroy empty islands.
    for (auto other_island_entity : other_island_entities) {
        auto &ctx = m_island_ctx_map.at(other_island_entity);
        ctx->terminate();
        m_island_ctx_map.erase(other_island_entity);
        m_registry->destroy(other_island_entity);
    }

    // Prevents glitch where entities are moved into an island that was sleeping
    // and thus its timestamp is outdated.
    if (m_registry->has<sleeping_tag>(island_entity)) {
        auto &isle_timestamp = m_registry->get<island_timestamp>(island_entity);
        isle_timestamp.value = m_timestamp;
    }

    return island_entity;
}

void island_coordinator::create_island(std::vector<entt::entity> nodes, bool sleeping) {
#if EDYN_DEBUG && !EDYN_DISABLE_ASSERT
    for (auto entity : nodes) {
        EDYN_ASSERT(m_registry->has<graph_node>(entity));
    }
#endif

    auto timestamp = (double)performance_counter() / (double)performance_frequency();
    auto island_entity = create_island(timestamp, sleeping);
    insert_to_island(island_entity, nodes, {});
}

void island_coordinator::refresh_dirty_entities() {
    auto dirty_view = m_registry->view<dirty>();
    auto resident_view = m_registry->view<island_resident>();
    auto multi_resident_view = m_registry->view<multi_island_resident>();

    auto refresh = [this] (entt::entity entity, dirty &dirty, entt::entity island_entity) {
        auto &ctx = m_island_ctx_map.at(island_entity);
        auto &builder = ctx->m_delta_builder;

        if (dirty.is_new_entity) {
            builder->created(entity);
        }

        builder->created(entity, *m_registry,
            dirty.created_indexes.begin(), dirty.created_indexes.end());
        builder->updated(entity, *m_registry,
            dirty.updated_indexes.begin(), dirty.updated_indexes.end());
        builder->destroyed(entity,
            dirty.destroyed_indexes.begin(), dirty.destroyed_indexes.end());
    };

    dirty_view.each([&] (entt::entity entity, dirty &dirty) {
        if (resident_view.contains(entity)) {
            refresh(entity, dirty, resident_view.get(entity).island_entity);
        } else if (multi_resident_view.contains(entity)) {
            auto &resident = multi_resident_view.get(entity);
            for (auto island_entity : resident.island_entities) {
                refresh(entity, dirty, island_entity);
            }
        }
    });

    m_registry->clear<dirty>();
}

void island_coordinator::on_island_delta(entt::entity source_island_entity, const island_delta &delta) {
    m_importing_delta = true;
    auto &source_ctx = m_island_ctx_map.at(source_island_entity);
    delta.import(*m_registry, source_ctx->m_entity_map);

    // Insert entity mappings for new entities into the current delta.
    for (auto remote_entity : delta.created_entities()) {
        if (!source_ctx->m_entity_map.has_rem(remote_entity)) continue;
        auto local_entity = source_ctx->m_entity_map.remloc(remote_entity);
        source_ctx->m_delta_builder->insert_entity_mapping(remote_entity, local_entity);
    }

    auto procedural_view = m_registry->view<procedural_tag>();
    auto node_view = m_registry->view<graph_node>();

    // Insert nodes in the graph for each rigid body.
    auto &graph = m_registry->ctx<entity_graph>();
    auto &index_source = source_ctx->m_delta_builder->get_index_source();
    auto insert_node = [&] (entt::entity remote_entity, auto &) {
        if (!source_ctx->m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = source_ctx->m_entity_map.remloc(remote_entity);
        auto non_connecting = !m_registry->has<procedural_tag>(local_entity);
        auto node_index = graph.insert_node(local_entity, non_connecting);
        m_registry->emplace<graph_node>(local_entity, node_index);

        if (procedural_view.contains(local_entity)) {
            m_registry->emplace<island_resident>(local_entity, source_island_entity);
        } else {
            auto &resident = m_registry->emplace<multi_island_resident>(local_entity);
            resident.island_entities.insert(source_island_entity);
        }

        source_ctx->m_nodes.insert(local_entity);
    };

    delta.created_for_each<dynamic_tag>(index_source, insert_node);
    delta.created_for_each<static_tag>(index_source, insert_node);
    delta.created_for_each<kinematic_tag>(index_source, insert_node);

    auto assign_island_to_contact_points = [&] (const contact_manifold &manifold) {
        auto num_points = manifold.num_points();

        for (size_t i = 0; i < num_points; ++i) {
            auto point_entity = manifold.point[i];

            if (m_registry->valid(point_entity) && !m_registry->has<island_resident>(point_entity)) {
                m_registry->emplace<island_resident>(point_entity, source_island_entity);
            }
        }
    };

    // Insert edges in the graph for contact manifolds.
    delta.created_for_each<contact_manifold>(index_source, [&] (entt::entity remote_entity, const contact_manifold &manifold) {
        if (!source_ctx->m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = source_ctx->m_entity_map.remloc(remote_entity);
        auto &node0 = node_view.get(manifold.body[0]);
        auto &node1 = node_view.get(manifold.body[1]);
        auto edge_index = graph.insert_edge(local_entity, node0.node_index, node1.node_index);
        m_registry->emplace<graph_edge>(local_entity, edge_index);
        m_registry->emplace<island_resident>(local_entity, source_island_entity);
        source_ctx->m_edges.insert(local_entity);

        assign_island_to_contact_points(manifold);
    });

    // Insert edges in the graph for constraints (except contact constraints).
    delta.created_for_each(constraints_tuple, index_source, [&] (entt::entity remote_entity, const auto &con) {
        // Contact constraints are not added as edges to the graph.
        // The contact manifold which owns them is added instead.
        if constexpr(std::is_same_v<std::decay_t<decltype(con)>, contact_constraint>) return;

        if (!source_ctx->m_entity_map.has_rem(remote_entity)) return;

        auto local_entity = source_ctx->m_entity_map.remloc(remote_entity);
        auto &node0 = node_view.get(con.body[0]);
        auto &node1 = node_view.get(con.body[1]);
        auto edge_index = graph.insert_edge(local_entity, node0.node_index, node1.node_index);
        m_registry->emplace<graph_edge>(local_entity, edge_index);
        m_registry->emplace<island_resident>(local_entity, source_island_entity);
        source_ctx->m_edges.insert(local_entity);
    });

    delta.updated_for_each<contact_manifold>(index_source, [&] (entt::entity, const contact_manifold &manifold) {
        assign_island_to_contact_points(manifold);
    });

    m_importing_delta = false;
}

void island_coordinator::on_split_island(entt::entity source_island_entity, const msg::split_island &) {
    m_islands_to_split.push_back(source_island_entity);
}

void island_coordinator::split_islands() {
    for (auto island_entity : m_islands_to_split) {
        split_island(island_entity);
    }
    m_islands_to_split.clear();
}

void island_coordinator::split_island(entt::entity split_island_entity) {
    if (m_island_ctx_map.count(split_island_entity) == 0) return;

    auto &ctx = m_island_ctx_map.at(split_island_entity);
    auto connected_components = ctx->split();

    if (connected_components.size() <= 1) return;

    // Process any new messages enqueued during the split, such as created
    // entities that need to have their entity mappings added and the
    // update AABB `tree_view` of this island, which removes entities that
    // have moved due to the split.
    ctx->read_messages();

    // Map entities to the coordinator space.
    for (auto &connected_component : connected_components) {
        for (auto &entity : connected_component.nodes) {
            entity = ctx->m_entity_map.remloc(entity);
        }

        for (auto &entity : connected_component.edges) {
            entity = ctx->m_entity_map.remloc(entity);
        }
    }

    auto timestamp = m_registry->get<island_timestamp>(split_island_entity).value;
    bool sleeping = m_registry->has<sleeping_tag>(split_island_entity);
    auto multi_resident_view = m_registry->view<multi_island_resident>();
    auto procedural_view = m_registry->view<procedural_tag>();

    // Collect non-procedural entities that are still in the island that was split.
    // The first connected component in the array is the one left in the island
    // that was split.
    auto &source_connected_component = connected_components.front();
    std::vector<entt::entity> remaining_non_procedural_entities;

    for (auto entity : source_connected_component.nodes) {
        if (!procedural_view.contains(entity)) {
            remaining_non_procedural_entities.push_back(entity);
        }
    }

    for (size_t i = 1; i < connected_components.size(); ++i) {
        auto &connected = connected_components[i];
        bool contains_procedural = false;

        for (auto entity : connected.nodes) {
            if (procedural_view.contains(entity)) {
                contains_procedural = true;
                ctx->m_nodes.erase(entity);
            } else if (!vector_contains(remaining_non_procedural_entities, entity)) {
                // Remove island that was split from multi-residents if they're not
                // present in the source island.
                auto &resident = multi_resident_view.get(entity);
                resident.island_entities.erase(split_island_entity);
                ctx->m_nodes.erase(entity);
            }
        }

        for (auto entity : connected.edges) {
            ctx->m_edges.erase(entity);
        }

        // Do not create a new island if this connected component does not
        // contain any procedural node.
        if (!contains_procedural) continue;

        auto island_entity = create_island(timestamp, sleeping);
        insert_to_island(island_entity, connected.nodes, connected.edges);
    }
}

void island_coordinator::sync() {
    for (auto &pair : m_island_ctx_map) {
        auto island_entity = pair.first;
        auto &ctx = pair.second;

        if (!ctx->delta_empty()) {
            auto needs_wakeup = ctx->delta_needs_wakeup();
            ctx->send_delta();

            if (needs_wakeup && m_registry->has<sleeping_tag>(island_entity)) {
                ctx->send<msg::wake_up_island>();
            }
        }

        ctx->flush();
    }
}

void island_coordinator::update() {
    m_timestamp = (double)performance_counter() / (double)performance_frequency();

    for (auto &pair : m_island_ctx_map) {
        pair.second->read_messages();
    }

    init_new_nodes_and_edges();
    refresh_dirty_entities();
    sync();
    split_islands();
}

void island_coordinator::set_paused(bool paused) {
    for (auto &pair : m_island_ctx_map) {
        auto &ctx = pair.second;
        ctx->send<msg::set_paused>(paused);
    }
}

void island_coordinator::step_simulation() {
    for (auto &pair : m_island_ctx_map) {
        auto island_entity = pair.first;
        if (m_registry->has<sleeping_tag>(island_entity)) continue;

        auto &ctx = pair.second;
        ctx->send<msg::step_simulation>();
    }
}

void island_coordinator::set_fixed_dt(scalar dt) {
    for (auto &pair : m_island_ctx_map) {
        auto &ctx = pair.second;
        ctx->send<msg::set_fixed_dt>(dt);
    }
}

void island_coordinator::settings_changed() {
    auto &settings = m_registry->ctx<edyn::settings>();

    for (auto &pair : m_island_ctx_map) {
        auto &ctx = pair.second;
        ctx->send<msg::set_settings>(settings);
    }
}

}
