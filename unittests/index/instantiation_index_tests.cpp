module;

#include "modules/prelude.h"

module clice;

import :index.instantiation_shard;
import :index.tu_index;
import :unittests.utils;
import :unittests.utils.tester;

namespace clice::testing {
namespace {

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
    view.iterate_symbols([&](index::SymbolHash hash,
                             const index::SymbolIdentity& identity,
                             llvm::ArrayRef<std::uint32_t> references) {
        if(identity.name == "go") {
            ZEXPECT(identity.scope == index::SymbolScope::FileLocal);
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
    view.iterate_instantiation_files(
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
