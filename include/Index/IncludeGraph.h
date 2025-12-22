#pragma once

#include <vector>

#include "AST/SourceCode.h"

#include "llvm/ADT/DenseMap.h"

namespace clice {

class CompilationUnit;

}

namespace clice::index {

struct IncludeLocation {
    /// The file path of the include directive.
    std::uint32_t path_id = -1;

    /// The line number of the include directive, 1-based.
    std::uint32_t line = -1;

    /// The include location that introduces this file.
    std::uint32_t include = -1;

    friend bool operator==(const IncludeLocation&, const IncludeLocation&) = default;
};

struct IncludeGraph {
    /// If a header file doesn't have a #pragma once or guard macro,
    /// each inclusion of it will introduce a new header context, we
    /// don't want to save its path repeatedly, so cache it here.
    std::vector<std::string> paths;

    /// All include locations in this tu.
    std::vector<IncludeLocation> locations;

    /// Each `FileID` represents a new header context and is introduced
    /// by a new include directive. So a include directive is a new header
    /// context. A map between FileID and its include location.
    llvm::DenseMap<clang::FileID, std::uint32_t> file_table;

    static IncludeGraph from(CompilationUnit& unit);

    llvm::StringRef path(std::uint32_t path_ref) const {
        assert(path_ref < paths.size());
        return paths[path_ref];
    }

    std::uint32_t include_location_id(clang::FileID fid) const {
        auto it = file_table.find(fid);
        assert(it != file_table.end());
        return it->second;
    }

    std::uint32_t path_id(clang::FileID fid) {
        auto include = include_location_id(fid);
        if(include != -1) {
            return locations[include].path_id;
        } else {
            return paths.size() - 1;
        }
    }
};

}  // namespace clice::index
