module;

#include "modules/prelude.h"

module clice:analysis.wrapping;

import :analysis.module_graph;

namespace clice::analysis {

/// libc++'s std and std.compat modules, read from their sources.
struct StdModules {
    /// std.cppm and std.compat.cppm.
    std::vector<std::string> sources;

    /// The headers std.cppm's global module fragment includes: the ones
    /// `import std` stands for.
    std::vector<std::string> headers;

    /// The global names std.compat exports, the C library's.
    llvm::StringSet<> compat;
};

/// Read libc++'s module sources in `directory`, its `share/libc++/v1`.
std::expected<StdModules, std::string> read_std_modules(llvm::StringRef directory);

/// The wrapped modules of a partition as interface units over their
/// headers, and what the build needs to compile the program against them:
/// the program's sources stay as they are, a directory first on their
/// include path empties the wrapped headers unless mirrors are off, and a
/// force-included prelude imports the modules and replays their macros.
struct Wrapping {
    struct File {
        /// Relative to the output directory.
        std::string path;
        std::string content;
    };

    struct Module {
        std::string name;

        /// Its interface unit, relative to the output directory.
        std::string source;

        /// The generated modules it imports, all before it in `modules`.
        std::vector<std::string> imports;

        /// The directories other modules' files find its headers under:
        /// the include path of the library's own build target, which its
        /// unit compiles with.
        std::vector<std::string> include_roots;

        /// Directories first on its unit's include path, relative to the
        /// output directory: the headers of the modules it imports emptied.
        std::vector<std::string> mirrors;
    };

    /// What the build needs, paths relative to the output directory.
    struct Plan {
        /// libc++'s module sources, compiled as modules std and std.compat,
        /// when the partition's std module stands for the C++ headers.
        std::vector<std::string> std_sources;

        /// In import order.
        std::vector<Module> modules;

        /// What every compilation of the program puts first on its include
        /// path, and the header it force-includes.
        std::vector<std::string> mirrors;
        std::string prelude;

        /// Imports of modules the partition leaves to the program, dropped:
        /// a library's header naming the program's entities.
        std::vector<std::string> warnings;
    };

    std::vector<File> files;
    Plan plan;
};

/// Wrap the interfaces of a partition's wrapped modules. With `libcxx`, the
/// partition's external module `std` is libc++'s, imported as std.compat,
/// and the modules it keeps headers contribute their includes and macros
/// ahead of every import. `root` absolutizes the workspace-relative paths.
/// Without `mirrors`, no header is emptied: what still includes a wrapped
/// header parses it beside the import.
std::expected<Wrapping, std::string> wrap(const Partition& partition,
                                          llvm::ArrayRef<Interface> interfaces,
                                          const std::optional<StdModules>& libcxx,
                                          llvm::StringRef root,
                                          bool mirrors);

/// The wrapping of one build configuration, as `wrap` wrote it.
struct Configuration {
    /// The directory under the merged output holding its own files.
    std::string name;
    /// A preprocessor condition holding in its compilations alone.
    std::string condition;
    std::vector<Wrapping::File> files;
};

/// The wrappings of several configurations as one, for a build whose
/// compilations differ by platform or by configuration. A configuration's
/// global module fragments, macro headers and prelude move into its
/// directory; each module unit and the prelude pick them by its condition.
/// A module unit exports what every configuration exports, the rest under
/// the conditions of the configurations exporting it; the mirrors hold the
/// headers any configuration empties, none without `mirrors`. The plan
/// carries no paths the configurations found on their machines.
std::expected<Wrapping, std::string> merge(llvm::ArrayRef<Configuration> configurations,
                                           bool mirrors);

}  // namespace clice::analysis
