#include "syntax/preamble_synthesis.h"

#include <format>
#include <vector>

#include "syntax/lexer.h"
#include "syntax/scan.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

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

/// Pick the directive to cut at: the one on `line` when the include tree
/// names it and the directive does not name another file, else among
/// those resolving to `next_path`. An explicit
/// occurrence indexes them in directive order; otherwise unconditional
/// directives win over ones inside #if blocks, so an include occurrence in
/// an untaken branch does not shadow the real one.
static std::optional<std::size_t> find_match(llvm::StringRef content,
                                             llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                                             llvm::ArrayRef<std::optional<ResolveResult>> resolved,
                                             llvm::StringRef next_path,
                                             std::uint32_t line,
                                             std::optional<std::uint32_t> occurrence) {
    // A #line in the file moves what the tree counts lines by: a directive
    // there naming another file leaves the choice to the names.
    if(line != 0) {
        for(std::size_t j = 0; j < includes.size(); j += 1) {
            if(content.substr(0, includes[j].name_offset).count('\n') + 1 == line &&
               (!resolved[j] || resolved[j]->path == next_path)) {
                return j;
            }
        }
    }
    llvm::SmallVector<std::size_t> candidates;
    for(std::size_t j = 0; j < resolved.size(); j += 1) {
        if(resolved[j] && resolved[j]->path == next_path) {
            candidates.push_back(j);
        }
    }
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

/// Where a chain file's text last stood outside every brace before its cut,
/// and how many conditional blocks are open there.
struct TopLevel {
    std::uint32_t offset = 0;
    std::uint16_t conditionals = 0;
};

/// The last place before `cut` outside every brace — past a `;`, a `}` or
/// a directive — whose open conditional blocks are all still open at
/// `cut`, so the branches it sits in are ones the compile took. Nullopt
/// when `cut` itself sits outside every brace. Braces count lexically,
/// inactive branches' too: a miscount only moves where the prefix
/// splits, never what it holds.
static std::optional<TopLevel> last_top_level(llvm::StringRef content, std::uint32_t cut) {
    struct Candidate {
        std::uint32_t offset;
        llvm::SmallVector<std::uint32_t> branches;
    };

    Lexer lexer(content);
    std::int32_t depth = 0;
    // One id per conditional branch open, innermost last.
    llvm::SmallVector<std::uint32_t> branches;
    std::uint32_t next_branch = 0;
    std::vector<Candidate> candidates{
        {.offset = 0, .branches = {}}
    };
    auto mark = [&](std::uint32_t offset) {
        if(depth <= 0) {
            candidates.push_back({.offset = offset, .branches = branches});
        }
    };
    for(auto token = lexer.advance(); !token.is_eof() && token.range.begin < cut;
        token = lexer.advance()) {
        if(token.is_directive_hash()) {
            auto keyword = lexer.advance();
            auto name = keyword.is_eod() ? llvm::StringRef() : keyword.text(content);
            if(name == "if" || name == "ifdef" || name == "ifndef") {
                branches.push_back(next_branch);
                next_branch += 1;
            } else if(name == "elif" || name == "elifdef" || name == "elifndef" || name == "else") {
                if(!branches.empty()) {
                    branches.back() = next_branch;
                    next_branch += 1;
                }
            } else if(name == "endif" && !branches.empty()) {
                branches.pop_back();
            }
            auto end = keyword.is_eod() ? keyword : lexer.advance_until(clang::tok::eod);
            auto line_end = content.find('\n', end.range.begin);
            mark(line_end == llvm::StringRef::npos ? static_cast<std::uint32_t>(content.size())
                                                   : static_cast<std::uint32_t>(line_end + 1));
            continue;
        }
        if(token.kind == clang::tok::l_brace) {
            depth += 1;
        } else if(token.kind == clang::tok::r_brace) {
            depth -= 1;
            mark(token.range.end);
        } else if(token.kind == clang::tok::semi) {
            mark(token.range.end);
        }
    }
    if(depth <= 0) {
        return std::nullopt;
    }
    auto taken = llvm::find_if(llvm::reverse(candidates), [&](const Candidate& candidate) {
        return candidate.branches.size() <= branches.size() &&
               std::equal(candidate.branches.begin(), candidate.branches.end(), branches.begin());
    });
    return TopLevel{
        .offset = taken->offset,
        .conditionals = static_cast<std::uint16_t>(taken->branches.size()),
    };
}

/// Whether the text guards itself with `#pragma once`: the compile enters
/// it once, so its other includes find it skipped.
static bool pragma_once(llvm::StringRef content) {
    Lexer lexer(content);
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(!token.is_directive_hash()) {
            continue;
        }
        auto keyword = lexer.advance();
        if(keyword.is_eod() || keyword.text(content) != "pragma") {
            continue;
        }
        auto name = lexer.advance();
        if(!name.is_eod() && name.text(content) == "once") {
            return true;
        }
    }
    return false;
}

/// Append a #line marker for line `line` (1-based) of `path`.
static void append_line_marker(std::string& out, llvm::StringRef path, std::uint32_t line) {
    out += "#line ";
    out += std::to_string(line);
    out += ' ';
    append_quoted_path(out, path);
    out += '\n';
}

/// Append content[from, from + length) to `out`, recording the run.
static void copy_run(std::string& out,
                     std::vector<SourceRun>& runs,
                     llvm::StringRef content,
                     std::uint32_t from,
                     std::uint32_t length) {
    if(length == 0) {
        return;
    }
    runs.push_back({
        .offset = static_cast<std::uint32_t>(out.size()),
        .source_offset = from,
        .length = length,
    });
    out += content.substr(from, length);
}

/// Emit content[from, to) with every include of the target itself — a
/// different occurrence — redirected to the header's snapshot, or blanked
/// (keeping the line count) when there is none. Every other directive is
/// kept verbatim: each fragment sits in the directory of the file it was
/// cut from, so its includes, `__has_include` probes and macro-spelled
/// includes resolve there as they do in that file. Returns whether the
/// fragment names the snapshot.
static bool emit_fragment(std::string& out,
                          std::vector<SourceRun>& runs,
                          llvm::StringRef content,
                          std::uint32_t from,
                          std::uint32_t to,
                          llvm::ArrayRef<ScanResult::IncludeInfo> includes,
                          llvm::ArrayRef<std::optional<ResolveResult>> resolved,
                          llvm::StringRef target_path,
                          llvm::StringRef snapshot_path) {
    std::uint32_t pos = from;
    bool names_snapshot = false;
    for(std::size_t j = 0; j < includes.size(); j += 1) {
        auto& include = includes[j];
        if(include.name_offset < from || include.offset >= to) {
            continue;
        }
        if(!resolved[j] || resolved[j]->path != target_path) {
            continue;
        }
        if(!snapshot_path.empty()) {
            copy_run(out, runs, content, pos, include.name_offset - pos);
            append_quoted_path(out, snapshot_path);
            pos = include.name_offset + include.name_length;
            names_snapshot = true;
            continue;
        }
        auto line_start = content.rfind('\n', include.offset);
        auto begin = line_start == llvm::StringRef::npos
                         ? from
                         : std::max(from, static_cast<std::uint32_t>(line_start + 1));
        auto eol = content.find('\n', include.offset);
        auto end =
            eol == llvm::StringRef::npos ? to : std::min(to, static_cast<std::uint32_t>(eol));
        copy_run(out, runs, content, pos, begin - pos);
        pos = end;
    }
    copy_run(out, runs, content, pos, to - pos);
    if(!out.ends_with('\n')) {
        out += '\n';
    }
    return names_snapshot;
}

/// The path of a synthesized file with `content` in `directory`: named by
/// the content, the dot-prefixed name cannot collide with a real header
/// anyone includes.
static std::string synthesized_path(llvm::StringRef directory, llvm::StringRef content) {
    llvm::SmallString<256> path(directory);
    llvm::sys::path::append(path,
                            llvm::sys::path::Style::posix,
                            std::format(".clice-{:016x}.h", llvm::xxh3_64bits(content)));
    return std::string(path);
}

/// A fragment of a chain file being emitted: its text, and the runs of
/// the chain file it copies.
struct Fragment {
    std::string text;
    std::vector<SourceRun> runs;
};

/// Add a fragment cut from `source` to the context and return its path.
static std::string add_file(SynthesizedContext& context,
                            llvm::StringRef source,
                            Fragment fragment) {
    auto path = synthesized_path(llvm::sys::path::parent_path(source), fragment.text);
    context.files.push_back({
        .path = path,
        .content = std::move(fragment.text),
        .origin = {.source = source.str(), .runs = std::move(fragment.runs)},
    });
    return path;
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
    if(target_content && !pragma_once(*target_content)) {
        snapshot_path =
            synthesized_path(llvm::sys::path::parent_path(target_path), *target_content);
    }

    // Each chain file cut at its include of the next one: the text before
    // the cut (closing the conditionals it lands in) and the text after it
    // (reopening them).
    llvm::SmallVector<Fragment> before;
    llvm::SmallVector<Fragment> after;
    // The part of the first chain file cut inside braces from its last
    // top-level place on (SynthesizedContext::open).
    std::optional<Fragment> open;
    std::size_t open_index = 0;
    std::optional<unsigned> found_dir;
    for(std::size_t i = 0; i < chain.size(); i += 1) {
        auto& entry = chain[i];
        bool is_last = i + 1 == chain.size();
        auto next_path = is_last ? target_path : chain[i + 1].path;
        auto includer_dir = llvm::sys::path::parent_path(entry.path);

        auto scan_result = scan_quick(entry.content);

        std::vector<std::optional<ResolveResult>> resolved;
        resolved.reserve(scan_result.includes.size());
        for(auto& include: scan_result.includes) {
            resolved.push_back(resolve(include, includer_dir, found_dir));
        }

        // The occurrence choice applies to the direct includer only.
        auto match = find_match(entry.content,
                                scan_result.includes,
                                resolved,
                                next_path,
                                entry.line,
                                is_last ? occurrence : std::nullopt);
        if(!match) {
            return std::nullopt;
        }

        auto& matched = scan_result.includes[*match];
        auto cut = matched.offset;
        auto depth = matched.conditional_depth;
        // A directive the scan cannot resolve (its name spelled by a
        // macro) was still entered there: the next one's #include_next
        // resumes nowhere known.
        found_dir = resolved[*match] ? resolved[*match]->found_dir_idx : std::nullopt;

        // Before the cut: everything up to the matched directive, then
        // balancing #endifs when the cut lands inside #if blocks (most
        // commonly an include guard on an intermediate header). The guard
        // condition is still evaluated by the compiler, so the fragment's
        // semantics hold.
        auto emit = [&](Fragment& fragment, std::uint32_t from, std::uint32_t to) {
            context.snapshot |= emit_fragment(fragment.text,
                                              fragment.runs,
                                              entry.content,
                                              from,
                                              to,
                                              scan_result.includes,
                                              resolved,
                                              target_path,
                                              snapshot_path);
        };
        std::optional<TopLevel> top;
        if(!open) {
            top = last_top_level(entry.content, cut);
        }
        auto& head = before.emplace_back();
        append_line_marker(head.text, entry.path, 1);
        emit(head, 0, top ? top->offset : cut);
        if(top) {
            for(std::uint16_t d = top->conditionals; d > 0; d -= 1) {
                head.text += "#endif\n";
            }
            open_index = i;
            auto& rest = open.emplace();
            for(std::uint16_t d = top->conditionals; d > 0; d -= 1) {
                rest.text += "#if 1\n";
            }
            auto line =
                static_cast<std::uint32_t>(entry.content.substr(0, top->offset).count('\n')) + 1;
            append_line_marker(rest.text, entry.path, line);
            emit(rest, top->offset, cut);
        }
        auto& closing = top ? *open : head;
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            closing.text += "#endif\n";
        }

        // After the cut: everything past the matched directive's line. The
        // text before closed `depth` conditionals early, so reopen them
        // with `#if 1` to keep the fragment's own #endifs balanced.
        auto line_end = entry.content.find('\n', cut);
        auto resume = line_end == llvm::StringRef::npos
                          ? static_cast<std::uint32_t>(entry.content.size())
                          : static_cast<std::uint32_t>(line_end + 1);
        // The header's buffer appends its include of the suffix, and a
        // file the context did not cut can enter the header again before
        // the cut — an earlier occurrence: there the header is its text
        // alone, the includer's remainder belonging to the main file.
        auto& tail = after.emplace_back();
        if(is_last) {
            tail.text += "#if __INCLUDE_LEVEL__ == 1\n";
        }
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            tail.text += "#if 1\n";
        }
        auto resume_line =
            static_cast<std::uint32_t>(entry.content.substr(0, resume).count('\n')) + 1;
        append_line_marker(tail.text, entry.path, resume_line);
        emit(tail, resume, entry.content.size());
    }

    // The fragments nest the way the chain does: each one ends by
    // including the next — host first before the cut, direct includer
    // first after it — so every fragment is entered from the same place
    // its file would be.
    for(std::size_t i = chain.size(); i > 0; i -= 1) {
        auto& head = before[i - 1];
        bool splits = open && i - 1 == open_index;
        if(!context.prefix.empty()) {
            append_include(splits ? open->text : head.text, context.prefix);
        }
        if(splits) {
            context.open = add_file(context, chain[i - 1].path, std::move(*open));
            context.files.back().origin.forced = true;
        }
        context.prefix = add_file(context, chain[i - 1].path, std::move(head));
    }
    for(std::size_t i = 0; i < chain.size(); i += 1) {
        auto& tail = after[i];
        if(!context.suffix.empty()) {
            append_include(tail.text, context.suffix);
        }
        if(i + 1 == chain.size()) {
            tail.text += "#endif\n";
        }
        context.suffix = add_file(context, chain[i].path, std::move(tail));
    }
    if(context.snapshot) {
        auto length = static_cast<std::uint32_t>(target_content->size());
        context.files.insert(
            context.files.begin(),
            {
                .path = snapshot_path,
                .content = target_content->str(),
                .origin = {.source = target_path.str(),
                           .runs = {{.offset = 0, .source_offset = 0, .length = length}}},
        });
    }
    return context;
}

void SynthesizedContext::append_suffix_include(std::string& text) const {
    if(suffix.empty()) {
        return;
    }
    if(!text.ends_with('\n')) {
        text += '\n';
    }
    append_include(text, suffix);
}

}  // namespace clice
