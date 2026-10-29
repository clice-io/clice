module clice;

import :support.text;
import :syntax.completion;
import :syntax.dependency_graph;
import :syntax.include_resolver;
import :syntax.lexer;
import :syntax.scan;
import :vfs.path;

namespace clice {

namespace {

bool is_module_name_char(char c) {
    return is_identifier_char(c) || c == '.' || c == ':';
}

/// Clang's own include-completion filter widened by the fragment
/// extensions projects include (`.inl`, `.ipp`, `.def`, ...): header
/// extensions everywhere, extensionless files (the standard library's,
/// Qt's, frameworks') only where such headers live.
bool looks_like_header(llvm::StringRef name,
                       llvm::StringRef dir,
                       llvm::StringRef search_dir,
                       bool system) {
    auto extension = llvm::StringRef(path::extension(name)).lower();
    if(ranges::contains(std::array{".h",
                                   ".hh",
                                   ".hpp",
                                   ".hxx",
                                   ".h++",
                                   ".inc",
                                   ".inl",
                                   ".ipp",
                                   ".tcc",
                                   ".tpp",
                                   ".txx",
                                   ".def",
                                   ".cuh"},
                        extension)) {
        return true;
    }
    if(name.contains('.')) {
        return false;
    }
    auto dir_name = path::filename(dir);
    return system || dir_name.starts_with("Qt") || dir_name == "ActiveQt" ||
           dir.ends_with(".framework/Headers") || path::filename(search_dir) == "include";
}

}  // namespace

bool follows_access_operator(llvm::StringRef text,
                             std::uint32_t offset,
                             const clang::LangOptions& dialect) {
    auto line = text.substr(logical_line_begin(text, offset));
    auto end = offset - static_cast<std::uint32_t>(text.size() - line.size());
    Token before;
    Token last;
    Lexer lexer(line, dialect);
    for(auto token = lexer.advance(); !token.is_eof() && token.range.end <= end;
        token = lexer.advance()) {
        if(!token.is_eod()) {
            before = last;
            last = token;
        }
    }
    if(!last.valid() || last.range.end != end) {
        return false;
    }
    if(last.kind == clang::tok::coloncolon || last.kind == clang::tok::arrow) {
        return true;
    }
    /// A pack ellipsis being typed lexes as periods.
    return last.kind == clang::tok::period &&
           !(before.kind == clang::tok::period && before.range.end == last.range.begin);
}

std::vector<WrittenDesignator> written_designators(llvm::StringRef text,
                                                   std::uint32_t offset,
                                                   const clang::LangOptions& dialect) {
    struct Group {
        bool braces = false;
        std::vector<WrittenDesignator> designators;
    };

    std::vector<Group> groups;
    auto previous = clang::tok::unknown;
    bool directive = false;
    bool designator = false;
    bool after_field = false;
    Lexer lexer(text, dialect);
    for(auto token = lexer.advance(); !token.is_eof() && token.range.end <= offset;
        token = lexer.advance()) {
        if(token.is_directive_hash()) {
            directive = true;
        }
        if(directive) {
            directive = !token.is_eod();
            continue;
        }
        if(after_field &&
           (token.kind == clang::tok::period || token.kind == clang::tok::l_square)) {
            groups.back().designators.back().whole = false;
        }
        after_field = designator && token.is_identifier();
        if(after_field) {
            groups.back().designators.push_back({.field = token.text(text), .whole = true});
        }
        designator = token.kind == clang::tok::period && !groups.empty() && groups.back().braces &&
                     (previous == clang::tok::l_brace || previous == clang::tok::comma);
        switch(token.kind) {
            case clang::tok::l_brace: groups.push_back({.braces = true}); break;
            case clang::tok::l_paren:
            case clang::tok::l_square: groups.push_back({}); break;
            case clang::tok::r_brace:
            case clang::tok::r_paren:
            case clang::tok::r_square:
                if(!groups.empty()) {
                    groups.pop_back();
                }
                break;
            default: break;
        }
        previous = token.kind;
    }
    if(!designator) {
        return {};
    }
    return std::move(groups.back().designators);
}

CompletionName completion_name(llvm::StringRef text,
                               std::uint32_t offset,
                               const clang::LangOptions& dialect) {
    // Only a line opening with a directive, an import or a module
    // declaration has a context, and only then is the text before it lexed,
    // for the comment the line may be in.
    auto line = text.substr(logical_line_begin(text, offset));
    auto opening = Lexer(line, dialect).advance();
    auto word = opening.is_identifier() ? opening.text(line) : llvm::StringRef();
    if(!opening.is_directive_hash() && word != "import" && word != "export" && word != "module") {
        return {};
    }

    // The tokens of the logical line the cursor is on, through its end. The
    // cursor applies as a logical bound: a keyword must end at or before it
    // to count as typed.
    llvm::SmallVector<Token, 16> tokens;
    // The conditional blocks the lines before it leave open, innermost
    // last: whether each has taken its `#else`.
    llvm::SmallVector<bool, 8> open;
    {
        Lexer lexer(text, dialect);
        auto track = [&] {
            if(tokens.size() < 2 || !tokens[0].is_directive_hash() || !tokens[1].is_identifier()) {
                return;
            }
            auto name = tokens[1].text(text);
            if(name == "if" || name == "ifdef" || name == "ifndef") {
                open.push_back(false);
            } else if(name == "else" && !open.empty()) {
                open.back() = true;
            } else if(name == "endif" && !open.empty()) {
                open.pop_back();
            }
        };
        for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
            if(token.is_at_start_of_line) {
                if(token.range.begin > offset) {
                    break;
                }
                track();
                tokens.clear();
            }
            tokens.push_back(token);
        }
    }
    if(tokens.empty() || text.slice(tokens.back().range.begin, offset).contains('\n')) {
        return {};
    }
    auto before_cursor = [&](const Token& token) {
        return token.range.end <= offset;
    };
    // The line's tokens after the one at `index`, eod and all.
    auto after = [&](std::size_t index) {
        return llvm::ArrayRef(tokens).drop_front(index + 1);
    };

    auto& first = tokens.front();
    // A header name half typed from `begin`, the text after an include's
    // keyword or `__has_include(`: taken textually up to the cursor. In
    // `__has_include(` the name ends at the parenthesis closing the call.
    auto header_name = [&](std::uint32_t begin, bool parenthesized) -> CompletionName {
        // FIXME: a comment before the header name (`#include /* c */ <`) is not
        // skipped: no include completion is offered there, and a `<` trigger
        // answers empty.
        auto argument = text.slice(begin, offset).ltrim();
        CompletionName::Kind kind;
        if(argument.consume_front("\"")) {
            kind = CompletionName::Kind::IncludeQuoted;
        } else if(argument.consume_front("<")) {
            kind = CompletionName::Kind::IncludeAngled;
        } else {
            return {};
        }
        auto slash = argument.rfind('/');
        auto component = slash == llvm::StringRef::npos ? argument : argument.drop_front(slash + 1);
        // A closed directive may name a file with spaces; an unclosed one
        // ends its name at the first space.
        char closer = kind == CompletionName::Kind::IncludeQuoted ? '"' : '>';
        auto line = text.slice(offset, text.find_first_of("\r\n", offset));
        if(parenthesized) {
            line = line.take_until([](char c) { return c == ')'; });
        }
        bool closed = line.contains(closer);
        auto end = offset;
        while(end < offset + line.size() && text[end] != '/' && text[end] != closer &&
              (closed || !clang::isWhitespace(text[end]))) {
            end += 1;
        }
        return {kind,
                argument.str(),
                LocalSourceRange(offset - static_cast<std::uint32_t>(component.size()), end),
                closed};
    };

    if(first.is_directive_hash() && before_cursor(first)) {
        using enum CompletionName::Conditional;
        auto conditional = open.empty() ? None : open.back() ? Else : Open;
        if((tokens.size() < 2 || tokens[1].is_eod()) &&
           ranges::all_of(text.slice(first.range.end, offset), clang::isHorizontalWhitespace)) {
            return {
                .kind = CompletionName::Kind::Directive,
                .replace = {offset, offset},
                .conditional = conditional,
            };
        }
        if(tokens[1].is_identifier() && tokens[1].range.begin <= offset &&
           offset <= tokens[1].range.end) {
            return {
                .kind = CompletionName::Kind::Directive,
                .prefix = text.slice(tokens[1].range.begin, offset).str(),
                .replace = tokens[1].range,
                .conditional = conditional,
            };
        }
        if(!tokens[1].is_identifier() || !before_cursor(tokens[1])) {
            return {};
        }
        auto& keyword = tokens[1];
        auto name = keyword.text(text);
        CompletionName result;
        if(name == "include" || name == "include_next") {
            result = header_name(keyword.range.end, /*parenthesized=*/false);
        } else if(name == "if" || name == "elif") {
            // In a condition, a `__has_include(` whose header name the cursor
            // is in: the lexer reads a whole one as a single token.
            std::optional<std::uint32_t> header_begin;
            bool after_has_include = false;
            for(auto& token: after(1)) {
                if(token.is_eod() || !before_cursor(token)) {
                    break;
                }
                if(after_has_include && token.kind == clang::tok::l_paren) {
                    header_begin = token.range.end;
                } else if(token.is_header_name()) {
                    header_begin.reset();
                }
                after_has_include =
                    token.is_identifier() && (token.text(text) == "__has_include" ||
                                              token.text(text) == "__has_include_next");
            }
            if(header_begin) {
                result = header_name(*header_begin, /*parenthesized=*/true);
            }
        }
        result.directive = name.str();
        return result;
    }

    // `[export] import` opening a logical line always means an import
    // statement, and `module` a module declaration; lexing (instead of
    // textual matching) rules out longer identifiers like `importlib` and
    // sees through comments. An interface's `export module` declares a
    // name of its own: nothing to complete.
    bool exported = first.is_identifier() && before_cursor(first) && first.text(text) == "export";
    std::size_t at = exported ? 1 : 0;
    if(at >= tokens.size() || !tokens[at].is_identifier() || !before_cursor(tokens[at])) {
        return {};
    }
    auto& keyword = tokens[at];
    CompletionName::Kind kind;
    if(keyword.text(text) == "import") {
        kind = CompletionName::Kind::Import;
    } else if(keyword.text(text) == "module" && !exported) {
        kind = CompletionName::Kind::ModuleDeclaration;
    } else {
        return {};
    }

    // FIXME: a `;` inside a comment (`import /* ; */ m`) counts too: no module
    // names are offered after it.
    if(text.slice(first.range.begin, offset).contains(';')) {
        return {};
    }

    // `import->x` in C is a member access on a variable named `import`,
    // and `module.x` one on `module`.
    auto prefix = text.slice(keyword.range.end, offset).ltrim();
    if(!ranges::all_of(
           prefix,
           [](char c) { return is_module_name_char(c) || clang::isHorizontalWhitespace(c); }) ||
       prefix.starts_with(".")) {
        return {};
    }
    auto end = offset;
    while(end < text.size() && is_module_name_char(text[end])) {
        end += 1;
    }
    // A semicolon before the cursor ended the statement above, so one on
    // the rest of the line, past comments and attributes, closes this one.
    bool closed = ranges::any_of(after(at),
                                 [](const Token& token) { return token.kind == clang::tok::semi; });
    return {kind,
            prefix.str(),
            LocalSourceRange(offset - static_cast<std::uint32_t>(prefix.size()), end),
            closed};
}

std::vector<std::string> complete_module_declaration(const DependencyGraph& graph,
                                                     llvm::StringRef prefix) {
    std::vector<std::string> results;
    for(auto& entry: graph.modules()) {
        auto name = entry.getKey();
        if(!entry.getValue().empty() && !name.contains(':') && name.starts_with(prefix)) {
            results.push_back(name.str());
        }
    }
    ranges::sort(results);
    return results;
}

std::vector<std::string> complete_module_import(const DependencyGraph& graph,
                                                llvm::StringRef prefix,
                                                const ScanResult& unit) {
    // TODO: the graph's declarations are only refreshed on file save;
    // unsaved new module files won't appear in completions until written
    // to disk.
    auto [module, own_partition] = llvm::StringRef(unit.module_name).split(':');
    std::vector<std::string> results;
    for(auto& entry: graph.modules()) {
        if(entry.getValue().empty()) {
            continue;
        }
        // A partition is imported by its own module alone, by the partition
        // name; a unit never imports its own module or itself.
        auto [owner, partition] = entry.getKey().split(':');
        std::string name;
        if(partition.empty()) {
            if(owner == module) {
                continue;
            }
            name = owner.str();
        } else {
            if(module.empty() || owner != module || partition == own_partition ||
               (unit.is_interface_unit && graph.internal_partition(entry.getValue().front()))) {
                continue;
            }
            name = std::format(":{}", partition);
        }
        if(name.starts_with(prefix)) {
            results.push_back(std::move(name));
        }
    }
    ranges::sort(results);
    return results;
}

std::vector<IncludeCandidate> complete_include_path(const SearchConfig& config,
                                                    llvm::StringRef includer_dir,
                                                    llvm::StringRef prefix,
                                                    bool angled,
                                                    vfs::Scope& scope) {
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = prefix;
    auto slash_pos = prefix.rfind('/');
    if(slash_pos != llvm::StringRef::npos) {
        dir_prefix = prefix.slice(0, slash_pos);
        file_prefix = prefix.slice(slash_pos + 1, llvm::StringRef::npos);
    }

    std::vector<IncludeCandidate> results;
    llvm::StringSet<> seen;

    auto collect =
        [&](llvm::StringRef search_dir, bool system, std::uint32_t tier, std::uint32_t position) {
            llvm::SmallString<256> dir(search_dir);
            if(!dir_prefix.empty()) {
                path::push(dir, dir_prefix);
            }
            for(auto& entry: scope.list(dir).entries) {
                auto name = entry.getKey();
                if(!name.starts_with(file_prefix) || seen.contains(name)) {
                    continue;
                }

                bool is_dir = entry.getValue();
                if(!is_dir && !looks_like_header(name, dir, search_dir, system)) {
                    continue;
                }

                seen.insert(name);
                std::uint32_t hidden = name.starts_with(".");
                std::uint32_t reserved = name.starts_with("_");
                results.push_back({name.str(),
                                   is_dir,
                                   (hidden << 20) | (tier << 17) | (reserved << 16) | position});
            }
        };

    // A quoted include looks next to the file that contains it first.
    if(!angled) {
        collect(includer_dir, false, 0, 0);
    }
    for(unsigned i = angled ? config.angled_start_idx : 0; i < config.dirs.size(); i += 1) {
        std::uint32_t tier = i < config.angled_start_idx   ? 1
                             : i < config.system_start_idx ? 2
                             : config.dirs[i].driver       ? 4
                                                           : 3;
        collect(config.dirs[i].path, i >= config.system_start_idx, tier, i + 1);
    }

    return results;
}

}  // namespace clice
