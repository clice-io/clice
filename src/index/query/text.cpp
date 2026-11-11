module clice;

import :index.query;
import :index.query.site;
import :index.query.text;
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
