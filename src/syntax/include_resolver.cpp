#include "syntax/include_resolver.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

ResolvedSearchConfig resolve_search_config(const SearchConfig& config, vfs::Scope& scope) {
    ResolvedSearchConfig resolved;
    resolved.angled_start_idx = config.angled_start_idx;
    resolved.system_start_idx = config.system_start_idx;
    resolved.after_start_idx = config.after_start_idx;
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
                  vfs::Scope& scope) {
    scope.stats.lookups += 1;

    if(is_simple) {
        return listing->contains(filename);
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
    llvm::sys::path::append(full, filename);
    auto parent = llvm::sys::path::parent_path(full);
    auto name = llvm::sys::path::filename(full);
    return scope.list(parent).contains(name);
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
    if(llvm::sys::path::is_absolute(filename)) {
        if(llvm::sys::fs::exists(filename)) {
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
        llvm::sys::path::append(candidate, fname);
    };

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next && found_dir_idx) {
        for(unsigned i = *found_dir_idx + 1; i < config.dirs.size(); i += 1) {
            if(check_in_dir(config.dirs[i].path,
                            config.dirs[i].listing,
                            filename,
                            is_simple,
                            scope)) {
                make_candidate(config.dirs[i].path, filename);
                return ResolveResult{candidate, i};
            }
        }
        return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && includer_listing) {
        if(check_in_dir(includer_dir, includer_listing, filename, is_simple, scope)) {
            make_candidate(includer_dir, filename);
            return ResolveResult{.path = candidate};
        }
    }

    // 4. Search directories from appropriate start index.
    // TODO: macOS Framework search — for <Foo/Bar.h>, try Foo.framework/Headers/Bar.h
    //       in dirs marked as framework dirs (-F, -iframework).
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        if(check_in_dir(config.dirs[i].path, config.dirs[i].listing, filename, is_simple, scope)) {
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

}  // namespace clice
