module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :compile.diagnostic;
import :feature.feature;
import :index.format;
import :index.include_tree;
import :index.indexer;
import :index.instantiation_shard;
import :index.shard;
import :index.tu_index;
import :project.project;
import :semantic.identity;
import :semantic.selection;
import :support.logging;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :unittests.utils.tu_index_blob;
import :unittests.utils;

namespace clice::testing {

namespace lsp = kota::ipc::lsp;

namespace {

ZEST_SUITE(Indexer, Tester) {

/// One file's shard read back out of its TU index section through the
/// Shard reader — every assertion below therefore exercises the full
/// build → encode → read roundtrip, not builder-internal state.
struct DecodedShard {
    std::vector<index::Occurrence> occurrences;
    llvm::DenseMap<index::SymbolHash, std::vector<index::Relation>> relations{};

    bool empty() const {
        return occurrences.empty() && relations.empty();
    }
};

struct DecodedIndex {
    index::TUIndex view;
    DecodedShard focus_file_index;
    /// Non-focus sections keyed by path id.
    std::vector<std::pair<std::uint32_t, DecodedShard>> file_indices;
    index::SymbolTable symbols{};
};

DecodedIndex tu_index;

DecodedShard decode_shard(const index::Shard& shard) {
    DecodedShard decoded;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        decoded.occurrences.push_back(occurrence);
        return true;
    });
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        decoded.relations[hash].push_back(relation);
        return true;
    });
    return decoded;
}

void decode_index(const std::string& bytes) {
    tu_index = {};
    tu_index.view = index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
    auto& view = tu_index.view;
    ZASSERT(view.loaded());

    auto focus_path = view.path_count() - 1;
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto decoded = decode_shard(view.shard_of(view.section_path(i)));
        if(view.section_path(i) == focus_path) {
            tu_index.focus_file_index = std::move(decoded);
        } else {
            tu_index.file_indices.emplace_back(view.section_path(i), std::move(decoded));
        }
    }

    auto record = [&](index::SymbolHash hash, const index::SymbolView& identity) {
        auto& symbol = tu_index.symbols[hash];
        symbol.name = identity.name.str();
        symbol.args = identity.args.str();
        symbol.signature = identity.signature.str();
        symbol.parent = identity.parent;
        symbol.kind = identity.kind;
        symbol.scope = identity.scope;
        symbol.flags = identity.flags;
        symbol.file = identity.file;
        return &symbol;
    };
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& identity,
                             llvm::ArrayRef<std::uint32_t> references) {
        record(hash, identity)->reference_files.addMany(references.size(), references.data());
        return true;
    });
    // A function's locals are named by their sections only.
    auto record_locals = [&](const DecodedShard& decoded) {
        auto record_local = [&](index::SymbolHash hash) {
            if(!tu_index.symbols.contains(hash)) {
                if(auto identity = view.find_symbol(hash)) {
                    ZEXPECT(identity->scope == index::SymbolScope::FunctionLocal);
                    record(hash, *identity);
                }
            }
        };
        for(auto& occurrence: decoded.occurrences) {
            record_local(occurrence.target);
        }
        for(auto& hash: llvm::make_first_range(decoded.relations)) {
            record_local(hash);
        }
    };
    record_locals(tu_index.focus_file_index);
    for(auto& decoded: llvm::make_second_range(tu_index.file_indices)) {
        record_locals(decoded);
    }
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
    add_focus("main.cpp", code);
    ZASSERT(compile());

    decode_index(index::build_tu_index(*unit));
}

auto select(llvm::StringRef pos) -> std::vector<index::Occurrence> {
    auto offset = point(pos);
    auto& index = tu_index.focus_file_index;

    auto it =
        std::ranges::lower_bound(index.occurrences, offset, {}, [](index::Occurrence& occurrence) {
            return occurrence.range.end;
        });

    std::vector<index::Occurrence> occurrences;
    while(it != index.occurrences.end()) {
        if(it->range.touches(offset)) {
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

    ZASSERT(occurrences.front().range == expected);
};

/// Whether the focus file holds a symbol-pair relation of `kind` from the
/// symbol at marker `source` to the one at marker `target`.
bool has_pair(llvm::StringRef source, RelationKind::Kind kind, llvm::StringRef target) {
    auto sources = select(source);
    auto targets = select(target);
    if(sources.empty() || targets.empty()) {
        return false;
    }
    auto it = tu_index.focus_file_index.relations.find(sources.front().target);
    if(it == tu_index.focus_file_index.relations.end()) {
        return false;
    }
    return llvm::any_of(it->second, [&](const index::Relation& relation) {
        return relation.kind == kind && relation.target == targets.front().target;
    });
}

/// One focus-file occurrence: the text it covers, its symbol's kind and the
/// kinds of that symbol's relations whose range holds it.
struct Spelled {
    std::string text;
    SymbolKind::Kind kind;
    std::vector<RelationKind::Kind> relations{};

    bool operator==(const Spelled&) const = default;
};

/// Every focus-file occurrence inside range `name`, in file order.
std::vector<Spelled> spelled_in(llvm::StringRef name) {
    auto scope = range(name);
    llvm::StringRef content = sources.all_files[src_path].content;
    std::vector<Spelled> spelled;
    for(auto& occurrence: tu_index.focus_file_index.occurrences) {
        if(!scope.contains(occurrence.range)) {
            continue;
        }
        Spelled entry{
            .text = content.slice(occurrence.range.begin, occurrence.range.end).str(),
            .kind = tu_index.symbols.lookup(occurrence.target).kind,
        };
        for(auto& relation: tu_index.focus_file_index.relations.lookup(occurrence.target)) {
            if(relation.range.contains(occurrence.range)) {
                entry.relations.push_back(relation.kind);
            }
        }
        spelled.push_back(std::move(entry));
    }
    return spelled;
}

void GO_TO_DEFINITION(llvm::StringRef pos,
                      llvm::StringRef definition,
                      std::source_location location = std::source_location::current()) {
    auto expected = range(definition);
    auto occurrences = select(pos);

    ZASSERT(occurrences.size() == 1U);

    auto& index = tu_index.focus_file_index;
    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());

    auto& relations = it->second;
    auto target = std::ranges::find_if(relations, [](const index::Relation& relation) {
        return relation.kind == RelationKind::Definition;
    });

    ZASSERT(target != relations.end());
    ZASSERT(target->range == expected);
}

ZEST_CASE(Basic) {
    build_index(R"(
            int §(1)⟦f§(1)oo⟧();

            int §(2)⟦b§(2)ar⟧() {
                return §(3)⟦fo§(3)o⟧() + 1;
            }
        )");

    auto& index = tu_index.focus_file_index;
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

ZEST_CASE(InheritedConstructorCall) {
    build_index(R"(
            struct Base {
                §(base)⟦Base⟧(int x) {}
            };
            struct Derived : Base {
                using Base::Base;
            };
            void build() {
                Derived d §(paren)(3);
                Derived e §(brace){4};
            }
        )");

    GO_TO_DEFINITION("paren", "base");
    GO_TO_DEFINITION("brace", "base");
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

    auto& index = tu_index.focus_file_index;
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
    auto& relations = tu_index.focus_file_index.relations[select("box").front().target];
    ZASSERT(llvm::count_if(relations, [](const index::Relation& relation) {
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

    auto& index = tu_index.focus_file_index;

    // Find caller symbol and check for Callee relation.
    auto caller_occurrences = select("caller_def");
    ZASSERT(!caller_occurrences.empty());
    auto caller_hash = caller_occurrences.front().target;

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
    auto callee_occurrences = select("callee_def");
    ZASSERT(!callee_occurrences.empty());
    auto callee_hash = callee_occurrences.front().target;

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

    auto& index = tu_index.focus_file_index;

    // Calls inside a method body produce call edges, with the method as
    // the caller.
    auto method_occurrences = select("method_def");
    ZASSERT(!method_occurrences.empty());
    auto method_hash = method_occurrences.front().target;

    auto callee_occurrences = select("callee_def");
    ZASSERT(!callee_occurrences.empty());
    auto callee_hash = callee_occurrences.front().target;

    auto method_it = index.relations.find(method_hash);
    ZASSERT(method_it != index.relations.end());

    bool found_callee = false;
    for(auto& r: method_it->second) {
        if(r.kind == RelationKind::Callee && r.target == callee_hash) {
            found_callee = true;
            break;
        }
    }
    ZASSERT(found_callee);

    auto callee_it = index.relations.find(callee_hash);
    ZASSERT(callee_it != index.relations.end());

    bool found_caller = false;
    for(auto& r: callee_it->second) {
        if(r.kind == RelationKind::Caller && r.target == method_hash) {
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

    auto& index = tu_index.focus_file_index;

    // The relation of a using declaration is keyed by the same symbol
    // its occurrence references, so looking the occurrence's symbol up in
    // the relations always finds the using site.
    auto use_occurrences = select("use");
    ZASSERT(!use_occurrences.empty());
    auto hash = use_occurrences.front().target;

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

    auto& index = tu_index.focus_file_index;

    /// The dependent name resolves through the template resolver to the
    /// pattern's member; both sites share one symbol.
    auto use_occurrences = select("use");
    ZASSERT(!use_occurrences.empty());
    auto target_occurrences = select("target");
    ZASSERT(!target_occurrences.empty());
    ZASSERT(use_occurrences.front().target == target_occurrences.front().target);

    auto it = index.relations.find(use_occurrences.front().target);
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

            S §(make)⟦§(make)make⟧();
        )");

    ZASSERT(has_pair("field", RelationKind::TypeDefinition, "s"));
    ZASSERT(has_pair("alias", RelationKind::TypeDefinition, "s"));
    ZASSERT(has_pair("ec", RelationKind::TypeDefinition, "e"));
    ZASSERT(has_pair("make", RelationKind::TypeDefinition, "s"));
}

ZEST_CASE(SpecializedMemberType) {
    build_index(R"(
            struct §(widget)Widget {};

            template <typename T>
            struct Box {
                T §(value)value;
            };

            Widget read(Box<Widget>& box) { return box.§(use)⟦§(use)value⟧; }
        )");

    // At a name of the member in Box<Widget>, a relation spanning the name
    // gives the type the member has there.
    auto& relations = tu_index.focus_file_index.relations;
    auto it = relations.find(select("value").front().target);
    ZASSERT(it != relations.end());
    auto widget = select("widget").front().target;
    ZASSERT(llvm::any_of(it->second, [&](const index::Relation& relation) {
        return relation.kind == RelationKind::TypeDefinition && relation.range == range("use") &&
               relation.target == widget;
    }));
}

ZEST_CASE(ConstructorDestructorRelations) {
    build_index(R"(
            struct §(s)⟦§(s)S⟧ {
                §(ctor)S();
                ~S();
            };
        )");

    auto& index = tu_index.focus_file_index;
    auto class_occurrences = select("s");
    auto constructor_occurrences = select("ctor");
    ZASSERT(!class_occurrences.empty());
    ZASSERT(!constructor_occurrences.empty());
    auto class_hash = class_occurrences.front().target;
    auto ctor_hash = constructor_occurrences.front().target;

    auto it = index.relations.find(class_hash);
    ZASSERT(it != index.relations.end());

    bool found_ctor = false;
    bool found_dtor = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Constructor && r.target == ctor_hash) {
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
        if(r.kind == RelationKind::TypeDefinition && r.target == class_hash) {
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

    auto& index = tu_index.focus_file_index;
    auto definition_occurrences = select("def");
    auto use_occurrences = select("use");
    ZASSERT(!definition_occurrences.empty());
    ZASSERT(!use_occurrences.empty());
    ZASSERT(definition_occurrences.front().target == use_occurrences.front().target);

    auto it = index.relations.find(definition_occurrences.front().target);
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
    auto& relations = tu_index.focus_file_index.relations[target];
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
        auto& relations = tu_index.focus_file_index.relations[symbol_named(name).first];
        ZASSERT(relations.size() == 1U);
        ZASSERT(relations.front().kind == RelationKind::Declaration);
    }
}

ZEST_CASE(PastedFragmentUses) {
    kind = CompilationKind::Whole;
    add_file("kinds.inc", "KIND(red)\nKIND(green)\n");
    add_file("decls.inc", "int helper();\nint value = helper();\n");
    add_file("take.h", "constexpr int fallback = 1;\nint take(int x = fallback);\n");
    add_focus("main.cpp", R"(
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
        for(auto& relation: tu_index.focus_file_index.relations[symbol_named(name).first]) {
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

    auto& index = tu_index.focus_file_index;
    auto occurrences = select("m");
    ZASSERT(!occurrences.empty());
    ZASSERT(occurrences.front().target == unit->get<EntityTable>().module_entity("foo"));
    ZASSERT(symbol_named("foo").second.kind.value() == SymbolKind(SymbolKind::Module).value());

    auto it = index.relations.find(occurrences.front().target);
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
    auto occurrences = select("m");
    ZASSERT(!occurrences.empty());
    ZASSERT(occurrences.front().range == range("m"));

    auto& index = tu_index.focus_file_index;
    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());

    bool found_definition = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            found_definition = true;
        }
    }
    ZASSERT(found_definition);
}

ZEST_CASE(InternalPartitionName) {
    build_index(R"(module §(m)⟦§(m)foo:part⟧;)");

    // An internal partition defines the partition it declares.
    auto occurrences = select("m");
    ZASSERT(!occurrences.empty());
    ZASSERT(occurrences.front().range == range("m"));

    auto& index = tu_index.focus_file_index;
    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());
    ZEXPECT(llvm::any_of(it->second, [](auto& r) { return r.kind == RelationKind::Definition; }));
}

ZEST_CASE(ImportedDeclarationsSkipped) {
    kind = CompilationKind::Whole;
    add_files("main.cpp", R"(
#[widget.h]
struct Widget {
    int value;
};

#[foo.cppm]
module;
#include "widget.h"
export module foo;
export Widget make();

#[main.cpp]
import foo;

int main() {
    return §(use)make().value;
}
)");
    ZASSERT(compile_with_modules());
    decode_index(index::build_tu_index(*unit));

    // The module's own unit indexes its declarations and the headers behind
    // them; an importer's index holds what it parsed.
    ZEXPECT(tu_index.file_indices.empty());
    auto occurrences = select("use");
    ZASSERT(!occurrences.empty());
    ZEXPECT(occurrences.front().target == symbol_named("make").first);
}

ZEST_CASE(ImportedDeclarationsNamed) {
    add_files("main.cpp", R"(
#[widget.h]
struct Widget {
    int value;
};

#[foo.cppm]
module;
#include "widget.h"
export module foo;
export Widget make();
export struct Point {
    int x;
};
export int unused();

#[main.cpp]
import foo;

int main() {
    return make().value + Point{1}.x;
}
)");
    ZASSERT(compile_with_modules());
    decode_index(index::build_tu_index(*unit));

    // The focus file's index declares what it names from the module, in the
    // module's file; the header behind the module is the module's to index.
    ZASSERT(tu_index.file_indices.size() == 1U);
    auto& [path_id, module_index] = tu_index.file_indices.front();
    ZEXPECT(tu_index.view.path(path_id).ends_with("foo.cppm"));
    ZEXPECT(module_index.relations.size() == 3U);
    auto& make = module_index.relations[symbol_named("make").first];
    ZASSERT(make.size() == 1U);
    ZEXPECT(make.front().kind == RelationKind::Declaration);
    for(auto name: {"Point", "x"}) {
        auto& defined = module_index.relations[symbol_named(name).first];
        ZASSERT(defined.size() == 1U);
        ZEXPECT(defined.front().kind == RelationKind::Definition);
    }
}

ZEST_CASE(ImplementationUnitDeclaration) {
    add_files("main.cpp", R"(
#[foo.cppm]
export module foo;
export int x = 1;

#[main.cpp]
module §(m)⟦§(m)foo⟧;
)");
    ZASSERT(compile_with_modules());
    decode_index(index::build_tu_index(*unit));

    // An implementation unit's module declaration is a Declaration of the
    // module, not a Definition.
    auto occurrences = select("m");
    ZASSERT(!occurrences.empty());
    ZASSERT(occurrences.front().range == range("m"));

    auto& index = tu_index.focus_file_index;
    auto it = index.relations.find(occurrences.front().target);
    ZASSERT(it != index.relations.end());

    bool found_declaration = false;
    for(auto& r: it->second) {
        if(r.kind == RelationKind::Definition) {
            ZASSERT(false);
        }
        if(r.kind == RelationKind::Declaration) {
            found_declaration = true;
        }
    }
    ZASSERT(found_declaration);
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

    auto check_relations = [&](DecodedShard& idx) {
        for(auto& [hash, relations]: idx.relations) {
            for(auto& r: relations) {
                if(r.kind == RelationKind::Interface)
                    found_interface = true;
                if(r.kind == RelationKind::Implementation)
                    found_implementation = true;
            }
        }
    };

    check_relations(tu_index.focus_file_index);
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

    auto& index = tu_index.focus_file_index;

    // Find the declaration occurrence and verify Declaration relation exists.
    auto declaration_occurrences = select("decl");
    ZASSERT(!declaration_occurrences.empty());
    auto symbol_hash = declaration_occurrences.front().target;

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
    auto& relations = tu_index.focus_file_index.relations[hash];
    auto definition = std::ranges::find_if(relations, [](const index::Relation& relation) {
        return relation.kind == RelationKind::Definition;
    });
    ZASSERT(definition != relations.end());
    ZASSERT(definition->extent() == range("ext"));
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
    kind = CompilationKind::Whole;
    add_file("header.h", R"(
            #pragma once
            int §(hdr_func)⟦§(hdr_func)helper⟧();
        )");
    add_focus("main.cpp", R"(
            #include "header.h"

            int main() {
                return §(use_helper)helper();
            }
        )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));

    // The header should have its own FileIndex (separate from the focus file).
    ZASSERT(tu_index.file_indices.size() >= 1U);

    // The focus file should have a reference to helper.
    auto& focus_index = tu_index.focus_file_index;
    ZASSERT(!focus_index.occurrences.empty());

    // Find 'helper' reference in focus file.
    auto use_offset = point("use_helper");
    auto it = std::ranges::lower_bound(focus_index.occurrences,
                                       use_offset,
                                       {},
                                       [](const index::Occurrence& o) { return o.range.end; });
    ZASSERT(it != focus_index.occurrences.end());
    ZASSERT(it->range.touches(use_offset));

    // The helper symbol should exist in the TU symbol table.
    auto helper_hash = it->target;
    ZASSERT(tu_index.symbols.contains(helper_hash));

    // The helper's declaration should be in the header's index.
    bool found_in_header = false;
    for(auto& [path_id, file_index]: tu_index.file_indices) {
        for(auto& [symbol, relations]: file_index.relations) {
            if(symbol == helper_hash) {
                found_in_header = true;
                break;
            }
        }
        if(found_in_header)
            break;
    }
    ZASSERT(found_in_header);
}

ZEST_CASE(LocalClassAsBase) {
    kind = CompilationKind::Whole;
    add_file("make.h", R"(
            #pragma once
            inline auto make() {
                struct Made {};
                return Made{};
            }
        )");
    add_focus("main.cpp", R"(
            #include "make.h"
            struct Derived : decltype(make()) {};
        )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit));
    // Only the base relation names the header function's class in
    // main.cpp: the table lists it with both files.
    auto made = symbol_named("Made").second;
    ZEXPECT(static_cast<int>(made.scope) == static_cast<int>(index::SymbolScope::FunctionLocal));
    ZEXPECT(made.reference_files.cardinality() == 2u);
}

ZEST_CASE(KnownVariantsSkipped) {
    kind = CompilationKind::Whole;
    add_file("header.h", R"(
            #pragma once
            int only_in_header(int parameter);
            static int internal();
            int shared();
        )");
    add_focus("main.cpp", R"(
            #include "header.h"
            int user() { return shared(); }
        )");
    ZASSERT(compile());
    auto full = index::build_tu_index(*unit);
    auto full_view = index::TUIndex::from_bytes(full);
    ZASSERT(full_view.loaded());
    auto focus_id = full_view.path_count() - 1;
    std::uint32_t header = full_view.section_count();
    for(std::uint32_t i = 0; i < full_view.section_count(); i += 1) {
        if(full_view.section_path(i) != focus_id) {
            header = i;
        }
    }
    ZASSERT(header < full_view.section_count());
    std::vector<std::uint64_t> known = {full_view.section_hash(header)};
    auto table_names = [](const index::TUIndex& view) {
        std::vector<std::string> names;
        view.for_each_symbol([&](index::SymbolHash,
                                 const index::SymbolView& identity,
                                 llvm::ArrayRef<std::uint32_t>) {
            names.emplace_back(identity.name);
            return true;
        });
        std::ranges::sort(names);
        return names;
    };
    ZASSERT(table_names(full_view) ==
            std::vector<std::string>{"internal", "only_in_header", "shared", "user"});

    // The stored header travels as its hash alone, and the external
    // symbols only it names stay out of the table; the focus file's index,
    // every symbol it names and the internal ones still travel.
    decode_index(index::build_tu_index(*unit, {.known_variants = known}));
    auto& view = tu_index.view;
    ZASSERT(view.section_hash(header) == known.front());
    ZASSERT(view.section_blob(header).empty());
    ZASSERT(!view.section_blob(*view.section_of(focus_id)).empty());
    ZASSERT(table_names(view) == std::vector<std::string>{"internal", "shared", "user"});
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
        auto occurrences = select(name);
        ZASSERT(!occurrences.empty());
        auto hash = occurrences.front().target;
        ZASSERT(tu_index.symbols.contains(hash));
        ZASSERT(tu_index.symbols[hash].kind.value() == expected.value());
    };

    check_kind("cls", SymbolKind::Struct);
    check_kind("enm", SymbolKind::Enum);
    check_kind("func", SymbolKind::Function);
    check_kind("var", SymbolKind::Variable);
    check_kind("ns", SymbolKind::Namespace);
}

ZEST_CASE(OccurrencesAt) {
    build_index(R"(
        int §(x)⟦fo§(x)o⟧();
        int §(ref)⟦fo§(ref)o⟧() { return 0; }
    )");

    auto& fi = tu_index.focus_file_index;
    ZASSERT(!fi.occurrences.empty());
    const index::Shard& shard = tu_index.view.shard_of(tu_index.view.path_count() - 1);
    ZASSERT(shard.loaded());

    auto x_range = range("x");
    std::optional<index::Occurrence> found;
    shard.occurrences_at(point("x"), [&](const index::Occurrence& occurrence) {
        found = occurrence;
        return true;
    });
    ZASSERT(found);
    ZEXPECT(found->range.begin == x_range.begin);
    ZEXPECT(found->range.end == x_range.end);

    found.reset();
    shard.occurrences_at(point("ref"), [&](const index::Occurrence& occurrence) {
        found = occurrence;
        return true;
    });
    ZASSERT(found);
    ZEXPECT(found->target == fi.occurrences.front().target);

    found.reset();
    shard.occurrences_at(0, [&](const index::Occurrence& occurrence) {
        found = occurrence;
        return false;
    });
    ZEXPECT(!found.has_value());
}

ZEST_CASE(RelationsOf) {
    build_index(R"(
        void §(decl)⟦fo§(decl)o⟧();
        void §(def)⟦fo§(def)o⟧() {}
    )");

    const index::Shard& shard = tu_index.view.shard_of(tu_index.view.path_count() - 1);
    ZASSERT(shard.loaded());

    std::optional<index::Occurrence> occurrence;
    shard.occurrences_at(point("decl"), [&](const index::Occurrence& o) {
        occurrence = o;
        return false;
    });
    ZASSERT(occurrence);

    auto def_range = range("def");
    bool found_def = false;
    shard.relations_of(occurrence->target, RelationKind::Definition, [&](const index::Relation& r) {
        found_def = true;
        ZEXPECT(r.range.begin == def_range.begin);
        ZEXPECT(r.range.end == def_range.end);
        return false;
    });
    ZEXPECT(found_def);

    bool found_any = false;
    shard.relations_of(occurrence->target, RelationKind::Caller, [&](const index::Relation&) {
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

ZEST_CASE(ScopeFunctionLocal) {
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
                    static_cast<int>(index::SymbolScope::FunctionLocal));
            found.insert(symbol.name);
        }
    }
    ZASSERT(found == expected);
}

ZEST_CASE(ScopeInternal) {
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
                    static_cast<int>(index::SymbolScope::Internal));
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
    ZASSERT(scope("static_var") == static_cast<int>(index::SymbolScope::Internal));
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
    ZASSERT(scope("hidden_value") == static_cast<int>(index::SymbolScope::Internal));
    ZASSERT(scope("hidden_alias") == static_cast<int>(index::SymbolScope::Internal));
    ZASSERT(scope("prototype_param") == static_cast<int>(index::SymbolScope::FunctionLocal));
    ZASSERT(scope("local_value") == static_cast<int>(index::SymbolScope::FunctionLocal));
}

ZEST_CASE(ScopeCEnumerator) {
    add_focus("main.c", R"c(
            enum color { red };
            void host(void) { enum { local_value }; }
        )c");
    ZASSERT(compile("-std=c17"));
    decode_index(index::build_tu_index(*unit));

    ZASSERT(scope("red") == static_cast<int>(index::SymbolScope::External));
    ZASSERT(scope("local_value") == static_cast<int>(index::SymbolScope::FunctionLocal));
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

ZEST_CASE(FunctionSignatures) {
    build_index(R"(
        namespace geometry { struct Point {}; }
        int scale(int factor, double ratio = 1.0);
        int scale(geometry::Point point);
        struct Holder {
            void resize(int width, int height) const;
        };
        int counter;
    )");

    llvm::SmallVector<std::string> overloads;
    for(auto& symbol: llvm::make_second_range(tu_index.symbols)) {
        if(symbol.name == "scale") {
            overloads.push_back(symbol.signature);
        }
    }
    llvm::sort(overloads);
    ZEXPECT(overloads == (llvm::SmallVector<std::string>{"(geometry::Point point)",
                                                         "(int factor, double ratio = 1.0)"}));
    ZEXPECT(symbol_named("resize").second.signature == "(int width, int height) const");
    ZEXPECT(symbol_named("counter").second.signature.empty());
}

ZEST_CASE(CanonicalFile) {
    add_file("header.h", R"(
        int declared_twice();
        int header_only();
        inline int header_defined() { return 1; }
    )");
    add_focus("main.cpp", R"(
        #include "header.h"
        int declared_twice();
        int declared_twice() { return header_only() + header_defined(); }
        int main_only();
    )");
    ZASSERT(compile());
    decode_index(index::build_tu_index(*unit, {.all_files = true}));

    auto focus_path = tu_index.view.path_count() - 1;
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
    ZASSERT(twice.file == focus_path);

    auto only = symbol_named("header_only").second;
    ZASSERT(!has(only, index::SymbolFlags::HasDefinition));
    ZASSERT(only.file == header_path);

    auto defined = symbol_named("header_defined").second;
    ZASSERT(has(defined, index::SymbolFlags::HasDefinition));
    ZASSERT(defined.file == header_path);
    ZASSERT(has(defined, index::SymbolFlags::Completable));

    // A focus-only build keeps no header entries: a symbol the focus file
    // merely uses has no declaring entry at all.
    decode_index(index::build_tu_index(*unit));
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
    add_focus("main.cpp", R"(
#include "foo.h"
int Foo::§(def)⟦§(1)find⟧(int x) const { return 0; }
)");
    ZASSERT(compile_with_pch());

    // A full build keeps the preamble entries: this proves the entry exists
    // (so the gate test below cannot pass vacuously) and that the loaded
    // fid resolves through the graph to the header's path.
    decode_index(index::build_tu_index(*unit, {.all_files = true}));
    bool found = false;
    for(auto& [path_id, index]: tu_index.file_indices) {
        found |= tu_index.view.path(path_id).ends_with("foo.h");
    }
    ZASSERT(found);

    decode_index(index::build_tu_index(*unit));

    // Entries resolving into the preamble are dropped: the preamble's own
    // index covers them. Only the focus file's entries remain.
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
    add_focus("main.cpp", R"(
#include "bar.h"
struct Derived;
Derived* use();
)");
    ZASSERT(compile_with_pch());

    // Full build: the base-specifier entries end up in the preamble header
    // and its loaded fid resolves to the header's path.
    decode_index(index::build_tu_index(*unit, {.all_files = true}));
    bool found = false;
    for(auto& [path_id, index]: tu_index.file_indices) {
        found |= tu_index.view.path(path_id).ends_with("bar.h");
    }
    ZASSERT(found);

    decode_index(index::build_tu_index(*unit));

    ZASSERT(tu_index.file_indices.empty());
    ZASSERT(!tu_index.focus_file_index.occurrences.empty());
}

ZEST_CASE(HeaderMacroDropped) {
    // Macro occurrences flow through the same gate: a definition in an
    // included header is dropped from a focus-only build, while
    // the reference in the focus file is kept.
    add_file("baz.h", R"(
#define BAZ 1
)");
    add_focus("main.cpp", R"(
#include "baz.h"
int x = §(1)BAZ;
)");
    ZASSERT(compile());

    decode_index(index::build_tu_index(*unit));
    ZASSERT(tu_index.file_indices.empty());
    ZASSERT(!tu_index.focus_file_index.occurrences.empty());
}

ZEST_CASE(UnknownFidFallback) {
    // A preamble header's loaded FileID is unknown to a graph built
    // without indexed fids; lookups must degrade to the focus file
    // instead of crashing.
    add_file("foo.h", R"(
struct Foo {};
)");
    add_focus("main.cpp", R"(
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
    add_focus("main.cpp", R"(
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

    add_focus("main.cpp", code);

    // Compile on a generous fixed-size stack: clang's own Sema checkers
    // recurse once per term and need more than a default thread stack in
    // sanitized builds (and Windows main threads only get 1MB). 32MB is
    // an empirical bound with margin, not a derived number.
    bool compiled = false;
    llvm::thread compile_thread(std::optional<unsigned>(32u << 20), [&] { compiled = compile(); });
    compile_thread.join();
    ZASSERT(compiled);

    // Index on a deliberately tight stack: the traversal must use
    // constant stack space however deep the expression is, so any
    // reintroduced per-node recursion crashes here deterministically
    // instead of only on production workers with deeper files.
    feature::InactiveScan scan;
    llvm::thread index_thread(std::optional<unsigned>(2u << 20), [&] {
        // Mirror the stateful worker's post-compile sequence.
        scan = feature::inactive_regions(*unit);
        decode_index(index::build_tu_index(*unit));

        // The semantic map must also serve token classification and a
        // selection at the giant expansion's invocation on this stack:
        // per-token ancestor walks and recursive tree materialization
        // both used to degrade on chains this deep.
        feature::semantic_tokens(*unit, {});
        auto use_offset = static_cast<std::uint32_t>(code.rfind("A14"));
        SelectionTree::create_right(*unit, LocalSourceRange(use_offset, use_offset));
    });
    index_thread.join();

    ZASSERT(scan.regions.empty());

    // The traversal must have actually reached the decl behind the chain,
    // not bailed out early: expect an occurrence exactly at `bomb`.
    auto& occurrences = tu_index.focus_file_index.occurrences;
    auto bomb = std::ranges::find(occurrences, bomb_offset, [](index::Occurrence& occurrence) {
        return occurrence.range.begin;
    });
    ZASSERT(bomb != occurrences.end());
}

ZEST_CASE(SuperQualifierRef) {
    add_focus("main.cpp", R"(
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
    params.input.arguments.insert(params.input.arguments.end() - 1, "-fms-extensions");
    ZASSERT(try_compile());
    decode_index(index::build_tu_index(*unit));

    GO_TO_DEFINITION("use", "def");
}

ZEST_CASE(HierarchyOccurrenceRelations) {
    build_index(R"(
        §(all)⟦namespace demo {

        struct Base {
            virtual ~Base() = default;
            virtual int value() const;
        };

        struct Derived : Base {
            int value() const override;
        };

        int use(const Base& b) {
            return b.value();
        }

        }⟧
    )");

    ZEXPECT(spelled_in("all") ==
            std::vector<Spelled>{
                {"demo",     SymbolKind::Namespace, {RelationKind::Definition}                     },
                {"Base",     SymbolKind::Struct,    {RelationKind::Definition}                     },
                {"~Base",    SymbolKind::Method,    {RelationKind::Definition}                     },
                {"value",    SymbolKind::Method,    {RelationKind::Declaration}                    },
                {"Derived",  SymbolKind::Struct,    {RelationKind::Definition}                     },
                {"Base",     SymbolKind::Struct,    {RelationKind::Reference}                      },
                {"value",    SymbolKind::Method,    {RelationKind::Declaration}                    },
                {"override", SymbolKind::Method,    {RelationKind::Implied}                        },
                {"use",      SymbolKind::Function,  {RelationKind::Definition}                     },
                {"Base",     SymbolKind::Struct,    {RelationKind::Reference}                      },
                {"b",        SymbolKind::Parameter, {RelationKind::Definition}                     },
                {"b",        SymbolKind::Parameter, {RelationKind::Reference}                      },
                {"value",    SymbolKind::Method,    {RelationKind::Reference, RelationKind::Caller}},
    });
}

ZEST_CASE(CoAwaitReferencesOperator) {
    build_index(R"(
        namespace std {
        template <typename Ret, typename...>
        struct coroutine_traits {
            §(traits)⟦using promise_type = typename Ret::promise_type⟧;
        };

        template <typename = void>
        struct coroutine_handle {
            coroutine_handle() = default;
            template <typename Promise>
            coroutine_handle(coroutine_handle<Promise>) noexcept;
            static coroutine_handle from_address(void*) noexcept;
        };

        struct suspend_never {
            bool await_ready() const noexcept;
            void await_suspend(coroutine_handle<>) const noexcept;
            void await_resume() const noexcept;
        };
        }  // namespace std

        struct Task {
            struct promise_type {
                Task get_return_object();
                std::suspend_never initial_suspend();
                std::suspend_never final_suspend() noexcept;
                void return_void();
                void unhandled_exception();
            };
        };

        struct Awaitable {
            std::suspend_never §(operator)⟦§(operator)operator co_await⟧() const;
        };

        §(coro)⟦Task coro(Awaitable value) {
            §(await)co_await value;
        }⟧
    )");

    ZEXPECT(spelled_in("operator") ==
            std::vector<Spelled>{
                {"operator co_await", SymbolKind::Operator, {RelationKind::Declaration}},
    });
    ZEXPECT(spelled_in("traits") ==
            std::vector<Spelled>{
                {"promise_type", SymbolKind::Type, {RelationKind::Definition}},
                {"Ret",          SymbolKind::Type, {RelationKind::Reference} },
    });
    ZEXPECT(spelled_in("coro") == std::vector<Spelled>{
                                      {"Task",      SymbolKind::Struct,    {RelationKind::Reference} },
                                      {"coro",      SymbolKind::Function,  {RelationKind::Definition}},
                                      {"Awaitable", SymbolKind::Struct,    {RelationKind::Reference} },
                                      {"value",     SymbolKind::Parameter, {RelationKind::Definition}},
                                      {"co_await",  SymbolKind::Method,    {RelationKind::Reference} },
                                      {"co_await",  SymbolKind::Operator,  {RelationKind::Reference} },
                                      {"co_await",  SymbolKind::Method,    {RelationKind::Reference} },
                                      {"co_await",  SymbolKind::Method,    {RelationKind::Reference} },
                                      {"value",     SymbolKind::Parameter, {RelationKind::Reference} },
    });
    // Beside `operator co_await`, the keyword names the three awaiter
    // methods it calls.
    auto keyword = select("await");
    auto declaration = select("operator");
    ZASSERT(keyword.size() == 4U);
    ZASSERT(declaration.size() == 1U);
    ZEXPECT(llvm::any_of(keyword, [&](const index::Occurrence& occurrence) {
        return occurrence.target == declaration.front().target;
    }));
}

ZEST_CASE(InvalidBindingKeepsFile) {
    build_index(R"(
        void broken() {
            §(broken)⟦auto [a, b] = 0⟧;
        }

        struct Pair {
            int first;
            int second;
        };

        int fine() {
            §(fine)⟦auto [left, right] = Pair{};
            return left + right⟧;
        }
    )");

    ZEXPECT(std::ranges::any_of(unit->diagnostics(), [](const Diagnostic& diagnostic) {
        return diagnostic.id.level >= DiagnosticLevel::Error;
    }));
    ZEXPECT(spelled_in("broken") == std::vector<Spelled>{
                                        {"a", SymbolKind::Variable, {RelationKind::Definition}},
                                        {"b", SymbolKind::Variable, {RelationKind::Definition}},
    });
    ZEXPECT(spelled_in("fine") == std::vector<Spelled>{
                                      {"auto",  SymbolKind::Struct,   {RelationKind::Deduced}   },
                                      {"left",  SymbolKind::Variable, {RelationKind::Definition}},
                                      {"right", SymbolKind::Variable, {RelationKind::Definition}},
                                      {"Pair",  SymbolKind::Struct,   {RelationKind::Reference} },
                                      {"left",  SymbolKind::Variable, {RelationKind::Reference} },
                                      {"right", SymbolKind::Variable, {RelationKind::Reference} },
    });
}

ZEST_CASE(ClassParametersStayUnbound) {
    add_focus("main.cpp", R"(
        struct Widget {};

        template <typename T>
        struct Holder {
            void reset() {
                §(local)⟦auto copy = T()⟧;
            }
        };

        template struct Holder<Widget>;
    )");
    ZASSERT(compile());

    // Every unit shares a template's entries, so its parameter stays
    // unbound there, an open file's index included: `auto` stands for `T`.
    // What an instantiation deduces is its own record.
    decode_index(index::build_tu_index(*unit, {.all_files = true}));
    ZEXPECT(spelled_in("local") == std::vector<Spelled>{
                                       {"auto", SymbolKind::Type,     {RelationKind::Deduced}   },
                                       {"copy", SymbolKind::Variable, {RelationKind::Definition}},
                                       {"T",    SymbolKind::Type,     {RelationKind::Reference} },
    });
}

ZEST_CASE(DeducedKeywords) {
    build_index(R"(
        namespace std {
        template <class T>
        struct unique_ptr {};

        namespace __detail {
        template <class T>
        using __unique_ptr_t = unique_ptr<T>;
        }

        template <class T>
        __detail::__unique_ptr_t<T> make_unique() {
            return {};
        }
        }  // namespace std

        struct Widget {
            int size;

            int area() {
                return §(self)⟦this->size⟧;
            }
        };

        void once(§(parameter)⟦auto value⟧) {}

        void use() {
            once(Widget{});
            §(made)⟦auto made⟧ = std::make_unique<Widget>();
        }
    )");

    // `this` names nothing: the code spelling it sits in the class.
    ZEXPECT(spelled_in("self") == std::vector<Spelled>{
                                      {"size", SymbolKind::Field, {RelationKind::Reference}},
    });
    // The keyword names no template parameter of its own, and the type a
    // call passes is that instantiation's, recorded apart from the written
    // code (InstantiationIndex.AbbreviatedParameterDeduced).
    ZEXPECT(spelled_in("parameter") ==
            std::vector<Spelled>{
                {"value", SymbolKind::Parameter, {RelationKind::Definition}},
    });
    // A library's internal alias gives way to the type it names.
    ZEXPECT(spelled_in("made") == std::vector<Spelled>{
                                      {"auto", SymbolKind::Struct,   {RelationKind::Deduced}   },
                                      {"made", SymbolKind::Variable, {RelationKind::Definition}},
    });
}

ZEST_CASE(MacroNameFirstToken) {
    build_index(R"(
        #define §(macro)⟦NAME⟧ Widget

        struct Widget {
            §(destructor)⟦~NAME()⟧;
        };
    )");

    ZEXPECT(spelled_in("macro") == std::vector<Spelled>{
                                       {"NAME", SymbolKind::Macro, {RelationKind::Definition}},
    });
    ZEXPECT(spelled_in("destructor") == std::vector<Spelled>{
                                            {"~",    SymbolKind::Method, {RelationKind::Declaration}},
                                            {"NAME", SymbolKind::Macro,  {RelationKind::Reference}  },
    });
}

ZEST_CASE(GlobalAllocationAtKeyword) {
    build_index(R"(
        struct Widget {};

        void make() {
            §(new)⟦Widget* widget = ::new Widget⟧;
            §(delete)⟦::delete widget⟧;
        }
    )");

    ZEXPECT(spelled_in("new") ==
            std::vector<Spelled>{
                {"Widget", SymbolKind::Struct,   {RelationKind::Reference}                      },
                {"widget", SymbolKind::Variable, {RelationKind::Definition}                     },
                {"new",    SymbolKind::Operator, {RelationKind::Reference, RelationKind::Caller}},
                {"Widget", SymbolKind::Struct,   {RelationKind::Reference}                      },
    });
    ZEXPECT(spelled_in("delete") ==
            std::vector<Spelled>{
                {"delete", SymbolKind::Operator, {RelationKind::Reference, RelationKind::Caller}},
                {"widget", SymbolKind::Variable, {RelationKind::Reference}                      },
    });
}

ZEST_CASE(OccurrenceCoverage) {
    add_focus("main.cpp", R"cpp(
        struct S {
            int first;
            int field;
        };

        int by_offsetof() {
            return §(offsetof)⟦__builtin_offsetof(S, field)⟧;
        }

        int §(literal_operator)⟦operator""_unit(unsigned long long value)⟧;

        int literal() {
            return §(literal)⟦12_unit⟧;
        }

        void cleanup_target(int* pointer);

        void attributes(int value) {
            §(cleanup)⟦int x __attribute__((cleanup(cleanup_target)))⟧;
            §(assume)⟦[[assume(value > 0)]]⟧;
        }

        void jump(int n) {
            §(asm_goto)⟦asm goto("" : : : : done);
            if(n) {
                goto done;
            }
        done:⟧
            return;
        }

        constexpr bool enabled = true;

        struct Explicit {
            §(explicit)⟦explicit(enabled) Explicit(int);
            explicit(enabled) operator bool() const⟧;
        };

        struct Property {
            int get_value();
            void put_value(int);
            §(property)⟦__declspec(property(get = get_value, put = put_value)) int value⟧;
        };

        int property(Property p) {
            §(property_uses)⟦p.value = 1;
            return p.value⟧;
        }

        template <class T>
        struct Delegating {
            Delegating(T);
            §(delegating)⟦Delegating() : Delegating(T{}) {}⟧
        };
    )cpp");
    triple = "x86_64-unknown-linux-gnu";
    ZASSERT(compile("-std=c++23"));
    decode_index(index::build_tu_index(*unit));

    ZEXPECT(spelled_in("offsetof") == std::vector<Spelled>{
                                          {"S",     SymbolKind::Struct, {RelationKind::Reference}},
                                          {"field", SymbolKind::Field,  {RelationKind::Reference}},
    });
    ZEXPECT(spelled_in("literal_operator") ==
            std::vector<Spelled>{
                {R"(operator""_unit)", SymbolKind::Function,  {RelationKind::Declaration}},
                {"value",              SymbolKind::Parameter, {RelationKind::Definition} },
    });
    ZEXPECT(spelled_in("literal") ==
            std::vector<Spelled>{
                {"12_unit", SymbolKind::Function, {RelationKind::Reference, RelationKind::Caller}},
    });
    ZEXPECT(spelled_in("cleanup") ==
            std::vector<Spelled>{
                {"x",              SymbolKind::Variable, {RelationKind::Definition}},
                {"cleanup_target", SymbolKind::Function, {RelationKind::Reference} },
    });
    ZEXPECT(spelled_in("assume") == std::vector<Spelled>{
                                        {"value", SymbolKind::Parameter, {RelationKind::Reference}},
    });
    ZEXPECT(spelled_in("asm_goto") == std::vector<Spelled>{
                                          {"done", SymbolKind::Label,     {RelationKind::Reference} },
                                          {"n",    SymbolKind::Parameter, {RelationKind::Reference} },
                                          {"done", SymbolKind::Label,     {RelationKind::Reference} },
                                          {"done", SymbolKind::Label,     {RelationKind::Definition}},
    });
    ZEXPECT(spelled_in("explicit") ==
            std::vector<Spelled>{
                {"enabled",  SymbolKind::Variable, {RelationKind::Reference}  },
                {"Explicit", SymbolKind::Method,   {RelationKind::Declaration}},
                {"enabled",  SymbolKind::Variable, {RelationKind::Reference}  },
                {"operator", SymbolKind::Method,   {RelationKind::Declaration}},
    });
    ZEXPECT(spelled_in("property") == std::vector<Spelled>{
                                          {"value", SymbolKind::Field, {RelationKind::Definition}},
    });
    ZEXPECT(spelled_in("property_uses") ==
            std::vector<Spelled>{
                {"p",     SymbolKind::Parameter, {RelationKind::Reference}},
                {"value", SymbolKind::Field,     {RelationKind::Reference}},
                {"p",     SymbolKind::Parameter, {RelationKind::Reference}},
                {"value", SymbolKind::Field,     {RelationKind::Reference}},
    });
    ZEXPECT(spelled_in("delegating") ==
            std::vector<Spelled>{
                {"Delegating", SymbolKind::Method, {RelationKind::Definition}   },
                {"Delegating", SymbolKind::Struct, {RelationKind::Reference}    },
                {"(",          SymbolKind::Method, {RelationKind::WeakReference}},
                {"T",          SymbolKind::Type,   {RelationKind::Reference}    },
    });
}

ZEST_CASE(UnnamedDeclarationsUnrecorded) {
    build_index(R"(
        struct Pair {
            int first;
            int second;
        };

        int use() {
            §(binding)⟦auto [left, right] = Pair{};
            return left + right⟧;
        }

        §(pack)⟦template <class T>
        int pack(T source) {
            auto [... elements] = source;
            return (elements + ...);
        }⟧

        §(template_template)⟦template <template <class> class TT>
        struct Holder⟧;

        auto lambda = §(lambda)⟦[]<class T>(T) -> int { return 0; }⟧;

        §(parameters)⟦int f(unsigned);
        int g(int, char*);⟧
    )");

    ZEXPECT(spelled_in("binding") ==
            std::vector<Spelled>{
                {"auto",  SymbolKind::Struct,   {RelationKind::Deduced}   },
                {"left",  SymbolKind::Variable, {RelationKind::Definition}},
                {"right", SymbolKind::Variable, {RelationKind::Definition}},
                {"Pair",  SymbolKind::Struct,   {RelationKind::Reference} },
                {"left",  SymbolKind::Variable, {RelationKind::Reference} },
                {"right", SymbolKind::Variable, {RelationKind::Reference} },
    });
    ZEXPECT(spelled_in("pack") ==
            std::vector<Spelled>{
                {"T",        SymbolKind::Type,      {RelationKind::Definition}},
                {"pack",     SymbolKind::Function,  {RelationKind::Definition}},
                {"T",        SymbolKind::Type,      {RelationKind::Reference} },
                {"source",   SymbolKind::Parameter, {RelationKind::Definition}},
                {"auto",     SymbolKind::Type,      {RelationKind::Deduced}   },
                {"elements", SymbolKind::Variable,  {RelationKind::Definition}},
                {"source",   SymbolKind::Parameter, {RelationKind::Reference} },
                {"elements", SymbolKind::Variable,  {RelationKind::Reference} },
    });
    ZEXPECT(spelled_in("template_template") ==
            std::vector<Spelled>{
                {"TT",     SymbolKind::Type,   {RelationKind::Definition} },
                {"Holder", SymbolKind::Struct, {RelationKind::Declaration}},
    });
    ZEXPECT(spelled_in("lambda") == std::vector<Spelled>{
                                        {"T", SymbolKind::Type, {RelationKind::Definition}},
                                        {"T", SymbolKind::Type, {RelationKind::Reference} },
    });
    ZEXPECT(spelled_in("parameters") ==
            std::vector<Spelled>{
                {"f", SymbolKind::Function, {RelationKind::Declaration}},
                {"g", SymbolKind::Function, {RelationKind::Declaration}},
    });
}

ZEST_CASE(ConstrainedParameterOneDefinition) {
    build_index(R"(
        template <class T>
        concept Concept = true;

        §(constrained)⟦template <Concept T>
        void constrained(T value);⟧

        §(abbreviated)⟦void abbreviated(Concept auto value);⟧
    )");

    ZEXPECT(spelled_in("constrained") ==
            std::vector<Spelled>{
                {"Concept",     SymbolKind::Concept,   {RelationKind::Reference}  },
                {"T",           SymbolKind::Type,      {RelationKind::Definition} },
                {"constrained", SymbolKind::Function,  {RelationKind::Declaration}},
                {"T",           SymbolKind::Type,      {RelationKind::Reference}  },
                {"value",       SymbolKind::Parameter, {RelationKind::Definition} },
    });
    ZEXPECT(spelled_in("abbreviated") ==
            std::vector<Spelled>{
                {"abbreviated", SymbolKind::Function,  {RelationKind::Declaration}},
                {"Concept",     SymbolKind::Concept,   {RelationKind::Reference}  },
                {"value",       SymbolKind::Parameter, {RelationKind::Definition} },
    });
}

ZEST_CASE(SynthesizedCallsKeepRelations) {
    add_focus("main.cpp", R"(
        namespace std {
        template <class T>
        struct reference_wrapper {
            T* pointer;
            T& get() const {
                return *pointer;
            }
        };

        template <class T>
        reference_wrapper<T> ref(T& value) {
            return {&value};
        }
        }  // namespace std

        §(record)⟦struct S {
            void func() {}
        }⟧;

        void call() {
            §(calls)⟦S s;
            __builtin_invoke(&S::func, std::ref(s));
            __noop⟧;
        }
    )");
    ZASSERT(compile("-std=c++23"));
    decode_index(index::build_tu_index(*unit));

    ZEXPECT(spelled_in("record") == std::vector<Spelled>{
                                        {"S",    SymbolKind::Struct, {RelationKind::Definition}},
                                        {"func", SymbolKind::Method, {RelationKind::Definition}},
    });
    ZEXPECT(spelled_in("calls") ==
            std::vector<Spelled>{
                {"S",                SymbolKind::Struct,    {RelationKind::Reference}                      },
                {"s",                SymbolKind::Variable,  {RelationKind::Definition}                     },
                {"__builtin_invoke",
                 SymbolKind::Method,
                 {RelationKind::Reference, RelationKind::TypeDefinition, RelationKind::Caller}             },
                {"S",                SymbolKind::Struct,    {RelationKind::Reference}                      },
                {"func",             SymbolKind::Method,    {RelationKind::Reference}                      },
                {"std",              SymbolKind::Namespace, {RelationKind::Reference}                      },
                {"ref",              SymbolKind::Function,  {RelationKind::Reference, RelationKind::Caller}},
                {"s",                SymbolKind::Variable,  {RelationKind::Reference, RelationKind::Write} },
                {"__noop",           SymbolKind::Function,  {RelationKind::Reference, RelationKind::Caller}},
    });
}

};  // ZEST_SUITE(Indexer)

/// A TU index blob holding only the minimal path table verification demands,
/// under `version`.
std::string blob_of_version(std::uint32_t version) {
    auto blob = tu_index_blob();
    blob.format_version = version;
    blob.paths = {"/proj/main.cpp"};
    return tu_index_bytes(blob);
}

ZEST_SUITE(PreambleIndex, Tester) {

TempDir dir;
std::shared_ptr<index::TUIndex> state;

std::vector<index::DocumentLink> links;
std::vector<std::uint32_t> inactive;
std::vector<std::uint8_t> conditionals;
std::string diagnostics;

/// Compile, build a preamble TU index, persist it as the `.pch.idx` pair
/// and load it back through the production gate.
void build_state(std::source_location location = std::source_location::current()) {
    // Not an open file's compile: the preamble's index covers every file.
    kind = CompilationKind::Whole;
    ZASSERT(compile());

    links.resize(1);
    links[0].range = {12, 20};
    links[0].target = "/include/foo.h";
    inactive = {4, 9, 30, 42};
    conditionals = {1, 0, 2};
    diagnostics = R"([{"range":{},"message":"'M' macro redefined"}])";

    dir.touch("state.pch.idx",
              index::build_preamble_index(*unit, links, inactive, conditionals, diagnostics));
    state = load_pch_index(dir.path("state.pch.idx"));
    ZASSERT(state != nullptr);
}

index::SymbolHash hash_of(llvm::StringRef name,
                          std::source_location location = std::source_location::current()) {
    index::SymbolHash hash = 0;
    std::uint32_t count = 0;
    state->for_each_symbol([&](index::SymbolHash symbol_id,
                               const index::SymbolView& symbol,
                               llvm::ArrayRef<std::uint32_t>) {
        if(symbol.name == name) {
            hash = symbol_id;
            count += 1;
        }
        return true;
    });
    ZEXPECT(count == 1);
    return hash;
}

/// Walk a symbol's relations in every header section (all but the
/// focus file's), the way the query layer's overlay lookup serves them.
void lookup_headers(index::SymbolHash hash,
                    RelationKind kind,
                    llvm::function_ref<bool(llvm::StringRef, const index::Relation&)> callback) {
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto path_id = state->section_path(i);
        if(path_id == state->path_count() - 1) {
            continue;
        }
        bool keep = true;
        state->shard_of(path_id).relations_of(hash, kind, [&](const index::Relation& r) {
            keep = callback(state->path(path_id), r);
            return keep;
        });
        if(!keep) {
            return;
        }
    }
}

ZEST_CASE(ForcedIncludeServed) {
    add_file("forced.h", R"(int §(def)⟦forced_value⟧ = 1;)");
    add_focus("main.cpp", R"(int x = forced_value;)");

    // A compile-command forced include: clang records its include edge in
    // the predefines buffer, which is a valid location — so unlike the
    // synthetic buffers themselves, the file must stay in the TU index
    // under its own path.
    kind = CompilationKind::Whole;
    prepare();
    params.input.arguments.insert(params.input.arguments.end() - 1, "-include");
    params.input.arguments.insert(params.input.arguments.end() - 1, TestVFS::path("forced.h"));
    ZASSERT(try_compile());

    dir.touch("state.pch.idx", index::build_preamble_index(*unit, {}, {}, {}, {}));
    state = load_pch_index(dir.path("state.pch.idx"));
    ZASSERT(state != nullptr);

    bool found = false;
    lookup_headers(hash_of("forced_value"),
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       ZEXPECT(path.ends_with("forced.h"));
                       ZEXPECT(r.range == range("def", "forced.h"));
                       found = true;
                       return false;
                   });
    ZEXPECT(found);
}

ZEST_CASE(HeaderRelationLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
inline void bar() { §(href)⟦foo⟧(); }
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    // The definition inside the header is served from its section.
    bool found_def = false;
    lookup_headers(foo,
                   RelationKind::Definition,
                   [&](llvm::StringRef path, const index::Relation& r) {
                       ZEXPECT(path.ends_with("foo.h"));
                       ZEXPECT(r.range == range("def", "foo.h"));
                       found_def = true;
                       return false;
                   });
    ZEXPECT(found_def);

    // Header-internal references are in the TU index too.
    bool found_ref = false;
    lookup_headers(foo, RelationKind::Reference, [&](llvm::StringRef, const index::Relation& r) {
        if(r.range == range("href", "foo.h")) {
            found_ref = true;
            return false;
        }
        return true;
    });
    ZEXPECT(found_ref);

    // Everything needed to map entries to LSP positions rides in each shard;
    // the content itself is not stored.
    for(std::uint32_t i = 0; i < state->section_count(); i += 1) {
        auto& shard = state->shard_of(state->section_path(i));
        ZEXPECT(shard.content_size() > 0);
        ZEXPECT(!shard.line_starts().empty());
        ZEXPECT(shard.positions().text().empty());
    }
}

ZEST_CASE(PreambleLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦§(ref)foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");
    const index::Shard& preamble = state->shard_of(state->path_count() - 1);
    ZASSERT(preamble.loaded());

    // Occurrence lookup by offset in the preamble entry.
    bool found_occurrence = false;
    preamble.occurrences_at(point("ref"), [&](const index::Occurrence& occurrence) {
        ZEXPECT(occurrence.target == foo);
        ZEXPECT(occurrence.range == range("ref"));
        found_occurrence = true;
        return false;
    });
    ZEXPECT(found_occurrence);

    // Relation lookup by symbol in the preamble entry.
    bool found_relation = false;
    preamble.relations_of(foo, RelationKind::Reference, [&](const index::Relation& r) {
        ZEXPECT(r.range == range("ref"));
        found_relation = true;
        return false;
    });
    ZEXPECT(found_relation);
}

ZEST_CASE(SymbolTableLookup) {
    add_file("foo.h", R"(
inline void §(def)⟦foo⟧() {}
)");
    add_focus("main.cpp", R"(
#include "foo.h"
int main() { §(ref)⟦foo⟧(); return 0; }
)");
    build_state();

    auto foo = hash_of("foo");

    auto identity = state->find_symbol(foo);
    ZASSERT(identity);
    ZEXPECT(identity->name == "foo");
    ZEXPECT(identity->kind.value() == SymbolKind(SymbolKind::Function).value());

    ZEXPECT(!state->find_symbol(foo + 1).has_value());
}

ZEST_CASE(FeatureStateRoundtrip) {
    add_focus("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto loaded_links = state->links();
    ZASSERT(loaded_links.size() == 1);
    ZEXPECT(loaded_links[0].range == LocalSourceRange(12, 20));
    ZEXPECT(loaded_links[0].target == "/include/foo.h");

    ZEXPECT(state->inactive_regions() == llvm::ArrayRef<std::uint32_t>(inactive));
    ZEXPECT(state->open_conditionals() == llvm::ArrayRef<std::uint8_t>(conditionals));
    ZEXPECT(state->preamble_diagnostics() == diagnostics);

    // A TU index with no header sections answers lookups with silence,
    // not UB.
    bool visited = false;
    lookup_headers(42, RelationKind::Reference, [&](llvm::StringRef, const index::Relation&) {
        visited = true;
        return true;
    });
    ZEXPECT(!visited);
}

ZEST_CASE(RejectBadBlob) {
    ZEXPECT(load_pch_index(dir.path("missing.pch.idx")) == nullptr);

    dir.touch("garbage.pch.idx", "not a flatbuffer at all");
    ZEXPECT(load_pch_index(dir.path("garbage.pch.idx")) == nullptr);
}

ZEST_CASE(RejectVersionMismatch) {
    // A structurally valid blob of another format version — none (0, what a
    // version-less blob reads back) or the one before this build's — must
    // load as missing, so the PCH pair rebuilds instead of serving a stale
    // layout.
    for(auto version: {0u, index::index_format_version - 1}) {
        dir.touch("stale.pch.idx", blob_of_version(version));
        ZEXPECT(load_pch_index(dir.path("stale.pch.idx")) == nullptr);
    }
}

ZEST_CASE(AcceptCurrentVersionBlob) {
    // Positive control for RejectVersionMismatch: the same blob carrying the
    // CURRENT version loads — the rejection comes from the version's value,
    // not from the blob's shape.
    dir.touch("current.pch.idx", blob_of_version(index::index_format_version));
    ZEXPECT(load_pch_index(dir.path("current.pch.idx")) != nullptr);
}

ZEST_CASE(RejectCorruptBlob) {
    add_focus("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    auto read = read_file(dir.path("state.pch.idx"));
    ZASSERT(read);
    llvm::StringRef bytes = *read;
    ZASSERT(bytes.size() > 8);

    dir.touch("truncated.pch.idx", bytes.take_front(bytes.size() / 2));
    ZEXPECT(load_pch_index(dir.path("truncated.pch.idx")) == nullptr);

    // Bytes 4-7 carry the buffer identifier; a blob from another format
    // must be rejected up front.
    std::string clobbered = bytes.str();
    for(std::size_t i = 4; i < 8; i += 1) {
        clobbered[i] = 'X';
    }
    dir.touch("clobbered.pch.idx", clobbered);
    ZEXPECT(load_pch_index(dir.path("clobbered.pch.idx")) == nullptr);
}

ZEST_CASE(RejectCorruptSectionBlob) {
    add_focus("main.cpp", R"(
int main() { return 0; }
)");
    build_state();

    // Overwrite one section's blob bytes in place: the TU index stays
    // structurally valid, but the load gate verifies every blob and must
    // read the pair as missing instead of silently serving nothing.
    auto read = read_file(dir.path("state.pch.idx"));
    ZASSERT(read);
    std::string bytes = std::move(*read);

    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());
    ZASSERT(view.section_count() > 0);
    auto blob = view.section_blob(0);
    auto pos = llvm::StringRef(bytes).find(blob);
    ZASSERT(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        bytes[pos + i] = 'X';
    }

    dir.touch("bad_section.pch.idx", bytes);
    ZEXPECT(load_pch_index(dir.path("bad_section.pch.idx")) == nullptr);
}

ZEST_CASE(SourcePathAndPrefix) {
    add_focus("main.cpp", R"(
int value = 42;
int other = 1;
)");
    build_state();

    ZEXPECT(state->path(state->path_count() - 1).ends_with("main.cpp"));

    // The preamble text itself is not stored; the TU index keeps only the
    // identity of the exact prefix it was built from.
    auto content = unit->focus_file().content();
    ZEXPECT(state->matches_preamble(content));
    ZEXPECT(state->matches_preamble(content.str() + "\nint more = 2;"));
    ZEXPECT(!state->matches_preamble(content.drop_back(1)));
    ZEXPECT(!state->matches_preamble("int changed = 0;"));

    // An ordinary TU index never serves preamble state.
    auto ordinary = index::build_tu_index(*unit);
    ZEXPECT(!index::TUIndex::from_bytes(ordinary).matches_preamble(content));
}

};  // ZEST_SUITE(PreambleIndex)

/// One record of what a TU's instantiations resolve in its focus file, as
/// text: what it spans, its kind, and the names it relates.
struct Resolved {
    std::string text;
    index::InstantiationKind kind;
    std::string target;
    std::string from{};
    bool expansion = false;

    bool operator==(const Resolved&) const = default;
};

ZEST_SUITE(InstantiationIndex, Tester) {

index::TUIndex view;

std::string name_of(index::SymbolHash hash) {
    auto identity = view.find_symbol(hash);
    return identity ? identity->name.str() : "?";
}

void build(llvm::StringRef code,
           const index::TUIndexOptions& options = {},
           CompilationKind compilation = CompilationKind::Whole) {
    kind = compilation;
    add_focus("main.cpp", code);
    ZASSERT(compile());
    view = index::TUIndex::from_buffer(
        llvm::MemoryBuffer::getMemBufferCopy(index::build_tu_index(*unit, options)));
    ZASSERT(view.loaded());
}

std::optional<std::uint32_t> focus_section() {
    return view.instantiation_section_of(view.path_count() - 1);
}

/// The instantiation section of the file whose path ends with `name`.
std::optional<std::uint32_t> section_of(llvm::StringRef name) {
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        if(view.path(i).ends_with(name)) {
            return view.instantiation_section_of(i);
        }
    }
    return std::nullopt;
}

const index::InstantiationShard& focus_shard() {
    const static index::InstantiationShard none;
    auto section = focus_section();
    return section ? view.instantiation_shard(*section) : none;
}

/// Every record of the focus file, in stored order.
std::vector<Resolved> resolved() {
    llvm::StringRef content = sources.all_files[src_path].content;
    std::vector<Resolved> records;
    focus_shard().for_each_record([&](const index::InstantiationRecord& record) {
        records.push_back({
            .text = content.slice(record.range.begin, record.range.end).str(),
            .kind = record.kind,
            .target = name_of(record.target),
            .from = record.from != 0 ? name_of(record.from) : "",
            .expansion = record.expansion,
        });
        return true;
    });
    return records;
}

std::vector<index::InstantiationShard::Key> keys() {
    std::vector<index::InstantiationShard::Key> found;
    focus_shard().for_each_key([&](const index::InstantiationShard::Key& key) {
        found.push_back(key);
        return true;
    });
    return found;
}

using enum index::InstantiationKind;

ZEST_CASE(DependentCallsResolved) {
    build(R"(
        struct A { void go(); int size; };
        template <typename T>
        int run(T t) {
            t.go();
            return t.size;
        }
        int use() { return run(A{}); }
    )");
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"go", Reference, "go"},
                              {"go", Call, "go", "run"},
                              {"size", Reference, "size"},
    });
    auto found = keys();
    ZASSERT(found.size() == 1u);
    ZEXPECT(name_of(found.front().pattern) == "run");
    ZEXPECT(found.front().resolution.has_value());
    ZEXPECT(!found.front().producer.has_value());
}

ZEST_CASE(FocusCompileCallers) {
    // An open file's compile walks its instantiations in a build of their
    // own, whose nodes the written code's callers know nothing of.
    build(R"(
        void leaf();
        void unrelated() { leaf(); }
        struct A { void go(); };
        template <typename T>
        void run(T t) { { t.go(); } }
        void use() { run(A{}); }
    )",
          {},
          CompilationKind::Content);
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"go", Reference, "go"},
                              {"go", Call, "go", "run"},
    });
}

ZEST_CASE(IncludedFragmentResolved) {
    add_file("body.inc", "t.go();");
    build(R"(
        struct A { void go(); };
        template <typename T>
        void run(T t) {
        #include "body.inc"
        }
        void use() { run(A{}); }
    )");
    // No key could stand for the fragment's records: every unit walks it.
    ZEXPECT(keys().empty());
    auto fragment = section_of("body.inc");
    ZASSERT(fragment);
    ZEXPECT(view.instantiation_shard(*fragment).resolutions().size() == 1u);
}

ZEST_CASE(HomeIsTheDefinition) {
    // A redeclaration after the definition is what the unit's lookup
    // meets; the instantiation's records stay with the definition's file.
    add_file("def.h", R"(
        struct A { void go(); };
        template <typename T>
        void run(T t) { t.go(); }
    )");
    add_file("fwd.h", "template <typename T> void run(T t);");
    build(R"(
        #include "def.h"
        #include "fwd.h"
        void use() { run(A{}); }
    )");
    auto definition = section_of("def.h");
    ZASSERT(definition);
    ZEXPECT(view.instantiation_shard(*definition).keys().size() == 1u);
}

ZEST_CASE(StandardLibraryUnwalked) {
    add_file("algo", R"(
        #pragma GCC system_header
        namespace std {
        template <typename T>
        void call(T t) { t.go(); }
        }
        #define __library_go(t) (t).go()
        #define LIBRARY_GO(t) (t).go()
    )");
    llvm::StringRef code = R"(
        #include "algo"
        struct A { void go(); };
        void use() {
            std::call(A{});
            __library_go(A{});
            LIBRARY_GO(A{});
        }
    )";
    // The template and the macro of a reserved name are the library's own;
    // `LIBRARY_GO`, a name a user may spell too, resolves as before.
    build(code);
    auto library = section_of("algo");
    ZASSERT(library);
    ZEXPECT(view.instantiation_shard(*library).keys().empty());
    ZEXPECT(view.instantiation_shard(*library).resolutions().size() == 1u);

    build(code, {.standard_library_instantiations = true});
    library = section_of("algo");
    ZASSERT(library);
    ZEXPECT(view.instantiation_shard(*library).keys().size() == 1u);
    ZEXPECT(view.instantiation_shard(*library).resolutions().size() == 3u);
}

ZEST_CASE(ExplicitInstantiationsWalked) {
    // An explicit instantiation declaration instantiates no initializer,
    // the definition does.
    llvm::StringRef code = R"(
        struct A { static const int v = 1; };
        template <typename T>
        int value = T::v;
        extern template int value<A>;
    )";
    build(code);
    ZEXPECT(keys().empty());

    build((code + "template int value<A>;").str());
    ZEXPECT(keys().size() == 1u);
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"v", Reference, "v"},
    });
}

ZEST_CASE(EachInstantiationItsResolution) {
    build(R"(
        struct A { void go(); };
        struct B { void go(); };
        template <typename T>
        void run(T t) { t.go(); }
        void use() {
            run(A{});
            run(B{});
        }
    )");
    ZEXPECT(focus_shard().resolutions().size() == 2u);
    ZEXPECT(keys().size() == 2u);
    // Both `go`s are named at the one place, each by its own resolution.
    ZEXPECT(resolved().size() == 4u);
}

ZEST_CASE(NothingResolvedKeepsTheKey) {
    build(R"(
        template <typename T>
        T same(T value) { return value; }
        int use() { return same(1); }
    )");
    ZEXPECT(resolved().empty());
    auto found = keys();
    ZASSERT(found.size() == 1u);
    ZEXPECT(!found.front().resolution);
}

ZEST_CASE(KnownKeysSkipped) {
    llvm::StringRef code = R"(
        struct A { void go(); };
        template <typename T>
        void run(T t) { t.go(); }
        void use() { run(A{}); }
    )";
    build(code);
    auto found = keys();
    ZASSERT(found.size() == 1u);
    auto key = found.front().key;

    std::uint64_t known[] = {key};
    build(code, {.known_instantiations = known});
    auto section = focus_section();
    ZASSERT(section);
    ZEXPECT(view.instantiation_blob(*section).empty());
    ZEXPECT(view.skipped_instantiations(*section) == llvm::ArrayRef<std::uint64_t>{key});
    // The written code still names the template's own entries.
    ZEXPECT(view.section_count() != 0u);
}

ZEST_CASE(DependentBaseResolved) {
    build(R"(
        struct A {};
        template <typename T>
        struct Derived : T {};
        Derived<A> value;
    )");
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"T", Base, "A", "Derived"},
    });
}

ZEST_CASE(AbbreviatedParameterDeduced) {
    build(R"(
        struct A {};
        void take(auto value) {}
        void use() { take(A{}); }
    )");
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"auto", Deduced, "A"},
                              {"value", TypeDefinition, "A", "value"},
    });
}

ZEST_CASE(PackParametersMapped) {
    build(R"(
        struct A {};
        struct B {};
        void sink(A);
        void sink(B);
        void take(auto... values) { (sink(values), ...); }
        void use() { take(A{}, B{}); }
    )");
    // Every element of the pack is the one parameter `values`, which the
    // written code already names: each deduces its own type, and none is a
    // parameter of its own.
    auto records = resolved();
    for(auto type: {"A", "B"}) {
        ZEXPECT(llvm::is_contained(records, Resolved{"auto", Deduced, type}));
    }
    ZEXPECT(
        llvm::none_of(records, [](const Resolved& record) { return record.target == "values"; }));
}

ZEST_CASE(MacroDeclaredAutoUnwritten) {
    build(R"(
        struct A {};
        struct Maker { A get(); };
        #define TAKE auto made = maker.get()
        template <typename T>
        void make(T maker) { TAKE; }
        void use() { make(Maker{}); }
    )");
    ZEXPECT(std::ranges::none_of(resolved(), [](const Resolved& record) {
        return record.kind == TypeDefinition;
    }));
}

ZEST_CASE(MacroBodyNamesResolved) {
    build(R"(
        #define WIDTH 3
        #define AREA (WIDTH * WIDTH)
        int area = AREA;
    )");
    ZEXPECT(resolved() == std::vector<Resolved>{
                              {"WIDTH", Reference, "WIDTH", "", true},
                              {"WIDTH", Reference, "WIDTH", "", true},
    });
    // An expansion has a resolution, no key.
    ZEXPECT(keys().empty());
}

ZEST_CASE(ForeignInternalUnkeyed) {
    add_file("run.h", R"(
        template <typename T>
        void run(T t) { t.go(); }
    )");
    build(R"(
        #include "run.h"
        namespace {
        struct A { void go(); };
        }
        void use() { run(A{}); }
    )");
    auto section = section_of("run.h");
    ZASSERT(section);
    const auto& shard = view.instantiation_shard(*section);
    // Only the units walking it list run.h among the files naming the
    // internal `go`: a skipped key could not stand for it.
    ZEXPECT(shard.keys().empty());
    ZEXPECT(shard.resolutions().size() == 1u);
}

ZEST_CASE(SignatureNamesItsParameter) {
    build(R"(
        struct V { int front(); };
        template <typename C>
        auto first(C& c) -> decltype(c.front());
        int head(V& v) { return first(v); }
        template <typename C>
        auto first(C& c) -> decltype(c.front()) { return c.front(); }
    )");
    // The specialization takes its parameter from the declaration: the
    // declaration's signature names its own `c`, the definition its own, as
    // the written code does.
    ZEXPECT(llvm::none_of(resolved(), [](const Resolved& record) { return record.target == "c"; }));
}

ZEST_CASE(WrappedSignatureNamesParameter) {
    build(R"(
        #define WRAP(...) __VA_ARGS__
        struct V { int front(); };
        WRAP(
            template <typename C>
            auto first(C& c) -> decltype(c.front());
            int head(V& v) { return first(v); }
            template <typename C>
            auto first(C& c) -> decltype(c.front()) { return c.front(); }
        )
    )");
    // One macro argument holds both declarations: their text, not the
    // invocation, tells them apart.
    ZEXPECT(llvm::none_of(resolved(), [](const Resolved& record) { return record.target == "c"; }));
}

ZEST_CASE(ForeignCallerUnkeyed) {
    add_file("box.h", R"(
        namespace {
        struct Box {
            template <typename T>
            void put(T t);
        };
        }
    )");
    add_file("put.h", R"(
        template <typename T>
        void Box::put(T t) { t.go(); }
    )");
    build(R"(
        #include "box.h"
        #include "put.h"
        struct Ext { void go(); };
        void use() { Box{}.put(Ext{}); }
    )");
    auto section = section_of("put.h");
    ZASSERT(section);
    const auto& shard = view.instantiation_shard(*section);
    // The call's target is external, but its caller is the internal `put`
    // box.h declares.
    ZEXPECT(shard.keys().empty());
    ZEXPECT(shard.resolutions().size() == 1u);
}

ZEST_CASE(LocalClassNamesHeader) {
    add_file("run.h", R"(
        template <typename T>
        void run(T t) { t.go(); }
    )");
    build(R"(
        #include "run.h"
        void use() {
            struct Local { void go() {} };
            run(Local{});
        }
    )");
    // A function's own class is named in the header the instantiation
    // resolves in: the table lists it with both files.
    std::optional<std::size_t> files;
    view.for_each_symbol([&](index::SymbolHash hash,
                             const index::SymbolView& identity,
                             llvm::ArrayRef<std::uint32_t> references) {
        if(identity.name == "go") {
            ZEXPECT(identity.scope == index::SymbolScope::FunctionLocal);
            files = references.size();
        }
        return true;
    });
    ZEXPECT(files == 2u);
}

ZEST_CASE(SymbolsNameTheirFiles) {
    build(R"(
        struct A { void go(); };
        template <typename T>
        void run(T t) { t.go(); }
        void use() { run(A{}); }
    )");
    auto focus_path = view.path_count() - 1;
    bool named = false;
    view.for_each_instantiation_file(
        [&](index::SymbolHash hash, llvm::ArrayRef<std::uint32_t> files) {
            if(name_of(hash) == "go") {
                named = files == llvm::ArrayRef<std::uint32_t>{focus_path};
            }
            return true;
        });
    ZEXPECT(named);
}

};  // ZEST_SUITE(InstantiationIndex)

}  // namespace

}  // namespace clice::testing
