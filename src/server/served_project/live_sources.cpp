module clice;

import :index.instantiation_shard;
import :index.project_index;
import :server.served_project;

namespace clice {

namespace {

/// The preamble index of PCH `key`, loaded on its first read: the live
/// sources are views, and the load only fills the builder's cache.
std::shared_ptr<index::TUIndex> preamble_of(const PCHBuilder& pch, llvm::StringRef key) {
    return const_cast<PCHBuilder&>(pch).preamble_state(key);
}

/// An open document's index as the live sources hand it out: `shard` in
/// the buffer's coordinates, with the instantiations that serve beside it.
index::IndexSource buffer_source(const ServedProject& served,
                                 index::IndexSource::Kind kind,
                                 const ServedDocument& document,
                                 const index::Shard& shard) {
    auto& project = served.project;
    auto& session = *document.session;
    auto file = session.path_id;
    index::IndexSource source{
        .kind = kind,
        .file = file,
        .path = project.file_table.display(file),
        .shard = &shard,
        .positions = session.positions(),
    };
    // The buffer's own compile resolved what it instantiates and expands;
    // what the other units do in the file serves while the buffer is the
    // text they read.
    if(kind == index::IndexSource::Kind::SessionShard) {
        auto& state = *document.latest->index;
        if(auto section = state.instantiation_section_of(state.path_count() - 1)) {
            source.instantiations.push_back(&state.instantiation_shard(*section));
        }
    }
    auto* stored = project.index.instantiation_shard(file);
    if(kind != index::IndexSource::Kind::PreambleShard && stored &&
       stored->content_hash() == session.hash) {
        source.instantiations.push_back(stored);
    }
    return source;
}

/// The open document's own file index when it describes the buffer: the
/// compile is current, or it compiled these very bytes.
const index::Shard* buffer_shard(const ServedDocument& document) {
    auto& compiled = document.latest;
    if(!compiled || !compiled->index || !compiled->index->loaded()) {
        return nullptr;
    }
    // A shard compiled from exactly the buffer's bytes describes it however
    // its dependencies moved since (the shard's DepsOnly contract).
    auto& state = *compiled->index;
    auto& shard = state.shard_of(state.path_count() - 1);
    auto& session = *document.session;
    if(document.current || shard.matches_content(session.text().size(), session.hash)) {
        return &shard;
    }
    return nullptr;
}

/// The overlay TU index of an open document's PCH, or null when it has no
/// PCH or the index is unreadable. Returned by value: a reference into the
/// map value would not survive a rehash.
std::shared_ptr<index::TUIndex> overlay_of(const ServedProject& served,
                                           const ServedDocument& document) {
    if(!document.latest || !document.latest->loaded_pch()) {
        return nullptr;
    }
    return preamble_of(served.sched.pch, *document.latest->loaded_pch());
}

}  // namespace

bool ServedProject::is_open(Fid file) const {
    return find(file) != nullptr;
}

const index::Shard* ServedProject::matching_shard(const Session& session) const {
    auto* shard = project.index.shard(session.path_id);
    return shard && shard->matches_content(session.text().size(), session.hash) ? shard : nullptr;
}

std::optional<index::IndexSource> ServedProject::buffer_index(Fid file) const {
    auto* document = find(file);
    if(!document) {
        return std::nullopt;
    }
    if(auto* shard = buffer_shard(*document)) {
        return buffer_source(*this, index::IndexSource::Kind::SessionShard, *document, *shard);
    }
    auto* shard = matching_shard(*document->session);
    if(!shard) {
        return std::nullopt;
    }
    return buffer_source(*this, index::IndexSource::Kind::Shard, *document, *shard);
}

void ServedProject::for_each_session(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    for(auto& [file, document]: documents) {
        auto* shard = buffer_shard(document);
        if(shard &&
           !visit(buffer_source(*this, index::IndexSource::Kind::SessionShard, document, *shard))) {
            return;
        }
    }
}

void ServedProject::for_each_session_index(
    llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    for(auto& [file, document]: documents) {
        if(buffer_shard(document) && !visit(*document.latest->index)) {
            return;
        }
    }
}

void ServedProject::for_each_preamble(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    for(auto& [file, document]: documents) {
        auto state = overlay_of(*this, document);
        if(!state) {
            continue;
        }
        // The preamble shard's entries are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from. The prefix comparison validates the described region
        // directly — body edits never move preamble entries — so no dirty-flag
        // gating is needed on top.
        auto last = state->path_count() - 1;
        if(project.file_table.find(Spelling::absolute(state->path(last))) != file ||
           !state->matches_preamble(document.session->text())) {
            continue;
        }
        if(!visit(buffer_source(*this,
                                index::IndexSource::Kind::PreambleShard,
                                document,
                                state->shard_of(last)))) {
            return;
        }
    }
}

void ServedProject::for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    // Sessions with identical preambles share one blob; visit it once. A
    // preamble's PCH built over the build's precompiled header indexes
    // only what it adds: that header's blob is visited too.
    llvm::StringSet<> seen;
    auto visit_key = [&](llvm::StringRef key) {
        if(key.empty() || !seen.insert(key).second) {
            return true;
        }
        auto state = preamble_of(sched.pch, key);
        return state ? visit(*state) : true;
    };
    for(auto& [file, document]: documents) {
        auto& compiled = document.latest;
        if(!compiled || !compiled->loaded_pch()) {
            continue;
        }
        auto& key = *compiled->loaded_pch();
        if(!visit_key(key)) {
            return;
        }
        auto it = project.pch_cache.find(key);
        if(it != project.pch_cache.end() && !visit_key(std::string(it->second.base))) {
            return;
        }
    }
}

std::shared_ptr<index::TUIndex> ServedProject::preamble_index(Fid file) const {
    auto* document = find(file);
    if(!document) {
        return nullptr;
    }
    auto state = overlay_of(*this, *document);
    if(!state || !state->matches_preamble(document->session->text())) {
        return nullptr;
    }
    return state;
}

void
    ServedProject::for_each_pch_index(Fid file,
                                      llvm::function_ref<void(const index::TUIndex&)> visit) const {
    if(auto state = preamble_index(file)) {
        visit(*state);
    }
    auto compiled = latest(file);
    if(!compiled || !compiled->base_pch_key) {
        return;
    }
    if(auto state = preamble_of(sched.pch, *compiled->base_pch_key)) {
        visit(*state);
    }
}

}  // namespace clice
