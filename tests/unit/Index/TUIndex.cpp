#include "Test/Tester.h"
#include "Index/TUIndex.h"

namespace clice::testing {

namespace {

suite<"TUIndex"> suite = [] {
    Tester tester;
    index::TUIndex tu_index;

    auto build_index = [&](llvm::StringRef code,
                           std::source_location location = std::source_location::current()) {
        tester.clear();
        tester.add_main("main.cpp", code);
        fatal / expect(tester.compile(), location);

        tu_index = index::TUIndex::build(*tester.unit);
    };

    auto select = [&](llvm::StringRef pos,
                      llvm::StringRef file = "") -> std::vector<index::Occurrence> {
        auto offset = tester.point(pos, file);
        auto fid = file.empty() ? tester.unit->interested_file() : tester.unit->file_id(file);
        auto& index = tu_index.file_indices[fid];

        auto it = std::ranges::lower_bound(
            index.occurrences,
            offset,
            {},
            [](index::Occurrence& occurrence) { return occurrence.range.end; });

        std::vector<index::Occurrence> occurrences;
        while(it != index.occurrences.end()) {
            if(it->range.contains(offset)) {
                occurrences.emplace_back(*it);
                it++;
                continue;
            }

            break;
        }
        return occurrences;
    };

    auto expect_select = [&](llvm::StringRef pos,
                             llvm::StringRef expect_range,
                             llvm::StringRef file = "",
                             std::source_location location = std::source_location::current()) {
        auto offset = tester.point(pos, file);
        auto range = tester.range(expect_range, file);
        auto occurrences = select(pos, file);

        fatal / expect(!occurrences.empty(), location)
            << std::format("Fail to find symbol for offset: {}, target range: {}",
                           offset,
                           dump(range));

        /// FIXME: Make eq pretty print reflectable struct.
        expect(eq(dump(occurrences.front().range), dump(range)), location);
    };

    auto go_to_definition = [&](llvm::StringRef pos,
                                llvm::StringRef definition,
                                llvm::StringRef file = "",
                                std::source_location location = std::source_location::current()) {
        auto offset = tester.point(pos, file);
        auto range = tester.range(definition, file);
        auto occurrences = select(pos, file);

        fatal / expect(occurrences.size() == 1, location)
            << std::format("Fail to find symbol for offset: {}, target range: {}",
                           offset,
                           dump(range));

        auto fid = file.empty() ? tester.unit->interested_file() : tester.unit->file_id(file);
        auto& index = tu_index.file_indices[fid];

        auto it = index.relations.find(occurrences.front().target);
        fatal / expect(it != index.relations.end(), location)
            << std::format("Cannot find target: {}", occurrences.front().target);

        auto& relations = it->second;
        auto target =
            std::ranges::find(relations, RelationKind::Definition, &index::Relation::kind);

        fatal / expect(target != relations.end(), location)
            << std::format("Fail to find definition in {}", dump(relations));
        expect(eq(dump(target->range), dump(range)), location);
    };

    test("Basic") = [&] {
        build_index(R"(
            int @1[f$(1)oo]();

            int @2[b$(2)ar]() {
                return @3[fo$(3)o]() + 1;
            }
        )");

        expect(eq(tu_index.file_indices.size(), 1));
        auto& index = tu_index.file_indices.begin()->second;
        expect(eq(index.relations.size(), 2));
        expect(eq(index.occurrences.size(), 3));

        expect_select("1", "1");
        expect_select("2", "2");
        expect_select("3", "3");
    };

    test("ClassTemplate") = [&] {
        build_index(R"(
            template <typename T, typename U>
            struct $(primary_decl)foo;

            /// using type = $(forward_full)foo<int, int>;

            template <typename T, typename U>
            struct @primary[foo] {};

            template <typename T>
            struct $(partial_spec_decl)foo<T, T>;

            template <typename T>
            struct @partial_spec[foo]<T, T> {};

            template <>
            struct $(full_spec_decl)foo<int, int>;

            template <>
            struct @full_spec[foo]<int, int> {};

            template struct $(explicit_primary)foo<char, int>;

            template struct $(explicit_partial)foo<char, char>;

            $(implicit_primary_1)foo<int, char> b;
            $(implicit_primary_2)foo<char, int> c;
            $(implicit_partial)foo<char, char> d;
            $(implicit_full)foo<int, int> a;
        )");

        go_to_definition("primary_decl", "primary");
        go_to_definition("explicit_primary", "primary");
        go_to_definition("implicit_primary_1", "primary");
        go_to_definition("implicit_primary_2", "primary");
        go_to_definition("partial_spec_decl", "partial_spec");
        go_to_definition("explicit_partial", "partial_spec");
        go_to_definition("implicit_partial", "partial_spec");
        /// FIXME: Figure forward template declaration.
        /// go_to_definition("forward_full", "full_spec");
        go_to_definition("full_spec_decl", "full_spec");
        go_to_definition("implicit_full", "full_spec");
    };

    test("FunctionTemplate") = [&] {
        build_index(R"(
            template <typename T> void $(primary_decl)foo();

            template <typename T> void @primary[foo]() {}

            template <> void $(spec_decl)foo<int>();

            template <> void @spec[foo]<int>() {}

            template void $(explicit_primary)foo<char>();

            int main() {
                $(implicit_primary)foo<char>();
                $(implicit_spec)foo<int>();
            }
        )");

        go_to_definition("primary_decl", "primary");
        /// FIXME: clang doen't record location info of explicit function instantiation/
        /// See https://github.com/llvm/llvm-project/issues/115418.
        /// go_to_definition("explicit_primary", "primary");
        go_to_definition("implicit_primary", "primary");
        go_to_definition("spec_decl", "spec");
        go_to_definition("implicit_spec", "spec");
    };

    test("AliasTemplate") = [&] {
        build_index(R"(
            template <typename T>
            using @primary[foo] = T;

            $(implicit_primary)foo<int> a;
        )");

        go_to_definition("implicit_primary", "primary");
    };

    test("VarTemplate") = [&] {
        build_index(R"(
            template <typename T, typename U>
            extern int $(primary_decl)foo;

            template <typename T, typename U>
            int @primary[foo] = 1;

            template <typename T>
            extern int $(partial_spec_decl)foo<T, T>;

            template <typename T>
            int @partial_spec[foo]<T, T> = 2;

            template <>
            float @full_spec[foo]<int, int> = 1.0f;

            template int $(explicit_primary)foo<char, int>;

            template int $(explicit_partial)foo<char, char>;

            int main() {
                $(implicit_primary_1)foo<int, char> = 1;
                $(implicit_primary_2)foo<char, int> = 2;
                $(implicit_partial)foo<char, char> = 3;
                $(implicit_full)foo<int, int> = 4;
                return 0;
            }
        )");

        go_to_definition("primary_decl", "primary");
        /// go_to_definition("explicit_primary", "primary");
        go_to_definition("implicit_primary_1", "primary");
        go_to_definition("implicit_primary_2", "primary");
        go_to_definition("partial_spec_decl", "partial_spec");
        /// tester.GotoDefinition("explicit_partial", "partial_spec");
        go_to_definition("implicit_partial", "partial_spec");
        go_to_definition("implicit_full", "full_spec");
    };

    test("Concept") = [&] {
        build_index(R"(
            template <typename T>
            concept @primary[$(primary)foo] = true;

            static_assert($(implicit)foo<int>);

            $(implicit2)foo auto bar = 1;
        )");

        go_to_definition("primary", "primary");
        go_to_definition("implicit", "primary");
        go_to_definition("implicit2", "primary");
    };
};

}  // namespace

}  // namespace clice::testing
