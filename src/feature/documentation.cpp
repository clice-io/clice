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

Documentation read(std::string text, const DocumentationOptions& options, CopiedDocLookup lookup) {
    if(options.format == "plain") {
        return {.details = std::move(text), .verbatim = true};
    }
    return parse_documentation(text, lookup);
}

std::optional<std::string> nothing_copied(llvm::StringRef) {
    return std::nullopt;
}

}  // namespace

auto documentation(CompilationUnitRef unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation {
    auto copied = [&](llvm::StringRef name) -> std::optional<std::string> {
        for(const auto* target: copied_doc_targets(decl, name)) {
            if(auto text = doc_comment(unit, target, options.inherit); !text.empty()) {
                return text;
            }
        }
        return std::nullopt;
    };
    return read(doc_comment(unit, decl, options.inherit), options, copied);
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
    auto copied = [&](llvm::StringRef name) {
        return query.copied_doc_comment(hash, anchor, name, options.inherit);
    };
    return read(query.doc_comment(hash, anchor, options.inherit), options, copied);
}

void describe(markup::Paragraph& entry, llvm::StringRef direction, llvm::StringRef text) {
    if(!direction.empty()) {
        entry.append_text(" (" + direction.str() + ")");
    }
    if(!text.empty()) {
        entry.append_text(" — ").append_markdown(text);
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
    for(auto [title, entries]: {
            std::pair{llvm::StringRef("Return values: "), &doc.retvals},
            std::pair{llvm::StringRef("Throws: "),        &doc.throws }
    }) {
        if(entries->empty()) {
            continue;
        }
        output.add_paragraph().append_text(title);
        auto& items = output.add_bullet_list();
        for(const auto& entry: *entries) {
            describe(items.add_item().add_paragraph().append_code(entry.name), {}, entry.text);
        }
    }

    if(!doc.details.empty()) {
        if(doc.verbatim) {
            output.add_lines(doc.details);
        } else {
            output.add_markdown(doc.details);
        }
    }
    if(!doc.see.empty()) {
        output.add_paragraph().append_text("See also: ").append_markdown(llvm::join(doc.see, ", "));
    }
    return output;
}

}  // namespace clice::feature
