#include "test/test.h"
#include "test/tester.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "index/tu_index.h"

namespace clice::testing {
namespace {

TEST_SUITE(IndexQuery, Tester) {

index::ProjectIndex project_index;
llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;

/// Build TUIndex from code and merge into ProjectIndex + MergedIndex shards.
void build_and_merge(llvm::StringRef code,
                     std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    auto tu_index = index::TUIndex::build(*unit);
    auto file_ids_map = project_index.merge(tu_index);

    // Merge main file index as compilation context.
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    auto main_global_id = file_ids_map[main_tu_path_id];

    std::vector<index::IncludeLocation> include_locs;
    for(auto& loc: tu_index.graph.locations) {
        index::IncludeLocation remapped = loc;
        remapped.path_id = file_ids_map[loc.path_id];
        include_locs.push_back(remapped);
    }

    merged_indices[main_global_id].merge(main_global_id,
                                         tu_index.built_at,
                                         std::move(include_locs),
                                         tu_index.main_file_index);

    // Merge header file indices.
    for(auto& [fid, file_idx]: tu_index.file_indices) {
        auto tu_pid = tu_index.graph.path_id(fid);
        auto global_pid = file_ids_map[tu_pid];
        auto include_id = tu_index.graph.include_location_id(fid);
        merged_indices[global_pid].merge(global_pid, include_id, file_idx);
    }
}

/// Reset index state between test cases.
void reset() {
    project_index = index::ProjectIndex();
    merged_indices.clear();
    clear();
}

/// Lookup the symbol hash at a given annotation offset in any merged index.
index::SymbolHash lookup_symbol(llvm::StringRef pos) {
    auto offset = point(pos);
    index::SymbolHash result = 0;
    for(auto& [path_id, merged]: merged_indices) {
        merged.lookup(offset, [&](const index::Occurrence& o) {
            if(o.range.contains(offset)) {
                result = o.target;
                return false;
            }
            return true;
        });
        if(result != 0)
            break;
    }
    return result;
}

/// Find all relations of a given kind for a symbol across all merged indices.
std::vector<index::Relation> find_relations(index::SymbolHash symbol, RelationKind kind) {
    std::vector<index::Relation> results;

    auto sym_it = project_index.symbols.find(symbol);
    if(sym_it == project_index.symbols.end())
        return results;

    // Search every shard that references this symbol.
    for(auto file_id: sym_it->second.reference_files) {
        auto it = merged_indices.find(file_id);
        if(it == merged_indices.end())
            continue;

        it->second.lookup(symbol, kind, [&](const index::Relation& r) {
            results.push_back(r);
            return true;
        });
    }

    // Also search all shards (symbol may appear in files not tracked by reference_files).
    if(results.empty()) {
        for(auto& [pid, merged]: merged_indices) {
            merged.lookup(symbol, kind, [&](const index::Relation& r) {
                results.push_back(r);
                return true;
            });
        }
    }

    return results;
}

// ============================================================
// Test cases
// ============================================================

TEST_CASE(GoToDefinition) {
    reset();
    build_and_merge(R"(
        int $(decl)foo();

        int @def[$(def)foo]() { return 42; }

        int main() {
            return $(use)foo();
        }
    )");

    auto hash = lookup_symbol("use");
    ASSERT_NE(hash, 0UL);

    auto defs = find_relations(hash, RelationKind::Definition);
    ASSERT_FALSE(defs.empty());

    auto expected = range("def");
    ASSERT_EQ(dump(defs.front().range), dump(expected));
}

TEST_CASE(FindReferences) {
    reset();
    build_and_merge(R"(
        int $(decl)foo();

        int $(def)foo() { return 42; }

        int bar() {
            return $(ref1)foo() + $(ref2)foo();
        }
    )");

    auto hash = lookup_symbol("decl");
    ASSERT_NE(hash, 0UL);

    auto refs = find_relations(hash, RelationKind::Reference);
    ASSERT_GE(refs.size(), 2U);
}

TEST_CASE(DeclAndDef) {
    reset();
    build_and_merge(R"(
        int $(decl)foo();
        int @def[$(def)foo]() { return 42; }
    )");

    auto hash = lookup_symbol("decl");
    ASSERT_NE(hash, 0UL);

    auto decls = find_relations(hash, RelationKind::Declaration);
    ASSERT_FALSE(decls.empty());

    auto defs = find_relations(hash, RelationKind::Definition);
    ASSERT_FALSE(defs.empty());

    auto expected_def = range("def");
    ASSERT_EQ(dump(defs.front().range), dump(expected_def));
}

TEST_CASE(CallerCallee) {
    reset();
    build_and_merge(R"(
        void $(callee_def)callee() {}

        void $(caller_def)caller() {
            $(call_site)callee();
        }
    )");

    auto caller_hash = lookup_symbol("caller_def");
    ASSERT_NE(caller_hash, 0UL);

    auto callees = find_relations(caller_hash, RelationKind::Callee);
    ASSERT_FALSE(callees.empty());

    auto callee_hash = lookup_symbol("callee_def");
    ASSERT_NE(callee_hash, 0UL);

    auto callers = find_relations(callee_hash, RelationKind::Caller);
    ASSERT_FALSE(callers.empty());
}

TEST_CASE(OverrideRelation) {
    reset();
    build_and_merge(R"(
        struct Base {
            virtual void $(base_method)method() {}
        };

        struct Derived : Base {
            void $(derived_method)method() override {}
        };
    )");

    // Derived::method should have Interface relation to Base::method.
    auto derived_hash = lookup_symbol("derived_method");
    ASSERT_NE(derived_hash, 0UL);

    auto interfaces = find_relations(derived_hash, RelationKind::Interface);
    ASSERT_FALSE(interfaces.empty());

    // Base::method should have Implementation relation.
    auto base_hash = lookup_symbol("base_method");
    ASSERT_NE(base_hash, 0UL);

    auto impls = find_relations(base_hash, RelationKind::Implementation);
    ASSERT_FALSE(impls.empty());
}

TEST_CASE(BaseAndDerived) {
    reset();
    build_and_merge(R"(
        struct $(base_cls)Animal {
            virtual void speak() {}
        };

        struct $(derived_cls)Dog : $(base_ref)Animal {
            void speak() override {}
        };
    )");

    auto derived_hash = lookup_symbol("derived_cls");
    ASSERT_NE(derived_hash, 0UL);

    // Look for any Base relation in any shard.
    bool found_base = false;
    for(auto& [pid, merged]: merged_indices) {
        merged.lookup(derived_hash, RelationKind::Base, [&](const index::Relation& r) {
            found_base = true;
            return false;
        });
    }
    ASSERT_TRUE(found_base);
}

TEST_CASE(ClassTemplate) {
    reset();
    build_and_merge(R"(
        template <typename T>
        struct @primary[$(primary)foo] {};

        $(use)foo<int> x;
    )");

    auto hash = lookup_symbol("use");
    ASSERT_NE(hash, 0UL);

    auto defs = find_relations(hash, RelationKind::Definition);
    ASSERT_FALSE(defs.empty());
}

TEST_CASE(SymbolKinds) {
    reset();
    build_and_merge(R"(
        struct $(cls)MyClass {};
        void $(func)myFunc() {}
        int $(var)myVar = 0;
    )");

    auto cls_hash = lookup_symbol("cls");
    ASSERT_NE(cls_hash, 0UL);
    ASSERT_TRUE(project_index.symbols.contains(cls_hash));
    ASSERT_EQ(project_index.symbols[cls_hash].kind.value(), SymbolKind(SymbolKind::Struct).value());

    auto func_hash = lookup_symbol("func");
    ASSERT_NE(func_hash, 0UL);
    ASSERT_TRUE(project_index.symbols.contains(func_hash));
    ASSERT_EQ(project_index.symbols[func_hash].kind.value(),
              SymbolKind(SymbolKind::Function).value());

    auto var_hash = lookup_symbol("var");
    ASSERT_NE(var_hash, 0UL);
    ASSERT_TRUE(project_index.symbols.contains(var_hash));
    ASSERT_EQ(project_index.symbols[var_hash].kind.value(),
              SymbolKind(SymbolKind::Variable).value());
}

TEST_CASE(ReferenceFiles) {
    reset();
    build_and_merge(R"(
        int $(target)target = 42;
        int a = $(ref)target + 1;
    )");

    auto hash = lookup_symbol("target");
    ASSERT_NE(hash, 0UL);

    auto sym_it = project_index.symbols.find(hash);
    ASSERT_TRUE(sym_it != project_index.symbols.end());

    // reference_files should contain at least the main file.
    ASSERT_FALSE(sym_it->second.reference_files.isEmpty());
}

TEST_CASE(CrossFileQuery) {
    reset();

    add_file("header.h", R"(
        #pragma once
        int $(hdr_decl)helper();
    )");
    add_main("main.cpp", R"(
        #include "header.h"

        int main() {
            return $(use_helper)helper();
        }
    )");
    ASSERT_TRUE(compile());

    auto tu_index = index::TUIndex::build(*unit);
    auto file_ids_map = project_index.merge(tu_index);

    // Merge main file.
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    auto main_global_id = file_ids_map[main_tu_path_id];

    std::vector<index::IncludeLocation> include_locs;
    for(auto& loc: tu_index.graph.locations) {
        index::IncludeLocation remapped = loc;
        remapped.path_id = file_ids_map[loc.path_id];
        include_locs.push_back(remapped);
    }
    merged_indices[main_global_id].merge(main_global_id,
                                         tu_index.built_at,
                                         std::move(include_locs),
                                         tu_index.main_file_index);

    // Merge header file indices.
    for(auto& [fid, file_idx]: tu_index.file_indices) {
        auto tu_pid = tu_index.graph.path_id(fid);
        auto global_pid = file_ids_map[tu_pid];
        auto include_id = tu_index.graph.include_location_id(fid);
        merged_indices[global_pid].merge(global_pid, include_id, file_idx);
    }

    // Query: from usage in main.cpp, find the symbol via merged index.
    auto use_offset = point("use_helper");
    index::SymbolHash helper_hash = 0;
    merged_indices[main_global_id].lookup(use_offset, [&](const index::Occurrence& o) {
        if(o.range.contains(use_offset)) {
            helper_hash = o.target;
            return false;
        }
        return true;
    });
    ASSERT_NE(helper_hash, 0UL);

    // Find declaration across all shards -- should find it in header shard.
    auto decls = find_relations(helper_hash, RelationKind::Declaration);
    ASSERT_FALSE(decls.empty());
}

};  // TEST_SUITE(IndexQuery)
}  // namespace
}  // namespace clice::testing
