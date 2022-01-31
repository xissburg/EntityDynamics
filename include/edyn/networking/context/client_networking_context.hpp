#ifndef EDYN_NETWORKING_CLIENT_NETWORKING_CONTEXT_HPP
#define EDYN_NETWORKING_CLIENT_NETWORKING_CONTEXT_HPP

#include "edyn/util/entity_map.hpp"
#include "edyn/parallel/message_queue.hpp"
#include "edyn/networking/util/non_proc_comp_state_history.hpp"
#include "edyn/networking/util/networked_component_index_source.hpp"
#include "edyn/networking/util/client_pool_snapshot_importer.hpp"
#include "edyn/networking/util/client_pool_snapshot_exporter.hpp"
#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/sparse_set.hpp>
#include <entt/signal/sigh.hpp>
#include <memory>

namespace edyn {

namespace packet {
    struct edyn_packet;
}

struct pool_snapshot;
class extrapolation_job;
class client_pool_snapshot_importer;

struct extrapolation_job_context {
    std::unique_ptr<extrapolation_job> job;
    message_queue_in_out m_message_queue;
};

struct client_networking_context {
    client_networking_context();

    entt::entity client_entity {entt::null};
    entt::sparse_set owned_entities;

    edyn::entity_map entity_map;
    std::vector<entt::entity> created_entities;
    std::vector<entt::entity> destroyed_entities;
    bool importing_entities {false};

    double last_snapshot_time {0};
    double snapshot_rate {30};
    double round_trip_time {0};
    double server_playout_delay {0.3};
    bool extrapolation_enabled {true};
    unsigned max_concurrent_extrapolations {2};

    std::vector<extrapolation_job_context> extrapolation_jobs;
    non_proc_comp_state_history state_history;

    using request_entity_func_t = void(entt::entity);
    entt::sigh<request_entity_func_t> request_entity_signal;

    auto request_entity_sink() {
        return entt::sink{request_entity_signal};
    }

    using packet_observer_func_t = void(const packet::edyn_packet &);
    entt::sigh<packet_observer_func_t> packet_signal;

    auto packet_sink() {
        return entt::sink{packet_signal};
    }

    entt::sigh<void(void)> client_entity_assigned_signal;
    auto client_entity_assigned_sink() {
        return entt::sink{client_entity_assigned_signal};
    }

    std::shared_ptr<client_pool_snapshot_importer> pool_snapshot_importer;
    std::shared_ptr<client_pool_snapshot_exporter> pool_snapshot_exporter;
};

}

#endif // EDYN_NETWORKING_CLIENT_NETWORKING_CONTEXT_HPP
