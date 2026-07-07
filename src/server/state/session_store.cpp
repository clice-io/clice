#include "server/state/session_store.h"

#include <type_traits>
#include <utility>
#include <variant>

#include "support/logging.h"

#include "kota/ipc/lsp/position.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

std::shared_ptr<Session> SessionStore::find(std::uint32_t path_id) const {
    auto it = sessions.find(path_id);
    return it != sessions.end() ? it->second : nullptr;
}

std::shared_ptr<Session> SessionStore::open(std::uint32_t path_id) {
    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
    }
    auto session = std::make_shared<Session>();
    session->path_id = path_id;
    sessions[path_id] = session;
    return session;
}

void SessionStore::close(std::uint32_t path_id) {
    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
        sessions.erase(it);
    }
}

void SessionStore::for_each(llvm::function_ref<bool(std::uint32_t, const Session&)> visitor) const {
    for(auto& [path_id, session]: sessions) {
        if(!session)
            continue;
        if(!visitor(path_id, *session))
            break;
    }
}

void SessionStore::apply_open(Session& session, std::string text, int version) {
    session.version = version;
    session.text = std::move(text);
    session.line_starts = lsp::build_line_starts(session.text);
    session.generation++;
}

void SessionStore::apply_change(Session& session,
                                llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                                int version) {
    session.version = version;

    for(auto& change: changes) {
        std::visit(
            [&](auto& c) {
                using T = std::remove_cvref_t<decltype(c)>;
                if constexpr(std::is_same_v<T, protocol::TextDocumentContentChangeWholeDocument>) {
                    session.text = c.text;
                } else {
                    auto& range = c.range;
                    auto map = session.line_map();
                    auto start = map.to_offset(range.start);
                    auto end = map.to_offset(range.end);
                    if(start && end && *start <= *end) {
                        session.text.replace(*start, *end - *start, c.text);
                    } else {
                        // The client's view has drifted from ours (or the
                        // client is buggy). Drop the edit but keep serving:
                        // a full-document change or reopen resynchronizes.
                        LOG_ERROR(
                            "didChange range {}:{}-{}:{} does not fit the buffer " "(path_id={} version={}); edit dropped",
                            range.start.line,
                            range.start.character,
                            range.end.line,
                            range.end.character,
                            session.path_id,
                            version);
                    }
                }
                session.line_starts = lsp::build_line_starts(session.text);
            },
            change);
    }

    session.generation++;
    session.ast_dirty = true;
}

void SessionStore::reset_compile_state(Session& session, ResetDepth depth) {
    session.ast_dirty = true;
    switch(depth) {
        case ResetDepth::Superseded: {
            session.pch_ref.reset();
            session.ast_deps.reset();
            session.trial_done = false;
            // Invalidate any in-flight compile: without the bump it would
            // pass its generation check on completion and publish results
            // for the superseded identity.
            session.generation++;
            break;
        }
        case ResetDepth::Lost: {
            // An in-flight compile consumed the pre-loss world; it must not
            // clear ast_dirty when it lands (see Session::settle_compile).
            session.dirty_epoch++;
            break;
        }
    }
}

}  // namespace clice
