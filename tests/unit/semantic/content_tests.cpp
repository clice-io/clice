#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "semantic/content.h"
#include "support/logging.h"

#include "llvm/Support/Path.h"
#include "clang/AST/DeclCXX.h"

namespace clice::testing {

namespace {

struct File {
    llvm::StringRef name;
    llvm::StringRef content;
};

std::string hex(ContentHash hash) {
    return std::format("{}", hash);
}

/// One compiled TU with its content table, addressed by file and marker.
struct Compiled {
    Tester tester;
    ContentTable table;

    /// A fixture about broken input (an include the preprocessor cannot
    /// enter) compiles with errors on purpose.
    bool allow_errors = false;

    bool compile(llvm::ArrayRef<File> headers,
                 File main,
                 llvm::ArrayRef<llvm::StringRef> extra_args = {}) {
        for(auto& header: headers) {
            tester.add_file(header.name, header.content);
        }
        tester.add_main(main.name, main.content);
        tester.prepare();
        for(auto arg: extra_args) {
            tester.owned_args.insert(tester.owned_args.end() - 1, arg.str());
        }
        tester.params.arguments.clear();
        for(auto& arg: tester.owned_args) {
            tester.params.arguments.push_back(arg.c_str());
        }
        if(!tester.try_compile()) {
            return false;
        }
        // The AST builds through errors, and a mistyped fixture (`3_lit.v`
        // is one pp-number) would silently pin the wrong behavior.
        for(auto& diagnostic: tester.unit->diagnostics()) {
            if(diagnostic.id.level >= DiagnosticLevel::Error && !allow_errors) {
                LOG_ERROR("{}", diagnostic.message);
                return false;
            }
        }
        table = ContentTable::compute(*tester.unit);
        return true;
    }

    clang::FileID fid(llvm::StringRef file) {
        return tester.unit->file_id(TestVFS::path(file));
    }

    const ContentUnit& at(llvm::StringRef file, llvm::StringRef marker) {
        auto* found = table.find(fid(file), tester[file, marker]);
        if(!found) {
            LOG_FATAL("no unit at marker {} in {}", marker, file);
        }
        return *found;
    }

    std::uint32_t index(llvm::StringRef file, llvm::StringRef marker) {
        return static_cast<std::uint32_t>(&at(file, marker) - table.units.data());
    }

    std::string content(llvm::StringRef file, llvm::StringRef marker) {
        return hex(at(file, marker).content);
    }

    std::string own(llvm::StringRef file, llvm::StringRef marker) {
        return hex(at(file, marker).own);
    }

    std::string digest(llvm::StringRef file) {
        return hex(table.digests.lookup(fid(file)));
    }

    /// Whether the unit at `marker` has a direct edge to the unit at
    /// `dep_marker`.
    bool depends(llvm::StringRef file,
                 llvm::StringRef marker,
                 llvm::StringRef dep_file,
                 llvm::StringRef dep_marker) {
        return std::ranges::binary_search(at(file, marker).deps, index(dep_file, dep_marker));
    }

    /// How many inclusions of `name` have a digest.
    std::size_t digests_of(llvm::StringRef name) {
        return std::ranges::count_if(table.digests, [&](auto& entry) {
            return llvm::sys::path::filename(tester.unit->file_path(entry.first)) == name;
        });
    }

    /// Every byte of every file with units belongs to exactly one unit, and
    /// find() agrees at the boundaries.
    bool partitioned() {
        for(std::size_t i = 0; i < table.units.size(); i += 1) {
            auto& current = table.units[i];
            bool first = i == 0 || table.units[i - 1].fid != current.fid;
            bool last = i + 1 == table.units.size() || table.units[i + 1].fid != current.fid;
            auto size = tester.unit->file_content(current.fid).size();
            if(current.range.begin > current.range.end) {
                return false;
            }
            if(first ? current.range.begin != 0
                     : current.range.begin != table.units[i - 1].range.end) {
                return false;
            }
            if(last && current.range.end != size) {
                return false;
            }
            if(current.range.begin < current.range.end &&
               table.find(current.fid, current.range.begin) != &current) {
                return false;
            }
        }
        return true;
    }
};

TEST_SUITE(content) {

TEST_CASE(HeaderAcrossUnits) {
    File lib = {"lib.h", R"cpp(
#pragma once
namespace lib {
struct §(s)S { int §(m)value; };
inline int §(f)f(S s) { return s.value; }
template <typename T> T §(t)twice(T x) { return x + x; }
enum class §(e)Color { Red };
}
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({lib}, {"a.cpp", R"cpp(
#include "lib.h"
int §(ma)ma() { return 1; }
)cpp"}));
    Compiled b;
    ASSERT_TRUE(b.compile({lib}, {"b.cpp", R"cpp(
int §(x)x = 2;
#include "lib.h"
)cpp"}));

    for(auto marker: {"s", "f", "t", "e"}) {
        EXPECT_EQ(a.content("lib.h", marker), b.content("lib.h", marker));
    }
    EXPECT_EQ(&a.at("lib.h", "m"), &a.at("lib.h", "s"));
    EXPECT_EQ(a.digest("lib.h"), b.digest("lib.h"));
    EXPECT_EQ(a.at("a.cpp", "ma").decl->getDeclKindName(), llvm::StringRef("Function"));
    EXPECT_TRUE(a.partitioned());
    EXPECT_TRUE(b.partitioned());
}

TEST_CASE(IncludeOrder) {
    File x = {"x.h", "inline int §(x)x() { return 1; }\n"};
    File y = {"y.h", "inline int §(y)y() { return 2; }\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({x, y}, {"a.cpp", "#include \"x.h\"\n#include \"y.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({x, y}, {"b.cpp", "#include \"y.h\"\n#include \"x.h\"\n"}));

    EXPECT_EQ(a.content("x.h", "x"), b.content("x.h", "x"));
    EXPECT_EQ(a.content("y.h", "y"), b.content("y.h", "y"));
    EXPECT_EQ(a.digest("x.h"), b.digest("x.h"));
}

TEST_CASE(BodyChangePropagates) {
    File g1 = {"g.h", "inline int §(g)g() { return 1; }\n"};
    File g2 = {"g.h", "inline int §(g)g() { return 2; }\n"};
    File f = {"f.h", "#include \"g.h\"\ninline int §(f)f() { return g(); }\n"};
    File h = {"h.h", "inline int §(h)h() { return 3; }\n"};
    File main = {"main.cpp", "#include \"f.h\"\n#include \"h.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({g1, f, h}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({g2, f, h}, main));

    EXPECT_NE(a.content("g.h", "g"), b.content("g.h", "g"));
    EXPECT_NE(a.content("f.h", "f"), b.content("f.h", "f"));
    EXPECT_EQ(a.own("f.h", "f"), b.own("f.h", "f"));
    EXPECT_EQ(a.content("h.h", "h"), b.content("h.h", "h"));
    EXPECT_TRUE(a.depends("f.h", "f", "g.h", "g"));
    EXPECT_FALSE(a.depends("f.h", "f", "h.h", "h"));
}

TEST_CASE(SignatureChangePropagates) {
    File g1 = {"g.h", "inline int §(g)g(int) { return 1; }\n"};
    File g2 = {"g.h", "inline int §(g)g(long) { return 1; }\n"};
    File f = {"f.h", "#include \"g.h\"\ninline int §(f)f() { return g(1); }\n"};
    File main = {"main.cpp", "#include \"f.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({g1, f}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({g2, f}, main));

    EXPECT_NE(a.content("f.h", "f"), b.content("f.h", "f"));
}

TEST_CASE(MemberChangePropagates) {
    File s1 = {"s.h", "struct §(s)S { int a; };\n"};
    File s2 = {"s.h", "struct §(s)S { int a; int b; };\n"};
    File u = {"u.h", "#include \"s.h\"\ninline int §(u)use(S s) { return s.a; }\n"};
    File main = {"main.cpp", "#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({s1, u}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({s2, u}, main));

    EXPECT_NE(a.content("u.h", "u"), b.content("u.h", "u"));
    EXPECT_TRUE(a.depends("u.h", "u", "s.h", "s"));
}

TEST_CASE(OutOfLineDefinition) {
    File s = {"s.h", "#pragma once\nstruct §(s)S { int §(m)get() const; };\n"};
    File d1 = {"d.h", "#include \"s.h\"\ninline int §(d)S::get() const { return 1; }\n"};
    File d2 = {"d.h", "#include \"s.h\"\ninline int §(d)S::get() const { return 2; }\n"};
    File c = {"c.h", "#include \"s.h\"\ninline int §(c)call(const S& s) { return s.get(); }\n"};
    File main = {"main.cpp", "#include \"d.h\"\n#include \"c.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({s, d1, c}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({s, d2, c}, main));

    EXPECT_TRUE(a.depends("c.h", "c", "d.h", "d"));
    EXPECT_TRUE(a.depends("c.h", "c", "s.h", "s"));
    EXPECT_TRUE(a.depends("s.h", "s", "d.h", "d"));
    EXPECT_TRUE(a.depends("d.h", "d", "s.h", "s"));
    EXPECT_NE(a.content("c.h", "c"), b.content("c.h", "c"));
    EXPECT_EQ(a.own("c.h", "c"), b.own("c.h", "c"));
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(ImplicitConstructionDestruction) {
    File s = {"s.h", "#pragma once\nstruct §(s)S { S(); ~S(); };\n"};
    File d1 = {"d.h", "#include \"s.h\"\ninline §(ctor)S::S() {}\ninline §(dtor)S::~S() {}\n"};
    File d2 = {"d.h",
               "#include \"s.h\"\ninline §(ctor)S::S() {}\ninline §(dtor)S::~S() { int x = 0; }\n"};
    File u = {"u.h", "#include \"s.h\"\ninline void §(u)use() { S s; }\n"};
    File main = {"main.cpp", "#include \"d.h\"\n#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({s, d1, u}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({s, d2, u}, main));

    EXPECT_TRUE(a.depends("u.h", "u", "d.h", "ctor"));
    EXPECT_TRUE(a.depends("u.h", "u", "d.h", "dtor"));
    EXPECT_NE(a.content("u.h", "u"), b.content("u.h", "u"));
}

TEST_CASE(GeneratedCalls) {
    File decls = {"decls.h", R"cpp(
#pragma once
struct §(s)S { operator int() const; explicit operator bool() const; };
struct §(d)D { ~D(); };
struct §(it)It { int v; };
struct §(range)Range { It begin() const; It end() const; };
bool operator!=(It, It);
It& operator++(It&);
int operator*(It);
struct §(lit)Lit { int v; };
Lit operator""_lit(unsigned long long);
void* operator new(decltype(sizeof(0)), int*);
void operator delete(void*, int*) noexcept;
D make();
void release(int*);
)cpp"};
    File defs = {"defs.h", R"cpp(
#include "decls.h"
inline §(conv)S::operator int() const { return 1; }
inline §(boolconv)S::operator bool() const { return true; }
inline §(ddtor)D::~D() {}
inline It §(rbegin)Range::begin() const { return {0}; }
inline It §(rend)Range::end() const { return {3}; }
inline bool §(neq)operator!=(It a, It b) { return a.v != b.v; }
inline It& §(inc)operator++(It& i) { i.v += 1; return i; }
inline int §(deref)operator*(It i) { return i.v; }
inline Lit §(udl)operator""_lit(unsigned long long v) { return {static_cast<int>(v)}; }
inline void* §(pnew)operator new(decltype(sizeof(0)), int* p) { return p; }
inline void §(pdelete)operator delete(void*, int*) noexcept {}
inline D §(make)make() { return {}; }
inline void §(release)release(int*) {}
)cpp"};
    File users = {"users.h", R"cpp(
#include "decls.h"
inline int §(conv_user)to_int(const S& s) { int i = s; return i; }
inline bool §(bool_user)truthy(const S& s) { if(s) return true; return false; }
inline void §(del)destroy(D* p) { delete p; }
inline int §(loop)sum(const Range& r) { int s = 0; for(int x: r) s += x; return s; }
inline int §(udl_user)use_lit() { return (3_lit).v; }
inline S* §(placement)place(int* p) { return new(p) S; }
inline void §(temporary)temporary() { make(); }
inline void §(array)array() { D items[2]; }
inline void §(cleanup)guarded() { int x __attribute__((cleanup(release))) = 0; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({decls, defs, users},
                          {"main.cpp", "#include \"defs.h\"\n#include \"users.h\"\n"},
                          {"-fexceptions", "-fcxx-exceptions"}));

    EXPECT_TRUE(a.depends("users.h", "conv_user", "defs.h", "conv"));
    EXPECT_FALSE(a.depends("users.h", "conv_user", "defs.h", "boolconv"));
    EXPECT_TRUE(a.depends("users.h", "bool_user", "defs.h", "boolconv"));
    EXPECT_TRUE(a.depends("users.h", "del", "defs.h", "ddtor"));
    for(auto marker: {"rbegin", "rend", "neq", "inc", "deref"}) {
        EXPECT_TRUE(a.depends("users.h", "loop", "defs.h", marker));
    }
    EXPECT_TRUE(a.depends("users.h", "udl_user", "defs.h", "udl"));
    EXPECT_FALSE(a.depends("users.h", "udl_user", "defs.h", "conv"));
    EXPECT_TRUE(a.depends("users.h", "placement", "defs.h", "pnew"));
    EXPECT_TRUE(a.depends("users.h", "placement", "defs.h", "pdelete"));
    EXPECT_TRUE(a.depends("users.h", "temporary", "defs.h", "ddtor"));
    EXPECT_TRUE(a.depends("users.h", "array", "defs.h", "ddtor"));
    EXPECT_TRUE(a.depends("users.h", "cleanup", "defs.h", "release"));
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(InheritedConstructorClosure) {
    File decls = {"decls.h", R"cpp(
#pragma once
struct Base { Base(int); };
struct Derived : Base { using Base::Base; };
)cpp"};
    File defs1 = {"defs.h", "#include \"decls.h\"\ninline Base::Base(int) {}\n"};
    File defs2 = {"defs.h", "#include \"decls.h\"\ninline Base::Base(int x) { (void)x; }\n"};
    File user = {"user.h", "#include \"decls.h\"\ninline void §(inh)make() { Derived d(1); }\n"};
    File main = {"main.cpp", "#include \"defs.h\"\n#include \"user.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({decls, defs1, user}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({decls, defs2, user}, main));

    EXPECT_NE(a.content("user.h", "inh"), b.content("user.h", "inh"));
}

TEST_CASE(DefaultInitializerClosure) {
    File decls = {"decls.h", "#pragma once\nint helper();\nstruct §(s)S { int x = helper(); };\n"};
    File defs1 = {"defs.h", "#include \"decls.h\"\ninline int §(helper)helper() { return 1; }\n"};
    File defs2 = {"defs.h", "#include \"decls.h\"\ninline int §(helper)helper() { return 2; }\n"};
    File user = {"user.h", "#include \"decls.h\"\ninline S §(user)make_s() { return S{}; }\n"};
    File main = {"main.cpp", "#include \"defs.h\"\n#include \"user.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({decls, defs1, user}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({decls, defs2, user}, main));

    EXPECT_TRUE(a.depends("decls.h", "s", "defs.h", "helper"));
    EXPECT_NE(a.content("user.h", "user"), b.content("user.h", "user"));
}

TEST_CASE(Coroutines) {
    File decls = {"decls.h", R"cpp(
#pragma once
namespace std {
template <typename Ret, typename... Args> struct coroutine_traits {
    using promise_type = typename Ret::promise_type;
};
template <typename Promise = void> struct coroutine_handle {
    static coroutine_handle from_address(void*) noexcept;
    static coroutine_handle from_promise(Promise&);
    constexpr void* address() const noexcept;
};
template <> struct coroutine_handle<void> {
    template <typename Promise> coroutine_handle(coroutine_handle<Promise>) noexcept;
    static coroutine_handle from_address(void*);
    constexpr void* address() const noexcept;
};
struct suspend_always {
    bool await_ready() noexcept;
    void await_suspend(coroutine_handle<>) noexcept;
    void await_resume() noexcept;
};
}
struct Task {
    struct promise_type {
        Task get_return_object();
        std::suspend_always initial_suspend();
        std::suspend_always final_suspend() noexcept;
        void return_void();
        void unhandled_exception();
    };
};
)cpp"};
    File defs = {"defs.h", R"cpp(
#include "decls.h"
inline bool §(ready)std::suspend_always::await_ready() noexcept { return false; }
inline void §(suspend)std::suspend_always::await_suspend(coroutine_handle<>) noexcept {}
inline void §(resume)std::suspend_always::await_resume() noexcept {}
inline Task §(object)Task::promise_type::get_return_object() { return {}; }
inline std::suspend_always §(initial)Task::promise_type::initial_suspend() { return {}; }
inline std::suspend_always §(final)Task::promise_type::final_suspend() noexcept { return {}; }
inline void §(return_void)Task::promise_type::return_void() {}
inline void §(unhandled)Task::promise_type::unhandled_exception() {}
)cpp"};
    File user = {"user.h", R"cpp(
#include "decls.h"
inline Task §(coro)coro() { co_await std::suspend_always{}; co_return; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({decls, defs, user}, {"main.cpp", "#include \"defs.h\"\n#include \"user.h\"\n"}));

    for(auto marker: {"ready", "suspend", "resume", "object", "initial", "final", "return_void"}) {
        EXPECT_TRUE(a.depends("user.h", "coro", "defs.h", marker));
    }
}

TEST_CASE(StructuredBinding) {
    File decls = {"decls.h", R"cpp(
#pragma once
struct §(s)S { int a; };
template <decltype(sizeof(0)) I> int get(const S& s);
namespace std {
template <typename T> struct tuple_size;
template <> struct tuple_size<S> { static constexpr decltype(sizeof(0)) value = 1; };
template <decltype(sizeof(0)) I, typename T> struct tuple_element;
template <> struct §(element)tuple_element<0, S> { using type = int; };
}
)cpp"};
    File defs = {"defs.h", R"cpp(
#include "decls.h"
template <decltype(sizeof(0)) I> int §(get)get(const S& s) { return s.a; }
)cpp"};
    File user = {"user.h",
                 "#include \"decls.h\"\ninline int §(f)f(S s) { auto [x] = s; return x; }\n"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({decls, defs, user}, {"main.cpp", "#include \"defs.h\"\n#include \"user.h\"\n"}));

    EXPECT_TRUE(a.depends("user.h", "f", "defs.h", "get"));
    EXPECT_TRUE(a.depends("user.h", "f", "decls.h", "element"));
}

TEST_CASE(MacroDefinitions) {
    File u = {
        "u.h",
        "#include \"m.h\"\ninline int §(u)use_n() { return N; }\ninline int §(v)plain() { return 5; }\n"};
    File main = {"main.cpp", "#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile(
        {
            {"m.h", "#define N 3\n"},
            u
    },
        main));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {
            {"m.h", "#define N 4\n"},
            u
    },
        main));
    Compiled c;
    ASSERT_TRUE(c.compile(
        {
            {"m.h", "#define N 3 // NOLINT\n"},
            u
    },
        main));

    EXPECT_NE(a.content("u.h", "u"), b.content("u.h", "u"));
    EXPECT_NE(a.content("u.h", "u"), c.content("u.h", "u"));
    EXPECT_EQ(a.content("u.h", "v"), b.content("u.h", "v"));
    EXPECT_EQ(a.content("u.h", "v"), c.content("u.h", "v"));

    // A chain: the outer macro expands to the inner; both definitions count.
    File chain = {"u.h", "#include \"m.h\"\ninline int §(u)use_a() { return A; }\n"};
    Compiled outer;
    ASSERT_TRUE(outer.compile(
        {
            {"m.h", "#define B 3\n#define A B\n"},
            chain
    },
        main));
    Compiled inner;
    ASSERT_TRUE(inner.compile(
        {
            {"m.h", "#define B 3 // NOLINT\n#define A B\n"},
            chain
    },
        main));
    EXPECT_NE(outer.content("u.h", "u"), inner.content("u.h", "u"));
}

TEST_CASE(ArgumentMacroDefinition) {
    File u = {
        "u.h",
        "#include \"id.h\"\ninline int §(g)get() { return 1; }\ninline int §(idu)use_id() { return ID(get()); }\n"};
    File main = {"main.cpp", "#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile(
        {
            {"id.h", "#define ID(x) x\n"},
            u
    },
        main));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {
            {"id.h", "#define ID(x) x // NOLINT\n"},
            u
    },
        main));
    Compiled c;
    ASSERT_TRUE(c.compile(
        {
            {"id.h", "// NOLINTNEXTLINE\n#define ID(x) x\n"},
            u
    },
        main));

    EXPECT_NE(a.content("u.h", "idu"), b.content("u.h", "idu"));
    EXPECT_NE(a.content("u.h", "idu"), c.content("u.h", "idu"));
    EXPECT_EQ(a.content("u.h", "g"), b.content("u.h", "g"));
}

TEST_CASE(PreprocessorVariants) {
    File v = {"v.h", R"cpp(
#pragma once
inline int §(cfg)configured() {
#ifdef FLAG
    return 1;
#else
    return 0;
#endif
}
inline int §(plain)plain() { return 2; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({v}, {"a.cpp", "#define FLAG\n#include \"v.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({v}, {"b.cpp", "#include \"v.h\"\n"}));

    EXPECT_NE(a.content("v.h", "cfg"), b.content("v.h", "cfg"));
    EXPECT_EQ(a.content("v.h", "plain"), b.content("v.h", "plain"));
    EXPECT_NE(a.digest("v.h"), b.digest("v.h"));
}

TEST_CASE(Cycles) {
    File mutual = {"c.h", R"cpp(
struct B;
struct §(sa)A { B* b; };
struct §(sb)B { A* a; };
inline int g(int n);
inline int §(f)f(int n) { return n ? g(n - 1) : 0; }
inline int §(g)g(int n) { return n ? f(n - 1) : 1; }
)cpp"};
    File changed = {"c.h", R"cpp(
struct B;
struct §(sa)A { B* b; };
struct §(sb)B { A* a; };
inline int g(int n);
inline int §(f)f(int n) { return n ? g(n - 1) : 0; }
inline int §(g)g(int n) { return n ? f(n - 1) : 2; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({mutual}, {"a.cpp", "#include \"c.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({mutual}, {"b.cpp", "int x;\n#include \"c.h\"\n"}));
    Compiled c;
    ASSERT_TRUE(c.compile({changed}, {"c.cpp", "#include \"c.h\"\n"}));

    for(auto marker: {"sa", "sb", "f", "g"}) {
        EXPECT_EQ(a.content("c.h", marker), b.content("c.h", marker));
    }
    EXPECT_NE(a.content("c.h", "f"), c.content("c.h", "f"));
    EXPECT_NE(a.content("c.h", "g"), c.content("c.h", "g"));
    EXPECT_EQ(a.content("c.h", "sa"), c.content("c.h", "sa"));
    EXPECT_EQ(a.content("c.h", "sb"), c.content("c.h", "sb"));
    EXPECT_NE(a.content("c.h", "f"), a.content("c.h", "g"));
    EXPECT_TRUE(a.depends("c.h", "f", "c.h", "g"));
    EXPECT_TRUE(a.depends("c.h", "g", "c.h", "f"));
    EXPECT_TRUE(a.depends("c.h", "sa", "c.h", "sb"));
    EXPECT_TRUE(a.depends("c.h", "sb", "c.h", "sa"));
}

TEST_CASE(Diamond) {
    File a1 = {"a.h", "#pragma once\ninline int §(A)a() { return 1; }\n"};
    File a2 = {"a.h", "#pragma once\ninline int §(A)a() { return 2; }\n"};
    File b = {"b.h", "#pragma once\n#include \"a.h\"\ninline int §(B)b() { return a(); }\n"};
    File c = {"c.h", "#pragma once\n#include \"a.h\"\ninline int §(C)c() { return a() + 1; }\n"};
    File d = {"d.h",
              "#include \"b.h\"\n#include \"c.h\"\ninline int §(D)d() { return b() + c(); }\n"};
    File main = {"main.cpp", "#include \"d.h\"\n"};
    Compiled first;
    ASSERT_TRUE(first.compile({a1, b, c, d}, main));
    Compiled second;
    ASSERT_TRUE(second.compile({a2, b, c, d}, main));

    for(auto [file, marker]: {
            std::pair{"a.h", "A"},
            {"b.h", "B"},
            {"c.h", "C"},
            {"d.h", "D"}
    }) {
        EXPECT_NE(first.content(file, marker), second.content(file, marker));
    }
    EXPECT_EQ(first.own("d.h", "D"), second.own("d.h", "D"));
}

TEST_CASE(UnitBoundaries) {
    File layout = {"l.h", R"cpp(
int §(a)a, §(b)b;
struct §(s)S {} §(sv)sv;
int §(p)p; int §(q)q; // NOLINT
// NOLINTNEXTLINE
int §(nl)next_line;
#define MULTI(x) \
    int x##1, \
        x##2
MULTI(
    §(mm)m
)§(semi);
int §(tail)tail; // NOLINT(foo)
// NOLINTBEGIN
int §(inside)inside;
// NOLINTEND
)cpp"};
    File main = {"main.cpp", "#include \"l.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({layout}, main));

    EXPECT_EQ(&a.at("l.h", "a"), &a.at("l.h", "b"));
    EXPECT_EQ(a.at("l.h", "a").nodes.size(), 2u);
    EXPECT_EQ(&a.at("l.h", "s"), &a.at("l.h", "sv"));
    EXPECT_NE(&a.at("l.h", "p"), &a.at("l.h", "q"));
    EXPECT_EQ(&a.at("l.h", "mm"), &a.at("l.h", "semi"));
    EXPECT_NE(&a.at("l.h", "nl"), &a.at("l.h", "mm"));
    EXPECT_NE(&a.at("l.h", "mm"), &a.at("l.h", "tail"));
    EXPECT_TRUE(a.partitioned());

    // The shared line's NOLINT counts for both units on it; the NOLINTNEXTLINE
    // above a unit and the marker after it count for that unit.
    File markers = {"l.h", R"cpp(
int §(a)a, §(b)b;
struct §(s)S {} §(sv)sv;
int §(p)p; int §(q)q;
// NOLINTNEXTLINE(bar)
int §(nl)next_line;
#define MULTI(x) \
    int x##1, \
        x##2
MULTI(
    §(mm)m
)§(semi);
int §(tail)tail; // NOLINT(baz)
// NOLINTBEGIN
int §(inside)inside;
// NOLINTEND
)cpp"};
    Compiled b;
    ASSERT_TRUE(b.compile({markers}, main));
    EXPECT_NE(a.own("l.h", "p"), b.own("l.h", "p"));
    EXPECT_NE(a.own("l.h", "q"), b.own("l.h", "q"));
    EXPECT_NE(a.own("l.h", "nl"), b.own("l.h", "nl"));
    EXPECT_NE(a.own("l.h", "tail"), b.own("l.h", "tail"));
    EXPECT_EQ(a.own("l.h", "mm"), b.own("l.h", "mm"));

    // The BEGIN/END sequence of the file counts for every unit in it.
    File unclosed = {"l.h", R"cpp(
int §(a)a, §(b)b;
struct §(s)S {} §(sv)sv;
int §(p)p; int §(q)q; // NOLINT
// NOLINTNEXTLINE
int §(nl)next_line;
#define MULTI(x) \
    int x##1, \
        x##2
MULTI(
    §(mm)m
)§(semi);
int §(tail)tail; // NOLINT(foo)
// NOLINTBEGIN
int §(inside)inside;
)cpp"};
    Compiled c;
    ASSERT_TRUE(c.compile({unclosed}, main));
    EXPECT_NE(a.own("l.h", "mm"), c.own("l.h", "mm"));
}

TEST_CASE(TriviaBetweenUnits) {
    File h1 = {
        "h.h",
        "inline int §(first)first() { return 1; }\n// note\n// more\ninline int §(second)second() { return 2; }\n"};
    File h2 = {
        "h.h",
        "inline int §(first)first() { return 1; }\n// note\n// changed\ninline int §(second)second() { return 2; }\n"};
    File main = {"main.cpp", "#include \"h.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({h1}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({h2}, main));

    EXPECT_EQ(a.own("h.h", "first"), b.own("h.h", "first"));
    EXPECT_NE(a.own("h.h", "second"), b.own("h.h", "second"));
}

TEST_CASE(CommentChanges) {
    File h1 = {"h.h", "// note\ninline int §(f)f() { return 1; }\n"};
    File h2 = {"h.h", "// other note\ninline int §(f)f() { return 1; }\n"};
    File u = {"u.h", "#include \"h.h\"\ninline int §(u)use() { return f(); }\n"};
    File main = {"main.cpp", "#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({h1, u}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({h2, u}, main));

    EXPECT_NE(a.own("h.h", "f"), b.own("h.h", "f"));
    EXPECT_NE(a.content("u.h", "u"), b.content("u.h", "u"));
}

TEST_CASE(TemplateFamily) {
    File t = {"t.h", R"cpp(
#pragma once
template <typename T> struct §(tpl)Tpl { T v; };
template <> struct §(spec)Tpl<char> { int c; };
template <typename T> struct §(partial)Tpl<T*> { T* p; };
template <typename T> struct §(other)Other { T v; };
)cpp"};
    File u = {"u.h", "#include \"t.h\"\ninline int §(u)use(Tpl<int> t) { return t.v; }\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({t, u}, {"main.cpp", "#include \"u.h\"\n"}));

    EXPECT_TRUE(a.depends("u.h", "u", "t.h", "tpl"));
    EXPECT_TRUE(a.depends("u.h", "u", "t.h", "spec"));
    EXPECT_TRUE(a.depends("u.h", "u", "t.h", "partial"));
    EXPECT_FALSE(a.depends("u.h", "u", "t.h", "other"));
}

TEST_CASE(InstantiationSet) {
    File f = {"f.h", R"cpp(
#pragma once
template <typename T> int §(f)convert(T x) { int i = x; return i; }
inline int §(indep)indep() { return 0; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({f}, {"a.cpp", "#include \"f.h\"\nint r = convert(1);\n"}));
    Compiled b;
    ASSERT_TRUE(
        b.compile({f}, {"b.cpp", "#include \"f.h\"\nint r = convert(1) + convert(1.5);\n"}));
    Compiled c;
    ASSERT_TRUE(c.compile(
        {f},
        {"c.cpp",
         "#include \"f.h\"\nint r = convert(1);\nusing P = decltype(convert<double>)*;\n"}));

    EXPECT_NE(a.content("f.h", "f"), b.content("f.h", "f"));
    EXPECT_EQ(a.content("f.h", "f"), c.content("f.h", "f"));
    EXPECT_EQ(a.content("f.h", "indep"), b.content("f.h", "indep"));
}

TEST_CASE(LazyDefaultMaterialization) {
    File f = {"f.h", R"cpp(
#pragma once
template <typename T> int §(f)defaulted(int x = T(1.5)) { return x; }
template <typename T> struct §(s)S { int x = T(1.5); };
)cpp"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({f},
                  {"a.cpp", "#include \"f.h\"\nint r = defaulted<double>();\nS<double> s{};\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {f},
        {"b.cpp",
         "#include \"f.h\"\nint r = defaulted<double>(1);\nint n = sizeof(S<double>);\n"}));

    EXPECT_NE(a.content("f.h", "f"), b.content("f.h", "f"));
    EXPECT_NE(a.content("f.h", "s"), b.content("f.h", "s"));
}

TEST_CASE(DeclaredDefaultArgument) {
    File f = {"f.h", "#pragma once\ntemplate <typename T> int §(f)declared(int x = T(1.5));\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({f}, {"a.cpp", "#include \"f.h\"\nint r = declared<double>();\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({f}, {"b.cpp", "#include \"f.h\"\nint r = declared<double>(1);\n"}));

    EXPECT_NE(a.content("f.h", "f"), b.content("f.h", "f"));
}

TEST_CASE(ManyFieldsMaterialization) {
    std::string header = "#pragma once\ntemplate <typename T> struct §(s)S { int first = T(1.5);";
    for(int i = 0; i < 64; i += 1) {
        header += std::format(" int f{};", i);
    }
    header += " };\n";
    File s = {"s.h", header};
    Compiled a;
    ASSERT_TRUE(a.compile({s}, {"a.cpp", "#include \"s.h\"\nS<double> s{};\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({s}, {"b.cpp", "#include \"s.h\"\nint n = sizeof(S<double>);\n"}));

    EXPECT_NE(a.content("s.h", "s"), b.content("s.h", "s"));
}

TEST_CASE(GenericLambda) {
    File l = {
        "l.h",
        "#pragma once\ninline auto §(convert)convert = [](auto x) { int i = x; return i; };\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({l}, {"a.cpp", "#include \"l.h\"\nint r = convert(1);\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({l}, {"b.cpp", "#include \"l.h\"\nint r = convert(1.5);\n"}));
    Compiled c;
    ASSERT_TRUE(c.compile({l}, {"c.cpp", "#include \"l.h\"\nint r = convert(1) + 1;\n"}));

    EXPECT_NE(a.content("l.h", "convert"), b.content("l.h", "convert"));
    EXPECT_EQ(a.content("l.h", "convert"), c.content("l.h", "convert"));
}

TEST_CASE(InstantiationDependencies) {
    File s = {"s.h",
              "#pragma once\nstruct §(s)S {};\ninline int §(inspect)inspect(S) { return 1; }\n"};
    File call = {
        "call.h",
        "#pragma once\ntemplate <typename T> int §(call)call(T x) { return inspect(x); }\n"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({s, call},
                  {"main.cpp", "#include \"s.h\"\n#include \"call.h\"\nint r = call(S{});\n"}));

    EXPECT_TRUE(a.depends("call.h", "call", "s.h", "inspect"));
}

TEST_CASE(TemplateArguments) {
    File s = {"s.h", "#pragma once\nstruct §(s)S { int a; };\n"};
    File t = {"t.h", "#pragma once\ntemplate <typename T> int §(f)f() { return 1; }\n"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({s, t}, {"main.cpp", "#include \"s.h\"\n#include \"t.h\"\nint r = f<S*>();\n"}));

    EXPECT_TRUE(a.depends("t.h", "f", "s.h", "s"));
}

TEST_CASE(ExplicitInstantiation) {
    File v = {"v.h", R"cpp(
#pragma once
template <typename T> inline int §(v)v = T(1.5);
template <typename T> int §(f)f(T x) { int i = x; return i; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile(
        {v},
        {"a.cpp", "#include \"v.h\"\ntemplate int v<double>;\ntemplate int f<double>(double);\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {v},
        {"b.cpp",
         "#include \"v.h\"\nextern template int v<double>;\nextern template int f<double>(double);\n"}));

    EXPECT_NE(a.content("v.h", "v"), b.content("v.h", "v"));
    EXPECT_NE(a.content("v.h", "f"), b.content("v.h", "f"));
}

TEST_CASE(ExplicitInstantiationSpan) {
    File h = {"h.h", R"cpp(
template <typename T> int §(fwd)f(T);
int §(unrelated)unrelated;
template <typename T> int §(def)f(T x) { int i = x; return i; }
)cpp"};
    Compiled a;
    ASSERT_TRUE(a.compile({h}, {"main.cpp", "#include \"h.h\"\ntemplate int f<int>(int);\n"}));

    EXPECT_NE(&a.at("h.h", "fwd"), &a.at("h.h", "unrelated"));
    EXPECT_NE(&a.at("h.h", "unrelated"), &a.at("h.h", "def"));
    EXPECT_EQ(a.at("h.h", "unrelated").nodes.size(), 1u);
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(SplitTemplateDeclaration) {
    File fwd = {"fwd.h",
                "#pragma once\ntemplate <typename T> struct §(fwd)S;\nint §(after)after;\n"};
    File def = {
        "def.h",
        "#pragma once\n#include \"fwd.h\"\ntemplate <typename T> struct §(def)S { T v; };\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({fwd, def}, {"main.cpp", "#include \"def.h\"\nS<int> s;\n"}));

    EXPECT_NE(&a.at("fwd.h", "fwd"), &a.at("fwd.h", "after"));
    EXPECT_EQ(a.at("fwd.h", "fwd").fragments.size(), 0u);
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(SelectedOverload) {
    File ns = {"ns.h", R"cpp(
#pragma once
namespace A { inline int f() { return 1; } }
namespace B { inline double f() { return 1.5; } }
)cpp"};
    File u = {"u.h", R"cpp(
inline int §(u)use() {
    (void)&A::f;
    (void)&B::f;
    return f();
}
)cpp"};
    Compiled a;
    ASSERT_TRUE(
        a.compile({ns, u}, {"a.cpp", "#include \"ns.h\"\nusing namespace A;\n#include \"u.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(
        b.compile({ns, u}, {"b.cpp", "#include \"ns.h\"\nusing namespace B;\n#include \"u.h\"\n"}));

    EXPECT_NE(a.own("u.h", "u"), b.own("u.h", "u"));
}

TEST_CASE(Fragments) {
    File f = {"f.h", "inline int §(f)f() {\n#include \"body.inc\"\n}\n"};
    File main = {"main.cpp", "#include \"f.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile(
        {
            {"body.inc", "return 1;\n"},
            f
    },
        main));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {
            {"body.inc", "return 2;\n"},
            f
    },
        main));

    EXPECT_EQ(a.at("f.h", "f").fragments.size(), 1u);
    EXPECT_NE(a.own("f.h", "f"), b.own("f.h", "f"));
    EXPECT_TRUE(a.partitioned());

    // A fragment's own includes are fragments too.
    File nested = {"body.inc", "int x = 1;\n#include \"inner.inc\"\n"};
    Compiled outer;
    ASSERT_TRUE(outer.compile(
        {
            nested,
            {"inner.inc", "return x;\n"},
            f
    },
        main));
    Compiled inner;
    ASSERT_TRUE(inner.compile(
        {
            nested,
            {"inner.inc", "return x + 1;\n"},
            f
    },
        main));
    EXPECT_EQ(outer.at("f.h", "f").fragments.size(), 2u);
    EXPECT_NE(outer.own("f.h", "f"), inner.own("f.h", "f"));

    // A self-include the guard skips is no fragment.
    Compiled self;
    ASSERT_TRUE(self.compile(
        {
            {"s.h", "#pragma once\nstruct §(s)S {\n#include \"s.h\"\n};\n"}
    },
        {"main.cpp", "#include \"s.h\"\n"}));
    EXPECT_EQ(self.at("s.h", "s").fragments.size(), 0u);

    // A fragment resolved through a system include directory.
    File system = {"f.h", "inline int §(f)f() {\n#include <body.inc>\n}\n"};
    File body = {"sys/body.inc", "return 1;\n"};
    auto dir = TestVFS::path("sys");
    Compiled user_dir;
    ASSERT_TRUE(user_dir.compile({body, system}, main, {"-I", dir}));
    Compiled system_dir;
    ASSERT_TRUE(system_dir.compile({body, system}, main, {"-isystem", dir}));
    EXPECT_NE(user_dir.own("f.h", "f"), system_dir.own("f.h", "f"));
}

TEST_CASE(MissingInclude) {
    File f = {"f.h", "inline int §(f)f() {\n#include \"missing.inc\"\n}\nint §(after)after;\n"};
    Compiled broken;
    broken.allow_errors = true;
    ASSERT_TRUE(broken.compile({f}, {"main.cpp", "#include \"f.h\"\n"}));

    EXPECT_EQ(broken.at("f.h", "f").fragments.size(), 0u);
    EXPECT_NE(&broken.at("f.h", "f"), &broken.at("f.h", "after"));
    EXPECT_TRUE(broken.partitioned());
}

TEST_CASE(WrappedInclude) {
    File c1 = {"cdecls.h", "int §(cf)cf(int);\n"};
    File c2 = {"cdecls.h", "int §(cf)cf(long);\n"};
    File wrapper = {"wrapper.h", "#pragma once\n§(ext)extern \"C\" {\n#include \"cdecls.h\"\n}\n"};
    File user = {"user.h", "#include \"wrapper.h\"\ninline int §(user)use() { return cf(1); }\n"};
    File main = {"main.cpp", "#include \"user.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({c1, wrapper, user}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({c2, wrapper, user}, main));

    EXPECT_EQ(a.at("wrapper.h", "ext").decl->getDeclKindName(), llvm::StringRef("LinkageSpec"));
    EXPECT_EQ(a.at("wrapper.h", "ext").fragments.size(), 0u);
    EXPECT_EQ(a.digests_of("cdecls.h"), 1u);
    EXPECT_TRUE(a.depends("user.h", "user", "cdecls.h", "cf"));
    EXPECT_FALSE(a.depends("user.h", "user", "wrapper.h", "ext"));
    EXPECT_EQ(a.own("wrapper.h", "ext"), b.own("wrapper.h", "ext"));
    EXPECT_NE(a.own("cdecls.h", "cf"), b.own("cdecls.h", "cf"));
    EXPECT_NE(a.content("user.h", "user"), b.content("user.h", "user"));
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(RepeatedInclusion) {
    File twice = {"twice.inc", "int NAME;\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({twice}, {"main.cpp", R"cpp(
#define NAME first
#include "twice.inc"
#undef NAME
#define NAME second
#include "twice.inc"
)cpp"}));

    EXPECT_EQ(a.digests_of("twice.inc"), 2u);
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(PragmaPack) {
    File p = {"p.h", "struct §(s)S { char c; int i; };\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({p}, {"a.cpp", "#pragma pack(1)\n#include \"p.h\"\n#pragma pack()\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({p}, {"b.cpp", "#pragma pack(2)\n#include \"p.h\"\n#pragma pack()\n"}));

    EXPECT_NE(a.own("p.h", "s"), b.own("p.h", "s"));
}

TEST_CASE(PragmaAttribute) {
    File h = {"h.h", "inline void §(f)f(int x) {}\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({h}, {"a.cpp", "#include \"h.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({h}, {"b.cpp", R"cpp(
#pragma clang attribute push (__attribute__((noinline)), apply_to = function)
#include "h.h"
#pragma clang attribute pop
)cpp"}));

    EXPECT_NE(a.own("h.h", "f"), b.own("h.h", "f"));
}

TEST_CASE(CompileContext) {
    File h = {"h.h", "inline long §(f)f() { return 1; }\n"};
    File main = {"main.cpp", "#include \"h.h\"\n"};
    Compiled a;
    a.tester.triple = "x86_64-unknown-linux-gnu";
    ASSERT_TRUE(a.compile({h}, main));
    Compiled b;
    b.tester.triple = "i686-unknown-linux-gnu";
    ASSERT_TRUE(b.compile({h}, main));
    Compiled c;
    c.tester.triple = "x86_64-unknown-linux-gnu";
    ASSERT_TRUE(c.compile({h}, main, {"-Wall"}));

    EXPECT_NE(a.own("h.h", "f"), b.own("h.h", "f"));
    EXPECT_NE(a.own("h.h", "f"), c.own("h.h", "f"));
}

TEST_CASE(Containers) {
    File plain = {"n.h", R"cpp(
namespace §(empty)empty {}
§(sa)static_assert(true, "");
extern "C" {}
extern "C" { int §(ca)ca; int §(cb)cb; }
namespace N { inline int §(nf)f() { return 1; } }

inline int §(user)use() { return N::f(); }
)cpp"};
    File deprecated = {"n.h", R"cpp(
namespace §(empty)empty {}
§(sa)static_assert(true, "");
extern "C" {}
extern "C" { int §(ca)ca; int §(cb)cb; }
namespace [[deprecated]] N { inline int §(nf)f() { return 1; } }

inline int §(user)use() { return N::f(); }
)cpp"};
    File main = {"main.cpp", "#include \"n.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({plain}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({deprecated}, main));

    EXPECT_EQ(a.at("n.h", "empty").decl->getDeclKindName(), llvm::StringRef("Namespace"));
    EXPECT_EQ(a.at("n.h", "sa").decl->getDeclKindName(), llvm::StringRef("StaticAssert"));
    EXPECT_NE(&a.at("n.h", "ca"), &a.at("n.h", "cb"));
    EXPECT_TRUE(a.depends("n.h", "user", "n.h", "nf"));
    EXPECT_EQ(a.own("n.h", "user"), b.own("n.h", "user"));
    EXPECT_NE(a.content("n.h", "user"), b.content("n.h", "user"));
    EXPECT_TRUE(a.partitioned());
}

TEST_CASE(ContainerAcrossFiles) {
    File members = {"members.h", "inline int §(value)value() { return 1; }\n"};
    File user = {"user.h", "inline int §(user)use() { return N::value(); }\n"};
    File plain = {"a.cpp", "namespace N {\n#include \"members.h\"\n}\n#include \"user.h\"\n"};
    File deprecated = {
        "b.cpp",
        "namespace [[deprecated]] N {\n#include \"members.h\"\n}\n#include \"user.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({members, user}, plain));
    Compiled b;
    ASSERT_TRUE(b.compile({members, user}, deprecated));

    EXPECT_NE(a.content("user.h", "user"), b.content("user.h", "user"));

    // A wrapper with nothing of its own but the namespace is a unit in its
    // file, so a diagnostic on the namespace name has an owner.
    File wrapper = {"wrapper.h", "namespace §(ns)Bad_Name {\n#include \"members.h\"\n}\n"};
    Compiled wrapped;
    ASSERT_TRUE(wrapped.compile({members, wrapper}, {"main.cpp", "#include \"wrapper.h\"\n"}));
    EXPECT_EQ(wrapped.at("wrapper.h", "ns").decl->getDeclKindName(), llvm::StringRef("Namespace"));
    EXPECT_NE(wrapped.digest("wrapper.h"), hex(ContentHash{}));
    EXPECT_TRUE(wrapped.partitioned());
}

TEST_CASE(PositionIndependence) {
    File h1 = {"h.h", R"cpp(
inline int §(first)first() { return 1; }
inline int §(second)second(int x) { int y = x; return y; }
template <typename T> T §(tpl)twice(T x) { return x + x; }
inline auto §(lambda)lambda = [](int x) { return x; };
inline int §(caller)caller() { return lambda(1); }
)cpp"};
    File h2 = {"h.h", R"cpp(
// leading comment
inline int §(first)first() { return 1; }
inline int §(second)second(int x) { int y = x; return y; }
template <typename T> T §(tpl)twice(T x) { return x + x; }
inline auto §(lambda)lambda = [](int x) { return x; };
inline int §(caller)caller() { return lambda(1); }
)cpp"};
    File u = {"u.h", "#include \"h.h\"\ninline int §(u)use() { return second(1); }\n"};
    File main = {"main.cpp", "#include \"u.h\"\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({h1, u}, main));
    Compiled b;
    ASSERT_TRUE(b.compile({h2, u}, main));

    EXPECT_NE(a.content("h.h", "first"), b.content("h.h", "first"));
    for(auto marker: {"second", "tpl", "lambda", "caller"}) {
        EXPECT_EQ(a.content("h.h", marker), b.content("h.h", marker));
    }
    EXPECT_EQ(a.content("u.h", "u"), b.content("u.h", "u"));
    EXPECT_NE(a.digest("h.h"), b.digest("h.h"));
}

TEST_CASE(DiagnosticPragmas) {
    File h = {"h.h", "inline int §(f)f() { int unused = 0; return 0; }\n"};
    Compiled a;
    ASSERT_TRUE(a.compile({h}, {"a.cpp", "#include \"h.h\"\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile(
        {h},
        {"b.cpp", "#pragma clang diagnostic ignored \"-Wunused-variable\"\n#include \"h.h\"\n"}));
    Compiled c;
    ASSERT_TRUE(c.compile(
        {h},
        {"c.cpp",
         "_Pragma(\"clang diagnostic ignored \\\"-Wunused-variable\\\"\")\n#include \"h.h\"\n"}));

    EXPECT_NE(a.own("h.h", "f"), b.own("h.h", "f"));
    EXPECT_EQ(b.own("h.h", "f"), c.own("h.h", "f"));

    // A pragma inside the unit, under a condition the text does not show.
    File conditional = {"q.h", R"cpp(
inline void §(f)f() {
#if QUIET
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
    int unused = 0;
}
)cpp"};
    Compiled quiet;
    ASSERT_TRUE(quiet.compile({conditional}, {"main.cpp", "#define QUIET 1\n#include \"q.h\"\n"}));
    Compiled loud;
    ASSERT_TRUE(loud.compile({conditional}, {"main.cpp", "#define QUIET 0\n#include \"q.h\"\n"}));
    EXPECT_NE(quiet.own("q.h", "f"), loud.own("q.h", "f"));

    // The same pragma before or after the variable it would silence.
    File placement = {"p.h", R"cpp(
inline void §(f)f() {
#if EARLY
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
    int unused = 0;
#if !EARLY
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
}
)cpp"};
    Compiled early;
    ASSERT_TRUE(early.compile({placement}, {"main.cpp", "#define EARLY 1\n#include \"p.h\"\n"}));
    Compiled late;
    ASSERT_TRUE(late.compile({placement}, {"main.cpp", "#define EARLY 0\n#include \"p.h\"\n"}));
    EXPECT_NE(early.own("p.h", "f"), late.own("p.h", "f"));

    // The pragma and the declaration come out of one macro expansion, so
    // both sit at the invocation; their order inside it still counts.
    File expanded = {"m.h", R"cpp(
#define DECLARE(x) BEFORE inline void x() { int unused = 0; } AFTER
§(g)DECLARE(g)
)cpp"};
    Compiled before;
    ASSERT_TRUE(before.compile(
        {expanded},
        {"main.cpp",
         "#define BEFORE _Pragma(\"clang diagnostic ignored \\\"-Wunused-variable\\\"\")\n#define AFTER\n#include \"m.h\"\n"}));
    Compiled after;
    ASSERT_TRUE(after.compile(
        {expanded},
        {"main.cpp",
         "#define BEFORE\n#define AFTER _Pragma(\"clang diagnostic ignored \\\"-Wunused-variable\\\"\")\n#include \"m.h\"\n"}));
    EXPECT_NE(before.own("m.h", "g"), after.own("m.h", "g"));
}

TEST_CASE(SystemHeaders) {
    File h = {"sys/h.h", "inline int §(f)f() { return 1; }\n"};
    File main = {"main.cpp", "#include <h.h>\n"};
    auto dir = TestVFS::path("sys");
    Compiled a;
    ASSERT_TRUE(a.compile({h}, main, {"-I", dir}));
    Compiled b;
    ASSERT_TRUE(b.compile({h}, main, {"-isystem", dir}));
    Compiled c;
    ASSERT_TRUE(c.compile({h}, main, {"-isystem", dir, "-Wsystem-headers"}));

    EXPECT_NE(a.own("sys/h.h", "f"), b.own("sys/h.h", "f"));
    EXPECT_NE(b.own("sys/h.h", "f"), c.own("sys/h.h", "f"));
}

TEST_CASE(MainFileUnits) {
    Compiled a;
    ASSERT_TRUE(a.compile({}, {"main.cpp", "int §(x)x = 1;\nint §(y)y = 2;\n"}));
    Compiled b;
    ASSERT_TRUE(b.compile({}, {"main.cpp", "int §(x)x = 1;\nint §(y)y = 3;\n"}));

    EXPECT_NE(&a.at("main.cpp", "x"), &a.at("main.cpp", "y"));
    EXPECT_EQ(a.content("main.cpp", "x"), b.content("main.cpp", "x"));
    EXPECT_NE(a.content("main.cpp", "y"), b.content("main.cpp", "y"));
    EXPECT_NE(a.digest("main.cpp"), b.digest("main.cpp"));
    EXPECT_TRUE(a.partitioned());
}

};  // TEST_SUITE(content)

}  // namespace

}  // namespace clice::testing
