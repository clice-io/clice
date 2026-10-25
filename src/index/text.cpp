module;

#include "support/prelude.h"

module clice;

import :index.query;
import :index.search;
import :index.site;
import :index.text;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.lexer;
import :syntax.source_text;

namespace clice::index {

std::optional<DeclaredText>
    declared_text(const IndexQuery& index, SymbolHash hash, Fid anchor, RelationKind kind) {
    // Live sources first: buffer-true indexes also know symbols the project
    // table has never seen (an unsaved definition), the preamble region
    // holds the buffer's own macros, and an overlay is the only source for
    // a header seen under the live context. The first in-bounds payload
    // whose text is available wins.
    std::optional<DeclaredText> found;
    index.for_each_relation(hash,
                            anchor,
                            kind,
                            IndexQuery::Order::LiveFirst,
                            {},
                            [&](const IndexSource& source, const Relation& relation) {
                                auto extent = extent_of(source, relation);
                                if(!extent) {
                                    return true;
                                }
                                std::string kept;
                                auto text = index.source_text(source, kept);
                                if(!text) {
                                    return true;
                                }
                                auto range = extent->range;
                                found = DeclaredText{
                                    .extent = *extent,
                                    .text = std::string(text->substr(range.begin, range.length())),
                                };
                                return false;
                            });
    return found;
}

std::string doc_comment(const IndexQuery& index, SymbolHash hash, Fid anchor, bool inherit) {
    auto info = index.symbol_info(hash, anchor);
    SymbolKind kind = info ? info->kind : SymbolKind(SymbolKind::Invalid);
    // As the compiled unit's doc_comment: a namespace has none.
    if(kind == SymbolKind::Namespace) {
        return {};
    }
    // TODO: nothing is kept across lookups: every index-served hover lexes the
    // whole source again, a few ms for ordinary headers, more for large
    // generated ones.
    // Each source's text is read and its comments scanned once, however
    // many declarations it holds.
    llvm::DenseMap<std::pair<const Shard*, Fid>, std::unique_ptr<SourceText>> texts;
    auto text_of = [&](const IndexSource& source) -> SourceText* {
        auto& text = texts[{source.shard, source.file}];
        if(!text) {
            std::string kept;
            auto read = index.source_text(source, kept);
            if(!read) {
                return nullptr;
            }
            // Owned: the lexer reads past the end of what it lexes.
            text = std::make_unique<SourceText>(read->str());
            text->set_dialect(raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26));
        }
        return text.get();
    };
    DocCommentChoice choice;
    for(auto relation_kind: {RelationKind::Declaration, RelationKind::Definition}) {
        index.for_each_relation(
            hash,
            anchor,
            relation_kind,
            IndexQuery::Order::LiveFirst,
            {},
            [&](const IndexSource& source, const Relation& relation) {
                auto extent = relation.extent();
                auto* text = extent.begin < extent.end ? text_of(source) : nullptr;
                if(!text || extent.end > text->text().size()) {
                    return true;
                }
                auto content = text->text();
                auto decl = kind == SymbolKind::Macro
                                ? documented_macro(content, extent)
                                : DocumentedDecl{.range = extent,
                                                 .trailing = kind == SymbolKind::Field ||
                                                             kind == SymbolKind::EnumMember ||
                                                             kind == SymbolKind::Variable};
                return !choice.offer(content,
                                     documenting_comments(content, text->comments(), decl));
            });
        if(choice.decided()) {
            break;
        }
    }

    if(inherit && choice.empty() && (kind == SymbolKind::Method || kind == SymbolKind::Operator)) {
        for(auto& overridden: index.targets(hash, anchor, RelationKind::Interface)) {
            if(auto text = doc_comment(index, overridden.symbol, overridden.anchor, inherit);
               !text.empty()) {
                return text;
            }
        }
    }
    return std::move(choice).take();
}

auto copied_doc_targets(const IndexQuery& index, SymbolHash hash, Fid anchor, llvm::StringRef name)
    -> std::vector<IndexQuery::Located> {
    auto copied = parse_copied_name(name);
    if(!copied) {
        return {};
    }
    auto declares = [&](const IndexQuery::Located& candidate) {
        auto source = index.serving(candidate.site.file);
        std::string kept;
        auto text = source ? index.source_text(*source, kept) : std::nullopt;
        return text && declares_parameters(*text, candidate.site.range.end, *copied->parameters);
    };

    auto chain = index.container_chain(hash, anchor);
    for(auto depth = copied->absolute ? 1 : chain.size() + 1; depth > 0; depth -= 1) {
        SymbolQuery query{.mode = SymbolQuery::Mode::Exact,
                          .absolute = true,
                          .pattern = copied->parts.back().str()};
        for(auto& container: llvm::ArrayRef(chain).take_front(depth - 1)) {
            query.scope.push_back({.name = container.name});
        }
        for(auto part: llvm::ArrayRef(copied->parts).drop_back()) {
            query.scope.push_back({.name = part.str()});
        }
        auto found = locate(index, query);
        if(found.empty()) {
            continue;
        }
        if(copied->parameters && found.size() > 1) {
            std::erase_if(found, [&](const IndexQuery::Located& candidate) {
                return !declares(candidate);
            });
        }
        return found;
    }
    return {};
}

std::vector<std::string> context_lines(const IndexQuery& index, llvm::ArrayRef<Site> sites) {
    struct Text {
        std::string kept;
        std::optional<SourceText> source;
    };

    llvm::DenseMap<Fid, std::unique_ptr<Text>> texts;
    std::vector<std::string> lines;
    lines.reserve(sites.size());
    for(const auto& site: sites) {
        if(!site.file.valid()) {
            lines.emplace_back();
            continue;
        }
        auto& text = texts[site.file];
        if(!text) {
            text = std::make_unique<Text>();
            std::optional<llvm::StringRef> content;
            if(auto source = index.serving(site.file);
               source && !source->positions.text().empty()) {
                content = source->positions.text();
            } else if(auto* shard = index.project().shard(site.file)) {
                content = disk_text(site.path, *shard, text->kept);
            }
            if(content) {
                text->source.emplace(
                    *content,
                    raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26));
            }
        }
        if(!text->source || site.range.begin >= text->source->text().size()) {
            lines.emplace_back();
            continue;
        }
        lines.push_back(line_at(text->source->positions(), site.range.begin).str());
    }
    return lines;
}

}  // namespace clice::index
