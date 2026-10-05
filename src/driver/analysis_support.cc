#include "driver/analysis_support.h"

#include <format>
#include <utility>

#include "project/command_resolver.h"
#include "project/open_index.h"
#include "project/project.h"
#include "vfs/file_system.h"

#include "kota/codec/json/json.h"
#include "kota/support/glob_pattern.h"
#include "llvm/Support/Path.h"

namespace clice::driver {

namespace {

struct PartitionFile {
    struct Module {
        std::string name;
        std::vector<std::string> files;

        /// Its headers stay headers beside the wrapped modules.
        std::optional<bool> textual;

        /// An existing module interface stands for it.
        std::optional<bool> external;

        /// The module exporting the names of its headers.
        std::optional<std::string> provides;

        /// A program module rewritten into a named module, and where its
        /// primary interface unit goes.
        std::optional<bool> rewrite;
        std::optional<std::string> primary;
    };

    std::vector<Module> modules;
};

}  // namespace

std::vector<std::string> comma_list(llvm::StringRef text) {
    llvm::SmallVector<llvm::StringRef> parts;
    text.split(parts, ',', -1, false);
    std::vector<std::string> items;
    for(auto part: parts) {
        items.push_back(part.trim().str());
    }
    return items;
}

std::expected<std::optional<analysis::StdModules>, std::string>
    read_std(llvm::StringRef directory) {
    if(directory.empty()) {
        return std::nullopt;
    }
    return analysis::read_std_modules(directory);
}

std::expected<analysis::PartitionSpec, std::string>
    read_partition(analysis::PartitionSpec spec,
                   llvm::StringRef path,
                   const std::optional<analysis::StdModules>& libcxx) {
    if(path.empty()) {
        return spec;
    }
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::unexpected(
            std::format("cannot read {}: {}", path.str(), buffer.error().message()));
    }
    PartitionFile file;
    if(auto result = kota::codec::json::from_string((*buffer)->getBuffer(), file); !result) {
        return std::unexpected(
            std::format("{} is not a partition file: {}", path.str(), result.error().message));
    }
    for(auto& module: file.modules) {
        analysis::PartitionSpec::Module claimed{
            .name = std::move(module.name),
            .files = std::move(module.files),
        };
        auto textual = module.textual.value_or(false);
        auto external = module.external.value_or(false);
        if(textual && external) {
            return std::unexpected(
                std::format("module {} is both textual and external", claimed.name));
        }
        if(module.rewrite.value_or(false)) {
            // A kind beside it is left for the partition to reject.
            if(!textual && !external) {
                claimed.kind = analysis::ModuleKind::Program;
            }
            claimed.rewrite = true;
            claimed.primary = module.primary.value_or("");
        } else if(module.primary) {
            return std::unexpected(
                std::format("module {} has a primary interface but is not rewritten",
                            claimed.name));
        }
        if(textual) {
            claimed.kind = analysis::ModuleKind::Textual;
        }
        if(external) {
            claimed.kind = analysis::ModuleKind::External;
        }
        if(module.provides) {
            if(*module.provides != "std.compat" || !libcxx) {
                return std::unexpected(
                    std::format("module {} provided by {}: only std.compat, given --std",
                                claimed.name,
                                *module.provides));
            }
            for(auto& name: libcxx->compat) {
                claimed.provides.insert(name.getKey());
            }
        }
        spec.modules.push_back(std::move(claimed));
    }
    return spec;
}

std::expected<LoadedFacts, Failure> load_facts(llvm::StringRef workspace,
                                               llvm::StringRef configuration,
                                               llvm::StringRef scope) {
    std::vector<kota::GlobPattern> globs;
    for(auto& pattern: comma_list(scope)) {
        auto glob = kota::GlobPattern::create(pattern);
        if(!glob) {
            return std::unexpected(Failure{
                .error =
                    std::format("invalid --scope glob '{}': {}", pattern, glob.error().message),
            });
        }
        globs.push_back(std::move(*glob));
    }

    auto spelling = workspace_spelling(workspace);
    CanonicalPath root(spelling);
    FileTable files;
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    auto loaded = load_index(project, commands, root, configuration, /*with_build=*/false);
    if(!loaded) {
        return std::unexpected(Failure{.error = "no usable index; run `clice index` first"});
    }
    // A unit withheld as stale or corrupt takes its uses and edges with it.
    if(!loaded->dropped.empty()) {
        Failure failure{.error = "the index lacks some units; run `clice index` first"};
        for(auto unit: loaded->dropped) {
            failure.stale.emplace_back(files.display(unit));
        }
        std::ranges::sort(failure.stale);
        return std::unexpected(std::move(failure));
    }

    auto facts = analysis::collect(project, [&](llvm::StringRef path) {
        if(!globs.empty()) {
            return llvm::any_of(globs, [&](auto& glob) { return glob.match(path); });
        }
        return !llvm::sys::path::is_absolute(path) && !path.starts_with(".") &&
               !path.contains("/.");
    });
    return LoadedFacts{.root = llvm::StringRef(root).str(), .facts = std::move(facts)};
}

}  // namespace clice::driver
