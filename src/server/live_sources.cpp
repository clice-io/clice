#include "server/live_sources.h"

#include "llvm/ADT/StringSet.h"

namespace clice {

namespace {

/// The source file's preamble-region rows of an overlay envelope (buffer
/// offsets below the preamble bound).
const index::Shard& preamble_rows(const index::TUIndex& state) {
    return state.shard_of(state.path_count() - 1);
}

}  // namespace

bool ServerLiveSources::is_open(Fid file) const {
    return sessions.find(file) != nullptr;
}

index::RowSource ServerLiveSources::buffer_source(index::RowSource::Kind kind,
                                                  Fid file,
                                                  const Session& session,
                                                  const index::Shard& rows) const {
    return {
        .kind = kind,
        .file = file,
        .path = project.file_table.display(file),
        .rows = &rows,
        .coords = {session.text,
                   static_cast<std::uint32_t>(session.text.size()),
                   session.line_starts}
    };
}

const index::Shard* ServerLiveSources::session_rows(Fid file, const Session& session) const {
    auto projection = projections.projection(file);
    if(!projection || !projection->index || !projection->index->loaded()) {
        return nullptr;
    }
    // Rows compiled from exactly the buffer's bytes describe it however
    // their dependencies moved since (the shard's DepsOnly contract); a
    // header compiled with an appended suffix line never matches its
    // buffer's bytes and serves only while its compile is current.
    auto& rows = projection->file_rows();
    if(projections.current(file) || rows.matches_content(session.text.size(), session.hash)) {
        return &rows;
    }
    return nullptr;
}

std::optional<index::RowSource> ServerLiveSources::claim(Fid file) const {
    auto session = sessions.find(file);
    if(!session) {
        return std::nullopt;
    }
    if(auto* rows = session_rows(file, *session)) {
        return buffer_source(index::RowSource::Kind::SessionRows, file, *session, *rows);
    }
    auto it = project.project_index.shards.find(file);
    if(it == project.project_index.shards.end() ||
       !it->second.matches_content(session->text.size(), session->hash)) {
        return std::nullopt;
    }
    return buffer_source(index::RowSource::Kind::Shard, file, *session, it->second);
}

void
    ServerLiveSources::each_session(llvm::function_ref<bool(const index::RowSource&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        auto* rows = session_rows(file, session);
        return !rows ||
               visit(buffer_source(index::RowSource::Kind::SessionRows, file, session, *rows));
    });
}

void ServerLiveSources::each_session_index(
    llvm::function_ref<bool(const index::TUIndex&)> visit) const {
    sessions.for_each([&](Fid file, const Session& session) -> bool {
        return !session_rows(file, session) || visit(*projections.projection(file)->index);
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
    llvm::function_ref<bool(const index::RowSource&)> visit) const {
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
        // gating is needed on top. The blob stores clang's native path
        // (backslashes on Windows) while the table normalizes separators,
        // so compare through the table's lookup, not raw strings.
        if(project.file_table.find(Spelling::absolute(state->path(state->path_count() - 1))) != file ||
           !state->matches_prefix(session.text)) {
            return true;
        }
        return visit(buffer_source(index::RowSource::Kind::PreambleRows,
                                   file,
                                   session,
                                   preamble_rows(*state)));
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
