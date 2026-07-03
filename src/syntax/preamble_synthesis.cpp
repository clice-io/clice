#include "syntax/preamble_synthesis.h"

#include "syntax/scan.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"

namespace clice {

/// Emit a #line marker resetting location to line 1 of `path`.
/// Backslashes and quotes are escaped so Windows paths survive the
/// round-trip through the preprocessor's string literal parsing.
static void append_line_marker(std::string& out, llvm::StringRef path) {
    out += R"(#line 1 ")";
    for(char c: path) {
        if(c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += "\"\n";
}

/// Collect all directives in `includes` that bring in `next_path`, in
/// directive order. Prefers exact resolved-path matches; when resolution
/// found nothing, falls back to filename matches — but only if all
/// fallback candidates share one raw spelling (distinct spellings could
/// name different files, so refuse to guess between them).
static llvm::SmallVector<std::size_t>
    collect_candidates(llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                       llvm::ArrayRef<std::optional<std::string>> resolved,
                       llvm::StringRef next_path) {
    llvm::SmallVector<std::size_t> candidates;
    for(std::size_t j = 0; j < includes.size(); ++j) {
        if(resolved[j].has_value() && *resolved[j] == next_path) {
            candidates.push_back(j);
        }
    }
    if(!candidates.empty()) {
        return candidates;
    }

    auto next_filename = llvm::sys::path::filename(next_path);
    for(std::size_t j = 0; j < includes.size(); ++j) {
        if(llvm::sys::path::filename(includes[j].path) != next_filename) {
            continue;
        }
        if(!candidates.empty() && includes[candidates.front()].path != includes[j].path) {
            return {};
        }
        candidates.push_back(j);
    }
    return candidates;
}

/// Pick the directive to cut at. An explicit occurrence indexes the
/// candidate list directly; otherwise unconditional directives win over
/// ones inside #if blocks, so an include occurrence in an untaken branch
/// does not shadow the real one.
static std::optional<std::size_t> find_match(llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                                             llvm::ArrayRef<std::optional<std::string>> resolved,
                                             llvm::StringRef next_path,
                                             std::optional<std::uint32_t> occurrence) {
    auto candidates = collect_candidates(includes, resolved, next_path);
    if(candidates.empty()) {
        return std::nullopt;
    }
    if(occurrence.has_value()) {
        if(*occurrence >= candidates.size()) {
            return std::nullopt;
        }
        return candidates[*occurrence];
    }
    for(auto j: candidates) {
        if(!includes[j].conditional) {
            return j;
        }
    }
    return candidates.front();
}

std::optional<std::string> synthesize_preamble(llvm::ArrayRef<ChainEntry> chain,
                                               llvm::StringRef target_path,
                                               IncludeResolver resolve,
                                               std::optional<std::uint32_t> occurrence) {
    std::string preamble;

    for(std::size_t i = 0; i < chain.size(); ++i) {
        auto& entry = chain[i];
        bool is_last = i + 1 == chain.size();
        auto next_path = is_last ? target_path : chain[i + 1].path;
        auto includer_dir = llvm::sys::path::parent_path(entry.path);

        auto scan_result = scan(entry.content);

        llvm::SmallVector<std::optional<std::string>> resolved;
        resolved.reserve(scan_result.includes.size());
        for(auto& include: scan_result.includes) {
            resolved.push_back(
                resolve(include.path, include.is_angled, include.is_include_next, includer_dir));
        }

        // The occurrence choice applies to the direct includer only.
        auto match = find_match(scan_result.includes,
                                resolved,
                                next_path,
                                is_last ? occurrence : std::nullopt);
        if(!match) {
            return std::nullopt;
        }

        auto cut = scan_result.includes[*match].offset;
        append_line_marker(preamble, entry.path);

        // Emit the fragment before the cut, rewriting resolved quoted
        // includes to absolute paths: the preamble file lives in the cache
        // directory, so includer-relative lookup would resolve against the
        // wrong location. Angled includes never use the includer's directory
        // and keep their system-header semantics by staying untouched.
        // #include_next is kept verbatim too; its search-resume semantics
        // are wrong from the cache directory, but rewriting can't fix that.
        std::uint32_t pos = 0;
        for(std::size_t j = 0; j < *match; ++j) {
            auto& include = scan_result.includes[j];
            if(include.is_angled || include.is_include_next || !resolved[j].has_value()) {
                continue;
            }
            preamble += entry.content.substr(pos, include.name_offset - pos);
            preamble += '"';
            preamble += *resolved[j];
            preamble += '"';
            pos = include.name_offset + include.name_length;
        }
        preamble += entry.content.substr(pos, cut - pos);
        if(!preamble.ends_with('\n')) {
            preamble += '\n';
        }

        // The cut may land inside #if blocks — most commonly a classic
        // include guard on an intermediate header. Close them so the
        // preamble stays well-formed; the guard condition itself is still
        // evaluated by the compiler, so the fragment's semantics hold.
        for(std::uint16_t depth = scan_result.includes[*match].conditional_depth; depth > 0;
            --depth) {
            preamble += "#endif\n";
        }
    }

    return preamble;
}

std::uint32_t count_include_occurrences(llvm::StringRef content,
                                        llvm::StringRef includer_path,
                                        llvm::StringRef target_path,
                                        IncludeResolver resolve) {
    auto includer_dir = llvm::sys::path::parent_path(includer_path);
    auto scan_result = scan(content);

    llvm::SmallVector<std::optional<std::string>> resolved;
    resolved.reserve(scan_result.includes.size());
    for(auto& include: scan_result.includes) {
        resolved.push_back(
            resolve(include.path, include.is_angled, include.is_include_next, includer_dir));
    }

    return static_cast<std::uint32_t>(
        collect_candidates(scan_result.includes, resolved, target_path).size());
}

}  // namespace clice
