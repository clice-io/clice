module clice;

import :command.table;
import :support.fuzzy_matcher;
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

/// Whether a file looks like a header, by clang's own include-completion
/// filter widened by the fragment extensions projects include (`.inl`,
/// `.ipp`, `.def`, ...): a header extension anywhere, no extension (the
/// standard library's, Qt's, frameworks') only where such headers live.
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
    // A header name half typed after the token at `index`, an include's
    // keyword or the `(` of `__has_include(`: from the token after it, past
    // the comments between, taken textually up to the cursor. In
    // `__has_include(` the name ends at the parenthesis closing the call.
    auto header_name = [&](std::size_t index, bool parenthesized) -> CompletionName {
        auto begin = tokens[index].range.end;
        if(auto rest = after(index);
           !rest.empty() && !rest.front().is_eod() && rest.front().range.begin <= offset) {
            begin = rest.front().range.begin;
        }
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
            result = header_name(1, /*parenthesized=*/false);
        } else if(name == "if" || name == "elif") {
            // In a condition, a `__has_include(` whose header name the cursor
            // is in: the lexer reads a whole one as a single token.
            std::optional<std::size_t> paren;
            bool after_has_include = false;
            for(std::size_t index = 2; index < tokens.size(); index += 1) {
                auto& token = tokens[index];
                if(token.is_eod() || !before_cursor(token)) {
                    break;
                }
                if(after_has_include && token.kind == clang::tok::l_paren) {
                    paren = index;
                } else if(token.is_header_name()) {
                    paren.reset();
                }
                after_has_include =
                    token.is_identifier() && (token.text(text) == "__has_include" ||
                                              token.text(text) == "__has_include_next");
            }
            if(paren) {
                result = header_name(*paren, /*parenthesized=*/true);
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

    if(ranges::any_of(tokens, [&](const Token& token) {
           return token.kind == clang::tok::semi && before_cursor(token);
       })) {
        return {};
    }

    // `import->x` in C is a member access on a variable named `import`,
    // and `module.x` one on `module`. The name begins past the comments
    // after the keyword.
    auto begin = keyword.range.end;
    if(auto rest = after(at);
       !rest.empty() && !rest.front().is_eod() && rest.front().range.begin <= offset) {
        begin = rest.front().range.begin;
    }
    auto prefix = text.slice(begin, offset).ltrim();
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

auto module_providers(const DependencyGraph& graph, llvm::ArrayRef<OpenModule> open)
    -> ModuleProviders {
    ModuleProviders providers;
    for(auto& entry: graph.modules()) {
        for(auto file: entry.getValue()) {
            if(ranges::contains(open, file, &OpenModule::file)) {
                continue;
            }
            providers.modules[entry.getKey()].push_back(file);
            if(graph.internal_partition(file)) {
                providers.internal.insert(file);
            }
        }
    }
    for(const auto& each: open) {
        if(each.name.empty()) {
            continue;
        }
        providers.modules[each.name].push_back(each.file);
        if(each.internal) {
            providers.internal.insert(each.file);
        }
    }
    return providers;
}

std::vector<std::string> complete_module_declaration(const ModuleProviders& providers,
                                                     llvm::StringRef prefix) {
    std::vector<std::string> results;
    for(auto& entry: providers.modules) {
        auto name = entry.getKey();
        if(!name.contains(':') && name.starts_with(prefix)) {
            results.push_back(name.str());
        }
    }
    ranges::sort(results);
    return results;
}

std::vector<std::string> complete_module_import(const ModuleProviders& providers,
                                                llvm::StringRef prefix,
                                                const ScanResult& unit) {
    auto [module, own_partition] = llvm::StringRef(unit.module_name).split(':');
    std::vector<std::string> results;
    for(auto& entry: providers.modules) {
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
               (unit.is_interface_unit && providers.internal.contains(entry.getValue().front()))) {
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

    FuzzyMatcher matcher(file_prefix);
    auto builtin_headers = Spelling::absolute(path::join(resource_dir(), "include")).str();
    std::vector<IncludeCandidate> results;
    llvm::StringSet<> seen;

    auto collect =
        [&](const SearchDir& search_dir, bool system, std::uint64_t tier, std::uint64_t position) {
            // `<Foo/Bar.h>` in a framework directory is Foo.framework/Headers/Bar.h,
            // and its entries are the frameworks.
            llvm::SmallString<256> dir(search_dir.path);
            bool frameworks = search_dir.framework && dir_prefix.empty();
            if(search_dir.framework && !dir_prefix.empty()) {
                auto [name, rest] = dir_prefix.split('/');
                path::push(dir, (name + ".framework").str());
                path::push(dir, "Headers");
                if(!rest.empty()) {
                    path::push(dir, rest);
                }
            } else if(!dir_prefix.empty()) {
                path::push(dir, dir_prefix);
            }
            for(auto& entry: scope.list(dir).entries) {
                llvm::StringRef name = entry.getKey();
                bool is_dir = entry.getValue();
                if(frameworks && !(is_dir && name.consume_back(".framework"))) {
                    continue;
                }
                auto score = matcher.match(name);
                if(!score || seen.contains(name)) {
                    continue;
                }
                seen.insert(name);

                auto internal = [&] {
                    auto below = dir_prefix.empty() ? name.str() : (dir_prefix + "/" + name).str();
                    return search_dir.path == builtin_headers &&
                           builtin_header(below) == BuiltinHeader::Internal;
                };
                bool hidden = name.starts_with(".");
                bool unlikely =
                    !is_dir &&
                    (!looks_like_header(name, dir, search_dir.path, system) || internal());
                bool reserved = name.starts_with("_");
                // The score is in [0, 2]: a better match takes a smaller rank.
                auto mismatch = static_cast<std::uint64_t>((2.0f - *score) * 1000.0f);
                results.push_back({name.str(),
                                   is_dir,
                                   (std::uint64_t(hidden) << 32) | (std::uint64_t(unlikely) << 31) |
                                       (mismatch << 20) | (tier << 17) |
                                       (std::uint64_t(reserved) << 16) | position});
            }
        };

    // A quoted include looks next to the file that contains it first.
    if(!angled) {
        collect({.path = includer_dir.str()}, false, 0, 0);
    }
    for(unsigned i = angled ? config.angled_start_idx : 0; i < config.dirs.size(); i += 1) {
        std::uint64_t tier = i < config.angled_start_idx   ? 1
                             : i < config.system_start_idx ? 2
                             : config.dirs[i].driver       ? 4
                                                           : 3;
        collect(config.dirs[i], i >= config.system_start_idx, tier, i + 1);
    }

    return results;
}

}  // namespace clice
