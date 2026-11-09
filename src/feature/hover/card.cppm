/// The inside of hover its two sources share: the card of the file a
/// directive names, which the AST and the index's document links both
/// show, and how wide the card lays its types out.

module clice:feature.hover.card;

import :feature.hover;
import :semantic.symbol;
import :support.prelude;
import :support.text;
import :syntax.token;

namespace clice::feature {

/// The columns a type the card shows takes at most before it goes on over
/// more lines (display::Options::width).
constexpr std::uint32_t card_width = 60;

/// The card of the directive argument at `range` of `content`, whose
/// target is `file`.
inline HoverInfo header_card(llvm::StringRef content, LocalSourceRange range, std::string file) {
    auto name = content.substr(range.begin, range.length()).trim();
    if(name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                            (name.front() == '<' && name.back() == '>'))) {
        name = name.drop_front().drop_back();
    }

    HoverInfo info;
    info.name = unspliced(name);
    info.kind = SymbolKind::Header;
    info.file = std::move(file);
    info.symbol_range = range;
    return info;
}

}  // namespace clice::feature
