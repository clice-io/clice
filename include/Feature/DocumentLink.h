#pragma once

#include <vector>

#include "AST/SourceCode.h"
#include "Index/Shared.h"

namespace clice::feature {

struct DocumentLink {
    /// The range of the whole link.
    LocalSourceRange range;

    /// The target string path.
    std::string file;
};

using DocumentLinks = std::vector<DocumentLink>;

/// Generate document link for main file.
DocumentLinks document_links(CompilationUnitRef unit);

/// Generate document link for all source file.
index::Shared<DocumentLinks> index_document_link(CompilationUnitRef unit);

}  // namespace clice::feature
