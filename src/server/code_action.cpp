module;

#include "modules/prelude.h"

module clice;

import :command.command;
import :index.search;
import :index.shard;
import :index.symbol_query;
import :index.text;
import :server.editor_context;
import :server.features;
import :syntax.include_resolver;
import :syntax.source_text;
import :vfs.file_system;
import :vfs.path;

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
/// file's directory (where a quoted include looks first), else the
/// shortest path below one of the command's search directories (resolved,
/// however the command spells them), angled past the quoted segment —
/// whichever of these the command's lookup order actually resolves to
/// `header`, since a shorter spelling can name a same-named file in an
/// earlier directory.
std::optional<std::string> include_spelling(CanonicalRef header,
                                            const SearchConfig& search,
                                            const Spelling& file,
                                            vfs::Scope& scope) {
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
        auto resolved =
            resolve_include(candidate.name, candidate.angled, directory, false, 0, search, scope);
        if(resolved && CanonicalPath(Spelling::absolute(resolved->path)) == header) {
            return candidate.angled ? std::format("<{}>", candidate.name)
                                    : std::format("\"{}\"", candidate.name);
        }
    }
    return std::nullopt;
}

/// Whether an include spelling names a library's internal header, which
/// its public headers include and its users never should: libstdc++'s
/// and glibc's `bits/`, libc++'s `__`-prefixed directories and files.
bool internal_header(llvm::StringRef spelling) {
    if(!spelling.starts_with('<')) {
        return false;
    }
    auto name = spelling.drop_front().drop_back();
    return llvm::any_of(
        llvm::make_range(llvm::sys::path::begin(name), llvm::sys::path::end(name)),
        [](llvm::StringRef part) { return part == "bits" || part.starts_with("__"); });
}

}  // namespace

std::optional<std::string> Features::include_for(const feature::ScopeCandidate& candidate,
                                                 bool c,
                                                 Fid file,
                                                 const SearchConfig& search,
                                                 vfs::Scope& scope) const {
    namespace stdlib = clang::tooling::stdlib;
    std::string qualifier;
    for(const auto& segment: candidate.scope) {
        qualifier += segment + "::";
    }
    if(auto symbol = stdlib::Symbol::named(qualifier,
                                           candidate.name,
                                           c ? stdlib::Lang::C : stdlib::Lang::CXX)) {
        if(auto header = symbol->header()) {
            return header->name().str();
        }
    }
    // The canonical declaration's header first, a definition when there is
    // one; then the headers declaring the symbol, for a definition in a
    // source file or in a header no `#include` can name.
    llvm::SmallVector<Fid, 2> headers;
    if(auto located = query.resolve(candidate.symbol, file);
       located && is_header_path(located->site.path)) {
        headers.push_back(located->site.file);
    }
    for(const auto& site: query.sites(candidate.symbol, file, RelationKind::Declaration)) {
        if(site.file.valid() && is_header_path(site.path) &&
           !llvm::is_contained(headers, site.file)) {
            headers.push_back(site.file);
        }
    }
    auto includer = project.file_table.spelling(file);
    for(auto header: headers) {
        if(header == file || is_context_header_path(project.file_table.resolve(header))) {
            continue;
        }
        auto spelling =
            include_spelling(project.file_table.resolve(header), search, includer, scope);
        if(spelling && !internal_header(*spelling)) {
            return spelling;
        }
    }
    return std::nullopt;
}

kota::task<std::vector<protocol::CodeAction>, kota::ipc::Error>
    Features::code_action(Ticket ticket,
                          const protocol::Range& range,
                          llvm::ArrayRef<protocol::CodeActionKind> only,
                          kota::cancellation_token token) {
    auto& session = ticket.session;
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

    auto actions = co_await dispatcher.code_actions(ticket, range, std::move(token)).or_fail();

    auto path_id = session->path_id;
    auto path = project.file_table.display(path_id);
    auto uri = feature::to_uri(path);
    auto map = session->positions();

    /// The action rendered over focus-file replacements, all of them or
    /// none: half an edit set would corrupt the buffer.
    auto emit = [&](std::string title,
                    protocol::CodeActionKind kind,
                    llvm::ArrayRef<TextReplacement> replacements) {
        std::vector<protocol::TextEdit> edits;
        for(const auto& replacement: replacements) {
            auto converted = map.range(replacement.range, PositionEncoding::UTF16);
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
        return query.first_site(entity, path_id, RelationKind::Definition).has_value();
    };

    auto resolve_define = [&](feature::CodeAction& action, const feature::DefineRequest& request) {
        if(auto text = feature::assemble_definitions(request.pieces, defined_elsewhere)) {
            emit(std::move(action.title),
                 std::move(action.kind),
                 feature::format_edits(
                     path,
                     session->text(),
                     {
                         {request.range, request.before + *text + request.after}
            }));
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

        // After the last definition of the container's members in the
        // host's serving index, in the coordinates that source guarantees;
        // at the end of the file when it holds none. The relations are
        // read directly: an open host's own index knows its definitions
        // before the background index merges them into the project index.
        std::optional<protocol::Position> after;
        auto source = request.container != 0 ? query.serving(host) : std::nullopt;
        if(source) {
            source->shard->for_each_relation([&](index::SymbolHash hash,
                                                 const index::Relation& relation) {
                if(relation.kind != RelationKind::Definition) {
                    return true;
                }
                auto chain = query.container_chain(hash, source->file);
                if(llvm::none_of(chain, [&](const index::SymbolInfo& container) {
                       return container.hash == request.container;
                   })) {
                    return true;
                }
                auto definition = index::declared_text(query, hash, host, RelationKind::Definition);
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
            std::optional<SourceText> disk;
            auto* host_text = host_session ? host_session->source.get() : nullptr;
            if(!host_text) {
                auto read = vfs::read(host_path);
                if(!read) {
                    return;
                }
                host_text = &disk.emplace((*read)->getBuffer().str());
            }
            auto content = host_text->text();
            auto end = *host_text->positions().position(static_cast<std::uint32_t>(content.size()),
                                                        PositionEncoding::UTF16);
            edit.range = {end, end};
            edit.new_text =
                (content.empty() || content.ends_with('\n') ? "\n" : "\n\n") + formatted;
        }
        out.push_back(render(
            std::format("{} in {}", action.title, llvm::sys::path::filename(host_path)),
            std::move(action.kind),
            FileEdit{
                .uri = feature::to_uri(host_path),
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
        for(const auto& located: index::locate(query, symbol_query)) {
            if(located.symbol.name != request.name) {
                continue;
            }
            for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
                for(const auto& site: query.sites(located.symbol.hash, located.site.file, kind)) {
                    if(site.file.valid() && site.file != path_id && is_header_path(site.path) &&
                       !is_context_header_path(site.path) &&
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
        auto ref = contexts.resolve_command(path_id, directory, arguments).ref;
        auto search = project.cdb.search_config(ref);
        vfs::Scope scope(project.file_table.dirs);
        for(const auto& header: headers) {
            auto spelling =
                include_spelling(header, search, project.file_table.spelling(path_id), scope);
            if(spelling && !internal_header(*spelling)) {
                emit(std::format("Add #include {}", *spelling),
                     action.kind,
                     {
                         {{request.insertion.offset, request.insertion.offset},
                          request.insertion.text(*spelling)}
                });
            }
        }
    };

    for(auto& action: actions) {
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
