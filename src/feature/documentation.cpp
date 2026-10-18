module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :index.query;
import :index.text;
import :semantic.display;
import :semantic.documentation;
import :support.markup;
import :syntax.documentation;

namespace clice::feature {

namespace {

/// How many `@copydoc`s deep documentation is followed: a cycle ends there.
constexpr unsigned copy_depth = 4;

Documentation read(std::string text, const DocumentationOptions& options, CopiedDocLookup lookup) {
    if(options.format == "plain") {
        return {.details = std::move(text), .verbatim = true};
    }
    return parse_documentation(text, lookup);
}

std::optional<Documentation> nothing_copied(llvm::StringRef) {
    return std::nullopt;
}

/// Read `text` as `options` say, each `@copydoc` taking the documentation
/// (`document`, one level deeper) of the first of the targets it names
/// (`targets`) that has one.
Documentation read_copying(std::string text,
                           const DocumentationOptions& options,
                           unsigned depth,
                           auto targets,
                           auto document) {
    auto copied = [&](llvm::StringRef name) -> std::optional<Documentation> {
        if(depth == copy_depth) {
            return std::nullopt;
        }
        for(const auto& target: targets(name)) {
            if(auto doc = document(target, depth + 1); !doc.empty()) {
                return doc;
            }
        }
        return std::nullopt;
    };
    return read(std::move(text), options, copied);
}

Documentation decl_documentation(CompilationUnit& unit,
                                 const clang::NamedDecl* decl,
                                 const DocumentationOptions& options,
                                 unsigned depth) {
    return read_copying(
        doc_comment(unit, decl, options.inherit),
        options,
        depth,
        [&](llvm::StringRef name) { return copied_doc_targets(unit, decl, name); },
        [&](const clang::NamedDecl* target, unsigned next) {
            return decl_documentation(unit, target, options, next);
        });
}

Documentation symbol_documentation(const index::IndexQuery& query,
                                   index::SymbolHash hash,
                                   Fid anchor,
                                   const DocumentationOptions& options,
                                   unsigned depth) {
    return read_copying(
        index::doc_comment(query, hash, anchor, options.inherit),
        options,
        depth,
        [&](llvm::StringRef name) { return index::copied_doc_targets(query, hash, anchor, name); },
        [&](const index::IndexQuery::Located& target, unsigned next) {
            return symbol_documentation(query, target.symbol.hash, target.site.file, options, next);
        });
}

}  // namespace

auto documentation(CompilationUnit& unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation {
    return decl_documentation(unit, decl, options, 0);
}

auto documentation(CompilationUnit& unit,
                   const clang::MacroInfo* macro,
                   const DocumentationOptions& options) -> Documentation {
    return read(doc_comment(unit, macro), options, nothing_copied);
}

auto documentation(const index::IndexQuery& query,
                   index::SymbolHash hash,
                   Fid anchor,
                   const DocumentationOptions& options) -> Documentation {
    // A parameter shows what its function's comment says of it, as hover's
    // card does, and a template parameter, indexed as a type or a variable,
    // what its template's says.
    auto info = query.symbol_info(hash, anchor);
    if(info && info->kind == SymbolKind::Parameter && info->parent != 0) {
        auto owner = symbol_documentation(query, info->parent, anchor, options, 0);
        const auto* param = owner.param(info->name);
        return param ? Documentation{.brief = param->text} : Documentation();
    }
    if(info && info->parent != 0 &&
       (info->kind == SymbolKind::Type || info->kind == SymbolKind::Variable)) {
        auto owner = symbol_documentation(query, info->parent, anchor, options, 0);
        if(const auto* param = owner.template_param(info->name)) {
            return Documentation{.brief = param->text};
        }
    }
    return symbol_documentation(query, hash, anchor, options, 0);
}

void describe(markup::Paragraph& entry, llvm::StringRef direction, llvm::StringRef text) {
    if(!direction.empty()) {
        entry.append_text(" (" + direction.str() + ")");
    }
    if(!text.empty()) {
        entry.append_text(" — ").append_markdown(text);
    }
}

void add_entries(markup::Document& output,
                 llvm::StringRef title,
                 llvm::ArrayRef<Documentation::Entry> entries) {
    if(entries.empty()) {
        return;
    }
    if(!title.empty()) {
        output.add_paragraph().append_text(title);
    }
    auto& items = output.add_bullet_list();
    for(const auto& entry: entries) {
        describe(items.add_item().add_paragraph().append_code(entry.name), {}, entry.text);
    }
}

void add_details(markup::Document& output, const Documentation& documentation) {
    if(!documentation.details.empty()) {
        if(documentation.verbatim) {
            output.add_lines(documentation.details);
        } else {
            output.add_markdown(documentation.details);
        }
    }
    if(!documentation.see.empty()) {
        output.add_paragraph()
            .append_text("See also: ")
            .append_markdown(llvm::join(documentation.see, ", "));
    }
}

void add_summary(markup::Document& output,
                 const Documentation& documentation,
                 std::optional<llvm::StringRef> attribute) {
    if(attribute || documentation.deprecated) {
        auto& paragraph = output.add_paragraph().append_text("Deprecated");
        if(attribute && !attribute->empty()) {
            paragraph.append_text(": " + attribute->str());
        } else if(documentation.deprecated && !documentation.deprecated->empty()) {
            paragraph.append_text(": ").append_markdown(*documentation.deprecated);
        }
    }
    if(!documentation.brief.empty()) {
        output.add_markdown(documentation.brief);
    }
}

void add_parameters(markup::Document& output,
                    llvm::StringRef title,
                    llvm::ArrayRef<Documentation::Param> documented) {
    if(documented.empty()) {
        return;
    }
    output.add_paragraph().append_text(title);
    auto& list = output.add_bullet_list();
    for(const auto& param: documented) {
        describe(list.add_item().add_paragraph().append_code(param.name),
                 param.direction,
                 param.text);
    }
}

auto present(const Documentation& documentation) -> markup::Document {
    markup::Document output;
    add_summary(output, documentation, std::nullopt);
    add_parameters(output, "Template parameters: ", documentation.template_params);
    add_parameters(output, "Parameters: ", documentation.params);
    if(!documentation.returns.empty()) {
        output.add_paragraph().append_text("Returns: ").append_markdown(documentation.returns);
    }
    add_entries(output, "Return values: ", documentation.retvals);
    add_entries(output, "Throws: ", documentation.throws);
    add_details(output, documentation);
    return output;
}

}  // namespace clice::feature
