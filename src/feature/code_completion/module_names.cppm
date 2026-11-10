/// Completing the module name of an `import` or of an implementation
/// unit's module declaration.

module clice:feature.code_completion.module_names;

import :feature.code_completion.cursor;
import :feature.code_completion;
import :support.prelude;
import :syntax.dependency_graph;
import :syntax.scan;
import :vfs.ids;

namespace clice::feature {

/// The modules the project provides and the units providing them: the
/// graph's, an open unit's as its buffer declares it (`open`).
struct ModuleProviders {
    llvm::StringMap<llvm::SmallVector<Fid, 2>> modules;

    /// The providers of internal partitions.
    llvm::DenseSet<Fid> internal;
};

auto module_providers(const DependencyGraph& graph, llvm::ArrayRef<OpenModule> open = {})
    -> ModuleProviders;

/// A module name a completion offers, with how well the name typed
/// matches it (FuzzyMatcher's score).
struct ModuleCandidate {
    std::string name;
    float score = 0;
};

/// The names an implementation unit's module declaration can take that
/// `typed` matches, the best match first, ties by name: the modules the
/// interfaces provide.
std::vector<ModuleCandidate> complete_module_declaration(const ModuleProviders& providers,
                                                         llvm::StringRef typed);

/// The names an `import` in the unit whose module declaration `unit`
/// scanned can take that `typed` matches, the best match first, ties by
/// name: the other provided modules, and the partitions of the unit's own
/// module but its own, spelled `:partition` — in an interface unit, its
/// interface partitions only.
std::vector<ModuleCandidate> complete_module_import(const ModuleProviders& providers,
                                                    llvm::StringRef typed,
                                                    const ScanResult& unit);

/// The modules an `import` or a module declaration being typed
/// (`completing`) may name, each with its documentation: the better the
/// name typed matches, the earlier; then the unit's own partitions first,
/// then the modules whose interface lies nearest the file — the fewest
/// directories up from it to one holding the interface.
/// The other open units provide what their buffers declare, the file what
/// its text does.
auto module_completion(const CompletionName& completing, const CompletionInputs& inputs)
    -> protocol::CompletionList;

}  // namespace clice::feature
