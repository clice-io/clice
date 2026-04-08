#include "syntax/completion.h"

#include "syntax/include_resolver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset) {
    auto line_start = text.rfind('\n', offset > 0 ? offset - 1 : 0);
    line_start = (line_start == llvm::StringRef::npos) ? 0 : line_start + 1;

    auto line = text.slice(line_start, offset);
    auto trimmed = line.ltrim();

    if(trimmed.starts_with("#")) {
        auto directive = trimmed.drop_front(1).ltrim();
        if(directive.consume_front("include")) {
            directive = directive.ltrim();
            if(directive.consume_front("\"")) {
                return {CompletionContext::IncludeQuoted, directive.str()};
            }
            if(directive.consume_front("<")) {
                return {CompletionContext::IncludeAngled, directive.str()};
            }
        }
        return {};
    }

    auto import_check = trimmed;
    if(import_check.consume_front("export") && !import_check.empty() &&
       !std::isalnum(import_check[0])) {
        import_check = import_check.ltrim();
    }
    if(import_check.consume_front("import") &&
       (import_check.empty() || !std::isalnum(import_check[0]))) {
        import_check = import_check.ltrim();

        auto line_end = text.find('\n', offset);
        if(line_end == llvm::StringRef::npos)
            line_end = text.size();
        auto rest_of_line = text.slice(line_start, line_end);
        if(!rest_of_line.contains(';')) {
            return {CompletionContext::Import, import_check.str()};
        }
    }

    return {};
}

std::vector<std::string>
    complete_module_import(const llvm::DenseMap<std::uint32_t, std::string>& modules,
                           llvm::StringRef prefix) {
    std::vector<std::string> results;
    for(auto& [path_id, module_name]: modules) {
        if(llvm::StringRef(module_name).starts_with(prefix)) {
            results.push_back(module_name);
        }
    }
    return results;
}

std::vector<IncludeCandidate> complete_include_path(const ResolvedSearchConfig& resolved,
                                                    llvm::StringRef prefix,
                                                    bool angled,
                                                    DirListingCache& dir_cache) {
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = prefix;
    auto slash_pos = prefix.rfind('/');
    if(slash_pos != llvm::StringRef::npos) {
        dir_prefix = prefix.slice(0, slash_pos);
        file_prefix = prefix.slice(slash_pos + 1, llvm::StringRef::npos);
    }

    unsigned start_idx = angled ? resolved.angled_start_idx : 0;

    std::vector<IncludeCandidate> results;
    llvm::StringSet<> seen;

    for(unsigned i = start_idx; i < resolved.dirs.size(); ++i) {
        auto& search_dir = resolved.dirs[i];

        const llvm::StringSet<>* entries = nullptr;
        if(!dir_prefix.empty()) {
            llvm::SmallString<256> sub_path(search_dir.path);
            llvm::sys::path::append(sub_path, dir_prefix);
            entries = resolve_dir(sub_path, dir_cache);
        } else {
            entries = search_dir.entries;
        }

        if(!entries)
            continue;

        for(auto& entry: *entries) {
            auto name = entry.getKey();
            if(!name.starts_with(file_prefix))
                continue;
            if(!seen.insert(name).second)
                continue;

            llvm::SmallString<256> full_path(search_dir.path);
            if(!dir_prefix.empty()) {
                llvm::sys::path::append(full_path, dir_prefix);
            }
            llvm::sys::path::append(full_path, name);

            bool is_dir = false;
            llvm::sys::fs::is_directory(llvm::Twine(full_path), is_dir);

            results.push_back({name.str(), is_dir});
        }
    }

    return results;
}

}  // namespace clice
