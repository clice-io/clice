module clice:unittests.utils.index_query_tester;

import :compile.compilation;
import :index.query;
import :index.search;
import :index.symbol_query;
import :project.project;
import :server.served_project;
import :support.prelude;
import :unittests.server.served_project_fixture;
import :unittests.utils.merge_unit;
import :unittests.utils.tester;
import :unittests.utils;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {

struct IndexQueryTester : Tester {
    ServedProjectFixture fx;
    Project& project = fx.project().project;
    index::IndexQuery& query = project.index_query;

    Fid focus_id;
    Fid header_id;

    std::vector<index::IndexQuery::Located> search(llvm::StringRef text, std::size_t limit = 10) {
        return index::search(query, *index::SymbolQuery::parse(text), limit);
    }

    /// Compile the added sources as the background index does and merge the
    /// unit's index.
    void index_workspace() {
        kind = CompilationKind::Whole;
        ZASSERT(compile());
        merge_unit(project, *unit, focus_id);
        if(auto header = project.file_table.find(Spelling::absolute(TestVFS::path("header.h")))) {
            header_id = *header;
        }
    }
};

}  // namespace clice::testing
