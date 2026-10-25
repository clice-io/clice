module;

#include "support/prelude.h"

module clice;

import :syntax.include_resolver;
import :vfs.file_system;
import :vfs.path;

namespace clice {

ResolvedSearchConfig resolve_search_config(const SearchConfig& config, vfs::Scope& scope) {
    ResolvedSearchConfig resolved;
    resolved.angled_start_idx = config.angled_start_idx;
    resolved.hidden = config.hidden;
    resolved.dirs.reserve(config.dirs.size());
    for(auto& dir: config.dirs) {
        resolved.dirs.push_back({dir.path, &scope.list(dir.path)});
    }
    return resolved;
}

namespace {

/// Check if a file exists in a directory, handling multi-component include paths.
/// For simple filenames (no '/'), checks pre-resolved entries directly.
/// For multi-component paths like "llvm/Support/raw_ostream.h", constructs the
/// full path and lists the actual parent subdirectory.
bool check_in_dir(llvm::StringRef dir_path,
                  const vfs::Listing* listing,
                  llvm::StringRef filename,
                  bool is_simple,
                  const vfs::HiddenFiles* hidden,
                  vfs::Scope& scope) {
    auto shown = [&] {
        return !hidden || !hidden->hides(path::join(dir_path, filename));
    };
    if(is_simple) {
        return listing->contains_file(filename) && shown();
    }

    // Quick rejection: check if first path component exists in pre-resolved
    // entries. For "llvm/Support/raw_ostream.h", check if "llvm" exists in
    // the search dir listing. Most search dirs won't have it, so we skip
    // the expensive full path construction + subdirectory resolution.
    // Skip this for relative paths starting with "." or ".." (e.g. "../foo.h").
    auto first_sep = filename.find_first_of("/\\");
    auto first_component = filename.substr(0, first_sep);
    if(first_component != "." && first_component != "..") {
        if(!listing->contains(first_component)) {
            return false;
        }
    }

    // First component matched — construct full path, resolve actual subdirectory.
    llvm::SmallString<256> full;
    full = dir_path;
    path::push(full, filename);
    auto parent = path::parent(full.str());
    auto name = path::filename(full.str());
    return scope.list(parent).contains_file(name) && shown();
}

}  // namespace

std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             const vfs::Listing* includer_listing,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             std::optional<unsigned> found_dir_idx,
                                             const ResolvedSearchConfig& config,
                                             vfs::Scope& scope) {
    // 1. Absolute path: check directly via stat().
    if(path::is_absolute(filename)) {
        if(fs::exists(filename).value_or(false)) {
            return ResolveResult{.path = llvm::SmallString<256>(filename)};
        }
        return std::nullopt;
    }

    // Check if filename has path separators (multi-component like "llvm/Support/foo.h").
    bool is_simple =
        filename.find('/') == llvm::StringRef::npos && filename.find('\\') == llvm::StringRef::npos;

    // The candidate keeps `..` as written: the OS resolves it past a
    // symlinked directory, where a lexical collapse would name another file.
    llvm::SmallString<256> candidate;
    auto make_candidate = [&](llvm::StringRef dir, llvm::StringRef fname) {
        candidate = dir;
        path::push(candidate, fname);
    };

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next && found_dir_idx) {
        for(unsigned i = *found_dir_idx + 1; i < config.dirs.size(); i += 1) {
            if(check_in_dir(config.dirs[i].path,
                            config.dirs[i].listing,
                            filename,
                            is_simple,
                            config.hidden.get(),
                            scope)) {
                make_candidate(config.dirs[i].path, filename);
                return ResolveResult{candidate, i};
            }
        }
        return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && includer_listing) {
        if(check_in_dir(includer_dir,
                        includer_listing,
                        filename,
                        is_simple,
                        config.hidden.get(),
                        scope)) {
            make_candidate(includer_dir, filename);
            return ResolveResult{.path = candidate};
        }
    }

    // 4. Search directories from appropriate start index.
    // FIXME: framework lookup (<Foo/Bar.h> in Foo.framework/Headers/Bar.h)
    // waits on SearchConfig carrying the framework directories.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        if(check_in_dir(config.dirs[i].path,
                        config.dirs[i].listing,
                        filename,
                        is_simple,
                        config.hidden.get(),
                        scope)) {
            make_candidate(config.dirs[i].path, filename);
            return ResolveResult{candidate, i};
        }
    }

    return std::nullopt;
}

std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             std::optional<unsigned> found_dir_idx,
                                             const SearchConfig& config,
                                             vfs::Scope& scope) {
    auto resolved_config = resolve_search_config(config, scope);
    const vfs::Listing* includer_listing =
        includer_dir.empty() ? nullptr : &scope.list(includer_dir);
    return resolve_include(filename,
                           is_angled,
                           includer_listing,
                           includer_dir,
                           is_include_next,
                           found_dir_idx,
                           resolved_config,
                           scope);
}

std::optional<ResolveResult> resolve_forced_include(llvm::StringRef filename,
                                                    llvm::StringRef directory,
                                                    const ResolvedSearchConfig& config,
                                                    vfs::Scope& scope) {
    return resolve_include(filename,
                           false,
                           &scope.list(directory),
                           directory,
                           false,
                           std::nullopt,
                           config,
                           scope);
}

/// Whether an include's path names a library's internal header (see
/// include_argument).
static bool internal_header(bool angled, llvm::StringRef name) {
    return angled && ranges::any_of(path::components(name), [](std::string_view part) {
               return part == "bits" || part.starts_with("__");
           });
}

std::optional<std::string> include_argument(CanonicalRef header,
                                            const SearchConfig& search,
                                            const Spelling& file,
                                            vfs::Scope& scope) {
    auto below = [&](CanonicalRef root) -> std::optional<llvm::StringRef> {
        if(root.empty() || !path::is_within(header, root) || header.size() <= root.size()) {
            return std::nullopt;
        }
        return llvm::StringRef(header).drop_front(root.size()).ltrim("/\\");
    };

    struct Candidate {
        llvm::StringRef name;
        bool angled;
    };

    std::vector<Candidate> candidates;
    auto directory = file.parent();
    if(auto relative = below(CanonicalPath(directory))) {
        candidates.push_back({*relative, false});
    }
    std::vector<CanonicalPath> dirs;
    for(auto& dir: search.dirs) {
        dirs.push_back(CanonicalPath(Spelling::absolute(dir.path)));
    }
    for(auto [index, dir]: views::enumerate(dirs)) {
        if(auto relative = below(dir)) {
            candidates.push_back(
                {*relative, static_cast<std::size_t>(index) >= search.angled_start_idx});
        }
    }
    ranges::stable_sort(candidates, {}, [](const Candidate& candidate) {
        return candidate.name.size();
    });
    for(const auto& candidate: candidates) {
        auto resolved =
            resolve_include(candidate.name, candidate.angled, directory, false, 0, search, scope);
        if(resolved && CanonicalPath(Spelling::absolute(resolved->path)) == header) {
            if(internal_header(candidate.angled, candidate.name)) {
                return std::nullopt;
            }
            return candidate.angled ? std::format("<{}>", candidate.name)
                                    : std::format("\"{}\"", candidate.name);
        }
    }
    return std::nullopt;
}

}  // namespace clice
