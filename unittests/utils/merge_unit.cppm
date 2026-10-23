module;

#include "support/prelude.h"

module clice:unittests.utils.merge_unit;

import :compile.compilation;
import :index.indexer;
import :index.project_index;
import :index.tu_index;
import :project.project;
import :support.bytes;
import :unittests.utils;

namespace clice::testing {

/// Index a compiled unit and merge it into the project's index as the
/// background index does. `focus` receives the unit's own file id.
inline void merge_unit(Project& project, CompilationUnit& unit, Fid& focus) {
    auto bytes = index::build_tu_index(unit);
    auto view = index::TUIndex::from_bytes(as_chars(bytes));
    ZASSERT(view.loaded());
    focus = project.file_table.intern(Spelling::absolute(view.path(view.path_count() - 1)));
    ZASSERT(project.index.merge(as_chars(bytes), {}).has_value());
}

}  // namespace clice::testing
