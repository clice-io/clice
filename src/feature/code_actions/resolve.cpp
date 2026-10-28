module;

#include "support/prelude.h"

module clice;

import :command.file_kind;
import :feature.code_actions;
import :feature.formatting;
import :feature.protocol;
import :index.query;
import :index.search;
import :index.shard;
import :index.site;
import :index.symbol;
import :index.symbol_query;
import :index.text;
import :semantic.symbol;
import :syntax.source_text;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

namespace {

/// The texts of the definitions no other source defines yet: this TU
/// proved nothing defines them here, another TU may.
std::vector<std::string> kept_definitions(llvm::ArrayRef<DefinitionPiece> pieces,
                                          const index::IndexQuery* query,
                                          Fid file) {
    std::vector<std::string> kept;
    for(const auto& piece: pieces) {
        if(!query || !query->first_site(piece.entity, file, RelationKind::Definition)) {
            kept.push_back(piece.text);
        }
    }
    return kept;
}

/// The end of the last definition of a member of `container` in the
/// host's serving index, in the coordinates that source guarantees: the
/// relations are read directly, as an open host's own index knows its
/// definitions before the background index merges them into the project
/// index.
std::optional<index::LineColumn> after_members(const index::IndexQuery& query,
                                               Fid host,
                                               std::uint64_t container) {
    auto source = query.serving(host);
    if(!source) {
        return std::nullopt;
    }
    std::optional<index::LineColumn> after;
    source->shard->for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        if(relation.kind != RelationKind::Definition) {
            return true;
        }
        auto chain = query.container_chain(hash, source->file);
        if(ranges::none_of(chain,
                           [&](const index::SymbolInfo& each) { return each.hash == container; })) {
            return true;
        }
        auto definition = index::declared_text(query, hash, host, RelationKind::Definition);
        if(!definition) {
            return true;
        }
        auto end = definition->extent.end;
        if(!after || std::pair(end.line, end.column) > std::pair(after->line, after->column)) {
            after = end;
        }
        return true;
    });
    return after;
}

/// The headers declaring the name an IncludeRequest names, as the index
/// knows them: the files of the declarations and definitions of the
/// symbols spelling it exactly in its scope, each once; the focus file
/// `file`, sources and context headers left out.
std::vector<CanonicalRef> declaring_headers(const index::IndexQuery& query,
                                            const IncludeRequest& request,
                                            Fid file) {
    auto& files = query.file_table();
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
                if(site.file.valid() && site.file != file && is_header_path(site.path) &&
                   !is_context_header_path(site.path) &&
                   seen.insert(files.resolve(site.file)).second) {
                    headers.push_back(files.resolve(site.file));
                }
            }
        }
    }
    return headers;
}

/// One file's edits as an LSP code action.
protocol::CodeAction to_protocol(std::string title,
                                 protocol::CodeActionKind kind,
                                 protocol::TextDocumentEdit change) {
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

}  // namespace

bool admits(llvm::ArrayRef<protocol::CodeActionKind> only, llvm::StringRef kind) {
    if(only.empty()) {
        return true;
    }
    return ranges::any_of(only, [&](const protocol::CodeActionKind& wanted) {
        llvm::StringRef prefix = wanted;
        return kind == prefix || (kind.starts_with(prefix) && kind[prefix.size()] == '.');
    });
}

bool admits_any(llvm::ArrayRef<protocol::CodeActionKind> only) {
    return ranges::any_of(code_action_kinds,
                          [&](llvm::StringRef kind) { return admits(only, kind); });
}

auto resolve_actions(std::vector<CodeAction> actions,
                     llvm::ArrayRef<protocol::CodeActionKind> only,
                     const ResolveInputs& inputs) -> std::vector<ResolvedAction> {
    std::vector<ResolvedAction> resolved;
    for(auto& action: actions) {
        if(!admits(only, action.kind)) {
            continue;
        }
        if(!action.index) {
            resolved.push_back({
                .title = std::move(action.title),
                .kind = std::move(action.kind),
                .edits = std::move(action.edits),
            });
            continue;
        }
        if(auto* request = std::get_if<DefineRequest>(&*action.index)) {
            auto kept = kept_definitions(request->pieces, inputs.query, inputs.file);
            if(kept.empty()) {
                continue;
            }
            auto text = request->before + llvm::join(kept, "\n") + request->after;
            resolved.push_back({
                .title = std::move(action.title),
                .kind = std::move(action.kind),
                .edits = format_edits(inputs.path, inputs.content, {{request->range, text}}
                    ),
            });
        } else if(auto* request = std::get_if<DefineInHostRequest>(&*action.index)) {
            auto kept = kept_definitions(request->pieces, inputs.query, inputs.file);
            if(kept.empty()) {
                continue;
            }
            if(!inputs.host.valid()) {
                resolved.push_back({
                    .title = std::move(action.title),
                    .kind = std::move(action.kind),
                    .host_definitions = std::move(kept),
                });
                continue;
            }
            auto text = format_snippet(inputs.host_style_path, llvm::join(kept, "\n"));
            std::optional<HostInsertion> insertion;
            if(auto after = inputs.query && request->container != 0
                                ? after_members(*inputs.query, inputs.host, request->container)
                                : std::nullopt) {
                insertion = HostInsertion{.at = *after, .text = "\n\n" + text};
            } else if(auto* host_text = inputs.host_text()) {
                auto content = host_text->text();
                insertion = HostInsertion{
                    .at = *index::line_column(host_text->positions(),
                                              static_cast<std::uint32_t>(content.size())),
                    .text = (content.empty() || content.ends_with('\n') ? "\n" : "\n\n") + text,
                };
            } else {
                continue;
            }
            resolved.push_back({
                .title = std::format("{} in {}", action.title, path::filename(inputs.host_path)),
                .kind = std::move(action.kind),
                .host = std::move(insertion),
            });
        } else if(inputs.query) {
            auto& request = std::get<IncludeRequest>(*action.index);
            for(auto header: declaring_headers(*inputs.query, request, inputs.file)) {
                auto argument = inputs.include_argument(header);
                if(!argument) {
                    continue;
                }
                auto offset = request.insertion.offset;
                resolved.push_back({
                    .title = std::format("Add #include {}", *argument),
                    .kind = action.kind,
                    .edits = {{{offset, offset}, request.insertion.text(*argument)}},
                });
            }
        }
    }
    return resolved;
}

auto to_protocol(std::vector<ResolvedAction> actions,
                 const PositionMap& positions,
                 PositionEncoding encoding,
                 const protocol::OptionalVersionedTextDocumentIdentifier& focus,
                 const protocol::OptionalVersionedTextDocumentIdentifier& host)
    -> std::vector<protocol::CodeAction> {
    std::vector<protocol::CodeAction> result;
    for(auto& action: actions) {
        protocol::TextDocumentEdit change;
        if(action.host) {
            auto at = action.host->at.position(encoding);
            change.text_document = host;
            change.edits.emplace_back(protocol::TextEdit{
                .range = {at, at},
                .new_text = std::move(action.host->text)
            });
        } else if(action.host_definitions.empty()) {
            auto edits = to_protocol(action.edits, positions, encoding);
            if(!edits) {
                continue;
            }
            change.text_document = focus;
            for(auto& edit: *edits) {
                change.edits.emplace_back(std::move(edit));
            }
        } else {
            continue;
        }
        result.push_back(
            to_protocol(std::move(action.title), std::move(action.kind), std::move(change)));
    }
    return result;
}

}  // namespace clice::feature
