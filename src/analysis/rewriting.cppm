module;

#include "modules/prelude.h"

module clice:analysis.rewriting;

import :analysis.module_graph;

namespace clice::analysis {

/// The program modules a partition rewrites, as the units of named modules.
/// A header becomes a partition: an interface partition its module's primary
/// interface re-exports when files of other modules name it, else an
/// implementation partition. A source becomes an implementation unit. Its
/// includes become imports, of a partition of the same module or of another
/// module, its forward declarations of other modules' entities go, and the
/// macros a header defines for other files move to a macro header beside
/// it. Sources of other modules including the rewritten headers import their
/// modules instead.
struct Rewriting {
    struct File {
        /// Workspace-relative.
        std::string path;
        std::string content;
    };

    struct Module {
        std::string name;
        std::string primary;
        std::vector<std::string> interfaces;
        std::vector<std::string> partitions;
        std::vector<std::string> sources;

        /// The rewritten modules its units import.
        std::vector<std::string> imports;
    };

    /// What the build needs, paths workspace-relative.
    struct Plan {
        std::vector<Module> modules;

        /// Sources of other modules that import rewritten ones: no module
        /// units, but no longer including the rewritten headers.
        std::vector<std::string> importers;

        /// The macro headers written beside the headers defining the macros.
        std::vector<std::string> macros;

        /// The headers their partitions replace.
        std::vector<std::string> removed;

        /// `path=module`: sources defining what a header of a rewritten
        /// module declares, moved to that module, where the definition must
        /// be attached.
        std::vector<std::string> moved;

        std::vector<std::string> warnings;
    };

    std::vector<File> files;
    Plan plan;
};

/// Rewrite the partition's rewritten modules out of the files under `root`.
/// `interfaces` are the partition's, whose wrapped modules' textual headers
/// stay included; `prelude` is the header each rewritten file includes first,
/// workspace-relative.
std::expected<Rewriting, std::string> rewrite(const Facts& facts,
                                              Partition partition,
                                              llvm::ArrayRef<Interface> interfaces,
                                              llvm::StringRef prelude,
                                              llvm::StringRef root);

}  // namespace clice::analysis
