module clice;

import :analysis.facts;
import :analysis.facts.builder;
import :index.project_index;
import :index.shard;
import :project.project;
import :vfs.path;

namespace clice::analysis {

llvm::SmallVector<std::uint32_t> Facts::including_files(std::uint32_t file, bool pasted) const {
    llvm::SmallVector<std::uint32_t> result;
    llvm::SmallVector<std::uint32_t> pending{file};
    llvm::DenseSet<std::uint32_t> visited;
    while(!pending.empty()) {
        auto current = pending.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }
        if(!files[current].fragment) {
            result.push_back(current);
            continue;
        }
        if(pasted || files[current].pasters.empty()) {
            pending.append(files[current].includers.begin(), files[current].includers.end());
        }
    }
    return result;
}

llvm::SmallVector<std::uint32_t> Facts::charged_files(std::uint32_t file) const {
    return including_files(file, false);
}

std::vector<llvm::SmallVector<std::uint32_t, 1>> Facts::pasted_fragments() const {
    std::vector<llvm::SmallVector<std::uint32_t, 1>> pasted(files.size());
    for(std::uint32_t file = 0; file < files.size(); file += 1) {
        if(files[file].fragment) {
            for(auto charged: charged_files(file)) {
                pasted[charged].push_back(file);
            }
        }
    }
    return pasted;
}

Facts collect(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope) {
    FactsBuilder builder(project, in_scope);
    builder.list_files();
    builder.read_includes();
    builder.read_shards();
    builder.find_pasters();
    builder.read_macros();
    builder.find_usings();
    builder.find_aliases();
    builder.read_entities();
    builder.read_specializations();
    builder.read_uses();
    builder.find_context_macros();
    return std::move(builder.facts);
}

std::expected<LoadedFacts, LoadError> load_facts(Project& project,
                                                 llvm::ArrayRef<std::string> scope) {
    std::vector<kotatsu::GlobPattern> globs;
    for(auto& pattern: scope) {
        auto glob = kotatsu::GlobPattern::create(pattern);
        if(!glob) {
            return std::unexpected(LoadError{
                .message =
                    std::format("invalid --scope glob '{}': {}", pattern, glob.error().message),
            });
        }
        globs.push_back(std::move(*glob));
    }

    auto& files = project.file_table;
    // A file written since it was indexed (as `clice refactor rename`
    // leaves the files it edits) would give facts about text that is gone.
    LoadError stale{.message =
                        "some files changed since they were indexed; run `clice index` first"};
    project.index.for_each_indexed_file([&](Fid file, std::uint64_t) {
        if(auto* shard = project.index.shard(file);
           shard && project.freshness.stale(file, shard->content_hash())) {
            stale.dropped.emplace_back(files.display(file));
        }
    });
    // A unit owed the variants a rebuild discarded is missing from them.
    for(auto unit: project.index.owed_units()) {
        stale.dropped.emplace_back(files.display(unit));
    }
    if(!stale.dropped.empty()) {
        ranges::sort(stale.dropped);
        return std::unexpected(std::move(stale));
    }

    auto facts = collect(project, [&](llvm::StringRef path) {
        if(!globs.empty()) {
            return ranges::any_of(globs, [&](auto& glob) { return glob.match(path); });
        }
        return !path::is_absolute(path) && !path.starts_with(".") && !path.contains("/.");
    });
    if(project.index.damaged()) {
        return std::unexpected(LoadError{.message = project.damaged_index_error()});
    }
    return LoadedFacts{.root = llvm::StringRef(project.root).str(), .facts = std::move(facts)};
}

}  // namespace clice::analysis
