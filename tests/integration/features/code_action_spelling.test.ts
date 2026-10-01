/// The code an action writes must compile where it lands and mean what the
/// source meant: each case applies an action to the buffer and recompiles
/// it, and the fixtures' static_asserts fail when a spelled name finds a
/// different entity there.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "@clice/tools/client";
import { actionsOf, applyTextEdits, editsFor, positionAt } from "@clice/tools/client/edits";
import { parseAnnotations } from "@clice/tools/snap/annotation";
import { expect, test, type SessionFactory } from "../fixtures.ts";

/// The opened main.cpp of `source` (annotated with `§(name)` points),
/// compiled as C++23 for a target without MSVC compatibility, which
/// declares `size_t` implicitly, and formatted in LLVM style.
async function open(session: SessionFactory, source: string) {
    const annotated = parseAnnotations(source);
    const workspace = session.tmpdir();
    workspace.write(".clang-format", "BasedOnStyle: LLVM\n");
    workspace.write("main.cpp", annotated.content);
    workspace.writeCDB(["main.cpp"], {
        std: "c++23",
        extraArgs: ["--target=x86_64-unknown-linux-gnu"],
    });
    const client = await session.spawn(workspace).initialize(workspace);
    const [uri] = await client.openAndWait("main.cpp");
    return new OpenFile(client, uri, annotated.content, annotated.offsets);
}

class OpenFile {
    private readonly client: CliceClient;
    private readonly uri: string;
    private readonly original: string;
    private readonly markers: Map<string, number>;
    private version = 0;

    constructor(client: CliceClient, uri: string, original: string, markers: Map<string, number>) {
        this.client = client;
        this.uri = uri;
        this.original = original;
        this.markers = markers;
    }

    async titles(marker: string): Promise<string[]> {
        return (await this.actions(marker)).map((action) => action.title);
    }

    /// Apply the action titled `title` at `marker` to the original text:
    /// the result must compile without errors. The buffer returns to the original
    /// text afterwards.
    async apply(marker: string, title: string): Promise<string> {
        const actions = await this.actions(marker);
        const action = actions.find((candidate) => candidate.title === title);
        expect(
            action,
            `${title} among ${JSON.stringify(actions.map((a) => a.title))}`,
        ).toBeDefined();
        const edited = applyTextEdits(this.original, editsFor(action!, this.uri));
        await this.change(edited);
        this.client.assertNoErrors(this.uri);
        await this.change(this.original);
        return edited;
    }

    private async actions(marker: string): Promise<proto.CodeAction[]> {
        const offset = this.markers.get(marker);
        expect(offset, marker).toBeDefined();
        const position = positionAt(this.original, offset!);
        return actionsOf(
            await this.client.codeActions(this.uri, { start: position, end: position }),
        );
    }

    private async change(text: string): Promise<void> {
        this.version += 1;
        this.client.change(this.uri, this.version, text);
        await this.client.waitForRecompile(this.uri);
    }
}

test("define members of templates over auto parameters", async ({ session }) => {
    const buffer = await open(
        session,
        `template <class T>
concept Small = sizeof(T) <= 4;

template <auto V>
struct S {
    void §(plain)f();
};

template <Small auto V>
struct C {
    void §(constrained)g();
};

template <auto... Vs>
struct P {
    void §(pack)h();
};

template <auto V>
struct D {
    void a();
    void b();
};

template <auto V>
void D<V>::a() {
    §(body)int x = 0;
}
`,
    );
    expect(await buffer.apply("plain", "Define 'S<V>::f' out of line")).toContain(
        "template <auto V> void S<V>::f() {}",
    );
    expect(await buffer.apply("constrained", "Define 'C<V>::g' out of line")).toContain(
        "template <Small auto V> void C<V>::g() {}",
    );
    expect(await buffer.apply("pack", "Define 'P<Vs...>::h' out of line")).toContain(
        "template <auto... Vs> void P<Vs...>::h() {}",
    );
    expect(await buffer.apply("body", "Define missing members of 'D'")).toContain(
        "template <auto V> void D<V>::b() {}",
    );
});

test("define members spelling private member types", async ({ session }) => {
    const buffer = await open(
        session,
        `class Owner {
    enum Kind { A };
    struct Part {};

    template <Kind K>
    struct Slot {
        void §(head)f();
    };

    Part §(result)part();
};
`,
    );
    expect(await buffer.apply("head", "Define 'Owner::Slot<K>::f' out of line")).toContain(
        "template <Owner::Kind K> void Owner::Slot<K>::f() {}",
    );
    expect(await buffer.apply("result", "Define 'Owner::part' out of line")).toContain(
        "Owner::Part Owner::part() {}",
    );
});

test("expand auto through a decltype declared type", async ({ session }) => {
    const buffer = await open(
        session,
        `namespace n {
struct M {};
decltype(M{}) m;
decltype(M{})* pm;
}  // namespace n

void f() {
    §(qualified)auto x = n::m;
    static_assert(__is_same(decltype(x), n::M));
    §(pointer)auto y = n::pm;
    static_assert(__is_same(decltype(y), n::M*));
}

using namespace n;

void g() {
    §(directive)auto z = m;
    static_assert(__is_same(decltype(z), n::M));
}
`,
    );
    expect(await buffer.apply("qualified", "Replace 'auto' with 'n::M'")).toContain("n::M x");
    expect(await buffer.apply("pointer", "Replace 'auto' with 'decltype(n::M{})*'")).toContain(
        "decltype(n::M{})* y",
    );
    expect(await buffer.apply("directive", "Replace 'auto' with 'n::M'")).toContain("n::M z");
});

test("names keep the qualifiers shadowing needs", async ({ session }) => {
    const buffer = await open(
        session,
        `namespace a {
template <class T>
struct Box {};
struct X {};
X make();
Box<X> make_box();
namespace b {
struct X {};
void f() {
    §(inner)auto x = a::make();
    static_assert(__is_same(decltype(x), a::X));
    §(argument)auto box = a::make_box();
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
    §(global)auto g = ::make_global();
    static_assert(__is_same(decltype(g), ::G));
}
template <class T>
void g() {
    §(parameter)auto t = make_t();
    static_assert(__is_same(decltype(t), ns::T));
}
template void g<int>();
}  // namespace ns
`,
    );
    expect(await buffer.apply("inner", "Replace 'auto' with 'a::X'")).toContain("a::X x");
    expect(await buffer.apply("argument", "Replace 'auto' with 'Box<a::X>'")).toContain(
        "Box<a::X> box",
    );
    expect(await buffer.apply("global", "Replace 'auto' with '::G'")).toContain("::G g");
    expect(await buffer.apply("parameter", "Replace 'auto' with 'ns::T'")).toContain("ns::T t");
});

test("names hidden where the type lands", async ({ session }) => {
    const buffer = await open(
        session,
        `struct X {};
X make_x();

void local() {
    int X = 0;
    §(local)auto v = make_x();
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
    §(transitive)auto t = make_t();
    static_assert(__is_same(decltype(t), ::a::T));
}
}  // namespace n

struct Y {};
Y make_y();
int Y;

void g() {
    §(variable)auto y = make_y();
}

template <class A, class B>
struct Pair {};
template <char C>
struct Tag {};
Pair<X, Tag<'X'>> make_pair();

namespace literal {
struct X {};
void f() {
    §(literal)auto v = make_pair();
    static_assert(__is_same(decltype(v), Pair<::X, Tag<'X'>>));
}
}  // namespace literal

struct Å {};
Å make_å();

namespace u {
struct Å {};
void f() {
    §(unicode)auto v = make_å();
    static_assert(__is_same(decltype(v), ::Å));
}
}  // namespace u
`,
    );
    expect(await buffer.apply("local", "Replace 'auto' with '::X'")).toContain("::X v");
    expect(await buffer.apply("transitive", "Replace 'auto' with '::a::T'")).toContain("::a::T t");
    expect(await buffer.titles("variable")).toEqual([]);
    expect(await buffer.apply("literal", "Replace 'auto' with 'Pair<::X, Tag<'X'>>'")).toContain(
        "Pair<::X, Tag<'X'>> v",
    );
    expect(await buffer.apply("unicode", "Replace 'auto' with '::Å'")).toContain("::Å v");
});

test("internal type names become standard ones", async ({ session }) => {
    const buffer = await open(
        session,
        `void f() {
    §(size)auto n = sizeof(int);
    §(null)auto p = nullptr;
    static_assert(__is_same(decltype(p), decltype(nullptr)));
}

namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int*)0 - (int*)0);
using nullptr_t = decltype(nullptr);
}  // namespace std

§(global)auto global_size = sizeof(int);

void g(int* a, int* b) {
    §(size_std)auto n = sizeof(int);
    static_assert(__is_same(decltype(n), std::size_t));
    §(difference)auto d = b - a;
    static_assert(__is_same(decltype(d), std::ptrdiff_t));
    §(null_std)auto p = nullptr;
    static_assert(__is_same(decltype(p), std::nullptr_t));
}
`,
    );
    expect(await buffer.titles("size")).toEqual([]);
    expect(await buffer.titles("global")).toEqual([]);
    expect(await buffer.apply("null", "Replace 'auto' with 'decltype(nullptr)'")).toContain(
        "decltype(nullptr) p",
    );
    expect(await buffer.apply("size_std", "Replace 'auto' with 'std::size_t'")).toContain(
        "std::size_t n",
    );
    expect(await buffer.apply("difference", "Replace 'auto' with 'std::ptrdiff_t'")).toContain(
        "std::ptrdiff_t d",
    );
    expect(await buffer.apply("null_std", "Replace 'auto' with 'std::nullptr_t'")).toContain(
        "std::nullptr_t p",
    );
});

test("types the insertion point cannot name stay auto", async ({ session }) => {
    const buffer = await open(
        session,
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
        §(member)auto p = make();
    }
};

void f() {
    struct Own {
        struct Nested {};
    };
    §(own)auto o = Own{};
    §(nested)auto n = Own::Nested{};
    §(local)auto h = local();
    §(private)auto p = C::make();
    auto less = [](int a, int b) { return a < b; };
    Set<int, decltype(less)> set;
    §(closure)auto it = set.begin();
    §(specialization)auto item = id(C::make_item());
    §(expression)auto box = local_box();
    Self self;
    §(this)auto pointer = self.pointer();
}
`,
    );
    for (const marker of ["local", "private", "closure", "specialization", "expression", "this"]) {
        expect(await buffer.titles(marker), marker).toEqual([]);
    }
    expect(await buffer.apply("member", "Replace 'auto' with 'C::Private'")).toContain(
        "C::Private p",
    );
    expect(await buffer.apply("own", "Replace 'auto' with 'Own'")).toContain("Own o");
    expect(await buffer.apply("nested", "Replace 'auto' with 'Own::Nested'")).toContain(
        "Own::Nested n",
    );
});

test("lookup follows the scopes in effect", async ({ session }) => {
    const buffer = await open(
        session,
        `namespace app {
namespace v2 {
struct Config {};
}  // namespace v2
struct Config {};
Config load();

void directive() {
    using namespace v2;
    §(directive)auto c = load();
    static_assert(__is_same(decltype(c), app::Config));
}

struct Node {};
Node make_node();

struct Tree {
    struct Node {};

    friend void visit(Tree&) {
        §(friend)auto n = make_node();
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
    §(outer)auto u = make_u();
    static_assert(__is_same(decltype(u), app::U));
}

template struct S<int>;
}  // namespace app
`,
    );
    expect(await buffer.apply("directive", "Replace 'auto' with 'app::Config'")).toContain(
        "app::Config c",
    );
    expect(await buffer.apply("friend", "Replace 'auto' with 'app::Node'")).toContain(
        "app::Node n",
    );
    expect(await buffer.apply("outer", "Replace 'auto' with 'app::U'")).toContain("app::U u");
});

test("cv-qualifiers stay on the deduced pointer", async ({ session }) => {
    const buffer = await open(
        session,
        `#define CONST const
#define STORAGE static

int* pointer();
using Pointer = §(alias)decltype(pointer());

void f(int* q, int** pp) {
    const §(pointer)auto p = q;
    static_assert(__is_same(decltype(p), int* const));
    const §(pointee)auto* cp = pp;
    static_assert(__is_same(decltype(cp), int* const*));
    static const §(specifiers)auto s = q;
    static_assert(__is_same(decltype(s), int* const));
    const static §(parted)auto t = q;
    CONST §(macro)auto m = q;
    const /* owned */ §(comment)auto c = q;
    const STORAGE §(hidden)auto h = q;
}
`,
    );
    expect(await buffer.apply("pointer", "Replace 'const auto' with 'int* const'")).toContain(
        "int* const p = q;",
    );
    expect(await buffer.apply("pointee", "Replace 'const auto' with 'int* const'")).toContain(
        "int* const* cp = pp;",
    );
    expect(await buffer.apply("specifiers", "Replace 'const auto' with 'int* const'")).toContain(
        "static int* const s = q;",
    );
    expect(await buffer.apply("alias", "Replace 'decltype(pointer())' with 'int*'")).toContain(
        "using Pointer = int*;",
    );
    for (const marker of ["parted", "macro", "hidden", "comment"]) {
        expect(await buffer.titles(marker), marker).toEqual([]);
    }
});

test("one override per signature shared by bases", async ({ session }) => {
    const buffer = await open(
        session,
        `struct A {
    virtual void f() = 0;
    virtual void g() noexcept = 0;
};

struct B {
    virtual void f() = 0;
    virtual void g() = 0;
};

struct §(derived)D : A, B {};

static_assert(!__is_abstract(D));
`,
    );
    const edited = await buffer.apply("derived", "Implement pure virtual methods of 'D'");
    expect(edited).toContain("  void f() override;\n  void g() noexcept override;\n");
});

test("a shared override takes the spelling the class can name", async ({ session }) => {
    const buffer = await open(
        session,
        `class Aliased {
    using Count = int;

public:
    virtual void resize(Count) = 0;
};

struct Plain {
    virtual void resize(int) = 0;
};

struct §(spelled)Both : Aliased, Plain {};

static_assert(!__is_abstract(Both));
`,
    );
    expect(await buffer.apply("spelled", "Implement pure virtual methods of 'Both'")).toContain(
        "  void resize(int) override;\n",
    );
});

test("overrides repeat the specifiers they must", async ({ session }) => {
    const buffer = await open(
        session,
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

struct §(derived)Impl : base::Interface {};

static_assert(!__is_abstract(Impl));
`,
    );
    const edited = await buffer.apply("derived", "Implement pure virtual methods of 'Impl'");
    expect(edited).toContain(
        [
            "  void log(...) override;",
            "  void macro() noexcept override;",
            "  void value() noexcept override;",
            "  void lax() override;",
            "  consteval int compute() override;",
        ].join("\n"),
    );
});

test("no override while noexcept is uninstantiated", async ({ session }) => {
    const buffer = await open(
        session,
        `template <bool B>
struct Pending {
    virtual void wait() noexcept(B) = 0;
};

struct §(waiter)Waiter : Pending<true> {};
`,
    );
    expect(await buffer.titles("waiter")).toEqual([]);
});
