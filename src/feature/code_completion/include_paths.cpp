module clice;

import :command.search_config;
import :command.table;
import :feature.code_completion.cursor;
import :feature.code_completion.include_paths;
import :feature.code_completion.ranking;
import :support.fuzzy_matcher;
import :syntax.position;
import :vfs.dir_cache;
import :vfs.path;

namespace clice::feature {

namespace {

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

/// Entry in the include path completion result.
struct IncludeCandidate {
    std::string name;
    bool is_directory = false;

    /// Its rank, ascending. A hidden name (`.git/`) follows all the others,
    /// and a file no header would be named as (`README.md`, `main.cpp`), or
    /// a builtin header that must not be included directly, the rest. Then
    /// the better the typed name matches: the whole name, a prefix, words of
    /// it (`v` finds `string_view`). Then where the search found it: the
    /// includer's directory, the quoted then the angled search directories,
    /// the system ones the command names, last the toolchain's own; in
    /// search order within each, after the others of its group when the
    /// name is reserved (`_`-led: a library's internals, libc++'s
    /// `__algorithm/`, MinGW's `_mingw.h`).
    std::uint64_t order = 0;
};

/// Return header and directory names matching the typed name in the given search paths.
/// @param config        The command's search directories.
/// @param includer_dir  Directory of the file being edited, searched first by "" includes.
/// @param prefix        Partially-typed include path (e.g. "vec" or "sys/").
/// @param angled        True for <> includes, false for "" includes.
/// @param scope         The request's directory listings.
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
    // A name several directories hold is listed once, at its best rank: a
    // file no header would be named as does not hide a header behind it.
    llvm::StringMap<std::size_t> listed;

    auto collect =
        [&](const SearchDir& search_dir, bool system, std::uint64_t tier, std::uint64_t position) {
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
                if(!score) {
                    continue;
                }

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
                IncludeCandidate candidate{name.str(),
                                           is_dir,
                                           (std::uint64_t(hidden) << 32) |
                                               (std::uint64_t(unlikely) << 31) | (mismatch << 20) |
                                               (tier << 17) | (std::uint64_t(reserved) << 16) |
                                               position};
                auto [it, inserted] = listed.try_emplace(name, results.size());
                if(inserted) {
                    results.push_back(std::move(candidate));
                } else if(candidate.order < results[it->second].order) {
                    results[it->second] = std::move(candidate);
                }
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

}  // namespace

auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList {
    bool angled = (completing.kind == CompletionName::Kind::IncludeAngled);
    auto candidates =
        complete_include_path(search, path::parent(path), completing.prefix, angled, scope);

    // A directory leaves the path open: the `/` typed after it asks
    // again, for the directory's own entries. A header ends the path
    // and closes the directive, replacing the rest of the path through
    // a closing delimiter already there.
    char closer = angled ? '>' : '"';
    auto edit = [&](const IncludeCandidate& candidate) {
        auto end = completing.replace.end;
        if(!candidate.is_directory && completing.closed) {
            end = static_cast<std::uint32_t>(text.find(closer, end)) + 1;
        }
        return protocol::TextEdit{
            .range = *map.range({completing.replace.begin, end}, encoding),
            .new_text = candidate.is_directory ? candidate.name : candidate.name + closer,
        };
    };
    std::vector<protocol::CompletionItem> items;
    items.reserve(candidates.size());
    for(auto& c: candidates) {
        protocol::CompletionItem item;
        item.label = c.is_directory ? c.name + "/" : c.name;
        item.kind = c.is_directory ? protocol::CompletionItemKind::Folder
                                   : protocol::CompletionItemKind::File;
        item.sort_text = std::format("{:010}", c.order);
        item.text_edit = edit(c);
        items.push_back(std::move(item));
    }
    return completion_list(std::move(items), limit);
}

}  // namespace clice::feature
