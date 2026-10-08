module;

#include "modules/prelude.h"

module clice;

import :syntax.completion;
import :syntax.dependency_graph;
import :syntax.include_resolver;
import :syntax.lexer;
import :syntax.scan;

namespace clice {

namespace {

/// Bytes of multi-byte UTF-8 sequences count too: clang accepts extended
/// characters in identifiers.
bool is_identifier_char(char c) {
    return clang::isAsciiIdentifierContinue(c) || static_cast<unsigned char>(c) >= 0x80;
}

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
    auto extension = llvm::sys::path::extension(name).lower();
    if(llvm::is_contained({".h",
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
    auto dir_name = llvm::sys::path::filename(dir);
    return system || dir_name.starts_with("Qt") || dir_name == "ActiveQt" ||
           dir.ends_with(".framework/Headers") ||
           llvm::sys::path::filename(search_dir) == "include";
}

}  // namespace

bool follows_access_operator(llvm::StringRef text, std::uint32_t offset) {
    auto before = text.take_front(offset);
    if(before.ends_with("::")) {
        return true;
    }
    /// `x-->y` is a postfix decrement followed by `>`.
    if(before.ends_with("->")) {
        return !before.ends_with("-->");
    }
    if(!before.ends_with(".") || before.ends_with("..")) {
        return false;
    }
    /// A dot right after a numeric literal continues the literal (`3.`).
    auto operand = before.drop_back(1);
    auto start = operand.size();
    while(start > 0 && is_identifier_char(operand[start - 1])) {
        start -= 1;
    }
    return start == operand.size() || !clang::isDigit(operand[start]);
}

PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset) {
    // TODO: cache newline offsets from incremental text updates to avoid
    // the linear line-boundary scans on every completion trigger.
    //
    // The full (NUL-terminated) text is lexed and the cursor applies as a
    // logical bound: a keyword must end at or before it to count as typed.
    auto lexer = Lexer::from_line(text, offset);
    auto before_cursor = [&](const Token& token) {
        return token.range.end <= offset;
    };

    auto first = lexer.advance();

    if(first.is_directive_hash() && before_cursor(first)) {
        auto keyword = lexer.advance();
        if(!keyword.is_identifier() || !before_cursor(keyword) || keyword.text(text) != "include") {
            return {};
        }
        // The argument is likely half-typed, so its prefix is taken
        // textually between the keyword token and the cursor.
        auto argument = text.slice(keyword.range.end, offset).ltrim();
        CompletionContext kind;
        if(argument.consume_front("\"")) {
            kind = CompletionContext::IncludeQuoted;
        } else if(argument.consume_front("<")) {
            kind = CompletionContext::IncludeAngled;
        } else {
            return {};
        }
        auto slash = argument.rfind('/');
        auto component = slash == llvm::StringRef::npos ? argument : argument.drop_front(slash + 1);
        // A closed directive may name a file with spaces; an unclosed one
        // ends its name at the first space.
        char closer = kind == CompletionContext::IncludeQuoted ? '"' : '>';
        auto line = text.slice(offset, text.find_first_of("\r\n", offset));
        bool closed = line.contains(closer);
        auto end = offset;
        while(end < offset + line.size() && text[end] != '/' && text[end] != closer &&
              (closed || !clang::isWhitespace(text[end]))) {
            end += 1;
        }
        return {kind,
                argument.str(),
                LocalSourceRange(offset - static_cast<std::uint32_t>(component.size()), end)};
    }

    // `[export] import` opening a logical line always means an import
    // statement; lexing (instead of textual matching) rules out longer
    // identifiers like `importlib` and sees through comments.
    auto import_keyword = first;
    if(first.is_identifier() && before_cursor(first) && first.text(text) == "export") {
        import_keyword = lexer.advance();
    }
    if(!import_keyword.is_identifier() || !before_cursor(import_keyword) ||
       import_keyword.text(text) != "import") {
        return {};
    }

    if(text.slice(first.range.begin, offset).contains(';')) {
        return {};
    }

    // `import->x` in C is a member access on a variable named `import`.
    auto prefix = text.slice(import_keyword.range.end, offset).ltrim();
    if(!llvm::all_of(prefix, [](char c) {
           return is_module_name_char(c) || clang::isHorizontalWhitespace(c);
       })) {
        return {};
    }
    auto end = offset;
    while(end < text.size() && is_module_name_char(text[end])) {
        end += 1;
    }
    // A semicolon before the cursor ended the statement above, so one on
    // the rest of the line, past comments and attributes, closes this one.
    bool closed = false;
    for(auto token = lexer.advance(); !token.is_eof() && !token.is_at_start_of_line;
        token = lexer.advance()) {
        if(token.kind == clang::tok::semi) {
            closed = true;
            break;
        }
    }
    return {CompletionContext::Import,
            prefix.str(),
            LocalSourceRange(offset - static_cast<std::uint32_t>(prefix.size()), end),
            closed};
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
    std::ranges::sort(results);
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

    auto collect = [&](llvm::StringRef search_dir, bool system) {
        llvm::SmallString<256> dir(search_dir);
        if(!dir_prefix.empty()) {
            llvm::sys::path::append(dir, dir_prefix);
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
            results.push_back({name.str(), is_dir});
        }
    };

    // A quoted include looks next to the file that contains it first.
    if(!angled) {
        collect(includer_dir, false);
    }
    for(unsigned i = angled ? config.angled_start_idx : 0; i < config.dirs.size(); i += 1) {
        collect(config.dirs[i].path, i >= config.system_start_idx);
    }

    return results;
}

}  // namespace clice
