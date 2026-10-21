module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :server.session_store;
import :support.logging;

namespace clice {

std::shared_ptr<Session> SessionStore::find(Fid path_id) const {
    auto it = sessions.find(path_id);
    return it != sessions.end() ? it->second : nullptr;
}

std::shared_ptr<Session> SessionStore::open(Fid path_id) {
    auto session = std::make_shared<Session>();
    session->path_id = path_id;
    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
        park(*it->second);
    }
    sessions[path_id] = session;
    return session;
}

void SessionStore::close(Fid path_id) {
    auto it = sessions.find(path_id);
    if(it != sessions.end()) {
        it->second->generation++;
        park(*it->second);
        sessions.erase(it);
    }
}

void SessionStore::park(Session& session) {
    session.closed = true;
    parked[session.path_id] = {session.quarantine, session.hash};
}

void SessionStore::for_each(llvm::function_ref<bool(Fid, const Session&)> visitor) const {
    for(auto& [path_id, session]: sessions) {
        if(!session)
            continue;
        if(!visitor(path_id, *session))
            break;
    }
}

void SessionStore::apply_open(Session& session, std::string text, int version) {
    session.version = version;
    session.set_text(std::move(text));
    session.generation++;
    if(auto it = parked.find(session.path_id); it != parked.end()) {
        session.quarantine = std::move(it->second.quarantine);
        if(it->second.hash != session.hash) {
            session.quarantine->on_change(Quarantine::Clock::now());
        }
        parked.erase(it);
    }
}

void SessionStore::apply_change(Session& session,
                                llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                                int version,
                                PositionEncoding encoding) {
    session.version = version;

    bool applied = false;
    for(auto& change: changes) {
        std::visit(
            [&](auto& c) {
                using T = std::remove_cvref_t<decltype(c)>;
                if constexpr(std::is_same_v<T, protocol::TextDocumentContentChangeWholeDocument>) {
                    if(session.text() != c.text) {
                        session.set_text(c.text);
                        applied = true;
                    }
                } else {
                    auto map = session.positions();
                    auto& range = c.range;
                    if(!map.offset(range.start, encoding) || !map.offset(range.end, encoding)) {
                        // The client's view has drifted from ours (or the
                        // client is buggy). LSP 3.17 requires clamping
                        // positions past the document instead of dropping
                        // the edit, which would silently desync every
                        // subsequent position until a full sync or reopen.
                        LOG_INFO(
                            "didChange range {}:{}-{}:{} does not fit the buffer "
                            "(path_id={} version={}); clamped",
                            range.start.line,
                            range.start.character,
                            range.end.line,
                            range.end.character,
                            session.path_id,
                            version);
                    }
                    auto [start, end] = map.offset_range(range, encoding);
                    auto text = session.text();
                    if(text.substr(start, end - start) != c.text) {
                        std::string edited;
                        edited.reserve(text.size() - (end - start) + c.text.size());
                        edited.append(text.take_front(start));
                        edited.append(c.text);
                        edited.append(text.drop_front(end));
                        session.set_text(std::move(edited));
                        applied = true;
                    }
                }
            },
            change);
    }

    // A real content change lets crashed kinds retry; a no-op edit leaves
    // the crashing bytes in place.
    if(applied) {
        session.quarantine->on_change(Quarantine::Clock::now());
    }

    session.generation++;
}

}  // namespace clice
