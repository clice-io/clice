#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "server/options.h"
#include "server/worker_protocol.h"

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"

#include "llvm/ADT/DenseMap.h"

namespace clice {

namespace et = eventide;

struct WorkerHandle {
    et::process proc;
    std::unique_ptr<et::ipc::StreamTransport> transport;
    std::unique_ptr<et::ipc::BincodePeer> peer;
    bool alive = false;
    std::string label;
};

class WorkerPool {
public:
    WorkerPool(const Options& options, et::event_loop& loop);

    et::task<> start();
    et::task<> stop();

    template <typename Params>
    auto send_stateless(const Params& params, et::ipc::request_options opts = {})
        -> et::ipc::RequestResult<Params> {
        if(stateless.workers.empty()) {
            co_return et::outcome_error(et::ipc::protocol::Error(
                et::ipc::protocol::ErrorCode::InternalError, "no stateless workers available"));
        }
        auto& w = stateless.workers[stateless.next % stateless.workers.size()];
        stateless.next++;
        if(!w->alive || !w->peer) {
            co_return et::outcome_error(et::ipc::protocol::Error(
                et::ipc::protocol::ErrorCode::InternalError, "stateless worker not alive"));
        }
        co_return co_await w->peer->send_request(params, opts);
    }

    template <typename Params>
    auto send_stateful(std::uint32_t path_id,
                       const Params& params,
                       et::ipc::request_options opts = {})
        -> et::ipc::RequestResult<Params> {
        if(stateful.workers.empty()) {
            co_return et::outcome_error(et::ipc::protocol::Error(
                et::ipc::protocol::ErrorCode::InternalError, "no stateful workers available"));
        }
        auto idx = assign_worker(path_id);
        auto& w = stateful.workers[idx];
        if(!w->alive || !w->peer) {
            co_return et::outcome_error(et::ipc::protocol::Error(
                et::ipc::protocol::ErrorCode::InternalError, "stateful worker not alive"));
        }
        co_return co_await w->peer->send_request(params, opts);
    }

    void register_evicted_handler(std::function<void(const std::string&)> handler);

    bool has_workers() const;

private:
    et::task<std::unique_ptr<WorkerHandle>> spawn_worker(Options::Mode mode,
                                                          const std::string& label);
    et::task<> monitor_worker(std::size_t index, bool is_stateful);
    et::task<> collect_stderr(et::pipe stderr_pipe, std::string prefix);
    et::task<> restart_worker(bool is_stateful, std::size_t index);

    std::size_t assign_worker(std::uint32_t path_id);
    void clear_owner(std::size_t worker_index);
    void remove_owner(std::uint32_t path_id);

    struct StatelessState {
        std::vector<std::unique_ptr<WorkerHandle>> workers;
        std::size_t next = 0;
    } stateless;

    struct StatefulState {
        std::vector<std::unique_ptr<WorkerHandle>> workers;
        llvm::DenseMap<std::uint32_t, std::size_t> owner;
        std::list<std::uint32_t> owner_lru;
        llvm::DenseMap<std::uint32_t, std::list<std::uint32_t>::iterator> owner_lru_index;
    } stateful;

    const Options& options;
    et::event_loop& loop;
    std::function<void(const std::string&)> evicted_handler;
};

}  // namespace clice
