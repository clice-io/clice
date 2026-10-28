module;

#include "support/prelude.h"

module clice;

import :command.file_kind;
import :compile.compilation;
import :feature.code_actions.action;
import :feature.code_actions;
import :feature.formatting;
import :index.query;
import :index.search;
import :index.shard;
import :index.symbol;
import :index.symbol_query;
import :index.text;
import :semantic.decls;
import :semantic.display;
import :semantic.semantics;
import :semantic.symbol;
import :support.text;
import :syntax.lexer;
import :syntax.source_text;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

namespace action {

namespace {

constexpr std::array specs = {
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = implement_pure_virtuals,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = memberwise_constructor,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::SwitchStmt>(),
         .reach = Reach::Ancestors,
         .run = populate_switch,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::TypeLoc>(),
         .reach = Reach::Innermost,
         .run = expand_deduced_type,
         .format = false},
};

void format_actions(CompilationUnit& unit, std::vector<CodeAction>& actions, std::size_t from) {
    auto path = unit.file_path(unit.focus_file().file());
    for(auto& action: views::drop(actions, from)) {
        if(!action.edits.empty()) {
            action.edits = format_edits(path, unit.focus_file().content(), std::move(action.edits));
        }
    }
}

/// Walk the ancestor chain of the selection's innermost node and offer
/// each spec its innermost matching node, once.
void enumerate(CompilationUnit& unit,
               const SelectionTree& tree,
               bool focus_is_header,
               std::vector<CodeAction>& out) {
    const auto* innermost = tree.common_ancestor();
    if(!innermost) {
        return;
    }

    std::bitset<specs.size()> fired;
    for(const auto* node = innermost; node; node = node->parent) {
        auto kind = node->data.getNodeKind();
        for(auto [index, spec]: views::enumerate(specs)) {
            if(fired[index] || (spec.reach == Reach::Innermost && node != innermost) ||
               !spec.anchor.isBaseOf(kind)) {
                continue;
            }
            fired[index] = true;
            auto before = out.size();
            spec.run(Context{unit, *node, focus_is_header}, out);
            if(spec.format) {
                format_actions(unit, out, before);
            }
        }
    }
}

}  // namespace

}  // namespace action

auto code_actions(CompilationUnit& unit, LocalSourceRange selection) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    action::fix_its(unit, selection, out);
    action::add_include(unit, selection, out);

    auto path = unit.file_path(unit.focus_file().file());
    bool focus_is_header = is_header_path(path);
    SelectionTree::create_each(unit, selection, [&](SelectionTree tree) {
        auto before = out.size();
        action::enumerate(unit, tree, focus_is_header, out);
        return out.size() > before;
    });

    auto before = out.size();
    action::expand_macro(unit, selection, out);
    action::format_actions(unit, out, before);
    assert(ranges::all_of(out, [](const CodeAction& action) {
        return ranges::any_of(code_action_kinds,
                              [&](llvm::StringRef kind) { return kind == action.kind; });
    }));
    return out;
}

auto assemble_definitions(llvm::ArrayRef<DefinitionPiece> pieces,
                          llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::string> {
    std::string text;
    for(const auto& piece: pieces) {
        if(defined_elsewhere(piece.entity)) {
            continue;
        }
        if(!text.empty()) {
            text += '\n';
        }
        text += piece.text;
    }
    if(text.empty()) {
        return std::nullopt;
    }
    return text;
}

bool admits(llvm::ArrayRef<protocol::CodeActionKind> only, llvm::StringRef kind) {
    if(only.empty()) {
        return true;
    }
    return ranges::any_of(only, [&](const protocol::CodeActionKind& wanted) {
        llvm::StringRef prefix = wanted;
        return kind == prefix || (kind.starts_with(prefix) && kind[prefix.size()] == '.');
    });
}

protocol::CodeAction to_protocol(std::string title, protocol::CodeActionKind kind, FileEdit file) {
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

auto define_edits(const DefineRequest& request,
                  llvm::StringRef file,
                  llvm::StringRef content,
                  llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::vector<TextReplacement>> {
    auto text = assemble_definitions(request.pieces, defined_elsewhere);
    if(!text) {
        return std::nullopt;
    }
    return format_edits(file,
                        content,
                        {
                            {request.range, request.before + *text + request.after}});
}

auto host_edit(const index::IndexQuery& query,
               Fid host,
               std::uint64_t container,
               llvm::StringRef text,
               llvm::function_ref<SourceText*()> host_text,
               PositionEncoding encoding) -> std::optional<protocol::TextEdit> {
    std::optional<protocol::Position> after;
    auto source = container != 0 ? query.serving(host) : std::nullopt;
    if(source) {
        source->shard->for_each_relation(
            [&](index::SymbolHash hash, const index::Relation& relation) {
                if(relation.kind != RelationKind::Definition) {
                    return true;
                }
                auto chain = query.container_chain(hash, source->file);
                if(ranges::none_of(chain, [&](const index::SymbolInfo& each) {
                       return each.hash == container;
                   })) {
                    return true;
                }
                auto definition = index::declared_text(query, hash, host, RelationKind::Definition);
                if(!definition) {
                    return true;
                }
                auto end = definition->extent.end.position(encoding);
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
        edit.new_text = "\n\n" + text.str();
        return edit;
    }
    auto* source_text = host_text();
    if(!source_text) {
        return std::nullopt;
    }
    auto content = source_text->text();
    auto end =
        *source_text->positions().position(static_cast<std::uint32_t>(content.size()), encoding);
    edit.range = {end, end};
    edit.new_text = (content.empty() || content.ends_with('\n') ? "\n" : "\n\n") + text.str();
    return edit;
}

auto declaring_headers(const index::IndexQuery& query, const IncludeRequest& request, Fid file)
    -> std::vector<CanonicalRef> {
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

}  // namespace clice::feature
