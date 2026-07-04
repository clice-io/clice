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

/// Emit `path` as a quoted include/marker operand, escaping backslashes
/// and quotes so Windows paths survive string-literal parsing.
static void append_quoted_path(std::string& out, llvm::StringRef path) {
    out += '"';
    for(char c: path) {
        if(c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
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

/// Append a #line marker for line `line` (1-based) of `path`.
static void append_line_marker_at(std::string& out, llvm::StringRef path, std::uint32_t line) {
    out += "#line ";
    out += std::to_string(line);
    out += " \"";
    for(char c: path) {
        if(c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += "\"\n";
}

/// Emit content[from, to) rewriting resolved quoted includes to absolute
/// paths: the synthesized files live in the cache directory, so
/// includer-relative lookup would resolve against the wrong location.
/// Angled includes never use the includer's directory and keep their
/// system-header semantics; #include_next is kept verbatim (its
/// search-resume semantics cannot survive relocation anyway).
static void emit_rewritten(std::string& out,
                           llvm::StringRef content,
                           std::uint32_t from,
                           std::uint32_t to,
                           llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                           llvm::ArrayRef<std::optional<std::string>> resolved,
                           llvm::StringRef target_path,
                           llvm::StringRef self_snapshot_path) {
    std::uint32_t pos = from;
    for(std::size_t j = 0; j < includes.size(); ++j) {
        auto& include = includes[j];
        if(include.name_offset < from || include.offset >= to) {
            continue;
        }
        if(!resolved[j].has_value()) {
            continue;
        }

        // Another include of the target itself (a different occurrence):
        // redirect it to the disk snapshot, or blank the directive line
        // (keeping the line count) when no snapshot is available.
        if(*resolved[j] == target_path) {
            if(!self_snapshot_path.empty()) {
                out += content.substr(pos, include.name_offset - pos);
                append_quoted_path(out, self_snapshot_path);
                pos = include.name_offset + include.name_length;
                continue;
            }
            auto line_start = content.rfind('\n', include.offset);
            auto begin = line_start == llvm::StringRef::npos
                             ? from
                             : std::max(from, static_cast<std::uint32_t>(line_start + 1));
            auto eol = content.find('\n', include.offset);
            auto end =
                eol == llvm::StringRef::npos ? to : std::min(to, static_cast<std::uint32_t>(eol));
            out += content.substr(pos, begin - pos);
            pos = end;
            continue;
        }

        if(include.is_angled || include.is_include_next) {
            continue;
        }
        out += content.substr(pos, include.name_offset - pos);
        append_quoted_path(out, *resolved[j]);
        pos = include.name_offset + include.name_length;
    }
    out += content.substr(pos, to - pos);
    if(!out.ends_with('\n')) {
        out += '\n';
    }
}

std::optional<SynthesizedContext> synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                                                     llvm::StringRef target_path,
                                                     IncludeResolver resolve,
                                                     std::optional<std::uint32_t> occurrence,
                                                     llvm::StringRef self_snapshot_path) {
    SynthesizedContext out;
    llvm::SmallVector<std::string> suffix_fragments;

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

        auto& matched = scan_result.includes[*match];
        auto cut = matched.offset;
        auto depth = matched.conditional_depth;

        // Prefix: everything before the matched directive, then balancing
        // #endifs when the cut lands inside #if blocks (most commonly an
        // include guard on an intermediate header). The guard condition is
        // still evaluated by the compiler, so the fragment's semantics hold.
        append_line_marker(out.prefix, entry.path);
        emit_rewritten(out.prefix,
                       entry.content,
                       0,
                       cut,
                       scan_result.includes,
                       resolved,
                       target_path,
                       self_snapshot_path);
        for(std::uint16_t d = depth; d > 0; --d) {
            out.prefix += "#endif\n";
        }

        // Suffix: everything after the matched directive's line, mirrored
        // (assembled innermost-first below). The prefix closed `depth`
        // conditionals early, so reopen them with `#if 1` to keep the
        // fragment's own #endifs balanced.
        auto line_end = entry.content.find('\n', cut);
        auto resume = line_end == llvm::StringRef::npos
                          ? static_cast<std::uint32_t>(entry.content.size())
                          : static_cast<std::uint32_t>(line_end + 1);
        std::string fragment;
        for(std::uint16_t d = depth; d > 0; --d) {
            fragment += "#if 1\n";
        }
        auto resume_line =
            static_cast<std::uint32_t>(entry.content.substr(0, resume).count('\n')) + 1;
        append_line_marker_at(fragment, entry.path, resume_line);
        emit_rewritten(fragment,
                       entry.content,
                       resume,
                       entry.content.size(),
                       scan_result.includes,
                       resolved,
                       target_path,
                       self_snapshot_path);
        suffix_fragments.push_back(std::move(fragment));
    }

    for(auto& fragment: llvm::reverse(suffix_fragments)) {
        out.suffix += fragment;
    }

    return out;
}

std::optional<std::string> synthesize_preamble(llvm::ArrayRef<ChainEntry> chain,
                                               llvm::StringRef target_path,
                                               IncludeResolver resolve,
                                               std::optional<std::uint32_t> occurrence) {
    auto context = synthesize_context(chain, target_path, resolve, occurrence);
    if(!context) {
        return std::nullopt;
    }
    return std::move(context->prefix);
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
