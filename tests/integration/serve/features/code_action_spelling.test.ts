/// The code an action writes must compile where it lands and mean what the
/// source meant: each case applies an action to the buffer and recompiles
/// it, and the fixtures' static_asserts fail when a spelled name finds a
/// different entity there.

import type { Serve } from "@clice/tools/actions";
import { actionsOf } from "@clice/tools/client/edits";
import * as proto from "vscode-languageserver-protocol";
import { at, expect, serve } from "../../fixtures.ts";

/// main.cpp holding `source`, compiled as C++23 for a target without MSVC
/// compatibility, which declares `size_t` implicitly, and formatted in
/// LLVM style.
function spelling(name: string, source: string, body: (s: Serve) => Promise<void>): void {
    serve.files(
        { ".clang-format": "BasedOnStyle: LLVM\n", "main.cpp": source },
        {
            manifest: {
                cxx: ["-std=c++23", "--target=x86_64-unknown-linux-gnu"],
                units: { "main.cpp": [] },
            },
        },
    )(name, async ({ s }) => {
        await s.compiled("main.cpp");
        await body(s);
    });
}

async function titles(s: Serve, anchor: string): Promise<string[]> {
    return actionsOf(await s.codeActions(at("main.cpp", anchor))).map((action) => action.title);
}

/// Apply the action titled `title` at `anchor` to the original text: the
/// result must compile without errors. The buffer returns to the original
/// text afterwards.
async function apply(s: Serve, anchor: string, title: string): Promise<string> {
    const { text, diagnostics } = await s.applyAction(at("main.cpp", anchor), title);
    expect(
        diagnostics.filter((diagnostic) => diagnostic.severity === proto.DiagnosticSeverity.Error),
    ).toEqual([]);
    s.edit("main.cpp", { text: s.disk.read("main.cpp") });
    await s.compiled("main.cpp");
    return text;
}

spelling(
    "define members of templates over auto parameters",
    `template <class T>
concept Small = sizeof(T) <= 4;

template <auto V>
struct S {
    void f();
};

template <Small auto V>
struct C {
    void g();
};

template <auto... Vs>
struct P {
    void h();
};

template <auto V>
struct D {
    void a();
    void b();
};

template <auto V>
void D<V>::a() {
    int x = 0;
}
`,
    async (s) => {
        expect(await apply(s, "void |f()", "Define 'S<V>::f' out of line")).toContain(
            "template <auto V> void S<V>::f() {}",
        );
        expect(await apply(s, "void |g()", "Define 'C<V>::g' out of line")).toContain(
            "template <Small auto V> void C<V>::g() {}",
        );
        expect(await apply(s, "void |h()", "Define 'P<Vs...>::h' out of line")).toContain(
            "template <auto... Vs> void P<Vs...>::h() {}",
        );
        expect(await apply(s, "int x = 0;", "Define missing members of 'D'")).toContain(
            "template <auto V> void D<V>::b() {}",
        );
    },
);

spelling(
    "define members spelling private member types",
    `class Owner {
    enum Kind { A };
    struct Part {};

    template <Kind K>
    struct Slot {
        void f();
    };

    Part part();
};
`,
    async (s) => {
        expect(await apply(s, "void |f()", "Define 'Owner::Slot<K>::f' out of line")).toContain(
            "template <Owner::Kind K> void Owner::Slot<K>::f() {}",
        );
        expect(await apply(s, "Part |part()", "Define 'Owner::part' out of line")).toContain(
            "Owner::Part Owner::part() {}",
        );
    },
);

spelling(
    "expand auto through a decltype declared type",
    `namespace n {
struct M {};
decltype(M{}) m;
decltype(M{})* pm;
}  // namespace n

void f() {
    auto x = n::m;
    static_assert(__is_same(decltype(x), n::M));
    auto y = n::pm;
    static_assert(__is_same(decltype(y), n::M*));
}

using namespace n;

void g() {
    auto z = m;
    static_assert(__is_same(decltype(z), n::M));
}
`,
    async (s) => {
        expect(await apply(s, "auto x = n::m", "Replace 'auto' with 'n::M'")).toContain("n::M x");
        expect(
            await apply(s, "auto y = n::pm", "Replace 'auto' with 'decltype(n::M{})*'"),
        ).toContain("decltype(n::M{})* y");
        expect(await apply(s, "auto z = m", "Replace 'auto' with 'n::M'")).toContain("n::M z");
    },
);

spelling(
    "names keep the qualifiers shadowing needs",
    `namespace a {
template <class T>
struct Box {};
struct X {};
X make();
Box<X> make_box();
namespace b {
struct X {};
void f() {
    auto x = a::make();
    static_assert(__is_same(decltype(x), a::X));
    auto box = a::make_box();
    static_assert(__is_same(decltype(box), a::Box<a::X>));
}
}  // namespace b
}  // namespace a

struct G {};
G make_global();

namespace ns {
struct G {};
struct T {};
T make_t();
void f() {
    auto g = ::make_global();
    static_assert(__is_same(decltype(g), ::G));
}
template <class T>
void g() {
    auto t = make_t();
    static_assert(__is_same(decltype(t), ns::T));
}
template void g<int>();
}  // namespace ns
`,
    async (s) => {
        expect(await apply(s, "auto x =", "Replace 'auto' with 'a::X'")).toContain("a::X x");
        expect(await apply(s, "auto box =", "Replace 'auto' with 'Box<a::X>'")).toContain(
            "Box<a::X> box",
        );
        expect(await apply(s, "auto g =", "Replace 'auto' with '::G'")).toContain("::G g");
        expect(await apply(s, "auto t =", "Replace 'auto' with 'ns::T'")).toContain("ns::T t");
    },
);

spelling(
    "names hidden where the type lands",
    `struct X {};
X make_x();

void local() {
    int X = 0;
    auto v = make_x();
    static_assert(__is_same(decltype(v), ::X));
}

namespace shadow {
struct a {};
}  // namespace shadow
namespace mid {
using namespace shadow;
}  // namespace mid
namespace a {
struct T {};
}  // namespace a
a::T make_t();

namespace n {
using namespace mid;
void f() {
    auto t = make_t();
    static_assert(__is_same(decltype(t), ::a::T));
}
}  // namespace n

struct Y {};
Y make_y();
int Y;

void g() {
    auto y = make_y();
}

template <class A, class B>
struct Pair {};
template <char C>
struct Tag {};
Pair<X, Tag<'X'>> make_pair();

namespace literal {
struct X {};
void f() {
    auto v = make_pair();
    static_assert(__is_same(decltype(v), Pair<::X, Tag<'X'>>));
}
}  // namespace literal

struct Å {};
Å make_å();

namespace u {
struct Å {};
void f() {
    auto v = make_å();
    static_assert(__is_same(decltype(v), ::Å));
}
}  // namespace u
`,
    async (s) => {
        expect(await apply(s, "auto v = make_x", "Replace 'auto' with '::X'")).toContain("::X v");
        expect(await apply(s, "auto t = make_t", "Replace 'auto' with '::a::T'")).toContain(
            "::a::T t",
        );
        expect(await titles(s, "auto y = make_y")).toEqual([]);
        expect(
            await apply(s, "auto v = make_pair", "Replace 'auto' with 'Pair<::X, Tag<'X'>>'"),
        ).toContain("Pair<::X, Tag<'X'>> v");
        expect(await apply(s, "auto v = make_å", "Replace 'auto' with '::Å'")).toContain("::Å v");
    },
);

spelling(
    "internal type names become standard ones",
    `void f() {
    auto n = sizeof(int);
    auto p = nullptr;
    static_assert(__is_same(decltype(p), decltype(nullptr)));
}

namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int*)0 - (int*)0);
using nullptr_t = decltype(nullptr);
}  // namespace std

auto global_size = sizeof(int);

void g(int* a, int* b) {
    auto n = sizeof(int);
    static_assert(__is_same(decltype(n), std::size_t));
    auto d = b - a;
    static_assert(__is_same(decltype(d), std::ptrdiff_t));
    auto p = nullptr;
    static_assert(__is_same(decltype(p), std::nullptr_t));
}
`,
    async (s) => {
        expect(await titles(s, "void f() {\n    |auto n")).toEqual([]);
        expect(await titles(s, "auto global_size")).toEqual([]);
        expect(
            await apply(s, "sizeof(int);\n    |auto p", "Replace 'auto' with 'decltype(nullptr)'"),
        ).toContain("decltype(nullptr) p");
        expect(
            await apply(s, "int* b) {\n    |auto n", "Replace 'auto' with 'std::size_t'"),
        ).toContain("std::size_t n");
        expect(await apply(s, "auto d =", "Replace 'auto' with 'std::ptrdiff_t'")).toContain(
            "std::ptrdiff_t d",
        );
        expect(
            await apply(
                s,
                "std::ptrdiff_t));\n    |auto p",
                "Replace 'auto' with 'std::nullptr_t'",
            ),
        ).toContain("std::nullptr_t p");
    },
);

spelling(
    "types the insertion point cannot name stay auto",
    `template <class T, class Compare>
struct Set {
    struct iterator {};
    iterator begin() { return {}; }
};

template <class T>
struct Box {};

template <class T>
T id(T value);

struct Self {
    auto pointer() -> decltype(this)*;
};

auto local() {
    struct Hidden {};
    return Hidden{};
}

auto local_box() {
    int local = 0;
    return Box<decltype(local)>{};
}

class C {
    struct Private {};
    template <class T>
    struct Item {};

public:
    static Private make();
    static Item<int> make_item();

    void member() {
        auto p = make();
    }
};

void f() {
    struct Own {
        struct Nested {};
    };
    auto o = Own{};
    auto n = Own::Nested{};
    auto h = local();
    auto p = C::make();
    auto less = [](int a, int b) { return a < b; };
    Set<int, decltype(less)> set;
    auto it = set.begin();
    auto item = id(C::make_item());
    auto box = local_box();
    Self self;
    auto pointer = self.pointer();
}
`,
    async (s) => {
        for (const anchor of [
            "auto h = local()",
            "auto p = C::make()",
            "auto it = set.begin()",
            "auto item = id(",
            "auto box = local_box()",
            "auto pointer = self.pointer()",
        ]) {
            expect(await titles(s, anchor), anchor).toEqual([]);
        }
        expect(await apply(s, "auto p = make()", "Replace 'auto' with 'C::Private'")).toContain(
            "C::Private p",
        );
        expect(await apply(s, "auto o = Own{}", "Replace 'auto' with 'Own'")).toContain("Own o");
        expect(
            await apply(s, "auto n = Own::Nested{}", "Replace 'auto' with 'Own::Nested'"),
        ).toContain("Own::Nested n");
    },
);

spelling(
    "lookup follows the scopes in effect",
    `namespace app {
namespace v2 {
struct Config {};
}  // namespace v2
struct Config {};
Config load();

void directive() {
    using namespace v2;
    auto c = load();
    static_assert(__is_same(decltype(c), app::Config));
}

struct Node {};
Node make_node();

struct Tree {
    struct Node {};

    friend void visit(Tree&) {
        auto n = make_node();
        static_assert(__is_same(decltype(n), app::Node));
    }
};

struct U {};
U make_u();

template <class T>
struct S {
    void f();
};

template <class U>
void S<U>::f() {
    auto u = make_u();
    static_assert(__is_same(decltype(u), app::U));
}

template struct S<int>;
}  // namespace app
`,
    async (s) => {
        expect(await apply(s, "auto c = load()", "Replace 'auto' with 'app::Config'")).toContain(
            "app::Config c",
        );
        expect(await apply(s, "auto n = make_node()", "Replace 'auto' with 'app::Node'")).toContain(
            "app::Node n",
        );
        expect(await apply(s, "auto u = make_u()", "Replace 'auto' with 'app::U'")).toContain(
            "app::U u",
        );
    },
);

spelling(
    "cv-qualifiers stay on the deduced pointer",
    `#define CONST const
#define STORAGE static

int* pointer();
using Pointer = decltype(pointer());

void f(int* q, int** pp) {
    const auto p = q;
    static_assert(__is_same(decltype(p), int* const));
    const auto* cp = pp;
    static_assert(__is_same(decltype(cp), int* const*));
    static const auto s = q;
    static_assert(__is_same(decltype(s), int* const));
    const static auto t = q;
    CONST auto m = q;
    const /* owned */ auto c = q;
    const STORAGE auto h = q;
}
`,
    async (s) => {
        expect(await apply(s, "auto p = q", "Replace 'const auto' with 'int* const'")).toContain(
            "int* const p = q;",
        );
        expect(await apply(s, "auto* cp", "Replace 'const auto' with 'int* const'")).toContain(
            "int* const* cp = pp;",
        );
        expect(await apply(s, "auto s = q", "Replace 'const auto' with 'int* const'")).toContain(
            "static int* const s = q;",
        );
        expect(
            await apply(s, "decltype(pointer())", "Replace 'decltype(pointer())' with 'int*'"),
        ).toContain("using Pointer = int*;");
        for (const anchor of ["auto t = q", "auto m = q", "auto h = q", "auto c = q"]) {
            expect(await titles(s, anchor), anchor).toEqual([]);
        }
    },
);

spelling(
    "one override per signature shared by bases",
    `struct A {
    virtual void f() = 0;
    virtual void g() noexcept = 0;
};

struct B {
    virtual void f() = 0;
    virtual void g() = 0;
};

struct D : A, B {};

static_assert(!__is_abstract(D));
`,
    async (s) => {
        const edited = await apply(s, "D : A, B", "Implement pure virtual methods of 'D'");
        expect(edited).toContain("  void f() override;\n  void g() noexcept override;\n");
    },
);

spelling(
    "a shared override takes the spelling the class can name",
    `class Aliased {
    using Count = int;

public:
    virtual void resize(Count) = 0;
};

struct Plain {
    virtual void resize(int) = 0;
};

struct Both : Aliased, Plain {};

static_assert(!__is_abstract(Both));
`,
    async (s) => {
        expect(
            await apply(s, "Both : Aliased", "Implement pure virtual methods of 'Both'"),
        ).toContain("  void resize(int) override;\n");
    },
);

spelling(
    "overrides repeat the specifiers they must",
    `#define NOEXCEPT noexcept

namespace base {
constexpr bool flag = true;

struct Interface {
    virtual void log(...) = 0;
    virtual void macro() NOEXCEPT = 0;
    virtual void value() noexcept(flag) = 0;
    virtual void lax() noexcept(false) = 0;
    virtual consteval int compute() = 0;
};
}  // namespace base

struct Impl : base::Interface {};

static_assert(!__is_abstract(Impl));
`,
    async (s) => {
        const edited = await apply(
            s,
            "Impl : base::Interface",
            "Implement pure virtual methods of 'Impl'",
        );
        expect(edited).toContain(
            [
                "  void log(...) override;",
                "  void macro() noexcept override;",
                "  void value() noexcept override;",
                "  void lax() override;",
                "  consteval int compute() override;",
            ].join("\n"),
        );
    },
);

spelling(
    "no override while noexcept is uninstantiated",
    `template <bool B>
struct Pending {
    virtual void wait() noexcept(B) = 0;
};

struct Waiter : Pending<true> {};
`,
    async (s) => {
        expect(await titles(s, "Waiter : Pending")).toEqual([]);
    },
);
