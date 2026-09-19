#include "index/symbol_query.h"

#include <algorithm>
#include <format>
#include <utility>

#include "index/serialization.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

namespace clice::index {

namespace {

using Mode = SymbolQuery::Mode;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digits(llvm::StringRef text) {
    return !text.empty() && llvm::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

/// Whether `text` ends in an operator's name, possibly with some of its
/// symbol characters: a bracket there spells the operator, not arguments.
bool ends_in_operator(llvm::StringRef text) {
    return text.rtrim("<>=-*").ends_with("operator");
}

bool has_wildcard(llvm::StringRef text) {
    return text.contains('*') || text.contains('?');
}

bool quoted(llvm::StringRef text) {
    return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}

/// Split `text` at the characters `at` accepts, outside quotes and
/// angle brackets. Quotes and brackets stay in the pieces.
std::expected<std::vector<std::string>, std::string>
    split(llvm::StringRef text, llvm::function_ref<std::size_t(llvm::StringRef)> at) {
    std::vector<std::string> pieces;
    std::string current;
    bool in_quotes = false;
    int depth = 0;
    for(std::size_t i = 0; i < text.size();) {
        char c = text[i];
        if(in_quotes) {
            in_quotes = c != '"';
            current += c;
            i += 1;
            continue;
        }
        if(c == '"') {
            in_quotes = true;
        } else if(c == '<' && !ends_in_operator(current)) {
            depth += 1;
        } else if(c == '>' && depth > 0 && !ends_in_operator(current)) {
            depth -= 1;
        } else if(depth == 0) {
            if(auto width = at(text.drop_front(i))) {
                pieces.push_back(std::move(current));
                current.clear();
                i += width;
                continue;
            }
        }
        current += c;
        i += 1;
    }
    if(in_quotes) {
        return std::unexpected("unterminated quote");
    }
    if(depth != 0) {
        return std::unexpected("unbalanced '<'");
    }
    pieces.push_back(std::move(current));
    return pieces;
}

/// A name and the arguments it ends in (`Widget<int>`), the whole text
/// as the name when it ends in none.
std::pair<llvm::StringRef, llvm::StringRef> split_args(llvm::StringRef segment) {
    if(!segment.ends_with(">")) {
        return {segment, {}};
    }
    int depth = 0;
    for(std::size_t i = segment.size(); i > 0; i -= 1) {
        char c = segment[i - 1];
        depth += c == '>' ? 1 : c == '<' ? -1 : 0;
        if(depth == 0) {
            auto name = segment.take_front(i - 1);
            if(name.empty() || ends_in_operator(name)) {
                break;
            }
            return {name, segment.drop_front(i - 1)};
        }
    }
    return {segment, {}};
}

std::optional<SymbolQuery::Position> parse_position(llvm::StringRef term) {
    auto [head, last] = term.rsplit(':');
    if(!is_digits(last) || head.empty()) {
        return std::nullopt;
    }
    SymbolQuery::Position position;
    auto [path, middle] = head.rsplit(':');
    if(is_digits(middle) && !path.empty()) {
        position.path = path.str();
        middle.getAsInteger(10, position.line);
        int column = 0;
        last.getAsInteger(10, column);
        position.column = column;
    } else {
        position.path = head.str();
        last.getAsInteger(10, position.line);
    }
    llvm::StringRef file = position.path;
    if(!file.contains('/') && !file.contains('\\') && !file.contains('.')) {
        return std::nullopt;
    }
    return position;
}

std::optional<SymbolHash> parse_handle(llvm::StringRef term) {
    SymbolHash hash = 0;
    if(!term.consume_front("#") || term.empty() || term.size() > 16 ||
       term.getAsInteger(16, hash) || reserved_key(hash)) {
        return std::nullopt;
    }
    return hash;
}

std::expected<void, std::string> parse_name(SymbolQuery& query, llvm::StringRef term) {
    // Quotes around a qualified name quote its last segment.
    if(quoted(term) && term.contains("::")) {
        auto inner = term.drop_front().drop_back();
        auto pieces = split(inner, [](llvm::StringRef rest) -> std::size_t {
            return rest.starts_with("::") ? 2 : 0;
        });
        if(!pieces) {
            return std::unexpected(pieces.error());
        }
        if(pieces->size() > 1) {
            std::string requoted;
            for(auto& piece: llvm::ArrayRef(*pieces).drop_back()) {
                requoted += piece + "::";
            }
            requoted += "\"" + pieces->back() + "\"";
            return parse_name(query, requoted);
        }
    }
    auto segments = split(term, [](llvm::StringRef rest) -> std::size_t {
        return rest.starts_with("::") ? 2 : 0;
    });
    if(!segments) {
        return std::unexpected(segments.error());
    }
    auto& pieces = *segments;
    if(pieces.size() > 1 && pieces.front().empty()) {
        query.absolute = true;
        pieces.erase(pieces.begin());
    }
    for(std::size_t i = 0; i + 1 < pieces.size(); i += 1) {
        llvm::StringRef piece = pieces[i];
        if(piece.empty() || has_wildcard(piece) || piece.contains('"')) {
            return std::unexpected(
                std::format("a scope names a container: '{}'", std::string_view(piece)));
        }
        auto [name, args] = split_args(piece);
        query.scope.push_back({.name = name.str(), .args = args.str()});
    }
    llvm::StringRef last = pieces.back();
    if(last.empty()) {
        query.mode = pieces.size() > 1 ? Mode::Members : Mode::Fuzzy;
        return {};
    }
    if(last == "*") {
        query.mode = Mode::Members;
        return {};
    }
    if(last == "**") {
        query.mode = Mode::Subtree;
        return {};
    }
    if(last.front() == '"') {
        if(!quoted(last) || last.size() == 2) {
            return std::unexpected("a quoted name needs its closing quote and a name inside");
        }
        query.mode = Mode::Exact;
        auto [name, args] = split_args(last.drop_front().drop_back());
        query.pattern = name.str();
        query.args = args.str();
        return {};
    }
    if(last.contains('"')) {
        return std::unexpected("quotes go around the whole name");
    }
    auto [name, args] = split_args(last);
    query.mode = has_wildcard(name) ? Mode::Glob : Mode::Fuzzy;
    query.pattern = name.str();
    query.args = args.str();
    return {};
}

std::string without_spaces(llvm::StringRef text) {
    std::string out;
    for(char c: text) {
        if(!is_space(c)) {
            out += c;
        }
    }
    return out;
}

std::string with_forward_slashes(llvm::StringRef path) {
    std::string out = path.str();
    std::ranges::replace(out, '\\', '/');
    return out;
}

bool segment_matches(const SymbolQuery::Segment& segment, const ScopeEntry& entry) {
    return llvm::StringRef(segment.name).equals_insensitive(entry.name) &&
           args_match(segment.args, entry.args);
}

}  // namespace

std::expected<SymbolQuery, std::string> SymbolQuery::parse(llvm::StringRef text) {
    auto terms = split(text, [](llvm::StringRef rest) -> std::size_t {
        return is_space(rest.front()) ? 1 : 0;
    });
    if(!terms) {
        return std::unexpected(terms.error());
    }
    SymbolQuery query;
    bool named = false;
    for(llvm::StringRef term: *terms) {
        if(term.empty()) {
            continue;
        }
        auto [key, value] = term.split(':');
        if(key == "kind" && !value.empty() && !value.starts_with(":")) {
            llvm::SmallVector<llvm::StringRef> names;
            value.split(names, ',', -1, false);
            for(auto name: names) {
                auto kind = parse_kind(name);
                if(!kind) {
                    return std::unexpected(
                        std::format("unknown symbol kind '{}'", std::string_view(name)));
                }
                query.kinds.push_back(*kind);
            }
            continue;
        }
        if(key == "path" && !value.empty() && !value.starts_with(":")) {
            query.paths.push_back(value.str());
            continue;
        }
        if(named) {
            return std::unexpected(
                std::format("one name per query; '{}' is a second", std::string_view(term)));
        }
        named = true;
        if(term.starts_with("#")) {
            auto handle = parse_handle(term);
            if(!handle) {
                return std::unexpected(
                    std::format("invalid symbol id '{}'", std::string_view(term)));
            }
            query.handle = handle;
            continue;
        }
        if(auto position = parse_position(term)) {
            if(position->line < 1 || position->column.value_or(1) < 1) {
                return std::unexpected("lines and columns count from 1");
            }
            query.position = std::move(position);
            continue;
        }
        if(auto parsed = parse_name(query, term); !parsed) {
            return std::unexpected(parsed.error());
        }
    }
    return query;
}

std::optional<SymbolKind> SymbolQuery::parse_kind(llvm::StringRef name) {
    constexpr auto names = kota::meta::reflection<SymbolKind::Kind>::member_names;
    for(std::size_t i = 0; i < names.size(); i += 1) {
        if(name.equals_insensitive(llvm::StringRef(names[i].data(), names[i].size()))) {
            return SymbolKind(static_cast<SymbolKind::Kind>(i));
        }
    }
    return std::nullopt;
}

bool args_match(llvm::StringRef wanted, llvm::StringRef actual) {
    return wanted.empty() || without_spaces(wanted) == without_spaces(actual);
}

bool glob_matches(llvm::StringRef pattern, llvm::StringRef name) {
    bool sensitive = llvm::any_of(pattern, [](char c) { return c >= 'A' && c <= 'Z'; });
    auto same = [&](char p, char n) {
        return sensitive ? p == n : llvm::toLower(p) == llvm::toLower(n);
    };
    std::size_t p = 0;
    std::size_t n = 0;
    std::optional<std::size_t> star;
    std::size_t resume = 0;
    while(n < name.size()) {
        if(p < pattern.size() && (pattern[p] == '?' || same(pattern[p], name[n]))) {
            p += 1;
            n += 1;
        } else if(p < pattern.size() && pattern[p] == '*') {
            star = p;
            p += 1;
            resume = n;
        } else if(star) {
            p = *star + 1;
            resume += 1;
            n = resume;
        } else {
            return false;
        }
    }
    while(p < pattern.size() && pattern[p] == '*') {
        p += 1;
    }
    return p == pattern.size();
}

llvm::SmallVector<llvm::StringRef, 4> glob_literals(llvm::StringRef pattern) {
    llvm::SmallVector<llvm::StringRef, 4> literals;
    pattern.split(literals, '*', -1, false);
    llvm::SmallVector<llvm::StringRef, 4> result;
    for(auto literal: literals) {
        llvm::SmallVector<llvm::StringRef, 4> runs;
        literal.split(runs, '?', -1, false);
        result.append(runs);
    }
    return result;
}

bool path_matches(llvm::StringRef wanted, llvm::StringRef path) {
    auto want = with_forward_slashes(wanted);
    auto have = with_forward_slashes(path);
    llvm::StringRef w = want;
    llvm::StringRef h = have;
    if(!w.contains('/')) {
        return llvm::sys::path::filename(h, llvm::sys::path::Style::posix) == w;
    }
    bool rooted = w.front() == '/' || (w.size() > 1 && w[1] == ':');
    if(rooted) {
        return w.back() == '/' ? h.starts_with(w) : h == w || h.starts_with(want + "/");
    }
    if(w.back() == '/') {
        return h.contains("/" + want);
    }
    return h.ends_with("/" + want);
}

bool segments_match(llvm::ArrayRef<SymbolQuery::Segment> segments,
                    llvm::ArrayRef<ScopeEntry> chain,
                    bool exact) {
    if(exact) {
        return segments.size() == chain.size() &&
               llvm::all_of(llvm::zip_equal(segments, chain), [](auto pair) {
                   return segment_matches(std::get<0>(pair), std::get<1>(pair));
               });
    }
    std::size_t next = 0;
    for(auto& entry: chain) {
        if(next < segments.size() && segment_matches(segments[next], entry)) {
            next += 1;
        }
    }
    return next == segments.size();
}

bool in_scope(const SymbolQuery& query, llvm::ArrayRef<ScopeEntry> chain) {
    llvm::ArrayRef<SymbolQuery::Segment> scope = query.scope;
    if(query.absolute) {
        if(query.mode == SymbolQuery::Mode::Subtree) {
            return chain.size() >= scope.size() &&
                   segments_match(scope, chain.take_front(scope.size()), true);
        }
        return segments_match(scope, chain, true);
    }
    if(query.mode == SymbolQuery::Mode::Members && !scope.empty()) {
        return !chain.empty() && segments_match(scope.take_back(1), chain.take_back(1), true) &&
               segments_match(scope.drop_back(), chain.drop_back(), false);
    }
    return segments_match(scope, chain, false);
}

}  // namespace clice::index
