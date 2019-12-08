#include "edyn/dynamics/island_util.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/comp/constraint.hpp"
#include "edyn/comp/constraint_row.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include <entt/entt.hpp>

namespace edyn {

void wakeup_island(entt::entity island_ent, entt::registry &registry) {
    // Remove the `sleeping_tag` from all the entities associated with the
    // given island.
    registry.reset<sleeping_tag>(island_ent);

    auto &isle = registry.get<island>(island_ent);
    isle.sleep_step = UINT64_MAX;

    for (auto e : isle.entities) {
        registry.reset<sleeping_tag>(e);

        auto rel_con = registry.get<relation_container>(e);
        for (auto rel_ent : rel_con.entities) {
            registry.reset<sleeping_tag>(rel_ent);

            // If this relation has an associated constraint, also wake up all
            // its rows.
            auto con = registry.try_get<constraint>(rel_ent);
            if (con) {
                for (size_t i = 0; i < con->num_rows; ++i) {
                    registry.reset<sleeping_tag>(con->row[i]);
                }
            }
        }
    }
}

void put_islands_to_sleep(entt::registry &registry, uint64_t step, scalar dt) {
    auto vel_view = registry.view<dynamic_tag, linvel, angvel>(exclude_sleeping);
    auto island_view = registry.view<island>(exclude_sleeping);

    island_view.each([&] (auto ent, auto &isle) {
        // Check if there are any entities in this island moving faster than
        // the sleep threshold.
        bool sleep = true;
        for (auto e : isle.entities) {
            auto [v, w] = vel_view.get<linvel, angvel>(e);
            if (length2(v) > island_linear_sleep_threshold * island_linear_sleep_threshold || 
                length2(w) > island_angular_sleep_threshold * island_angular_sleep_threshold) {
                sleep = false;
                break;
            }
        }

        if (!sleep) {
            return;
        }

        // Put to sleep if the velocity of all entities in this island have
        // been under the threshold for some time.
        if (isle.sleep_step == UINT64_MAX) {
            isle.sleep_step = step;
        } else if ((step - isle.sleep_step) * dt > island_time_to_sleep) {
            registry.assign<sleeping_tag>(ent);

            // Assign `sleeping_tag` to all entities in this island and also
            // to all relations associated with them.
            for (auto e : isle.entities) {
                auto [v, w] = vel_view.get<linvel, angvel>(e);
                v = vector3_zero;
                w = vector3_zero;
                registry.assign<sleeping_tag>(e);

                auto rel_con = registry.get<relation_container>(e);
                for (auto rel_ent : rel_con.entities) {
                    registry.assign_or_replace<sleeping_tag>(rel_ent);

                    // If this relation has an associated constraint, assing a
                    // `sleeping_tag` to all its rows.
                    auto con = registry.try_get<constraint>(rel_ent);
                    if (con) {
                        for (size_t i = 0; i < con->num_rows; ++i) {
                            registry.assign_or_replace<sleeping_tag>(con->row[i]);
                        }
                    }
                }
            }
        }
    });
}

void island_on_construct_relation(entt::entity entity, entt::registry &registry, relation &rel) {
    // Allow the related entities to refer to their relations.
    registry.get_or_assign<relation_container>(rel.entity[0]).entities.push_back(entity);
    registry.get_or_assign<relation_container>(rel.entity[1]).entities.push_back(entity);

    // Find all islands involved in this new relation.
    std::vector<entt::entity> island_ents;

    for (auto ent : rel.entity) {
        auto node = registry.try_get<island_node>(ent);
        if (node && 
            std::find(island_ents.begin(), island_ents.end(), 
                      node->island_entity) == island_ents.end()) {
            island_ents.push_back(node->island_entity);
        }
    }

    // All entities are in the same island. Nothing needs to be done.
    if (island_ents.size() < 2) {
        if (!island_ents.empty()) {
            wakeup_island(island_ents[0], registry);
        }
        return;
    }

    // Merge all into one island.
    size_t biggest_idx = 0;
    size_t biggest_size = 0;

    for (size_t i = 0; i < island_ents.size(); ++i) {
        auto &isle = registry.get<island>(island_ents[i]);
        if (isle.entities.size() > biggest_size) {
            biggest_size = isle.entities.size();
            biggest_idx = i;
        }
    }

    auto biggest_ent = island_ents[biggest_idx];
    auto &biggest_isle = registry.get<island>(biggest_ent);

    for (size_t i = 0; i < island_ents.size(); ++i) {
        if (i != biggest_idx) {
            auto other_ent = island_ents[i];
            auto &other_isle = registry.get<island>(other_ent);

            for (auto ent : other_isle.entities) {
                biggest_isle.entities.push_back(ent);

                auto &node = registry.get<island_node>(ent);
                node.island_entity = biggest_ent;
            }
        }
    }

    // Destroy other islands after to ensure the `biggest_isle` reference won't
    // be invalidated during the previous loop.
    for (size_t i = 0; i < island_ents.size(); ++i) {
        if (i != biggest_idx) {
            registry.destroy(island_ents[i]);
        }
    }

    wakeup_island(biggest_ent, registry);
}

void island_on_destroy_relation(entt::entity entity, entt::registry &registry) {
    // Perform graph-walks using the entities in the destroyed relation as the
    // starting point (ignoring this destroyed relation, of course). Store the 
    // entities visited in each case in a set. If all sets are equal, it means
    // the island has not been broken and nothing needs to be done. If the sets
    // are different, destroy this island and create news islands for each set.
    // Update `island_node`s to point the new islands.

    auto &rel = registry.get<relation>(entity);

    // Remove the destroyed relation from the `relation_container` of the
    // related entities. Empty containers are removed at the end. This makes
    // it unecessary to check for existence in the next step.
    for (size_t i = 0; i < max_relations; ++i) {
        auto &container = registry.get<relation_container>(rel.entity[i]);
        auto it = std::find(container.entities.begin(), container.entities.end(), entity);
        std::swap(*it, *(container.entities.end() - 1));
        container.entities.pop_back();
    }

    // Store all entities found while walking the graph from each starting
    // point.
    std::array<std::vector<entt::entity>, max_relations> connected_entities;

    // Walk the graph starting from each entity in the destroyed relation.
    for (size_t i = 0; i < max_relations; ++i) {
        // Only dynamic entities matter since constraints do not affect static
        // and kinematic entities.
        if (!registry.has<dynamic_tag>(rel.entity[i])) {
            continue;
        }

        std::vector<entt::entity> visit_me;
        visit_me.push_back(rel.entity[i]);

        while (!visit_me.empty()) {
            auto ent = visit_me.back();
            visit_me.pop_back();
            auto found_it = std::find(connected_entities[i].begin(), 
                                      connected_entities[i].end(), ent);

            if (found_it == connected_entities[i].end()) {
                connected_entities[i].push_back(ent);
            } else {
                continue; // Already visited.
            }

            // Grab neighboring entities to visit.
            auto &container = registry.get<relation_container>(ent);

            for (auto rel_ent : container.entities) {
                auto &rel = registry.get<relation>(rel_ent);
                
                if (rel.entity[0] != ent && registry.has<dynamic_tag>(rel.entity[0])) {
                    visit_me.push_back(rel.entity[0]);
                }

                if (rel.entity[1] != ent && registry.has<dynamic_tag>(rel.entity[1])) {
                    visit_me.push_back(rel.entity[1]);
                }
            }
        }
    }

    // If the sets are different, split island.
    for (size_t i = 0; i < max_relations; ++i) {
        std::sort(connected_entities[i].begin(), connected_entities[i].end());
    }

    if (connected_entities[0] != connected_entities[1]) {
        // Remove entities from the biggest island and move them to a new
        // island.
        size_t bigger_idx = connected_entities[0].size() > connected_entities[1].size() ? 0 : 1;
        size_t smaller_idx = (bigger_idx + 1) % 2;
        auto &node = registry.get<island_node>(rel.entity[bigger_idx]);
        auto &isle = registry.get<island>(node.island_entity);
        // TODO: minor optimization: swap with last then pop.
        auto erase_it = std::remove_if(isle.entities.begin(), isle.entities.end(), [&] (auto &ent) {
            return std::find(connected_entities[smaller_idx].begin(), 
                             connected_entities[smaller_idx].end(), ent) 
                   != connected_entities[smaller_idx].end();
        });
        isle.entities.erase(erase_it, isle.entities.end());

        // Create new island for the second half.
        auto [other_island_ent, other_isle] = registry.create<island>();    
        other_isle.entities = std::move(connected_entities[smaller_idx]);

        // Update all nodes in the second set.
        for (auto ent : other_isle.entities) {
            auto &node = registry.get<island_node>(ent);
            node.island_entity = other_island_ent;
        }

        // Wake up both.
        wakeup_island(node.island_entity, registry);
        wakeup_island(other_island_ent, registry);
    } else {
        // Island survives. Wake everyone up though. They might have work to do
        // now.
        auto &node = registry.get<island_node>(rel.entity[0]);
        wakeup_island(node.island_entity, registry);
    }

    // Remove empty `relation_container`s.
    for (size_t i = 0; i < max_relations; ++i) {
        auto &container = registry.get<relation_container>(rel.entity[i]);
        if (container.entities.empty()) {
            registry.remove<relation_container>(rel.entity[i]);
        }
    }
}

}