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
        resolved.dirs.push_back({dir.path, &scope.list(dir.path), dir.framework});
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

/// The header `rest` of the framework `bundle` (`Foo.framework`) below
/// `dir`, as clang's framework lookup finds it: in its Headers/, else in
/// its PrivateHeaders/.
std::optional<llvm::SmallString<256>> find_in_framework(llvm::StringRef dir,
                                                        llvm::StringRef bundle,
                                                        llvm::StringRef rest,
                                                        vfs::Scope& scope) {
    for(llvm::StringRef headers: {"Headers", "PrivateHeaders"}) {
        llvm::SmallString<256> full(dir);
        path::push(full, bundle);
        path::push(full, headers);
        path::push(full, rest);
        if(scope.list(path::parent(full.str())).contains_file(path::filename(full.str()))) {
            return full;
        }
    }
    return std::nullopt;
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
        if(fs::exists(filename)) {
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

    auto [framework, framework_rest] = filename.split('/');
    auto search_from = [&](unsigned start) -> std::optional<ResolveResult> {
        // clang takes the first framework directory holding the framework:
        // a header it lacks is not looked for in the framework of a later
        // one.
        // LIMITATION: clang's subframework lookup: `<Sub/x.h>` written
        // inside Umb.framework, which clang finds in its
        // Frameworks/Sub.framework, stays unresolved here. Only the SDK's
        // umbrella frameworks rely on it; their headers never change, and
        // the compile itself, being clang's, finds them.
        bool framework_found = false;
        for(unsigned i = start; i < config.dirs.size(); i += 1) {
            auto& dir = config.dirs[i];
            if(dir.framework) {
                if(framework_rest.empty() || framework_found) {
                    continue;
                }
                auto bundle = (framework + ".framework").str();
                if(!dir.listing->contains(bundle)) {
                    continue;
                }
                framework_found = true;
                if(auto found = find_in_framework(dir.path, bundle, framework_rest, scope)) {
                    return ResolveResult{std::move(*found), i};
                }
            } else if(check_in_dir(dir.path,
                                   dir.listing,
                                   filename,
                                   is_simple,
                                   config.hidden.get(),
                                   scope)) {
                make_candidate(dir.path, filename);
                return ResolveResult{candidate, i};
            }
        }
        return std::nullopt;
    };

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next && found_dir_idx) {
        return search_from(*found_dir_idx + 1);
    }

    // 3. Quoted include: try includer's directory first.
    // LIMITATION: under MSVC compatibility (clang-cl's default,
    // -fms-compatibility) clang goes on to the directories of every file on
    // the include stack, up to the source file, before the search
    // directories; this looks next to the includer alone, so a header found
    // that way, which clang flags with -Wmicrosoft-include, stays unresolved
    // and gets no host when opened alone. Only legacy MSVC code relies on it.
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
    return search_from(is_angled ? config.angled_start_idx : 0);
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

IncludeArguments::IncludeArguments(const SearchConfig& search,
                                   const Spelling& file,
                                   vfs::Scope& scope) :
    search(search), scope(scope), directory(file.parent()),
    resolved_directory(CanonicalPath(directory)) {
    for(auto& dir: search.dirs) {
        resolved_dirs.push_back(CanonicalPath(Spelling::absolute(dir.path)));
    }
}

std::optional<std::string> IncludeArguments::of(CanonicalRef header) {
    auto [known_entry, inserted] = known.try_emplace(llvm::StringRef(header));
    auto& argument = known_entry->second;
    if(!inserted) {
        return argument;
    }
    auto below = [&](CanonicalRef root) -> std::optional<llvm::StringRef> {
        if(root.empty() || !path::is_within(header, root) || header.size() <= root.size()) {
            return std::nullopt;
        }
        return llvm::StringRef(header).drop_front(root.size()).ltrim("/\\");
    };

    struct Candidate {
        std::string name;
        bool angled;
    };

    std::vector<Candidate> candidates;
    if(auto relative = below(resolved_directory)) {
        candidates.push_back({relative->str(), false});
    }
    for(auto [index, dir]: views::enumerate(resolved_dirs)) {
        auto relative = below(dir);
        if(!relative) {
            continue;
        }
        bool angled = static_cast<std::size_t>(index) >= search.angled_start_idx;
        if(!search.dirs[index].framework) {
            candidates.push_back({relative->str(), angled});
            continue;
        }
        auto [bundle, inside] = relative->split('/');
        // A versioned framework's Headers/ links into Versions/<version>/,
        // where the header's identity is.
        if(inside.consume_front("Versions/")) {
            inside = inside.split('/').second;
        }
        auto [headers, rest] = inside.split('/');
        if(bundle.consume_back(".framework") && !rest.empty() &&
           (headers == "Headers" || headers == "PrivateHeaders")) {
            candidates.push_back({(bundle + "/" + rest).str(), angled});
        }
    }
    ranges::stable_sort(candidates, {}, [](const Candidate& candidate) {
        return candidate.name.size();
    });
    for(const auto& candidate: candidates) {
        auto resolved =
            resolve_include(candidate.name, candidate.angled, directory, false, 0, search, scope);
        if(resolved && CanonicalPath(Spelling::absolute(resolved->path)) == header) {
            if(!internal_header(candidate.angled, candidate.name)) {
                argument = candidate.angled ? std::format("<{}>", candidate.name)
                                            : std::format("\"{}\"", candidate.name);
            }
            break;
        }
    }
    return argument;
}

}  // namespace clice
