#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command/command.h"
#include "index/shard.h"
#include "index/symbol_query.h"
#include "server/editor_context.h"
#include "server/features.h"
#include "server/position.h"
#include "support/filesystem.h"
#include "syntax/include_resolver.h"

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
        return kind == prefix || (kind.starts_with(prefix) && kind[prefix.size()] == '.');
    });
}

struct FileEdit {
    std::string uri;
    std::optional<int> version;
    std::vector<protocol::TextEdit> edits;
};

protocol::CodeAction render(std::string title, protocol::CodeActionKind kind, FileEdit file) {
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
        .kind = std::move(kind),
        .edit = std::move(edit),
    };
}

/// How `header` is spelled in an include directive of `file`, the header
/// by identity and the file as the build reaches it: its path below the
/// file's directory (where a quoted include looks first), else the shortest path below one of the
/// command's search directories (resolved, however the command spells them), angled past the quoted
/// segment — whichever of these the command's lookup order actually resolves to `header`, since a
/// shorter spelling can name a same-named file in an earlier directory.
std::optional<std::string> include_spelling(CanonicalRef header,
                                            const SearchConfig& search,
                                            const Spelling& file,
                                            DirListingCache& dir_cache) {
    auto below = [&](CanonicalRef root) -> std::optional<llvm::StringRef> {
        if(root.empty() || !path::under(header, root) || header.size() <= root.size()) {
            return std::nullopt;
        }
        return llvm::StringRef(header).drop_front(root.size()).ltrim("/\\");
    };

    struct Candidate {
        llvm::StringRef name;
        bool angled;
    };

    std::vector<Candidate> candidates;
    // Quoted names start from the directory the build reaches the file
    // through, as clang's lookup does.
    auto directory = file.parent();
    if(auto relative = below(CanonicalPath(directory))) {
        candidates.push_back({*relative, false});
    }
    std::vector<CanonicalPath> dirs;
    for(auto& dir: search.dirs) {
        dirs.push_back(CanonicalPath(Spelling::absolute(dir.path)));
    }
    for(auto [index, dir]: llvm::enumerate(dirs)) {
        if(auto relative = below(dir)) {
            candidates.push_back({*relative, index >= search.angled_start_idx});
        }
    }
    std::ranges::stable_sort(candidates, {}, [](const Candidate& candidate) {
        return candidate.name.size();
    });
    for(const auto& candidate: candidates) {
        auto resolved = resolve_include(candidate.name,
                                        candidate.angled,
                                        directory,
                                        false,
                                        0,
                                        search,
                                        dir_cache);
        if(resolved && CanonicalPath(Spelling::absolute(resolved->path)) == header) {
            return candidate.angled ? std::format("<{}>", candidate.name)
                                    : std::format("\"{}\"", candidate.name);
        }
    }
    return std::nullopt;
}

}  // namespace

kota::task<std::vector<protocol::CodeAction>, kota::ipc::Error>
    Features::code_action(std::shared_ptr<Session> session,
                          const protocol::Range& range,
                          llvm::ArrayRef<protocol::CodeActionKind> only,
                          std::optional<kota::cancellation_token> token) {
    std::vector<protocol::CodeAction> out;
    if(llvm::none_of(feature::code_action_kinds,
                     [&](std::string_view kind) { return admits(only, kind); })) {
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
    auto path = project.file_table.resolve(path_id);
    auto uri = feature::to_uri(project.file_table.display(path_id));
    auto map = session->line_map();

    /// The action rendered over main-file replacements, all of them or
    /// none: half an edit set would corrupt the buffer.
    auto emit = [&](std::string title,
                    protocol::CodeActionKind kind,
                    llvm::ArrayRef<feature::TextReplacement> replacements) {
        std::vector<protocol::TextEdit> edits;
        for(const auto& replacement: replacements) {
            auto converted = feature::to_range(map, replacement.range);
            if(!converted) {
                return;
            }
            edits.push_back({.range = *converted, .new_text = replacement.text});
        }
        out.push_back(
            render(std::move(title),
                   std::move(kind),
                   FileEdit{.uri = uri, .version = session->version, .edits = std::move(edits)}));
    };
    auto defined_elsewhere = [&](std::uint64_t entity) {
        return query.first_site(entity, RelationKind::Definition).has_value();
    };

    auto resolve_define = [&](feature::CodeAction& action, const feature::DefineRequest& request) {
        if(auto text = feature::assemble_definitions(request.pieces, defined_elsewhere)) {
            emit(std::move(action.title),
                 std::move(action.kind),
                 feature::format_edits(
                     path,
                     session->text,
                     {
                         {request.range, request.before + *text + request.after}
            }));
        }
    };

    auto resolve_host = [&](feature::CodeAction& action,
                            const feature::DefineInHostRequest& request) {
        auto text = feature::assemble_definitions(request.pieces, defined_elsewhere);
        Fid host = host_of(path_id);
        if(!text || !host.valid()) {
            return;
        }
        auto host_path = project.file_table.resolve(host);
        auto host_session = sessions.find(host);
        auto formatted = feature::format_snippet(host_path, *text);

        // After the last definition of the container's members in the
        // host's serving rows, in the coordinates that source vouches
        // for; at the end of the file when it holds none. The rows are
        // read directly: an open host's own rows know its definitions
        // before the background index merges them into the project table.
        std::optional<protocol::Position> after;
        auto source = request.container != 0 ? query.serving(host) : std::nullopt;
        if(source) {
            source->rows->for_each_relation([&](index::SymbolHash hash,
                                                const index::Relation& relation) {
                if(relation.kind != RelationKind::Definition) {
                    return true;
                }
                auto chain = query.container_chain(hash);
                if(llvm::none_of(chain, [&](const index::SymbolRef& container) {
                       return container.hash == request.container;
                   })) {
                    return true;
                }
                auto definition = query.definition_text(hash);
                if(!definition) {
                    return true;
                }
                protocol::Position end{.line = definition->extent.end.line,
                                       .character = definition->extent.end.utf16_column};
                if(!after ||
                   std::pair(end.line, end.character) > std::pair(after->line, after->character)) {
                    after = end;
                }
                return true;
            });
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
                content = without_bom(*read).str();
            } else {
                return;
            }
            auto end = feature::to_position(feature::LineMap(content), content.size());
            if(!end) {
                return;
            }
            edit.range = {*end, *end};
            edit.new_text =
                (content.empty() || content.ends_with('\n') ? "\n" : "\n\n") + formatted;
        }
        out.push_back(render(
            std::format("{} in {}", action.title, llvm::sys::path::filename(host_path)),
            std::move(action.kind),
            FileEdit{
                .uri = feature::to_uri(project.file_table.display(host_path)),
                .version = host_session ? std::optional(host_session->version) : std::nullopt,
                .edits = {std::move(edit)},
            }));
    };

    auto resolve_include = [&](feature::CodeAction& action,
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
        std::vector<CanonicalRef> headers;
        for(const auto& located: query.locate(symbol_query)) {
            if(located.symbol.name != request.name) {
                continue;
            }
            for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
                for(const auto& site: query.sites(located.symbol.hash, kind)) {
                    if(site.file.valid() && site.file != path_id && is_header_path(site.path) &&
                       seen.insert(project.file_table.resolve(site.file)).second) {
                        headers.push_back(project.file_table.resolve(site.file));
                    }
                }
            }
        }
        if(headers.empty()) {
            return;
        }
        std::string directory;
        std::vector<std::string> arguments;
        auto ref = contexts.resolve_command(path, directory, arguments).ref;
        auto search = project.cdb.search_config(ref);
        DirListingCache dir_cache;
        dir_cache.shared = &project.file_table;
        llvm::StringRef text = session->text;
        std::string before = request.offset == text.size() && !text.ends_with('\n') ? "\n" : "";
        for(const auto& header: headers) {
            if(auto spelling = include_spelling(header,
                                                search,
                                                project.file_table.spelling(path_id),
                                                dir_cache)) {
                emit(std::format("Add #include {}", *spelling),
                     action.kind,
                     {
                         {{request.offset, request.offset},
                          std::format("{}#include {}\n", before, *spelling)}
                });
            }
        }
    };

    for(auto& action: result.value()) {
        if(!admits(only, action.kind)) {
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
