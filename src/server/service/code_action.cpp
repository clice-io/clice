#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command/command.h"
#include "index/symbol_query.h"
#include "sched/context.h"
#include "server/protocol/position.h"
#include "server/service/features.h"
#include "support/filesystem.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace {

/// LSP kinds are hierarchical: a requested "refactor" admits
/// "refactor.rewrite".
bool admits(llvm::ArrayRef<protocol::CodeActionKind> only, llvm::StringRef kind) {
    if(only.empty()) {
        return true;
    }
    return llvm::any_of(only, [&](const protocol::CodeActionKind& wanted) {
        llvm::StringRef prefix = wanted;
        return prefix.empty() || kind == prefix ||
               (kind.starts_with(prefix) && kind[prefix.size()] == '.');
    });
}

constexpr std::array producible = {
    feature::CodeActionKind::QuickFix,
    feature::CodeActionKind::Refactor,
    feature::CodeActionKind::RefactorInline,
    feature::CodeActionKind::RefactorRewrite,
};

struct FileEdit {
    std::string uri;
    std::optional<int> version;
    std::vector<protocol::TextEdit> edits;
};

protocol::CodeAction render(std::string title, feature::CodeActionKind kind, FileEdit file) {
    protocol::TextDocumentEdit change{
        .text_document = {.uri = std::move(file.uri), .version = file.version},
    };
    for(auto& edit: file.edits) {
        change.edits.emplace_back(std::move(edit));
    }
    protocol::WorkspaceEdit edit;
    edit.document_changes = std::vector<protocol::variant<protocol::TextDocumentEdit,
                                                          protocol::CreateFile,
                                                          protocol::RenameFile,
                                                          protocol::DeleteFile>>{};
    edit.document_changes->emplace_back(std::move(change));
    return protocol::CodeAction{
        .title = std::move(title),
        .kind = protocol::CodeActionKind(feature::code_action_kind_name(kind)),
        .edit = std::move(edit),
    };
}

bool is_header_file(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    return type == types::TY_CHeader || type == types::TY_CXXHeader;
}

/// How `header` is spelled in an include directive of `file`: its path
/// below the file's own directory (where a quoted include looks first),
/// else the shortest path below one of the command's search directories,
/// angled past the quoted segment.
std::optional<std::string> include_spelling(llvm::StringRef header,
                                            const SearchConfig& search,
                                            llvm::StringRef file) {
    auto below = [&](llvm::StringRef root) -> std::optional<llvm::StringRef> {
        if(root.empty() || !path::under(header, root) || header.size() <= root.size()) {
            return std::nullopt;
        }
        return header.drop_front(root.size()).ltrim("/\\");
    };
    if(auto relative = below(llvm::sys::path::parent_path(file))) {
        return std::format("\"{}\"", *relative);
    }
    std::optional<llvm::StringRef> best;
    bool angled = false;
    for(auto [index, dir]: llvm::enumerate(search.dirs)) {
        auto relative = below(dir.path);
        if(relative && (!best || relative->size() < best->size())) {
            best = relative;
            angled = index >= search.angled_start_idx;
        }
    }
    if(!best) {
        return std::nullopt;
    }
    return angled ? std::format("<{}>", *best) : std::format("\"{}\"", *best);
}

/// The position past the last byte of `content`.
protocol::Position end_of(llvm::StringRef content) {
    auto line = static_cast<std::uint32_t>(std::ranges::count(content, '\n'));
    auto last = content.rfind('\n');
    auto tail = content.substr(last == llvm::StringRef::npos ? 0 : last + 1);
    return {.line = line, .character = static_cast<std::uint32_t>(tail.size())};
}

}  // namespace

kota::task<std::vector<protocol::CodeAction>, kota::ipc::Error>
    Features::code_action(std::shared_ptr<Session> session,
                          const protocol::Range& range,
                          llvm::ArrayRef<protocol::CodeActionKind> only,
                          std::optional<kota::cancellation_token> token) {
    std::vector<protocol::CodeAction> out;
    if(llvm::none_of(producible, [&](feature::CodeActionKind kind) {
           return admits(only, feature::code_action_kind_name(kind));
       })) {
        co_return out;
    }
    // Code actions are AST products with no index projection; a session
    // the policy keeps un-compiled answers honestly empty rather than
    // forcing the compile the policy declined.
    if(!ast_answerable(*session) && session->serving == ServingMode::IndexOnly) {
        co_return out;
    }

    auto ticket = Ticket::take(session);
    auto result = co_await dispatcher.code_actions(ticket, range, std::move(token));
    if(!result.has_value()) {
        co_return kota::outcome_error(std::move(result.error()));
    }

    auto path_id = session->path_id;
    auto path = workspace.file_table.resolve(path_id);
    auto uri = feature::to_uri(path);
    auto map = session->line_map();
    auto main_edit = [&](LocalSourceRange local, std::string text) -> std::optional<FileEdit> {
        auto converted = feature::to_range(map, local);
        if(!converted) {
            return std::nullopt;
        }
        return FileEdit{
            .uri = uri,
            .version = session->version,
            .edits = {{.range = *converted, .new_text = std::move(text)}},
        };
    };
    auto defined_elsewhere = [&](std::uint64_t entity) {
        return query.first_site(entity, RelationKind::Definition).has_value();
    };

    auto resolve_define = [&](const feature::CodeAction& action,
                              const feature::DefineRequest& request) {
        auto text = feature::assemble_definitions(request, defined_elsewhere);
        if(!text) {
            return;
        }
        if(!request.host) {
            std::vector<protocol::TextEdit> edits;
            for(auto& replacement: feature::format_edits(
                    path,
                    session->text,
                    {
                        {request.range, request.before + *text + request.after}
            })) {
                if(auto converted = feature::to_range(map, replacement.range)) {
                    edits.push_back({.range = *converted, .new_text = std::move(replacement.text)});
                }
            }
            if(!edits.empty()) {
                out.push_back(render(action.title,
                                     action.kind,
                                     FileEdit{uri, session->version, std::move(edits)}));
            }
            return;
        }

        Fid host = host_of(path_id);
        if(!host.valid()) {
            return;
        }
        auto host_path = workspace.file_table.resolve(host);
        auto host_session = sessions.find(host);
        auto formatted = feature::format_snippet(host_path, *text);

        // After the last definition of the container's members the index
        // places in the host, in the coordinates its serving source
        // vouches for; at the end of the file when it holds none.
        std::optional<protocol::Position> after;
        if(request.container != 0) {
            for(const auto& located: query.definitions_in(host)) {
                auto chain = query.container_chain(located.symbol.hash);
                if(llvm::none_of(chain, [&](const index::SymbolRef& container) {
                       return container.hash == request.container;
                   })) {
                    continue;
                }
                auto definition = query.definition_text(located.symbol.hash);
                if(!definition) {
                    continue;
                }
                protocol::Position end{.line = definition->extent.end.line,
                                       .character = definition->extent.end.utf16_column};
                if(!after ||
                   std::pair(end.line, end.character) > std::pair(after->line, after->character)) {
                    after = end;
                }
            }
        }
        protocol::TextEdit edit;
        if(after) {
            edit.range = {*after, *after};
            edit.new_text = "\n\n" + formatted;
        } else {
            std::string content;
            if(host_session) {
                content = host_session->text;
            } else if(auto read = fs::read(host_path)) {
                content = std::move(*read);
            } else {
                return;
            }
            auto end = end_of(content);
            edit.range = {end, end};
            edit.new_text =
                (content.empty() || content.ends_with('\n') ? "\n" : "\n\n") + formatted;
        }
        out.push_back(render(
            std::format("{} in {}", action.title, llvm::sys::path::filename(host_path)),
            action.kind,
            FileEdit{
                .uri = feature::to_uri(host_path),
                .version = host_session ? std::optional(host_session->version) : std::nullopt,
                .edits = {std::move(edit)},
            }));
    };

    auto resolve_include = [&](const feature::CodeAction& action,
                               const feature::IncludeRequest& request) {
        index::SymbolQuery symbol_query;
        symbol_query.mode = index::SymbolQuery::Mode::Exact;
        symbol_query.pattern = request.name;
        for(auto segment: llvm::split(llvm::StringRef(request.scope), "::")) {
            if(!segment.empty()) {
                symbol_query.scope.push_back({.name = segment.str()});
            }
        }
        llvm::StringSet<> seen;
        std::vector<std::string> headers;
        for(const auto& located: query.locate(symbol_query)) {
            if(located.symbol.name != request.name) {
                continue;
            }
            for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
                for(const auto& site: query.sites(located.symbol.hash, kind)) {
                    if(site.file != path_id && is_header_file(site.path) &&
                       seen.insert(site.path).second) {
                        headers.push_back(site.path.str());
                    }
                }
            }
        }
        if(headers.empty()) {
            return;
        }
        std::string directory;
        std::vector<std::string> arguments;
        CommandRef ref;
        contexts
            .resolve_command(path, directory, arguments, ContextUse::Editor, nullptr, {}, {}, &ref);
        auto search = workspace.cdb.search_config(ref);
        for(const auto& header: headers) {
            auto spelling = include_spelling(header, search, path);
            if(!spelling) {
                continue;
            }
            if(auto edit = main_edit({request.offset, request.offset},
                                     std::format("#include {}\n", *spelling))) {
                out.push_back(render(std::format("Add #include {}", *spelling),
                                     action.kind,
                                     std::move(*edit)));
            }
        }
    };

    for(auto& action: result.value()) {
        if(!admits(only, feature::code_action_kind_name(action.kind))) {
            continue;
        }
        if(!action.index) {
            std::vector<protocol::TextEdit> edits;
            for(auto& replacement: action.edits) {
                if(auto converted = feature::to_range(map, replacement.range)) {
                    edits.push_back({.range = *converted, .new_text = std::move(replacement.text)});
                }
            }
            if(!edits.empty()) {
                out.push_back(render(std::move(action.title),
                                     action.kind,
                                     FileEdit{uri, session->version, std::move(edits)}));
            }
            continue;
        }
        std::visit(
            [&](const auto& request) {
                using Request = std::remove_cvref_t<decltype(request)>;
                if constexpr(std::same_as<Request, feature::DefineRequest>) {
                    resolve_define(action, request);
                } else {
                    resolve_include(action, request);
                }
            },
            *action.index);
    }
    co_return out;
}

}  // namespace clice
