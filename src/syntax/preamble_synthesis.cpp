#include "syntax/preamble_synthesis.h"

#include <format>

#include "syntax/scan.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

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

/// Emit content[from, to) with every include of the target itself — a
/// different occurrence — redirected to the header's snapshot, or blanked
/// (keeping the line count) when there is none. Every other directive is
/// kept verbatim: each fragment sits in the directory of the file it was
/// cut from, so its includes, `__has_include` probes and macro-spelled
/// includes resolve there as they do in that file.
static void emit_fragment(std::string& out,
                          llvm::StringRef content,
                          std::uint32_t from,
                          std::uint32_t to,
                          llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                          llvm::ArrayRef<std::optional<std::string>> resolved,
                          llvm::StringRef target_path,
                          llvm::StringRef snapshot_path) {
    std::uint32_t pos = from;
    for(std::size_t j = 0; j < includes.size(); j += 1) {
        auto& include = includes[j];
        if(include.name_offset < from || include.offset >= to) {
            continue;
        }
        if(resolved[j] != target_path) {
            continue;
        }
        if(!snapshot_path.empty()) {
            out += content.substr(pos, include.name_offset - pos);
            append_quoted_path(out, snapshot_path);
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
    }
    out += content.substr(pos, to - pos);
    if(!out.ends_with('\n')) {
        out += '\n';
    }
}

/// Add a synthesized file to the context under a name derived from its
/// content, in `directory`, and return that path. The dot-prefixed name
/// cannot collide with a real header anyone includes.
static std::string add_file(SynthesizedContext& context,
                            llvm::StringRef directory,
                            std::string content) {
    llvm::SmallString<256> path(directory);
    llvm::sys::path::append(path, std::format(".clice-{:016x}.h", llvm::xxh3_64bits(content)));
    context.files.emplace_back(std::string(path), std::move(content));
    return std::string(path);
}

/// Emit an include of a synthesized file, on its own line.
static void append_include(std::string& out, llvm::StringRef path) {
    out += "#include ";
    append_quoted_path(out, path);
    out += '\n';
}

std::optional<SynthesizedContext>
    synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                       llvm::StringRef target_path,
                       IncludeResolver resolve,
                       std::optional<std::uint32_t> occurrence,
                       std::optional<llvm::StringRef> target_content) {
    SynthesizedContext context;
    std::string snapshot_path;
    if(target_content) {
        snapshot_path =
            add_file(context, llvm::sys::path::parent_path(target_path), target_content->str());
    }

    // Each chain file cut at its include of the next one: the text before
    // the cut (closing the conditionals it lands in) and the text after it
    // (reopening them).
    llvm::SmallVector<std::string> before;
    llvm::SmallVector<std::string> after;
    for(std::size_t i = 0; i < chain.size(); i += 1) {
        auto& entry = chain[i];
        bool is_last = i + 1 == chain.size();
        auto next_path = is_last ? target_path : chain[i + 1].path;
        auto includer_dir = llvm::sys::path::parent_path(entry.path);

        auto scan_result = scan_quick(entry.content);

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

        // Before the cut: everything up to the matched directive, then
        // balancing #endifs when the cut lands inside #if blocks (most
        // commonly an include guard on an intermediate header). The guard
        // condition is still evaluated by the compiler, so the fragment's
        // semantics hold.
        auto& head = before.emplace_back();
        append_line_marker(head, entry.path);
        emit_fragment(head,
                      entry.content,
                      0,
                      cut,
                      scan_result.includes,
                      resolved,
                      target_path,
                      snapshot_path);
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            head += "#endif\n";
        }

        // After the cut: everything past the matched directive's line. The
        // text before closed `depth` conditionals early, so reopen them
        // with `#if 1` to keep the fragment's own #endifs balanced.
        auto line_end = entry.content.find('\n', cut);
        auto resume = line_end == llvm::StringRef::npos
                          ? static_cast<std::uint32_t>(entry.content.size())
                          : static_cast<std::uint32_t>(line_end + 1);
        auto& tail = after.emplace_back();
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            tail += "#if 1\n";
        }
        auto resume_line =
            static_cast<std::uint32_t>(entry.content.substr(0, resume).count('\n')) + 1;
        append_line_marker_at(tail, entry.path, resume_line);
        emit_fragment(tail,
                      entry.content,
                      resume,
                      entry.content.size(),
                      scan_result.includes,
                      resolved,
                      target_path,
                      snapshot_path);
    }

    // The fragments nest the way the chain does: each one ends by
    // including the next — host first before the cut, direct includer
    // first after it — so every fragment is entered from the same place
    // its file would be.
    for(std::size_t i = chain.size(); i > 0; i -= 1) {
        auto& head = before[i - 1];
        if(!context.prefix.empty()) {
            append_include(head, context.prefix);
        }
        context.prefix =
            add_file(context, llvm::sys::path::parent_path(chain[i - 1].path), std::move(head));
    }
    for(std::size_t i = 0; i < chain.size(); i += 1) {
        auto& tail = after[i];
        if(!context.suffix.empty()) {
            append_include(tail, context.suffix);
        }
        context.suffix =
            add_file(context, llvm::sys::path::parent_path(chain[i].path), std::move(tail));
    }
    return context;
}

std::uint32_t count_include_occurrences(llvm::StringRef content,
                                        llvm::StringRef includer_path,
                                        llvm::StringRef target_path,
                                        IncludeResolver resolve) {
    auto includer_dir = llvm::sys::path::parent_path(includer_path);
    auto scan_result = scan_quick(content);

    llvm::SmallVector<std::optional<std::string>> resolved;
    resolved.reserve(scan_result.includes.size());
    for(auto& include: scan_result.includes) {
        resolved.push_back(
            resolve(include.path, include.is_angled, include.is_include_next, includer_dir));
    }

    return static_cast<std::uint32_t>(
        collect_candidates(scan_result.includes, resolved, target_path).size());
}

void SynthesizedContext::append_suffix_include(std::string& text) const {
    if(suffix.empty()) {
        return;
    }
    if(!text.ends_with('\n')) {
        text += '\n';
    }
    text += "#include \"";
    // Escape like the line markers: Windows separators must survive the
    // preprocessor's string literal parsing.
    for(char c: suffix) {
        if(c == '\\' || c == '"') {
            text += '\\';
        }
        text += c;
    }
    text += "\"\n";
}

}  // namespace clice
