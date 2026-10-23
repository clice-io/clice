module;

#include "support/prelude.h"

module clice;

import :feature.navigation;
import :unittests.utils.index_query_tester;

namespace clice::testing {
namespace {

ZEST_SUITE(Navigation, IndexQueryTester) {

ZEST_CASE(LocalReferencesClosed) {
    add_focus("main.cpp", R"(
        int compute(int §(param)p) {
            int §(local)q = p;
            return §(use)q + p;
        }
    )");
    index_workspace();

    for(auto [marker, expected]: {
            std::pair{"use",   2U},
            std::pair{"param", 3U}
    }) {
        auto cursor = query.symbol_at(focus_id, point(marker));
        ZASSERT(cursor);
        ZEXPECT(feature::references(query, *cursor, true).size() == expected);
    }
}

ZEST_CASE(HighlightsClosedFile) {
    add_focus("main.cpp", R"(
        int tally() {
            int §(decl)⟦count⟧ = 0;
            §(write)⟦count⟧ += 1;
            return §(read)⟦§(read)count⟧;
        }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("read"));
    ZASSERT(cursor);
    auto highlights = feature::document_highlights(query, *cursor);
    ZASSERT(highlights.size() == 3U);
    ZEXPECT(highlights[0].site.range == range("decl"));
    ZEXPECT(highlights[0].kind == feature::HighlightKind::Text);
    ZEXPECT(highlights[1].site.range == range("write"));
    ZEXPECT(highlights[1].kind == feature::HighlightKind::Write);
    ZEXPECT(highlights[2].site.range == range("read"));
    ZEXPECT(highlights[2].kind == feature::HighlightKind::Read);
}

ZEST_CASE(InternalAcrossFiles) {
    add_file("header.h", R"(
        static int §(def)helper() { return 1; }
        inline int via_header() { return §(header_use)helper(); }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int call() { return §(main_use)helper(); }
    )");
    index_workspace();

    auto from_main = query.symbol_at(focus_id, point("main_use"));
    auto from_header = query.symbol_at(header_id, point("def", "header.h"));
    ZASSERT(from_main);
    ZASSERT(from_header);
    ZASSERT(from_main->symbols == from_header->symbols);
    ZEXPECT(feature::references(query, *from_main, true).size() == 3U);
    ZEXPECT(feature::references(query, *from_header, true).size() == 3U);
    auto located = query.resolve_at(*from_main);
    ZASSERT(located.size() == 1U);
    ZEXPECT(located.front().site.path.ends_with("header.h"));
}

ZEST_CASE(InternalTypeTarget) {
    add_file("header.h", R"(
        namespace {
        struct Point { int x; };
        struct Pair { Point first; int second; };
        Pair make() { return {}; }
        }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int read() { auto [§(var)point, count] = make(); return point.x + count; }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("var"));
    ZASSERT(cursor);
    auto sites = feature::type_definition(query, *cursor, cursor->symbols.front());
    ZASSERT(sites.size() == 1U);
    ZEXPECT(sites.front().path.ends_with("header.h"));
}

ZEST_CASE(InternalAcrossUnits) {
    llvm::StringRef header = "static int helper() { return 1; }\n";
    add_file("header.h", header);
    add_focus("a.cpp", R"(
        #include "header.h"
        int a() { return §(use)helper(); }
    )");
    index_workspace();
    auto a_id = focus_id;
    auto a_use = point("use");

    clear();
    add_file("header.h", header);
    add_focus("b.cpp", R"(
        #include "header.h"
        int b() { return helper(); }
    )");
    index_workspace();

    auto cursor = query.symbol_at(a_id, a_use);
    ZASSERT(cursor);
    ZEXPECT(feature::references(query, *cursor, true).size() == 3U);
}

ZEST_CASE(OverloadSetCursor) {
    add_focus("main.cpp", R"(
        void §(int)take(int);
        void §(double)take(double);
        template <class T> void call(T t) { §(use)take(t); }
    )");
    index_workspace();

    auto cursor = query.symbol_at(focus_id, point("use"));
    ZASSERT(cursor);
    ZASSERT(cursor->symbols.size() == 2U);
    auto sites = feature::definition(query, *cursor);
    ZASSERT(sites.size() == 2U);
    ZEXPECT(sites[0].range.begin == point("int"));
    ZEXPECT(sites[1].range.begin == point("double"));
    ZEXPECT(query.resolve_at(*cursor).size() == 2U);
}

};  // ZEST_SUITE(Navigation)

}  // namespace
}  // namespace clice::testing
