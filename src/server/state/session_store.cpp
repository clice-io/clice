#include "server/state/session_store.h"

#include <type_traits>
#include <utility>
#include <variant>

#include "server/protocol/position.h"
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
    session.quarantine.reset();
}

void SessionStore::apply_change(Session& session,
                                llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                                int version) {
    session.version = version;

    bool applied = false;
    for(auto& change: changes) {
        std::visit(
            [&](auto& c) {
                using T = std::remove_cvref_t<decltype(c)>;
                if constexpr(std::is_same_v<T, protocol::TextDocumentContentChangeWholeDocument>) {
                    if(session.text != c.text) {
                        session.text = c.text;
                        applied = true;
                    }
                } else {
                    auto& range = c.range;
                    auto map = session.line_map();
                    auto start = map.to_offset(range.start);
                    auto end = map.to_offset(range.end);
                    if(!start || !end || *start > *end) {
                        // The client's view has drifted from ours (or the
                        // client is buggy). LSP 3.17 requires clamping
                        // positions past the document instead of dropping
                        // the edit, which would silently desync every
                        // subsequent position until a full sync or reopen.
                        LOG_INFO(
                            "didChange range {}:{}-{}:{} does not fit the buffer " "(path_id={} version={}); clamped",
                            range.start.line,
                            range.start.character,
                            range.end.line,
                            range.end.character,
                            session.path_id,
                            version);
                        start = clamped_offset(map, range.start);
                        end = clamped_offset(map, range.end);
                        if(*start > *end) {
                            // Inverted even after clamping: treat it as an
                            // empty range at the clamped start.
                            end = start;
                        }
                    }
                    if(llvm::StringRef(session.text).substr(*start, *end - *start) != c.text) {
                        session.text.replace(*start, *end - *start, c.text);
                        applied = true;
                    }
                }
                session.line_starts = lsp::build_line_starts(session.text);
            },
            change);
    }

    // A real content change re-arms a quarantined document with one probe
    // attempt; no-op edits grant none (see Quarantine::on_edit).
    session.quarantine.on_edit(applied);

    session.generation++;
    session.ast_dirty = true;
}

void SessionStore::reset_compile_state(Session& session, ResetDepth depth) {
    session.ast_dirty = true;
    switch(depth) {
        case ResetDepth::Superseded: {
            session.pch_key.reset();
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
