module clice:project.shared_headers;

import :project.graph.pch;
import :support.prelude;
import :vfs.ids;

namespace clice {

/// The shared headers of one background index pass: per group of the pass's
/// units compiled under one command, headers of `#include`s of what most of
/// them include, written to the cache directory and precompiled once,
/// whose PCH the units load instead of each parsing those includes
/// (Project::plan_shared_headers). Only index runs read them: the editor
/// compiles a file as its build does. A PCH lives for the pass, and so
/// does its header's own index, whose variants and instantiations the units
/// loading it send by hash.
struct SharedHeaders {
    struct Header {
        /// The header's file under the cache directory.
        Fid file;

        PCHRequest request;

        /// How many units of the pass parsed over its PCH, and how many
        /// were given it and parsed without it.
        std::uint32_t loaded = 0;

        std::uint32_t rejected = 0;
    };

    std::vector<Header> headers;

    /// Each unit loading one, into `headers`.
    llvm::DenseMap<Fid, std::uint32_t> units;

    /// Each header's file.
    llvm::DenseSet<Fid> files;
};

}  // namespace clice
