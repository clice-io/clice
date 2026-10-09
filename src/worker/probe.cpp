module;

#include "modules/prelude.h"

module clice;

import :worker.probe;

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

bool BuildProbe::release(std::uint64_t id) {
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
}

}  // namespace clice
