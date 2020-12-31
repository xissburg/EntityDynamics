#ifndef EDYN_COMP_DIRTY_HPP
#define EDYN_COMP_DIRTY_HPP

#include <unordered_set>
#include <entt/core/type_info.hpp>

namespace edyn {

/**
 * @brief Marks an entity as dirty, consequently scheduling them for a refresh
 *      in the other end, i.e. from island worker to island coordinator and
 *      vice versa. These components are consumed by an island worker or 
 *      coordinator in their update, i.e. they get processed and deleted right 
 *      after.
 */
struct dirty {
    // If the entity was just created, this flag must be set.
    bool is_new_entity {false};

    using index_set_t = std::unordered_set<entt::id_type>;

    index_set_t created_indexes;
    index_set_t updated_indexes;
    index_set_t destroyed_indexes;
    entity_set island_entities;

    /**
     * @brief Marks the given components as created.
     * @tparam Ts The created component types.
     * @return This object.
     */
    template<typename... Ts>
    dirty & created() {
        return cud<Ts...>(&dirty::created_indexes);
    }

    /**
     * @brief Marks the given components as updated.
     * @tparam Ts The updated component types.
     * @return This object.
     */
    template<typename... Ts>
    dirty & updated() {
        return cud<Ts...>(&dirty::updated_indexes);
    }

    /**
     * @brief Marks the given components as destroyed.
     * @tparam Ts The destroyed component types.
     * @return This object.
     */
    template<typename... Ts>
    dirty & destroyed() {
        return cud<Ts...>(&dirty::destroyed_indexes);
    }

    /**
     * @brief Marks the owner as a newly created entity.
     * @return This object.
     */
    dirty & set_new() {
        is_new_entity = true;
        return *this;
    }

    /**
     * @brief Limits the updates to select islands.
     * @param island_entity One island that should receive this update.
     * @return This object.
     */
    dirty & islands(entt::entity island_entity) {
        island_entities.insert(island_entity);
        return *this;
    }

    /**
     * @brief Limits the updates to select islands.
     * @param first An iterator to the first element in the collection.
     * @param last An iterator to the last element in the collection.
     * @return This object.
     */
    template<typename Iterator>
    dirty & islands(Iterator first, Iterator last) {
        island_entities.insert(first, last);
        return *this;
    }

private:
    // CUD: Create, Update, Delete.
    template<typename... Ts>
    dirty & cud(index_set_t dirty:: *member) {
        ((this->*member).insert(entt::type_index<Ts>::value()), ...);
        return *this;
    }
};

}

#endif // EDYN_COMP_DIRTY_HPP