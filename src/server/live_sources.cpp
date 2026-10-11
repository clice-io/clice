module;

#include "modules/prelude.h"

module clice;

import :server.live_sources;

namespace clice {

namespace {

/// The source file's preamble-region rows of an overlay envelope (buffer
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
    return {
        .kind = kind,
        .file = file,
        .path = project.file_table.display(file),
        .shard = &shard,
        .positions = session.positions(),
    };
}

const index::Shard* ServerLiveSources::session_shard(Fid file, const Session& session) const {
    auto projection = projections.projection(file);
    if(!projection || !projection->index || !projection->index->loaded()) {
        return nullptr;
    }
    // Rows compiled from exactly the buffer's bytes describe it however
    // their dependencies moved since (the shard's DepsOnly contract); a
    // header compiled with an appended suffix line never matches its
    // buffer's bytes and serves only while its compile is current.
    auto& shard = projection->file_shard();
    if(projections.current(file) || shard.matches_content(session.text.size(), session.hash)) {
        return &shard;
    }
    return nullptr;
}

std::optional<index::IndexSource> ServerLiveSources::serving_buffer(Fid file) const {
    auto session = sessions.find(file);
    if(!session) {
        return std::nullopt;
    }
    if(auto* shard = session_shard(file, *session)) {
        return buffer_source(index::IndexSource::Kind::SessionIndex, file, *session, *shard);
    }
    auto it = project.project_index.shards.find(file);
    if(it == project.project_index.shards.end() ||
       !it->second.matches_content(session->text.size(), session->hash)) {
        return std::nullopt;
    }
    return buffer_source(index::IndexSource::Kind::Shard, file, *session, it->second);
}

void
    ServerLiveSources::each_session(llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        auto* shard = session_shard(file, session);
        return !shard ||
               visit(buffer_source(index::IndexSource::Kind::SessionIndex, file, session, *shard));
    });
}

void ServerLiveSources::each_session_index(
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

void ServerLiveSources::each_preamble(
    llvm::function_ref<bool(const index::IndexSource&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        auto state = overlay_of(file);
        if(!state) {
            return true;
        }
        // The preamble entry's rows are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from. The prefix comparison validates the described region
        // directly — body edits never move preamble rows — so no dirty-flag
        // gating is needed on top.
        if(project.file_table.find(Spelling::absolute(state->path(state->path_count() - 1))) !=
               file ||
           !state->matches_prefix(session.text)) {
            return true;
        }
        return visit(buffer_source(index::IndexSource::Kind::PreambleIndex,
                                   file,
                                   session,
                                   preamble_shard(*state)));
    });
}

void ServerLiveSources::each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    // Sessions with identical preambles share one blob; visit it once.
    llvm::StringSet<> seen;
    sessions.for_each([&](Fid file, const Session&) -> bool {
        auto projection = projections.projection(file);
        if(!projection || !projection->pch_key || !seen.insert(*projection->pch_key).second) {
            return true;
        }
        auto state = overlay_of(file);
        return state ? visit(*state) : true;
    });
}

std::shared_ptr<index::TUIndex> ServerLiveSources::preamble_blob(Fid file) const {
    auto session = sessions.find(file);
    if(!session) {
        return nullptr;
    }
    auto state = overlay_of(file);
    if(!state || !state->matches_prefix(session->text)) {
        return nullptr;
    }
    return state;
}

}  // namespace clice
