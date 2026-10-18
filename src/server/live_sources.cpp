module;

#include "modules/prelude.h"

module clice;

import :index.instantiation_shard;
import :server.live_sources;

namespace clice {

namespace {

/// The source file's preamble-region shard of an overlay TU index (buffer
/// offsets below the preamble bound).
const index::Shard& preamble_shard(const index::TUIndex& state) {
    return state.shard_of(state.path_count() - 1);
}

}  // namespace

bool ServerLiveSources::is_open(Fid file) const {
    return sessions.find(file) != nullptr;
}

index::IndexSource ServerLiveSources::buffer_source(index::IndexSource::Kind kind,
                                                    Fid file,
                                                    const Session& session,
                                                    const index::Shard& shard) const {
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
        auto& state = *projections.projection(file)->index;
        if(auto section = state.instantiation_section_of(state.path_count() - 1)) {
            source.instantiations.push_back(&state.instantiation_shard(*section));
        }
    }
    auto it = project.project_index.instantiations.find(file);
    if(kind != index::IndexSource::Kind::PreambleShard &&
       it != project.project_index.instantiations.end() &&
       it->second.content_hash() == session.hash) {
        source.instantiations.push_back(&it->second);
    }
    return source;
}

const index::Shard* ServerLiveSources::session_shard(Fid file, const Session& session) const {
    auto projection = projections.projection(file);
    if(!projection || !projection->index || !projection->index->loaded()) {
        return nullptr;
    }
    // A shard compiled from exactly the buffer's bytes describes it however
    // its dependencies moved since (the shard's DepsOnly contract).
    auto& shard = projection->file_shard();
    if(projections.current(file) || shard.matches_content(session.text().size(), session.hash)) {
        return &shard;
    }
    return nullptr;
}

std::optional<index::IndexSource> ServerLiveSources::buffer_index(Fid file) const {
    auto session = sessions.find(file);
    if(!session) {
        return std::nullopt;
    }
    if(auto* shard = session_shard(file, *session)) {
        return buffer_source(index::IndexSource::Kind::SessionShard, file, *session, *shard);
    }
    auto it = project.project_index.shards.find(file);
    if(it == project.project_index.shards.end() ||
       !it->second.matches_content(session->text().size(), session->hash)) {
        return std::nullopt;
    }
    return buffer_source(index::IndexSource::Kind::Shard, file, *session, it->second);
}

void ServerLiveSources::for_each_session(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        auto* shard = session_shard(file, session);
        return !shard ||
               visit(buffer_source(index::IndexSource::Kind::SessionShard, file, session, *shard));
    });
}

void ServerLiveSources::for_each_session_index(
    llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        return !session_shard(file, session) || visit(*projections.projection(file)->index);
    });
}

std::shared_ptr<index::TUIndex> ServerLiveSources::overlay_of(Fid file) const {
    auto projection = projections.projection(file);
    if(!projection || !projection->pch_key) {
        return nullptr;
    }
    // Returned by value: a reference into the map value would not survive
    // a rehash.
    return pch.preamble_state(*projection->pch_key);
}

void ServerLiveSources::for_each_preamble(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        auto state = overlay_of(file);
        if(!state) {
            return true;
        }
        // The preamble shard's entries are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from. The prefix comparison validates the described region
        // directly — body edits never move preamble entries — so no dirty-flag
        // gating is needed on top.
        if(project.file_table.find(Spelling::absolute(state->path(state->path_count() - 1))) !=
               file ||
           !state->matches_prefix(session.text())) {
            return true;
        }
        return visit(buffer_source(index::IndexSource::Kind::PreambleShard,
                                   file,
                                   session,
                                   preamble_shard(*state)));
    });
}

void ServerLiveSources::for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    // Sessions with identical preambles share one blob; visit it once. A
    // preamble's PCH built over the build's precompiled header indexes
    // only what it adds: that header's blob is visited too.
    llvm::StringSet<> seen;
    auto visit_key = [&](llvm::StringRef key) {
        if(key.empty() || !seen.insert(key).second) {
            return true;
        }
        auto state = pch.preamble_state(key);
        return state ? visit(*state) : true;
    };
    sessions.for_each([&](Fid file, const Session&) -> bool {
        auto projection = projections.projection(file);
        if(!projection || !projection->pch_key) {
            return true;
        }
        if(!visit_key(*projection->pch_key)) {
            return false;
        }
        auto it = project.pch_cache.find(*projection->pch_key);
        return it == project.pch_cache.end() || visit_key(std::string(it->second.base));
    });
}

std::shared_ptr<index::TUIndex> ServerLiveSources::preamble_index(Fid file) const {
    auto session = sessions.find(file);
    if(!session) {
        return nullptr;
    }
    auto state = overlay_of(file);
    if(!state || !state->matches_prefix(session->text())) {
        return nullptr;
    }
    return state;
}

}  // namespace clice
