#pragma once

#include <expected>
#include <string>
#include <vector>

#include "analysis/module_graph.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

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
/// include path empties the wrapped headers and a force-included prelude
/// imports the modules and replays their macros.
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

    std::vector<File> files;

    /// In import order.
    std::vector<Module> modules;

    /// libc++'s module sources, compiled as modules std and std.compat,
    /// when the partition's std module stands for the C++ headers.
    std::vector<std::string> std_sources;

    /// What every compilation of the program puts first on its include
    /// path, and the header it force-includes; relative to the output
    /// directory.
    std::vector<std::string> mirrors;
    std::string prelude;

    /// Imports of modules the partition leaves to the program, dropped:
    /// a library's header naming the program's entities.
    std::vector<std::string> warnings;
};

/// Wrap the interfaces of a partition's wrapped modules. With `libcxx`, the
/// partition's external module `std` is libc++'s, imported as std.compat,
/// and the modules it keeps headers contribute their includes and macros
/// ahead of every import. `root` absolutizes the workspace-relative paths.
std::expected<Wrapping, std::string> wrap(const Partition& partition,
                                          llvm::ArrayRef<Interface> interfaces,
                                          const StdModules* libcxx,
                                          llvm::StringRef root);

}  // namespace clice::analysis
