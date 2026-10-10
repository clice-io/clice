module;

#include "modules/prelude.h"

module clice;

import :worker.probe;
import :worker.protocol;

namespace clice {

llvm::StringRef build_kind_name(BuildKind kind) {
    return build_kind_names[std::to_underlying(kind)];
}

std::optional<BuildKind> parse_build_kind(llvm::StringRef name) {
    auto it = llvm::find(build_kind_names, name);
    if(it == build_kind_names.end()) {
        return std::nullopt;
    }
    return static_cast<BuildKind>(it - build_kind_names.begin());
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

std::uint64_t BuildProbe::hold(BuildKind kind, std::string file) {
    auto id = next_id;
    next_id += 1;
    placed.push_back({.id = id, .kind = kind, .file = std::move(file)});
    return id;
}

std::uint64_t BuildProbe::gate(std::string tag) {
    auto id = next_id;
    next_id += 1;
    standing.push_back({.id = id, .tag = std::move(tag)});
    return id;
}

void BuildProbe::sending(llvm::StringRef tag, const std::shared_ptr<kota::ipc::BincodePeer>& peer) {
    auto it =
        llvm::find_if(standing, [&](const Gate& gate) { return !gate.sent && gate.tag == tag; });
    if(it == standing.end()) {
        return;
    }
    it->sent = true;
    it->worker = peer;
    peer->send_notification(worker::GateParams{.id = it->id, .tag = it->tag});
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

}  // namespace clice
