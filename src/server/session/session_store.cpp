#include "server/session/session_store.h"

#include <type_traits>
#include <utility>
#include <variant>

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
                    }
                }
                session.line_starts = lsp::build_line_starts(session.text);
            },
            change);
    }

    session.generation++;
    session.ast_dirty = true;
}

}  // namespace clice
