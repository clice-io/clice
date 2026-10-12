module;

#include "modules/prelude.h"

module clice;

import :worker.probe;
import :worker.protocol;

namespace clice {

llvm::StringRef build_kind_name(BuildKind kind) {
    return build_kind_names[std::to_underlying(kind)];
}

/// The enumerator `names` spells `name` for.
template <typename Enum, std::size_t N>
static std::optional<Enum> parse_name(const std::array<llvm::StringLiteral, N>& names,
                                      llvm::StringRef name) {
    auto it = llvm::find(names, name);
    if(it == names.end()) {
        return std::nullopt;
    }
    return static_cast<Enum>(it - names.begin());
}

std::optional<BuildKind> parse_build_kind(llvm::StringRef name) {
    return parse_name<BuildKind>(build_kind_names, name);
}

std::optional<LoopEvent> parse_loop_event(llvm::StringRef name) {
    return parse_name<LoopEvent>(loop_event_names, name);
}

kota::task<> BuildProbe::returned(BuildKind kind, llvm::StringRef file) {
    builds[file][std::to_underlying(kind)] += 1;
    auto it = llvm::find_if(placed, [&](const Hold& hold) {
        return !hold.parked && hold.kind == kind && hold.file == file;
    });
    if(it == placed.end()) {
        co_return;
    }
    it->parked = true;
    // The release erases the hold before this frame resumes.
    auto released = it->released;
    on_held.emit(it->id);
    co_await released->wait();
}

std::uint64_t BuildProbe::take_id() {
    auto id = next_id;
    next_id += 1;
    return id;
}

std::uint64_t BuildProbe::hold(BuildKind kind, std::string file) {
    auto id = take_id();
    placed.push_back({.id = id, .kind = kind, .file = std::move(file)});
    return id;
}

std::uint64_t BuildProbe::gate(std::string tag) {
    auto id = take_id();
    standing.push_back({.id = id, .tag = std::move(tag)});
    return id;
}

std::optional<std::uint64_t>
    BuildProbe::sending(llvm::StringRef tag, const std::shared_ptr<kota::ipc::BincodePeer>& peer) {
    auto it =
        llvm::find_if(standing, [&](const Gate& gate) { return !gate.sent && gate.tag == tag; });
    if(it == standing.end()) {
        return std::nullopt;
    }
    it->sent = true;
    it->worker = peer;
    peer->send_notification(worker::GateParams{.id = it->id, .tag = it->tag});
    return it->id;
}

void BuildProbe::gate_parked(std::uint64_t id) {
    auto it = llvm::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
    if(it == standing.end()) {
        return;
    }
    it->parked = true;
    on_held.emit(id);
}

/// Tell the worker a gate went to that it is released.
static void release_gate(const BuildProbe::Gate& gate) {
    if(auto worker = gate.worker.lock()) {
        worker->send_notification(worker::GateReleaseParams{gate.id});
    }
}

void BuildProbe::answered(std::uint64_t id) {
    auto it = llvm::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
    if(it != standing.end() && !it->parked) {
        release_gate(*it);
        standing.erase(it);
    }
}

bool BuildProbe::release(std::uint64_t id) {
    if(auto it = llvm::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
       it != standing.end()) {
        release_gate(*it);
        standing.erase(it);
        return true;
    }
    auto it = llvm::find_if(placed, [&](const Hold& hold) { return hold.id == id; });
    if(it == placed.end()) {
        return false;
    }
    it->released->set();
    placed.erase(it);
    return true;
}

void BuildProbe::release_all() {
    for(auto& hold: placed) {
        hold.released->set();
    }
    placed.clear();
    for(auto& gate: standing) {
        release_gate(gate);
    }
    standing.clear();
}

std::uint64_t BuildProbe::watch(LoopEvent event) {
    auto id = take_id();
    watches.push_back({.id = id, .event = event});
    return id;
}

void BuildProbe::happened(LoopEvent event) {
    llvm::SmallVector<std::uint64_t> seen;
    std::erase_if(watches, [&](const Watch& watch) {
        if(watch.event != event) {
            return false;
        }
        seen.push_back(watch.id);
        return true;
    });
    for(auto id: seen) {
        on_held.emit(id);
    }
}

}  // namespace clice
