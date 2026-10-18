module;

#include "modules/prelude.h"

module clice;

import :index.query;
import :index.search;
import :index.text;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice::index {

namespace {

std::string extract_line(llvm::StringRef content, std::uint32_t offset) {
    if(content.empty() || offset >= content.size())
        return {};
    // StringRef::rfind looks strictly before its position.
    std::size_t line_start = 0;
    if(auto pos = content.rfind('\n', offset); pos != llvm::StringRef::npos) {
        line_start = pos + 1;
    }
    auto line_end = content.find('\n', offset);
    if(line_end == llvm::StringRef::npos)
        line_end = content.size();
    return content.slice(line_start, line_end).str();
}

std::optional<DeclaredText> declared_text(const IndexQuery& index, SymbolHash hash,
                                                                  Fid anchor,
                                                                  RelationKind kind) {
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
                          std::unique_ptr<llvm::MemoryBuffer> kept;
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

}  // namespace

std::optional<DeclaredText> definition_text(const IndexQuery& index, SymbolHash hash,
                                                                    Fid anchor) {
    return declared_text(index, hash, anchor, RelationKind::Definition);
}

std::optional<DeclaredText> declaration_text(const IndexQuery& index, SymbolHash hash,
                                                                     Fid anchor) {
    return declared_text(index, hash, anchor, RelationKind::Declaration);
}

std::string doc_comment(const IndexQuery& index, SymbolHash hash, Fid anchor, bool inherit) {
    const static auto dialect = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26);
    auto info = index.symbol_info(hash, anchor);
    SymbolKind kind = info ? info->kind : SymbolKind(SymbolKind::Invalid);
    // As the compiled unit's doc_comment: a namespace has none.
    if(kind == SymbolKind::Namespace) {
        return {};
    }
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
                std::unique_ptr<llvm::MemoryBuffer> kept;
                auto text = extent.begin < extent.end ? index.source_text(source, kept) : std::nullopt;
                if(!text || extent.end > text->size()) {
                    return true;
                }
                // The lexer reads past the end of what it lexes.
                std::string content(*text);
                auto decl = kind == SymbolKind::Macro
                                ? documented_macro(content, extent)
                                : DocumentedDecl{.range = extent,
                                                 .trailing = kind == SymbolKind::Field ||
                                                             kind == SymbolKind::EnumMember ||
                                                             kind == SymbolKind::Variable};
                auto comments = scan_comments(content, dialect);
                return !choice.offer(content, documenting_comments(content, comments, decl));
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
        std::unique_ptr<llvm::MemoryBuffer> kept;
        auto text = source ? index.source_text(*source, kept) : std::nullopt;
        return text && declares_parameters(*text, candidate.site.range.end, *copied->parameters);
    };

    auto chain = index.container_chain(hash);
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
            std::erase_if(found, [&](const IndexQuery::Located& candidate) { return !declares(candidate); });
        }
        return found;
    }
    return {};
}

std::vector<std::string> context_lines(const IndexQuery& index, llvm::ArrayRef<Site> sites) {
    struct Text {
        std::optional<IndexSource> source;
        std::unique_ptr<llvm::MemoryBuffer> kept;

        llvm::StringRef get() const {
            if(source && !source->positions.text().empty()) {
                return source->positions.text();
            }
            return kept ? kept->getBuffer() : llvm::StringRef();
        }
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
            text->source = index.serving(site.file);
            if(text->get().empty()) {
                if(auto* shard = index.project().shard(site.file)) {
                    disk_text(site.path, *shard, text->kept);
                }
            }
        }
        lines.push_back(extract_line(text->get(), site.range.begin));
    }
    return lines;
}

}  // namespace clice::index
