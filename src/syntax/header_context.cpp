module clice;

import :support.text;
import :syntax.header_context;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.preamble;
import :syntax.scan;
import :vfs.path;

namespace clice {

namespace {

/// Emit `path` as a quoted include/marker operand, escaping backslashes
/// and quotes so Windows paths survive string-literal parsing.
void append_quoted_path(std::string& out, llvm::StringRef path) {
    out += '"';
    for(char c: path) {
        if(c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
}

/// The directives of a chain file that enter a header the resolver can
/// find: `#include` and `#include_next` naming one, not a macro spelling
/// it.
std::vector<ScanResult::IncludeInfo> includes_of(const LexicalInfo& lexical,
                                                 llvm::StringRef content) {
    std::vector<ScanResult::IncludeInfo> includes;
    for(const auto& directive: lexical.directives) {
        if(directive.kind != LexicalInfo::Directive::Kind::Include) {
            continue;
        }
        auto words = lexical.words(directive);
        auto keyword = words.front().text(content);
        if(keyword == "import" || words.size() < 2 || !words[1].is_header_name()) {
            continue;
        }
        auto name = words[1].text(content);
        includes.push_back({
            .path = name.drop_front().drop_back().str(),
            .offset = directive.range.begin,
            .name_offset = words[1].range.begin,
            .name_length = words[1].range.length(),
            .conditional = directive.depth > 0,
            .conditional_depth = static_cast<std::uint16_t>(directive.depth),
            .is_angled = name.starts_with("<"),
            .is_include_next = keyword == "include_next",
        });
    }
    return includes;
}

/// Pick the directive to cut at: the one on `line` when the include tree
/// names it and the directive does not name another file, else among
/// those resolving to `next_path`. An explicit
/// occurrence indexes them in directive order; otherwise unconditional
/// directives win over ones inside #if blocks, so an include occurrence in
/// an untaken branch does not shadow the real one.
std::optional<std::size_t> find_match(llvm::StringRef content,
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

/// Where a chain file is cut among its braces. Braces count lexically, in
/// both branches of a conditional and none a macro writes: a miscount
/// moves where the prefix splits and where the suffix ends.
struct Cut {
    /// The `#` of the directive entering the next file.
    std::uint32_t offset = 0;

    /// How many braces are open at the cut; at most zero outside every
    /// brace.
    std::int32_t braces = 0;

    /// With braces open: the last place before the cut outside every
    /// brace — past a `;`, a `}` or a directive — whose open conditional
    /// blocks are all still open at the cut, so the branches it sits in
    /// are ones the compile took; and how many blocks are open there.
    std::uint32_t top = 0;
    std::uint16_t top_conditionals = 0;
};

Cut place_cut(const LexicalInfo& lexical, llvm::StringRef content, std::uint32_t offset) {
    struct Candidate {
        std::uint32_t offset;
        llvm::SmallVector<std::uint32_t> branches;
    };

    Cut cut{.offset = offset};
    // The conditional branches open, innermost last, each by the directive
    // opening it.
    llvm::SmallVector<std::uint32_t> branches;
    std::vector<Candidate> candidates{
        {.offset = 0, .branches = {}}
    };
    auto mark = [&](std::uint32_t at) {
        if(cut.braces <= 0) {
            candidates.push_back({.offset = at, .branches = branches});
        }
    };
    auto directive = lexical.directives.begin();
    for(std::uint32_t index = 0;
        index < lexical.tokens.size() && lexical.tokens[index].range.begin < offset;
        index += 1) {
        const auto& token = lexical.tokens[index];
        if(directive != lexical.directives.end() && index == directive->first) {
            using enum LexicalInfo::Directive::Kind;
            auto id = static_cast<std::uint32_t>(directive - lexical.directives.begin());
            if(directive->kind == If) {
                branches.push_back(id);
            } else if(directive->kind == Else && !branches.empty()) {
                branches.back() = id;
            } else if(directive->kind == EndIf && !branches.empty()) {
                branches.pop_back();
            }
            mark(line_end(content, lexical.tokens[directive->last].range.begin));
            index = directive->last;
            ++directive;
            continue;
        }
        if(token.kind == clang::tok::l_brace) {
            cut.braces += 1;
        } else if(token.kind == clang::tok::r_brace) {
            cut.braces -= 1;
            mark(token.range.end);
        } else if(token.kind == clang::tok::semi) {
            mark(token.range.end);
        }
    }
    if(cut.braces > 0) {
        auto taken = ranges::find_if(views::reverse(candidates), [&](const Candidate& candidate) {
            return candidate.branches.size() <= branches.size() &&
                   std::equal(candidate.branches.begin(),
                              candidate.branches.end(),
                              branches.begin());
        });
        cut.top = taken->offset;
        cut.top_conditionals = static_cast<std::uint16_t>(taken->branches.size());
    }
    return cut;
}

/// Where the declaration `close` ends: past the declarators its line goes
/// on with and the `;` ending them (`} object;`, `} table[] = {1, 2};`),
/// else past `close` itself.
std::uint32_t declaration_end(const LexicalInfo& lexical, const Token& close) {
    std::int32_t nested = 0;
    bool first = true;
    for(const auto& next: lexical.code_from(close.range.end)) {
        if(next.is_directive_hash() ||
           (first && next.is_at_start_of_line && next.kind != clang::tok::semi)) {
            break;
        }
        first = false;
        if(next.kind == clang::tok::semi && nested == 0) {
            return next.range.end;
        }
        if(next.kind == clang::tok::l_brace) {
            nested += 1;
        } else if(next.kind == clang::tok::r_brace) {
            if(nested == 0) {
                break;
            }
            nested -= 1;
        }
    }
    return close.range.end;
}

/// Where the braces open at `cut` close, reading on from `resume`, with no
/// conditional block opened past the cut still open: where the declaration
/// the `}` closing them ends, or past the `#endif` ending the block they
/// close in; the content's end when they never do.
std::uint32_t braces_close(const LexicalInfo& lexical,
                           llvm::StringRef content,
                           const Cut& cut,
                           std::uint32_t resume) {
    auto depth = cut.braces;
    std::int32_t conditionals = 0;
    auto& tokens = lexical.tokens;
    auto first = lexical.token_at(resume);
    auto directive = ranges::partition_point(lexical.directives,
                                             [&](const auto& each) { return each.first < first; });
    for(auto index = first; index < tokens.size(); index += 1) {
        if(directive != lexical.directives.end() && index == directive->first) {
            if(directive->kind == LexicalInfo::Directive::Kind::If) {
                conditionals += 1;
            } else if(directive->kind == LexicalInfo::Directive::Kind::EndIf) {
                conditionals -= 1;
                if(depth <= 0 && conditionals <= 0) {
                    return line_end(content, tokens[directive->last].range.begin);
                }
            }
            index = directive->last;
            ++directive;
            continue;
        }
        if(tokens[index].kind == clang::tok::l_brace) {
            depth += 1;
        } else if(tokens[index].kind == clang::tok::r_brace) {
            depth -= 1;
            if(depth <= 0 && conditionals <= 0) {
                return declaration_end(lexical, tokens[index]);
            }
        }
    }
    return static_cast<std::uint32_t>(content.size());
}

/// How many conditional blocks are open at `offset`.
std::uint32_t open_conditionals(const LexicalInfo& lexical, std::uint32_t offset) {
    std::uint32_t open = 0;
    for(const auto& directive: lexical.directives) {
        if(directive.range.begin >= offset) {
            break;
        }
        if(directive.kind == LexicalInfo::Directive::Kind::If) {
            open += 1;
        } else if(directive.kind == LexicalInfo::Directive::Kind::EndIf && open > 0) {
            open -= 1;
        }
    }
    return open;
}

/// Append a #line marker for line `line` (1-based) of `path`.
void append_line_marker(std::string& out, llvm::StringRef path, std::uint32_t line) {
    out += "#line ";
    out += std::to_string(line);
    out += ' ';
    append_quoted_path(out, path);
    out += '\n';
}

/// Append content[from, from + length) to `out`, recording the run.
void copy_run(std::string& out,
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
/// FIXME: a macro-spelled include naming the target is kept verbatim too, so
/// that occurrence enters the open buffer, not the snapshot.
bool emit_fragment(std::string& out,
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
        auto begin = std::max(from, line_begin(content, include.offset));
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
std::string synthesized_path(llvm::StringRef directory, llvm::StringRef content) {
    return path::join(directory, std::format(".clice-{:016x}.h", llvm::xxh3_64bits(content)));
}

/// A fragment of a chain file being emitted: its text, and the runs of
/// the chain file it copies.
struct Fragment {
    std::string text;
    std::vector<SourceRun> runs;
};

/// Add a fragment cut from `source` to the context and return its path.
std::string add_file(SynthesizedContext& context,
                     llvm::StringRef source,
                     Fragment fragment,
                     SynthesizedFile::Entry entry = SynthesizedFile::Entry::Included) {
    auto path = synthesized_path(path::parent(source), fragment.text);
    context.files.push_back({
        .path = path,
        .content = std::move(fragment.text),
        .origin = {.source = source.str(), .runs = std::move(fragment.runs)},
        .entry = entry,
    });
    return path;
}

}  // namespace

llvm::StringRef suffix_path(const SynthesizedFiles& files) {
    for(const auto& file: files) {
        if(file.entry == SynthesizedFile::Entry::Appended) {
            return file.path;
        }
    }
    return {};
}

void append_include(std::string& text, llvm::StringRef path) {
    if(!text.empty() && !text.ends_with('\n')) {
        text += '\n';
    }
    text += "#include ";
    append_quoted_path(text, path);
    text += '\n';
}

std::optional<SynthesizedContext>
    synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                       llvm::StringRef target_path,
                       IncludeResolver resolve,
                       std::optional<std::uint32_t> occurrence,
                       std::optional<llvm::StringRef> target_content) {
    SynthesizedContext context;
    std::string snapshot_path;
    if(target_content && !has_pragma_once(*target_content, target_content->size())) {
        snapshot_path = synthesized_path(path::parent(target_path), *target_content);
    }

    // Each chain file cut at its include of the next one: the text before
    // the cut (closing the conditionals it lands in) and, inside braces,
    // the text after it (reopening them).
    llvm::SmallVector<Fragment> before;
    llvm::SmallVector<std::pair<std::size_t, Fragment>> after;
    // The part of the first chain file cut inside braces from its last
    // top-level place on (SynthesizedContext::open).
    std::optional<Fragment> open;
    std::size_t open_index = 0;
    std::optional<unsigned> found_dir;
    const auto& dialect = dialect_of(chain.empty() ? "" : chain.front().path);
    for(std::size_t i = 0; i < chain.size(); i += 1) {
        auto& entry = chain[i];
        bool is_last = i + 1 == chain.size();
        auto next_path = is_last ? target_path : chain[i + 1].path;
        auto includer_dir = path::parent(entry.path);

        auto lexical = lexical_scan(entry.content, dialect);
        auto includes = includes_of(lexical, entry.content);

        std::vector<std::optional<ResolveResult>> resolved;
        resolved.reserve(includes.size());
        for(auto& include: includes) {
            resolved.push_back(resolve(include, includer_dir, found_dir));
        }

        // The occurrence choice applies to the direct includer only.
        auto match = find_match(entry.content,
                                includes,
                                resolved,
                                next_path,
                                entry.line,
                                is_last ? occurrence : std::nullopt);
        if(!match) {
            return std::nullopt;
        }

        auto& matched = includes[*match];
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
                                              includes,
                                              resolved,
                                              target_path,
                                              snapshot_path);
        };
        auto cut = place_cut(lexical, entry.content, matched.offset);
        bool splits = !open && cut.braces > 0;
        auto& head = before.emplace_back();
        append_line_marker(head.text, entry.path, 1);
        emit(head, 0, splits ? cut.top : cut.offset);
        if(splits) {
            for(std::uint16_t d = cut.top_conditionals; d > 0; d -= 1) {
                head.text += "#endif\n";
            }
            open_index = i;
            auto& rest = open.emplace();
            for(std::uint16_t d = cut.top_conditionals; d > 0; d -= 1) {
                rest.text += "#if 1\n";
            }
            auto line =
                static_cast<std::uint32_t>(entry.content.substr(0, cut.top).count('\n')) + 1;
            append_line_marker(rest.text, entry.path, line);
            emit(rest, cut.top, cut.offset);
        }
        auto& closing = splits ? *open : head;
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            closing.text += "#endif\n";
        }

        // After the cut: past the matched directive's line, up to where the
        // braces this file opened close, the whole rest inside an earlier
        // file's, and nothing outside every brace. The focus file's text
        // always goes on with the include of the innermost fragment, even
        // an empty one: an error a scope still open raises at the end then
        // lies past the editor's text, a scope a macro opened included. The
        // text before closed `depth` conditionals early, so reopen them with
        // `#if 1`, and close those still open where the text ends. A file
        // the context did not cut can enter the header again before the
        // cut — an earlier occurrence: there the header is its text alone,
        // the includer's remainder belonging to the focus file.
        if(!open && !is_last) {
            continue;
        }
        auto resume = line_end(entry.content, cut.offset);
        auto end = splits ? braces_close(lexical, entry.content, cut, resume)
                   : open ? static_cast<std::uint32_t>(entry.content.size())
                          : resume;
        auto& tail = after.emplace_back(i, Fragment{}).second;
        if(is_last) {
            tail.text += "#if __INCLUDE_LEVEL__ == 1\n";
        }
        for(std::uint16_t d = depth; d > 0; d -= 1) {
            tail.text += "#if 1\n";
        }
        auto resume_line =
            static_cast<std::uint32_t>(entry.content.substr(0, resume).count('\n')) + 1;
        append_line_marker(tail.text, entry.path, resume_line);
        emit(tail, resume, end);
        for(auto d = open_conditionals(lexical, end); d > 0; d -= 1) {
            tail.text += "#endif\n";
        }
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
            context.open = add_file(context,
                                    chain[i - 1].path,
                                    std::move(*open),
                                    SynthesizedFile::Entry::Forced);
        }
        context.prefix =
            add_file(context,
                     chain[i - 1].path,
                     std::move(head),
                     i == 1 ? SynthesizedFile::Entry::Prefix : SynthesizedFile::Entry::Included);
    }
    std::string suffix;
    for(auto& [i, tail]: after) {
        if(!suffix.empty()) {
            append_include(tail.text, suffix);
        }
        bool is_last = i + 1 == chain.size();
        if(is_last) {
            tail.text += "#endif\n";
        }
        suffix =
            add_file(context,
                     chain[i].path,
                     std::move(tail),
                     is_last ? SynthesizedFile::Entry::Appended : SynthesizedFile::Entry::Included);
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

}  // namespace clice
