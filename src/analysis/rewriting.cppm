module clice:analysis.rewriting;

import :analysis.module_graph;
import :support.prelude;

namespace clice::analysis {

/// What a wrapping gives the files a partition rewrites, paths
/// relative to the project root.
struct WrappedModules {
    /// The modules a rewritten module's prelude partition imports, in order.
    std::vector<std::string> imports;

    /// The header replaying their macros, which declares nothing.
    std::string macros;

    /// The header a translation unit that stays one includes first, as
    /// does a module unit's global module fragment.
    std::string prelude;
};

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
        /// Relative to the project root.
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

    /// What the build needs, paths relative to the project root.
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
/// and kept modules' headers of internal-linkage entities stay included. A
/// rewritten module's prelude partition imports the `wrapped` modules for
/// all its units; a unit with a global module fragment includes the prelude
/// there, one without includes the macro header where it uses their macros.
std::expected<Rewriting, std::string> rewrite(const Facts& facts,
                                              Partition partition,
                                              llvm::ArrayRef<Interface> interfaces,
                                              const WrappedModules& wrapped,
                                              llvm::StringRef root);

}  // namespace clice::analysis
