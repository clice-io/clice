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

/// Find the directive in `includes` that brings in `next_path`. Prefers an
/// exact resolved-path match, falling back to a filename match so a
/// resolution failure doesn't kill the whole synthesis. Within each phase,
/// unconditional directives win over ones inside #if blocks: an include
/// occurrence in an untaken branch must not shadow the real one.
static std::optional<std::size_t> find_match(llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                                             llvm::ArrayRef<std::optional<std::string>> resolved,
                                             llvm::StringRef next_path) {
    std::optional<std::size_t> match;
    for(std::size_t j = 0; j < includes.size(); ++j) {
        if(resolved[j].has_value() && *resolved[j] == next_path) {
            if(!includes[j].conditional) {
                return j;
            }
            if(!match) {
                match = j;
            }
        }
    }
    if(match) {
        return match;
    }

    // Filename fallback: refuse to guess between distinct raw spellings,
    // but duplicates of the same spelling bring in the same file — take
    // the first (preferring an unconditional one).
    auto next_filename = llvm::sys::path::filename(next_path);
    for(std::size_t j = 0; j < includes.size(); ++j) {
        if(llvm::sys::path::filename(includes[j].path) != next_filename) {
            continue;
        }
        if(match.has_value() && includes[*match].path != includes[j].path) {
            return std::nullopt;
        }
        if(!match || (includes[*match].conditional && !includes[j].conditional)) {
            match = j;
        }
    }
    return match;
}

std::optional<std::string> synthesize_preamble(llvm::ArrayRef<ChainEntry> chain,
                                               llvm::StringRef target_path,
                                               IncludeResolver resolve) {
    std::string preamble;

    for(std::size_t i = 0; i < chain.size(); ++i) {
        auto& entry = chain[i];
        auto next_path = (i + 1 < chain.size()) ? chain[i + 1].path : target_path;
        auto includer_dir = llvm::sys::path::parent_path(entry.path);

        auto scan_result = scan(entry.content);

        llvm::SmallVector<std::optional<std::string>> resolved;
        resolved.reserve(scan_result.includes.size());
        for(auto& include: scan_result.includes) {
            resolved.push_back(
                resolve(include.path, include.is_angled, include.is_include_next, includer_dir));
        }

        auto match = find_match(scan_result.includes, resolved, next_path);
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

}  // namespace clice
