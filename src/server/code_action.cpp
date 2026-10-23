module;

#include "modules/prelude.h"

module clice;

import :feature.code_action.code_action;
import :feature.formatting;
import :feature.protocol;
import :server.editor_context;
import :server.features;
import :syntax.include_resolver;
import :syntax.source_text;
import :vfs.file_system;

namespace clice {

kota::task<std::vector<protocol::CodeAction>, kota::ipc::Error>
    Features::code_action(Ticket ticket,
                          const protocol::Range& range,
                          llvm::ArrayRef<protocol::CodeActionKind> only,
                          kota::cancellation_token token) {
    auto& session = ticket.session;
    std::vector<protocol::CodeAction> out;
    if(std::ranges::none_of(feature::code_action_kinds,
                            [&](std::string_view kind) { return feature::admits(only, kind); })) {
        co_return out;
    }
    // Code actions are AST products the index cannot compute; a session
    // the policy keeps un-compiled answers honestly empty rather than
    // forcing the compile the policy declined.
    if(!ast_answerable(*session) && session->serving == ServingMode::IndexOnly) {
        co_return out;
    }

    auto actions = co_await dispatcher.code_actions(ticket, range, std::move(token)).or_fail();

    auto path_id = session->path_id;
    auto path = project.file_table.display(path_id);
    auto uri = feature::to_uri(path);
    auto map = session->positions();

    auto emit = [&](std::string title,
                    protocol::CodeActionKind kind,
                    llvm::ArrayRef<TextReplacement> replacements) {
        auto edits = feature::to_text_edits(replacements, map, PositionEncoding::UTF16);
        if(!edits) {
            return;
        }
        out.push_back(feature::to_protocol(
            std::move(title),
            std::move(kind),
            {.uri = uri, .version = session->version, .edits = std::move(*edits)}));
    };
    auto defined_elsewhere = [&](std::uint64_t entity) {
        return query.first_site(entity, path_id, RelationKind::Definition).has_value();
    };

    auto resolve_define = [&](feature::CodeAction& action, const feature::DefineRequest& request) {
        if(auto edits = feature::define_edits(request, path, session->text(), defined_elsewhere)) {
            emit(std::move(action.title), std::move(action.kind), *edits);
        }
    };

    auto resolve_host = [&](feature::CodeAction& action,
                            const feature::DefineInHostRequest& request) {
        auto text = feature::assemble_definitions(request.pieces, defined_elsewhere);
        Fid host = contexts.host_of(path_id);
        if(!text || !host.valid()) {
            return;
        }
        auto host_path = project.file_table.display(host);
        auto host_session = sessions.find(host);
        auto formatted = feature::format_snippet(
            host_session ? host_path : project.file_table.spelling(host).str(),
            *text);

        std::optional<SourceText> disk;
        auto edit =
            feature::host_edit(query, host, request.container, formatted, [&]() -> SourceText* {
                if(auto* open = host_session ? host_session->source.get() : nullptr) {
                    return open;
                }
                auto read = vfs::read(host_path);
                return read ? &disk.emplace((*read)->getBuffer().str()) : nullptr;
            });
        if(!edit) {
            return;
        }
        out.push_back(feature::to_protocol(
            std::format("{} in {}", action.title, llvm::sys::path::filename(host_path)),
            std::move(action.kind),
            {
                .uri = feature::to_uri(host_path),
                .version = host_session ? std::optional(host_session->version) : std::nullopt,
                .edits = {std::move(*edit)},
            }));
    };

    auto resolve_include = [&](feature::CodeAction& action,
                               const feature::IncludeRequest& request) {
        auto headers = feature::declaring_headers(query, request, path_id);
        if(headers.empty()) {
            return;
        }
        std::string directory;
        std::vector<std::string> arguments;
        auto ref = contexts.resolve_command(path_id, directory, arguments).ref;
        auto search = project.toolchain.search_config(ref);
        vfs::Scope scope(project.file_table.dirs);
        for(const auto& header: headers) {
            if(auto argument =
                   include_argument(header, search, project.file_table.spelling(path_id), scope)) {
                emit(std::format("Add #include {}", *argument),
                     action.kind,
                     {
                         {{request.insertion.offset, request.insertion.offset},
                          request.insertion.text(*argument)}
                });
            }
        }
    };

    for(auto& action: actions) {
        if(!feature::admits(only, action.kind)) {
            continue;
        }
        if(!action.index) {
            emit(std::move(action.title), std::move(action.kind), action.edits);
            continue;
        }
        std::visit(
            [&](const auto& request) {
                using Request = std::remove_cvref_t<decltype(request)>;
                if constexpr(std::same_as<Request, feature::DefineRequest>) {
                    resolve_define(action, request);
                } else if constexpr(std::same_as<Request, feature::DefineInHostRequest>) {
                    resolve_host(action, request);
                } else {
                    resolve_include(action, request);
                }
            },
            *action.index);
    }
    co_return out;
}

}  // namespace clice
