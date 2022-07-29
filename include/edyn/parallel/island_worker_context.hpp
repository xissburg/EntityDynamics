#ifndef EDYN_PARALLEL_ISLAND_WORKER_CONTEXT_HPP
#define EDYN_PARALLEL_ISLAND_WORKER_CONTEXT_HPP

#include <memory>
#include <entt/entity/fwd.hpp>
#include <entt/signal/fwd.hpp>
#include <entt/entity/sparse_set.hpp>
#include "edyn/comp/island.hpp"
#include "edyn/util/entity_map.hpp"
#include "edyn/parallel/message.hpp"
#include "edyn/parallel/island_worker.hpp"
#include "edyn/parallel/message_dispatcher.hpp"

namespace edyn {

class registry_operation_collection;
class registry_operation_builder;

/**
 * Context of an island worker in the main thread in an island coordinator.
 */
class island_worker_context {

    island_worker *m_worker;
    bool m_pending_flush;

public:
    entt::sparse_set m_islands;
    entity_map m_entity_map;
    std::unique_ptr<registry_operation_builder> m_op_builder;
    double m_timestamp;

    island_worker_context(island_worker *worker, std::unique_ptr<registry_operation_builder> op_builder);

    /**
     * Returns whether there are any pending registry operations to be sent.
     */
    bool reg_ops_empty() const;

    /**
     * Reads messages sent by worker.
     */
    void read_messages();

    /**
     * Sends current registry operations and clears it up, making it ready for more
     * updates.
     */
    void send_reg_ops(message_queue_identifier source);

    /**
     * Ensures messages are delivered and processed by waking up the worker
     * in case it is sleeping.
     */
    void flush();

    template<typename Message, typename... Args>
    void send(message_queue_identifier source, Args &&... args) {
        message_dispatcher::global().send<Message>(m_worker->message_queue_id(), source, std::forward<Args>(args)...);
        m_pending_flush = true;
    }

    auto message_queue_id() const {
        return m_worker->message_queue_id();
    }

    /**
     * Schedules worker to be terminated.
     */
    void terminate();
};

}

#endif // EDYN_PARALLEL_ISLAND_WORKER_CONTEXT_HPP
