module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.documentation;
import :feature.feature;
import :index.query;
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

Documentation decl_documentation(CompilationUnitRef unit,
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
        query.doc_comment(hash, anchor, options.inherit),
        options,
        depth,
        [&](llvm::StringRef name) { return query.copied_doc_targets(hash, anchor, name); },
        [&](const index::IndexQuery::Located& target, unsigned next) {
            return symbol_documentation(query, target.symbol.hash, target.site.file, options, next);
        });
}

}  // namespace

auto documentation(CompilationUnitRef unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation {
    return decl_documentation(unit, decl, options, 0);
}

auto documentation(CompilationUnitRef unit,
                   const clang::MacroInfo* macro,
                   const DocumentationOptions& options) -> Documentation {
    return read(doc_comment(unit, macro), options, nothing_copied);
}

auto documentation(const index::IndexQuery& query,
                   index::SymbolHash hash,
                   Fid anchor,
                   const DocumentationOptions& options) -> Documentation {
    // A parameter shows what its function's comment says of it, as hover's
    // card does.
    if(auto info = query.symbol_info(hash, anchor);
       info && info->kind == SymbolKind::Parameter && info->parent != 0) {
        auto owner = symbol_documentation(query, info->parent, anchor, options, 0);
        const auto* param = owner.param(info->name);
        return param ? Documentation{.brief = param->text} : Documentation();
    }
    return symbol_documentation(query, hash, anchor, options, 0);
}

auto parameter_names(const clang::FunctionDecl& function, unsigned index)
    -> llvm::SmallVector<std::string, 1> {
    llvm::SmallVector<std::string, 1> names;
    auto add = [&](const clang::FunctionDecl& declaration) {
        for(const auto* redecl: declaration.redecls()) {
            if(index >= redecl->getNumParams()) {
                continue;
            }
            auto name = redecl->getParamDecl(index)->getName();
            if(!name.empty() && !llvm::is_contained(names, name)) {
                names.push_back(name.str());
            }
        }
    };
    add(function);
    if(const auto* pattern = function.getTemplateInstantiationPattern();
       pattern && pattern->getNumParams() == function.getNumParams()) {
        add(*pattern);
    }
    return names;
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

auto present(const Documentation& documentation) -> markup::Document {
    markup::Document output;
    const auto& doc = documentation;
    if(doc.deprecated) {
        auto& paragraph = output.add_paragraph().append_text("Deprecated");
        if(!doc.deprecated->empty()) {
            paragraph.append_text(": ").append_markdown(*doc.deprecated);
        }
    }
    if(!doc.brief.empty()) {
        output.add_markdown(doc.brief);
    }

    auto list = [&](llvm::StringRef title, llvm::ArrayRef<Documentation::Param> params) {
        if(params.empty()) {
            return;
        }
        output.add_paragraph().append_text(title);
        auto& items = output.add_bullet_list();
        for(const auto& param: params) {
            describe(items.add_item().add_paragraph().append_code(param.name),
                     param.direction,
                     param.text);
        }
    };
    list("Template parameters: ", doc.template_params);
    list("Parameters: ", doc.params);

    if(!doc.returns.empty()) {
        output.add_paragraph().append_text("Returns: ").append_markdown(doc.returns);
    }
    add_entries(output, "Return values: ", doc.retvals);
    add_entries(output, "Throws: ", doc.throws);
    add_details(output, doc);
    return output;
}

}  // namespace clice::feature
