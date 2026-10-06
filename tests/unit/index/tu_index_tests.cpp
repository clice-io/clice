module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

#include "clang/Basic/Stack.h"

module clice;

import :compile.selection;
import :feature.feature;
import :index.serialization;
import :index.shard;
import :index.tu_index;
import :support.logging;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace lsp = kota::ipc::lsp;

namespace {

ZEST_SUITE(tu_index, Tester) {

/// One file's rows read back out of its envelope section through the
/// Shard reader — every assertion below therefore exercises the full
/// build → encode → read roundtrip, not builder-internal state.
struct DecodedRows {
    std::vector<index::Occurrence> occurrences;
    llvm::DenseMap<index::SymbolHash, std::vector<index::Relation>> relations{};

    bool empty() const {
        return occurrences.empty() && relations.empty();
    }
};

struct DecodedIndex {
    index::TUIndex view;
    DecodedRows main_file_index;
    /// Non-main sections keyed by path id.
    std::vector<std::pair<std::uint32_t, DecodedRows>> file_indices;
    index::SymbolTable symbols{};
};

DecodedIndex tu_index;

DecodedRows decode_rows(const index::Shard& shard) {
    DecodedRows rows;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        rows.occurrences.push_back(occurrence);
        return true;
    });
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        rows.relations[hash].push_back(relation);
        return true;
    });
    return rows;
}

void decode_index(const std::string& envelope) {
    tu_index = {};
    tu_index.view = index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(envelope));
    auto& view = tu_index.view;
    ZASSERT(view.loaded());

    auto main_path = view.path_count() - 1;
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto rows = decode_rows(view.shard_of(view.section_path(i)));
        if(view.section_path(i) == main_path) {
            tu_index.main_file_index = std::move(rows);
        } else {
            tu_index.file_indices.emplace_back(view.section_path(i), std::move(rows));
        }
    }

    view.iterate_symbols(
        [&](index::SymbolHash hash, const index::SymbolIdentity& identity, llvm::StringRef bitmap) {
            auto& symbol = tu_index.symbols[hash];
            symbol.name = identity.name.str();
            symbol.args = identity.args.str();
            symbol.parent = identity.parent;
            symbol.kind = identity.kind;
            symbol.scope = identity.scope;
            symbol.flags = identity.flags;
            symbol.file = identity.file;
            symbol.reference_files =
                index::read_bitmap(bitmap.data(), bitmap.size()).value_or(Bitmap{});
            return true;
        });
}

/// The one symbol with this name (and specialization arguments) in the
/// decoded table.
std::pair<index::SymbolHash, index::Symbol> symbol_named(llvm::StringRef name,
                                                         llvm::StringRef args = "",
                                                         std::optional<SymbolKind> kind = {}) {
    std::optional<std::pair<index::SymbolHash, index::Symbol>> found;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(symbol.name == name && symbol.args == args &&
           (!kind || symbol.kind.value() == kind->value())) {
            if(found) {
                LOG_FATAL("symbol {}{} is not unique", name, args);
            }
            found.emplace(hash, symbol);
        }
    }
    if(!found) {
        LOG_FATAL("no symbol {}{}", name, args);
    }
    return *found;
}

bool has(const index::Symbol& symbol, index::SymbolFlags flag) {
    return index::has_flag(symbol.flags, flag);
}

int scope(llvm::StringRef name) {
    return static_cast<int>(symbol_named(name).second.scope);
}

void build_index(llvm::StringRef code,
                 std::source_location location = std::source_location::current()) {
    add_main("main.cpp", code);
    ZASSERT(compile());

    decode_index(index::build_tu_index(*unit));
}

auto select(llvm::StringRef pos) -> std::vector<index::Occurrence> {
    auto offset = point(pos);
    auto& index = tu_index.main_file_index;

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
                   std::source_location location = std::source_location::current()) {
    auto expected = range(expect_range);
    auto occurrences = select(pos);

    ZASSERT(!occurrences.empty());

    /// FIXME: Make eq pretty print reflectable struct.
    ZASSERT(dump(occurrences.front().range) == dump(expected));
};

/// Whether the main file holds a symbol-pair row of `kind` from the
/// symbol at marker `source` to the one at marker `target`.
bool has_pair(llvm::StringRef source, RelationKind::Kind kind, llvm::StringRef target) {
    auto sources = select(source);
    auto targets = select(target);
    if(sources.empty() || targets.empty()) {
        return false;
    }
    auto it = tu_index.main_file_index.relations.find(sources.front().target);
    if(it == tu_index.main_file_index.relations.end()) {
        return false;
    }
    return llvm::any_of(it->second, [&](const index::Relation& relation) {
        return relation.kind == kind && relation.target_symbol == targets.front().target;
    });
}

void GO_TO_DEFINITION(llvm::StringRef pos,
                      llvm::StringRef definition,
                      std::source_location location = std::source_location::current()) {
    auto expected = range(definition);
    auto occurrences = select(pos);

    ZASSERT(occurrences.size() == 1U);

    auto& index = tu_index.main_file_index;
    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());
    ///<< std::format("Cannot find target: {}", occurrences.front().target);

    auto& relations = it->second;
    auto target = std::ranges::find_if(relations, [](const index::Relation& relation) {
        return relation.kind == RelationKind::Definition;
    });

    ZASSERT(target != relations.end());
    ///   << std::format("Fail to find definition in {}", dump(relations));
    ZASSERT(dump(target->range) == dump(expected));
}

ZEST_CASE(Basic) {
    build_index(R"(
            int §(1)⟦f§(1)oo⟧();

            int §(2)⟦b§(2)ar⟧() {
                return §(3)⟦fo§(3)o⟧() + 1;
            }
        )");

    auto& index = tu_index.main_file_index;
    ZASSERT(index.relations.size() == 2U);
    ZASSERT(index.occurrences.size() == 3U);

    EXPECT_SELECT("1", "1");
    EXPECT_SELECT("2", "2");
    EXPECT_SELECT("3", "3");
}

ZEST_CASE(ClassTemplate) {
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

ZEST_CASE(FunctionTemplate) {
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

ZEST_CASE(InstantiationLocalCollapse) {
    build_index(R"(
            struct Fn {
                template <typename T>
                int operator()(T §(parm)x) const {
                    int §(local)loc = 1;
                    return §(local_ref)loc + static_cast<int>(§(parm_ref)x);
                }
            };
            constexpr Fn fn{};
            int a = fn(1);
            int b = fn(2.0);
        )");

    for(auto pos: {"parm", "local", "local_ref", "parm_ref"}) {
        auto occurrences = select(pos);
        std::set<index::SymbolHash> targets;
        for(auto& occurrence: occurrences) {
            targets.insert(occurrence.target);
        }
        ZEXPECT(targets.size() == 1U);
    }
}

ZEST_CASE(LambdaCaptureCollapse) {
    build_index(R"(
            struct Fn {
                template <typename T>
                int operator()(T §(parm)x) const {
                    auto lambda = [&] {
                        return static_cast<int>(§(parm_ref)x);
                    };
                    return lambda();
                }
            };
            constexpr Fn fn{};
            int a = fn(1);
            int b = fn(2.0);
        )");

    for(auto pos: {"parm", "parm_ref"}) {
        auto occurrences = select(pos);
        std::set<index::SymbolHash> targets;
        for(auto& occurrence: occurrences) {
            targets.insert(occurrence.target);
        }
        ZEXPECT(targets.size() == 1U);
    }
}

ZEST_CASE(AliasTemplate) {
    build_index(R"(
            template <typename T>
            using §(primary)⟦foo⟧ = T;

            §(implicit_primary)foo<int> a;
        )");

    GO_TO_DEFINITION("implicit_primary", "primary");
}

ZEST_CASE(VarTemplate) {
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

ZEST_CASE(Concept) {
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

ZEST_CASE(Reference) {
    build_index(R"(
            int §(decl)foo = 42;

            int bar() {
                return §(ref)foo + 1;
            }
        )");

    auto& index = tu_index.main_file_index;
    auto occurrences = select("ref");
    ZASSERT(occurrences.size() == 1U);

    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());

    auto& relations = it->second;
    auto ref = std::ranges::find_if(relations, [](const index::Relation& r) {
        return r.kind == RelationKind::Reference;
    });
    ZASSERT(ref != relations.end());
}

ZEST_CASE(BaseAndDerived) {
    build_index(R"(
            struct §(base)⟦§(base)Base⟧ {
                virtual void foo() {}
            };

            struct §(derived)⟦§(derived)Derived⟧ : public Base {
                void foo() override {}
            };
        )");

    ZASSERT(has_pair("derived", RelationKind::Base, "base"));
    ZASSERT(has_pair("base", RelationKind::Derived, "derived"));
}

ZEST_CASE(SpecializationRelations) {
    build_index(R"(
            template <class T>
            struct §(box)Box {
                void §(get)get();
                static int §(count)count;
            };
            template <>
            struct §(box_char)Box<char> {};
            template <class T>
            struct §(box_ptr)Box<T*> {};
            template <>
            void Box<int>::§(get_int)get() {}
            template <>
            int Box<long>::§(count_long)count = 0;

            template <class T>
            void §(fn)fn(T) {}
            template <class T>
            void §(fn_ptr)fn(T*) {}
            template <>
            void §(fn_int)fn<int>(int*) {}

            template <class T>
            constexpr int §(width)width = 0;
            template <>
            constexpr int §(width_char)width<char> = 1;
            template <class T>
            constexpr int §(width_ptr)width<T*> = 2;

            template <class T>
            struct Outer {
                struct §(inner)Inner;
            };
            template <>
            struct Outer<int>::§(inner_int)Inner {};

            template struct Box<short>;
            Box<double> used;
        )");

    for(auto [primary, specialization]: {
            std::pair{"box",    "box_char"  },
            std::pair{"box",    "box_ptr"   },
            std::pair{"get",    "get_int"   },
            std::pair{"count",  "count_long"},
            std::pair{"fn_ptr", "fn_int"    },
            std::pair{"width",  "width_char"},
            std::pair{"width",  "width_ptr" },
            std::pair{"inner",  "inner_int" },
    }) {
        ZASSERT(has_pair(primary, RelationKind::Specialization, specialization));
        ZASSERT(has_pair(specialization, RelationKind::Primary, primary));
    }
    // The overload the specialization does not specialize stays unrelated.
    ZASSERT(select("fn").size() == 1U);
    ZASSERT(!has_pair("fn", RelationKind::Specialization, "fn_int"));

    // Instantiations, explicit or implicit, specialize nothing.
    auto& rows = tu_index.main_file_index.relations[select("box").front().target];
    ZASSERT(llvm::count_if(rows, [](const index::Relation& relation) {
                return relation.kind == RelationKind::Specialization;
            }) == 2);
}

ZEST_CASE(CallerAndCallee) {
    build_index(R"(
            void §(callee_def)callee() {}

            void §(caller_def)caller() {
                §(call_site)callee();
            }
        )");

    auto& index = tu_index.main_file_index;

    // Find caller symbol and check for Callee relation.
    auto caller_occs = select("caller_def");
    ZASSERT(!caller_occs.empty());
    auto caller_hash = caller_occs.front().target;

    auto caller_it = index.relations.find(caller_hash);
    ZASSERT(caller_it != index.relations.end());

    bool found_callee = false;
    for(auto& r: caller_it->second) {
        if(r.kind == RelationKind::Callee) {
            found_callee = true;
            break;
        }
    }
    ZASSERT(found_callee);

    // Find callee symbol and check for Caller relation.
    auto callee_occs = select("callee_def");
    ZASSERT(!callee_occs.empty());
    auto callee_hash = callee_occs.front().target;

    auto callee_it = index.relations.find(callee_hash);
    ZASSERT(callee_it != index.relations.end());

    bool found_caller = false;
    for(auto& r: callee_it->second) {
        if(r.kind == RelationKind::Caller) {
            found_caller = true;
            break;
        }
    }
    ZASSERT(found_caller);
}

ZEST_CASE(MethodCallerCallee) {
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
    ZASSERT(!method_occs.empty());
    auto method_hash = method_occs.front().target;

    auto callee_occs = select("callee_def");
    ZASSERT(!callee_occs.empty());
    auto callee_hash = callee_occs.front().target;

    auto method_it = index.relations.find(method_hash);
    ZASSERT(method_it != index.relations.end());

    bool found_callee = false;
    for(auto& r: method_it->second) {
        if(r.kind == RelationKind::Callee && r.target_symbol == callee_hash) {
            found_callee = true;
            break;
        }
    }
    ZASSERT(found_callee);

    auto callee_it = index.relations.find(callee_hash);
    ZASSERT(callee_it != index.relations.end());

    bool found_caller = false;
    for(auto& r: callee_it->second) {
        if(r.kind == RelationKind::Caller && r.target_symbol == method_hash) {
            found_caller = true;
            break;
        }
    }
    ZASSERT(found_caller);
}

ZEST_CASE(UsingRelationKey) {
    build_index(R"(
            namespace ns { void §(target)foo(); }
            using ns::§(use)⟦§(use)foo⟧;
        )");

    auto& index = tu_index.main_file_index;

    // The relation row of a using declaration is keyed by the same symbol
    // its occurrence references, so looking the occurrence's symbol up in
    // the relations always finds the using site.
    auto use_occs = select("use");
    ZASSERT(!use_occs.empty());
    auto hash = use_occs.front().target;

    auto it = index.relations.find(hash);
    ZASSERT(it != index.relations.end());

    bool found_use = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::WeakReference && r.range == range("use")) {
            found_use = true;
            break;
        }
    }
    ZASSERT(found_use);
}

ZEST_CASE(CtorInitMemberRef) {
    build_index(R"(
            struct S {
                int §(def)⟦x⟧;
                S() : §(use)x(1) {}
            };
        )");

    GO_TO_DEFINITION("use", "def");
}

ZEST_CASE(DesignatedInitRef) {
    build_index(R"(
            struct Point {
                int §(def)⟦x⟧;
                int y;
            };

            Point p = {.§(use)x = 1};
        )");

    GO_TO_DEFINITION("use", "def");
}

ZEST_CASE(RewrittenOperatorRef) {
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
                auto §(def)⟦operator<=>⟧(const S&) const = default;
            };

            bool lt(S a, S b) { return a §(use)< b; }
        )");

    /// a < b is rewritten to (a <=> b) < 0; the operator token references
    /// the rewritten-to operator.
    GO_TO_DEFINITION("use", "def");
}

ZEST_CASE(DependentWeakReference) {
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
    ZASSERT(!use_occs.empty());
    auto target_occs = select("target");
    ZASSERT(!target_occs.empty());
    ZASSERT(use_occs.front().target == target_occs.front().target);

    auto it = index.relations.find(use_occs.front().target);
    ZASSERT(it != index.relations.end());

    bool found_weak = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::WeakReference && r.range == range("use")) {
            found_weak = true;
            break;
        }
    }
    ZASSERT(found_weak);
}

ZEST_CASE(TypeDefinitionRelations) {
    build_index(R"(
            struct §(s)⟦§(s)S⟧ {};

            struct Holder {
                S §(field)⟦§(field)field⟧;
            };

            using §(alias)⟦§(alias)Alias⟧ = S;

            enum §(e)⟦§(e)E⟧ { §(ec)⟦§(ec)A⟧ };
        )");

    ZASSERT(has_pair("field", RelationKind::TypeDefinition, "s"));
    ZASSERT(has_pair("alias", RelationKind::TypeDefinition, "s"));
    ZASSERT(has_pair("ec", RelationKind::TypeDefinition, "e"));
}

ZEST_CASE(ConstructorDestructorRelations) {
    build_index(R"(
            struct §(s)⟦§(s)S⟧ {
                §(ctor)S();
                ~S();
            };
        )");

    auto& index = tu_index.main_file_index;
    auto class_occs = select("s");
    auto ctor_occs = select("ctor");
    ZASSERT(!class_occs.empty());
    ZASSERT(!ctor_occs.empty());
    auto class_hash = class_occs.front().target;
    auto ctor_hash = ctor_occs.front().target;

    auto it = index.relations.find(class_hash);
    ZASSERT(it != index.relations.end());

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
    ZASSERT(found_ctor);
    ZASSERT(found_dtor);

    // The constructor points back at its class for go-to-type-definition.
    auto ctor_it = index.relations.find(ctor_hash);
    ZASSERT(ctor_it != index.relations.end());

    bool found_type = false;
    for(auto& r: ctor_it->second) {
        if(r.kind == RelationKind::TypeDefinition && r.target_symbol == class_hash) {
            found_type = true;
        }
    }
    ZASSERT(found_type);
}

ZEST_CASE(MacroRelations) {
    build_index(R"(
            #define §(def)⟦§(def)FOO⟧ 1
            int x = §(use)⟦§(use)FOO⟧;
        )");

    auto& index = tu_index.main_file_index;
    auto def_occs = select("def");
    auto use_occs = select("use");
    ZASSERT(!def_occs.empty());
    ZASSERT(!use_occs.empty());
    ZASSERT(def_occs.front().target == use_occs.front().target);

    auto it = index.relations.find(def_occs.front().target);
    ZASSERT(it != index.relations.end());

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
    ZASSERT(found_definition);
    ZASSERT(found_reference);
}

ZEST_CASE(MacroDefinedOperand) {
    build_index(R"(
            #define §(def)FOO 1
            #if defined(§(paren)⟦§(paren)FOO⟧) && defined §(bare)⟦§(bare)FOO⟧
            #endif
        )");

    auto target = select("def").front().target;
    auto& relations = tu_index.main_file_index.relations[target];
    for(auto name: {"paren", "bare"}) {
        auto occurrences = select(name);
        ZASSERT(occurrences.size() == 1U);
        ZASSERT(occurrences.front().target == target);
        ZASSERT(std::ranges::any_of(relations, [&](const index::Relation& relation) {
            return relation.kind == RelationKind::Reference && relation.range == range(name);
        }));
    }
}

ZEST_CASE(DependentOperatorUnreferenced) {
    build_index(R"(
        struct Id {};
        bool operator==(const Id&, const Id&);
        int operator<=>(const Id&, const Id&);

        template <typename T>
        struct Box {
            T* ptr;
            bool empty() const { return ptr == nullptr; }
            bool same(const Box& other) const { return this != &other; }
            bool ordered(T a, T b) const { return a >= b; }
        };
    )");

    for(auto name: {"operator==", "operator<=>"}) {
        auto& relations = tu_index.main_file_index.relations[symbol_named(name).first];
        ZASSERT(relations.size() == 1U);
        ZASSERT(relations.front().kind == RelationKind::Declaration);
    }
}

ZEST_CASE(PastedFragmentUses) {
    add_file("kinds.inc", "KIND(red)\nKIND(green)\n");
    add_file("decls.inc", "int helper();\nint value = helper();\n");
    add_file("take.h", "constexpr int fallback = 1;\nint take(int x = fallback);\n");
    add_main("main.cpp", R"(
        #include "decls.inc"
        #include "take.h"
        int take(int x) { return x; }
        enum Color { red, green };
        int pick(Color color) {
            switch(color) {
        #define KIND(name) case name: return helper();
        #include §(paste)⟦"kinds.inc"⟧
        #undef KIND
            }
            return 0;
        }
    )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));

    auto pasted_at = [&](llvm::StringRef name) {
        std::vector<LocalSourceRange> ranges;
        for(auto& relation: tu_index.main_file_index.relations[symbol_named(name).first]) {
            if(relation.kind == RelationKind::Pasted) {
                ranges.push_back(relation.range);
            }
        }
        return ranges;
    };
    for(auto name: {"red", "green", "helper"}) {
        auto ranges = pasted_at(name);
        ZASSERT(ranges.size() == 1U);
        ZASSERT(ranges.front() == range("paste"));
    }
    ZASSERT(pasted_at("Color").empty());
    ZASSERT(pasted_at("fallback").empty());
}

ZEST_CASE(ModuleName) {
    build_index(R"(export module §(m)⟦§(m)foo⟧;)");

    auto& index = tu_index.main_file_index;
    auto occs = select("m");
    ZASSERT(!occs.empty());
    ZASSERT(occs.front().target == unit->module_entity("foo"));
    ZASSERT(symbol_named("foo").second.kind.value() == SymbolKind(SymbolKind::Module).value());

    auto it = index.relations.find(occs.front().target);
    ZASSERT(it != index.relations.end());

    bool found_definition = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            found_definition = true;
        }
    }
    ZASSERT(found_definition);
}

ZEST_CASE(ModulePartitionName) {
    build_index(R"(export module §(m)⟦§(m)foo:part⟧;)");

    // The occurrence spans the whole written name, partition included.
    auto occs = select("m");
    ZASSERT(!occs.empty());
    ZASSERT(occs.front().range == range("m"));

    auto& index = tu_index.main_file_index;
    auto it = index.relations.find(occs.front().target);
    ZASSERT(it != index.relations.end());

    bool found_definition = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            found_definition = true;
        }
    }
    ZASSERT(found_definition);
}

ZEST_CASE(ImplementationUnitReference) {
    add_files("main.cpp", R"(
#[foo.cppm]
export module foo;
export int x = 1;

#[main.cpp]
module §(m)⟦§(m)foo⟧;
)");
    ZASSERT(compile_with_modules());
    decode_index(index::build_tu_index(*unit));

    // An implementation unit's declaration is a Reference, not a Definition.
    auto occs = select("m");
    ZASSERT(!occs.empty());
    ZASSERT(occs.front().range == range("m"));

    auto& index = tu_index.main_file_index;
    auto it = index.relations.find(occs.front().target);
    ZASSERT(it != index.relations.end());

    bool found_reference = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            ZASSERT(false);
        }
        if(r.kind == RelationKind::Reference) {
            found_reference = true;
        }
    }
    ZASSERT(found_reference);
}

ZEST_CASE(OverrideRelation) {
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

    auto check_relations = [&](DecodedRows& idx) {
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
    for(auto& [path_id, idx]: tu_index.file_indices) {
        check_relations(idx);
    }

    ZASSERT(found_interface);
    ZASSERT(found_implementation);
}

ZEST_CASE(DeclarationAndDefinition) {
    build_index(R"(
            int §(decl)foo();

            int §(def)⟦§(def)foo⟧() { return 42; }
        )");

    auto& index = tu_index.main_file_index;

    // Find the declaration occurrence and verify Declaration relation exists.
    auto decl_occs = select("decl");
    ZASSERT(!decl_occs.empty());
    auto symbol_hash = decl_occs.front().target;

    auto it = index.relations.find(symbol_hash);
    ZASSERT(it != index.relations.end());

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
    ZASSERT(found_decl);
    ZASSERT(found_def);
}

ZEST_CASE(MacroDefinitionExtent) {
    build_index(R"(
        #define MAKE_FN(name) int name() { return 42; }
        §(ext)⟦MAKE_FN(generated)⟧
    )");

    auto [hash, symbol] = symbol_named("generated");
    auto& relations = tu_index.main_file_index.relations[hash];
    auto definition = std::ranges::find_if(relations, [](const index::Relation& relation) {
        return relation.kind == RelationKind::Definition;
    });
    ZASSERT(definition != relations.end());
    ZASSERT(dump(definition->definition_range()) == dump(range("ext")));
}

ZEST_CASE(SpelledInMacroRedeclarations) {
    build_index(R"(
        #define FWD(name) class name;
        FWD(Written)
        class Written {};

        #define MAKE(name) class name {};
        MAKE(Generated)
    )");

    ZASSERT(!has(symbol_named("Written").second, index::SymbolFlags::SpelledInMacro));
    ZASSERT(has(symbol_named("Generated").second, index::SymbolFlags::SpelledInMacro));
}

ZEST_CASE(CrossFileHeaderIndex) {
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
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));

    // The header should have its own FileIndex (separate from main).
    ZASSERT(tu_index.file_indices.size() >= 1U);

    // The main file should have a reference to helper.
    auto& main_index = tu_index.main_file_index;
    ZASSERT(!main_index.occurrences.empty());

    // Find 'helper' reference in main file.
    auto use_offset = point("use_helper");
    auto it = std::ranges::lower_bound(main_index.occurrences,
                                       use_offset,
                                       {},
                                       [](const index::Occurrence& o) { return o.range.end; });
    ZASSERT(it != main_index.occurrences.end());
    ZASSERT(it->range.contains(use_offset));

    // The helper symbol should exist in the TU symbol table.
    auto helper_hash = it->target;
    ZASSERT(tu_index.symbols.contains(helper_hash));

    // The helper's declaration should be in the header's rows.
    bool found_in_header = false;
    for(auto& [path_id, file_index]: tu_index.file_indices) {
        for(auto& [sym, rels]: file_index.relations) {
            if(sym == helper_hash) {
                found_in_header = true;
                break;
            }
        }
        if(found_in_header)
            break;
    }
    ZASSERT(found_in_header);
}

ZEST_CASE(SymbolKinds) {
    build_index(R"(
            struct §(cls)MyClass {};
            enum §(enm)MyEnum { A, B };
            void §(func)myFunc() {}
            int §(var)myVar = 0;
            namespace §(ns)MyNS {}
        )");

    auto check_kind = [&](llvm::StringRef name, SymbolKind expected) {
        auto occs = select(name);
        ZASSERT(!occs.empty());
        auto hash = occs.front().target;
        ZASSERT(tu_index.symbols.contains(hash));
        ZASSERT(tu_index.symbols[hash].kind.value() == expected.value());
    };

    check_kind("cls", SymbolKind::Struct);
    check_kind("enm", SymbolKind::Enum);
    check_kind("func", SymbolKind::Function);
    check_kind("var", SymbolKind::Variable);
    check_kind("ns", SymbolKind::Namespace);
}

ZEST_CASE(LookupOccurrence) {
    build_index(R"(
        int §(x)⟦fo§(x)o⟧();
        int §(ref)⟦fo§(ref)o⟧() { return 0; }
    )");

    auto& fi = tu_index.main_file_index;
    ZASSERT(!fi.occurrences.empty());
    const index::Shard& shard = tu_index.view.shard_of(tu_index.view.path_count() - 1);
    ZASSERT(shard.loaded());

    auto x_range = range("x");
    std::optional<index::Occurrence> found;
    shard.lookup(point("x"), [&](const index::Occurrence& occ) {
        found = occ;
        return true;
    });
    ZASSERT(found);
    ZEXPECT(found->range.begin == x_range.begin);
    ZEXPECT(found->range.end == x_range.end);

    found.reset();
    shard.lookup(point("ref"), [&](const index::Occurrence& occ) {
        found = occ;
        return true;
    });
    ZASSERT(found);
    ZEXPECT(found->target == fi.occurrences.front().target);

    found.reset();
    shard.lookup(0, [&](const index::Occurrence& occ) {
        found = occ;
        return false;
    });
    ZEXPECT(!found.has_value());
}

ZEST_CASE(LookupRelation) {
    build_index(R"(
        void §(decl)⟦fo§(decl)o⟧();
        void §(def)⟦fo§(def)o⟧() {}
    )");

    const index::Shard& shard = tu_index.view.shard_of(tu_index.view.path_count() - 1);
    ZASSERT(shard.loaded());

    std::optional<index::Occurrence> occ;
    shard.lookup(point("decl"), [&](const index::Occurrence& o) {
        occ = o;
        return false;
    });
    ZASSERT(occ);

    auto def_range = range("def");
    bool found_def = false;
    shard.lookup(occ->target, RelationKind::Definition, [&](const index::Relation& r) {
        found_def = true;
        ZEXPECT(r.range.begin == def_range.begin);
        ZEXPECT(r.range.end == def_range.end);
        return false;
    });
    ZEXPECT(found_def);

    bool found_any = false;
    shard.lookup(occ->target, RelationKind::Caller, [&](const index::Relation&) {
        found_any = true;
        return false;
    });
    ZEXPECT(!found_any);
}

ZEST_CASE(ScopeExternal) {
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
            ZASSERT(static_cast<int>(symbol.scope) ==
                    static_cast<int>(index::SymbolScope::External));
            found.insert(symbol.name);
        }
    }
    ZASSERT(found == expected);
}

ZEST_CASE(ScopeFileLocal) {
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
            ZASSERT(static_cast<int>(symbol.scope) ==
                    static_cast<int>(index::SymbolScope::FileLocal));
            found.insert(symbol.name);
        }
    }
    ZASSERT(found == expected);
}

ZEST_CASE(ScopeTULocal) {
    build_index(R"(
            static int static_var = 0;
            static void static_func() {}
            namespace { int anon_var = 1; }
        )");

    std::set<std::string> expected{"static_var", "static_func", "anon_var"};
    std::set<std::string> found;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(expected.contains(symbol.name)) {
            ZASSERT(static_cast<int>(symbol.scope) ==
                    static_cast<int>(index::SymbolScope::TULocal));
            found.insert(symbol.name);
        }
    }
    ZASSERT(found == expected);
}

ZEST_CASE(ScopeModuleLinkage) {
    build_index(R"(
            export module m;
            int module_var = 0;
            export int exported_var = 0;
            static int static_var = 0;
        )");

    ZASSERT(scope("module_var") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("exported_var") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("static_var") == static_cast<int>(index::SymbolScope::TULocal));
}

ZEST_CASE(ExportedFlag) {
    build_index(R"(
            export module m;
            export int exported_var = 0;
            export {
                int block_fn();
                struct Exported { void member(); };
            }
            export namespace api { int nested(); }
            export template <typename Param> concept Small = sizeof(Param) < 4;
            namespace detail { export int open(); }
            namespace hidden { int closed(); }
            int module_var = 0;
            int block_fn() { return 0; }
        )");

    auto exported = [&](llvm::StringRef name) {
        return has(symbol_named(name).second, index::SymbolFlags::Exported);
    };
    ZASSERT(exported("exported_var"));
    ZASSERT(exported("block_fn"));
    ZASSERT(exported("Exported"));
    ZASSERT(exported("nested"));
    ZASSERT(exported("Small"));
    ZASSERT(exported("detail"));
    ZASSERT(!exported("hidden"));
    ZASSERT(!exported("Param"));
    ZASSERT(!exported("member"));
    ZASSERT(!exported("module_var"));
}

ZEST_CASE(AnonymousScopeFlag) {
    build_index(R"(
            enum { unnamed_value };
            struct Holder { union { int union_member; }; };
            struct { int declarator_member; } unnamed_object;
        )");

    auto anonymous = [&](llvm::StringRef member) {
        auto parent = symbol_named(member).second.parent;
        return has(tu_index.symbols[parent], index::SymbolFlags::AnonymousScope);
    };
    ZASSERT(anonymous("unnamed_value"));
    ZASSERT(anonymous("union_member"));
    ZASSERT(!anonymous("declarator_member"));
}

ZEST_CASE(ScopeNoLinkage) {
    build_index(R"(
            enum { unnamed_value };
            struct { int unnamed_field; } unnamed_object;
            struct Holder { enum { member_value }; };
            namespace { enum { hidden_value }; using hidden_alias = int; }
            typedef void (*callback)(int prototype_param);
            void host() { enum { local_value }; }
        )");

    ZASSERT(scope("unnamed_value") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("unnamed_field") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("member_value") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("callback") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("hidden_value") == static_cast<int>(index::SymbolScope::TULocal));
    ZASSERT(scope("hidden_alias") == static_cast<int>(index::SymbolScope::TULocal));
    ZASSERT(scope("prototype_param") == static_cast<int>(index::SymbolScope::FileLocal));
    ZASSERT(scope("local_value") == static_cast<int>(index::SymbolScope::FileLocal));
}

ZEST_CASE(ScopeCEnumerator) {
    add_main("main.c", R"c(
            enum color { red };
            void host(void) { enum { local_value }; }
        )c");
    ZASSERT(compile("-std=c17"));
    decode_index(index::build_tu_index(*unit));

    ZASSERT(scope("red") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("local_value") == static_cast<int>(index::SymbolScope::FileLocal));
}

ZEST_CASE(BareNameAndParentChain) {
    build_index(R"(
        namespace ns { struct Outer { struct Inner { void method(); }; }; }
        void ns::Outer::Inner::method() {}
        namespace ns { inline namespace v1 { enum Color { Red }; } }
        void local_host() { struct Local { int field; }; }
    )");

    auto [ns, ns_symbol] = symbol_named("ns");
    auto [outer, outer_symbol] = symbol_named("Outer");
    auto [inner, inner_symbol] = symbol_named("Inner");
    auto [method, method_symbol] = symbol_named("method");
    ZASSERT(ns_symbol.parent == 0u);
    ZASSERT(outer_symbol.parent == ns);
    ZASSERT(inner_symbol.parent == outer);
    ZASSERT(method_symbol.parent == inner);

    auto [v1, v1_symbol] = symbol_named("v1");
    ZASSERT(has(v1_symbol, index::SymbolFlags::InlineNamespace));
    ZASSERT(v1_symbol.parent == ns);
    auto [color, color_symbol] = symbol_named("Color");
    ZASSERT(color_symbol.parent == v1);
    ZASSERT(symbol_named("Red").second.parent == color);

    auto [host, host_symbol] = symbol_named("local_host");
    auto [local, local_symbol] = symbol_named("Local");
    ZASSERT(local_symbol.parent == host);
    ZASSERT(symbol_named("field").second.parent == local);
}

ZEST_CASE(SpecializationArguments) {
    build_index(R"(
        template <typename T> struct Box { T value; };
        template <> struct Box<int> { int value; };
        template <typename T> struct Box<T*> { T* value; };
        template <typename T> T identity(T t) { return t; }
        template <> int identity<int>(int t) { return t; }
        Box<int> a;
        Box<double> b;
        Box<char*> c;
    )");

    auto [primary, primary_symbol] = symbol_named("Box");
    ZASSERT(has(primary_symbol, index::SymbolFlags::Template));
    ZASSERT(!has(primary_symbol, index::SymbolFlags::Specialization));
    ZASSERT(has(primary_symbol, index::SymbolFlags::Completable));

    auto [full, full_symbol] = symbol_named("Box", "<int>");
    ZASSERT(has(full_symbol, index::SymbolFlags::Specialization));
    ZASSERT(!has(full_symbol, index::SymbolFlags::Template));
    ZASSERT(!has(full_symbol, index::SymbolFlags::Completable));

    auto [partial, partial_symbol] = symbol_named("Box", "<T *>");
    ZASSERT(has(partial_symbol, index::SymbolFlags::Specialization));
    ZASSERT(has(partial_symbol, index::SymbolFlags::Template));

    // Members of the full specialization hang off it; the primary's
    // members and every instantiation's off the primary.
    std::set<index::SymbolHash> value_parents;
    for(auto& [hash, symbol]: tu_index.symbols) {
        if(symbol.name == "value") {
            value_parents.insert(symbol.parent);
        }
    }
    ZASSERT(value_parents == (std::set<index::SymbolHash>{primary, full, partial}));

    auto [function, function_symbol] = symbol_named("identity", "<int>");
    ZASSERT(has(function_symbol, index::SymbolFlags::Specialization));
    ZASSERT(has(symbol_named("identity").second, index::SymbolFlags::Template));
}

ZEST_CASE(SymbolFacts) {
    build_index(R"(
        [[deprecated]] void old();
        struct { int in_anonymous; } anonymous_instance;
        #define DECLARE(name) void name();
        DECLARE(spelled)
        enum Unscoped { Plain };
        struct Holder {
            enum Nested { Inner };
            enum class Scoped { Hidden };
            Holder();
            ~Holder();
            operator int();
            void member();
        };
        bool operator==(Holder, Holder);
        namespace n { struct Scoped { Scoped(); ~Scoped(); }; }
    )");

    ZASSERT(has(symbol_named("old").second, index::SymbolFlags::Deprecated));
    auto holder = symbol_named("Holder", "", SymbolKind::Struct).second;
    ZASSERT(!has(holder, index::SymbolFlags::Deprecated));

    auto [anonymous, anonymous_symbol] = symbol_named("(anonymous struct)");
    ZASSERT(has(anonymous_symbol, index::SymbolFlags::Unnamed));
    ZASSERT(symbol_named("in_anonymous").second.parent == anonymous);

    ZASSERT(has(symbol_named("spelled").second, index::SymbolFlags::SpelledInMacro));
    ZASSERT(!has(symbol_named("old").second, index::SymbolFlags::SpelledInMacro));
    ZASSERT(!has(symbol_named("old").second, index::SymbolFlags::SystemHeader));

    ZASSERT(has(symbol_named("old").second, index::SymbolFlags::Completable));
    ZASSERT(has(symbol_named("Plain").second, index::SymbolFlags::Completable));
    ZASSERT(has(symbol_named("Inner").second, index::SymbolFlags::Completable));
    ZASSERT(!has(symbol_named("Hidden").second, index::SymbolFlags::Completable));
    ZASSERT(!has(symbol_named("member").second, index::SymbolFlags::Completable));
    ZASSERT(has(symbol_named("DECLARE").second, index::SymbolFlags::Completable));

    using index::NameForm;
    ZASSERT(index::name_form(symbol_named("member").second.flags) == NameForm::Identifier);
    ZASSERT(index::name_form(holder.flags) == NameForm::Identifier);
    ZASSERT(index::name_form(symbol_named("~Holder").second.flags) == NameForm::Destructor);
    ZASSERT(index::name_form(symbol_named("operator int").second.flags) == NameForm::Conversion);
    ZASSERT(index::name_form(symbol_named("operator==").second.flags) == NameForm::Operator);
    auto [holder_class, holder_symbol] = symbol_named("Holder", "", SymbolKind::Struct);
    auto constructor = symbol_named("Holder", "", SymbolKind::Method).second;
    ZASSERT(index::name_form(constructor.flags) == NameForm::Constructor);
    ZASSERT(constructor.parent == holder_class);

    // A scoped class's constructor and destructor spell the class's own
    // name, never its qualifier.
    auto [scoped, scoped_symbol] = symbol_named("Scoped", "", SymbolKind::Struct);
    ZASSERT(symbol_named("Scoped", "", SymbolKind::Method).second.parent == scoped);
    ZASSERT(symbol_named("~Scoped").second.parent == scoped);
}

ZEST_CASE(CanonicalFile) {
    add_file("header.h", R"(
        int declared_twice();
        int header_only();
        inline int header_defined() { return 1; }
    )");
    add_main("main.cpp", R"(
        #include "header.h"
        int declared_twice();
        int declared_twice() { return header_only() + header_defined(); }
        int main_only();
    )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));

    auto main_path = tu_index.view.path_count() - 1;
    auto header_path = [&] {
        for(std::uint32_t id = 0; id < tu_index.view.path_count(); id += 1) {
            if(tu_index.view.path(id).ends_with("header.h")) {
                return id;
            }
        }
        LOG_FATAL("header.h is not in the path table");
    }();

    auto twice = symbol_named("declared_twice").second;
    ZASSERT(has(twice, index::SymbolFlags::HasDefinition));
    ZASSERT(twice.file == main_path);

    auto only = symbol_named("header_only").second;
    ZASSERT(!has(only, index::SymbolFlags::HasDefinition));
    ZASSERT(only.file == header_path);

    auto defined = symbol_named("header_defined").second;
    ZASSERT(has(defined, index::SymbolFlags::HasDefinition));
    ZASSERT(defined.file == header_path);
    ZASSERT(has(defined, index::SymbolFlags::Completable));

    // A main-file-only build keeps no header rows: a symbol the main file
    // merely uses has no declaring row at all.
    decode_index(index::build_tu_index(*unit, true));
    auto used = symbol_named("header_only").second;
    ZASSERT(!has(used, index::SymbolFlags::HasDefinition));
    ZASSERT(used.file == index::no_file);
    ZASSERT(symbol_named("main_only").second.file == tu_index.view.path_count() - 1);
}

ZEST_CASE(PreambleDefaultArgument) {
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
    ZASSERT(compile_with_pch());

    // A full build keeps the preamble rows: this proves the row exists
    // (so the gate test below cannot pass vacuously) and that the loaded
    // fid resolves through the graph to the header's path.
    decode_index(index::build_tu_index(*unit));
    bool found = false;
    for(auto& [path_id, index]: tu_index.file_indices) {
        found |= tu_index.view.path(path_id).ends_with("foo.h");
    }
    ZASSERT(found);

    decode_index(index::build_tu_index(*unit, true));

    // Rows resolving into the preamble are dropped: the preamble's own
    // index covers them. Only the main file's rows remain.
    ZASSERT(tu_index.file_indices.empty());
    EXPECT_SELECT("1", "def");
}

ZEST_CASE(PreambleBaseSpecifier) {
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
    ZASSERT(compile_with_pch());

    // Full build: the base-specifier rows land in the preamble header
    // and its loaded fid resolves to the header's path.
    decode_index(index::build_tu_index(*unit));
    bool found = false;
    for(auto& [path_id, index]: tu_index.file_indices) {
        found |= tu_index.view.path(path_id).ends_with("bar.h");
    }
    ZASSERT(found);

    decode_index(index::build_tu_index(*unit, true));

    ZASSERT(tu_index.file_indices.empty());
    ZASSERT(!tu_index.main_file_index.occurrences.empty());
}

ZEST_CASE(HeaderMacroDropped) {
    // Macro occurrences flow through the same gate: a definition in an
    // included header is dropped from an main-file-only build, while
    // the reference in the main file is kept.
    add_file("baz.h", R"(
#define BAZ 1
)");
    add_main("main.cpp", R"(
#include "baz.h"
int x = §(1)BAZ;
)");
    ZASSERT(compile());

    decode_index(index::build_tu_index(*unit, true));
    ZASSERT(tu_index.file_indices.empty());
    ZASSERT(!tu_index.main_file_index.occurrences.empty());
}

ZEST_CASE(UnknownFidFallback) {
    // A preamble header's loaded FileID is unknown to a graph built
    // without indexed fids; lookups must degrade to the main file
    // instead of crashing.
    add_file("foo.h", R"(
struct Foo {};
)");
    add_main("main.cpp", R"(
#include "foo.h"
int x = 1;
)");
    ZASSERT(compile_with_pch());

    auto tree = index::IncludeTree::from(*unit);
    auto fid = unit->file_id(TestVFS::path("foo.h"));
    ZASSERT(fid.isValid());
    ZASSERT(tree.node_of(fid) == static_cast<std::uint32_t>(-1));
    ZASSERT(tree.path_id(fid) == static_cast<std::uint32_t>(tree.paths.size() - 1));
}

ZEST_CASE(PreambleFidResolved) {
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
    ZASSERT(compile_with_pch());

    auto fid = unit->file_id(TestVFS::path("foo.h"));
    ZASSERT(fid.isValid());

    auto tree = index::IncludeTree::from(*unit, {fid});
    auto node = tree.node_of(fid);
    ZASSERT(node != static_cast<std::uint32_t>(-1));
    ZASSERT(tree.paths[tree.path_id(fid)].ends_with("foo.h"));
}

ZEST_CASE(DeepExpressionChain) {
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
    ZASSERT(compiled);

    // Index on a deliberately tight stack: the traversal must use
    // constant stack space however deep the expression is, so any
    // reintroduced per-node recursion crashes here deterministically
    // instead of only on production workers with deeper files.
    feature::InactiveScan scan;
    llvm::thread index_thread(std::optional<unsigned>(clang::DesiredStackSize / 4), [&] {
        // Mirror the stateful worker's post-compile sequence.
        scan = feature::inactive_regions(*unit);
        decode_index(index::build_tu_index(*unit, true));

        // The semantic map must also serve token classification and a
        // selection at the giant expansion's invocation on this stack:
        // per-token ancestor walks and recursive tree materialization
        // both used to degrade on chains this deep.
        feature::semantic_tokens(*unit);
        auto use_offset = static_cast<std::uint32_t>(code.rfind("A14"));
        SelectionTree::create_right(*unit, LocalSourceRange(use_offset, use_offset));
    });
    index_thread.join();

    ZASSERT(scan.regions.empty());

    // The traversal must have actually reached the decl behind the chain,
    // not bailed out early: expect an occurrence exactly at `bomb`.
    auto& occurrences = tu_index.main_file_index.occurrences;
    auto bomb = std::ranges::find(occurrences, bomb_offset, [](index::Occurrence& occurrence) {
        return occurrence.range.begin;
    });
    ZASSERT(bomb != occurrences.end());
}

ZEST_CASE(SuperQualifierRef) {
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
    ZASSERT(try_compile());
    decode_index(index::build_tu_index(*unit));

    GO_TO_DEFINITION("use", "def");
}

ZEST_CASE(EnvelopeSections) {
    add_file("header.h", R"(
            #pragma once
            inline int §(hdr)helper() { return 1; }
        )");
    add_main("main.cpp", R"(
            #include "header.h"
            int main() { return §(use)helper(); }
        )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));
    ZASSERT(!tu_index.file_indices.empty());

    auto& view = tu_index.view;
    ZASSERT(view.built_at() > 0);

    // Sections ascend by path id and the main file's rows are the
    // last path id's section.
    for(std::uint32_t i = 1; i < view.section_count(); i += 1) {
        ZASSERT(view.section_path(i - 1) < view.section_path(i));
    }
    auto main_section = view.section_of(view.path_count() - 1);
    ZASSERT(main_section);

    // Every section's hash is the byte identity of its blob, the blob
    // loads as a self-contained single-variant shard under that identity,
    // and the whole envelope passes the persisted-load gate.
    ZASSERT(view.shards_verify());
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto blob = view.section_blob(i);
        ZASSERT(view.section_hash(i) == llvm::xxh3_64bits(blob));
        auto shard = index::Shard::from_bytes(blob);
        ZASSERT(shard.loaded());
        ZASSERT(shard.variants().size() == std::size_t(1));
        ZASSERT(shard.variants().front() == view.section_hash(i));
    }

    // The graph travels with the envelope: every path resolves and the
    // consumed-content hash column covers the whole table.
    for(std::uint32_t id = 0; id < view.path_count(); id += 1) {
        ZASSERT(!view.path(id).empty());
        ZASSERT(view.path_hash(id) != 0);
    }

    // Symbols read back identically through iteration and point lookup.
    ZASSERT(!tu_index.symbols.empty());
    for(auto& [hash, symbol]: tu_index.symbols) {
        auto identity = view.find_symbol(hash);
        ZASSERT(identity);
        ZASSERT(identity->name == llvm::StringRef(symbol.name));
        ZASSERT(identity->kind.value() == symbol.kind.value());
        ZASSERT(static_cast<int>(identity->scope) == static_cast<int>(symbol.scope));
    }
}

ZEST_CASE(CorruptSectionBytesRejected) {
    // The persisted-load gate must reject any flipped section byte, in
    // particular flips that still form a structurally valid shard (an
    // opaque hash field, say) — only the byte hash catches those, and
    // serving them would navigate by corrupted rows across restarts.
    add_main("main.cpp", R"(
            int value = 1;
            int main() { return value; }
        )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));

    auto& view = tu_index.view;
    ZASSERT(view.section_count() > 0);
    auto blob = view.section_blob(0);
    auto offset = static_cast<std::size_t>(blob.data() - view.bytes().data());

    bool structurally_valid_flip = false;
    std::string bytes = view.bytes().str();
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        std::string mutated = bytes;
        mutated[offset + i] ^= 0x01;
        auto reloaded = index::TUIndex::from_bytes(mutated);
        if(!reloaded.loaded()) {
            continue;
        }
        if(index::Shard::from_bytes(reloaded.section_blob(0)).loaded()) {
            structurally_valid_flip = true;
        }
        ZASSERT(!reloaded.shards_verify());
    }
    ZASSERT(structurally_valid_flip);
}

ZEST_CASE(FromRejectsHostileInput) {
    ZASSERT(!index::TUIndex::from_bytes("not a flatbuffer at all").loaded());

    build_index(R"(
            int foo() { return 42; }
        )");
    auto bytes = tu_index.view.bytes();

    // Sanity: the intact envelope loads, so the rejections below are earned.
    ZASSERT(index::TUIndex::from_bytes(bytes).loaded());

    ZASSERT(!index::TUIndex::from_bytes(bytes.substr(0, bytes.size() / 2)).loaded());

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    ZASSERT(bytes.size() > 8);
    std::string clobbered(bytes.data(), bytes.size());
    for(std::size_t i = 4; i < 8; i += 1) {
        clobbered[i] = 'X';
    }
    ZASSERT(!index::TUIndex::from_bytes(clobbered).loaded());
}

ZEST_CASE(FromRejectsStaleFormatVersion) {
    // Only the version slot and the path table are written: every other
    // field reads back absent, which is structurally valid — the verdict
    // must hinge on the version value. Field order MUST mirror the
    // envelope layout (tu_index.cpp): format_version is slot 0.
    struct VersionAndPaths {
        std::uint32_t format_version = 0;
        std::int64_t built_at = 0;
        std::vector<std::string> paths = {"/proj/main.cpp"};
    };

    auto bytes_of = [](const std::vector<std::uint8_t>& blob) {
        return llvm::StringRef(reinterpret_cast<const char*>(blob.data()), blob.size());
    };

    auto stale = kota::codec::fbs::to_bytes(
        VersionAndPaths{.format_version = index::index_format_version + 1});
    ZASSERT(stale);
    ZASSERT(!index::TUIndex::from_bytes(bytes_of(*stale)).loaded());

    // Positive control: the same shape carrying the current version loads,
    // so the rejection above comes from the value, not the blob's shape.
    auto current =
        kota::codec::fbs::to_bytes(VersionAndPaths{.format_version = index::index_format_version});
    ZASSERT(current);
    ZASSERT(index::TUIndex::from_bytes(bytes_of(*current)).loaded());
}

/// Hand-built envelopes for hostile-input tests. Field order MUST mirror
/// the envelope layout (tu_index.cpp).
struct MirrorSection {
    std::uint32_t path_id = 0;
    std::uint64_t hash = 0;
    std::vector<std::uint8_t> blob;
};

struct MirrorEnvelope {
    std::uint32_t format_version = index::index_format_version;
    std::int64_t built_at = 0;
    std::vector<std::string> paths;
    std::vector<std::uint64_t> path_hashes;
    std::vector<index::IncludeNode> nodes;
    index::SymbolTable symbols{};
    std::vector<MirrorSection> sections;
};

std::string mirror_bytes(const MirrorEnvelope& envelope) {
    auto bytes = kota::codec::fbs::to_bytes(envelope);
    if(!bytes) {
        return {};
    }
    return std::string(bytes->begin(), bytes->end());
}

ZEST_CASE(FromRejectsReservedParents) {
    // Symbol parents become DenseSet keys in a query's container walk; the
    // sentinel values must fail the envelope as a whole.
    MirrorEnvelope honest;
    honest.paths = {"/proj/main.cpp"};
    honest.symbols[42].name = "sym";
    honest.symbols[42].parent = 7;
    ZASSERT(index::TUIndex::from_bytes(mirror_bytes(honest)).loaded());

    MirrorEnvelope hostile = honest;
    hostile.symbols[42].parent = ~std::uint64_t(0);
    ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
    hostile.symbols[42].parent = ~std::uint64_t(0) - 1;
    ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsOutOfRangeParents) {
    // A node's parent indexes the node table in every consumer; only
    // another node or the root sentinel is acceptable.
    MirrorEnvelope honest;
    honest.paths = {"/proj/main.cpp", "/proj/a.h"};
    honest.nodes.push_back({.file = 1, .parent = ~0u, .line = 1});
    honest.nodes.push_back({.file = 1, .parent = 0, .line = 2});
    ZASSERT(index::TUIndex::from_bytes(mirror_bytes(honest)).loaded());

    MirrorEnvelope hostile = honest;
    hostile.nodes.back().parent = 2;
    ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsOutOfRangePathIds) {
    // Structural verification does not constrain field values, and the
    // merge pipeline dereferences every decoded path id against the path
    // table without further checks — an envelope pointing outside its own
    // table must be rejected as a whole. Symbol reference-file ids are
    // deliberately not gated here: ProjectIndex::merge bounds them, pinned
    // by project_index_tests.

    // Positive control first: the same shapes with in-range ids load, so
    // the rejections below come from the hostile values.
    MirrorEnvelope honest;
    honest.paths = {"/proj/main.cpp"};
    honest.nodes.push_back({.file = 0, .parent = 0, .line = 1});
    honest.sections.push_back({.path_id = 0});
    ZASSERT(index::TUIndex::from_bytes(mirror_bytes(honest)).loaded());

    {
        MirrorEnvelope hostile;
        hostile.paths = {"/proj/main.cpp"};
        hostile.nodes.push_back({.file = 7, .parent = 0, .line = 1});
        ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
    }
    {
        MirrorEnvelope hostile;
        hostile.paths = {"/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 7});  // Only path id 0 exists.
        ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
    }
}

ZEST_CASE(FromRejectsEmptyPathTable) {
    // The builder ends every path table with the main file, and
    // consumers address path_count() - 1 unchecked — an envelope with no
    // paths at all is corrupt.
    MirrorEnvelope hostile;
    ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
}

ZEST_CASE(FromRejectsUnsortedSections) {
    // section_of binary-searches the section table by path id; a repeated
    // or out-of-order id would attribute one file's rows to another.

    // Positive control: the ascending shape loads.
    MirrorEnvelope honest;
    honest.paths = {"/proj/a.h", "/proj/main.cpp"};
    honest.sections.push_back({.path_id = 0});
    honest.sections.push_back({.path_id = 1});
    ZASSERT(index::TUIndex::from_bytes(mirror_bytes(honest)).loaded());

    {
        MirrorEnvelope hostile;
        hostile.paths = {"/proj/a.h", "/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 1});
        hostile.sections.push_back({.path_id = 0});
        ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
    }
    {
        MirrorEnvelope hostile;
        hostile.paths = {"/proj/a.h", "/proj/main.cpp"};
        hostile.sections.push_back({.path_id = 1});
        hostile.sections.push_back({.path_id = 1});
        ZASSERT(!index::TUIndex::from_bytes(mirror_bytes(hostile)).loaded());
    }
}

ZEST_CASE(AbsentPathHashesReadZero) {
    // The hash column may be shorter than the path table on a foreign
    // envelope (structurally valid: the field reads back empty); absent
    // entries read as 0, "unavailable".
    MirrorEnvelope envelope;
    envelope.paths = {"/proj/a.h", "/proj/main.cpp"};
    envelope.path_hashes = {7};
    auto bytes = mirror_bytes(envelope);
    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());
    ZASSERT(view.path_hash(0) == 7u);
    ZASSERT(view.path_hash(1) == 0u);
}

};  // ZEST_SUITE(tu_index)

}  // namespace
}  // namespace clice::testing
