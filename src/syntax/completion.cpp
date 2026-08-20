#include "syntax/completion.h"

#include "syntax/include_resolver.h"
#include "syntax/lexer.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

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
        if(argument.consume_front("\"")) {
            return {CompletionContext::IncludeQuoted, argument.str()};
        }
        if(argument.consume_front("<")) {
            return {CompletionContext::IncludeAngled, argument.str()};
        }
        return {};
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

    // Only complete while the statement is still open on this line.
    auto line_end = text.find('\n', offset);
    if(line_end == llvm::StringRef::npos) {
        line_end = text.size();
    }
    if(text.slice(first.range.begin, line_end).contains(';')) {
        return {};
    }

    auto prefix = text.slice(import_keyword.range.end, offset).ltrim();
    return {CompletionContext::Import, prefix.str()};
}

std::vector<std::string>
    complete_module_import(const llvm::DenseMap<std::uint32_t, std::string>& modules,
                           llvm::StringRef prefix) {
    std::vector<std::string> results;
    // FIXME: exclude the current file's own module name from results
    // (self-import is never valid). Needs the requesting path_id passed in.
    // TODO: `modules` is only refreshed on file save; unsaved new module
    // files won't appear in completions until written to disk.
    for(auto& [path_id, module_name]: modules) {
        if(llvm::StringRef(module_name).starts_with(prefix)) {
            results.push_back(module_name);
        }
    }
    return results;
}

std::vector<IncludeCandidate> complete_include_path(const ResolvedSearchConfig& resolved,
                                                    llvm::StringRef prefix,
                                                    bool angled,
                                                    DirListingCache& dir_cache) {
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = prefix;
    auto slash_pos = prefix.rfind('/');
    if(slash_pos != llvm::StringRef::npos) {
        dir_prefix = prefix.slice(0, slash_pos);
        file_prefix = prefix.slice(slash_pos + 1, llvm::StringRef::npos);
    }

    unsigned start_idx = angled ? resolved.angled_start_idx : 0;

    std::vector<IncludeCandidate> results;
    llvm::StringSet<> seen;

    for(unsigned i = start_idx; i < resolved.dirs.size(); ++i) {
        auto& search_dir = resolved.dirs[i];

        const llvm::StringSet<>* entries = nullptr;
        if(!dir_prefix.empty()) {
            llvm::SmallString<256> sub_path(search_dir.path);
            llvm::sys::path::append(sub_path, dir_prefix);
            entries = resolve_dir(sub_path, dir_cache);
        } else {
            entries = search_dir.entries;
        }

        if(!entries)
            continue;

        for(auto& entry: *entries) {
            auto name = entry.getKey();
            if(!name.starts_with(file_prefix))
                continue;
            if(!seen.insert(name).second)
                continue;

            llvm::SmallString<256> full_path(search_dir.path);
            if(!dir_prefix.empty()) {
                llvm::sys::path::append(full_path, dir_prefix);
            }
            llvm::sys::path::append(full_path, name);

            bool is_dir = false;
            llvm::sys::fs::is_directory(llvm::Twine(full_path), is_dir);

            results.push_back({name.str(), is_dir});
        }
    }

    return results;
}

}  // namespace clice
