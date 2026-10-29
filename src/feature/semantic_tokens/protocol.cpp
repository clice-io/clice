module clice;

import :feature.semantic_tokens;
import :syntax.token;

namespace clice::feature {

namespace {

std::string lower_first(llvm::StringRef name) {
    auto result = name.str();
    if(!result.empty()) {
        result[0] = static_cast<char>(llvm::toLower(result[0]));
    }
    return result;
}

class Encoder {
public:
    Encoder(const PositionMap& map,
            PositionEncoding encoding,
            const SemanticTokenLegend& legend,
            protocol::SemanticTokens& output) :
        map(map), encoding(encoding), legend(legend), output(output) {}

    void append(const SemanticToken& token, std::optional<LocalSourceRange> window) {
        if(token.range.end <= token.range.begin || token.range.end > map.size()) {
            return;
        }
        auto type = legend.type_of[static_cast<std::size_t>(token.kind)];
        if(!type) {
            return;
        }
        auto modifiers = legend.bits(token.modifiers);

        if(window && (token.range.end <= window->begin || token.range.begin >= window->end)) {
            return;
        }
        auto begin = map.position(token.range.begin, encoding);
        auto end = map.position(token.range.end, encoding);

        // LSP semantic tokens have no multiline support (unless the client
        // negotiates the capability), so split the token into per-line pieces.
        for(auto line = begin->line; line <= end->line; line += 1) {
            auto bounds = *map.line_bounds(line);
            if(window &&
               ((line == end->line ? token.range.end : bounds.end) <= window->begin ||
                (line == begin->line ? token.range.begin : bounds.begin) >= window->end)) {
                continue;
            }
            auto start = line == begin->line ? begin->character : 0;
            auto stop =
                line == end->line ? end->character : map.position(bounds.end, encoding)->character;
            emit(line, start, stop - start, *type, modifiers);
        }
    }

private:
    /// Emits one LSP entry at the absolute (line, character), computing the
    /// delta against the previously emitted entry. This is the single place
    /// that reads and updates the previous-position bookkeeping.
    void emit(std::uint32_t line,
              std::uint32_t character,
              std::uint32_t token_length,
              std::uint32_t type,
              std::uint32_t modifiers) {
        if(token_length == 0) {
            return;
        }

        auto delta_line = line - last_line;
        auto delta_start = delta_line == 0 ? character - last_start_character : character;
        output.data.push_back(delta_line);
        output.data.push_back(delta_start);
        output.data.push_back(token_length);
        output.data.push_back(type);
        output.data.push_back(modifiers);

        last_line = line;
        last_start_character = character;
    }

    const PositionMap& map;
    PositionEncoding encoding;
    const SemanticTokenLegend& legend;
    protocol::SemanticTokens& output;
    std::uint32_t last_line = 0;
    std::uint32_t last_start_character = 0;
};

}  // namespace

std::string legend_name(SemanticTokenKind kind) {
    return lower_first(kota::meta::enum_name(kind));
}

std::string legend_name(SemanticTokenModifier modifier) {
    return lower_first(kota::meta::enum_name(modifier));
}

auto semantic_token_legend(const SemanticTokensOptions& options) -> SemanticTokenLegend {
    SemanticTokenLegend legend;
    auto renamed = [&](const std::map<std::string, std::string>& names, std::string name) {
        auto it = names.find(name);
        return it == names.end() ? name : it->second;
    };
    auto position = [](std::vector<std::string>& names, const std::string& name) {
        auto it = ranges::find(names, name);
        if(it == names.end()) {
            names.push_back(name);
            return static_cast<std::uint32_t>(names.size() - 1);
        }
        return static_cast<std::uint32_t>(it - names.begin());
    };

    llvm::StringSet<> known;
    for(auto kind: kota::meta::reflection<SemanticTokenKind>::member_values) {
        auto name = legend_name(kind);
        known.insert(name);
        if(kind == SemanticTokenKind::None) {
            continue;
        }
        if(auto sent = renamed(options.kinds, name); !sent.empty()) {
            legend.type_of[static_cast<std::size_t>(kind)] = position(legend.types, sent);
        }
    }
    for(auto& [name, sent]: options.kinds) {
        if(!known.contains(name) || name == "none") {
            legend.unknown.push_back(name);
        }
    }

    known.clear();
    for(auto modifier: kota::meta::reflection<SemanticTokenModifier>::member_values) {
        auto name = legend_name(modifier);
        known.insert(name);
        if(auto sent = renamed(options.modifiers, name); !sent.empty()) {
            legend.bit_of[static_cast<std::size_t>(modifier)] = std::uint32_t(1)
                                                                << position(legend.modifiers, sent);
        }
    }
    for(auto& [name, sent]: options.modifiers) {
        if(!known.contains(name)) {
            legend.unknown.push_back(name);
        }
    }
    return legend;
}

auto named_tokens(llvm::ArrayRef<SemanticToken> tokens, const SemanticTokenLegend& legend)
    -> std::vector<NamedToken> {
    std::vector<NamedToken> named;
    for(const auto& token: tokens) {
        auto type = legend.type_of[static_cast<std::size_t>(token.kind)];
        if(!type) {
            continue;
        }
        NamedToken entry{.range = token.range, .kind = legend.types[*type]};
        auto bits = legend.bits(token.modifiers);
        for(auto [position, name]: views::enumerate(legend.modifiers)) {
            if(bits & (std::uint32_t(1) << position)) {
                entry.modifiers.push_back(name);
            }
        }
        named.push_back(std::move(entry));
    }
    return named;
}

auto to_protocol(llvm::ArrayRef<SemanticToken> tokens,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 const SemanticTokenLegend& legend,
                 std::optional<LocalSourceRange> window) -> protocol::SemanticTokens {
    protocol::SemanticTokens result;
    result.data.reserve(tokens.size() * 5);

    Encoder encoder(map, encoding, legend, result);
    for(const auto& token: tokens) {
        encoder.append(token, window);
    }

    return result;
}

auto semantic_tokens_edits(llvm::ArrayRef<std::uint32_t> before,
                           llvm::ArrayRef<std::uint32_t> after)
    -> std::vector<protocol::SemanticTokensEdit> {
    std::size_t prefix = 0;
    while(prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) {
        prefix += 1;
    }
    std::size_t suffix = 0;
    while(suffix < before.size() - prefix && suffix < after.size() - prefix &&
          before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        suffix += 1;
    }
    if(prefix == before.size() && prefix == after.size()) {
        return {};
    }
    auto inserted = after.slice(prefix, after.size() - prefix - suffix);
    return {
        {
         .start = static_cast<std::uint32_t>(prefix),
         .delete_count = static_cast<std::uint32_t>(before.size() - prefix - suffix),
         .data = std::vector<std::uint32_t>(inserted.begin(), inserted.end()),
         }
    };
}

}  // namespace clice::feature
