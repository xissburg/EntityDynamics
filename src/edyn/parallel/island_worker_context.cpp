#include "edyn/parallel/island_worker_context.hpp"
#include "edyn/parallel/island_delta.hpp"
#include "edyn/parallel/island_worker.hpp"
#include "edyn/parallel/island_delta_builder.hpp"

namespace edyn {

island_worker_context::island_worker_context(entt::entity island_entity,
            island_worker *worker,
            std::unique_ptr<island_delta_builder> delta_builder,
            message_queue_in_out message_queue)
    : m_island_entity(island_entity)
    , m_worker(worker)
    , m_message_queue(message_queue)
    , m_delta_builder(std::move(delta_builder))
    , m_pending_flush(false)
{
    m_message_queue.sink<island_delta>().connect<&island_worker_context::on_island_delta>(*this);
    m_message_queue.sink<msg::split_island>().connect<&island_worker_context::on_split_island>(*this);
}

island_worker_context::~island_worker_context() {
    m_message_queue.sink<island_delta>().disconnect(*this);
}

void island_worker_context::on_island_delta(const island_delta &delta) {
    m_island_delta_signal.publish(m_island_entity, delta);
}

void island_worker_context::on_split_island(const msg::split_island &topo) {
    m_split_island_signal.publish(m_island_entity, topo);
}

bool island_worker_context::delta_empty() const {
    return m_delta_builder->empty();
}

bool island_worker_context::delta_needs_wakeup() const {
    return m_delta_builder->needs_wakeup();
}

void island_worker_context::read_messages() {
    m_message_queue.update();
}

void island_worker_context::send_delta() {
    send<island_delta>(m_delta_builder->finish());
}

void island_worker_context::flush() {
    if (m_pending_flush) {
        m_worker->reschedule();
        m_pending_flush = false;
    }
}

void island_worker_context::terminate() {
    m_worker->terminate();
}

}
