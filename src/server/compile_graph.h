#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "server/path_pool.h"
#include "server/worker_protocol.h"

#include "eventide/async/async.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

namespace et = eventide;

struct CompileUnit {
    std::uint32_t path_id = 0;
    llvm::SmallVector<std::uint32_t> dependencies;
    llvm::SmallVector<std::uint32_t> dependents;

    bool dirty = true;
    bool compiling = false;

    std::unique_ptr<et::cancellation_source> source =
        std::make_unique<et::cancellation_source>();

    std::unique_ptr<et::event> completion;
};

/// Manages the DAG of PCH/PCM compilation dependencies.
/// Tracks which compilation units are dirty and orchestrates their rebuild
/// in topological order, with cancellation and cycle detection support.
class CompileGraph {
public:
    using CompileDispatcher = std::function<
        et::task<bool>(std::uint32_t path_id, et::cancellation_token token)>;

    explicit CompileGraph(ServerPathPool& pool) : path_pool(pool) {}

    void register_unit(std::uint32_t path_id,
                       llvm::ArrayRef<std::uint32_t> deps);

    void remove_unit(std::uint32_t path_id);

    void update(std::uint32_t path_id);

    et::task<bool> compile_deps(std::uint32_t path_id, et::event_loop& loop);

    void set_dispatcher(CompileDispatcher dispatcher);

    bool has_unit(std::uint32_t path_id) const;

    const CompileUnit* find_unit(std::uint32_t path_id) const;

    std::pair<std::string, std::uint32_t> get_pch(std::uint32_t path_id) const;
    llvm::StringMap<std::string> get_pcms(std::uint32_t path_id) const;

    void set_pch_path(std::uint32_t path_id, const std::string& pch_path,
                      std::uint32_t preamble_bound);
    void set_pcm_path(std::uint32_t path_id, const std::string& module_name,
                      const std::string& pcm_path);

private:
    et::task<bool> compile_impl(std::uint32_t path_id, et::event_loop& loop,
                                llvm::SmallVector<std::uint32_t>* stack = nullptr);

    llvm::DenseMap<std::uint32_t, CompileUnit> units;
    ServerPathPool& path_pool;
    CompileDispatcher dispatcher;

    llvm::DenseMap<std::uint32_t, std::pair<std::string, std::uint32_t>> pch_cache;
    llvm::DenseMap<std::uint32_t, std::pair<std::string, std::string>> pcm_cache;
};

}  // namespace clice
