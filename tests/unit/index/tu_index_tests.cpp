#include <algorithm>
#include <format>
#include <set>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "index/serialization.h"
#include "index/tu_index.h"
#include "semantic/selection.h"

#include "llvm/Support/thread.h"
#include "clang/Basic/Stack.h"

namespace clice::testing {

namespace lsp = kota::ipc::lsp;

namespace {

TEST_SUITE(tu_index, Tester) {

index::TUIndex tu_index;

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile());

    tu_index = index::TUIndex::build(*unit);
}

auto select(llvm::StringRef pos, llvm::StringRef file = "") -> std::vector<index::Occurrence> {
    auto offset = point(pos, file);
    auto fid = file.empty() ? unit->interested_file() : unit->file_id(file);
    auto& index =
        fid == unit->interested_file() ? tu_index.main_file_index : tu_index.file_indices[fid];

    auto it =
        std::ranges::lower_bound(index.occurrences, offset, {}, [](index::Occurrence& occurrence) {
            return occurrence.range.end;
        });

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
}

void EXPECT_SELECT(llvm::StringRef pos,
                   llvm::StringRef expect_range,
                   llvm::StringRef file = "",
                   std::source_location location = std::source_location::current()) {
    auto offset = point(pos, file);
    auto expected = range(expect_range, file);
    auto occurrences = select(pos, file);

    ASSERT_FALSE(occurrences.empty());
    /// << std::format("Fail to find symbol for offset: {}, target range: {}", offset,
    /// dump(expected));

    /// FIXME: Make eq pretty print reflectable struct.
    ASSERT_EQ(dump(occurrences.front().range), dump(expected));
};

void GO_TO_DEFINITION(llvm::StringRef pos,
                      llvm::StringRef definition,
                      llvm::StringRef file = "",
                      std::source_location location = std::source_location::current()) {
    auto offset = point(pos, file);
    auto expected = range(definition, file);
    auto occurrences = select(pos, file);

    ASSERT_EQ(occurrences.size(), 1U);
    /// << std::format("Fail to find symbol for offset: {}, target range: {}", offset,
    /// dump(expected));

    auto fid = file.empty() ? unit->interested_file() : unit->file_id(file);
    auto& index =
        fid == unit->interested_file() ? tu_index.main_file_index : tu_index.file_indices[fid];

    auto it = index.relations.find(occurrences.front().target);
    ASSERT_TRUE(it != index.relations.end());
    ///<< std::format("Cannot find target: {}", occurrences.front().target);

    auto& relations = it->second;
    auto target = std::ranges::find_if(relations, [](const index::Relation& relation) {
        return relation.kind == RelationKind::Definition;
    });

    ASSERT_TRUE(target != relations.end());
    ///   << std::format("Fail to find definition in {}", dump(relations));
    ASSERT_EQ(dump(target->range), dump(expected));
}

TEST_CASE(Basic) {
    build_index(R"(
            int §(1)⟦f§(1)oo⟧();

            int §(2)⟦b§(2)ar⟧() {
                return §(3)⟦fo§(3)o⟧() + 1;
            }
        )");

    auto& index = tu_index.main_file_index;
    ASSERT_EQ(index.relations.size(), 2U);
    ASSERT_EQ(index.occurrences.size(), 3U);

    EXPECT_SELECT("1", "1");
    EXPECT_SELECT("2", "2");
    EXPECT_SELECT("3", "3");
}

TEST_CASE(ClassTemplate) {
    build_index(R"(
            template <typename T, typename U>
            struct §(primary_decl)foo;

            /// using type = §(forward_full)foo<int, int>;

            template <typename T, typename U>
            struct §(primary)⟦foo⟧ {};

            template <typename T>
            struct §(partial_spec_decl)foo<T, T>;

            template <typename T>
            struct §(partial_spec)⟦foo⟧<T, T> {};

            template <>
            struct §(full_spec_decl)foo<int, int>;

            template <>
            struct §(full_spec)⟦foo⟧<int, int> {};

            template struct §(explicit_primary)foo<char, int>;

            template struct §(explicit_partial)foo<char, char>;

            §(implicit_primary_1)foo<int, char> b;
            §(implicit_primary_2)foo<char, int> c;
            §(implicit_partial)foo<char, char> d;
            §(implicit_full)foo<int, int> a;
        )");

    GO_TO_DEFINITION("primary_decl", "primary");
    GO_TO_DEFINITION("explicit_primary", "primary");
    GO_TO_DEFINITION("implicit_primary_1", "primary");
    GO_TO_DEFINITION("implicit_primary_2", "primary");
    GO_TO_DEFINITION("partial_spec_decl", "partial_spec");
    GO_TO_DEFINITION("explicit_partial", "partial_spec");
    GO_TO_DEFINITION("implicit_partial", "partial_spec");
    /// FIXME: Figure forward template declaration.
    /// GO_TO_DEFINITION("forward_full", "full_spec");
    GO_TO_DEFINITION("full_spec_decl", "full_spec");
    GO_TO_DEFINITION("implicit_full", "full_spec");
}

TEST_CASE(FunctionTemplate) {
    build_index(R"(
            template <typename T> void §(primary_decl)foo();

            template <typename T> void §(primary)⟦foo⟧() {}

            template <> void §(spec_decl)foo<int>();

            template <> void §(spec)⟦foo⟧<int>() {}

            template void §(explicit_primary)foo<char>();

            int main() {
                §(implicit_primary)foo<char>();
                §(implicit_spec)foo<int>();
            }
        )");

    GO_TO_DEFINITION("primary_decl", "primary");
    /// FIXME: clang doen't record location info of explicit function instantiation/
    /// See https://github.com/llvm/llvm-project/issues/115418.
    /// GO_TO_DEFINITION("explicit_primary", "primary");
    GO_TO_DEFINITION("implicit_primary", "primary");
    GO_TO_DEFINITION("spec_decl", "spec");
    GO_TO_DEFINITION("implicit_spec", "spec");
}

TEST_CASE(AliasTemplate) {
    build_index(R"(
            template <typename T>
            using §(primary)⟦foo⟧ = T;

            §(implicit_primary)foo<int> a;
        )");

    GO_TO_DEFINITION("implicit_primary", "primary");
}

TEST_CASE(VarTemplate) {
    build_index(R"(
            template <typename T, typename U>
            extern int §(primary_decl)foo;

            template <typename T, typename U>
            int §(primary)⟦foo⟧ = 1;

            template <typename T>
            extern int §(partial_spec_decl)foo<T, T>;

            template <typename T>
            int §(partial_spec)⟦foo⟧<T, T> = 2;

            template <>
            float §(full_spec)⟦foo⟧<int, int> = 1.0f;

            template int §(explicit_primary)foo<char, int>;

            template int §(explicit_partial)foo<char, char>;

            int main() {
                §(implicit_primary_1)foo<int, char> = 1;
                §(implicit_primary_2)foo<char, int> = 2;
                §(implicit_partial)foo<char, char> = 3;
                §(implicit_full)foo<int, int> = 4;
                return 0;
            }
        )");

    GO_TO_DEFINITION("primary_decl", "primary");
    /// GO_TO_DEFINITION("explicit_primary", "primary");
    GO_TO_DEFINITION("implicit_primary_1", "primary");
    GO_TO_DEFINITION("implicit_primary_2", "primary");
    GO_TO_DEFINITION("partial_spec_decl", "partial_spec");
    /// GotoDefinition("explicit_partial", "partial_spec");
    GO_TO_DEFINITION("implicit_partial", "partial_spec");
    GO_TO_DEFINITION("implicit_full", "full_spec");
}

TEST_CASE(Concept) {
    build_index(R"(
            template <typename T>
            concept §(primary)⟦§(primary)foo⟧ = true;

            static_assert(§(implicit)foo<int>);

            §(implicit2)foo auto bar = 1;
        )");

    GO_TO_DEFINITION("primary", "primary");
    GO_TO_DEFINITION("implicit", "primary");
    GO_TO_DEFINITION("implicit2", "primary");
}

TEST_CASE(Reference) {
    build_index(R"(
            int §(decl)foo = 42;

            int bar() {
                return §(ref)foo + 1;
            }
        )");

    auto& index = tu_index.main_file_index;
    auto occurrences = select("ref");
    ASSERT_EQ(occurrences.size(), 1U);

    auto it = index.relations.find(occurrences.front().target);
    ASSERT_TRUE(it != index.relations.end());

    auto& relations = it->second;
    auto ref = std::ranges::find_if(relations, [](const index::Relation& r) {
        return r.kind == RelationKind::Reference;
    });
    ASSERT_TRUE(ref != relations.end());
}

TEST_CASE(BaseAndDerived) {
    build_index(R"(
            struct §(base)⟦§(base)Base⟧ {
                virtual void foo() {}
            };

            struct §(derived)⟦§(derived)Derived⟧ : public Base {
                void foo() override {}
            };
        )");

    auto& index = tu_index.main_file_index;
    auto base_occs = select("base");
    auto derived_occs = select("derived");
    ASSERT_FALSE(base_occs.empty());
    ASSERT_FALSE(derived_occs.empty());
    auto base_hash = base_occs.front().target;
    auto derived_hash = derived_occs.front().target;

    auto has_pair =
        [&](index::SymbolHash source, RelationKind::Kind kind, index::SymbolHash target) {
            auto it = index.relations.find(source);
            if(it == index.relations.end()) {
                return false;
            }
            for(auto& r: it->second) {
                if(r.kind == kind && r.target_symbol == target) {
                    return true;
                }
            }
            return false;
        };

    ASSERT_TRUE(has_pair(derived_hash, RelationKind::Base, base_hash));
    ASSERT_TRUE(has_pair(base_hash, RelationKind::Derived, derived_hash));
}

TEST_CASE(CallerAndCallee) {
    build_index(R"(
            void §(callee_def)callee() {}

            void §(caller_def)caller() {
                §(call_site)callee();
            }
        )");

    auto& index = tu_index.main_file_index;

    // Find caller symbol and check for Callee relation.
    auto caller_occs = select("caller_def");
    ASSERT_FALSE(caller_occs.empty());
    auto caller_hash = caller_occs.front().target;

    auto caller_it = index.relations.find(caller_hash);
    ASSERT_TRUE(caller_it != index.relations.end());

    bool found_callee = false;
    for(auto& r: caller_it->second) {
        if(r.kind == RelationKind::Callee) {
            found_callee = true;
            break;
        }
    }
    ASSERT_TRUE(found_callee);

    // Find callee symbol and check for Caller relation.
    auto callee_occs = select("callee_def");
    ASSERT_FALSE(callee_occs.empty());
    auto callee_hash = callee_occs.front().target;

    auto callee_it = index.relations.find(callee_hash);
    ASSERT_TRUE(callee_it != index.relations.end());

    bool found_caller = false;
    for(auto& r: callee_it->second) {
        if(r.kind == RelationKind::Caller) {
            found_caller = true;
            break;
        }
    }
    ASSERT_TRUE(found_caller);
}

TEST_CASE(MethodCallerCallee) {
    build_index(R"(
            void §(callee_def)callee() {}

            struct S {
                void §(method_def)method() {
                    §(call_site)callee();
                }
            };
        )");

    auto& index = tu_index.main_file_index;

    // Calls inside a method body produce call edges, with the method as
    // the caller.
    auto method_occs = select("method_def");
    ASSERT_FALSE(method_occs.empty());
    auto method_hash = method_occs.front().target;

    auto callee_occs = select("callee_def");
    ASSERT_FALSE(callee_occs.empty());
    auto callee_hash = callee_occs.front().target;

    auto method_it = index.relations.find(method_hash);
    ASSERT_TRUE(method_it != index.relations.end());

    bool found_callee = false;
    for(auto& r: method_it->second) {
        if(r.kind == RelationKind::Callee && r.target_symbol == callee_hash) {
            found_callee = true;
            break;
        }
    }
    ASSERT_TRUE(found_callee);

    auto callee_it = index.relations.find(callee_hash);
    ASSERT_TRUE(callee_it != index.relations.end());

    bool found_caller = false;
    for(auto& r: callee_it->second) {
        if(r.kind == RelationKind::Caller && r.target_symbol == method_hash) {
            found_caller = true;
            break;
        }
    }
    ASSERT_TRUE(found_caller);
}

TEST_CASE(UsingRelationKey) {
    build_index(R"(
            namespace ns { void §(target)foo(); }
            using ns::§(use)⟦§(use)foo⟧;
        )");

    auto& index = tu_index.main_file_index;

    // The relation row of a using declaration is keyed by the same symbol
    // its occurrence references, so looking the occurrence's symbol up in
    // the relations always finds the using site.
    auto use_occs = select("use");
    ASSERT_FALSE(use_occs.empty());
    auto hash = use_occs.front().target;

    auto it = index.relations.find(hash);
    ASSERT_TRUE(it != index.relations.end());

    bool found_use = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::WeakReference && r.range == range("use")) {
            found_use = true;
            break;
        }
    }
    ASSERT_TRUE(found_use);
}

TEST_CASE(CtorInitMemberRef) {
    build_index(R"(
            struct S {
                int §(def)⟦x⟧;
                S() : §(use)x(1) {}
            };
        )");

    GO_TO_DEFINITION("use", "def");
}

TEST_CASE(DesignatedInitRef) {
    build_index(R"(
            struct Point {
                int §(def)⟦x⟧;
                int y;
            };

            Point p = {.§(use)x = 1};
        )");

    GO_TO_DEFINITION("use", "def");
}

TEST_CASE(RewrittenOperatorRef) {
    build_index(R"(
            namespace std {
            struct strong_ordering {
                int n;
                constexpr operator int() const { return n; }
                static const strong_ordering equal, greater, less;
            };
            constexpr strong_ordering strong_ordering::equal = {0};
            constexpr strong_ordering strong_ordering::greater = {1};
            constexpr strong_ordering strong_ordering::less = {-1};
            }

            struct S {
                int v;
                auto §(def)⟦operator⟧<=>(const S&) const = default;
            };

            bool lt(S a, S b) { return a §(use)< b; }
        )");

    /// a < b is rewritten to (a <=> b) < 0; the operator token references
    /// the rewritten-to operator.
    GO_TO_DEFINITION("use", "def");
}

TEST_CASE(DependentWeakReference) {
    build_index(R"(
            template <typename T>
            struct Base {
                static constexpr int §(target)value = 1;
            };

            template <typename T>
            int use() { return Base<T>::§(use)⟦§(use)value⟧; }
        )");

    auto& index = tu_index.main_file_index;

    /// The dependent name resolves through the template resolver to the
    /// pattern's member; both sites share one symbol.
    auto use_occs = select("use");
    ASSERT_FALSE(use_occs.empty());
    auto target_occs = select("target");
    ASSERT_FALSE(target_occs.empty());
    ASSERT_EQ(use_occs.front().target, target_occs.front().target);

    auto it = index.relations.find(use_occs.front().target);
    ASSERT_TRUE(it != index.relations.end());

    bool found_weak = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::WeakReference && r.range == range("use")) {
            found_weak = true;
            break;
        }
    }
    ASSERT_TRUE(found_weak);
}

TEST_CASE(TypeDefinitionRelations) {
    build_index(R"(
            struct §(s)⟦§(s)S⟧ {};

            struct Holder {
                S §(field)⟦§(field)field⟧;
            };

            using §(alias)⟦§(alias)Alias⟧ = S;

            enum §(e)⟦§(e)E⟧ { §(ec)⟦§(ec)A⟧ };
        )");

    auto& index = tu_index.main_file_index;

    auto has_type_definition = [&](llvm::StringRef source, llvm::StringRef target) {
        auto source_occs = select(source);
        auto target_occs = select(target);
        if(source_occs.empty() || target_occs.empty()) {
            return false;
        }
        auto source_hash = source_occs.front().target;
        auto target_hash = target_occs.front().target;
        auto it = index.relations.find(source_hash);
        if(it == index.relations.end()) {
            return false;
        }
        for(auto& r: it->second) {
            if(r.kind == RelationKind::TypeDefinition && r.target_symbol == target_hash) {
                return true;
            }
        }
        return false;
    };

    ASSERT_TRUE(has_type_definition("field", "s"));
    ASSERT_TRUE(has_type_definition("alias", "s"));
    ASSERT_TRUE(has_type_definition("ec", "e"));
}

TEST_CASE(ConstructorDestructorRelations) {
    build_index(R"(
            struct §(s)⟦§(s)S⟧ {
                §(ctor)S();
                ~S();
            };
        )");

    auto& index = tu_index.main_file_index;
    auto class_occs = select("s");
    auto ctor_occs = select("ctor");
    ASSERT_FALSE(class_occs.empty());
    ASSERT_FALSE(ctor_occs.empty());
    auto class_hash = class_occs.front().target;
    auto ctor_hash = ctor_occs.front().target;

    auto it = index.relations.find(class_hash);
    ASSERT_TRUE(it != index.relations.end());

    bool found_ctor = false;
    bool found_dtor = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Constructor && r.target_symbol == ctor_hash) {
            found_ctor = true;
        }
        if(r.kind == RelationKind::Destructor) {
            found_dtor = true;
        }
    }
    ASSERT_TRUE(found_ctor);
    ASSERT_TRUE(found_dtor);

    // The constructor points back at its class for go-to-type-definition.
    auto ctor_it = index.relations.find(ctor_hash);
    ASSERT_TRUE(ctor_it != index.relations.end());

    bool found_type = false;
    for(auto& r: ctor_it->second) {
        if(r.kind == RelationKind::TypeDefinition && r.target_symbol == class_hash) {
            found_type = true;
        }
    }
    ASSERT_TRUE(found_type);
}

TEST_CASE(MacroRelations) {
    build_index(R"(
            #define §(def)⟦§(def)FOO⟧ 1
            int x = §(use)⟦§(use)FOO⟧;
        )");

    auto& index = tu_index.main_file_index;
    auto def_occs = select("def");
    auto use_occs = select("use");
    ASSERT_FALSE(def_occs.empty());
    ASSERT_FALSE(use_occs.empty());
    ASSERT_EQ(def_occs.front().target, use_occs.front().target);

    auto it = index.relations.find(def_occs.front().target);
    ASSERT_TRUE(it != index.relations.end());

    bool found_definition = false;
    bool found_reference = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition && r.range == range("def")) {
            found_definition = true;
        }
        if(r.kind == RelationKind::Reference && r.range == range("use")) {
            found_reference = true;
        }
    }
    ASSERT_TRUE(found_definition);
    ASSERT_TRUE(found_reference);
}

TEST_CASE(ModuleName) {
    build_index(R"(export module §(m)⟦§(m)foo⟧;)");

    auto& index = tu_index.main_file_index;
    auto occs = select("m");
    ASSERT_FALSE(occs.empty());

    auto it = index.relations.find(occs.front().target);
    ASSERT_TRUE(it != index.relations.end());

    bool found_definition = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            found_definition = true;
        }
    }
    ASSERT_TRUE(found_definition);
}

TEST_CASE(ModulePartitionName) {
    build_index(R"(export module §(m)⟦§(m)foo:part⟧;)");

    // The occurrence spans the whole written name, partition included.
    auto occs = select("m");
    ASSERT_FALSE(occs.empty());
    ASSERT_EQ(occs.front().range, range("m"));

    auto& index = tu_index.main_file_index;
    auto it = index.relations.find(occs.front().target);
    ASSERT_TRUE(it != index.relations.end());

    bool found_definition = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            found_definition = true;
        }
    }
    ASSERT_TRUE(found_definition);
}

TEST_CASE(ImplementationUnitReference) {
    add_files("main.cpp", R"(
#[foo.cppm]
export module foo;
export int x = 1;

#[main.cpp]
module §(m)⟦§(m)foo⟧;
)");
    ASSERT_TRUE(compile_with_modules());
    tu_index = index::TUIndex::build(*unit);

    // An implementation unit's declaration is a Reference, not a Definition.
    auto occs = select("m");
    ASSERT_FALSE(occs.empty());
    ASSERT_EQ(occs.front().range, range("m"));

    auto& index = tu_index.main_file_index;
    auto it = index.relations.find(occs.front().target);
    ASSERT_TRUE(it != index.relations.end());

    bool found_reference = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            ASSERT_TRUE(false);
        }
        if(r.kind == RelationKind::Reference) {
            found_reference = true;
        }
    }
    ASSERT_TRUE(found_reference);
}

TEST_CASE(OverrideRelation) {
    build_index(R"(
            struct Base {
                virtual void method() {}
            };

            struct Derived : Base {
                void method() override {}
            };
        )");

    // The semantic visitor stores:
    //   handleRelation(method, Interface, override, ...)  — overriding method has Interface
    //   handleRelation(override, Implementation, method, ...) — base method has Implementation
    // Search for both relation kinds across all indices.
    bool found_interface = false;
    bool found_implementation = false;

    auto check_relations = [&](index::FileIndex& idx) {
        for(auto& [hash, rels]: idx.relations) {
            for(auto& r: rels) {
                if(r.kind == RelationKind::Interface)
                    found_interface = true;
                if(r.kind == RelationKind::Implementation)
                    found_implementation = true;
            }
        }
    };

    check_relations(tu_index.main_file_index);
    for(auto& [fid, idx]: tu_index.file_indices) {
        check_relations(idx);
    }

    ASSERT_TRUE(found_interface);
    ASSERT_TRUE(found_implementation);
}

TEST_CASE(DeclarationAndDefinition) {
    build_index(R"(
            int §(decl)foo();

            int §(def)⟦§(def)foo⟧() { return 42; }
        )");

    auto& index = tu_index.main_file_index;

    // Find the declaration occurrence and verify Declaration relation exists.
    auto decl_occs = select("decl");
    ASSERT_FALSE(decl_occs.empty());
    auto symbol_hash = decl_occs.front().target;

    auto it = index.relations.find(symbol_hash);
    ASSERT_TRUE(it != index.relations.end());

    bool found_decl = false;
    bool found_def = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Declaration) {
            found_decl = true;
        }
        if(r.kind == RelationKind::Definition) {
            found_def = true;
        }
    }
    ASSERT_TRUE(found_decl);
    ASSERT_TRUE(found_def);
}

TEST_CASE(CrossFileHeaderIndex) {
    add_file("header.h", R"(
            #pragma once
            int §(hdr_func)⟦§(hdr_func)helper⟧();
        )");
    add_main("main.cpp", R"(
            #include "header.h"

            int main() {
                return §(use_helper)helper();
            }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    // The header should have its own FileIndex (separate from main).
    ASSERT_TRUE(tu_index.file_indices.size() >= 1U);

    // The main file should have a reference to helper.
    auto& main_index = tu_index.main_file_index;
    ASSERT_FALSE(main_index.occurrences.empty());

    // Find 'helper' reference in main file.
    auto use_offset = point("use_helper");
    auto it = std::ranges::lower_bound(main_index.occurrences,
                                       use_offset,
                                       {},
                                       [](const index::Occurrence& o) { return o.range.end; });
    ASSERT_TRUE(it != main_index.occurrences.end());
    ASSERT_TRUE(it->range.contains(use_offset));

    // The helper symbol should exist in the TU symbol table.
    auto helper_hash = it->target;
    ASSERT_TRUE(tu_index.symbols.contains(helper_hash));

    // The helper's declaration should be in the header FileIndex.
    bool found_in_header = false;
    for(auto& [fid, file_index]: tu_index.file_indices) {
        for(auto& [sym, rels]: file_index.relations) {
            if(sym == helper_hash) {
                found_in_header = true;
                break;
            }
        }
        if(found_in_header)
            break;
    }
    ASSERT_TRUE(found_in_header);
}

TEST_CASE(SymbolKinds) {
    build_index(R"(
            struct §(cls)MyClass {};
            enum §(enm)MyEnum { A, B };
            void §(func)myFunc() {}
            int §(var)myVar = 0;
            namespace §(ns)MyNS {}
        )");

    auto check_kind = [&](llvm::StringRef name, SymbolKind expected) {
        auto occs = select(name);
        ASSERT_FALSE(occs.empty());
        auto hash = occs.front().target;
        ASSERT_TRUE(tu_index.symbols.contains(hash));
        ASSERT_EQ(tu_index.symbols[hash].kind.value(), expected.value());
    };

    check_kind("cls", SymbolKind::Struct);
    check_kind("enm", SymbolKind::Enum);
    check_kind("func", SymbolKind::Function);
    check_kind("var", SymbolKind::Variable);
    check_kind("ns", SymbolKind::Namespace);
}

TEST_CASE(LookupOccurrence) {
    build_index(R"(
        int §(x)⟦fo§(x)o⟧();
        int §(ref)⟦fo§(ref)o⟧() { return 0; }
    )");

    auto& fi = tu_index.main_file_index;
    ASSERT_FALSE(fi.occurrences.empty());

    auto x_range = range("x");
    const index::Occurrence* found = nullptr;
    fi.lookup(point("x"), [&](const index::Occurrence& occ) {
        found = &occ;
        return true;
    });
    ASSERT_TRUE(found);
    EXPECT_EQ(found->range.begin, x_range.begin);
    EXPECT_EQ(found->range.end, x_range.end);

    found = nullptr;
    fi.lookup(point("ref"), [&](const index::Occurrence& occ) {
        found = &occ;
        return true;
    });
    ASSERT_TRUE(found);
    EXPECT_EQ(found->target, fi.occurrences.front().target);

    found = nullptr;
    fi.lookup(0, [&](const index::Occurrence& occ) {
        found = &occ;
        return false;
    });
    EXPECT_FALSE(found);
}

TEST_CASE(LookupRelation) {
    build_index(R"(
        void §(decl)⟦fo§(decl)o⟧();
        void §(def)⟦fo§(def)o⟧() {}
    )");

    auto& fi = tu_index.main_file_index;

    const index::Occurrence* occ = nullptr;
    fi.lookup(point("decl"), [&](const index::Occurrence& o) {
        occ = &o;
        return false;
    });
    ASSERT_TRUE(occ);

    auto def_range = range("def");
    bool found_def = false;
    fi.lookup(occ->target, RelationKind::Definition, [&](const index::Relation& r) {
        found_def = true;
        EXPECT_EQ(r.range.begin, def_range.begin);
        EXPECT_EQ(r.range.end, def_range.end);
        return false;
    });
    EXPECT_TRUE(found_def);

    bool found_any = false;
    fi.lookup(occ->target, RelationKind::Caller, [&](const index::Relation&) {
        found_any = true;
        return false;
    });
    EXPECT_FALSE(found_any);
}

TEST_CASE(ScopeExternal) {
    build_index(R"(
            int global_var = 0;
            void global_func() {}
            struct GlobalClass { int member; };
            namespace ns { int ns_var = 1; }
        )");

    std::set<std::string> expected{"global_var",
                                   "global_func",
                                   "GlobalClass",
                                   "member",
                                   "ns_var",
                                   "ns"};
    std::set<std::string> found;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(expected.contains(symbol.name)) {
            ASSERT_EQ(static_cast<int>(symbol.scope),
                      static_cast<int>(index::SymbolScope::External));
            found.insert(symbol.name);
        }
    }
    ASSERT_EQ(found, expected);
}

TEST_CASE(ScopeFileLocal) {
    build_index(R"(
            void foo() {
                int local_var = 42;
            }
            void bar(int param) {}
        )");

    std::set<std::string> expected{"local_var", "param"};
    std::set<std::string> found;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(expected.contains(symbol.name)) {
            ASSERT_EQ(static_cast<int>(symbol.scope),
                      static_cast<int>(index::SymbolScope::FileLocal));
            found.insert(symbol.name);
        }
    }
    ASSERT_EQ(found, expected);
}

TEST_CASE(ScopeTULocal) {
    build_index(R"(
            static int static_var = 0;
            static void static_func() {}
            namespace { int anon_var = 1; }
        )");

    std::set<std::string> expected{"static_var", "static_func", "anon_var"};
    std::set<std::string> found;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(expected.contains(symbol.name)) {
            ASSERT_EQ(static_cast<int>(symbol.scope),
                      static_cast<int>(index::SymbolScope::TULocal));
            found.insert(symbol.name);
        }
    }
    ASSERT_EQ(found, expected);
}

TEST_CASE(PreambleDefaultArgument) {
    // An out-of-line definition inherits the default argument expression
    // from the in-class declaration; its DeclRefExpr is located in the
    // preamble header, whose FileID is loaded from the PCH.
    add_file("foo.h", R"(
struct Foo {
    static constexpr int npos = -1;
    int find(int x = npos) const;
};
)");
    add_main("main.cpp", R"(
#include "foo.h"
int Foo::§(def)⟦§(1)find⟧(int x) const { return 0; }
)");
    ASSERT_TRUE(compile_with_pch());

    // A full build keeps the preamble rows: this proves the row exists
    // (so the gate test below cannot pass vacuously) and that the loaded
    // fid resolves through the graph to the header's path.
    tu_index = index::TUIndex::build(*unit);
    bool found = false;
    for(auto& [fid, index]: tu_index.file_indices) {
        found |= tu_index.graph.path(tu_index.graph.path_id(fid)).ends_with("foo.h");
    }
    ASSERT_TRUE(found);

    tu_index = index::TUIndex::build(*unit, true);

    // Rows resolving into the preamble are dropped: the preamble's own
    // index covers them. Only the main file's rows remain.
    ASSERT_TRUE(tu_index.file_indices.empty());
    EXPECT_SELECT("1", "def");
}

TEST_CASE(PreambleBaseSpecifier) {
    // A forward declaration reaches the definition's base specifiers via
    // the shared DefinitionData; their source ranges are in the preamble
    // header, whose FileID is loaded from the PCH.
    add_file("bar.h", R"(
struct Base {};
struct Derived : Base {};
)");
    add_main("main.cpp", R"(
#include "bar.h"
struct Derived;
Derived* use();
)");
    ASSERT_TRUE(compile_with_pch());

    // Full build: the base-specifier rows land in the preamble header
    // and its loaded fid resolves to the header's path.
    tu_index = index::TUIndex::build(*unit);
    bool found = false;
    for(auto& [fid, index]: tu_index.file_indices) {
        found |= tu_index.graph.path(tu_index.graph.path_id(fid)).ends_with("bar.h");
    }
    ASSERT_TRUE(found);

    tu_index = index::TUIndex::build(*unit, true);

    ASSERT_TRUE(tu_index.file_indices.empty());
    ASSERT_FALSE(tu_index.main_file_index.occurrences.empty());
}

TEST_CASE(HeaderMacroDropped) {
    // Macro occurrences flow through the same gate: a definition in an
    // included header is dropped from an interested-only build, while
    // the reference in the main file is kept.
    add_file("baz.h", R"(
#define BAZ 1
)");
    add_main("main.cpp", R"(
#include "baz.h"
int x = §(1)BAZ;
)");
    ASSERT_TRUE(compile());

    tu_index = index::TUIndex::build(*unit, true);
    ASSERT_TRUE(tu_index.file_indices.empty());
    ASSERT_FALSE(tu_index.main_file_index.occurrences.empty());
}

TEST_CASE(UnknownFidFallback) {
    // A preamble header's loaded FileID is unknown to a graph built
    // without indexed fids; lookups must degrade to the interested
    // file instead of crashing.
    add_file("foo.h", R"(
struct Foo {};
)");
    add_main("main.cpp", R"(
#include "foo.h"
int x = 1;
)");
    ASSERT_TRUE(compile_with_pch());

    auto graph = index::IncludeGraph::from(*unit);
    auto fid = unit->file_id(TestVFS::path("foo.h"));
    ASSERT_TRUE(fid.isValid());
    ASSERT_EQ(graph.include_location_id(fid), static_cast<std::uint32_t>(-1));
    ASSERT_EQ(graph.path_id(fid), static_cast<std::uint32_t>(graph.paths.size() - 1));
}

TEST_CASE(PreambleFidResolved) {
    // When a preamble header's fid is passed as an indexed fid, its
    // include chain is recovered through the SourceManager even though
    // this parse's preprocessor callbacks never saw the include.
    add_file("foo.h", R"(
struct Foo {};
)");
    add_main("main.cpp", R"(
#include "foo.h"
int x = 1;
)");
    ASSERT_TRUE(compile_with_pch());

    auto fid = unit->file_id(TestVFS::path("foo.h"));
    ASSERT_TRUE(fid.isValid());

    auto graph = index::IncludeGraph::from(*unit, {fid});
    auto include = graph.include_location_id(fid);
    ASSERT_TRUE(include != static_cast<std::uint32_t>(-1));
    ASSERT_TRUE(graph.path(graph.path_id(fid)).ends_with("foo.h"));
}

TEST_CASE(DeepExpressionChain) {
    // Doubling macros expand to a ~32k-term binary expression chain.
    // Regression test: indexing such an AST must not overflow the stack.
    std::string code = "#define A0 1+1\n";
    for(int i = 1; i <= 14; i++) {
        code += std::format("#define A{} A{}+A{}\n", i, i - 1, i - 1);
    }
    code += "int bomb = A14;\n";
    auto bomb_offset = static_cast<std::uint32_t>(code.find("bomb"));

    add_main("main.cpp", code);

    // Compile on a generous fixed-size stack: clang's own Sema checkers
    // recurse once per term and need more than a default thread stack in
    // sanitized builds (and Windows main threads only get 1MB). 32MB is
    // an empirical bound with margin, not a derived number.
    bool compiled = false;
    llvm::thread compile_thread(std::optional<unsigned>(4 * clang::DesiredStackSize),
                                [&] { compiled = compile(); });
    compile_thread.join();
    ASSERT_TRUE(compiled);

    // Index on a deliberately tight stack: the traversal must use
    // constant stack space however deep the expression is, so any
    // reintroduced per-node recursion crashes here deterministically
    // instead of only on production workers with deeper files.
    feature::InactiveScan scan;
    llvm::thread index_thread(std::optional<unsigned>(clang::DesiredStackSize / 4), [&] {
        // Mirror the stateful worker's post-compile sequence.
        scan = feature::inactive_regions(*unit);
        tu_index = index::TUIndex::build(*unit, true);

        // The semantic map must also serve token classification and a
        // selection at the giant expansion's invocation on this stack:
        // per-token ancestor walks and recursive tree materialization
        // both used to degrade on chains this deep.
        feature::semantic_tokens(*unit);
        auto use_offset = static_cast<std::uint32_t>(code.rfind("A14"));
        SelectionTree::create_right(*unit, LocalSourceRange(use_offset, use_offset));
    });
    index_thread.join();

    ASSERT_TRUE(scan.regions.empty());

    // The traversal must have actually reached the decl behind the chain,
    // not bailed out early: expect an occurrence exactly at `bomb`.
    auto& occurrences = tu_index.main_file_index.occurrences;
    auto bomb = std::ranges::find(occurrences, bomb_offset, [](index::Occurrence& occurrence) {
        return occurrence.range.begin;
    });
    ASSERT_TRUE(bomb != occurrences.end());
}

TEST_CASE(SuperQualifierRef) {
    add_main("main.cpp", R"(
            struct Base {
                void m();
            };
            struct §(def)⟦Derived⟧ : Base {
                void f() { §(use)__super::m(); }
            };
        )");
    prepare("-std=c++20");
    /// __super needs Microsoft extensions; splice the flag in before the
    /// trailing source path.
    owned_args.insert(owned_args.end() - 1, "-fms-extensions");
    params.arguments.clear();
    for(auto& arg: owned_args) {
        params.arguments.push_back(arg.c_str());
    }
    ASSERT_TRUE(try_compile());
    tu_index = index::TUIndex::build(*unit);

    GO_TO_DEFINITION("use", "def");
}

TEST_CASE(SerializeRoundTrip) {
    add_file("header.h", R"(
            #pragma once
            inline int §(hdr)helper() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int main() { return §(use)helper(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);
    ASSERT_FALSE(tu_index.file_indices.empty());

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    tu_index.serialize(os);

    auto loaded = index::TUIndex::from(buf);
    ASSERT_TRUE(loaded.has_value());

    ASSERT_EQ(loaded->built_at.count(), tu_index.built_at.count());
    ASSERT_TRUE(loaded->graph.paths == tu_index.graph.paths);
    ASSERT_TRUE(loaded->graph.locations == tu_index.graph.locations);
    ASSERT_TRUE(loaded->graph.path_hashes == tu_index.graph.path_hashes);

    // The persisted per-file rows are keyed by path id; recompute the
    // expected conversion from the build-time FileID-keyed state.
    llvm::DenseMap<std::uint32_t, std::pair<std::size_t, std::size_t>> expected;
    for(auto& [fid, file_index]: tu_index.file_indices) {
        expected[tu_index.graph.path_id(fid)] = {file_index.occurrences.size(),
                                                 file_index.relations.size()};
    }
    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(loaded->path_file_indices.size(), expected.size());
    for(auto& [path_id, counts]: expected) {
        auto it = loaded->path_file_indices.find(path_id);
        ASSERT_TRUE(it != loaded->path_file_indices.end());
        ASSERT_EQ(it->second.occurrences.size(), counts.first);
        ASSERT_EQ(it->second.relations.size(), counts.second);
    }

    ASSERT_TRUE(loaded->main_file_index.occurrences == tu_index.main_file_index.occurrences);
    ASSERT_EQ(loaded->main_file_index.relations.size(), tu_index.main_file_index.relations.size());

    ASSERT_EQ(loaded->symbols.size(), tu_index.symbols.size());
    for(auto& [hash, symbol]: tu_index.symbols) {
        auto it = loaded->symbols.find(hash);
        ASSERT_TRUE(it != loaded->symbols.end());
        ASSERT_EQ(it->second.name, symbol.name);
        ASSERT_EQ(it->second.kind.value(), symbol.kind.value());
        ASSERT_EQ(static_cast<int>(it->second.scope), static_cast<int>(symbol.scope));
        ASSERT_TRUE(it->second.reference_files == symbol.reference_files);
    }
}

TEST_CASE(FromRejectsHostileInput) {
    ASSERT_FALSE(index::TUIndex::from("not a flatbuffer at all").has_value());

    build_index(R"(
            int foo() { return 42; }
        )");

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    tu_index.serialize(os);

    // Sanity: the intact blob loads, so the rejections below are earned.
    ASSERT_TRUE(index::TUIndex::from(buf).has_value());

    ASSERT_FALSE(index::TUIndex::from(llvm::StringRef(buf.data(), buf.size() / 2)).has_value());

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    ASSERT_TRUE(buf.size() > 8);
    std::string clobbered(buf.data(), buf.size());
    for(std::size_t i = 4; i < 8; ++i) {
        clobbered[i] = 'X';
    }
    ASSERT_FALSE(index::TUIndex::from(clobbered).has_value());
}

TEST_CASE(FromRejectsStaleFormatVersion) {
    // Only the version slot is written: every other field reads back
    // absent, which is structurally valid — the verdict must hinge on the
    // value. Field order MUST mirror TUIndex (tu_index.h): format_version
    // is slot 0.
    struct VersionOnly {
        std::uint32_t format_version = 0;
    };

    auto bytes_of = [](const std::vector<std::uint8_t>& blob) {
        return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
    };

    auto stale = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version + 1});
    ASSERT_TRUE(stale.has_value());
    ASSERT_FALSE(index::TUIndex::from(bytes_of(*stale)).has_value());

    // Positive control: the same shape carrying the current version loads,
    // so the rejection above comes from the value, not the blob's shape.
    auto current = kota::codec::fbs::to_bytes(VersionOnly{index::index_format_version});
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(index::TUIndex::from(bytes_of(*current)).has_value());
}

TEST_CASE(FromRejectsOutOfRangePathIds) {
    // Structural verification does not constrain field values, and the
    // merge pipeline dereferences every decoded path id against the path
    // table without further checks (Indexer::merge indexes paths and
    // path_hashes, ProjectIndex::merge indexes file_ids_map with
    // reference_files values) — a blob pointing outside its own table must
    // be rejected as a whole.
    auto serialized = [](index::TUIndex& index) {
        std::string buf;
        llvm::raw_string_ostream os(buf);
        index.serialize(os);
        return buf;
    };

    // Positive control first: the same shapes with in-range ids load, so
    // the rejections below come from the hostile values.
    index::TUIndex honest;
    honest.built_at = std::chrono::milliseconds(0);
    honest.graph.paths = {"/proj/main.cpp"};
    honest.graph.locations.push_back({.path_id = 0, .line = 1, .include = 0});
    honest.path_file_indices.try_emplace(0);
    honest.symbols[42].reference_files.add(0);
    ASSERT_TRUE(index::TUIndex::from(serialized(honest)).has_value());

    {
        index::TUIndex hostile;
        hostile.built_at = std::chrono::milliseconds(0);
        hostile.graph.paths = {"/proj/main.cpp"};
        hostile.graph.locations.push_back({.path_id = 7, .line = 1, .include = 0});
        ASSERT_FALSE(index::TUIndex::from(serialized(hostile)).has_value());
    }
    {
        index::TUIndex hostile;
        hostile.built_at = std::chrono::milliseconds(0);
        hostile.graph.paths = {"/proj/main.cpp"};
        hostile.path_file_indices.try_emplace(7);  // Only path id 0 exists.
        ASSERT_FALSE(index::TUIndex::from(serialized(hostile)).has_value());
    }
    {
        index::TUIndex hostile;
        hostile.built_at = std::chrono::milliseconds(0);
        hostile.graph.paths = {"/proj/main.cpp"};
        hostile.symbols[42].reference_files.add(7);
        ASSERT_FALSE(index::TUIndex::from(serialized(hostile)).has_value());
    }
}

TEST_CASE(FromNormalizesPathHashes) {
    build_index(R"(
            int foo() { return 42; }
        )");
    ASSERT_FALSE(tu_index.graph.paths.empty());

    // A blob without path hashes (structurally valid: the field reads back
    // empty) must come back resized to the path table, all "unavailable".
    tu_index.graph.path_hashes.clear();
    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    tu_index.serialize(os);

    auto loaded = index::TUIndex::from(buf);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->graph.path_hashes.size(), loaded->graph.paths.size());
    for(auto hash: loaded->graph.path_hashes) {
        ASSERT_EQ(hash, 0u);
    }
}

TEST_CASE(ReserializeKeepsPathIndices) {
    add_file("header.h", R"(
            #pragma once
            inline int helper() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int main() { return helper(); }
        )");
    ASSERT_TRUE(compile());
    tu_index = index::TUIndex::build(*unit);

    llvm::SmallString<4096> buf;
    llvm::raw_svector_ostream os(buf);
    tu_index.serialize(os);

    auto loaded = index::TUIndex::from(buf);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->file_indices.empty());
    ASSERT_FALSE(loaded->path_file_indices.empty());

    // A deserialized index has no FileID-keyed state; re-serializing must
    // keep the path-keyed rows instead of wiping them from an empty map.
    llvm::SmallString<4096> again;
    llvm::raw_svector_ostream os2(again);
    loaded->serialize(os2);

    auto reloaded = index::TUIndex::from(again);
    ASSERT_TRUE(reloaded.has_value());
    ASSERT_EQ(reloaded->path_file_indices.size(), loaded->path_file_indices.size());
    for(auto& [path_id, file_index]: loaded->path_file_indices) {
        ASSERT_TRUE(reloaded->path_file_indices.contains(path_id));
    }
}

};  // TEST_SUITE(tu_index)

}  // namespace
}  // namespace clice::testing
