module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :syntax.lexer;

namespace clice::feature {

/// Find the range of the filename argument in a preprocessor directive line.
/// `content` is the full source text; `offset` may point at the directive,
/// operator, or inside its argument. Returns the range of the first
/// filename-like token (header name, string literal, or macro identifier)
/// containing or following the offset, or nullopt if none.
auto find_directive_argument(llvm::StringRef content,
                             std::uint32_t offset,
                             const clang::LangOptions* lang_opts)
    -> std::optional<LocalSourceRange> {
    auto lexer = Lexer::from_line(content, offset, {.lang_opts = lang_opts});
    bool directive = lexer.next().kind == clang::tok::hash;
    bool after_keyword = false;

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof() || token.is_eod()) {
            return std::nullopt;
        }

        // A filename passed through a macro argument (`#if HAS(<c.h>)`)
        // follows no keyword of its own: the offset pins its start.
        if(directive && token.range.begin == offset) {
            if(token.kind == clang::tok::string_literal) {
                return token.range;
            }
            if(token.kind == clang::tok::less) {
                for(auto close = lexer.advance(); !close.is_eod() && !close.is_eof();
                    close = lexer.advance()) {
                    if(close.kind == clang::tok::greater) {
                        return LocalSourceRange{token.range.begin, close.range.end};
                    }
                }
                return std::nullopt;
            }
        }

        if(token.is_identifier()) {
            auto text = token.text(content);
            // The __has_include family are reserved operators that may recur
            // within one #if line; every occurrence restarts the match.
            if(text == "__has_include" || text == "__has_include_next" || text == "__has_embed") {
                after_keyword = true;
                continue;
            }
            // A directive keyword only counts before the first match; a
            // later one is a macro standing in for the filename (legal
            // pre-C++20 even for one literally named `import`).
            if(!after_keyword && takes_header_name(text)) {
                after_keyword = true;
                continue;
            }
        }

        if(token.range.end <= offset || !after_keyword) {
            continue;
        }

        if(token.is_header_name() || token.kind == clang::tok::string_literal ||
           token.is_identifier()) {
            return token.range;
        }
    }
}

auto document_links(CompilationUnit& unit) -> std::vector<index::DocumentLink> {
    std::vector<index::DocumentLink> links;

    auto focus = unit.focus_file();
    auto directives_it = unit.directives().find(focus.file());
    if(directives_it == unit.directives().end()) {
        return links;
    }

    auto content = focus.content();
    auto& directives = directives_it->second;
    auto* lang_opts = &unit.lang_options();

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto offset = unit.seen_at(loc).offset;
        if(offset >= content.size())
            return;
        auto range = find_directive_argument(content, offset, lang_opts);
        if(!range)
            return;
        links.push_back(index::DocumentLink{.range = *range, .target = target.str()});
    };

    for(const auto& include: directives.includes) {
        if(include.fid.isValid()) {
            add_link(include.location, unit.file_path(include.fid));
        }
    }

    for(const auto& has_include: directives.has_includes) {
        if(has_include.file) {
            add_link(has_include.location, unit.file_path(*has_include.file));
        }
    }

    for(const auto& embed: directives.embeds) {
        if(embed.file) {
            add_link(embed.loc, unit.file_path(*embed.file));
        }
    }

    for(const auto& has_embed: directives.has_embeds) {
        if(has_embed.file) {
            add_link(has_embed.loc, unit.file_path(*has_embed.file));
        }
    }

    /// import lib.extra;
    ///        ^~~~~~~~~ links to the unit declaring the module
    /// The preprocessor reports an import before the module is loaded;
    /// the AST's import declarations carry the loaded module.
    llvm::StringMap<clang::SourceLocation> definitions;
    for(auto* decl: unit.context().local_imports()) {
        auto* module = decl->getImportedModule();
        definitions.try_emplace(module->getFullModuleName(), module->DefinitionLoc);
    }
    for(const auto& import: directives.imports) {
        auto definition = definitions.lookup(import.name);
        if(definition.isInvalid() || import.name_locations.empty()) {
            continue;
        }
        auto range = focus.produced_range(
            clang::SourceRange(import.name_locations.front(), import.name_locations.back()));
        if(!range) {
            continue;
        }
        links.push_back(index::DocumentLink{
            .range = *range,
            .target = unit.file_path(unit.file_id(definition)).str(),
        });
    }

    // Directives are collected grouped by kind; the reply promises
    // document order.
    std::ranges::sort(links, {}, [](const index::DocumentLink& link) { return link.range.begin; });

    return links;
}

}  // namespace clice::feature
